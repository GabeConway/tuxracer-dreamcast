# design-gl — closing the OpenGL 1.2 + GLU gap against GLdc

Tux Racer 0.61 targets OpenGL 1.2 + GLU; GLdc implements a subset. This is the
measured gap and what was done about each item.

Files: `dc/include/tr_glcompat.h`, `dc/src/gl_compat.c`, `dc/include/GL/glext.h`,
wired in by `dc/Makefile` via `-include tr_glcompat.h` on every TU.
`src/` was **not** modified; no upstream patch was required.

## 1. Ground truth

GLdc's archives — and the build's own `.o` files — are **slim-LTO**: plain
`sh-elf-nm` prints `plugin needed to handle lto object` and no symbols, which
looks identical to a missing symbol. Use `sh-elf-gcc-nm`.

```
sh-elf-gcc-nm --defined-only $KOS_PORTS/lib/libGL.a $KOS_PORTS/lib/libGLU.a \
  | grep " T " | awk '{print $3}' | sed 's/^_//'      # 200 real exports
grep -hoE "^#define +GLU?_[A-Z0-9_]+" $KOS_PORTS/include/GL/*.h | awk '{print $2}'
grep -rhoE '\b(gl|glu)[A-Z][A-Za-z0-9]*' src/         # 84 entry points called
grep -rhoE '\bGLU?_[A-Z0-9_]+'          src/          # enums used
```

Headers and library **disagree**; check both. `glKosGetMatrix` is prototyped at
`GL/gl.h:713` but has no definition in `libGL.a` — using it fails the link.

## 2. Gap table — called by the game, not exported by GLdc

`comm -23 <called> <exported ∪ declared>` → 24 items.

| Entry point | Call site | Decision | Notes |
|---|---|---|---|
| `glGenLists` `glNewList` `glEndList` `glCallList` | `hier_util.c:187-191,220` | **implement** | geometry recorder, §3 |
| `gluNewQuadric` `gluDeleteQuadric` `gluQuadricDrawStyle` `gluQuadricNormals` `gluQuadricOrientation` `gluSphere` | `hier_util.c:29-43` | **nothing to do** | `#define USE_GLUSPHERE 0` (`hier_util.c:25`) — the whole block is inside `#if USE_GLUSPHERE`, never compiled. `draw_sphere()` at `:46` is hand-written immediate mode. No tessellator needed. |
| `glTexGeni` `glTexGenfv` | `gl_util.c:94,165`, `course_render.c:278`, `hud.c:366`, `quadtree.cpp:1026` | **emulate** | highest-value item, §4 |
| `glGetTexLevelParameteriv` | `textures.c:241`, `splash_screen.c:98` | **implement** | track dims on upload, §5 |
| `gluScaleImage` | `textures.c:146` | **implement** | bilinear, `GL_UNSIGNED_BYTE` only |
| `glMultMatrixd` | `hier_util.c:209`, `view.c:320` | **implement** | narrow to float, §6 |
| `glColor3dv` `glColor4dv` `glVertex2dv` `glTexCoord2d` `glTexCoord2dv` | `fonts.c:231` + others | **implement** | narrow to float, §6 |
| `glClearStencil` | `render_util.c:86` | **no-op** | §7 |
| `glLockArraysEXT` `glUnlockArraysEXT` | `gl_util.c:414-416` | **report absent** | never called; they appear only as *string literals*. §6a |
| `SDL_GL_GetProcAddress` | `gl_util.c:403` | **implement, returns NULL** | §6a |
| `glXGetProcAddressARB` | `gl_util.c:407` | **nothing to do** | inside `#elif defined(HAVE_GLXGETPROCADDRESSARB)`; `config.h:65` does not define it. |

Missing **enums** (17) are supplied by `tr_glcompat.h`, each `#ifndef`-guarded:
`GL_S/T/R/Q`, `GL_TEXTURE_GEN_S/T`, `GL_TEXTURE_GEN_MODE`,
`GL_OBJECT_PLANE`, `GL_EYE_PLANE`, `GL_OBJECT_LINEAR`, `GL_EYE_LINEAR`,
`GL_SPHERE_MAP`, `GL_TEXTURE_WIDTH/HEIGHT`, `GL_COMPILE`, `GL_VIEWPORT`,
`GL_DOUBLEBUFFER`.

