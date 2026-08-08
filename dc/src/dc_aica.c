/* dc_aica.c - AICA voices driven straight from the SH-4. See dc/include/dc_aica.h
 * for why this exists at all.
 *
 * Every register write here mirrors KallistiOS's ARM firmware
 * (kernel/arch/dreamcast/sound/arm/aica.c), with the addresses translated from
 * the ARM's view of the AICA (0x00800000) to the SH-4's (0xa0700000). Keeping
 * the sequence identical is deliberate: the firmware's order is the one that is
 * known to work on real hardware.
 */

#include <stdint.h>

#include <dc/g2bus.h>
#include <dc/spu.h>
#include <dc/sound/sound.h>
#include <kos/thread.h>

#include "dc_aica.h"

#define SNDREG(x)       (0xa0700000 + (x))
#define CHNREG(ch, x)   SNDREG(0x80 * (ch) + (x))

/* EVERY G2 access below is inside a g2_lock() block, and that is not
 * belt-and-braces: g2_read_*()/g2_write_*() are only atomic between g2_lock()
 * and g2_unlock(), which disable IRQs and suspend G2 DMA (dc/g2bus.h:148).
 * The G2 bus cannot do PIO and DMA at once, so an interrupt or a GD-ROM/AICA
 * DMA landing in the middle of one of these register writes wedges the bus.
 *
 * MEASURED: without the locks the game hung before its first frame as soon as
 * a second thread existed -- the music thread was enough, even doing nothing
 * but sleeping -- because preemption put an IRQ inside a G2 transaction. The
 * KOS calls this file replaces (snd_sh4_to_aica etc.) all lock; skipping it
 * was the bug, and it was invisible while the game was single-threaded.
 *
 * Blocks are kept short for the same reason -- IRQs are off inside them, so
 * the 64-voice sweeps lock per voice rather than once around the loop. */

static int aica_up = 0;

/* Linear 0..255 -> the AICA's 0..255 logarithmic attenuation, where 0 is
 * loudest. Copied verbatim from arm/aica.c, which generated it as
 * 16 * log2(255 / i). */
