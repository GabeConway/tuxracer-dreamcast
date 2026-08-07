/*
 * tr_glcompat.h -- OpenGL 1.2 + GLU compatibility shim for GLdc / PowerVR2.
 *
 * Tux Racer 0.61 targets an OpenGL 1.2 + GLU implementation. GLdc implements
 * a subset. This header closes the gap; dc/src/gl_compat.c has the bodies and
 * kb/design-gl.md has the evidence table (what GLdc really exports, measured
 * with sh-elf-gcc-nm against libGL.a/libGLU.a) plus the per-item decision.
 *
 * MECHANISM
 * ---------
 * This header is FORCE-INCLUDED into every TU by dc/Makefile (`-include
 * tr_glcompat.h`). It works in two distinct ways, and the distinction matters
 * when you read the code:
 *
 *   1. PURE ADDITIONS. Entry points GLdc simply does not have (glNewList,
 *      glTexGeni, glMultMatrixd, gluScaleImage, ...). These are declared and
 *      defined under their REAL names. No macro, no redirection: the linker
 *      resolves the game's call to our definition because nothing else
 *      defines that symbol. Nothing about the call is rewritten.
 *
 *   2. INTERCEPTS. Entry points GLdc *does* have but which we must observe to
 *      make (1) work -- e.g. we cannot answer glGetTexLevelParameteriv without
 *      watching glTexImage2D, and we cannot emulate texgen without watching
 *      glDrawElements. These are redirected with an object-like macro to a
 *      tr_-prefixed wrapper that does its bookkeeping and then calls the real
 *      GLdc function. Every one is listed in the INTERCEPTS block at the
 *      bottom of this file with a note saying WHY it is intercepted.
 *
 * We chose force-include + macro over `-Wl,--wrap=` because (a) the additions
 * need declarations in a header anyway, so a header is unavoidable, and (b)
 * kos-ports libGL.a is a slim-LTO archive, where --wrap is unreliable.
 *
 * dc/src/gl_compat.c is force-included too, so it #undefs the whole INTERCEPTS
 * block at the top before defining the wrappers. See the note there.
 */

#ifndef TR_GLCOMPAT_H
#define TR_GLCOMPAT_H

/* Pull the real GLdc headers in FIRST, while the intercept macros are still
 * undefined. If a GL header were included after the #define block below, its
 * prototypes would be rewritten into tr_-prefixed ones and conflict with ours.
 * All three have include guards, so the game's later <GL/gl.h> is a no-op. */
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glkos.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 1. Enums GLdc does not define.
 *
 * Measured: comm -23 (enums used by src/) (enums #defined in kos-ports GL headers)
 * Values are the canonical GL 1.2 / khronos ones -- they must match, because
 * the game passes them straight back to us.
 * Each is #ifndef-guarded so a future GLdc that adds them wins instead of
 * colliding.
 * ========================================================================== */

#ifndef GL_S
#define GL_S                        0x2000
#endif
#ifndef GL_T
#define GL_T                        0x2001
#endif
#ifndef GL_R
#define GL_R                        0x2002
#endif
#ifndef GL_Q
#define GL_Q                        0x2003
#endif

#ifndef GL_TEXTURE_GEN_S
#define GL_TEXTURE_GEN_S            0x0C60
#endif
#ifndef GL_TEXTURE_GEN_T
#define GL_TEXTURE_GEN_T            0x0C61
#endif
#ifndef GL_TEXTURE_GEN_MODE
#define GL_TEXTURE_GEN_MODE         0x2500
#endif
#ifndef GL_OBJECT_PLANE
#define GL_OBJECT_PLANE             0x2501
#endif
#ifndef GL_EYE_PLANE
#define GL_EYE_PLANE                0x2502
#endif
#ifndef GL_EYE_LINEAR
#define GL_EYE_LINEAR               0x2400
#endif
#ifndef GL_OBJECT_LINEAR
#define GL_OBJECT_LINEAR            0x2401
#endif
#ifndef GL_SPHERE_MAP
#define GL_SPHERE_MAP               0x2402
#endif

#ifndef GL_TEXTURE_WIDTH
#define GL_TEXTURE_WIDTH            0x1000
#endif
#ifndef GL_TEXTURE_HEIGHT
#define GL_TEXTURE_HEIGHT           0x1001
#endif

#ifndef GL_COMPILE
#define GL_COMPILE                  0x1300
#endif
#ifndef GL_COMPILE_AND_EXECUTE
#define GL_COMPILE_AND_EXECUTE      0x1301
#endif

#ifndef GL_VIEWPORT
#define GL_VIEWPORT                 0x0BA2
#endif
#ifndef GL_DOUBLEBUFFER
#define GL_DOUBLEBUFFER             0x0C32
#endif

/* ==========================================================================
 * 2. Pure additions -- real names, no macro redirection.
 * ========================================================================== */

/* --- Display lists -------------------------------------------------------
 * src/hier_util.c:187-191 is the ONLY glNewList site in the whole game, and
 * the only thing it ever records is draw_sphere(), which emits exclusively
 * glBegin / glNormal3f / glVertex3f / glEnd (src/hier_util.c:46-152). So the
 * recorder is geometry-only: it flattens every primitive to GL_TRIANGLES and
 * replays the list as a single glDrawArrays. See kb/design-gl.md. */
GLuint    glGenLists(GLsizei range);
void      glNewList(GLuint list, GLenum mode);
void      glEndList(void);
void      glCallList(GLuint list);
void      glDeleteLists(GLuint list, GLsizei range);
GLboolean glIsList(GLuint list);

/* --- Texture coordinate generation --------------------------------------
 * GLdc has no texgen at all. GL_OBJECT_LINEAR and GL_SPHERE_MAP are emulated
 * on the CPU; see gl_compat.c. glTexGeni/glTexGenfv only record state. */
void glTexGeni(GLenum coord, GLenum pname, GLint param);
void glTexGenf(GLenum coord, GLenum pname, GLfloat param);
void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params);
void glTexGendv(GLenum coord, GLenum pname, const GLdouble *params);

