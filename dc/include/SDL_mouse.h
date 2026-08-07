/*
 * SDL_mouse.h -- Dreamcast shim, NOT libSDL.
 *
 * Companion to SDL_keysym.h, and there for the same reason: src/winsys.h's
 * SDL branch defines its public pointer enums in terms of SDL constants
 * (src/winsys.h:114-123),
 *
 *     typedef enum { WS_LEFT_BUTTON = SDL_BUTTON_LEFT, ... } winsys_mouse_button_t;
 *     typedef enum { WS_MOUSE_DOWN = SDL_PRESSED, ... } winsys_button_state_t;
 *
 * and src/winsys.h is upstream-verbatim so the names must exist.
 *
 * There is no mouse on a stock Dreamcast. dc/src/dc_winsys.c drives a virtual
 * cursor from the analog stick and reports the A button as WS_LEFT_BUTTON, so
 * WS_MIDDLE_BUTTON and WS_RIGHT_BUTTON are never emitted -- they are declared
 * only because src/ui_mgr.c:456-462 switches on all three.
 *
 * SDL_PRESSED / SDL_RELEASED live in SDL_events.h in real SDL 1.2, not here;
 * they are folded into this file because the shim has no event header and
 * these two values exist solely to type winsys_button_state_t.
 *
 * Values are SDL 1.2's, unchanged.
 */

#ifndef DC_SHIM_SDL_MOUSE_H
#define DC_SHIM_SDL_MOUSE_H

#ifdef SDL_MAJOR_VERSION
#  error "Real SDL headers are on the include path; the Dreamcast shim must not be mixed with them."
#endif

#define SDL_BUTTON_LEFT     1
#define SDL_BUTTON_MIDDLE   2
#define SDL_BUTTON_RIGHT    3

#define SDL_RELEASED        0
#define SDL_PRESSED         1

#endif /* DC_SHIM_SDL_MOUSE_H */