Present and functional, **left alone**: `glReadPixels`, `glReadBuffer`
(`screenshot.c:46-49`), `glRectf`/`glRecti`, and `glDrawElements` with
`GL_UNSIGNED_INT` indices — used by GLdc's own `examples/nehe02de/main.c:92`,
so the terrain's index type is safe and needs no narrowing.

## 3. Display lists — a geometry recorder, not a command buffer

The brief suggested lists are used "heavily by `fonts.c`, `tux.c`,
`course_render.c`". **That is wrong.** Measured:

```
grep -rn 'glNewList\|glGenLists\|glCallLists\|glListBase' src/
  -> src/hier_util.c:187, src/hier_util.c:188   (nothing else, anywhere)
```

`fonts.c` has no display list calls at all. So there is exactly **one**
recording site, and the only thing it records is `draw_sphere()`
(`hier_util.c:46-152`), which emits *nothing but* `glBegin` / `glNormal3f` /
`glVertex3f` / `glEnd`. A general command buffer is unwarranted: the shim
records positions + normals only.

Everything is flattened to `GL_TRIANGLES` at record time so a list replays as a
**single `glDrawArrays`**. Two reasons: `traverse_dag` (`hier_util.c:216-222`)
calls `glCallList` once per scene node and Tux is built entirely from spheres,
so preserving per-latitude strips would cost ~8 submissions × ~30 nodes ≈ 240
draw calls/frame instead of ~30 (the ~3× vertex expansion is bought back
easily); and GLdc's array path refuses fans outright — `GL/gl.h:662`, *"Only
GL_TRIANGLES, GL_TRIANGLE_STRIP, and GL_QUADS are supported"* — while
`draw_sphere` uses `GL_TRIANGLE_FAN` for the pole caps, so conversion was
mandatory regardless. Strip winding is flipped on odd triangles so facing
survives. Bound is 14 lists (`hier.h:28-29`, divisions 3..16); `TR_MAX_LISTS`
is 64.

**Replay clobbers client array state**, and that is a real hazard:
`course_load.c:321-330` sets the terrain's vertex/normal/**colour** pointers
*once* at course load and never again. Leaving `GL_COLOR_ARRAY` enabled would
draw Tux with the terrain's colours; leaving the vertex pointer moved would
blank the terrain for the rest of the run. The shim shadows all four pointers
plus their enables and restores them exactly after every replay.

## 4. Texgen — the item that matters most

GLdc has **no texgen whatsoever**. Two render modes enable it
(`gl_util.c:87-88` GAUGE_BARS, `gl_util.c:158-159` COURSE), and the COURSE case
is the entire terrain: `course_render.c:274-280` sets `GL_OBJECT_PLANE` to
`x/TEX_SCALE`, `z/TEX_SCALE` (`TEX_SCALE 6`, `textures.h:28`), and the terrain
draws via `glDrawElements` (`quadtree.cpp:1013`) from an interleaved array with
**no texture coordinate array at all** (`course_load.c:321-330` sets vertex,
normal and colour pointers only). Dropping texgen would render the whole course
with whatever the last `glTexCoord2f` left behind — one texel stretched over the
map. So it is emulated on the CPU:

- **`GL_OBJECT_LINEAR`, array path.** `s,t = dot(plane, (x,y,z,1))` is a pure
  function of the vertex and the terrain array is static after load, so
  coordinates are generated **once** into a cache that grows to the highest
  index seen, invalidated only by a plane change or new vertex pointer. Steady
  state is one index scan per draw for the max index. Memory 8 B/vertex — a
  61×1000 course ≈ 490 KB against the ~2.2 MB the interleaved array already
  costs. *Assumed*: courses stay in that size class; not measured against
  shipped course data.
- **`GL_OBJECT_LINEAR`, immediate path.** GAUGE_BARS draws `GL_QUADS` of
  `glVertex2f` (`hud.c:384-390`); the per-vertex hook emits `glTexCoord2f`
  first. Correct per spec — object-plane texgen applies in object coordinates,
  so `hud.c`'s enclosing `glTranslatef` does not affect it.
- **`GL_SPHERE_MAP`, array path** (`quadtree.cpp:1026`, envmap terrain).
  Per spec from eye-space position and normal, regenerated every draw but only
  for the indices that draw references, into a **separate** buffer so it cannot
  corrupt the object-linear cache. Two deliberate caveats: the normal uses the
  modelview's upper 3×3 rather than its inverse transpose (identical for the
  rotation+translation modelviews this game uses), and `GL_MODELVIEW_MATRIX` is
  seeded to identity because **GLdc's support for that query is unverified** —
  no GLdc source ships in the SDK image. If it is a no-op, sphere mapping
  degrades to object space rather than reading stack garbage.

