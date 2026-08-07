/* dc_aica.h - AICA voices driven straight from the SH-4.
 *
 * KallistiOS plays sound by uploading an ARM7 firmware into sound RAM and
 * talking to it through a command queue. That firmware does not run under
 * Flycast: its main loop ends in timer_wait(), which spins on a counter only
 * the timer FIQ advances, and the FIQ never arrives (harness/dc/audiotest,
 * kb/design-audio.md). Every Mix_* call the game made was therefore queued to
 * a processor that was never going to read the queue.
 *
 * The ARM is only a proxy. The AICA's channel registers sit in the G2 register
 * file at 0xa0700000 and the SH-4 can write them directly, which is what this
 * file does -- the same register sequence KOS's firmware would have executed
 * (kernel/arch/dreamcast/sound/arm/aica.c), minus the queue.
 *
 * MEASURED (harness/dc/audiotest): a voice programmed this way plays and its
 * position register advances, on the same Flycast build where the KOS driver
 * is silent.
 *
 * Sound RAM allocation still uses KOS (snd_mem_malloc / snd_sfx_load): those
 * are pure SH-4 code and never touch the ARM.
 */

#ifndef DC_AICA_H
#define DC_AICA_H

#include <stdint.h>

#define DC_AICA_VOICES      64

/* Sample formats, same values the hardware takes (dc/sound/aica_comm.h). */
#define DC_AICA_FMT_16BIT   0
#define DC_AICA_FMT_8BIT    1
#define DC_AICA_FMT_ADPCM   2

/* The AICA addresses samples with a 23-bit pointer and counts them with 16-bit
 * loop registers, so no single voice can see more than this many samples. */
#define DC_AICA_MAX_SAMPLES 65534

/* Brings up the SPU (KOS's snd_init(), for the RAM allocator), sets the master
 * volume, and silences all 64 voices. Safe to call more than once. */
void dc_aica_init(void);

/* Start a voice. `aram` is an offset into sound RAM as returned by
 * snd_mem_malloc(); `nsamp` is in samples, not bytes; `vol` is 0..255 linear
 * (converted to the AICA's logarithmic attenuation here) and `pan` is 0 left,
 * 128 centre, 255 right. A looping voice repeats [0, nsamp) forever. */
void dc_aica_play(int ch, uint32_t aram, int fmt, uint32_t nsamp,
                  int loop, uint32_t freq, int vol, int pan);

/* Start several voices in the same instant -- the two halves of a stereo
 * effect, or the two channels of the music ring, must not drift apart.
 * Program them with dc_aica_play_delayed(), then key them on together. */
void dc_aica_play_delayed(int ch, uint32_t aram, int fmt, uint32_t nsamp,
                          int loop, uint32_t freq, int vol, int pan);
void dc_aica_key_on(uint64_t chmask);

void dc_aica_stop(int ch);
void dc_aica_stop_all(void);
void dc_aica_set_vol(int ch, int vol);
void dc_aica_set_pan(int ch, int pan);

/* Current playback position of a voice, in samples from its start. This is the
 * only feedback the stream loop has, so it is read from the hardware register
 * rather than from any mirrored copy. */
uint32_t dc_aica_pos(int ch);

#endif /* DC_AICA_H */
