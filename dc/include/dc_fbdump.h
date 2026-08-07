/*
 * dc_fbdump.h — guest framebuffer dump over the serial console.
 *
 * See dc/src/dc_fbdump.c for why this exists and what it costs, and
 * tools/fbdump-to-png.py for the host side.
 */

#ifndef TR_DC_FBDUMP_H
#define TR_DC_FBDUMP_H

#ifdef TR_HARNESS
void tr_fbdump(void);
#else
#define tr_fbdump() ((void)0)
#endif

#endif /* TR_DC_FBDUMP_H */
