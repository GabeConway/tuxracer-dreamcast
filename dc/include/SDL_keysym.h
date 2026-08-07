/*
 * SDL_keysym.h -- Dreamcast shim, NOT libSDL.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * dc/include/config.h defines HAVE_SDL, so src/winsys.h takes its SDL branch
 * (src/winsys.h:40-123) and *defines its own public enums in terms of SDLK_*
 * constants*:
 *
 *     typedef enum { WSK_NOT_AVAIL = SDLK_UNKNOWN, ... WSK_LAST } winsys_keysym_t;
 *
 * src/winsys.h is upstream-verbatim, so those names have to come from
 * somewhere. dc/include/SDL.h is scoped to the audio + clock shim and
 * deliberately does not carry them, so they live here, in the same file real
 * SDL 1.2 puts them in, and SDL.h pulls this in the same way real SDL does.
 *
 * Consumers, verified with
 *   grep -rln 'WSK_\|winsys_keysym_t' src/
 * are src/winsys.h, src/keyboard.c, src/keyboard_util.c, src/race_select.c,
 * src/event_select.c -- none of which name an SDLK_* constant directly. So
 * the only contract this file has to honour is: every SDLK_* that
 * src/winsys.h names must exist, and the values must keep upstream's
 * invariant that "special" keys are >= 256 (src/winsys.c:344 used
 * `key >= 256` to set the special flag; dc/src/dc_winsys.c sets it
 * explicitly, but src/keyboard.c:26-27 still sizes both key tables at
 * WSK_LAST, so SDLK_LAST must exceed every other value here).
 *
 * Values are SDL 1.2's, unchanged. Printable keys are simply their ASCII
 * codes in SDL 1.2 (SDLK_a == 'a', SDLK_RETURN == 13, SDLK_ESCAPE == 27), so
 * only the >= 256 block needs spelling out; the handful of sub-256 names
 * below are the ones with non-obvious spellings.
 *
 * Cost: 2 * SDLK_LAST pointers of BSS in src/keyboard.c:29-30 == 2584 bytes
 * on this 16 MB machine. Trimming SDLK_LAST would save a couple of KB and
 * silently break any options file that binds a function key, so it stays.
 */

#ifndef DC_SHIM_SDL_KEYSYM_H
#define DC_SHIM_SDL_KEYSYM_H

#ifdef SDL_MAJOR_VERSION
#  error "Real SDL headers are on the include path; the Dreamcast shim must not be mixed with them."
#endif

typedef enum {
    SDLK_UNKNOWN        = 0,
    SDLK_BACKSPACE      = 8,
    SDLK_TAB            = 9,
    SDLK_RETURN         = 13,
    SDLK_ESCAPE         = 27,
    SDLK_SPACE          = 32,
    SDLK_DELETE         = 127,

    /* Numeric keypad */
    SDLK_KP0            = 256,
    SDLK_KP1            = 257,
    SDLK_KP2            = 258,
    SDLK_KP3            = 259,
    SDLK_KP4            = 260,
    SDLK_KP5            = 261,
    SDLK_KP6            = 262,
    SDLK_KP7            = 263,
    SDLK_KP8            = 264,
    SDLK_KP9            = 265,
    SDLK_KP_PERIOD      = 266,
    SDLK_KP_DIVIDE      = 267,
    SDLK_KP_MULTIPLY    = 268,
    SDLK_KP_MINUS       = 269,
    SDLK_KP_PLUS        = 270,
    SDLK_KP_ENTER       = 271,
    SDLK_KP_EQUALS      = 272,

    /* Arrows + Home/End pad */
    SDLK_UP             = 273,
    SDLK_DOWN           = 274,
    SDLK_RIGHT          = 275,
    SDLK_LEFT           = 276,
    SDLK_INSERT         = 277,
    SDLK_HOME           = 278,
    SDLK_END            = 279,
    SDLK_PAGEUP         = 280,
    SDLK_PAGEDOWN       = 281,

    /* Function keys */
    SDLK_F1             = 282,
    SDLK_F2             = 283,
    SDLK_F3             = 284,
    SDLK_F4             = 285,
    SDLK_F5             = 286,
    SDLK_F6             = 287,
    SDLK_F7             = 288,
    SDLK_F8             = 289,
    SDLK_F9             = 290,
    SDLK_F10            = 291,
    SDLK_F11            = 292,
    SDLK_F12            = 293,
    SDLK_F13            = 294,
    SDLK_F14            = 295,
    SDLK_F15            = 296,

    /* Key state modifier keys */
    SDLK_NUMLOCK        = 300,
    SDLK_CAPSLOCK       = 301,
    SDLK_SCROLLOCK      = 302,
    SDLK_RSHIFT         = 303,
    SDLK_LSHIFT         = 304,
    SDLK_RCTRL          = 305,
    SDLK_LCTRL          = 306,
    SDLK_RALT           = 307,
    SDLK_LALT           = 308,
    SDLK_RMETA          = 309,
    SDLK_LMETA          = 310,

    SDLK_LAST           = 323
} SDLKey;

#endif /* DC_SHIM_SDL_KEYSYM_H */