/* --- Double-precision entry points --------------------------------------
 * src/tux_types.h:30 makes scalar_t a double, so upstream reaches for the *d
 * forms. sh-elf is -m4-single: float is in hardware, double is emulated and
 * roughly an order of magnitude slower. Converting at the API boundary is the
 * cheap fix -- the values are all colours/coords that never needed 53 bits. */
void glMultMatrixd(const GLdouble *m);
void glLoadMatrixd(const GLdouble *m);
void glColor3dv(const GLdouble *v);
void glColor4dv(const GLdouble *v);
void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a);
void glVertex2dv(const GLdouble *v);
void glVertex3dv(const GLdouble *v);
void glNormal3dv(const GLdouble *v);
void glTexCoord2d(GLdouble s, GLdouble t);
void glTexCoord2dv(const GLdouble *v);

/* --- Stencil -------------------------------------------------------------
 * PowerVR2 has NO stencil buffer. GLdc's own gl.h files glStencilFunc and
 * glStencilOp under "Non Operational Stubs for portability"; glClearStencil
 * it does not have at all. This is a documented no-op -- see kb/design-gl.md
 * for why the visual consequence is nil (USE_STENCIL_BUFFER is not defined
 * for this build, so upstream's own no-stencil fallback is what runs). */
void glClearStencil(GLint s);

/* --- GL entry point lookup -----------------------------------------------
 * src/gl_util.c:403 does
 *     get_gl_proc = (get_gl_proc_fptr_t) SDL_GL_GetProcAddress;
 * because dc/include/config.h defines HAVE_SDL. We do not link SDL, so
 * nothing declares it.
 *
 * This lives here rather than in dc/include/SDL.h on purpose: the only thing
 * the function is ever asked for is GL entry points ("glLockArraysEXT",
 * "glUnlockArraysEXT" -- src/gl_util.c:414,416, the only two call sites in the
 * game), so whether it can answer is a GL question, not an SDL one. It is
 * declared with SDL 1.2's exact signature so that if the winsys shim ever
 * declares it too, the two are compatible rather than conflicting.
 *
 * It always returns NULL. See gl_compat.c for why, and kb/design-gl.md for
 * what that means for the use_cva / cva_hack config options. */
void *SDL_GL_GetProcAddress(const char *proc);

/* --- Queries GLdc cannot answer ------------------------------------------ */
void  glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
void  glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params);
GLint gluScaleImage(GLenum format,
                    GLsizei wIn,  GLsizei hIn,  GLenum typeIn,  const void *dataIn,
                    GLsizei wOut, GLsizei hOut, GLenum typeOut, void *dataOut);

/* ==========================================================================
 * 3. Intercept wrappers.
 *
 * Declared here, defined in gl_compat.c, wired up by the macro block at the
 * bottom. The four immediate-mode ones are static inline with a fast path so
 * that the common case (no list recording, no texgen) costs one predictable
 * branch instead of a function call, which matters at per-vertex frequency.
 * ========================================================================== */

/* Nonzero only while a display list is being recorded or texgen is enabled.
 * Read on every immediate-mode vertex, so it lives in the header. */
extern int tr_gl_imm_hook;

void tr_gl_begin_hook(GLenum mode);
void tr_gl_end_hook(void);
void tr_gl_vertex3f_hook(GLfloat x, GLfloat y, GLfloat z);
void tr_gl_normal3f_hook(GLfloat x, GLfloat y, GLfloat z);
void tr_gl_texcoord2f_hook(GLfloat s, GLfloat t);

/* NOTE: the bodies below deliberately appear BEFORE the #define block, so the
 * glBegin/glVertex3f/... they name are the real GLdc symbols, not the macros.
 * Moving either piece past the other creates infinite recursion. */

static inline void tr_glBegin(GLenum mode) {
    if (__builtin_expect(tr_gl_imm_hook != 0, 0)) { tr_gl_begin_hook(mode); return; }
    glBegin(mode);
}

