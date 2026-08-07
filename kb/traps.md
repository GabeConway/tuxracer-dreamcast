# Traps

Things that cost real time, and the check that catches each one next time.
Symptom first — that is how you will arrive here.

---

## `scif_flush()` permanently kills serial output

**Symptom.** The guest logs normally, then goes completely silent. A later
crash produces no register dump at all; the run shows up only as a wall-clock
timeout, which reads as "hang" rather than "crash".

**Cause.** KOS's `scif_flush()` clears TEND and spins waiting for it to come
back. Flycast never re-raises TEND on an already-idle TX FIFO, the spin times
out, and KOS latches `serial_enabled = 0` for the rest of the run.

**Check.** `grep -rn 'scif_flush' dc/ src/` must return nothing. `printf()`
already flushes; the call is never needed.

*(Inherited from OpenCrossing-Dreamcast, verified there on KOS 2.3.0 +
Flycast v2.6. Not independently re-verified in this repo.)*

---

## `mkdcdisc -d` instead of `-D` makes every asset open miss

**Symptom.** The image boots, the game starts, and every single file open
fails — with no error that names the real problem.

**Cause.** `-d DIR` puts the directory *itself* on the disc, so files land at
`/cd/discroot/<name>`. `dc/include/config.h` sets `DATA_DIR "/cd"`, so the
game looks at `/cd/<name>`.

**Check.** `dc/build-dc-docker.sh` uses `-D`. If assets go missing, list the
disc root before blaming the loader.

---

## An environment variable that is *set but empty* defeats make's `?=`

**Symptom.** A knob documented as having a default silently behaves as if the
default were empty — e.g. `TR_OPT` blanks and `$KOS_CFLAGS`' own `-O2` wins,
so a build you believe is `-O0` is not.

**Cause.** `make` treats a variable present in the environment as defined even
when its value is empty, so `?=` does not fire. `docker run -e TR_OPT=` passes
exactly that.

**Check.** In `dc/build-dc.sh`, every knob with a Makefile-side default is
forwarded with the `[ -n "${VAR+x}" ] && ENVARGS+=(-e VAR="$VAR")` form, never
unconditionally. Verified on this host that a failing `[ -n ... ] && ...` does
**not** trip `set -e` in bash 3.2:

```
$ bash -c 'set -euo pipefail; A=(); [ -n "${NOPE+x}" ] && A+=(x); echo survived'
survived
```

---

## `"${ARRAY[@]}"` is an unbound variable in bash 3.2

**Symptom.** `dc/build-dc.sh` dies with `unbound variable` on any build that
does not set `TR_DATA`.

**Cause.** macOS ships bash 3.2, where expanding an *empty* array under
`set -u` is an error.

**Check.** Use `${ARRAY[@]+"${ARRAY[@]}"}`, which expands to nothing when the
array is empty.

---

## `-fstrict-aliasing` on 2001 C

**Symptom.** Not a compile error. A render that is subtly wrong, or a physics
value that is NaN, only at `-O2`.

**Cause.** `src/alglib.c` and `src/image.c` pun between float and integer
representations through casts. GCC 15 at `-O2` assumes that never happens.

**Check.** `dc/Makefile` passes `-fno-strict-aliasing` to every TU. If someone
"cleans up the flags", this is the one that must not go.
