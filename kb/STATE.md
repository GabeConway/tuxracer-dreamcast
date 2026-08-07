# STATE

What actually works, as of the last commit that touched this file. Not a
roadmap. If something is not listed as verified here, assume it is not.

**Updated:** 2026-08-06

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

- **The screen is black.** The loop runs at 59 fps and the draw-call census
  says the game submits ~405 immediate-mode primitives per frame
  (`end=24300` per second at mode 0 / splash), but a direct read of the
  displayed framebuffer finds 0 of 307,200 pixels non-black. Geometry goes
  in, nothing comes out. This is the one thing between here and M3.
- **Settings do not persist.** The config file is written under `/ram`,
  which is gone at power-off. A VMU save is a different shape of code — one
  fixed-name file written whole, not a directory — so it belongs in a save
  layer, not in `getpwuid`'s fake home. See `dc/src/dc_posix.c`.
- **`glAlphaFunc` is a GLdc no-op** and the game enables `GL_ALPHA_TEST` for
  trees and particles. Expect tree billboards to blend rather than punch
  through, and to sort wrong against terrain. Needs GLdc's punch-through
  list, not a shim fix. Not yet seen, because nothing has been seen.
- **`use_cva` / `cva_hack` are inert options.** GLdc has no compiled vertex
  arrays; `SDL_GL_GetProcAddress` returns NULL by design, so the game takes
  its own extension-absent path either way.

## Not yet measured

- Frame budget with a course rendering. The 59 fps above is a splash screen.
- Whether KOS's iso9660 driver returns the lowercase Rock Ridge names or the
  uppercase 8.3 ones (`kb/design-disc.md`). Course loading appeared to work,
  which is weak evidence for lowercase, but no one has read a course asset
  by path and confirmed it.
- Any optimisation level. `-O2` is the default because it is the sensible
  default, not because it was benchmarked.

## Milestones

| | Gate | State |
|---|---|---|
| **M0** | Harness green against a known-good ELF | **done** |
| **M1** | Every TU compiles for sh-elf | **done** |
| **M2** | Links, boots, reaches the render loop | **done** |
| **M3** | Something visible on screen | **blocked** — see "Known broken" |
| **M4** | A course loads and is playable | not started |
| **M5** | ≤ 33.3 ms/frame with a course rendering | not started |
