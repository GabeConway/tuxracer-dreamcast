/*
 * dc_winsys.c -- KallistiOS + GLdc implementation of src/winsys.h.
 *
 * Replaces src/winsys.c (SDL 1.2), which is excluded from the Dreamcast TU
 * list.  Every function declared in src/winsys.h is defined here.
 *
 * The Dreamcast has no window system, no pointer and no keyboard in the
 * normal case, so this file's real job is to turn one maple controller into
 * the event stream src/ expects: a keyboard callback, a mouse-button
 * callback and a motion callback, driven from a poll-and-diff loop.
 *
 * See kb/design-winsys.md for the reasoning and the input map.
 */

#include "tuxracer.h"
#include "winsys.h"
#include "game_config.h"
#include "dc_input.h"
#include "tr_harness.h"
#include "dc_fbdump.h"

#include <setjmp.h>
#include <stdlib.h>

#include <kos/timer.h>
#include <kos/thread.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <GL/glkos.h>


/*---------------------------------------------------------------------------*/
/* Tunables                                                                   */
/*---------------------------------------------------------------------------*/

/* Virtual cursor travel at full stick deflection, in pixels per second.
   640px wide screen; ~1.3 s to cross it corner to corner. */
#define DC_CURSOR_SPEED     500.0

/* Upper bound on the frame delta used for cursor integration.  A long
   synchronous load (course/texture load blocks the loop for seconds) would
   otherwise fling the cursor across the screen on the first frame back. */
#define DC_MAX_CURSOR_DT    0.1


/*---------------------------------------------------------------------------*/
/* Callbacks and window state                                                 */
/*---------------------------------------------------------------------------*/

static winsys_display_func_t display_func = NULL;
static winsys_idle_func_t idle_func = NULL;
static winsys_reshape_func_t reshape_func = NULL;
static winsys_keyboard_func_t keyboard_func = NULL;
static winsys_mouse_func_t mouse_func = NULL;
static winsys_motion_func_t motion_func = NULL;
static winsys_motion_func_t passive_motion_func = NULL;
static winsys_atexit_func_t atexit_func = NULL;

static bool_t redisplay = False;

/* Non-escaping longjmp target for winsys_exit(); see winsys_exit(). */
static jmp_buf exit_jmp;
static bool_t loop_running = False;
static int exit_code = 0;


/*---------------------------------------------------------------------------*/
/* Controller snapshot (shared with dc_joystick.c via dc_input.h)             */
/*---------------------------------------------------------------------------*/

static dc_pad_t pad;

void dc_input_poll( void )
{
    maple_device_t *dev;
    cont_state_t *st;

    /* Re-enumerate every frame rather than caching the device pointer: pads
       are hot-pluggable on the Dreamcast and a cached pointer would dangle
       across an unplug. */
    dev = maple_enum_type( 0, MAPLE_FUNC_CONTROLLER );
    st = ( dev != NULL ) ? (cont_state_t *) maple_dev_status( dev ) : NULL;

    if ( st == NULL ) {
        memset( &pad, 0, sizeof( pad ) );
        return;
    }

    pad.present = 1;
    pad.buttons = st->buttons;
    pad.joyx = st->joyx;
    pad.joyy = st->joyy;
    pad.ltrig = st->ltrig;
    pad.rtrig = st->rtrig;
}

const dc_pad_t *dc_input_state( void )
{
    return &pad;
}


/*---------------------------------------------------------------------------*/
/* Synthetic keyboard                                                         */
/*---------------------------------------------------------------------------*/

/* The pad drives exactly these keysyms.  Everything else the game binds
   (screenshot '=', view keys '1'/'2'/'3', reset, snow toggle) has no pad
   input and is unreachable -- documented in kb/design-winsys.md.

   Enter is 13 (== SDLK_RETURN) and Escape is 27 (== SDLK_ESCAPE) because that
   is what the menu code compares against literally (src/race_select.c:1237,
   1243; src/event_select.c:506,512; src/game_type_select.c:232,236).  Note
   src/keyboard_util.c:61 maps the *name* "enter" to '\n' (10) instead -- if
   we sent 10, no menu button would fire. */
enum {
    VK_LEFT = 0,
    VK_RIGHT,
    VK_UP,
    VK_DOWN,
    VK_ENTER,
    VK_ESCAPE,
    VK_PAUSE,
    DC_NUM_VKEYS
};

