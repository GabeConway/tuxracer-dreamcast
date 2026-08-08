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
| **Audio plays** | Music streams and effects fire through a full race. The AICA is driven from the SH-4 (`dc/src/dc_aica.c`); KOS's ARM firmware never runs under Flycast — `kb/design-audio.md`. Seven effects in AICA RAM: 357,536 B used, 1,510,240 B free. |
| **The render loop runs** | **59 fps, vsync-locked**, sustained over 33 s. Zero GL errors. |
| Framebuffer capture | `tools/fbdump.sh` produces a real PNG of the guest screen. |

## Known broken

- **A latent memory bug whose symptom is decided purely by memory layout, and
  it has already flipped polarity once.** Before the audio rewrite, a build
  without the framebuffer-dump code stalled after race-select; with it (set to
  a frame that never arrives, so it never fires) the same source raced. After
  the audio rewrite the reverse is true: with the dump code the game hangs
  before its first frame — that is what shipped as the broken v0.1.0 release —
  and with `TR_FBDUMP_FRAME=0` it boots, races and exits clean. The default in
  `dc/Makefile` is now 0. **Top open issue**; nothing about it is fixed, only
  re-masked. Full evidence in `kb/design-perf.md`.
- **The shipping configuration is the one that must be tested.** v0.1.0 was
  tagged on the strength of harness builds (`-DTR_AUTOKEY`, `-DTR_AUDIO_TRACE`),
  and every one of those extra defines moves the heap. The release CDI, built
  with none of them, hung at a black screen. Gate a release on a build with no
  extra defines.
- **Racing runs at 9–11 fps** with music playing (was 12–15 before audio
  worked; libmodplug decodes 22050 Hz stereo on the SH-4 alongside the game).
  Menus are vsync-locked at 59. Levers are ranked in `kb/design-perf.md`; none
  applied. The audio-specific levers are in `kb/design-audio.md` §3.
- **Settings do not persist.** KOS's ramdisk has no `mkdir`
  (`fs_ramdisk.c:778`) and the VMU filesystem is flat, so
  `<home>/.tuxracer/options` cannot exist. Needs a VMU save layer.
- **Rainbow texture corruption** on the track marks behind Tux, on the fish,
  and on the course path. Reported from a real playthrough 2026-08-07, so it is
  wider than the "track marks only" this file used to claim. Not diagnosed;
  trees and terrain are correct, which points at a format/conversion path used
  by some textures and not others.
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
