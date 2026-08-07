/*
 * SDL_types.h -- Dreamcast shim, NOT libSDL.
 *
 * Split out from SDL.h on purpose: Uint16/Uint32 are the only SDL spelling
 * that leaks into the audio path (src/audio.c:62 `Uint16 format`), but the
 * winsys/input shim will want the same typedefs. Keeping them in their own
 * header means both shims can include this without fighting over SDL.h.
 */

#ifndef DC_SHIM_SDL_TYPES_H
#define DC_SHIM_SDL_TYPES_H

#ifdef SDL_MAJOR_VERSION
#  error "Real SDL headers are on the include path; the Dreamcast shim must not be mixed with them."
#endif

#include <stdint.h>

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;

#endif /* DC_SHIM_SDL_TYPES_H */
