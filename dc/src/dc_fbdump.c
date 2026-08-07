/*
 * dc_fbdump.c — ship a real picture of the guest framebuffer to the host.
 *
 * WHY THIS EXISTS. Flycast has no headless mode and host-side capture is
 * blocked on this machine: `screencapture` needs a Screen Recording TCC grant
 * and `osascript`/System Events hangs on the Accessibility prompt, so
 * Flycast's own F12 screenshot is unreachable unattended
 * (harness/dc/README.md). The harness's existing FBHASH/FBTHUMB answers "did
 * this frame change?" but a 16x12 thumbnail cannot answer "does the title
 * screen look right?", which is the question that actually blocks a render
 * bring-up.
 *
 * So the guest sends the framebuffer out over the serial console and the host
 * turns it back into a PNG (tools/fbdump-to-png.py).
 *
 * COST, and why the defaults are what they are. The console runs at 1.5 Mbps
 * (~150 KB/s, dc/src/dc_harness.c). A full 640x480 RGB565 frame is 614,400 B,
 * which base64 expands to ~819 KB and ~5.5 s of wall clock. Halving each axis
 * gives 320x240 = 153,600 B, ~205 KB of base64, ~1.4 s — enough resolution to
 * see geometry, texturing, fog and UI layout, at a quarter of the cost. That
 * is the default; TR_FBDUMP_FULL=1 sends the full frame when a detail matters.
 *
 * This is a diagnostic, not part of the game. It is compiled only under
 * TR_HARNESS, and it only fires on the frame it is asked for.
 */

#include "tr_harness.h"

#ifdef TR_HARNESS

#include <kos.h>
#include <stdio.h>
#include <stdint.h>

#include <dc/video.h>
#include <dc/pvr.h>

#include "dc_fbdump.h"

#ifndef TR_FBDUMP_FULL
#define TR_FBDUMP_FULL 0
#endif

#define FB_W 640
#define FB_H 480

#if TR_FBDUMP_FULL
#define STEP 1
#else
#define STEP 2
#endif

#define OUT_W (FB_W / STEP)
#define OUT_H (FB_H / STEP)

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * One row per line. A row is OUT_W pixels = 2*OUT_W bytes, which base64s to a
 * bounded string we can build on the stack — no allocation, and no single
 * multi-hundred-KB write for the runner to buffer.
 *
 * Line-per-row also means a truncated dump (timeout, killed run) still decodes
 * to a partial image instead of nothing.
 */
static void emit_row(const uint16_t *row) {
    static char out[((OUT_W * 2 + 2) / 3) * 4 + 16];
    const uint8_t *p = (const uint8_t *)row;
    int n = OUT_W * 2;
    int i = 0, o = 0;

    while(i + 2 < n) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = b64tab[v & 63];
        i += 3;
    }
    if(i < n) {
        uint32_t v = (uint32_t)p[i] << 16;
        int rem = n - i;
        if(rem == 2) v |= (uint32_t)p[i + 1] << 8;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = (rem == 2) ? b64tab[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    printf("FBROW %s\n", out);
}

/*
 * How many of the first 640x480 pixels at `base` are non-black, and the
 * brightest value seen. One line of output per candidate buffer, which is the
 * cheapest way to answer "is the picture somewhere else in VRAM?" without
 * shipping several hundred KB per guess.
 */
static void probe(const char *what, const uint16_t *base) {
    unsigned nonzero = 0, maxv = 0;
    int i;

    for(i = 0; i < FB_W * FB_H; i++) {
        uint16_t v = base[i];
        if(v) nonzero++;
        if(v > maxv) maxv = v;
    }
    printf("FBPROBE %-10s ptr=%08lx nonzero=%u/%u max=%04x\n",
           what, (unsigned long)(uintptr_t)base, nonzero,
           (unsigned)(FB_W * FB_H), maxv);
}

void tr_fbdump(void) {
    static uint16_t row[OUT_W];
    const uint16_t *fb;
    uint32_t fb_off;
    int x, y;

    /*
     * NOT vram_s. That is where vid_set_mode() left the CPU's view, and a
     * first attempt at this dump read it and got 240 rows of pure black while
     * the emulator window was visibly showing something. GLdc does not render
     * through vram_s: the PVR renders into its own buffer and flips by
     * rewriting the display start address, so the frame the user is looking at
     * is wherever PVR_FB_ADDR currently points.
     *
     * Reading the register is also the only form that stays correct across the
     * flip — hardcoding either buffer would give whichever one is not on
     * screen half the time.
     */
    fb_off = PVR_GET(PVR_FB_ADDR) & 0x00fffffc;
    fb = (const uint16_t *)(0xa5000000 + fb_off);

    probe("vram_s",  vram_s);
    probe("fb_addr", fb);
    probe("il_addr", (const uint16_t *)(0xa5000000 +
                                        (PVR_GET(PVR_FB_IL_ADDR) & 0x00fffffc)));

    printf("FBDUMP begin %dx%d rgb565\n", OUT_W, OUT_H);
    printf("FBDUMP addr %08lx\n", (unsigned long)fb_off);

    for(y = 0; y < OUT_H; y++) {
        const uint16_t *src = &fb[(y * STEP) * FB_W];
        for(x = 0; x < OUT_W; x++) {
            row[x] = src[x * STEP];
        }
        emit_row(row);
    }

    printf("FBDUMP end\n");
}

#endif /* TR_HARNESS */
