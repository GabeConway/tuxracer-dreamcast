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
#include <dc/sound/aica_comm.h>
#include <dc/spu.h>

#include <modplug/modplug.h>

#include "SDL.h"
#include "SDL_mixer.h"
#include "dc_aica.h"

/* KOS's snd_effect_t, which snd_sfx_load() returns a pointer to as an opaque
 * sfxhnd_t. It is declared in snd_sfxmgr.c, not in a header, because KOS only
 * ever expects you to hand the handle back to snd_sfx_play(). We do not use
 * snd_sfx_play() -- it queues to an ARM firmware that never runs (dc_aica.h) --
 * so we need the fields: where the sample landed in sound RAM, how long it is,
 * and in what format. Layout copied from
 * kernel/arch/dreamcast/sound/snd_sfxmgr.c:32-41. */
typedef struct dc_snd_effect {
    uint32_t locl, locr;
    uint32_t len;
    uint32_t rate;
    uint32_t used;
    uint32_t fmt;
    uint16_t stereo;
} dc_snd_effect_t;

#ifdef TR_AUDIO_TRACE
int mus_cb_count = 0;
#endif

/* Per-channel ring buffer for the music stream, in AICA sound RAM. Two voices
 * loop over these forever and the music thread writes ahead of the play
 * position. 16 KB per channel = 8192 16-bit samples = 371 ms at 22050 Hz: a
 * wide margin over the 20 ms poll interval, for 32 KB of a 2 MB pool. */
#define DC_MUS_BUFSIZE  (16 * 1024)
#define DC_MUS_SAMPLES  (DC_MUS_BUFSIZE / 2)

/* How often the music thread refills. Must be well under the time it takes the
 * AICA to drain the ring. */
#define DC_MUS_POLL_MS  20

/* Do not refill for less than this many samples.
 *
 * MEASURED (playtest, racing view): 512 -> 9-11 fps, 2048 -> 4-8 fps. Batching
 * does NOT pay here. The cost is libmodplug's decode, which is per-sample and
 * therefore constant either way; what batching adds is a single long
 * de-interleave-and-upload burst that lands inside one frame instead of being
 * spread over several. Small and frequent is the right shape for a 33 ms
 * budget. Do not raise this without a playtest fps number in hand. */
#define DC_MUS_MIN_FILL 512

/* Voices 0 and 1 are the music ring's left and right. Effects never get them. */
#define DC_MUS_VOICE_L  0
#define DC_MUS_VOICE_R  1
#define DC_SFX_FIRST    2

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

static ModPlugFile       *mus_mpf     = NULL;
static Mix_Music         *mus_current = NULL;
static kthread_t         *mus_thread  = NULL;
static int                mus_rate    = 22050;

/* The music ring: two AICA-side buffers (left, right), the write cursor into
 * them in samples, and the main-RAM staging buffers libmodplug decodes into
 * before the de-interleave. */
static uint32_t           mus_ring[2] = { 0, 0 };
static int16_t           *mus_decode  = NULL;   /* interleaved, from ModPlug */
static int16_t           *mus_split[2] = { NULL, NULL };
static uint32_t           mus_write   = 0;
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

/* Stop and free the whole voice group `chn` belongs to (see chan_group).
 * Caller holds audio_lock. */
static void release_channel(int chn)
{
    int primary, v;

    if (chn < DC_SFX_FIRST || chn >= DC_AICA_VOICES)
        return;

    primary = chan_group[chn];
    if (primary < 0) {
        /* Not one of ours -- stop it anyway, that is what Mix_HaltChannel
         * promises, but there is nothing to unbook. */
        dc_aica_stop(chn);
        return;
    }

    for (v = DC_SFX_FIRST; v < DC_AICA_VOICES; v++) {
        if (chan_group[v] != primary)
            continue;

        dc_aica_stop(v);
        chan_owner[v]    = NULL;
        chan_group[v]    = -1;
        chan_reserved[v] = 0;
    }
}