typedef struct {
    unsigned int key;
    bool_t special;
} dc_vkey_t;

static const dc_vkey_t vkeys[ DC_NUM_VKEYS ] = {
    { WSK_LEFT,  True  },
    { WSK_RIGHT, True  },
    { WSK_UP,    True  },
    { WSK_DOWN,  True  },
    { 13,        False },   /* Enter */
    { 27,        False },   /* Escape */
    { 'p',       False }    /* pause_key default, game_config.c:575 */
};

static unsigned int prev_vkeys = 0;

static unsigned int compute_vkey_mask( void )
{
    unsigned int m = 0;

    if ( !pad.present ) {
        return 0;
    }

    if ( pad.buttons & CONT_DPAD_LEFT )  m |= 1u << VK_LEFT;
    if ( pad.buttons & CONT_DPAD_RIGHT ) m |= 1u << VK_RIGHT;
    if ( pad.buttons & CONT_DPAD_UP )    m |= 1u << VK_UP;
    if ( pad.buttons & CONT_DPAD_DOWN )  m |= 1u << VK_DOWN;

    if ( pad.buttons & CONT_B ) {
        m |= 1u << VK_ESCAPE;
    }

    if ( pad.buttons & CONT_START ) {
        /* Start alone is Enter.  Start with the left trigger held is 'p'
           (pause).  The pad has no spare button, and sending 'p'
           unconditionally would collide with the practice-mode shortcut in
           src/game_type_select.c:241 while Enter is already bound there. */
        if ( pad.ltrig >= DC_TRIG_THRESHOLD ) {
            m |= 1u << VK_PAUSE;
        } else {
            m |= 1u << VK_ENTER;
        }
    }

    return m;
}

static void dispatch_keys( int cursor_ix, int cursor_iy )
{
    unsigned int now = compute_vkey_mask();
    unsigned int changed = now ^ prev_vkeys;
    int i;

    /* Commit the new state before dispatching: a callback may longjmp out via
       winsys_exit(), and a stale prev_vkeys would then replay on re-entry. */
    prev_vkeys = now;

    if ( keyboard_func == NULL || changed == 0 ) {
        return;
    }

    for ( i = 0; i < DC_NUM_VKEYS; i++ ) {
        if ( ( changed & ( 1u << i ) ) == 0 ) {
            continue;
        }

        (*keyboard_func)( vkeys[i].key,
                          vkeys[i].special,
                          (bool_t) ( ( now & ( 1u << i ) ) == 0 ),
                          cursor_ix, cursor_iy );
    }
}


/*---------------------------------------------------------------------------*/
/* Virtual cursor                                                             */
/*---------------------------------------------------------------------------*/

/* Window coordinates: origin top-left, y grows downward -- the convention
   src/ui_mgr.c:447 and :486 flip with `y = getparam_y_resolution() - y'. */
static scalar_t cursor_x = DC_SCREEN_WIDTH / 2;
static scalar_t cursor_y = DC_SCREEN_HEIGHT / 2;
static bool_t click_down = False;

static scalar_t deadzone( int raw )
{
    if ( raw > -DC_STICK_DEADZONE && raw < DC_STICK_DEADZONE ) {
        return 0.0;
    }
    return (scalar_t) raw / 128.0;
}

