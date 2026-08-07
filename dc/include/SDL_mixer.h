/*
 * SDL_mixer.h -- Dreamcast shim, NOT SDL_mixer.
 *
 * SDL_mixer has no KallistiOS port. src/audio.c and src/audio_data.c are built
 * verbatim, so this header declares exactly the SDL_mixer surface they use and
 * dc/src/dc_mixer.c implements it on KOS's snd_sfx / snd_stream + libmodplug.
 *
 * The complete symbol set was taken from
 *   grep -rhno 'Mix_[A-Za-z_]*' src/ | sort -u
 * and is: Mix_Chunk Mix_Music Mix_OpenAudio Mix_CloseAudio Mix_QuerySpec
 *         Mix_LoadWAV Mix_FreeChunk Mix_PlayChannel Mix_HaltChannel Mix_Volume
 *         Mix_VolumeChunk Mix_LoadMUS Mix_FreeMusic Mix_PlayMusic Mix_HaltMusic
 *         Mix_PlayingMusic Mix_VolumeMusic Mix_GetError.
 * Anything else in real SDL_mixer is intentionally absent -- if a future change
 * needs it, add it here AND in dc_mixer.c rather than guessing.
 */

#ifndef DC_SHIM_SDL_MIXER_H
#define DC_SHIM_SDL_MIXER_H

#ifdef MIX_MAJOR_VERSION
#  error "Real SDL_mixer headers are on the include path; the Dreamcast shim must not be mixed with them."
#endif

#include "SDL_types.h"
#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Audio sample formats, values copied from SDL 1.2's SDL_audio.h so that the
   integer literals src/audio.c stores in Uint16 keep their upstream meaning.
   SH-4 under KOS is little-endian, hence S16SYS == S16LSB. */
#define AUDIO_U8        0x0008
#define AUDIO_S8        0x8008
#define AUDIO_U16LSB    0x0010
#define AUDIO_S16LSB    0x8010
#define AUDIO_U16MSB    0x1010
#define AUDIO_S16MSB    0x9010
#define AUDIO_U16       AUDIO_U16LSB
#define AUDIO_S16       AUDIO_S16LSB
#define AUDIO_U16SYS    AUDIO_U16LSB
#define AUDIO_S16SYS    AUDIO_S16LSB

/* Upstream's volume scale. src/audio.c clamps to [0,128] at lines 645-650 and
   662-666 and initialises sound_context_data_t.volume to 128 (audio.c:205). */
#define MIX_MAX_VOLUME  128

/* Number of channels Mix_PlayChannel can hand back. The AICA has 64 hardware
   voices (kos .../sound/snd_sfxmgr.c:49, a uint64_t in-use mask). */
#define MIX_CHANNELS    64

/* Both types are opaque here. Real SDL_mixer exposes Mix_Chunk's fields, but
   src/ only ever holds pointers to them and casts those to char pointers for a
   NULL test (src/audio_data.c:139-147) -- it never dereferences either -- so
   keeping them opaque stops anyone from depending on a layout we do not have. */
typedef struct Mix_Chunk  Mix_Chunk;
typedef struct _Mix_Music Mix_Music;

/* Open the device. `format` is one of AUDIO_*; `chunksize` is the SDL callback
   buffer size and is accepted but not meaningful on AICA. Returns 0 / -1. */
int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize);

void Mix_CloseAudio(void);

/* Returns 0 if closed, non-zero (the open count) if open, and fills any
   non-NULL out-parameter with the format actually in use -- not the format
   requested. src/audio.c:147 is_audio_open() is the only caller. */
int Mix_QuerySpec(int *frequency, Uint16 *format, int *channels);

/* Load a .wav into AICA sound RAM. NULL on failure, error via Mix_GetError. */
Mix_Chunk *Mix_LoadWAV(const char *file);
void Mix_FreeChunk(Mix_Chunk *chunk);

/* channel == -1 means "first free channel". Returns the channel actually used,
   or -1 on failure. loops: 0 = play once, -1 = loop forever. */
int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);

/* channel == -1 halts every channel. */
int Mix_HaltChannel(int channel);

/* Volumes are 0..MIX_MAX_VOLUME. Both return the PREVIOUS volume. A negative
   `volume` queries without setting. Mix_Volume(-1, v) sets every channel. */
int Mix_Volume(int channel, int volume);
int Mix_VolumeChunk(Mix_Chunk *chunk, int volume);

/* Music: Impulse Tracker (.it) modules, decoded by libmodplug. */
Mix_Music *Mix_LoadMUS(const char *file);
void Mix_FreeMusic(Mix_Music *music);

/* loops: 0 = play once, -1 = loop forever. Returns 0 / -1. */
int Mix_PlayMusic(Mix_Music *music, int loops);
int Mix_HaltMusic(void);
int Mix_PlayingMusic(void);
int Mix_VolumeMusic(int volume);

/* Same aliasing real SDL_mixer uses -- the mixer has no error channel of its
   own. src/audio_data.c:147 calls it. */
#define Mix_GetError SDL_GetError

#ifdef __cplusplus
}
#endif

#endif /* DC_SHIM_SDL_MIXER_H */
