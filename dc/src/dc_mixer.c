/* dc_mixer.c - SDL_mixer's API, implemented on KallistiOS audio.
 *
 * src/audio.c and src/audio_data.c are upstream Tux Racer, built verbatim.
 * They call SDL_mixer; SDL_mixer has no KOS port. This file is the whole of
 * the replacement. dc/include/SDL_mixer.h and dc/include/SDL.h declare it.
 *
 * TWO HALVES, TWO DIFFERENT KOS SUBSYSTEMS
 *
 *   Sound effects (the sounds/ *.wav files) -> dc/sound/sfxmgr.h. snd_sfx_load() DMAs the
 *   whole sample into the AICA's 2 MB of dedicated sound RAM and returns a
 *   handle; playback costs the SH-4 nothing after that. This is the right
 *   backing because Tux Racer's effects are a dozen short clips that are
 *   started and stopped dozens of times a second by src/racing.c:315-361.
 *
 *   Music (the music/ *.it Impulse Tracker modules) -> libmodplug decoding into a KOS
 *   snd_stream, pumped by a dedicated thread. Modules must be decoded on the
 *   SH-4; there is no hardware for them.
 *
 * THREE PLACES THIS DELIBERATELY DIVERGES FROM SDL_mixer. All three are
 * described in kb/design-audio.md; read that before "fixing" any of them.
 *
 *   1. Mix_LoadMUS() does not load anything. It records the path and
 *      Mix_PlayMusic() does the ModPlug_Load. src/audio_data.c caches every
 *      music record for the life of the process (music_hash_), and a decoded
 *      .it costs main RAM we do not have 5 copies of on a 16 MB machine.
 *      Consequence: a load hitch at each track change, not at startup.
 *
 *   2. Finite repeat counts (loops > 0) are not supported for sound effects.
 *      The AICA's loop flag is a boolean - loop forever or not at all
 *      (kos .../sound/snd_sfxmgr.c:775, chan->loop = data->loop). Tux Racer
 *      only ever passes 0 or -1 (src/racing.c, src/phys_sim.c:767), so this
 *      costs nothing today; loops > 0 is treated as loop-forever.
 *
 *   3. Looping effects get a RESERVED PAIR of AICA voices. KOS's free-channel
 *      picker is a round-robin over the 64 voices that does NOT check whether a
 *      voice is still sounding (snd_sfxmgr.c:722-745) - so ~32 one-shot plays
 *      would steal the still-looping snow/ice/rock/flying voice out from under
 *      src/racing.c. snd_sfx_chn_alloc() takes voices out of that rotation, and
 *      it has to be a pair because every WAV in data/sounds is stereo and a
 *      stereo effect plays on chn and chn+1 (snd_sfxmgr.c:788-796).
 */

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>

#include <arch/timer.h>
#include <kos/dbglog.h>
#include <kos/mutex.h>
#include <kos/thread.h>

#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include <dc/sound/aica_comm.h>

#include <modplug/modplug.h>

#include "SDL.h"
#include "SDL_mixer.h"

#ifdef TR_AUDIO_TRACE
int mus_cb_count = 0;
#endif

/* Per-channel SPU ring buffer for the music stream. snd_stream_poll() asks the
 * callback for at most buffer_size bytes for a stereo stream
 * (kos .../sound/snd_stream.c:697,845), so this also sizes the decode buffer.
 * 16 KB per channel = 32 KB of AICA RAM and ~186 ms of 22050 Hz stereo, which
 * is a comfortable margin over the 20 ms poll interval without being greedy
 * with a 2 MB pool. */
#define DC_MUS_BUFSIZE  (16 * 1024)

/* How often the music thread pumps the stream. Must be well under the time it
 * takes the AICA to drain DC_MUS_BUFSIZE. */
#define DC_MUS_POLL_MS  20

#define DC_AICA_VOICES  64

