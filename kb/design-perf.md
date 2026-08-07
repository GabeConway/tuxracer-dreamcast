# Frame rate, and what the census says — evidence

## Measured

Guest-side, one `PERF` line per second from `dc/src/dc_winsys.c`, on Flycast.
`end` is `glEnd` calls (immediate-mode primitives), `drawelem` is
`glDrawElements`, `calllist` is `glCallList` — all per second.

| Screen | fps | frame | end/s | drawelem/s | calllist/s |
|---|---|---|---|---|---|
| Splash / menus (mode 0–3) | **59** | 16 ms | ~25,000 | 0 | 0 |
| Racing (mode 6) | **12–15** | 66–85 ms | ~2,300 | ~42 | ~476 |

Menus are vsync-locked; there is no headroom question there. Racing is the
problem: 66–85 ms against a 33.3 ms budget for 30 fps.

> **Flycast time is not silicon time.** Guest `timer_ms_gettime64()` is
> reproducible and fine as a relative gate, but Flycast models no cache, no bus
> contention and no store-queue stalls. These numbers rank changes against each
> other; they do not predict a real Dreamcast.

## What the census implies

At 13 fps the guest issues roughly **180 immediate-mode primitives, 37 display
list calls and 3 glDrawElements per FRAME**. Every `glEnd` in GLdc is a
`glDrawArrays` into `submitVertices`, which checks dirty state and may emit a
PVR polygon header (GLdc `GL/draw.c`). Submission cost, not fill rate, is the
thing to attack first — the PowerVR2 is fill-rate rich and submission poor.

The 476 display-list calls per second are Tux: `src/hier_util.c` draws one
sphere per scene node and Tux is built entirely from spheres.

## Levers, ranked, none of them applied yet

1. **Batch consecutive immediate-mode primitives in the shim.** The game emits
   one `glBegin`/`glEnd` per tree billboard and per particle. Coalescing runs
   that share GL state into one `glDrawArrays` is a pure `dc/src` change and
   attacks the dominant cost directly. Biggest expected win.
2. **`forward_clip_distance` (upstream 75) and `tree_detail_distance` (20).**
   Both are read per frame and only cull, so they are cheap to change and
   directly scale the tree count. **Attempted and reverted** — see below.
3. **`course_detail_level` (upstream 75).** Changes the terrain mesh built at
   load. Also attempted, also reverted.

## The attempt that was reverted, and what it cost

`dc/src/dc_defaults.c` applied Dreamcast-appropriate values through upstream's
own `setparam_*` API. Two things were learned and both are worth keeping:

- **The config-file route is impossible.** `src/game_config.c` reads
  `<home>/.tuxracer/options`, and KOS's ramdisk has **no `mkdir` at all**:
  `kernel/fs/fs_ramdisk.c:778` is a NULL slot and `:909` states only the root
  directory exists. The VMU filesystem is flat for the same reason. MEASURED —
  the first version printed `cannot write /ram/.tuxracer/options` every boot.
- **Calling `setparam_*` from `winsys_init()` hangs the boot**, reproducibly,
  part way through loading the sound effects. `winsys_init` is reached at
  `src/main.c:179`, *before* `tuxracer_init.tcl` is sourced, and these
  functions go through the Tcl interpreter. Moving the call to the first
  `winsys_swap_buffers` cleared that.

It was reverted anyway, because with it applied the run stopped completing
course loads — and then it turned out the same stall reproduces **without** it.
See the open problem below. The lever is real; it should go back in once that
is understood, not before.

## Open problem: course loading stalls, and it is EXTREMELY layout-sensitive

**This is the top open issue in the project.** Everything else here is
secondary to it.

**Symptom.** After the race-select screen the guest stops printing and never
reaches mode 5/6. Not a crash: no register dump, no assert, no fail marker —
silence until the deadline. Reproduced at 200 s, 320 s and 600 s.

**It is decided by memory layout, and by nothing else.** Four measurements,
all on the same source, all with the flag-stamp rebuild in place so no
mixed-flag binary is involved:

| Build | Result |
|---|---|
| plain | stalls at race-select, every time |
| `-DTR_FBDUMP_FRAME=380 -DTR_FBDUMP_FULL=1` | loads in ~40 s, races 54 s, repeatedly |
| `-DTR_FBDUMP_FRAME=999999 -DTR_FBDUMP_FULL=1` (**never fires**) | races, same as above |
| default + `-DTR_AUTOEXIT=1500` | stalls again |

The third row is the important one. The dump never runs, so the ~5 s pause it
costs is not the mechanism. What remains is ~1.5 KB of statics kept alive and a
handful of instructions in `winsys_swap_buffers`. The fourth row shows that
adding a *different* small amount of code puts it back.

**A build difference that changes only layout deciding whether a course load
completes is a latent memory bug.** 16 MB is tight — 2.33 MB of `.bss` before
the heap even starts — and course loading is peak allocation: the terrain mesh,
the `.rgb` decode buffers and `tr_gluBuild2DMipmaps`'s downscale scratch are all
live at once.

**What did NOT work as a probe:** a `volatile unsigned char pad[2048]` in its
own TU. The ELF came out byte-identical in size because `$KOS_LDFLAGS` carries
`-Wl,--gc-sections` and `-fdata-sections` puts it in its own section, so the
linker dropped it. Any future padding probe must be referenced from live code.
`-Wl,--no-gc-sections` does not link at all (tried; `ld` fails).

**The workaround that is currently shipping.** `dc/Makefile` defines
`TR_FBDUMP_FRAME ?= 999999`, which keeps the dump code alive and never fires
it, because a game that plays beats a game that stalls. It is labelled as a
workaround at the definition site. `TR_FBDUMP_FRAME=0` removes it and
reproduces the stall.

**Next step.** Instrument the KOS heap around `load_course` — `mallinfo()` /
`malloc_stats()` before, during and after — and print the high-water mark. If
it is near the ceiling, free the `.rgb` decode buffer before the downscale
rather than after, and shrink `.bss`. If it is not, the next suspect is a
fixed-size buffer in the course loader or in `tr_gluBuild2DMipmaps`'s scratch.

**`harness/dc/playtest.sh` is blocked on this.** It builds with `TR_AUTOKEY`
and `TR_AUTOEXIT` so the run can end on a marker instead of a timeout, and
asserts `MARK:RACING` — but the extra `TR_AUTOEXIT` code is itself enough to
trigger the stall, so the gate currently fails on a build that is otherwise
fine. Fix the memory bug and the gate starts working; do not "fix" the gate.

## Not yet measured

- Any optimisation level other than `-O2`. `TR_OPT=-O0` is the bisection knob.
- Whether the SH-4's `-mfsrra`/`-mfsca` math paths are being used by the
  physics loops at all.
- Real hardware, at all.
