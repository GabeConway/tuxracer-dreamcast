/*
 * dc/include/GL/glext.h -- shadows kos-ports' GL/glext.h.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * src/gl_util.h:52-55 is a hard build stop:
 *
 *     #include <GL/glext.h>
 *     #if !defined(GL_GLEXT_VERSION) || GL_GLEXT_VERSION < 6
 *     #   error "*** You need a more recent copy of glext.h. ***"
 *     #endif
 *
 * kos-ports' GL/glext.h is a 232-line hand-written ARB-multitexture header. It
 * defines neither GL_GLEXT_VERSION nor the GL_EXT_compiled_vertex_array
 * function-pointer typedefs that src/gl_util.h:57-58 declares globals with.
 * Without this shim src/gl_util.c, src/quadtree.cpp and every TU that pulls in
 * gl_util.h fails to compile. src/ is verbatim upstream, so the fix goes here.
 *
 * dc/include is first on the include path, so this file wins; #include_next
 * then pulls in the real kos-ports header so nothing is lost.
 */

#ifndef TR_DC_GLEXT_H
#define TR_DC_GLEXT_H

#include_next <GL/glext.h>

/* Claim a version that clears upstream's >= 6 gate. The number is only ever
 * compared, never used to select behaviour: src/ reads GL_GLEXT_VERSION in
 * exactly one place, the #error above. */
#ifndef GL_GLEXT_VERSION
#define GL_GLEXT_VERSION 7
#endif

/* GL_EXT_compiled_vertex_array.
 *
 * These typedefs must exist because src/gl_util.c:393-394 defines
 *   PFNGLLOCKARRAYSEXTPROC   glLockArraysEXT_p   = NULL;
 *   PFNGLUNLOCKARRAYSEXTPROC glUnlockArraysEXT_p = NULL;
 * and src/quadtree.cpp:998-1020 calls through them.
 *
 * The pointers must STAY NULL on Dreamcast. src/gl_util.c:400-410 resolves
 * them from SDL_GL_GetProcAddress (dc/include/config.h defines HAVE_SDL), and
 * both call sites are guarded by `if ( glLockArraysEXT_p && ... )`, so a NULL
 * return simply skips CVA -- which is correct, GLdc has no
 * compiled-vertex-array extension and locking would be a no-op anyway. If the
 * SDL shim's SDL_GL_GetProcAddress ever returns non-NULL for these names,
 * quadtree.cpp will jump into it. See kb/design-gl.md.
 *
 * Note upstream src/gl_util.h:47 does `#undef GL_EXT_compiled_vertex_array`
 * immediately before including this file, so defining it here is deliberate
 * but inert -- nothing in src/ tests it. */
#ifndef GL_EXT_compiled_vertex_array
#define GL_EXT_compiled_vertex_array 1
#endif

#ifndef GL_ARRAY_ELEMENT_LOCK_FIRST_EXT
#define GL_ARRAY_ELEMENT_LOCK_FIRST_EXT 0x81A8
#define GL_ARRAY_ELEMENT_LOCK_COUNT_EXT 0x81A9
#endif

typedef void (*PFNGLLOCKARRAYSEXTPROC)(GLint first, GLsizei count);
typedef void (*PFNGLUNLOCKARRAYSEXTPROC)(void);

#endif /* TR_DC_GLEXT_H */
