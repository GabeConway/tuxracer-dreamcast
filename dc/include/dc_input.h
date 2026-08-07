/*
 * dc/include/dc_input.h -- shared Dreamcast controller snapshot.
 *
 * dc_winsys.c owns the poll (it drives the frame loop and must diff pad state
 * against the previous frame to synthesise key events).  dc_joystick.c is a
 * pure reader of the same snapshot, so the analog axes the physics code reads
 * are guaranteed to come from the exact same maple sample as the key events
 * dispatched that frame.  Two independent polls would let the two paths
 * disagree inside a single frame.
 */
#ifndef DC_INPUT_H
#define DC_INPUT_H 1

#include <stdint.h>

/* The Dreamcast framebuffer this port uses.  Fixed; there is no resize path. */
#define DC_SCREEN_WIDTH     640
#define DC_SCREEN_HEIGHT    480

/* Analog trigger reading (0-255) at or above which the trigger counts as a
   digital press.  ~25% pull; loose enough not to need a firm squeeze. */
#define DC_TRIG_THRESHOLD   64

/* Stick deflection (of 128) inside which the stick is treated as centred.
   The retail DC stick rests within about +/-10 counts; 24 leaves headroom for
   a worn pad without eating a noticeable amount of usable travel. */
#define DC_STICK_DEADZONE   24

typedef struct {
    int      present;   /* 0 when no controller is plugged in; all fields 0 */
    uint32_t buttons;   /* CONT_* bitmask from dc/maple/controller.h */
    int      joyx;      /* -128..127, 0 centred, positive = right */
    int      joyy;      /* -128..127, 0 centred, positive = DOWN (see note) */
    int      ltrig;     /* 0..255 */
    int      rtrig;     /* 0..255 */
} dc_pad_t;

/* joyy polarity note: KOS computes cooked joyy as (raw - 128)
   (kernel/arch/dreamcast/hardware/maple/controller.c:179) and the raw DC
   Y axis reads 0 at full-up / 255 at full-down, so positive == down.  That
   matches the SDL axis convention src/racing.c:194-203 was written against
   (joy_y > 0.5 => brake, joy_y < -0.5 => paddle), so no sign flip is needed. */

/* Sample the first attached controller.  Cheap; safe to call when no pad is
   present.  Called once per frame from winsys_process_events(). */
void dc_input_poll( void );

/* Most recent snapshot.  Never NULL. */
const dc_pad_t *dc_input_state( void );

#endif /* DC_INPUT_H */

/* EOF */