## 5. Queries GLdc cannot answer

`GL/gl.h:754-764` files these under **"Non Operational Stubs for portability"**:
`glAlphaFunc`, `glPolygonMode`, `glPolygonOffset`, `glGetTexParameterfv/iv`,
`glColorMask`, `glPixelStorei`, `glStencilFunc`, `glStencilOp`, `glGetTexImage`.
They link but do nothing.

- **`glGetTexLevelParameteriv`** is absent entirely and `glGetTexParameteriv` is
  a stub, so GLdc cannot be asked. Dimensions are recorded on the way in by
  intercepting `glBindTexture` + `glTexImage2D` + **`gluBuild2DMipmaps`**. The
  third is essential: `textures.c:160` uploads every game texture through GLU,
  which calls `glTexImage2D` *inside libGL* where a macro cannot reach it.
- **`GL_VIEWPORT`** is not even a defined enum in GLdc and `screenshot.c:42`
  needs it — `glViewport` is intercepted and the value shadowed.
- **`GL_MAX_TEXTURE_SIZE`** (`textures.c:130`) is asked of GLdc first, then
  clamped to the PVR2 maximum of 1024 either way; an unanswered query would
  decide the downscale on an uninitialised variable. Lowering to 512 is a
  one-line VRAM lever if 8 MB gets tight.

## 6. Doubles

`tux_types.h:30` makes `scalar_t` a `double`, so upstream reaches for `*d`/`*dv`
forms. sh-elf is `-m4-single`: `float` runs on the FPU, `double` is emulated.
All are thin narrowing wrappers. The vertex/texcoord ones route through the
shim's own hooks, not GLdc directly, so a double-precision vertex inside a
display list or under texgen behaves identically to a float one.

## 6a. CVA — `SDL_GL_GetProcAddress` returns NULL

`config.h` defines `HAVE_SDL`, so `gl_util.c:403` resolves GL entry points
through `SDL_GL_GetProcAddress`. We do not link SDL, so nothing declared it —
this was the last blocking compile error in the tree. It is declared in
`tr_glcompat.h` and defined in `gl_compat.c` rather than in `dc/include/SDL.h`,
because the only names it is ever passed are GL ones: `"glLockArraysEXT"` and
`"glUnlockArraysEXT"` (`gl_util.c:414,416`). SDL 1.2's exact signature is used
so it stays compatible if the winsys shim also declares it.

**It always returns NULL, and that is the right answer rather than a stub.**
GLdc does not implement CVA — neither symbol is in `libGL.a`, and there is
nothing for it to mean: CVA asks a driver to lock a vertex range to reuse
transformed results across draws, and GLdc transforms into the PVR store queues
per submission with no such cache. A no-op `glLockArraysEXT` would be a lie
costing a call per terrain draw. NULL puts `gl_util.c:418-431` on its own
`"extension NOT supported"` path, which upstream already ships.

**Consequence: `use_cva` and `cva_hack` are inert.** Both call sites
(`quadtree.cpp:1001,1017`) read `if (glLockArraysEXT_p && getparam_use_cva())`
and the pointer test fails first, so toggling either config option changes
**nothing** — no slowdown, no speedup. `cva_hack` works around psychedelic
colours on TNT/TNT2 drivers (`quadtree.cpp:1003`) and is meaningless here.

## 7. No-ops and their exact visual consequence

