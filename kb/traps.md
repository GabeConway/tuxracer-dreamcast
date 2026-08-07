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

---

## `## .` — upstream token pastes that no modern cpp accepts

**Symptom.** 26 copies of

```
src/game_config.c:119:28: error: pasting "." and "int" does not give a valid
preprocessing token
```

and nothing else in the file is wrong.

**Cause.** `src/game_config.c:116` and `:310` build member accesses with the
paste operator:

```c
#define INIT_PARAM( nam, val, typename, commnt ) \
   Params. ## nam ## .loaded = False; \
```

`.` followed by `nam` was never a single preprocessing token, so `. ## nam`
was always invalid — gcc 2.95 accepted it with a warning, gcc 15 rejects it.
The pastes are also **pointless**: `nam` is a macro parameter, so plain
`Params.nam.loaded` expands identically.

**Fix (applied).** One of only two edits to `src/`, in its own commit:
`s/\. ## /./g; s/ ## \./\./g` over `src/game_config.c`, 12 lines. The
`getparam_ ## name` and `typename ## _val` pastes are legitimate and were left
alone.

**Check.** `grep -rn '\. ##\|## \.' src/` must return nothing.

---

## `sh-elf-nm` reports no symbols for anything GL — they are LTO objects

**Symptom.** Enumerating what GLdc actually exports:

```
sh-elf-nm --defined-only $KOS_PORTS/lib/libGL.a | grep " T "
```

prints **nothing**. The same happens on the build's own `dc/build/obj/**/*.o`.
It reads exactly like "the library is empty" or "that symbol is missing".

**Cause.** kos-ports builds `libGL.a` with slim LTO, and `kos-cc` emits LTO
objects too. Plain `nm` cannot read them; the real message is buried above the
output:

```
sh-elf-nm: aligned_vector.c.obj: plugin needed to handle lto object
```

**Fix.** Use **`sh-elf-gcc-nm`**, which loads the LTO plugin. Two further
gotchas once it works: symbols carry a leading underscore (`sed 's/^_//'`), and
grepping only for `" T "` misses data — `glLockArraysEXT_p` is `B` (`.bss`), so
grep `[TDB]` when checking whether a symbol is defined anywhere.

**Check.** `sh-elf-gcc-nm --defined-only $KOS_PORTS/lib/libGL.a | grep -c " T "`
should print ~200, not 0.

---

## GLdc's headers declare functions its library does not define

**Symptom.** `glKosGetMatrix(GL_MODELVIEW, m)` compiles cleanly — it is
prototyped at `$KOS_PORTS/include/GL/gl.h:713` — and then fails at link with an
undefined reference.

**Cause.** GLdc's `GL/gl.h` and `libGL.a` are out of sync. `glKosGetMatrix` is
declared but never defined. There is no GLdc *source* in the SDK image (only
`libGL/inst/{include,lib,examples}`), so the header is the only documentation
available and it is not trustworthy.

**Fix.** Treat the header as a claim, not evidence. Before relying on any GLdc
entry point, confirm it is in the `sh-elf-gcc-nm` export list (above). For the
modelview specifically, `glGetFloatv(GL_MODELVIEW_MATRIX, m)` is the exported
alternative — though whether GLdc honours that *pname* is itself unverified, so
`dc/src/gl_compat.c` seeds the matrix to identity first rather than trusting it.

**Check.** Diff called-vs-exported before writing the call, not after the link:

```
comm -23 <(called symbols) <(sh-elf-gcc-nm --defined-only ... | grep " T ")
```

Note this also cuts the other way: `GL/gl.h:754-764` *defines* a block of
functions under the comment "Non Operational Stubs for portability"
(`glAlphaFunc`, `glStencilFunc`, `glStencilOp`, `glGetTexParameteriv`,
`glColorMask`, `glPixelStorei`, ...). They link and silently do nothing, which
is worse than absent. See `kb/design-gl.md` §5.