/* --------------------------------------------------------------------------
 * Shared state
 * -------------------------------------------------------------------------- */

/* Recursive because SDL_LockAudio() is the public face of this lock and a
 * caller that holds it may then call a Mix_* entry point that locks again.
 * (src/winsys.c:333 does Lock/Unlock back to back, but nothing stops a future
 * dc_winsys.c from holding it across a Mix_ call.) */
static mutex_t audio_lock = RECURSIVE_MUTEX_INITIALIZER;

static char err_buf[192] = "";

static int    audio_open    = 0;
static int    spec_freq     = 22050;
static Uint16 spec_format   = AUDIO_S16SYS;
static int    spec_channels = 2;

/* Which chunk is sounding on each AICA voice, so that a later Mix_VolumeChunk()
 * or Mix_Volume() can push the new level to a voice that is already playing --
 * src/racing.c:315-361 re-sets the terrain-sound volumes every single frame,
 * and upstream SDL_mixer really does change a sounding effect's level. */
static Mix_Chunk *chan_owner[DC_AICA_VOICES];

/* Voice grouping. A stereo effect occupies chn AND chn+1 (snd_sfxmgr.c:788-796),
 * and every WAV in data/sounds is stereo, so a reservation that covers only the
 * left voice leaves the right one in the round-robin to be stolen. chan_group[v]
 * is the primary voice of the group v belongs to, or -1 if v is idle. */
static signed char chan_group[DC_AICA_VOICES];
static unsigned char chan_reserved[DC_AICA_VOICES];

static int sfx_master_volume = MIX_MAX_VOLUME;

/* Running total of AICA sound RAM handed out by snd_sfx_load(), measured from
 * snd_mem_available() rather than guessed. Logged so a real run reports the
 * real budget instead of an estimate. */
static uint32_t sfx_ram_used = 0;

struct Mix_Chunk {
    sfxhnd_t hnd;
    int      volume;    /* 0..MIX_MAX_VOLUME */
};

struct _Mix_Music {
    char *path;         /* see divergence #1 at the top of this file */
};

static snd_stream_hnd_t   mus_stream  = SND_STREAM_INVALID;
static ModPlugFile       *mus_mpf     = NULL;
static Mix_Music         *mus_current = NULL;
static uint8_t           *mus_buf     = NULL;
static kthread_t         *mus_thread  = NULL;
static int                mus_rate    = 22050;
static int                mus_volume  = MIX_MAX_VOLUME;
static volatile int       mus_playing = 0;
static volatile int       mus_thread_run = 0;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, sizeof(err_buf), fmt, ap);
    va_end(ap);
}

/* SDL_mixer volume is 0..128 and is a per-chunk level scaled by a per-channel
 * level. The AICA wants a single 0..255 value, so fold them here. */
static int aica_volume(int chunk_vol)
{
    int v;

    if (chunk_vol < 0)
        chunk_vol = 0;
    if (chunk_vol > MIX_MAX_VOLUME)
        chunk_vol = MIX_MAX_VOLUME;

    v = (chunk_vol * sfx_master_volume * 255) / (MIX_MAX_VOLUME * MIX_MAX_VOLUME);

    if (v > 255)
        v = 255;
    return v;
}

/* Change the level of a voice that is already sounding. sfxmgr has no call for
 * this, but the AICA firmware does: AICA_CH_CMD_UPDATE with the SET_VOL bit.
 * This is the same mechanism snd_stream_volume() uses. */
static void aica_set_chan_volume(int chn, int vol)
{
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    cmd->cmd       = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size      = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id    = chn;
    chan->cmd      = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL;
    chan->vol      = vol;

    snd_sh4_to_aica(tmp, cmd->size);
}

/* Stop and free the whole voice group `chn` belongs to (see chan_group).
 * Caller holds audio_lock. */
