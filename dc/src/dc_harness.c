/*
 * dc_harness.c — emits the harness protocol's bracketing lines without
 * touching upstream's main().
 *
 * src/main.c is vendored verbatim, so there is nowhere to put "print BEGIN
 * before anything else". A constructor is the way in: KOS runs .ctors before
 * main(), which is early enough to raise the serial baud and print BEGIN, and
 * costs no upstream diff at all.
 *
 * Compiled into every build; inert unless TR_HARNESS is defined.
 */

#include "tr_harness.h"

#ifdef TR_HARNESS

#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <dc/scif.h>

/*
 * 50 MHz / 32 = 1562500 is the SH-4 SCIF maximum, and Flycast models SCIF baud
 * faithfully: at KOS's default 57600 the guest gets ~5.8 KB/s and ~190 bytes of
 * logging eats an entire 30 fps frame budget, which would corrupt every timing
 * measurement the harness takes. At 1562500 it is ~150 KB/s.
 *
 * This is EMULATOR-ONLY. A real coder's cable will not sync at 1.5 Mbps, which
 * is exactly why it lives behind TR_HARNESS and not in the shipping build.
 */
#define TR_HARNESS_BAUD 1562500

static int tr_ended = 0;

/*
 * HARD RULE (verified on KOS 2.3.0 + Flycast v2.6): never call scif_flush().
 * It clears TEND and spins waiting for it to come back; Flycast never re-raises
 * TEND on an already-idle TX FIFO, so the spin times out and KOS latches
 * serial_enabled = 0 — permanently killing all further serial output, INCLUDING
 * the crash dump. A crash after a flush is invisible and shows up only as a
 * wall-clock timeout. printf() already flushes; the call is never needed.
 */

void tr_harness_end(int rc) {
    if(tr_ended) return;   /* the host kills us on the first END line anyway */
    tr_ended = 1;
    printf("TR-DC-HARNESS-END rc=%d\n", rc);
}

/* Runs on the normal exit(3) path so a game that quits cleanly still closes
 * the bracket. Abnormal exits are covered by the runner's timeout and by the
 * KOS crash dump, both of which the host can tell apart from a clean end. */
static void tr_harness_atexit(void) {
    tr_harness_end(0);
}

__attribute__((constructor))
static void tr_harness_init(void) {
    scif_set_parameters(TR_HARNESS_BAUD, 1);
    scif_init();
    dbgio_dev_select("scif");   /* so nothing else can steal the channel */

    printf("TR-DC-HARNESS-BEGIN\n");
    printf("MARK:BOOT_OK\n");

    atexit(tr_harness_atexit);
}

#endif /* TR_HARNESS */
