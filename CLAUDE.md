# tuxracer-dreamcast — working agreement

Native Sega Dreamcast port of Tux Racer 0.61 on KallistiOS + GLdc. Read
[`kb/STATE.md`](kb/STATE.md) first, every session: it is the only file that
claims to describe what currently works.

## 1. Hard rules

1. **`src/` is upstream, verbatim.** It was committed unmodified in the first
   commit so that every later commit reads as a port diff. Fix things in
   `dc/include/` (a shim header that shadows the system one) or `dc/src/` (a
   replacement TU excluded from the build in `dc/Makefile`'s `SRC_EXCLUDE`).
   If a `src/` edit is genuinely unavoidable, it needs its own commit, a
   comment at the edit site saying why, and a line in `kb/traps.md`.
2. **Never commit a disc image, an ELF, or the data package.** `dc/build/` and
   `data/` are gitignored. The data is fetched and sha256-verified by
   `tools/fetch-data.sh`.
3. **Never run CMake or autoconf.** The Dreamcast build is `dc/Makefile`,
   driven by `dc/build-dc.sh`. Upstream's `configure` is dead weight here;
   `dc/include/config.h` is the hand-written answer to every `HAVE_*`.
4. **`--platform linux/arm64` on every `docker run`/`docker build`.** Without
   it an accidental amd64 pull drops the whole build into qemu: slow and flaky.
   This host also has no BuildKit — `DOCKER_BUILDKIT=0`, and never pass
   `--progress`.
5. **Never call `scif_flush()` from guest code.** It permanently kills serial
   output on Flycast, including the crash dump. `printf()` already flushes.
   See `harness/dc/README.md`.
6. **Absolute paths in scripts.** Sessions run from varying working
   directories.

## 2. The loop

```bash
DC_TARGET=objs bash dc/build-dc.sh    # does it compile?          <- fast gate
bash dc/build-dc.sh                   # does it link + package?
harness/dc/smoke.sh                   # does it boot and say so?
harness/dc/crash.sh dc/build/TuxRacer.cdi   # if it didn't, why not
```

`make objs` is the cheap signal — 70-odd TUs, no link. Do not go looking for
runtime behaviour before that is green.

## 3. The `kb/` convention

`kb/` is the project's memory, and it holds **evidence, not tutorials**.

- `kb/STATE.md` — what works right now, what is next, what is known broken.
  Update it in the same commit as the change it describes.
- `kb/design-*.md` — one subsystem each. Every claim cites the command that
  produced it or a `file:line`. Anything assumed rather than verified is
  labelled as assumed, explicitly.
- `kb/traps.md` — things that cost real time and would cost it again. Each
  entry: the symptom, the actual cause, and the check that catches it next
  time.

A `kb/` file that says "X should work" without saying how that was established
is worse than no file: it gets trusted. Numbers come from a run, not from
memory. When a measurement is superseded, correct the file in place and say it
was corrected — do not leave two numbers standing.

## 4. Platform budget

| Resource | Size | Consequence |
|---|---|---|
| Main RAM | 16 MB | KOS + the ELF + every heap allocation. No large static buffers. |
| VRAM | 8 MB | All textures. Upstream's PC texture set does not fit uncompressed. |
| Sound RAM | 2 MB | AICA-local, separate from main RAM. |
| CPU | SH-4 @ 200 MHz | `-m4-single`. `double` is real double precision and slow — keep it out of per-vertex and per-frame paths. |
| GPU | PowerVR2 | Tile-based deferred. **No stencil buffer.** |

## 5. Style

C99 for `dc/src/` (`-std=gnu99`), gnu89 for `src/` — that is what upstream
was written against. 4-space indent, no tabs. Comments explain **why**, and
carry the evidence: `/* MEASURED: ... */` beats `/* this is faster */`.
