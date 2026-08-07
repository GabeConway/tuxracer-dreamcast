# Tcl on Jim — evidence

`src/` is written against Tcl 8.0; the target has Jim Tcl 0.84.
`dc/include/tcl.h` declares the Tcl 8.0 surface `grep -rn 'Tcl_' src/` reports
and no more; `dc/src/tcl_compat.c` implements it over `-ljim`.

## 1. What the shipped libjim actually is

`/opt/toolchains/dc/kos-ports/libjimtcl/files/KOSMakefile.mk:2`:
`CONFIGURE_ARGS = --without-ext="aio,zlib"`.
`jim-config.h`: `JIM_VERSION 84`, `0.84-8-g825be07`, `JIM_UTF8 1`, `JIM_TAINT 1`.

`sh-elf-ar t libjim.a` — C extensions in: `array clock eventloop exec file
history interp json namespace pack package readdir regexp tclprefix`. Tcl-level:
`binary ensemble glob jsonencode nshelper oo stdlib tclcompat tree`.
**No `jim-aio.o`, no `jim-zlib.o`.**

Inventory for what the stock data scripts use, verified in the 0.84 sources:

| Script command | Present? | Where |
|---|---|---|
| `if while foreach catch eval expr proc global set incr return break continue list lindex llength lappend concat string format info source` | yes | `jim.c` core table |
| `regexp` `regsub` | yes | `jim-regexp.c:666-667` |
| `cd` `pwd` `file` | yes | `jim-file.c` |
| `glob` | yes | `glob.tcl` → `package require readdir` → `jim-readdir.o` |
| `open gets close read eof flush puts fconfigure` | **NO** | all behind aio |

`puts` disappears with the rest because `tclcompat.tcl:12` wraps every one of
those procs in `if {[exists -command stdout]} {...}`, and `stdout` is an aio
command. A stock interp on this target has **no `puts` at all**.

`courses/course_idx.tcl` uses four: `open` (:6), `gets` (:15), `puts stderr`
(:7, :90) — and `tuxracer_init.tcl:23` sources it unconditionally. The `open`
failure is caught by the surrounding `catch`; the `puts stderr` on the next line
is not, so the error escapes `Tcl_EvalFile` into `src/main.c:105 handle_error()`
— **a hard exit at boot, not a degraded course list**.

`tcl_compat.c` §8 therefore registers `open`/`gets`/`close`/`eof`/`puts` itself:
read-only, line-oriented, no channel objects. Not provided: `read`, `seek`,
`tell`, `flush`, `fconfigure`, write modes, sockets.

**Taint is inert.** `JIM_TAINT 1` is compiled in, but `grep -rn JIM_TAINT_STD
*.c` finds one taint source — `jim-aio.c:2727` — which is not linked. `cd` and
`exec` carry `JIM_CMD_NOTAINT` and would reject tainted arguments; nothing can
taint anything, so they never fire.

## 2. `access()` — a link-time blocker, not a Tcl one

`nm --undefined-only libjim.a` minus every symbol defined by every `.a` under
`/opt/toolchains/dc` leaves exactly one name: `_access`. newlib *declares*
`access()` (`sh-elf/include/sys/unistd.h:20`) and defines it nowhere.

`jim-file.c:492` implements `file exists` as `access(path, F_OK)`, and
`jim-file.o` is linked unconditionally by `Jim_InitStaticExtensions`. Without a
definition the game does not link. `tcl_compat.c` §11 supplies one over `stat()`
— KOS's iso9660 mount is read-only and has no permission bits, so a successful
`stat` answers every mode. Verified by linking a synthetic TU that replays every
`src/` call pattern: `kos-cc -o sig.elf sig.o tcl_compat.o -ljim` succeeds,
`nm --undefined-only sig.elf` is empty, `_access` resolves to ours.

## 3. API map

| Tcl 8.0 | Jim |
|---|---|
| `Tcl_CreateInterp` | `Jim_CreateInterp` + `Jim_RegisterCoreCommands` + `Jim_InitStaticExtensions` + our I/O commands (mirrors `jimsh.c:103-107`) |
| `Tcl_Interp *` | `Jim_Interp *` — a cast, not a wrapper, so callbacks get back the exact pointer `src/main.c:151` stored |
| `Tcl_EvalFile` | `Jim_EvalFile`, then `Jim_MakeErrorMessage` on failure |
| `Tcl_CreateCommand` | `Jim_CreateCommand`, privData = `{Tcl_CmdProc, ClientData}` bridge |
| `Tcl_AppendResult` | result → `Jim_NewStringObj` → `Jim_AppendString` × n → `Jim_SetResult` |
| `Tcl_GetStringResult` | `Jim_String(Jim_GetResult(...))` |
| `Tcl_SetObjResult` | `Jim_SetResult` after converting our `Tcl_Obj` |
| `Tcl_New{String,Int,Boolean}Obj` | own heap record, converted and freed by `Tcl_SetObjResult` (§5) |
| `Tcl_Get{Int,Double,Boolean}` | temp `Jim_Obj` + `Jim_Get{Long,Double,Boolean}` |
| `Tcl_SplitList` / `Tcl_Free` | `Jim_ListLength`/`Jim_ListGetIndex` into one `malloc` block / `free` |
| `Tcl_{Get,Set}Var` | `Jim_{Get,Set}GlobalVariableStr` |
| `Tcl_*Hash*` | **not** Jim's — own open hash, §4 |
| `Tcl_InterpDeleted` | constant 0 |
| `Tcl_SetStdChannel`, `Tcl_MakeFileChannel` | no-ops, §6 |

