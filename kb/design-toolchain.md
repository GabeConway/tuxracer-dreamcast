# Toolchain and build — evidence

## 1. The SDK image

`tuxracer-dc:sdk` is built by `dc/build-dc-image.sh` from one of two recipes.

**Measured, 2026-08-06 on this host (colima, linux/arm64, 4 cores):**

| Recipe | Base | Wall clock |
|---|---|---|
| `dc/Dockerfile.overlay` | `opencrossing-dc:sdk` | **~90 s** |
| `dc/Dockerfile` | `debian:bookworm` | ~27 min (inherited figure, not re-measured here) |

The overlay exists because the expensive part — building `sh-elf-gcc` 15.2.0
and KallistiOS — was already paid for by the OpenCrossing-Dreamcast project on
this machine, and the only delta Tux Racer needs is two kos-ports.
`build-dc-image.sh` picks it automatically when the base image is present.
`TR_DC_FORCE_STANDALONE=1` proves the standalone path still works.

Verified contents (`docker run --rm --platform linux/arm64 tuxracer-dc:sdk
bash -c 'ls $KOS_PORTS/lib'`):

```
libGL.a  libGLU.a  libjim.a  libmodplug.a  libz.a
```

Headers: `$KOS_PORTS/include/{GL,jimtcl,modplug}`, KOS at
`/opt/toolchains/dc/kos/include`.

`libGLU.a` exports exactly five functions
(`grep -oE "GLAPI[^(]*glu[A-Za-z0-9]+" $KOS_PORTS/include/GL/glu.h`):

```
gluOrtho2D  gluPerspective  gluLookAt  gluBuild2DMipmaps  gluErrorString
```

The game calls ten. The other five are `gl_compat.c`'s problem —
see `kb/design-gl.md`.

## 2. `--platform linux/arm64` is not optional

Inherited finding from OpenCrossing-Dreamcast (`kb/design-toolchain.md` §2):
without the explicit platform, an accidental amd64 pull silently drops the
whole build into qemu. Slow and flaky. Every `docker run` and `docker build` in
this repo passes it.

This host has no BuildKit: `DOCKER_BUILDKIT=0` is required and `--progress` is
unsupported and hard-fails.

## 3. Translation-unit count

`make count` inside the container. Upstream `src/` holds 69 `.c` and 3 `.cpp`;
`SRC_EXCLUDE` drops `winsys.c` and `joystick.c`, so the game contributes
**67 `.c` + 3 `.cpp`**, plus whatever is in `dc/src/`. A different number means
a wildcard or an exclusion broke — check `make sources`.

## 4. Compiler flags, and why

From `dc/Makefile`:

- `-std=gnu89` for `src/`. Upstream is 2001 C written against gcc 2.95; the
  autoconf build used the compiler default of the day. gnu99 on this source
  produces a wall of diagnostics for no benefit.
- `-fno-strict-aliasing`. **Not a style preference.** `src/alglib.c` and
  `src/image.c` pun between float and integer representations through casts.
  GCC 15 at `-O2` is entitled to assume that never happens, and the failure
  mode is a silently wrong render, not a compile error.
- `-fno-omit-frame-pointer`. Costs a register. Without it KOS's crash handler
  prints "frame pointers not enabled!" and `harness/dc/crash.sh` produces an
  empty stack — a crash you cannot symbolise costs far more than the register.
- `-g`. The ELF is kept unstripped for `sh-elf-addr2line`; the CDI's
  `1ST_READ.BIN` is scrambled and stripped regardless, which is exactly why the
  provenance sidecar exists.

`-O2` is the default for both the game and `dc/src`. **Unverified**: no
optimisation level has been benchmarked on this port yet. `TR_OPT=-O0` is the
bisection knob.

## 5. CDI packaging

Inherited measurement (OpenCrossing-Dreamcast, same `mkdcdisc` commit
`3c2ef63a`):

```
mkdcdisc     -e elf -o out.cdi  ->  740,083,145 B, 15.6 s
mkdcdisc -N  -e elf -o out.cdi  ->    1,783,337 B,  0.021 s
```

`-N` by default. The padding is **appended after the filesystem**, so it does
not push content toward the outer tracks — that was measured false there, and
the correction is carried here so nobody re-derives it. `DC_CDI_PAD=1` is for
CD-R burns and read-speed-realistic timing runs only.

`-D /discroot`, never `-d`: `-d` includes the directory itself and everything
lands at `/cd/discroot/...`, which misses every open, because
`dc/include/config.h` sets `DATA_DIR "/cd"`.
