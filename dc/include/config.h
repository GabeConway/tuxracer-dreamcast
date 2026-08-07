/*
 * config.h — hand-written replacement for the autoconf-generated one.
 *
 * Upstream Tux Racer discovers all of this with ./configure. There is no
 * configure run for sh-elf, so every answer is stated here explicitly and
 * every one of them is a claim about KallistiOS + newlib that the build will
 * check for us: get one wrong and the compile fails loudly rather than
 * producing a subtly different game.
 *
 * tuxracer.h includes this via `#ifdef HAVE_CONFIG_H` (src/tuxracer.h:23),
 * and dc/Makefile passes -DHAVE_CONFIG_H to every TU.
 */

#ifndef TR_DC_CONFIG_H
#define TR_DC_CONFIG_H

#define VERSION "0.61-dc"
#define PACKAGE "tuxracer"

/*
 * The data directory. On the Dreamcast the game data lives at the root of the
 * disc, which KOS mounts at /cd. src/game_config.c:78 falls back to
 * "/usr/local/share/tuxracer" without this, and every asset open would miss.
 *
 * The game also chdir()s into course directories (src/course_load.c:345), so
 * /cd must be a real, seekable, chdir-able mount — it is; KOS's iso9660 VFS
 * supports both.
 */
#define DATA_DIR "/cd"

/*
 * SDL: we do NOT link SDL. dc/include/SDL.h and dc/include/SDL_mixer.h are
 * shims over KOS audio (dc/src/dc_mixer.c), and src/winsys.c — the only real
 * SDL video/event consumer — is excluded from the build in favour of
 * dc/src/dc_winsys.c.
 *
 * HAVE_SDL is still defined because src/ uses it to select the *event model*,
 * not the library: with it undefined, large parts of src/ fall back to GLUT
 * (HAVE_GLUT), which does not exist here at all.
 */
#define HAVE_SDL 1
#define HAVE_SDL_MIXER 1

/*
 * NOT defined: HAVE_SDL_JOYSTICKOPEN. That macro gates upstream's
 * src/joystick.c, which we replace wholesale with dc/src/dc_joystick.c. The
 * Dreamcast pad is wired in through the winsys layer instead, so the joystick
 * code paths in src/ stay compiled out.
 */

/* Tcl. dc/include/tcl.h is our Jim Tcl compatibility header (kb/design-tcl.md). */
#define TCL_HEADER <tcl.h>

/*
 * Math feature probes. newlib provides isnan() as a macro in <math.h>; it does
 * NOT provide the SVR4 finite(). src/tuxracer.h maps FINITE() onto whichever
 * of these is available, in the order HAVE_FINITE, HAVE__FINITE, HAVE_ISNAN.
 */
#define HAVE_ISNAN 1

/* newlib has gettimeofday(); KOS backs it with the AICA-independent timer. */
#define HAVE_GETTIMEOFDAY 1

/*
 * NOT defined: HAVE_IEEEFP_H, HAVE_GL_GLX_H, HAVE_GLXGETPROCADDRESSARB,
 * HAVE_GLUT. There is no X11 and no GLUT on this platform, and GLdc resolves
 * every entry point at link time, so the glXGetProcAddressARB path in
 * src/gl_util.c must stay compiled out.
 */

#endif /* TR_DC_CONFIG_H */