/* Find `n` adjacent free effect voices and return the first, or -1.
 *
 * "Free" means: not one of the music pair, not reserved by a looping effect,
 * and not currently booked by a one-shot we started. One-shots are recycled
 * round-robin (there is no completion interrupt to tell us when a sample ends,
 * and polling 64 positions per frame would cost more than it saves), but
 * reserved voices are never handed out -- so a burst of fish-pickup sounds can
 * no longer steal the terrain loop out from under src/racing.c. */
static int alloc_voices(int n, int allow_steal)
{
    static int next = DC_SFX_FIRST;
    int pass, i, v, k, ok;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < DC_AICA_VOICES - DC_SFX_FIRST; i++) {
            v = DC_SFX_FIRST +
                ((next - DC_SFX_FIRST + i) % (DC_AICA_VOICES - DC_SFX_FIRST));

            if (v + n > DC_AICA_VOICES)
                continue;

            ok = 1;
            for (k = 0; k < n; k++) {
                if (chan_reserved[v + k])
                    ok = 0;
                /* First pass: only genuinely idle voices. Second pass: allow
                 * recycling a sounding one-shot, which is what SDL_mixer does
                 * when it runs out of channels. */
                if (pass == 0 && chan_owner[v + k] != NULL)
                    ok = 0;
            }

            if (!ok)
                continue;

            next = v + n;
            if (next >= DC_AICA_VOICES)
                next = DC_SFX_FIRST;
            return v;
        }

        if (!allow_steal)
            break;
    }

    return -1;
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

/* Write `frames` decoded stereo frames from mus_decode into the ring at
 * mus_write, splitting them into the two AICA-side buffers and wrapping. The
 * AICA plays 16-bit mono per voice, so stereo is two voices reading two
 * de-interleaved buffers -- exactly what KOS's snd_pcm_split does for its own
 * stream, done here in C because the amounts are small and this runs on a
 * background thread. Caller holds audio_lock. */
static void ring_write(int frames)
{
    int chunk, done = 0;

    while (done < frames) {
        int first = frames - done;
        int i;

        if (mus_write + (uint32_t)first > DC_MUS_SAMPLES)
            first = (int)(DC_MUS_SAMPLES - mus_write);

        for (i = 0; i < first; i++) {
            mus_split[0][i] = mus_decode[(done + i) * 2 + 0];
            mus_split[1][i] = mus_decode[(done + i) * 2 + 1];
        }

        spu_memload(mus_ring[0] + mus_write * 2, mus_split[0], first * 2);
        spu_memload(mus_ring[1] + mus_write * 2, mus_split[1], first * 2);

        mus_write += (uint32_t)first;
        if (mus_write >= DC_MUS_SAMPLES)
            mus_write = 0;

        done += first;
        chunk = first;
        if (chunk == 0)
            break;      /* defensive: never spin if the ring maths goes wrong */
    }
}

/* Fill silence rather than leaving the tail of a finished module looping
 * forever. Caller holds audio_lock. */
static void ring_silence(int frames)
{
    int i;

    for (i = 0; i < frames * 2; i++)
        mus_decode[i] = 0;

    ring_write(frames);
}

/* One refill pass: how far has the AICA got, and how much can we write without
 * overtaking it? Caller holds audio_lock. */
static void mus_pump(void)
{
    uint32_t pos, space;
    int      want, got, frames;

    if (!mus_playing || mus_mpf == NULL)
        return;

    /* The voice's own position register is the only feedback there is. Keeping
     * one sample of gap means "full" and "empty" never look alike. */
    pos = dc_aica_pos(DC_MUS_VOICE_L);
    if (pos >= DC_MUS_SAMPLES)
        pos = 0;

    space = (pos + DC_MUS_SAMPLES - mus_write - 1) % DC_MUS_SAMPLES;

    if (space < DC_MUS_MIN_FILL)
        return;

    want = (int)space;
    if (want > DC_MUS_SAMPLES / 2)
        want = DC_MUS_SAMPLES / 2;    /* bounded work per pass */

    got = ModPlug_Read(mus_mpf, mus_decode, want * 4);   /* stereo, 16-bit */

#ifdef TR_AUDIO_TRACE
    mus_cb_count++;
#endif

    if (got <= 0) {
        /* End of module. With mLoopCount == -1 this never happens; when it
         * does, hand the ring silence and let update_audio()
         * (src/audio.c:667) see Mix_PlayingMusic() go false next frame. */
        ring_silence((int)space > DC_MUS_SAMPLES / 2
                     ? DC_MUS_SAMPLES / 2 : (int)space);
        mus_playing = 0;
        return;
    }

    frames = got / 4;
    if (frames > 0)
        ring_write(frames);
}