static void update_pointer( scalar_t dt )
{
    /* All three pointer callbacks are NULL exactly in the modes that own the
       stick for steering (src/racing.c:72-75, src/intro.c:68-70).  Freezing
       the cursor there stops a race from parking it in a screen corner. */
    bool_t pointer_active = (bool_t) ( mouse_func != NULL ||
                                       motion_func != NULL ||
                                       passive_motion_func != NULL );
    bool_t moved = False;
    bool_t a_now;
    scalar_t fx, fy;

    if ( pad.present && pointer_active ) {
        fx = deadzone( pad.joyx );
        fy = deadzone( pad.joyy );

        if ( fx != 0.0 || fy != 0.0 ) {
            cursor_x += fx * DC_CURSOR_SPEED * dt;
            cursor_y += fy * DC_CURSOR_SPEED * dt;

            if ( cursor_x < 0 ) cursor_x = 0;
            if ( cursor_y < 0 ) cursor_y = 0;
            if ( cursor_x > DC_SCREEN_WIDTH - 1 )  cursor_x = DC_SCREEN_WIDTH - 1;
            if ( cursor_y > DC_SCREEN_HEIGHT - 1 ) cursor_y = DC_SCREEN_HEIGHT - 1;

            moved = True;
        }
    }

    /* Motion is reported before the button edge so a click lands on the
       coordinates the widget code just saw. */
    if ( moved ) {
        if ( click_down ) {
            if ( motion_func ) {
                (*motion_func)( (int) cursor_x, (int) cursor_y );
            }
        } else {
            if ( passive_motion_func ) {
                (*passive_motion_func)( (int) cursor_x, (int) cursor_y );
            }
        }
    }

    /* A is the left mouse button.  Tracked even while pointer_active is
       False so that a press-and-release during a race cannot leak a stale
       button-down into the next menu. */
    a_now = (bool_t) ( pad.present && ( pad.buttons & CONT_A ) );

    if ( a_now != click_down ) {
        click_down = a_now;

        if ( mouse_func ) {
            (*mouse_func)( WS_LEFT_BUTTON,
                           a_now ? WS_MOUSE_DOWN : WS_MOUSE_UP,
                           (int) cursor_x, (int) cursor_y );
        }
    }
}


/*---------------------------------------------------------------------------*/
/* winsys.h implementation                                                    */
/*---------------------------------------------------------------------------*/

void winsys_post_redisplay()
{
    redisplay = True;
}

void winsys_set_display_func( winsys_display_func_t func )
{
    display_func = func;
}

void winsys_set_idle_func( winsys_idle_func_t func )
{
    idle_func = func;
}

void winsys_set_reshape_func( winsys_reshape_func_t func )
{
    reshape_func = func;
}

void winsys_set_keyboard_func( winsys_keyboard_func_t func )
{
    keyboard_func = func;
}

void winsys_set_mouse_func( winsys_mouse_func_t func )
{
    mouse_func = func;
}

void winsys_set_motion_func( winsys_motion_func_t func )
{
    motion_func = func;
}

void winsys_set_passive_motion_func( winsys_motion_func_t func )
{
    passive_motion_func = func;
}


/*
  Submits the GL vertex lists to the PVR and flips.  GLdc does the whole
  pvr_wait_ready / scene_begin / list submit / scene_finish sequence inside
  this one call, which is why no PVR scene is ever left open between frames
  (see winsys_exit() -- that property is what makes the longjmp safe).
*/
void winsys_swap_buffers()
{
    glKosSwapBuffers();

#ifdef TR_HARNESS
    /*
     * One PERF line per second, never per frame: at 1.5 Mbps a console line
     * still costs real emulated time, and the harness explicitly forbids
     * logging inside a timed window. Once a second is enough to prove the loop
     * is alive and to gate the frame budget in harness/dc/perf.sh, and it is
     * the only frame-rate number this project has -- upstream's own fps
     * counter (src/fps.c) only ever draws to the HUD.
     */
    {
        static uint64 t_last = 0;
        static int    frames = 0;
        uint64        now = timer_ms_gettime64();

        frames++;

        /* TR_FBDUMP_FRAME=<n> ships frame n to the host as a picture
         * (dc/src/dc_fbdump.c). Off unless the build asks for it: the dump
         * costs ~1.4 s of console time and would wreck any timing around it. */
#ifdef TR_FBDUMP_FRAME
        {
            static int total = 0;
            if(++total == (TR_FBDUMP_FRAME)) {
                tr_fbdump();
            }
        }
#endif

        /* One mark the moment the game first reaches the racing view. This is
         * what turns "it boots" into a real regression gate: harness/dc/
         * playtest.sh asserts on it, so a change that breaks course loading
         * fails a check instead of being noticed weeks later in a screenshot. */
        {
            static int raced = 0;
            if(!raced && g_game.mode == RACING) {
                raced = 1;
                TR_MARK("RACING");
            }
        }

#ifdef TR_AUTOEXIT
        /* End the run cleanly after N frames so the harness gets its END
         * marker. A game never exits on its own, and neither does Flycast, so
         * without this every gate can only ever end in a timeout -- which is
         * indistinguishable from a hang. */
        {
            static int total_ae = 0;
            if(++total_ae >= (TR_AUTOEXIT)) {
                tr_harness_end(0);
                exit(0);
            }
        }
#endif

        if(t_last == 0) {
            t_last = now;
            TR_MARK("FIRST_FRAME");
        }
        else if(now - t_last >= 1000) {
            TR_PERF("fps=%d frame_ms=%d mode=%d end=%u drawelem=%u calllist=%u",
                    (int)((frames * 1000ULL) / (now - t_last)),
                    (int)((now - t_last) / (frames ? frames : 1)),
                    (int)g_game.mode,
                    tr_gl_n_end, tr_gl_n_drawelem, tr_gl_n_calllist);
            tr_gl_n_end = tr_gl_n_drawelem = tr_gl_n_calllist = 0;
            t_last = now;
            frames = 0;
        }
    }
#endif
}