static void release_channel(int chn)
{
    int primary, v;

    if (chn < 0 || chn >= DC_AICA_VOICES)
        return;

    primary = chan_group[chn];
    if (primary < 0) {
        /* Not one of ours -- stop it anyway, that is what Mix_HaltChannel
         * promises, but there is nothing to unbook. */
        snd_sfx_stop(chn);
        return;
    }

    for (v = 0; v < DC_AICA_VOICES; v++) {
        if (chan_group[v] != primary)
            continue;

        snd_sfx_stop(v);
        if (chan_reserved[v]) {
            snd_sfx_chn_free(v);
            chan_reserved[v] = 0;
        }
        chan_owner[v] = NULL;
        chan_group[v] = -1;
    }
}

/* Reserve two ADJACENT voices, so a stereo looping effect keeps both halves.
 * snd_sfx_chn_alloc() always returns the lowest free voice (snd_sfxmgr.c:837),
 * so two calls in a row are usually adjacent; when a hole makes them not, keep
 * the higher one and try again. Bounded, and everything is freed on failure.
 * Returns the primary voice, or -1. Caller holds audio_lock. */
static int alloc_voice_pair(void)
{
    int held[DC_AICA_VOICES];
    int n = 0, i, prev = -1, primary = -1;

    while (n < DC_AICA_VOICES) {
        int c = snd_sfx_chn_alloc();
        if (c < 0)
            break;

        held[n++] = c;

        if (prev >= 0 && c == prev + 1) {
            primary = prev;
            break;
        }
        prev = c;
    }

    /* Give back everything that is not the chosen pair. */
    for (i = 0; i < n; i++) {
        if (primary >= 0 && (held[i] == primary || held[i] == primary + 1))
            continue;
        snd_sfx_chn_free(held[i]);
    }

    return primary;
}

/* libmodplug decodes internally at 44100 and downmixes to whatever you ask
 * for, but only 11025/22050/44100 are legal output rates (modplug.h:72). */
static int clamp_mus_rate(int hz)
{
    if (hz >= 44100)
        return 44100;
    if (hz >= 22050)
        return 22050;
    return 11025;
}

/* --------------------------------------------------------------------------
 * Music thread
 * -------------------------------------------------------------------------- */

/* Runs on mus_thread with audio_lock held (the thread takes it around
 * snd_stream_poll), so it needs no locking of its own.
 *
 * KOS names these parameters "samples", but snd_stream_fill() passes and reads
 * BYTES (kos .../sound/snd_stream.c:717 -- get_data(hnd, needed_bytes, &got_bytes)).
 * Getting this wrong gives you a stream that plays at a quarter speed. */
static void *mus_get_data(snd_stream_hnd_t hnd, int req_bytes, int *recv_bytes)
{
    int got = 0;

    (void)hnd;

    if (req_bytes > DC_MUS_BUFSIZE)
        req_bytes = DC_MUS_BUFSIZE;

    if (mus_mpf != NULL && mus_buf != NULL)
        got = ModPlug_Read(mus_mpf, mus_buf, req_bytes);

#ifdef TR_AUDIO_TRACE
    mus_cb_count++;
#endif

    if (got <= 0) {
        /* End of module (ModPlug_Read returns 0 at the end, and with
         * mLoopCount == -1 it never does). Returning NULL makes KOS fill the
         * ring with silence and snd_stream_poll() return 0 -- no glitch, and
         * update_audio() (src/audio.c:667) sees Mix_PlayingMusic() go false on
         * the next frame and clears its own bookkeeping. */
        mus_playing = 0;
        *recv_bytes = 0;
        return NULL;
    }

    /* A short read is fine: KOS advances its write position by exactly what we
     * hand back (snd_stream.c:737-784). */
    *recv_bytes = got;
    return mus_buf;
}