static void *mus_thread_fn(void *param)
{
    (void)param;

    while (mus_thread_run) {
        mutex_lock(&audio_lock);
        mus_pump();
#ifdef TR_AUDIO_TRACE
        {
            static int polls = 0;
            static uint64 t = 0;
            uint64 now = timer_ms_gettime64();
            polls++;
            if(now - t >= 2000) {
                printf("dc_mixer: polls=%d reads=%d playing=%d pos=%lu write=%lu\n",
                       polls, mus_cb_count, (int)mus_playing,
                       (unsigned long)dc_aica_pos(DC_MUS_VOICE_L),
                       (unsigned long)mus_write);
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
    dc_aica_stop(DC_MUS_VOICE_L);
    dc_aica_stop(DC_MUS_VOICE_R);

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

    /* Brings up the SPU and KOS's sound-RAM allocator. No KOS stream and no
     * sfxmgr playback: both of those route through an ARM firmware that does
     * not run here (dc/include/dc_aica.h). Loading and allocation still use
     * KOS -- that part is SH-4 code. */
    dc_aica_init();

    spec_channels = (channels >= 2) ? 2 : 1;
    /* The AICA path is 16-bit PCM. If the game asked for AUDIO_U8 we still
     * report the truth here rather than echoing the request back. */
    spec_format = AUDIO_S16SYS;
    (void)format;
    mus_rate  = clamp_mus_rate(frequency > 0 ? frequency : 22050);
    spec_freq = mus_rate;

    /* The music ring lives in sound RAM; the decode and split buffers in main
     * RAM. If any of it fails, effects still work and only music is lost --
     * say so and carry on rather than failing the whole device. */
    mus_ring[0] = snd_mem_malloc(DC_MUS_BUFSIZE);
    mus_ring[1] = snd_mem_malloc(DC_MUS_BUFSIZE);
    mus_decode  = memalign(32, (DC_MUS_SAMPLES / 2) * 4);
    mus_split[0] = memalign(32, DC_MUS_BUFSIZE);
    mus_split[1] = memalign(32, DC_MUS_BUFSIZE);

    if (mus_ring[0] == 0 || mus_ring[1] == 0 || mus_decode == NULL ||
        mus_split[0] == NULL || mus_split[1] == NULL) {
        dbglog(DBG_WARNING, "dc_mixer: no music ring (%lu B AICA free), "
               "music disabled\n", (unsigned long)snd_mem_available());
        if (mus_ring[0] != 0) { snd_mem_free(mus_ring[0]); mus_ring[0] = 0; }
        if (mus_ring[1] != 0) { snd_mem_free(mus_ring[1]); mus_ring[1] = 0; }
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

    dc_aica_stop_all();
    memset(chan_owner, 0, sizeof(chan_owner));
    memset(chan_reserved, 0, sizeof(chan_reserved));
    memset(chan_group, -1, sizeof(chan_group));

    if (mus_ring[0] != 0) { snd_mem_free(mus_ring[0]); mus_ring[0] = 0; }
    if (mus_ring[1] != 0) { snd_mem_free(mus_ring[1]); mus_ring[1] = 0; }

    free(mus_decode);
    free(mus_split[0]);
    free(mus_split[1]);
    mus_decode = NULL;
    mus_split[0] = NULL;
    mus_split[1] = NULL;

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
    for (i = DC_SFX_FIRST; i < DC_AICA_VOICES; i++) {
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
    const dc_snd_effect_t *e;
    uint32_t nsamp;
    uint64_t keymask;
    int chn, voices, vol, loop, v;

    if (!audio_open || chunk == NULL || chunk->hnd == SFXHND_INVALID)
        return -1;

    e = (const dc_snd_effect_t *)chunk->hnd;

    mutex_lock(&audio_lock);

    /* A stereo effect is two voices reading two de-interleaved copies, and
     * every WAV in data/sounds is stereo. They must be adjacent so that the
     * pair can be booked, volume-tracked and released as one group. */
    voices = e->stereo ? 2 : 1;
    loop   = (loops != 0);          /* divergence #2: boolean, not a count */

    if (channel >= DC_SFX_FIRST && channel + voices <= DC_AICA_VOICES) {
        release_channel(channel);
        chn = channel;
    } else {
        chn = alloc_voices(voices, 1);
    }

    if (chn < 0) {
        mutex_unlock(&audio_lock);
        set_error("no free AICA voice");
        return -1;
    }

    for (v = 0; v < voices; v++)
        release_channel(chn + v);

    nsamp = e->len;
    if (nsamp > DC_AICA_MAX_SAMPLES)
        nsamp = DC_AICA_MAX_SAMPLES;    /* see kb/design-audio.md: three of the
                                         * shipped effects are longer than the
                                         * hardware can address. */

    vol = aica_volume(chunk->volume);
    keymask = 0;

    /* Program both halves before keying either on, or the left channel starts
     * ahead of the right and the effect arrives smeared. */
    dc_aica_play_delayed(chn, e->locl, (int)e->fmt, nsamp, loop, e->rate,
                         vol, e->stereo ? 0 : 128);
    keymask |= 1ULL << chn;

    if (e->stereo) {
        dc_aica_play_delayed(chn + 1, e->locr, (int)e->fmt, nsamp, loop,
                             e->rate, vol, 255);
        keymask |= 1ULL << (chn + 1);
    }

    dc_aica_key_on(keymask);

    for (v = 0; v < voices; v++) {
        chan_owner[chn + v]    = chunk;
        chan_group[chn + v]    = (signed char)chn;
        /* Divergence #3, see the file header: a looping effect holds its
         * voices until something halts it, so the one-shot round-robin can
         * never steal the terrain loop out from under src/racing.c. */
        chan_reserved[chn + v] = (unsigned char)loop;
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
        for (i = DC_SFX_FIRST; i < DC_AICA_VOICES; i++)
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

        for (i = DC_SFX_FIRST; i < DC_AICA_VOICES; i++) {
            if (chan_owner[i] != NULL)
                dc_aica_set_vol(i, aica_volume(chan_owner[i]->volume));
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
        for (i = DC_SFX_FIRST; i < DC_AICA_VOICES; i++) {
            if (chan_owner[i] == chunk)
                dc_aica_set_vol(i, aica_volume(volume));
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

    if (mus_ring[0] == 0 || mus_ring[1] == 0) {
        set_error("no music ring available");
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

    /* Start from a silent ring so the first refill cannot be heard chasing the
     * play head, then prime it before either voice is keyed on. */
    spu_memset(mus_ring[0], 0, DC_MUS_BUFSIZE);
    spu_memset(mus_ring[1], 0, DC_MUS_BUFSIZE);
    mus_write = 0;

    mus_playing = 1;
    mus_pump();                 /* fills up to half the ring */

    {
        int vol = (mus_volume * 255) / MIX_MAX_VOLUME;

        dc_aica_play_delayed(DC_MUS_VOICE_L, mus_ring[0], DC_AICA_FMT_16BIT,
                             DC_MUS_SAMPLES, 1, (uint32_t)mus_rate, vol, 0);
        dc_aica_play_delayed(DC_MUS_VOICE_R, mus_ring[1], DC_AICA_FMT_16BIT,
                             DC_MUS_SAMPLES, 1, (uint32_t)mus_rate, vol, 255);
        /* Both voices in the same instant: a ring played by two voices that
         * started a few samples apart never comes back into phase. */
        dc_aica_key_on((1ULL << DC_MUS_VOICE_L) | (1ULL << DC_MUS_VOICE_R));
    }

    mus_current = music;

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

        dc_aica_set_vol(DC_MUS_VOICE_L, (mus_volume * 255) / MIX_MAX_VOLUME);
        dc_aica_set_vol(DC_MUS_VOICE_R, (mus_volume * 255) / MIX_MAX_VOLUME);
    }

    mutex_unlock(&audio_lock);
    return prev;
}