static const uint8_t vol_log[256] = {
    255, 127, 111, 102, 95, 90, 86, 82, 79, 77, 74, 72, 70, 68, 66, 65,
    63, 62, 61, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 50, 49, 48,
    47, 47, 46, 45, 45, 44, 43, 43, 42, 42, 41, 41, 40, 40, 39, 39,
    38, 38, 37, 37, 36, 36, 35, 35, 34, 34, 34, 33, 33, 33, 32, 32,
    31, 31, 31, 30, 30, 30, 29, 29, 29, 28, 28, 28, 27, 27, 27, 27,
    26, 26, 26, 25, 25, 25, 25, 24, 24, 24, 24, 23, 23, 23, 23, 22,
    22, 22, 22, 21, 21, 21, 21, 20, 20, 20, 20, 20, 19, 19, 19, 19,
    18, 18, 18, 18, 18, 17, 17, 17, 17, 17, 17, 16, 16, 16, 16, 16,
    15, 15, 15, 15, 15, 15, 14, 14, 14, 14, 14, 14, 13, 13, 13, 13,
    13, 13, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11, 11, 11, 11, 10,
    10, 10, 10, 10, 10, 10, 9, 9, 9, 9, 9, 9, 9, 8, 8, 8,
    8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6,
    6, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static uint8_t calc_vol(int vol) {
    if(vol < 0)
        vol = 0;
    if(vol > 255)
        vol = 255;
    return vol_log[vol];
}

/* 0 left .. 128 centre .. 255 right, in the AICA's split representation:
 * bit 4 selects the side, bits 0-3 the attenuation of the other one. */
static uint8_t calc_pan(int pan) {
    if(pan < 0)
        pan = 0;
    if(pan > 255)
        pan = 255;

    if(pan == 0x80)
        return 0;
    if(pan < 0x80)
        return (uint8_t)(0x10 | ((0x7f - pan) >> 3));
    return (uint8_t)((pan - 0x80) >> 3);
}

/* freq -> the AICA's FNS/OCT pair: freq = 44100 * 2^oct * (1 + fns/1024). */
static uint32_t calc_freq(uint32_t freq) {
    uint32_t freq_lo, freq_base = 5644800;
    int      freq_hi = 7;

    if(freq == 0)
        freq = 44100;

    while(freq < freq_base && freq_hi > -8) {
        freq_base >>= 1;
        --freq_hi;
    }

    freq_lo = (freq << 10) / freq_base;
    return ((uint32_t)(freq_hi << 11)) | (freq_lo & 1023);
}

void dc_aica_init(void) {
    int ch;

    if(aica_up)
        return;

    /* snd_init() uploads KOS's (unusable, see the header) ARM firmware and
     * starts the RAM allocator. We want the allocator, and snd_sfx_load()
     * requires it. Loading the firmware is harmless: it hangs in its own main
     * loop and touches no channel we have programmed. */
    snd_init();

    /* Master volume and mixer routing, as arm/aica.c:aica_init() sets them.
     * Without this every voice plays into a muted mixer. */
    {
        g2_ctx_t ctx = g2_lock();
        g2_write_32(SNDREG(0x2800), 0x000f);
        g2_unlock(ctx);
    }

    for(ch = 0; ch < DC_AICA_VOICES; ch++)
        dc_aica_stop(ch);

    aica_up = 1;
}

static void program(int ch, uint32_t aram, int fmt, uint32_t nsamp,
                    int loop, uint32_t freq, int vol, int pan) {
    g2_ctx_t ctx;

    if(ch < 0 || ch >= DC_AICA_VOICES)
        return;

    if(nsamp > DC_AICA_MAX_SAMPLES)
        nsamp = DC_AICA_MAX_SAMPLES;

    ctx = g2_lock();

    /* Key off first: the loop registers of a sounding voice must not change
     * underneath it. */
    g2_write_32(CHNREG(ch, 0), 0x8000);

    g2_write_32(CHNREG(ch, 8), 0);                    /* loop start        */
    g2_write_32(CHNREG(ch, 12), nsamp & 0xffff);      /* loop end / length */
    g2_write_32(CHNREG(ch, 24), calc_freq(freq));

    g2_write_8(CHNREG(ch, 36), calc_pan(pan));
    g2_write_8(CHNREG(ch, 37), 0xf);                  /* DISDL: full send  */
    g2_write_8(CHNREG(ch, 40), 0x24);                 /* LPF off           */
    g2_write_8(CHNREG(ch, 41), calc_vol(vol));
    g2_write_32(CHNREG(ch, 16), 0x1f);                /* no volume envelope*/

    g2_write_32(CHNREG(ch, 4), aram & 0xffff);
    g2_write_32(CHNREG(ch, 0),
                (((uint32_t)fmt) << 7) | ((aram >> 16) & 0x7f) |
                (loop ? 0x0200 : 0));

    g2_unlock(ctx);
}

void dc_aica_play_delayed(int ch, uint32_t aram, int fmt, uint32_t nsamp,
                          int loop, uint32_t freq, int vol, int pan) {
    program(ch, aram, fmt, nsamp, loop, freq, vol, pan);
}

void dc_aica_key_on(uint64_t chmask) {
    int ch;

    for(ch = 0; ch < DC_AICA_VOICES; ch++) {
        if(chmask & (1ULL << ch)) {
            g2_ctx_t ctx = g2_lock();
            g2_write_32(CHNREG(ch, 0), g2_read_32(CHNREG(ch, 0)) | 0xc000);
            g2_unlock(ctx);
        }
    }
}

void dc_aica_play(int ch, uint32_t aram, int fmt, uint32_t nsamp,
                  int loop, uint32_t freq, int vol, int pan) {
    program(ch, aram, fmt, nsamp, loop, freq, vol, pan);
    dc_aica_key_on(1ULL << ch);
}

void dc_aica_stop(int ch) {
    g2_ctx_t ctx;

    if(ch < 0 || ch >= DC_AICA_VOICES)
        return;

    /* KEYONEX without KEYONB: the AICA reads the key state on the KEYONEX
     * edge, so this is what actually releases the voice. */
    ctx = g2_lock();
    g2_write_32(CHNREG(ch, 0), (g2_read_32(CHNREG(ch, 0)) & ~0x4000) | 0x8000);
    g2_unlock(ctx);
}

void dc_aica_stop_all(void) {
    int ch;

    for(ch = 0; ch < DC_AICA_VOICES; ch++)
        dc_aica_stop(ch);
}

void dc_aica_set_vol(int ch, int vol) {
    g2_ctx_t ctx;

    if(ch < 0 || ch >= DC_AICA_VOICES)
        return;

    ctx = g2_lock();
    g2_write_8(CHNREG(ch, 41), calc_vol(vol));
    g2_unlock(ctx);
}

void dc_aica_set_pan(int ch, int pan) {
    g2_ctx_t ctx;

    if(ch < 0 || ch >= DC_AICA_VOICES)
        return;

    ctx = g2_lock();
    g2_write_8(CHNREG(ch, 36), calc_pan(pan));
    g2_unlock(ctx);
}

uint32_t dc_aica_pos(int ch) {
    g2_ctx_t ctx;
    uint32_t pos;
    int i;

    if(ch < 0 || ch >= DC_AICA_VOICES)
        return 0;

    /* One shared "observe" register for all 64 voices: select the channel in
     * the low byte of 0x280c, give the AICA a moment, then read 0x2814. The
     * delay is arm/aica.c:aica_get_pos()'s, kept because the register is not
     * latched instantly. */
    ctx = g2_lock();
    g2_write_8(SNDREG(0x280d), (uint8_t)ch);

    for(i = 0; i < 20; i++)
        __asm__ volatile ("nop");

    pos = g2_read_32(SNDREG(0x2814)) & 0xffff;
    g2_unlock(ctx);

    return pos;
}