| No-op | Visual consequence |
|---|---|
| `glClearStencil` | **None.** `USE_STENCIL_BUFFER` is not defined for this build (absent from `config.h`), so `gl_util.c:278` and `tux_shadow.c:33` take upstream's *own* no-stencil fallback: cull-face instead of stencil, shadow alpha 0.1 instead of 0.3 (`tux_shadow.c:35,39`). Overlapping shadow polygons double-blend — exactly what upstream already accepts on non-stencil hardware. |
| `GL_STENCIL_BUFFER_BIT` masked out of `glClear` (`render_util.c:87-89`) | **None.** Nothing reads a stencil buffer. Masked rather than passed so GLdc is never handed a bit it does not recognise. |
| `glStencilFunc` / `glStencilOp` (GLdc's own stubs) | **None** — the only call sites, `gl_util.c:292-293`, are inside the dead `#ifdef USE_STENCIL_BUFFER`. |
| `GL_EYE_PLANE` treated as `GL_OBJECT_PLANE`; `GL_COMPILE_AND_EXECUTE` records without drawing; colours/texcoords inside a display list not recorded | **None today** — the game uses only `GL_OBJECT_PLANE` and `GL_COMPILE` (`hier_util.c:188`), and `draw_sphere` emits neither colours nor texcoords. Each warns once, except list colours: `glColor*` is hot and not intercepted, so if a list ever gains colours they are dropped silently. |

Every "warns once" is a tripwire, not a rendering path: if one prints, a claim
in this document is wrong.

## 8. Mechanism

`dc/Makefile` adds `-include tr_glcompat.h` to `COMMON`, putting the header in
front of every TU. It works two ways, deliberately split:

- **Pure additions** (`glNewList`, `glTexGeni`, `glMultMatrixd`,
  `gluScaleImage`, …) are declared and defined under their **real names** — no
  macro, nothing rewritten, the linker just resolves them to us.
- **Intercepts** (18 entry points GLdc *does* have but the shim must observe)
  are object-like macros to `tr_`-prefixed wrappers, each listed with its reason
  in the header's INTERCEPTS block. Six per-vertex ones are `static inline` with
  a `__builtin_expect` fast path, so the common case costs one predictable
  branch, not a call.

`-Wl,--wrap=` was rejected: the additions need a header regardless, and
kos-ports `libGL.a` is slim-LTO where `--wrap` is unreliable. `gl_compat.c` is
force-included too, so it both defines `TR_GLCOMPAT_IMPL` (covers a standalone
compile) **and** `#undef`s the macro block after the include (covers the
force-included build). Both are needed.

`dc/include/GL/glext.h` shadows the SDK's. Upstream `gl_util.h:52-55` is a hard
`#error` unless `GL_GLEXT_VERSION >= 6` and declares globals of type
`PFNGLLOCKARRAYSEXTPROC`; kos-ports' `glext.h` is a 232-line ARB-multitexture
header with neither. The shadow `#include_next`s the real one and adds the two
missing pieces. Without it every TU including `gl_util.h` fails.

## 9. Risks

- **`glAlphaFunc` is a GLdc no-op**, and `gl_util.c` enables `GL_ALPHA_TEST` for
  TREES and PARTICLES. Expect tree billboards to blend rather than punch
  through — wrong sort order against the terrain. The fix is routing those
  draws into GLdc's punch-through list; out of scope here, but the most likely
  visible artefact after texgen.
- **Sphere-map texgen depends on an unverified `GL_MODELVIEW_MATRIX` query.**
  Degrades safely (identity) but silently. Envmap courses only.
- **Object-linear texcoord cache is unbounded** — grows to the highest vertex
  index a course ever draws. Not measured against shipped course data.
- **`SDL_GL_GetProcAddress` must keep returning NULL** (§6a). Anything making it
  non-NULL for `"glLockArraysEXT"` sends `quadtree.cpp:1001` jumping into it.
- GLdc's `glReadPixels` is exported but **unverified** — no GLdc source ships in
  the SDK image and it sits in the same header region as the stubbed
  `glCopyTex*` family. `screenshot.c` and any framebuffer-hash gate depend on
  it. Left alone deliberately; if it proves a stub, the fallback is reading the
  PVR framebuffer directly, which is a separate change.

## 10. Build state

`make -C dc -k objs` compiles **77 of 77 TUs, zero errors**, from a clean
`dc/build`. The 13 remaining warnings are all pre-existing upstream `src/`
ones (`-Wwrite-strings` in the C++ TUs, `hier.c` maybe-uninitialised,
`save.c` format-overflow); `gl_compat.c` compiles clean.

Every `gl*` / `glu*` / `SDL_GL_*` symbol referenced across the 77 objects has a
definition, checked by differencing `sh-elf-gcc-nm --undefined-only` over the
objects against `--defined-only` over `libGL.a`, `libGLU.a` and the objects.
Grep for `[TDB]`, not just `T`: `glLockArraysEXT_p` / `glUnlockArraysEXT_p` are
`.bss`. `SDL_GL_GetProcAddress` is defined in exactly one object
(`dc/build/obj/dc/src/gl_compat.c.o`) — no duplicate-symbol risk against
`dc/include/SDL.h` or `dc/src/dc_mixer.c`.