static void *mus_thread_fn(void *param)
{
    (void)param;

    while (mus_thread_run) {
        mutex_lock(&audio_lock);
        if (mus_playing && mus_stream != SND_STREAM_INVALID) {
            snd_stream_poll(mus_stream);
        }
#ifdef TR_AUDIO_TRACE
            /* Is the AICA actually consuming? If it is not, the ring never
             * drains, snd_stream_poll stops asking mus_get_data for data, and
             * cb freezes. That distinguishes "the guest produced no samples"
             * from "the host did not play them" -- see kb/design-audio.md. */
            {
                static int polls = 0;
                static uint64 t = 0;
                uint64 now = timer_ms_gettime64();
                polls++;
                if(now - t >= 2000) {
                    printf("dc_mixer: polls=%d cb=%d playing=%d stream=%d mpf=%p\n",
                           polls, mus_cb_count, (int)mus_playing,
                           (int)mus_stream, (void*)mus_mpf);
                    t = now; polls = 0;
                }
            }
#endif
        mutex_unlock(&audio_lock);

        thd_sleep(DC_MUS_POLL_MS);
    }

    return NULL;
}

/* Caller holds audio_lock. */
static void stop_music_locked(void)
{
    if (mus_stream != SND_STREAM_INVALID && (mus_playing || mus_mpf != NULL))
        snd_stream_stop(mus_stream);

    mus_playing = 0;

    if (mus_mpf != NULL) {
        ModPlug_Unload(mus_mpf);
        mus_mpf = NULL;
    }
    mus_current = NULL;
}

/* --------------------------------------------------------------------------
 * SDL shim
 * -------------------------------------------------------------------------- */

int SDL_Init(Uint32 flags)
{
    /* Nothing to do: Mix_OpenAudio() brings up the AICA, and the video/input
     * subsystems belong to dc/src/dc_winsys.c. src/audio.c:77 only checks the
     * return value before proceeding. */
    (void)flags;
    return 0;
}

const char *SDL_GetError(void)
{
    return err_buf;
}

void SDL_LockAudio(void)
{
    mutex_lock(&audio_lock);
}

void SDL_UnlockAudio(void)
{
    mutex_unlock(&audio_lock);
}

Uint32 SDL_GetTicks(void)
{
    struct timespec ts = arch_timer_gettime();
    return (Uint32)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void SDL_Delay(Uint32 ms)
{
    thd_sleep(ms);
}

/* --------------------------------------------------------------------------
 * Device
 * -------------------------------------------------------------------------- */

int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize)
{
    (void)chunksize;    /* SDL callback buffer size; meaningless on AICA. */

    if (audio_open)
        return 0;

    /* snd_stream_init_ex() implicitly calls snd_init(), which is what sfxmgr
     * needs too, so this one call brings up both halves. The _ex form lets us
     * cap the stereo split buffer it allocates in MAIN RAM: the default is
     * SND_STREAM_BUFFER_MAX (64 KB) per channel = 128 KB, and we only ever
     * need DC_MUS_BUFSIZE. */
    if (snd_stream_init_ex(2, DC_MUS_BUFSIZE) < 0) {
        set_error("snd_stream_init_ex failed");
        return -1;
    }

    spec_channels = (channels >= 2) ? 2 : 1;
    /* The AICA stream path is 16-bit PCM. If the game asked for AUDIO_U8 we
     * still report the truth here rather than echoing the request back. */
    spec_format = AUDIO_S16SYS;
    (void)format;
    mus_rate  = clamp_mus_rate(frequency > 0 ? frequency : 22050);
    spec_freq = mus_rate;

    mus_buf = memalign(32, DC_MUS_BUFSIZE);
    if (mus_buf == NULL) {
        set_error("out of memory allocating %d byte music buffer", DC_MUS_BUFSIZE);
        snd_stream_shutdown();
        return -1;
    }

    mus_stream = snd_stream_alloc(mus_get_data, DC_MUS_BUFSIZE);
    if (mus_stream == SND_STREAM_INVALID) {
        /* Effects still work without this; only music is lost. Say so and
         * carry on rather than failing the whole device. */
        dbglog(DBG_WARNING, "dc_mixer: snd_stream_alloc failed, music disabled\n");
    }

    memset(chan_owner, 0, sizeof(chan_owner));
    memset(chan_reserved, 0, sizeof(chan_reserved));
    /* -1, not 0: 0 is a legal group id (voice 0). */
    memset(chan_group, -1, sizeof(chan_group));
    sfx_master_volume = MIX_MAX_VOLUME;
    sfx_ram_used = 0;

    mus_thread_run = 1;
    mus_thread = thd_create(0, mus_thread_fn, NULL);
    if (mus_thread == NULL) {
        dbglog(DBG_WARNING, "dc_mixer: music thread not created, music disabled\n");
        mus_thread_run = 0;
    }

    audio_open = 1;

    dbglog(DBG_INFO, "dc_mixer: open %d Hz, %d ch, 16-bit; AICA RAM free %lu bytes\n",
           spec_freq, spec_channels, (unsigned long)snd_mem_available());

    return 0;
}

