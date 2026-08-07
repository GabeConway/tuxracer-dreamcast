# STATE

What actually works, as of the last commit that touched this file. Not a
roadmap. If something is not listed as verified here, assume it is not.

**Updated:** 2026-08-06

---

## Verified working

| Thing | Evidence |
|---|---|
| SDK image `tuxracer-dc:sdk` | `dc/build-dc-image.sh` via the overlay recipe, ~90 s. `$KOS_PORTS/lib` holds `libGL.a libGLU.a libjim.a libmodplug.a libz.a`. |
| Flycast harness, end to end | `harness/dc/selftest/build.sh` → `harness/dc/smoke.sh --elf selftest.elf` passes all nine checks (KOS banner, maple enumeration, `MARK:BOOT_OK`, `rc=0`, no failed asserts). Done **before** any game code existed, so a later failure is the game's, not the harness's. |
| Game data | `tools/fetch-data.sh` → 151 files, 12 MB, sha256 verified. Fits a CD with room to spare. |
| `uname()` gap | Probed all seven POSIX headers `src/tuxracer.h` needs; only `sys/utsname.h` was missing. `getcwd`/`chdir` link-tested and resolve. `dc/src/dc_posix.c` compiles clean. |

## Not yet verified

- **`make objs` has never been green.** The first attempt stopped at
  `config.h:52: fatal error: tcl.h: No such file or directory`, which is the
  expected state until the compatibility layers land. No upstream TU has been
  compiled for sh-elf yet, so the real size of the porting work in `src/` is
  still unmeasured.
- Nothing has been linked, so no ELF, no CDI, and no statement about RAM
  footprint is possible yet.
- No optimisation level has been benchmarked. `-O2` is the default because it
  is the sensible default, not because it was measured.

## Milestones

| | Gate | State |
|---|---|---|
| **M0** | Harness green against a known-good ELF | **done** |
| **M1** | `DC_TARGET=objs bash dc/build-dc.sh` compiles every TU | in progress |
| **M2** | Links, boots in Flycast, `MARK:BOOT_OK` from the real game | not started |
| **M3** | Title screen renders | not started |
| **M4** | A course loads and is playable | not started |
| **M5** | Frame budget: ≤ 33.3 ms/frame at 30 fps | not started |

## Known-hard, already identified

- **No stencil buffer** on PowerVR2. Upstream calls `glClearStencil`,
  `glStencilFunc`, `glStencilOp`. See `kb/design-gl.md` for what that costs
  visually.
- **`libGLU.a` exports five functions; the game calls ten.** The missing five
  include `gluSphere` and the quadric API.
- **Display lists.** Upstream uses `glNewList`/`glCallList` heavily; GLdc's
  support for them is the single largest unknown in the render path.
- **Impulse Tracker soundtrack.** `.it` modules need `libmodplug` fed through
  a KOS `snd_stream` on its own thread. `kb/design-audio.md` records whether
  that landed or whether music is currently a documented no-op.