/*
  There is no hardware pointer, so a warp is just an assignment.  It
  deliberately does NOT synthesise a motion event: src/loop.c:145 warps to
  ui_get_mouse_position() every frame when capture_mouse is on, and echoing
  that back as motion would be an infinite feedback loop.
*/
void winsys_warp_pointer( int x, int y )
{
    cursor_x = x;
    cursor_y = y;
}


/*
  No cursor to show or hide -- src/ui_mgr.c:521 draws its own textured cursor
  quad at ui_get_mouse_position(), which is what the player actually sees.
*/
void winsys_show_cursor( bool_t visible )
{
    (void) visible;
}


/*
  We never synthesise repeats: every pad key is edge-triggered in
  dispatch_keys().  main.c:183 only ever asks for repeats to be disabled, so
  this is a no-op in both directions.
*/
void winsys_enable_key_repeat( bool_t enabled )
{
    (void) enabled;
}


void winsys_init( int *argc, char **argv, char *window_title,
                  char *icon_title )
{
    (void) argc;
    (void) argv;
    (void) icon_title;

    /* GLdc owns the PVR.  glKosInit() performs the pvr_init() itself and
       calling pvr_init() here would double-initialise it.  Verified in
       /opt/toolchains/dc/kos/examples/dreamcast/gldc/basic/gl/gltest.c:37-39
       ("OpenGL will initialize the PVR for you, so do not call pvr_init.
       OpenGL is initialized with glKosInit().") and :127, where glKosInit()
       is the first call in main() with no PVR setup before it.

       This runs at src/main.c:180, before init_opengl_extensions() and the
       first glBlendFunc(), so the context exists by the time src/ touches
       GL. */
    glKosInit();

    /* The framebuffer is 640x480 and there is no resize path.  Force the
       config to agree so every consumer of the resolution params sees the
       truth: src/render_util.c:45 (projection), src/ui_mgr.c:447 (y flip),
       src/loop.c:132-146 (cursor clamp).  getparam_fullscreen(),
       getparam_bpp_mode() and getparam_force_window_position() are ignored. */
    setparam_x_resolution( DC_SCREEN_WIDTH );
    setparam_y_resolution( DC_SCREEN_HEIGHT );

    printf( "%s: GLdc up, %dx%d\n", window_title,
            DC_SCREEN_WIDTH, DC_SCREEN_HEIGHT );

    TR_MARK( "WINSYS_INIT" );
}


void winsys_shutdown()
{
    /* Called from cleanup() (src/main.c:83), which is our atexit_func. */
    glKosShutdown();
}