void Mix_CloseAudio(void)
{
    if (!audio_open)
        return;

    mutex_lock(&audio_lock);
    stop_music_locked();
    audio_open = 0;
    mutex_unlock(&audio_lock);

    /* Join outside the lock: the thread takes audio_lock every iteration and
     * would never observe the stop flag if we held it here. */
    mus_thread_run = 0;
    if (mus_thread != NULL) {
        thd_join(mus_thread, NULL);
        mus_thread = NULL;
    }

    mutex_lock(&audio_lock);

    snd_sfx_stop_all();
    memset(chan_owner, 0, sizeof(chan_owner));
    memset(chan_reserved, 0, sizeof(chan_reserved));
    memset(chan_group, -1, sizeof(chan_group));

    if (mus_stream != SND_STREAM_INVALID) {
        snd_stream_destroy(mus_stream);
        mus_stream = SND_STREAM_INVALID;
    }
    snd_stream_shutdown();

    free(mus_buf);
    mus_buf = NULL;

    mutex_unlock(&audio_lock);
}

int Mix_QuerySpec(int *frequency, Uint16 *format, int *channels)
{
    if (!audio_open)
        return 0;

    if (frequency != NULL)
        *frequency = spec_freq;
    if (format != NULL)
        *format = spec_format;
    if (channels != NULL)
        *channels = spec_channels;

    return 1;
}

/* --------------------------------------------------------------------------
 * Sound effects
 * -------------------------------------------------------------------------- */

Mix_Chunk *Mix_LoadWAV(const char *file)
{
    Mix_Chunk *chunk;
    sfxhnd_t   hnd;
    uint32_t   before, after;

    if (!audio_open) {
        set_error("audio device not open");
        return NULL;
    }
    if (file == NULL) {
        set_error("NULL filename");
        return NULL;
    }

    mutex_lock(&audio_lock);

    before = snd_mem_available();
    hnd = snd_sfx_load(file);
    after = snd_mem_available();

    if (hnd == SFXHND_INVALID) {
        mutex_unlock(&audio_lock);
        /* snd_sfx_load() also fails on a well-formed WAV longer than 65534
         * samples -- ~2.97 s at 22050 Hz. Name that here so a mystery silent
         * effect is diagnosable from the log. */
        set_error("snd_sfx_load(%s) failed (bad WAV, out of AICA RAM, "
                  "or longer than 65534 samples)", file);
        return NULL;
    }

    chunk = malloc(sizeof(*chunk));
    if (chunk == NULL) {
        snd_sfx_unload(hnd);
        mutex_unlock(&audio_lock);
        set_error("out of memory");
        return NULL;
    }

    chunk->hnd    = hnd;
    chunk->volume = MIX_MAX_VOLUME;

    if (before > after)
        sfx_ram_used += (before - after);

    dbglog(DBG_INFO, "dc_mixer: %s -> %lu B AICA (total %lu B, %lu B free)\n",
           file, (unsigned long)(before - after),
           (unsigned long)sfx_ram_used, (unsigned long)after);

    mutex_unlock(&audio_lock);
    return chunk;
}

