# Tux Racer for the Sega Dreamcast

A native Dreamcast port of [Tux Racer](https://sourceforge.net/projects/tuxracer/)
0.61 — the 2001 original, not a fork — running on **KallistiOS** and **GLdc**,
with SDL removed entirely.

The two are almost exactly contemporary. Tux Racer shipped in January 2001; the
Dreamcast shipped in Europe fourteen months earlier. The game was written for
the hardware of its own moment, which is why this is a port and not a rewrite:
the 200 MHz SH-4 with its vector FPU, the 8 MB of VRAM and the 16 MB of main
RAM are close enough to a 2001 desktop that the interesting work is at the API
boundary, not in the game.

```
   upstream Tux Racer 0.61          this port
   ───────────────────────          ─────────
   desktop OpenGL 1.2 + GLU    ->   GLdc on the PowerVR2  (+ a compat shim)
   SDL 1.2 video / events      ->   KallistiOS maple + PVR
   SDL_mixer                   ->   KOS snd_sfx + libmodplug
   Tcl 8.x                     ->   Jim Tcl
   X11 / Win32 filesystem      ->   KOS iso9660 at /cd
```

## Status

> **Bring-up.** The build system, the SDK image and the Flycast test harness
> are in place. The compatibility layers are landing now. See
> [`kb/STATE.md`](kb/STATE.md) for what actually runs today — that file is kept
> honest, this section is not a roadmap.

## Quick start

```bash
bash dc/build-dc-image.sh            # once. ~2 min, or ~27 min from scratch.
bash tools/fetch-data.sh             # 12 MB of courses/textures/sounds -> ./data
TR_DATA=./data bash dc/build-dc.sh   # -> dc/build/TuxRacer.cdi

harness/dc/install-flycast.sh        # once
harness/dc/smoke.sh                  # boot it, assert on what the guest prints
```

The host needs nothing but Docker. The whole toolchain — `sh-elf-gcc` 15.2.0,
KallistiOS, GLdc, Jim Tcl, libmodplug, `mkdcdisc` — lives inside the
`tuxracer-dc:sdk` image, so there is nothing to install and nothing to break on
a macOS upgrade.

## Layout

| Path | What it is |
|---|---|
| `src/` | **Upstream 0.61, verbatim.** Committed unmodified first, so every later commit reads as a port diff. Do not edit it; add to `dc/` instead. |
| `dc/include/` | Shim headers, placed first on the include path: `config.h` (replaces autoconf), `tcl.h` (→ Jim), `SDL_mixer.h` (→ KOS audio), `tr_glcompat.h`. |
| `dc/src/` | Dreamcast-only C: windowing, input, audio, GL gap-filling, harness. |
| `dc/Makefile` | The build. Plain GNU make, not CMake, not autoconf. |
| `harness/dc/` | Flycast test harness — boot an image, read the guest's serial output, turn markers into an exit code. |
| `kb/` | Design notes. Evidence, not tutorials: every claim cites the command or `file:line` behind it. |
| `tools/` | `fetch-data.sh` and friends. |

## Why these choices

**No SDL.** Only two upstream files are real SDL consumers — `winsys.c`
(video + events) and `joystick.c` — and both are pure platform glue with no
game logic worth keeping. Replacing them is less work than making SDL 1.2's
Dreamcast video driver cooperate with GLdc, and it leaves the other 70
translation units untouched.

**Jim Tcl, not a mini-interpreter.** Tux Racer's data files are not config
files that happen to look like Tcl — they are Tcl, with `proc`, `regexp`,
`catch`, `open`/`gets` and `source`. Reimplementing that badly is a long tail
of mystery bugs in course loading. Jim is a real Tcl in about a tenth of the
size, it is already a kos-port, and the game only touches ~35 `Tcl_*` entry
points, which is a small adapter. See [`kb/design-tcl.md`](kb/design-tcl.md).

**The harness is not optional.** Flycast has no GDB stub, no headless mode and
never exits on its own, so "run it and look" does not scale and cannot gate a
commit. Instead the guest prints typed one-line records over the emulated SCIF
port and the host turns them into an exit code — including symbolising a KOS
crash dump back to `file:line`. It was lifted from
[OpenCrossing-Dreamcast](https://github.com/GabeConway/OpenCrossing-Dreamcast)
and rebranded; the operating manual is
[`harness/dc/README.md`](harness/dc/README.md).

## Licence

Tux Racer is GPL-2.0 (see [`LICENSE`](LICENSE)); everything added here is under
the same terms. The game data package is fetched from upstream at build time
and is not redistributed in this repository.