static inline void tr_glEnd(void) {
    if (__builtin_expect(tr_gl_imm_hook != 0, 0)) { tr_gl_end_hook(); return; }
    glEnd();
}

static inline void tr_glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    if (__builtin_expect(tr_gl_imm_hook != 0, 0)) { tr_gl_vertex3f_hook(x, y, z); return; }
    glVertex3f(x, y, z);
}

/* GLdc has glVertex2f, but texgen must see 2D vertices too (the HUD gauge
 * bars in src/hud.c are GL_QUADS of glVertex2f under object-linear texgen),
 * so it routes through the same hook with z = 0. */
static inline void tr_glVertex2f(GLfloat x, GLfloat y) {
    if (__builtin_expect(tr_gl_imm_hook != 0, 0)) { tr_gl_vertex3f_hook(x, y, 0.0f); return; }
    glVertex2f(x, y);
}

static inline void tr_glNormal3f(GLfloat x, GLfloat y, GLfloat z) {
    if (__builtin_expect(tr_gl_imm_hook != 0, 0)) { tr_gl_normal3f_hook(x, y, z); return; }
    glNormal3f(x, y, z);
}

static inline void tr_glTexCoord2f(GLfloat s, GLfloat t) {
    if (__builtin_expect(tr_gl_imm_hook != 0, 0)) { tr_gl_texcoord2f_hook(s, t); return; }
    glTexCoord2f(s, t);
}

/* Out-of-line intercepts: all cold (per-frame or per-object, not per-vertex). */
void tr_glEnable(GLenum cap);
void tr_glDisable(GLenum cap);
void tr_glClear(GLuint mask);
void tr_glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void tr_glGetIntegerv(GLenum pname, GLint *params);
void tr_glGetBooleanv(GLenum pname, GLboolean *params);
void tr_glBindTexture(GLenum target, GLuint texture);
void tr_glTexImage2D(GLenum target, GLint level, GLint internalFormat,
                     GLsizei width, GLsizei height, GLint border,
                     GLenum format, GLenum type, const GLvoid *data);
GLint tr_gluBuild2DMipmaps(GLenum target, GLint internalFormat,
                           GLsizei width, GLsizei height,
                           GLenum format, GLenum type, const void *data);
void tr_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void tr_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr);
void tr_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void tr_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void tr_glEnableClientState(GLenum cap);
void tr_glDisableClientState(GLenum cap);
void tr_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

#ifdef __cplusplus
}
#endif

/* ==========================================================================
 * 4. INTERCEPTS -- the macro block.
 *
 * Nothing here changes the meaning of a call except as noted. Each entry says
 * why it exists. gl_compat.c #undefs every one of these.
 * ========================================================================== */
#ifndef TR_GLCOMPAT_IMPL

/* Display-list recording + immediate-mode texgen need to see every vertex. */
#define glBegin              tr_glBegin
#define glEnd                tr_glEnd
#define glVertex2f           tr_glVertex2f
#define glVertex3f           tr_glVertex3f
#define glNormal3f           tr_glNormal3f
#define glTexCoord2f         tr_glTexCoord2f

/* GL_TEXTURE_GEN_S/T are unknown to GLdc; the wrapper swallows them (and
 * records the state) instead of letting GLdc raise GL_INVALID_ENUM. Every
 * other cap is passed through untouched. */
#define glEnable             tr_glEnable
#define glDisable            tr_glDisable

/* Masks off GL_STENCIL_BUFFER_BIT, which GLdc has no buffer for. Colour and
 * depth bits pass through unchanged. */
#define glClear              tr_glClear

/* GLdc's glGetIntegerv does not know GL_VIEWPORT (the enum is not even
 * defined in its gl.h), and src/screenshot.c:42 needs it. We shadow the
 * viewport ourselves. Also pins GL_MAX_TEXTURE_SIZE to the PVR2 limit. */
#define glViewport           tr_glViewport
#define glGetIntegerv        tr_glGetIntegerv
#define glGetBooleanv        tr_glGetBooleanv

/* GLdc's glGetTexParameteriv is a documented no-op stub, so the only way to
 * answer glGetTexLevelParameteriv(GL_TEXTURE_WIDTH/HEIGHT) is to record the
 * dimensions as they go in. */
#define glBindTexture        tr_glBindTexture
#define glTexImage2D         tr_glTexImage2D
#define gluBuild2DMipmaps    tr_gluBuild2DMipmaps

/* Texgen emulation for the vertex-array path needs the position and normal
 * sources; display-list replay and texgen injection both scribble on client
 * array state and must put it back exactly as the game left it. */
#define glVertexPointer      tr_glVertexPointer
#define glNormalPointer      tr_glNormalPointer
#define glTexCoordPointer    tr_glTexCoordPointer
#define glColorPointer       tr_glColorPointer
#define glEnableClientState  tr_glEnableClientState
#define glDisableClientState tr_glDisableClientState
#define glDrawElements       tr_glDrawElements

#endif /* !TR_GLCOMPAT_IMPL */

#endif /* TR_GLCOMPAT_H */
