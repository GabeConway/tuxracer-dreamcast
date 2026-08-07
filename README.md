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
   SDL_mixer                   ->   AICA voices driven from the SH-4 + libmodplug
   Tcl 8.x                     ->   Jim Tcl
   X11 / Win32 filesystem      ->   KOS iso9660 at /cd
```

## Status

**It plays, with sound.** Splash, menus and race-select render correctly, a
course loads, and Tux races down it with terrain, trees, fog and the HUD, while
music streams and the effects fire.

What is still wrong, in the order it is felt:

- **Racing is 9–11 fps** (90–110 ms against a 33.3 ms budget). Menus are
  vsync-locked at 59.
- **Rainbow texture corruption** on the track marks, the fish and the course
  path. Trees and terrain are correct, so it is a format/conversion path used
  by some textures and not others. Not diagnosed.
- **Settings do not persist** — KOS's ramdisk has no `mkdir` and the VMU
  filesystem is flat, so `~/.tuxracer/options` cannot exist.
- **Course loading stalls in some builds**, decided purely by memory layout.
  `dc/Makefile` ships a labelled workaround so the default build plays.

[`kb/STATE.md`](kb/STATE.md) is the honest list and is updated in the same
commit as the change it describes. This section is a summary of it, not a
roadmap.

## Quick start

```bash
bash dc/build-dc-image.sh            # once. ~2 min, or ~27 min from scratch.
bash tools/fetch-data.sh             # 12 MB of courses/textures/sounds -> ./data
TR_DATA=./data bash dc/build-dc.sh   # -> dc/build/TuxRacer.cdi

harness/dc/install-flycast.sh        # once
harness/dc/smoke.sh                  # boot it, assert on what the guest prints
harness/dc/playtest.sh               # the real gate: does it reach a race?
```

The host needs nothing but Docker. The whole toolchain — `sh-elf-gcc` 15.2.0,
KallistiOS, GLdc, Jim Tcl, libmodplug, `mkdcdisc` — lives inside the
`tuxracer-dc:sdk` image, so there is nothing to install and nothing to break on
a macOS upgrade.

## Running a release build

Releases carry a ready-made `TuxRacer.cdi` (the disc image, game data
included), `tuxracer.elf` (the raw binary, for emulators that boot one) and
`SHA256SUMS`. Grab the newest from
[Releases](https://github.com/GabeConway/tuxracer-dreamcast/releases).

**In an emulator.** Open `TuxRacer.cdi` in [Flycast](https://flyca.st/) —
no BIOS ROM needed, its HLE BIOS boots the image. This is the configuration
every claim in this README was measured in.

**On real hardware.** Untested. Nobody has run this on a Dreamcast yet, so
treat it as unknown rather than broken. The release image is built unpadded
(`mkdcdisc -N`), which is right for emulators; for a CD-R, rebuild with
`DC_CDI_PAD=1 TR_DATA=./data bash dc/build-dc.sh` so the layout matches what a
real drive expects. If you do burn one, the fps and any hardware-only faults
are worth an issue — the harness cannot see them.

**Controls** (`dc/src/dc_winsys.c`, `dc/src/dc_joystick.c`):

| | Racing | Menus |
|---|---|---|
| Analogue stick | steer / lean | moves the cursor |
| D-pad | — | arrow keys |
| **A** | paddle (same as R) | click / confirm |
| **R trigger** | paddle | — |
| **L trigger** | brake | — |
| **X** | trick | — |
| **Y** | jump | — |
| **B** | quit the race (it is Escape) | back |
| **Start** | Enter | Enter |
| **L + Start** | pause | pause |

B is deliberately *not* brake: it maps to Escape, which is the in-race quit
key, so braking with it would abort the run.

Tagged builds are produced by CI: push `v*` and
[`.github/workflows/release-cdi.yml`](.github/workflows/release-cdi.yml)
builds the toolchain image, fetches and sha256-verifies the data, and attaches
the results to the GitHub release. Nothing is built on ordinary pushes.

## The development loop

```bash
DC_TARGET=objs bash dc/build-dc.sh          # does it compile?   <- fast gate
bash dc/build-dc.sh                         # does it link + package?
harness/dc/smoke.sh                         # does it boot and say so?
harness/dc/crash.sh dc/build/TuxRacer.cdi   # if it didn't, why not
```

`make objs` is the cheap signal: ~79 translation units, no link. Runtime
behaviour is not worth looking at before that is green.
[`CLAUDE.md`](CLAUDE.md) is the working agreement — hard rules, the `kb/`
convention, and the platform budget.

## Layout

| Path | What it is |
|---|---|
| `src/` | **Upstream 0.61, verbatim.** Committed unmodified first, so every later commit reads as a port diff. Do not edit it; add to `dc/` instead. |
| `dc/include/` | Shim headers, placed first on the include path: `config.h` (replaces autoconf), `tcl.h` (→ Jim), `SDL_mixer.h` (→ the mixer), `dc_aica.h`, `tr_glcompat.h`. |
| `dc/src/` | Dreamcast-only C: windowing, input, audio, GL gap-filling, harness. |
| `dc/Makefile` | The build. Plain GNU make, not CMake, not autoconf. |
| `harness/dc/` | Flycast test harness — boot an image, read the guest's serial output, turn markers into an exit code. Plus `audiotest/`, the standalone AICA probe. |
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

**The AICA is driven from the SH-4, not through KOS's ARM firmware.** KOS plays
sound by uploading an ARM7 program into sound RAM and posting commands to it.
That firmware enables its timer FIQ with `msr CPSR_c`, which Flycast's ARM7
ignores, so the FIQ never arrives, the firmware hangs on its first
`timer_wait()`, and *every* queued command — music and effects alike — goes to
a processor that will never read it. The ARM is only a proxy, so
[`dc/src/dc_aica.c`](dc/src/dc_aica.c) writes the channel registers over G2
directly, and the music stream is two voices looping over a ring in sound RAM
that a thread refills ahead of the play position. The whole investigation, with
the probe that isolated it, is in
[`kb/design-audio.md`](kb/design-audio.md).

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
and is not committed to this repository.
