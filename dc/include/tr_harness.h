/*
 * tr_harness.h — guest side of the Flycast test harness protocol.
 *
 * The host runner (harness/dc/_runner.py) has no way to know what the guest is
 * doing except what the guest prints over the emulated SCIF serial port. This
 * header is how game code says it.
 *
 * Protocol, in full, is harness/dc/README.md §"Guest-side protocol". The
 * bracketing BEGIN/END lines and MARK:BOOT_OK are emitted automatically by
 * dc/src/dc_harness.c — game code only needs the record macros below.
 *
 * Everything here compiles to nothing when TR_HARNESS is not defined, so the
 * calls can stay in the source permanently. dc/build-dc.sh defines it by
 * default; a shipping build would not.
 */

#ifndef TR_HARNESS_H
#define TR_HARNESS_H

#ifdef TR_HARNESS

#include <stdio.h>

/* A named point in the run. The host matches these with --expect. */
#define TR_MARK(name)          printf("MARK:%s\n", (name))
#define TR_MARKF(fmt, ...)     printf("MARK:" fmt "\n", __VA_ARGS__)

/* A pass/fail check. Any `ASSERT fail` fails harness/dc/smoke.sh. */
#define TR_ASSERT(name, ok)    printf("ASSERT %s %s\n", (ok) ? "ok" : "fail", (name))

/* Numeric measurements, gated against a baseline by harness/dc/perf.sh.
 * NEVER call this inside a timed window: serial output costs real emulated
 * time, and at the default baud a single line can eat a whole frame. */
#define TR_PERF(fmt, ...)      printf("PERF " fmt "\n", __VA_ARGS__)

/* Ends the run. The host kills Flycast the instant it sees this, so do not
 * print it until everything that matters has already been printed. */
void tr_harness_end(int rc);

#else /* !TR_HARNESS */

#define TR_MARK(name)          ((void)0)
#define TR_MARKF(fmt, ...)     ((void)0)
#define TR_ASSERT(name, ok)    ((void)0)
#define TR_PERF(fmt, ...)      ((void)0)
#define tr_harness_end(rc)     ((void)0)

#endif /* TR_HARNESS */

#endif /* TR_HARNESS_H */