void Mix_FreeChunk(Mix_Chunk *chunk)
{
    int i;

    if (chunk == NULL)
        return;

    mutex_lock(&audio_lock);

    /* Upstream frees chunks from delete_unused_audio_data()
     * (src/audio_data.c:477) without first halting them; a voice still reading
     * the sample out of freed AICA RAM would scream. */
    for (i = 0; i < DC_AICA_VOICES; i++) {
        if (chan_owner[i] == chunk)
            release_channel(i);
    }

    snd_sfx_unload(chunk->hnd);
    free(chunk);

    mutex_unlock(&audio_lock);
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)
{
#ifdef TR_AUDIO_TRACE
    printf("dc_mixer: PlayChannel ch=%d chunk=%p loops=%d\n", channel, (void*)chunk, loops);
#endif
    sfx_play_data_t d;
    int chn, reserved = 0;

    if (!audio_open || chunk == NULL)
        return -1;

    mutex_lock(&audio_lock);

    if (loops != 0 && channel < 0) {
        /* Divergence #3, see the file header: keep looping effects out of the
         * round-robin so one-shots cannot steal their voice. A pair, because
         * every effect in data/sounds is stereo and therefore burns two. */
        chn = alloc_voice_pair();
        if (chn >= 0) {
            channel = chn;
            reserved = 1;
        }
    }

    if (channel >= 0 && channel < DC_AICA_VOICES && chan_group[channel] >= 0)
        release_channel(channel);

    memset(&d, 0, sizeof(d));
    d.chn  = channel;               /* -1 lets KOS pick */
    d.idx  = chunk->hnd;
    d.vol  = aica_volume(chunk->volume);
    d.pan  = 128;                   /* centre; Tux Racer never pans */
    d.loop = (loops != 0);          /* divergence #2: boolean, not a count */
    d.freq = 0;                     /* 0 => the sample's own rate */

    chn = snd_sfx_play_ex(&d);

    if (chn < 0) {
        if (reserved)
            snd_sfx_chn_free(channel);
        mutex_unlock(&audio_lock);
        set_error("no free AICA voice");
        return -1;
    }

    if (chn < DC_AICA_VOICES) {
        chan_owner[chn]    = chunk;
        chan_group[chn]    = (signed char)chn;
        chan_reserved[chn] = (unsigned char)reserved;

        /* Only reserved (looping) plays book the second voice. A one-shot on a
         * round-robin voice cannot claim chn+1 -- KOS may already have handed
         * that to someone else -- and one-shots are not volume-modulated, so
         * the missing bookkeeping costs nothing. */
        if (reserved && chn + 1 < DC_AICA_VOICES) {
            chan_owner[chn + 1]    = chunk;
            chan_group[chn + 1]    = (signed char)chn;
            chan_reserved[chn + 1] = 1;
        }
    }

    mutex_unlock(&audio_lock);
    return chn;
}

int Mix_HaltChannel(int channel)
{
    int i;

    if (!audio_open)
        return 0;

    mutex_lock(&audio_lock);

    if (channel < 0) {
        for (i = 0; i < DC_AICA_VOICES; i++)
            release_channel(i);
    } else if (channel < DC_AICA_VOICES) {
        release_channel(channel);
    }

    mutex_unlock(&audio_lock);
    return 0;
}

