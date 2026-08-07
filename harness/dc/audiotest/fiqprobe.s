@ fiqprobe -- a 60-instruction AICA ARM7 program that answers one question:
@ does Flycast deliver the AICA timer-A interrupt to the ARM core as an FIQ?
@
@ KOS's own firmware cannot answer it. arm_main() ends every iteration in
@ timer_wait(), which spins on a counter that only the FIQ handler advances
@ (sound/arm/main.c:224, sound/arm/crt0.s), so "no FIQ" and "ARM not running"
@ look identical from the SH-4 side. This program is the control: it sets up
@ timer A exactly the way aica_init() does, enables FIQ with the same three
@ instructions, and then bumps a plain counter in its main loop -- so the two
@ failure modes become two different numbers.
@
@ SH-4 reads (via g2_read_32 at SPU_RAM_UNCACHED_BASE + off):
@   0x21004  main-loop counter  -- moves => the ARM is executing
@   0x21008  FIQ counter        -- moves => Flycast delivers the FIQ
@   0x2100c  INTREQ as the handler saw it (crt0.s expects 2 == timer)
@
@ Assembled on the host with clang -target armv4t-none-eabi; there is no ARM
@ compiler in the SDK image (only KOS's prebuilt stream.drv).

    .arch armv4t
    .text
    .globl _start

_start:
    b       start           @ 0x00 reset
    b       hang            @ 0x04 undef
    b       hang            @ 0x08 swi
    b       hang            @ 0x0c prefetch abort
    b       hang            @ 0x10 data abort
    b       hang            @ 0x14 reserved
    b       hang            @ 0x18 irq
                            @ 0x1c fiq -- the handler runs in place, as in crt0.s
fiq:
    ldr     r8, =0x00021008
    ldr     r9, [r8]
    add     r9, r9, #1
    str     r9, [r8]

    ldr     r8, =0x00802d00     @ INTREQ: interrupt type, 3 bits
    ldr     r9, [r8]
    and     r9, r9, #7
    ldr     r10, =0x0002100c
    str     r9, [r10]

    ldr     r8, =0x00802d04     @ INTCLR, written four times as KOS does
    mov     r9, #1
    str     r9, [r8]
    str     r9, [r8]
    str     r9, [r8]
    str     r9, [r8]

    subs    pc, r14, #4

hang:
    b       hang

start:
    mov     sp, #0xb000

    ldr     r0, =0x00021004
    mov     r1, #0
    str     r1, [r0]
    ldr     r0, =0x00021008
    str     r1, [r0]
    ldr     r0, =0x0002100c
    str     r1, [r0]

    @ Same sequence as aica_init() (sound/arm/aica.c:17): mask, acknowledge,
    @ route the AICA event onto FIQ level 2, arm timer A, unmask timer A.
    ldr     r0, =0x0080289c     @ SCIEB
    mov     r1, #0
    str     r1, [r0]
    ldr     r0, =0x008028a4     @ SCIRE
    ldr     r1, =0x7ff
    str     r1, [r0]
    ldr     r0, =0x008028a8     @ SCILV0
    mov     r1, #0x18
    str     r1, [r0]
    ldr     r0, =0x008028ac     @ SCILV1
    mov     r1, #0x50
    str     r1, [r0]
    ldr     r0, =0x008028b0     @ SCILV2
    mov     r1, #0x08
    str     r1, [r0]
    ldr     r0, =0x00802890     @ TIMA: 256 - 10, i.e. ~4410 Hz
    mov     r1, #246
    str     r1, [r0]
    ldr     r0, =0x0080289c     @ SCIEB: bit 6, timer A
    mov     r1, #0x40
    str     r1, [r0]

    @ CPSR: I set (IRQ off), F cleared (FIQ on) -- crt0.s arm_fiq_enable.
    mov     r2, #0x13           @ SVC mode, I and F both clear
    msr     cpsr, r2

    @ Save CPSR back so the SH-4 can see whether the F bit (0x40) actually
    @ cleared. If it did not, the emulator is ignoring the msr and no FIQ can
    @ ever be taken -- a different bug from "the FIQ is never raised".
    mrs     r3, cpsr
    ldr     r0, =0x00021010
    str     r3, [r0]

loop:
    ldr     r0, =0x00021004
    ldr     r1, [r0]
    add     r1, r1, #1
    str     r1, [r0]
    b       loop

    .ltorg
