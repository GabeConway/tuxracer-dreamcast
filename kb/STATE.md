# STATE

What actually works, as of the last commit that touched this file. Not a
roadmap. If something is not listed as verified here, assume it is not.

**Updated:** 2026-08-07

---

## Verified working

| Thing | Evidence |
|---|---|
| SDK image `tuxracer-dc:sdk` | overlay recipe, ~90 s. `$KOS_PORTS/lib`: `libGL.a libGLU.a libjim.a libmodplug.a libz.a`, Jim patched. |
| Flycast harness | `selftest.elf` → `smoke.sh` passes all nine checks. Done before any game code existed, so a later failure is the game's. |
| Game data | 151 files, 12 MB upstream → 10 MB staged. sha256 verified. |
| **Every TU compiles** | `make count` → 67 game `.c` + 3 `.cpp` + 8 `dc/src` = **78 TUs, 0 errors**. |
| **It links** | `tuxracer.elf`: **1,011 KB text, 17 KB data, 2,332 KB bss**. Comfortable inside 16 MB. |
| **It boots** | KOS banner, maple enumerates the controller, `vid_set_mode: 640x480IL NTSC`, GLdc reports `1.1-698-ga1cd`. |
| **Tcl init completes** | All five courses, the event/cup tree, every texture and sound declaration. The Jim list-parser patch was required for this (`kb/traps.md`). |
| **Audio loads** | All seven effects into AICA RAM: 357,536 B used, 1,510,240 B free. |
| **The render loop runs** | **59 fps, vsync-locked**, sustained over 33 s. Zero GL errors. |
| Framebuffer capture | `tools/fbdump.sh` produces a real PNG of the guest screen. |

## Known broken

- **Course loading stalls, and it is decided purely by memory layout.**
  Four builds of identical source: plain stalls; with the framebuffer-dump
  code present it races (even with the dump set to a frame number that never
  arrives); adding `TR_AUTOEXIT` instead makes it stall again. That is a
  latent memory bug, not a rendering one. `dc/Makefile` currently ships a
  labelled workaround (`TR_FBDUMP_FRAME ?= 999999`) so the default build
  plays. **Top open issue** — full evidence and the next step in
  `kb/design-perf.md`.
- **Racing runs at 12–15 fps**, 66–85 ms against a 33.3 ms budget. Menus are
  vsync-locked at 59. Levers are ranked in `kb/design-perf.md`; none applied.
- **Settings do not persist.** KOS's ramdisk has no `mkdir`
  (`fs_ramdisk.c:778`) and the VMU filesystem is flat, so
  `<home>/.tuxracer/options` cannot exist. Needs a VMU save layer.
- **Track marks render as a rainbow smear** on the snow behind Tux. Not
  diagnosed. Trees and terrain are correct.
- **Texgen emulation degrades tree billboards.** With `-DTR_TEXGEN_DISABLE`
  the trees render clean green and the terrain loses its texture, which is the
  trade the switch exists to expose. Not resolved.
- Textures are downscaled to 128 px inside `tr_gluBuild2DMipmaps` to fit
  8 MB of VRAM (`kb/design-perf.md`, the commit that added it). Visible on
  close-up UI art. The *layout* is unaffected — that was the point.

## Milestones

| | Gate | State |
|---|---|---|
| **M0** | Harness green against a known-good ELF | **done** |
| **M1** | Every TU compiles for sh-elf | **done** |
| **M2** | Links, boots, reaches the render loop | **done** |
| **M3** | Something visible on screen | **done** — splash, menus, race-select all render correctly |
| **M4** | A course loads and is playable | **done** with a labelled workaround — 52 s of continuous racing, repeatedly; see the stall above |
| **M5** | ≤ 33.3 ms/frame with a course rendering | not started — currently 66–85 ms |