int Mix_Volume(int channel, int volume)
{
    int prev, i;

    /* Per-channel levels are not tracked; see the comment below. */
    (void)channel;

    mutex_lock(&audio_lock);

    /* SDL_mixer keeps one level per channel; we keep a single master, because
     * src/audio.c:652 only ever calls Mix_Volume(-1, v) and per-effect level
     * already comes through Mix_VolumeChunk(). */
    prev = sfx_master_volume;

    if (volume >= 0) {
        if (volume > MIX_MAX_VOLUME)
            volume = MIX_MAX_VOLUME;
        sfx_master_volume = volume;

        for (i = 0; i < DC_AICA_VOICES; i++) {
            if (chan_owner[i] != NULL)
                aica_set_chan_volume(i, aica_volume(chan_owner[i]->volume));
        }
    }

    mutex_unlock(&audio_lock);
    return prev;
}

int Mix_VolumeChunk(Mix_Chunk *chunk, int volume)
{
    int prev, i;

    if (chunk == NULL)
        return -1;

    mutex_lock(&audio_lock);

    prev = chunk->volume;

    if (volume >= 0) {
        if (volume > MIX_MAX_VOLUME)
            volume = MIX_MAX_VOLUME;
        chunk->volume = volume;

        /* Must reach voices that are already sounding: src/racing.c:334-361
         * modulates the terrain loops with set_sound_volume() every frame. */
        for (i = 0; i < DC_AICA_VOICES; i++) {
            if (chan_owner[i] == chunk)
                aica_set_chan_volume(i, aica_volume(volume));
        }
    }

    mutex_unlock(&audio_lock);
    return prev;
}

/* --------------------------------------------------------------------------
 * Music
 * -------------------------------------------------------------------------- */

Mix_Music *Mix_LoadMUS(const char *file)
{
#ifdef TR_AUDIO_TRACE
    printf("dc_mixer: LoadMUS %s\n", file ? file : "(null)");
#endif
    Mix_Music *music;
    FILE      *fp;

    if (file == NULL) {
        set_error("NULL filename");
        return NULL;
    }

    /* Divergence #1: record the path, decode later. Still open the file now so
     * that a missing/unreadable module is reported at load time, which is what
     * src/audio_data.c:145 expects. */
    fp = fopen(file, "rb");
    if (fp == NULL) {
        set_error("cannot open music file %s", file);
        return NULL;
    }
    fclose(fp);

    music = malloc(sizeof(*music));
    if (music == NULL) {
        set_error("out of memory");
        return NULL;
    }

    /*
     * The path is made ABSOLUTE here, and that is not tidiness -- it is the
     * whole reason the lazy load works.
     *
     * Mix_LoadMUS defers the decode to Mix_PlayMusic (see the header comment:
     * five decoded modules do not fit in 16 MB). But the caller's path is
     * relative -- data/tuxracer_init.tcl:31 passes "music/start1-jt.it" -- and
     * between the two calls the game chdir()s: src/course_load.c:345 and
     * src/file_util.c walk in and out of course directories, and
     * tuxracer_init.tcl has its own tux_goto_data_dir. By the time the splash
     * screen asks for its music the working directory is somewhere else and
     * fopen() misses.
     *
     * MEASURED: without this, Mix_PlayMusic returned -1 on the fopen for every
     * track, silently -- the game has no error path for it -- and the port
     * played no music at all while looking, from the outside, exactly like an
     * AICA or a libmodplug problem.
     */
    if (file[0] == '/') {
        music->path = strdup(file);
    }
    else {
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            cwd[0] = '\0';
        }
        music->path = (char *) malloc(strlen(cwd) + 1 + strlen(file) + 1);
        if (music->path != NULL) {
            sprintf(music->path, "%s/%s", cwd, file);
        }
    }

    if (music->path == NULL) {
        free(music);
        set_error("out of memory");
        return NULL;
    }

#ifdef TR_AUDIO_TRACE
    printf("dc_mixer: LoadMUS resolved -> %s\n", music->path);
#endif

    return music;
}

void Mix_FreeMusic(Mix_Music *music)
{
    if (music == NULL)
        return;

    mutex_lock(&audio_lock);

    if (mus_current == music)
        stop_music_locked();

    mutex_unlock(&audio_lock);

    free(music->path);
    free(music);
}

