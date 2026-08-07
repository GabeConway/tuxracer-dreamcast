/*
 * dc_joystick.c -- KallistiOS implementation of src/joystick.h.
 *
 * Replaces src/joystick.c (SDL), which is excluded from the Dreamcast TU
 * list.  The Dreamcast controller *is* the joystick: there is no enumeration
 * and no configurable axis mapping to honour, so this file is a thin,
 * deadzoned view onto the shared pad snapshot that dc_winsys.c samples once
 * per frame (see dc/include/dc_input.h for why the poll lives there).
 *
 * Button indices below are dictated by the upstream defaults in
 * src/game_config.c:585-606, which this port does not change:
 *     joystick_paddle_button   0
 *     joystick_trick_button    1
 *     joystick_brake_button    2
 *     joystick_jump_button     3
 *     joystick_continue_button 0
 * so index 0 must be the "confirm / go" button for the intro, pause and
 * game-over continue prompts as well as for paddling.
 */

#include "tuxracer.h"
#include "joystick.h"
#include "game_config.h"
#include "dc_input.h"

#include <dc/maple/controller.h>

/* Named for readability; the numbers are upstream's, see header comment. */
#define DC_JS_PADDLE    0
#define DC_JS_TRICK     1
#define DC_JS_BRAKE     2
#define DC_JS_JUMP      3


void init_joystick()
{
    /* Nothing to open.  Prime the snapshot so is_joystick_active() is
       meaningful immediately: main.c:224 calls us long before
       winsys_process_events() takes over the polling. */
    dc_input_poll();

    print_debug( DEBUG_JOYSTICK, "Dreamcast controller %s",
                 dc_input_state()->present ? "found" : "not connected" );
}


bool_t is_joystick_active()
{
    return (bool_t) dc_input_state()->present;
}


void update_joystick()
{
    /* Deliberately empty.  dc_winsys.c polls maple once at the top of every
       frame; re-polling here (racing.c:176, intro.c:121, paused.c:118,
       game_over.c:246 all call this mid-frame) would let the axes read a
       different sample than the key events already dispatched this frame. */
}


/*
  Deadzoned axis value in [-1,1], matching SDL's axis/32768.0 scaling range.
  getparam_joystick_x_axis()/y_axis() are ignored: the pad has exactly one
  analog stick, so remapping which "axis number" it is has no meaning here.
*/
static scalar_t axis_value( int raw )
{
    if ( raw > -DC_STICK_DEADZONE && raw < DC_STICK_DEADZONE ) {
        return 0.0;
    }

    /* /128 rather than /127: the stick's negative range is one count wider,
       so this keeps full-left at exactly -1.0 and costs a hair of full-right
       travel, which the caller's 0.1 and 0.5 thresholds do not care about
       (racing.c:181-203). */
    return (scalar_t) raw / 128.0;
}


scalar_t get_joystick_x_axis()
{
    return axis_value( dc_input_state()->joyx );
}


scalar_t get_joystick_y_axis()
{
    /* Positive is down/back, which racing.c:194 reads as brake and :202 as
       paddle when negative.  See the polarity note in dc_input.h. */
    return axis_value( dc_input_state()->joyy );
}


bool_t is_joystick_button_down( int button )
{
    const dc_pad_t *p = dc_input_state();

    if ( !p->present ) {
        return False;
    }

    switch ( button ) {
    case DC_JS_PADDLE:
        /* R trigger folds into paddle so the pad reads like a racing game;
           A does the same job for players who prefer face buttons, and is
           also what the continue prompts poll. */
        return (bool_t) ( ( p->buttons & CONT_A ) != 0 ||
                          p->rtrig >= DC_TRIG_THRESHOLD );

    case DC_JS_TRICK:
        return (bool_t) ( ( p->buttons & CONT_X ) != 0 );

    case DC_JS_BRAKE:
        /* L trigger only.  B is NOT a brake: dc_winsys.c maps B to Escape,
           which is the quit_key during a race (game_config.c:537), so
           doubling it up as brake would abort the run every time the player
           slowed down. */
        return (bool_t) ( p->ltrig >= DC_TRIG_THRESHOLD );

    case DC_JS_JUMP:
        return (bool_t) ( ( p->buttons & CONT_Y ) != 0 );

    default:
        /* Upstream warns once and returns False for out-of-range buttons;
           we just return False.  Only reachable if the user edits the
           joystick_*_button params in ~/.tuxracer/options. */
        return False;
    }
}


bool_t is_joystick_continue_button_down()
{
    if ( !dc_input_state()->present ) {
        return False;
    }

    if ( getparam_joystick_continue_button() < 0 ) {
        return False;
    }

    return is_joystick_button_down( getparam_joystick_continue_button() );
}

/* EOF */