/*
  Main loop.  Unlike the SDL version this CAN return, but only through
  winsys_exit(); src/main.c:275 calls it last and returns 0 straight after,
  so returning is a clean program end.
*/
void winsys_process_events()
{
    static uint64_t prev_ms;
    static winsys_reshape_func_t last_reshape;
    static scalar_t dt;
    static uint64_t now_ms;

    /* File-scope-lifetime storage (static) rather than locals: locals
       modified between setjmp() and longjmp() have indeterminate values
       after the longjmp unless they are volatile. */
    prev_ms = timer_ms_gettime64();
    last_reshape = NULL;

    loop_running = True;

    if ( setjmp( exit_jmp ) == 0 ) {
        while ( True ) {
            now_ms = timer_ms_gettime64();
            dt = (scalar_t) ( now_ms - prev_ms ) * 1.0e-3;
            prev_ms = now_ms;

            if ( dt > DC_MAX_CURSOR_DT ) {
                dt = DC_MAX_CURSOR_DT;
            }

            dc_input_poll();

            /* winsys_init() cannot fire the initial reshape because no mode
               has registered a handler that early (main.c:180 precedes every
               *_init()).  Firing once per newly-installed handler gives each
               mode its projection before its first display callback. */
            if ( reshape_func != NULL && reshape_func != last_reshape ) {
                last_reshape = reshape_func;
                (*reshape_func)( DC_SCREEN_WIDTH, DC_SCREEN_HEIGHT );
            }

            update_pointer( dt );
            dispatch_keys( (int) cursor_x, (int) cursor_y );

#ifdef TR_AUTOKEY
            /*
             * Unattended progress through the menus. The harness can read what
             * the guest prints but cannot press anything, so without this the
             * only screen that can ever be tested is the splash. Every
             * TR_AUTOKEY frames, synthesise a press-and-release of Enter --
             * which is what advances the splash, confirms a menu selection and
             * starts a race -- so a build can walk itself to the racing view
             * and dump a frame of it.
             *
             * Deliberately Enter and nothing else: a fixed key with a fixed
             * period is reproducible, and anything cleverer would be a script
             * that silently rots as the menus change.
             */
            {
                static int autokey_n = 0;
                if( keyboard_func != NULL && ++autokey_n % (TR_AUTOKEY) == 0 ) {
                    (*keyboard_func)( 13, False, False,
                                      (int) cursor_x, (int) cursor_y );
                    (*keyboard_func)( 13, False, True,
                                      (int) cursor_x, (int) cursor_y );
                }
            }
#endif

            if ( redisplay && display_func ) {
                redisplay = False;
                (*display_func)();
            } else if ( idle_func ) {
                (*idle_func)();
            }

            /* Yield so the dc_mixer.c decode thread gets the CPU even in the
               degenerate case where the current mode never swaps buffers
               (glKosSwapBuffers already blocks on vblank when it does).
               thd_pass() rather than the shim's SDL_Delay(1) that upstream
               used at src/winsys.c:410: a 1 ms sleep is 6% of a 60 Hz frame
               budget, and a plain yield achieves the same scheduling. */
            thd_pass();
        }
    }

    /* Only reachable via winsys_exit(). */
    loop_running = False;

    if ( atexit_func ) {
        (*atexit_func)();
    }

    printf( "\n*** Tux Racer: end of run, exit code %d ***\n", exit_code );
    fflush( stdout );

    /* Harness protocol: the host kills Flycast on this line, so it goes last
       (dc/include/tr_harness.h).  Compiles away without -DTR_HARNESS. */
    tr_harness_end( exit_code );
}


void winsys_atexit( winsys_atexit_func_t func )
{
    static bool_t called = False;

    check_assertion( called == False, "winsys_atexit called twice" );

    called = True;
    atexit_func = func;
}


/*
  Leaves the program.

  winsys_exit() is called from deep inside display/idle callbacks
  (src/game_type_select.c:81,233 on the Esc/quit path; src/error_util.c:64,79
  and src/image.c on fatal errors).  Calling exit() from there would tear the
  process down with GL and game state live on the stack.  Instead we longjmp
  back to the top of winsys_process_events(), run the atexit hook from a
  quiescent point, print an end-of-run marker and *return* to main(), which
  returns 0 and hands control back to the KOS loader normally.

  This is only safe because GLdc opens and closes the PVR scene entirely
  inside glKosSwapBuffers(), so no PVR scene is ever open at the point a
  callback can call us.  Vertices buffered for the current, abandoned frame
  are simply dropped.

  The one case we cannot longjmp out of is a fatal error raised before
  winsys_process_events() ran setjmp() (e.g. handle_system_error() during
  data loading).  There exit() is correct and is what we do.
*/
void winsys_exit( int code )
{
    exit_code = code;

    if ( loop_running ) {
        longjmp( exit_jmp, 1 );
    }

    /* Pre-loop failure path. */
    if ( atexit_func ) {
        (*atexit_func)();
    }

    printf( "\n*** Tux Racer: aborted before main loop, exit code %d ***\n",
            code );
    fflush( stdout );

    tr_harness_end( code );
    exit( code );
}

/* EOF */
