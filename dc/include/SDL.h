/*
 * SDL.h -- Dreamcast shim, NOT libSDL.
 *
 * There is no SDL on this build. src/audio.c, src/audio_data.c and src/loop.c
 * are compiled verbatim and they `#include "SDL.h"`, so this file has to exist
 * and has to declare exactly what those three touch -- nothing more.
 *
 * Scope, deliberately: audio + the millisecond clock. The video, event and
 * joystick sides of SDL are NOT here; src/winsys.c is not built on Dreamcast,
 * dc/src/dc_winsys.c replaces it. If you came here looking for SDL_Surface or
 * SDL_PollEvent, you want that file instead.
 *
 * Symbols declared here are implemented in dc/src/dc_mixer.c.
 *
 * Verified call sites (grep -rhno 'SDL_[A-Za-z_]*' src/audio.c src/audio_data.c
 * src/loop.c) -- the complete set is: SDL_Init, SDL_INIT_AUDIO, SDL_GetError,
 * SDL_GetTicks. SDL_LockAudio/SDL_UnlockAudio/SDL_Delay are additionally used
 * by src/winsys.c:333,397,410 and are declared here so a winsys replacement can
 * reuse them.
 */

#ifndef DC_SHIM_SDL_H
#define DC_SHIM_SDL_H

#ifdef SDL_MAJOR_VERSION
#  error "Real SDL headers are on the include path; the Dreamcast shim must not be mixed with them."
#endif

#include "SDL_types.h"

/* Keysym and pointer constants. Not part of the audio scope above, but they
   have to arrive through SDL.h: src/winsys.h:26 includes "SDL.h" and then
   defines winsys_keysym_t / winsys_mouse_button_t / winsys_button_state_t
   directly in terms of SDLK_*, SDL_BUTTON_* and SDL_PRESSED
   (src/winsys.h:47-123), and src/winsys.h is upstream-verbatim. Real SDL 1.2
   reaches these through SDL_keyboard.h/SDL_events.h from SDL.h too, so the
   include graph matches. Consumed by dc/src/dc_winsys.c. */
#include "SDL_keysym.h"
#include "SDL_mouse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Subsystem flags. Only SDL_INIT_AUDIO is honoured; the others are declared so
   that code testing for them still compiles, and are ignored by SDL_Init. */
#define SDL_INIT_TIMER          0x00000001
#define SDL_INIT_AUDIO          0x00000010
#define SDL_INIT_VIDEO          0x00000020
#define SDL_INIT_JOYSTICK       0x00000200
#define SDL_INIT_NOPARACHUTE    0x00100000

/* Always succeeds (returns 0). The AICA is not actually brought up here --
   Mix_OpenAudio() does that -- because src/audio.c:77 calls SDL_Init() purely
   as a gate before Mix_OpenAudio() and treats <0 as fatal. */
int SDL_Init(Uint32 flags);

/* Last error set by the shim or the mixer. Never NULL: returns "" when clean.
   src/audio_data.c:147 feeds this straight to a %s. */
const char *SDL_GetError(void);

/* Mutual exclusion against the music decode thread in dc/src/dc_mixer.c.
   Recursive: safe to call from inside a Mix_* call. */
void SDL_LockAudio(void);
void SDL_UnlockAudio(void);

/* Milliseconds since KOS start. src/loop.c:86 uses this for frame timing. */
Uint32 SDL_GetTicks(void);

/* Yields to other threads; the music thread needs this to get scheduled. */
void SDL_Delay(Uint32 ms);

#ifdef __cplusplus
}
#endif

#endif /* DC_SHIM_SDL_H */