int Mix_PlayMusic(Mix_Music *music, int loops)
{
#ifdef TR_AUDIO_TRACE
    printf("dc_mixer: PlayMusic %p loops=%d\n", (void*)music, loops);
#endif
    ModPlug_Settings settings;
    FILE   *fp;
    long    len;
    void   *raw;
    size_t  got;

    if (!audio_open || music == NULL)
        return -1;

    if (mus_stream == SND_STREAM_INVALID) {
        set_error("no music stream available");
        return -1;
    }

    fp = fopen(music->path, "rb");
    if (fp == NULL) {
        set_error("cannot open music file %s", music->path);
#ifdef TR_AUDIO_TRACE
        printf("dc_mixer: PlayMusic FAILED to open %s\n", music->path);
#endif
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) <= 0) {
        fclose(fp);
        set_error("cannot size music file %s", music->path);
        return -1;
    }
    rewind(fp);

    raw = malloc((size_t)len);
    if (raw == NULL) {
        fclose(fp);
        set_error("out of memory reading %s (%ld bytes)", music->path, len);
        return -1;
    }
    got = fread(raw, 1, (size_t)len, fp);
    fclose(fp);

    if (got != (size_t)len) {
        free(raw);
        set_error("short read on %s", music->path);
        return -1;
    }

    mutex_lock(&audio_lock);

    stop_music_locked();

    /* mChannels/mBits/mFrequency/mLoopCount only take effect on the next
     * ModPlug_Load (modplug.h:84-86), which is exactly the call below -- this
     * is the payoff for loading lazily. */
    ModPlug_GetSettings(&settings);
    settings.mChannels       = spec_channels;
    settings.mBits           = 16;
    settings.mFrequency      = mus_rate;
    /* LINEAR, not SPLINE/FIR: the SH-4 has to do this in software alongside
     * the game. Raise it only with a measured frame-time budget in hand. */
    settings.mResamplingMode = MODPLUG_RESAMPLE_LINEAR;
    settings.mFlags          = 0;
    settings.mLoopCount      = (loops < 0) ? -1 : loops;
    ModPlug_SetSettings(&settings);

    mus_mpf = ModPlug_Load(raw, (int)len);

    /* libmodplug copies what it needs out of the input block during Load, so
     * the file image can go back to the heap immediately. */
    free(raw);

    if (mus_mpf == NULL) {
        mutex_unlock(&audio_lock);
        set_error("ModPlug_Load(%s) failed", music->path);
        return -1;
    }

    snd_stream_start(mus_stream, (uint32_t)mus_rate, spec_channels == 2);
    snd_stream_volume(mus_stream, (mus_volume * 255) / MIX_MAX_VOLUME);

    mus_current = music;
    mus_playing = 1;

    mutex_unlock(&audio_lock);
    return 0;
}

int Mix_HaltMusic(void)
{
    /* Called every frame from src/audio.c:588 when music is disabled, so the
     * fast path has to be cheap. */
    if (!mus_playing && mus_mpf == NULL)
        return 0;

    mutex_lock(&audio_lock);
    stop_music_locked();
    mutex_unlock(&audio_lock);

    return 0;
}

int Mix_PlayingMusic(void)
{
    return mus_playing ? 1 : 0;
}

int Mix_VolumeMusic(int volume)
{
    int prev;

    mutex_lock(&audio_lock);

    prev = mus_volume;

    if (volume >= 0) {
        if (volume > MIX_MAX_VOLUME)
            volume = MIX_MAX_VOLUME;
        mus_volume = volume;

        if (mus_stream != SND_STREAM_INVALID)
            snd_stream_volume(mus_stream, (mus_volume * 255) / MIX_MAX_VOLUME);
    }

    mutex_unlock(&audio_lock);
    return prev;
}