The bridge NUL-terminates `argv` (`argv[argc] = NULL`), as does `Tcl_SplitList`.
Not cosmetic: `src/tcl_util.h:44 CHECK_ARG` and `src/course_mgr.c:447` walk until
`*argv == NULL`.

`Tcl_AppendResult` seeds from the current result rather than starting empty,
because `src/hier_cb.c:56-59` lets `Tcl_GetDouble` install "expected
floating-point number but got ..." and then appends to it. Safe because Jim
empties the result before every dispatch (`jim.c JimInvokeCommand`,
`Jim_SetEmptyResult` immediately before the call).

## 4. Why the hash table is ours, not Jim's

`src/hash.c:90` declares a `Tcl_HashSearch` **by value** and `src/hash.c:119`
reads `searchPtr->tablePtr`; `src/hier.c:28-29` declares two `Tcl_HashTable` by
value and `src/hash.c:27` does `malloc(sizeof(Tcl_HashTable))`. Complete types
with a stable layout are required. Jim's iterator is heap-allocated and has no
`tablePtr` to expose. ~150 lines of open hashing is smaller than the adapter
would be and does not move when Jim does a point release.

String keys only — both callers ask for `TCL_STRING_KEYS` (`src/hier.c:417-418`,
`src/hash.c:28`); `TCL_ONE_WORD_KEYS` is recorded and ignored. `Tcl_HashEntry`
ends in `char key[1]` and is over-allocated, because `src/` compiles as `gnu89`.
`Tcl_FirstHashEntry`/`NextHashEntry` fetch the next entry before returning the
current one, so `src/hash.c:114-121`'s scan-and-delete is safe.

## 5. `Tcl_NewStringObj` has no interpreter argument

Tcl 8.0's object constructors take no interp; every `Jim_Obj` constructor needs
one. Rather than keep a global "the one interp", a `Tcl_Obj` here is a small heap
record that `Tcl_SetObjResult` converts and frees. Sound only because **every**
construction site hands the object straight to `Tcl_SetObjResult` — checked, all
nine: `src/audio_data.c:624,652,764,774`, `src/course_mgr.c:1339`,
`src/game_config.c:1051,1056,1061,1066`. A future edit that builds one and drops
it leaks silently.

`Tcl_NewStringObj` honours the explicit length instead of `strlen`ing, because
`src/game_config.c:1056` passes `(&parm->val.char_val, 1)` — one char, not
NUL-terminated.

## 6. Deliberate stubs, and what they cost

- **`Tcl_SetStdChannel` / `Tcl_MakeFileChannel` → no-ops.** Their only caller,
  `src/tcl_util.c:140-170`, is entirely inside
  `#if defined( NATIVE_WIN32_COMPILER )`. Cost here: none.
- **`Tcl_InterpDeleted` → 0.** The shim never deletes an interpreter. The three
  `check_assertion`s (`src/main.c:113`, `src/tux.c:191`, `src/course_load.c:358`)
  become tautologies.
- **`Tcl_SplitList` never returns `TCL_ERROR`.** `jim.c SetListFromAny` is
  unconditionally `return JIM_OK` — its own comment says "The string->list
  conversion can't fail". Returning an error would also be dangerous:
  `src/tcl_util.c:43` calls `Tcl_Free` on an **uninitialised** `indices` in its
  error path. Cost: malformed lists split best-effort; the callers' own
  element-count checks (`src/tcl_util.c:37`) catch what matters.
- **`Tcl_GetVar`/`Tcl_SetVar` ignore `flags`, always global.** Every call site
  passes `TCL_GLOBAL_ONLY`.

## 7. Risks, stated plainly

- **Integer literals may parse differently.** Tcl 8.0 reads a leading `0` as
  octal; Jim 0.84 dropped octal. **Not verified against the data set** — none was
  found by inspection, but no exhaustive check was run.
- **`Jim_MakeErrorMessage` evaluates script** in the interpreter that just
  failed, to turn a bare message into `file:line: Error: ...` + stack dump
  (`stdlib.tcl:73`). Guarded by a `Jim_GetCommand` existence check. Remove it in
  `tcl_compat.c` §9 if it ever misbehaves.
- **The `open` handle table is fixed at 32 and leaks by design.**
  `get_course_info` (`courses/course_idx.tcl:5-43`) never closes what it opens —
  one `FILE *` per contrib course, 11 in stock data. Past 32, `open` errors and
  the surrounding `catch` degrades that course to "Unknown"/120.0.
- **Nothing here has been run.** All the above is compile- and link-time
  evidence. No script has been evaluated on hardware or in Flycast.

## 8. Reached but not fixed by this work

`kos-cc -std=gnu89 -DHAVE_CONFIG_H -fsyntax-only -Idc/include -Isrc
-I$KOS_PORTS/include` over every Tcl-touching TU. Clean: `hash.c tcl_util.c
hier.c hier_cb.c course_mgr.c keyframe.c audio_data.c fonts.c`. Two unrelated
failures, neither a Tcl problem, both blocking the build:

- `game_config.c:117` — gcc 15 rejects pasting `.` with an identifier in
  `Params. ## nam ## .loaded`. Upstream relied on gcc 2.95.
- `lights.c fog.c course_load.c tux.c part_sys.c main.c textures.c` — all die in
  `src/gl_util.h:55` on `glext.h`.
