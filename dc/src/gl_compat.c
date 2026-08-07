/*
 * gl_compat.c -- bodies for the GLdc / PowerVR2 compatibility shim.
 *
 * Read dc/include/tr_glcompat.h first: it explains the two mechanisms (pure
 * additions under real names vs. intercepts under macros) and lists every
 * intercept with its reason. kb/design-gl.md carries the measured API gap
 * table and the visual consequence of every no-op.
 *
 * ORDERING HAZARD: dc/Makefile force-includes tr_glcompat.h into every TU,
 * including this one, so by the time the first line below is reached the
 * intercept macros are already live and every wrapper would call itself. Two
 * things below stop that, and BOTH are needed:
 *
 *   - TR_GLCOMPAT_IMPL suppresses the macro block. This is what works when
 *     the file is compiled WITHOUT the force-include (the standalone
 *     sanity-compile in kb/design-gl.md does exactly that).
 *   - The #undef block AFTER the include tears the macros back down. This is
 *     what works when the file IS force-included, because by then the header
 *     has already been processed once and its include guard makes the
 *     #include below a no-op.
 *
 * The #undef list must stay in sync with the INTERCEPTS block in the header.
 */

#define TR_GLCOMPAT_IMPL 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* glGenerateMipmap lives in glext.h, not gl.h. dc/include/GL/glext.h shadows
 * the kos-ports header and #include_next-s it, so this gets both. */
#include <GL/glext.h>

#include "tr_glcompat.h"

#undef glBegin
#undef glEnd
#undef glVertex2f
#undef glVertex3f
#undef glNormal3f
#undef glTexCoord2f
#undef glEnable
#undef glDisable
#undef glClear
#undef glViewport
#undef glGetIntegerv
#undef glGetBooleanv
#undef glBindTexture
#undef glTexImage2D
#undef gluBuild2DMipmaps
#undef glVertexPointer
#undef glNormalPointer
#undef glTexCoordPointer
#undef glColorPointer
#undef glEnableClientState
#undef glDisableClientState
#undef glDrawElements
#undef glAlphaFunc

/* One-shot diagnostics. These fire on paths we believe are unreachable given
 * the call-site survey in kb/design-gl.md; if one ever prints, the survey was
 * wrong and the table needs revisiting. */
#define TR_WARN_ONCE(msg)                                            \
    do {                                                             \
        static int warned_ = 0;                                      \
        if (!warned_) {                                              \
            warned_ = 1;                                             \
            fprintf(stderr, "tr_glcompat: %s\n", (msg));             \
        }                                                            \
    } while (0)

/* Largest texture edge the port will upload. See the budget note in
 * tr_gluBuild2DMipmaps. -DTR_TEX_LIMIT=256 to try a sharper build; it will
 * run out of VRAM in a race unless something else has been freed first. */
#ifndef TR_TEX_LIMIT
#define TR_TEX_LIMIT 128
#endif

int tr_gl_imm_hook = 0;

unsigned tr_gl_n_end      = 0;
unsigned tr_gl_n_drawelem = 0;
unsigned tr_gl_n_calllist = 0;

/* ==========================================================================
 * Shadowed GL state
 *
 * GLdc cannot be queried for most of this (its glGetTexParameteriv and
 * friends are documented no-op stubs, and GL_VIEWPORT is not even a defined
 * enum in its gl.h), so we keep our own copy of exactly what we need.
 * ========================================================================== */

static GLint s_viewport[4] = { 0, 0, 640, 480 };

/* Client array shadow. Needed for two reasons: texgen emulation has to read
 * the position/normal sources itself, and both texgen injection and display
 * list replay temporarily repoint the arrays, so they must be able to put
 * back byte-for-byte what the game last specified. src/course_load.c:321-330
 * sets these up ONCE at course load and never again -- clobbering them
 * without restoring would blank the terrain for the rest of the run. */
typedef struct {
    GLint       size;
    GLenum      type;
    GLsizei     stride;
    const void *ptr;
    GLboolean   enabled;
} tr_array_t;

static tr_array_t s_arr_v = { 3, GL_FLOAT, 0, NULL, GL_FALSE };
static tr_array_t s_arr_n = { 3, GL_FLOAT, 0, NULL, GL_FALSE };
static tr_array_t s_arr_t = { 2, GL_FLOAT, 0, NULL, GL_FALSE };
static tr_array_t s_arr_c = { 4, GL_FLOAT, 0, NULL, GL_FALSE };

/* Texture coordinate generation. GLdc has none at all. */
static GLboolean s_tg_s      = GL_FALSE;
static GLboolean s_tg_t      = GL_FALSE;
static GLenum    s_tg_s_mode = GL_EYE_LINEAR;   /* GL default */
static GLenum    s_tg_t_mode = GL_EYE_LINEAR;
static GLfloat   s_tg_s_plane[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
static GLfloat   s_tg_t_plane[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
/* Bumped on any plane/mode change so the object-linear cache can tell that
 * its contents are stale without comparing eight floats per draw. */
static unsigned  s_tg_serial = 1;

static void tr_update_imm_hook(void);

/* ==========================================================================
 * Display lists
 *
 * src/hier_util.c:187-191 is the only glNewList site in the game and the only
 * thing it records is draw_sphere() (src/hier_util.c:46-152), which emits
 * nothing but glBegin / glNormal3f / glVertex3f / glEnd. That measurement is
 * what licenses this implementation to be a geometry recorder rather than a
 * general command buffer.
 *
 * Everything is flattened to GL_TRIANGLES at record time so a list replays as
 * a SINGLE glDrawArrays. The alternative -- preserving GL_TRIANGLE_FAN and
 * GL_TRIANGLE_STRIP batches -- would cost ~8 draw calls per sphere, and
 * traverse_dag draws one sphere per scene node (Tux is built entirely from
 * spheres), so that is ~240 submissions a frame against ~30. The 3x vertex
 * expansion is bought back many times over. GLdc's array path also refuses
 * GL_TRIANGLE_FAN outright (kos-ports GL/gl.h:662 "Only GL_TRIANGLES,
 * GL_TRIANGLE_STRIP, and GL_QUADS are supported"), so a fan would have needed
 * converting regardless.
 * ========================================================================== */

#define TR_MAX_LISTS 64

typedef struct {
    GLfloat  *pos;        /* 3 floats per vertex */
    GLfloat  *nrm;        /* 3 floats per vertex, NULL if the list had none */
    GLuint    nverts;
    GLuint    cap;
    GLboolean allocated;  /* name handed out by glGenLists */
    GLboolean compiled;   /* glEndList has run */
} tr_list_t;

static tr_list_t s_lists[TR_MAX_LISTS];
static GLuint    s_next_list = 1;      /* list name N maps to s_lists[N-1] */
static int       s_recording = -1;     /* index being recorded, -1 = none */

/* Scratch for the primitive currently between glBegin and glEnd. */
static GLenum   s_rec_mode  = GL_TRIANGLES;
static GLfloat *s_scr_pos   = NULL;
static GLfloat *s_scr_nrm   = NULL;
static GLuint   s_scr_n     = 0;
static GLuint   s_scr_cap   = 0;
static GLfloat  s_cur_nrm[3] = { 0.0f, 0.0f, 1.0f };
static GLboolean s_saw_nrm  = GL_FALSE;

static int tr_scr_reserve(GLuint need)
{
    GLuint cap;
    GLfloat *p, *n;

    if (need <= s_scr_cap) {
        return 1;
    }
    cap = s_scr_cap ? s_scr_cap : 64;
    while (cap < need) {
        cap *= 2;
    }
    p = (GLfloat *) realloc(s_scr_pos, cap * 3 * sizeof(GLfloat));
    n = (GLfloat *) realloc(s_scr_nrm, cap * 3 * sizeof(GLfloat));
    if (!p || !n) {
        /* Keep whatever realloc did give us; dropping the list is better than
         * dying, and the caller checks the return. */
        if (p) s_scr_pos = p;
        if (n) s_scr_nrm = n;
        TR_WARN_ONCE("out of memory recording a display list");
        return 0;
    }
    s_scr_pos = p;
    s_scr_nrm = n;
    s_scr_cap = cap;
    return 1;
}

static int tr_list_reserve(tr_list_t *l, GLuint need)
{
    GLuint cap;
    GLfloat *p, *n;

    if (need <= l->cap) {
        return 1;
    }
    cap = l->cap ? l->cap : 256;
    while (cap < need) {
        cap *= 2;
    }
    p = (GLfloat *) realloc(l->pos, cap * 3 * sizeof(GLfloat));
    if (!p) {
        TR_WARN_ONCE("out of memory building a display list");
        return 0;
    }
    l->pos = p;
    n = (GLfloat *) realloc(l->nrm, cap * 3 * sizeof(GLfloat));
    if (!n) {
        TR_WARN_ONCE("out of memory building a display list");
        return 0;
    }
    l->nrm = n;
    l->cap = cap;
    return 1;
}

static void tr_list_push(tr_list_t *l, GLuint i)
{
    GLuint d = l->nverts;

    if (!tr_list_reserve(l, d + 1)) {
        return;
    }
    l->pos[d * 3 + 0] = s_scr_pos[i * 3 + 0];
    l->pos[d * 3 + 1] = s_scr_pos[i * 3 + 1];
    l->pos[d * 3 + 2] = s_scr_pos[i * 3 + 2];
    l->nrm[d * 3 + 0] = s_scr_nrm[i * 3 + 0];
    l->nrm[d * 3 + 1] = s_scr_nrm[i * 3 + 1];
    l->nrm[d * 3 + 2] = s_scr_nrm[i * 3 + 2];
    l->nverts = d + 1;
}

static void tr_list_tri(tr_list_t *l, GLuint a, GLuint b, GLuint c)
{
    tr_list_push(l, a);
    tr_list_push(l, b);
    tr_list_push(l, c);
}

/* Flatten the scratch primitive into the list as GL_TRIANGLES. */
static void tr_list_flush_prim(tr_list_t *l)
{
    GLuint i;

    switch (s_rec_mode) {
    case GL_TRIANGLES:
        for (i = 0; i + 2 < s_scr_n; i += 3) {
            tr_list_tri(l, i, i + 1, i + 2);
        }
        break;

    case GL_TRIANGLE_STRIP:
        /* Odd triangles have reversed winding in a strip; swap two indices so
         * the flattened triangles all face the same way as the original. */
        for (i = 0; i + 2 < s_scr_n; i++) {
            if (i & 1) {
                tr_list_tri(l, i, i + 2, i + 1);
            } else {
                tr_list_tri(l, i, i + 1, i + 2);
            }
        }
        break;

    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        for (i = 1; i + 1 < s_scr_n; i++) {
            tr_list_tri(l, 0, i, i + 1);
        }
        break;

    case GL_QUADS:
        for (i = 0; i + 3 < s_scr_n; i += 4) {
            tr_list_tri(l, i, i + 1, i + 2);
            tr_list_tri(l, i, i + 2, i + 3);
        }
        break;

    default:
        /* Lines and points cannot be expressed as triangles. Nothing in the
         * game records them (kb/design-gl.md), so this is a tripwire. */
        TR_WARN_ONCE("display list recorded a non-triangulable primitive; dropped");
        break;
    }
    s_scr_n = 0;
}

GLuint glGenLists(GLsizei range)
{
    GLuint base, i;

    if (range <= 0) {
        return 0;
    }
    if (s_next_list - 1 + (GLuint) range > TR_MAX_LISTS) {
        TR_WARN_ONCE("display list pool exhausted (raise TR_MAX_LISTS)");
        return 0;
    }
    base = s_next_list;
    for (i = 0; i < (GLuint) range; i++) {
        tr_list_t *l = &s_lists[base - 1 + i];
        memset(l, 0, sizeof(*l));
        l->allocated = GL_TRUE;
    }
    s_next_list += (GLuint) range;
    return base;
}

GLboolean glIsList(GLuint list)
{
    if (list == 0 || list > TR_MAX_LISTS) {
        return GL_FALSE;
    }
    return s_lists[list - 1].allocated;
}

void glNewList(GLuint list, GLenum mode)
{
    tr_list_t *l;

    if (list == 0 || list > TR_MAX_LISTS || !s_lists[list - 1].allocated) {
        TR_WARN_ONCE("glNewList on an unallocated list name");
        return;
    }
    if (mode == GL_COMPILE_AND_EXECUTE) {
        /* Would need every recorded call forwarded to GLdc as well. The game
         * only ever uses GL_COMPILE (src/hier_util.c:188). */
        TR_WARN_ONCE("GL_COMPILE_AND_EXECUTE is not supported; recording only");
    }

    l = &s_lists[list - 1];
    l->nverts   = 0;
    l->compiled = GL_FALSE;
    s_recording = (int) (list - 1);
    s_scr_n     = 0;
    s_saw_nrm   = GL_FALSE;
    tr_update_imm_hook();
}

void glEndList(void)
{
    tr_list_t *l;

    if (s_recording < 0) {
        return;
    }
    l = &s_lists[s_recording];
    if (s_scr_n > 0) {
        /* glEnd was never called -- flush what we have rather than lose it. */
        tr_list_flush_prim(l);
    }
    l->compiled = GL_TRUE;
    if (!s_saw_nrm && l->nrm) {
        /* No normals were recorded, so do not enable the normal array on
         * replay: GLdc would light the geometry with whatever stale contents
         * the buffer has. Freeing it also makes the intent explicit. */
        free(l->nrm);
        l->nrm = NULL;
    }
    s_recording = -1;
    tr_update_imm_hook();
}

void glDeleteLists(GLuint list, GLsizei range)
{
    GLsizei i;

    for (i = 0; i < range; i++) {
        GLuint n = list + (GLuint) i;
        if (n == 0 || n > TR_MAX_LISTS) {
            continue;
        }
        free(s_lists[n - 1].pos);
        free(s_lists[n - 1].nrm);
        memset(&s_lists[n - 1], 0, sizeof(tr_list_t));
    }
}

/* Put back exactly the client array state the game last asked for. Only the
 * pointers we actually touched are re-issued; see s_arr_* above for why this
 * cannot be skipped. */
static void tr_restore_arrays(GLboolean vtx, GLboolean nrm,
                              GLboolean tex, GLboolean col)
{
    if (vtx) {
        if (s_arr_v.ptr) {
            glVertexPointer(s_arr_v.size, s_arr_v.type, s_arr_v.stride, s_arr_v.ptr);
        }
        if (s_arr_v.enabled) glEnableClientState(GL_VERTEX_ARRAY);
        else                 glDisableClientState(GL_VERTEX_ARRAY);
    }
    if (nrm) {
        if (s_arr_n.ptr) {
            glNormalPointer(s_arr_n.type, s_arr_n.stride, s_arr_n.ptr);
        }
        if (s_arr_n.enabled) glEnableClientState(GL_NORMAL_ARRAY);
        else                 glDisableClientState(GL_NORMAL_ARRAY);
    }
    if (tex) {
        if (s_arr_t.ptr) {
            glTexCoordPointer(s_arr_t.size, s_arr_t.type, s_arr_t.stride, s_arr_t.ptr);
        }
        if (s_arr_t.enabled) glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        else                 glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    if (col) {
        if (s_arr_c.ptr) {
            glColorPointer(s_arr_c.size, s_arr_c.type, s_arr_c.stride, s_arr_c.ptr);
        }
        if (s_arr_c.enabled) glEnableClientState(GL_COLOR_ARRAY);
        else                 glDisableClientState(GL_COLOR_ARRAY);
    }
}

void glCallList(GLuint list)
{
    tr_gl_n_calllist++;
    tr_list_t *l;

    if (list == 0 || list > TR_MAX_LISTS) {
        return;
    }
    l = &s_lists[list - 1];
    if (!l->compiled || l->nverts == 0) {
        return;
    }

    glVertexPointer(3, GL_FLOAT, 0, l->pos);
    glEnableClientState(GL_VERTEX_ARRAY);

    if (l->nrm) {
        glNormalPointer(GL_FLOAT, 0, l->nrm);
        glEnableClientState(GL_NORMAL_ARRAY);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
    }

    /* The course leaves GL_COLOR_ARRAY enabled for the whole run
     * (src/course_load.c:328). If we left it on, GLdc would read Tux's
     * vertices out of our arrays but his colours out of the terrain's,
     * producing garbage. Same reasoning for the texcoord array. */
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei) l->nverts);

    tr_restore_arrays(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* ==========================================================================
 * Texture coordinate generation
 * ========================================================================== */

void glTexGeni(GLenum coord, GLenum pname, GLint param)
{
    if (pname != GL_TEXTURE_GEN_MODE) {
        return;
    }
    if (coord == GL_S) {
        s_tg_s_mode = (GLenum) param;
    } else if (coord == GL_T) {
        s_tg_t_mode = (GLenum) param;
    }
    s_tg_serial++;
}

void glTexGenf(GLenum coord, GLenum pname, GLfloat param)
{
    glTexGeni(coord, pname, (GLint) param);
}

void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params)
{
    GLfloat *dst;

    if (pname == GL_TEXTURE_GEN_MODE) {
        glTexGeni(coord, pname, (GLint) params[0]);
        return;
    }
    /* GL_OBJECT_PLANE is stored verbatim (it is applied in object space).
     * GL_EYE_PLANE should be pre-multiplied by the inverse modelview at
     * specification time; the game never uses it, so we treat the two the
     * same and note the divergence rather than silently pretending. */
    if (pname != GL_OBJECT_PLANE && pname != GL_EYE_PLANE) {
        return;
    }
    if (pname == GL_EYE_PLANE) {
        TR_WARN_ONCE("GL_EYE_PLANE treated as GL_OBJECT_PLANE (no inverse modelview)");
    }

    if (coord == GL_S) {
        dst = s_tg_s_plane;
    } else if (coord == GL_T) {
        dst = s_tg_t_plane;
    } else {
        return;
    }
    dst[0] = params[0];
    dst[1] = params[1];
    dst[2] = params[2];
    dst[3] = params[3];
    s_tg_serial++;
}

void glTexGendv(GLenum coord, GLenum pname, const GLdouble *params)
{
    GLfloat f[4];
    f[0] = (GLfloat) params[0];
    f[1] = (GLfloat) params[1];
    f[2] = (GLfloat) params[2];
    f[3] = (GLfloat) params[3];
    glTexGenfv(coord, pname, f);
}

static int tr_texgen_active(void)
{
    return (s_tg_s || s_tg_t);
}

static void tr_objlinear(GLfloat x, GLfloat y, GLfloat z, GLfloat *s, GLfloat *t)
{
    *s = s_tg_s_plane[0] * x + s_tg_s_plane[1] * y
       + s_tg_s_plane[2] * z + s_tg_s_plane[3];
    *t = s_tg_t_plane[0] * x + s_tg_t_plane[1] * y
       + s_tg_t_plane[2] * z + s_tg_t_plane[3];
}

/* ==========================================================================
 * Immediate-mode hooks
 *
 * Reached only when tr_gl_imm_hook is set, i.e. while a list is recording or
 * texgen is on. Everything else takes the inline fast path in the header.
 * ========================================================================== */

void tr_gl_begin_hook(GLenum mode)
{
    if (s_recording >= 0) {
        s_rec_mode = mode;
        s_scr_n    = 0;
        return;
    }
    glBegin(mode);
}

void tr_gl_end_hook(void)
{
    if (s_recording >= 0) {
        tr_list_flush_prim(&s_lists[s_recording]);
        return;
    }
    glEnd();
}

void tr_gl_normal3f_hook(GLfloat x, GLfloat y, GLfloat z)
{
    if (s_recording >= 0) {
        s_cur_nrm[0] = x;
        s_cur_nrm[1] = y;
        s_cur_nrm[2] = z;
        s_saw_nrm    = GL_TRUE;
        return;
    }
    glNormal3f(x, y, z);
}

void tr_gl_texcoord2f_hook(GLfloat s, GLfloat t)
{
    if (s_recording >= 0) {
        /* draw_sphere() emits no texture coordinates, so a list containing
         * them means the survey in kb/design-gl.md is out of date. */
        TR_WARN_ONCE("texture coords inside a display list are not recorded");
        return;
    }
    glTexCoord2f(s, t);
}

void tr_gl_vertex3f_hook(GLfloat x, GLfloat y, GLfloat z)
{
    if (s_recording >= 0) {
        GLuint i = s_scr_n;
        if (!tr_scr_reserve(i + 1)) {
            return;
        }
        s_scr_pos[i * 3 + 0] = x;
        s_scr_pos[i * 3 + 1] = y;
        s_scr_pos[i * 3 + 2] = z;
        s_scr_nrm[i * 3 + 0] = s_cur_nrm[0];
        s_scr_nrm[i * 3 + 1] = s_cur_nrm[1];
        s_scr_nrm[i * 3 + 2] = s_cur_nrm[2];
        s_scr_n = i + 1;
        return;
    }

    if (tr_texgen_active()) {
        GLfloat s, t;
        if (s_tg_s_mode == GL_SPHERE_MAP || s_tg_t_mode == GL_SPHERE_MAP) {
            /* No immediate-mode call site uses sphere mapping (only
             * quadsquare::DrawEnvmapTris does, and that is an array draw), so
             * falling back to the plane equations here is strictly a
             * tripwire, not a rendering path. */
            TR_WARN_ONCE("GL_SPHERE_MAP in immediate mode; using object-linear");
        }
        tr_objlinear(x, y, z, &s, &t);
        glTexCoord2f(s, t);
    }
    glVertex3f(x, y, z);
}

/* ==========================================================================
 * Enable / disable
 * ========================================================================== */

static void tr_update_imm_hook(void)
{
    tr_gl_imm_hook = (s_recording >= 0) || s_tg_s || s_tg_t;
}

/*
 * GL_STENCIL_TEST: PowerVR2 has no stencil buffer and GLdc's glEnable/glDisable
 * switch has no case for it, so it falls through to
 * _glKosThrowError(GL_INVALID_VALUE) (GLdc a1cd80a8, GL/state.c). src/ calls
 * glDisable(GL_STENCIL_TEST) 15 times, several of them per frame, and
 * src/gl_util.c checks glGetError() every frame — so the unswallowed form
 * produced an unbounded "GL ERROR: GL_INVALID_VALUE when calling glDisable"
 * stream on the console, which at harness baud is a real per-frame cost on top
 * of being noise.
 *
 * Swallowing it is not a behaviour change: USE_STENCIL_BUFFER is not defined
 * for this build, so upstream's own no-stencil shadow path is what runs
 * (src/tux_shadow.c:35,39).
 */
void tr_glEnable(GLenum cap)
{
#ifdef TR_TEXGEN_DISABLE
    /* Bisection switch, not a feature: builds with texgen emulation off so a
     * rendering artefact can be attributed to it or cleared of it in one run.
     * The course terrain loses its texture entirely in such a build. */
    if (cap == GL_TEXTURE_GEN_S || cap == GL_TEXTURE_GEN_T) return;
#endif
    if (cap == GL_TEXTURE_GEN_S) { s_tg_s = GL_TRUE; tr_update_imm_hook(); return; }
    if (cap == GL_TEXTURE_GEN_T) { s_tg_t = GL_TRUE; tr_update_imm_hook(); return; }
    if (cap == GL_STENCIL_TEST) return;
    glEnable(cap);
}

void tr_glDisable(GLenum cap)
{
    if (cap == GL_TEXTURE_GEN_S) { s_tg_s = GL_FALSE; tr_update_imm_hook(); return; }
    if (cap == GL_TEXTURE_GEN_T) { s_tg_t = GL_FALSE; tr_update_imm_hook(); return; }
    if (cap == GL_STENCIL_TEST) return;
    glDisable(cap);
}

/* ==========================================================================
 * Clear / viewport / queries
 * ========================================================================== */

void tr_glClear(GLuint mask)
{
    /* PowerVR2 has no stencil buffer. Strip the bit rather than hand GLdc a
     * mask it does not recognise. Colour and depth pass through untouched. */
    glClear(mask & ~(GLuint) GL_STENCIL_BUFFER_BIT);
}

void glClearStencil(GLint s)
{
    /* No stencil buffer on PowerVR2. See kb/design-gl.md: USE_STENCIL_BUFFER
     * is not defined for this build, so upstream's own no-stencil shadow path
     * is what runs and nothing depends on this value. */
    (void) s;
}

void tr_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    s_viewport[0] = x;
    s_viewport[1] = y;
    s_viewport[2] = (GLint) w;
    s_viewport[3] = (GLint) h;
    glViewport(x, y, w, h);
}

/*
 * GLdc's glAlphaFunc validates against a list containing exactly one entry,
 * GL_GREATER (GLdc a1cd80a8, GL/state.c), and throws GL_INVALID_ENUM for
 * anything else. src/gl_util.c:185,204,343 asks for GL_GEQUAL, so on the stock
 * path the cutoff was NEVER SET and every alpha-tested billboard -- the trees,
 * the particles -- blended instead of punching through, sorting wrong against
 * the terrain.
 *
 * It also cost 1,946 console lines in a single 240 s run (MEASURED), because
 * src/gl_util.c checks glGetError() every frame and prints what it finds.
 *
 * GL_GEQUAL(x) and GL_GREATER(x) differ only for a fragment whose alpha is
 * exactly the reference value. At ref 0.5 on 8-bit alpha that is one value out
 * of 256, on assets that are almost entirely 0 or 255. Mapping one to the
 * other is the whole fix.
 */
void tr_glAlphaFunc(GLenum func, GLclampf ref)
{
    if (func == GL_GEQUAL) {
        func = GL_GREATER;
    }
    glAlphaFunc(func, ref);
}

void tr_glGetIntegerv(GLenum pname, GLint *params)
{
    if (pname == GL_VIEWPORT) {
        /* GLdc does not implement this query -- GL_VIEWPORT is not even a
         * defined enum in its gl.h -- and src/screenshot.c:42 needs it. */
        params[0] = s_viewport[0];
        params[1] = s_viewport[1];
        params[2] = s_viewport[2];
        params[3] = s_viewport[3];
        return;
    }

    if (pname == GL_MAX_TEXTURE_SIZE) {
        /* src/textures.c:130 uses this to decide whether to downscale, and an
         * unanswered query would leave the variable uninitialised. Ask GLdc
         * first, then clamp to the PVR2 hardware maximum of 1024 whether it
         * answered or not. */
        params[0] = 0;
        glGetIntegerv(pname, params);
        if (params[0] <= 0 || params[0] > 1024) {
            params[0] = 1024;
        }
        return;
    }

    glGetIntegerv(pname, params);
}

void tr_glGetBooleanv(GLenum pname, GLboolean *params)
{
    if (pname == GL_DOUBLEBUFFER) {
        /* GLdc always double-buffers via glKosSwapBuffers. Only consumed by
         * print_gl_info (src/gl_util.c:456). */
        params[0] = GL_TRUE;
        return;
    }
    glGetBooleanv(pname, params);
}

/* ==========================================================================
 * Texture dimension tracking
 *
 * GLdc's glGetTexParameteriv is listed under "Non Operational Stubs for
 * portability" in kos-ports GL/gl.h:758, and there is no
 * glGetTexLevelParameteriv at all. src/textures.c:241 and
 * src/splash_screen.c:98 both need the bound texture's size to lay out
 * geometry, so we record it on the way in.
 * ========================================================================== */

typedef struct { GLushort w, h; } tr_texdim_t;

static tr_texdim_t *s_texdim     = NULL;
static GLuint       s_texdim_cap = 0;
static GLuint       s_bound_tex  = 0;

static void tr_texdim_set(GLuint name, GLsizei w, GLsizei h)
{
    if (name == 0) {
        return;
    }
    if (name >= s_texdim_cap) {
        GLuint cap = s_texdim_cap ? s_texdim_cap : 64;
        tr_texdim_t *p;
        while (cap <= name) {
            cap *= 2;
        }
        p = (tr_texdim_t *) realloc(s_texdim, cap * sizeof(tr_texdim_t));
        if (!p) {
            TR_WARN_ONCE("out of memory tracking texture dimensions");
            return;
        }
        memset(p + s_texdim_cap, 0, (cap - s_texdim_cap) * sizeof(tr_texdim_t));
        s_texdim     = p;
        s_texdim_cap = cap;
    }
    s_texdim[name].w = (GLushort) w;
    s_texdim[name].h = (GLushort) h;
}

void tr_glBindTexture(GLenum target, GLuint texture)
{
    if (target == GL_TEXTURE_2D) {
        s_bound_tex = texture;
    }
    glBindTexture(target, texture);
}

void tr_glTexImage2D(GLenum target, GLint level, GLint internalFormat,
                     GLsizei width, GLsizei height, GLint border,
                     GLenum format, GLenum type, const GLvoid *data)
{
    if (target == GL_TEXTURE_2D && level == 0) {
        tr_texdim_set(s_bound_tex, width, height);
    }
    glTexImage2D(target, level, internalFormat, width, height, border,
                 format, type, data);
}

GLint tr_gluBuild2DMipmaps(GLenum target, GLint internalFormat,
                           GLsizei width, GLsizei height,
                           GLenum format, GLenum type, const void *data)
{
    /* Intercepted as well as glTexImage2D because GLU calls glTexImage2D
     * internally, inside libGL, where our macro cannot reach it. This is the
     * path src/textures.c:160 actually takes for every game texture. */
    if (target == GL_TEXTURE_2D) {
        tr_texdim_set(s_bound_tex, width, height);
    }

    /*
     * GLdc's own gluBuild2DMipmaps THROWS THE CALLER'S FORMAT AWAY
     * (GLdc a1cd80a8, GL/glu.c):
     *
     *     glTexImage2D(GL_TEXTURE_2D, 0, 3, width, height, 0,
     *                  GL_RGB, GL_UNSIGNED_BYTE, data);
     *     glGenerateMipmap(GL_TEXTURE_2D);
     *
     * -- internalFormat, format and type are all hardcoded. src/textures.c:160
     * uploads RGBA for every texture whose SGI RGB file has 4 channels, which
     * is most of them, so GLdc read 4-byte pixels as 3-byte ones. Every
     * subsequent pixel in the row is shifted by one more byte, which is
     * EXACTLY the failure that was on screen: the splash logo repeated ~5 times
     * across its 512 px quad, in magenta/green vertical stripes, while the
     * untextured background and snow particles beside it were perfect.
     *
     * So: never delegate to GLdc's GLU here. Upload with the real format.
     *
     * The mip chain is then ours to ask for, and only when GLdc will accept
     * it: glGenerateMipmap throws GL_INVALID_OPERATION with "Mipmaps cannot be
     * supported on non-square textures" (also MEASURED). For those, pin a
     * non-mipmap filter, because GLU's default leaves a mipmapping min filter
     * with no chain behind it and samples garbage. The cost is aliasing when
     * minified.
     */
    /*
     * THE VRAM BUDGET.
     *
     * MEASURED over the 113 .rgb files the game ships, as 16 bpp PVR textures
     * with mip chains: 13.03 MB with no limit, 4.15 MB with a 128 px limit.
     * The Dreamcast has 8 MB of VRAM TOTAL, and the two framebuffers
     * (1,228,800 B) and GLdc's PVR vertex buffer (655,360 B, GL/platforms/
     * sh4.c:9) come out of the same 8 MB, leaving ~6.1 MB. A single race needs
     * courses/common (5.60 MB) + textures/ (1.63 MB) + fonts (0.33 MB) + the
     * course itself, so unlimited does not fit and it does not fail politely:
     * the run reaches the racing view and then dies with "Ran out of memory,
     * defragmenting" and a hard assert at GL/texture.c:164.
     *
     * The obvious lever is upstream's own: src/textures.c:130 asks for
     * GL_MAX_TEXTURE_SIZE and squares anything larger down to it. DO NOT USE
     * IT. src/textures.c:156 also rewrites texImage->sizeX/sizeY to the cap,
     * and the UI lays itself out from those numbers -- reporting 128 renders
     * the entire menu, and the splash logo, at half size. That was tried, and
     * the screenshot is why this is here instead.
     *
     * Downscaling inside the upload is invisible to the game: texture
     * coordinates are normalised, so nothing above this line can tell.
     */
    if (target == GL_TEXTURE_2D &&
        (width > TR_TEX_LIMIT || height > TR_TEX_LIMIT) &&
        type == GL_UNSIGNED_BYTE &&
        (format == GL_RGB || format == GL_RGBA)) {
        GLsizei nc = (format == GL_RGBA) ? 4 : 3;
        GLsizei sw = width, sh = height;
        const GLubyte *src = (const GLubyte *) data;
        GLubyte *own = NULL;

        while ((sw > TR_TEX_LIMIT || sh > TR_TEX_LIMIT) && sw > 1 && sh > 1) {
            GLsizei dw = (sw > 1) ? sw / 2 : 1;
            GLsizei dh = (sh > 1) ? sh / 2 : 1;
            GLubyte *dst = (GLubyte *) malloc((size_t) dw * dh * nc);
            GLsizei x, y, c;

            if (dst == NULL) {
                TR_WARN_ONCE("out of RAM downscaling a texture");
                break;
            }

            /* 2x2 box filter. Point sampling would alias the tree and HUD
             * artwork badly, and these are one-off loads, not a hot path. */
            for (y = 0; y < dh; y++) {
                for (x = 0; x < dw; x++) {
                    for (c = 0; c < nc; c++) {
                        const GLubyte *a = src + (((y * 2) * sw) + x * 2) * nc + c;
                        const GLubyte *b = a + nc;
                        const GLubyte *d = a + sw * nc;
                        const GLubyte *e = d + nc;
                        dst[(y * dw + x) * nc + c] =
                            (GLubyte) (((unsigned) *a + *b + *d + *e) >> 2);
                    }
                }
            }

            free(own);
            own = dst;
            src = dst;
            sw = dw;
            sh = dh;
        }

        glTexImage2D(target, 0, internalFormat, sw, sh, 0, format, type, src);
        if (sw == sh) {
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        else {
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        free(own);
        return 0;
    }

    if (target == GL_TEXTURE_2D) {
        glTexImage2D(target, 0, internalFormat, width, height, 0,
                     format, type, data);

        if (width == height) {
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        else {
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        return 0;
    }

    return gluBuild2DMipmaps(target, internalFormat, width, height,
                             format, type, data);
}

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
    GLint w = 0, h = 0;

    if (target == GL_TEXTURE_2D && s_bound_tex < s_texdim_cap && s_texdim) {
        w = s_texdim[s_bound_tex].w;
        h = s_texdim[s_bound_tex].h;
    }
    /* Levels beyond 0 are the mipmap chain; halve and clamp, which is what
     * gluBuild2DMipmaps produced. Nothing in the game asks for level > 0. */
    for (; level > 0; level--) {
        w = (w > 1) ? w / 2 : 1;
        h = (h > 1) ? h / 2 : 1;
    }

    switch (pname) {
    case GL_TEXTURE_WIDTH:  *params = w; break;
    case GL_TEXTURE_HEIGHT: *params = h; break;
    default:                *params = 0; break;
    }
}

void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params)
{
    GLint i = 0;
    glGetTexLevelParameteriv(target, level, pname, &i);
    *params = (GLfloat) i;
}

/* ==========================================================================
 * Client array intercepts
 * ========================================================================== */

void tr_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    s_arr_v.size = size; s_arr_v.type = type; s_arr_v.stride = stride; s_arr_v.ptr = ptr;
    glVertexPointer(size, type, stride, ptr);
}

void tr_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr)
{
    s_arr_n.type = type; s_arr_n.stride = stride; s_arr_n.ptr = ptr;
    glNormalPointer(type, stride, ptr);
}

void tr_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    s_arr_t.size = size; s_arr_t.type = type; s_arr_t.stride = stride; s_arr_t.ptr = ptr;
    glTexCoordPointer(size, type, stride, ptr);
}

void tr_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    s_arr_c.size = size; s_arr_c.type = type; s_arr_c.stride = stride; s_arr_c.ptr = ptr;
    glColorPointer(size, type, stride, ptr);
}

void tr_glEnableClientState(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY:        s_arr_v.enabled = GL_TRUE; break;
    case GL_NORMAL_ARRAY:        s_arr_n.enabled = GL_TRUE; break;
    case GL_TEXTURE_COORD_ARRAY: s_arr_t.enabled = GL_TRUE; break;
    case GL_COLOR_ARRAY:         s_arr_c.enabled = GL_TRUE; break;
    default: break;
    }
    glEnableClientState(cap);
}

void tr_glDisableClientState(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY:        s_arr_v.enabled = GL_FALSE; break;
    case GL_NORMAL_ARRAY:        s_arr_n.enabled = GL_FALSE; break;
    case GL_TEXTURE_COORD_ARRAY: s_arr_t.enabled = GL_FALSE; break;
    case GL_COLOR_ARRAY:         s_arr_c.enabled = GL_FALSE; break;
    default: break;
    }
    glDisableClientState(cap);
}

/* ==========================================================================
 * Vertex-array texgen
 *
 * The whole course terrain is textured by GL_OBJECT_LINEAR texgen
 * (src/course_render.c:274-280, enabled at src/gl_util.c:158-159) and drawn
 * through glDrawElements with NO texture coordinate array
 * (src/course_load.c:321-330, src/quadtree.cpp:1013). Without emulation the
 * terrain renders with whatever the last glTexCoord2f left behind -- one
 * texel stretched over the entire course. This is the single highest-value
 * item in the shim.
 *
 * Object-linear coordinates depend only on the vertex position, so they are
 * generated once per (vertex pointer, plane) configuration into a cache that
 * grows to the highest index seen and is then reused for the rest of the run.
 * Sphere-map coordinates depend on the modelview and must be regenerated
 * every draw, but only for the indices that draw actually references.
 * ========================================================================== */

static GLfloat *s_tc_lin      = NULL;   /* object-linear cache, 2 floats/vertex */
static GLuint   s_tc_lin_cap  = 0;      /* vertices allocated */
static GLuint   s_tc_lin_gen  = 0;      /* vertices already generated */
static unsigned s_tc_lin_ser  = 0;      /* s_tg_serial the cache was built at */
static const void *s_tc_lin_src = NULL; /* vertex pointer it was built from */

static GLfloat *s_tc_sph      = NULL;   /* sphere-map scratch, 2 floats/vertex */
static GLuint   s_tc_sph_cap  = 0;

static GLfloat *tr_tc_reserve(GLfloat **buf, GLuint *cap, GLuint need)
{
    if (need > *cap) {
        GLuint c = *cap ? *cap : 1024;
        GLfloat *p;
        while (c < need) {
            c *= 2;
        }
        p = (GLfloat *) realloc(*buf, (size_t) c * 2 * sizeof(GLfloat));
        if (!p) {
            TR_WARN_ONCE("out of memory generating texture coordinates");
            return NULL;
        }
        *buf = p;
        *cap = c;
    }
    return *buf;
}

/* Read element `idx` of a shadowed float array. The game only ever specifies
 * GL_FLOAT for positions and normals (src/course_load.c:322,325). */
static int tr_fetch3f(const tr_array_t *a, GLuint idx, GLfloat *out)
{
    const unsigned char *base;
    const GLfloat *f;
    GLsizei stride;
    GLint n;

    if (!a->ptr || a->type != GL_FLOAT) {
        return 0;
    }
    n      = (a->size > 3) ? 3 : a->size;
    stride = a->stride ? a->stride : (GLsizei) (n * (GLint) sizeof(GLfloat));
    base   = (const unsigned char *) a->ptr + (size_t) idx * (size_t) stride;
    f      = (const GLfloat *) (const void *) base;

    out[0] = f[0];
    out[1] = (n > 1) ? f[1] : 0.0f;
    out[2] = (n > 2) ? f[2] : 0.0f;
    return 1;
}

static GLuint tr_index_at(const void *indices, GLenum type, GLsizei i)
{
    switch (type) {
    case GL_UNSIGNED_INT:   return ((const GLuint   *) indices)[i];
    case GL_UNSIGNED_SHORT: return ((const GLushort *) indices)[i];
    case GL_UNSIGNED_BYTE:  return ((const GLubyte  *) indices)[i];
    default:                return 0;
    }
}

static void tr_gen_spheremap(GLuint idx, const GLfloat *mv, GLfloat *dst)
{
    GLfloat p[3], n[3], ve[3], ne[3], u[3], r[3];
    GLfloat len, d, m;

    if (!tr_fetch3f(&s_arr_v, idx, p) || !tr_fetch3f(&s_arr_n, idx, n)) {
        dst[0] = dst[1] = 0.0f;
        return;
    }

    /* Eye-space position and normal. The normal uses the modelview's upper
     * 3x3 rather than its inverse transpose: the game's modelview is
     * rotation + translation only, where the two agree. */
    ve[0] = mv[0] * p[0] + mv[4] * p[1] + mv[8]  * p[2] + mv[12];
    ve[1] = mv[1] * p[0] + mv[5] * p[1] + mv[9]  * p[2] + mv[13];
    ve[2] = mv[2] * p[0] + mv[6] * p[1] + mv[10] * p[2] + mv[14];

    ne[0] = mv[0] * n[0] + mv[4] * n[1] + mv[8]  * n[2];
    ne[1] = mv[1] * n[0] + mv[5] * n[1] + mv[9]  * n[2];
    ne[2] = mv[2] * n[0] + mv[6] * n[1] + mv[10] * n[2];

    len = sqrtf(ve[0] * ve[0] + ve[1] * ve[1] + ve[2] * ve[2]);
    if (len < 1e-8f) {
        dst[0] = dst[1] = 0.5f;
        return;
    }
    u[0] = ve[0] / len; u[1] = ve[1] / len; u[2] = ve[2] / len;

    len = sqrtf(ne[0] * ne[0] + ne[1] * ne[1] + ne[2] * ne[2]);
    if (len < 1e-8f) {
        dst[0] = dst[1] = 0.5f;
        return;
    }
    ne[0] /= len; ne[1] /= len; ne[2] /= len;

    d = 2.0f * (ne[0] * u[0] + ne[1] * u[1] + ne[2] * u[2]);
    r[0] = u[0] - ne[0] * d;
    r[1] = u[1] - ne[1] * d;
    r[2] = u[2] - ne[2] * d;

    m = 2.0f * sqrtf(r[0] * r[0] + r[1] * r[1] +
                     (r[2] + 1.0f) * (r[2] + 1.0f));
    if (m < 1e-8f) {
        dst[0] = dst[1] = 0.5f;
        return;
    }
    dst[0] = r[0] / m + 0.5f;
    dst[1] = r[1] / m + 0.5f;
}

void tr_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    GLuint maxidx = 0;
    tr_gl_n_drawelem++;
    GLsizei i;
    GLfloat *tc;
    int spheremap;

    if (count <= 0) {
        return;
    }
    if (!tr_texgen_active() || !s_arr_v.enabled || !s_arr_v.ptr || !indices) {
        glDrawElements(mode, count, type, indices);
        return;
    }
    if (type != GL_UNSIGNED_INT && type != GL_UNSIGNED_SHORT &&
        type != GL_UNSIGNED_BYTE) {
        TR_WARN_ONCE("glDrawElements with an unsupported index type");
        glDrawElements(mode, count, type, indices);
        return;
    }

    for (i = 0; i < count; i++) {
        GLuint v = tr_index_at(indices, type, i);
        if (v > maxidx) {
            maxidx = v;
        }
    }

    spheremap = (s_tg_s_mode == GL_SPHERE_MAP || s_tg_t_mode == GL_SPHERE_MAP);

    if (spheremap) {
        /* Seeded to identity, not left uninitialised, because GLdc's support
         * for the GL_MODELVIEW_MATRIX query is unverified -- there is no GLdc
         * source in the SDK image to check. If the query is a no-op we fall
         * back to object-space sphere mapping, which is wrong but bounded;
         * reading stack garbage would not be.
         *
         * glKosGetMatrix() would have been the natural call and is prototyped
         * in kos-ports GL/gl.h, but it has NO definition in libGL.a (checked
         * with sh-elf-gcc-nm) -- a GLdc header/library mismatch. Using it
         * makes the link fail. */
        GLfloat mv[16] = { 1.0f, 0.0f, 0.0f, 0.0f,
                           0.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 1.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 1.0f };

        tc = tr_tc_reserve(&s_tc_sph, &s_tc_sph_cap, maxidx + 1);
        if (!tc) {
            glDrawElements(mode, count, type, indices);
            return;
        }
        /* Depends on the modelview, so it cannot be cached across frames.
         * Only the referenced vertices are written, so the cost tracks the
         * size of the draw, not the size of the array. */
        glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        for (i = 0; i < count; i++) {
            GLuint v = tr_index_at(indices, type, i);
            tr_gen_spheremap(v, mv, tc + v * 2);
        }
    } else {
        tc = tr_tc_reserve(&s_tc_lin, &s_tc_lin_cap, maxidx + 1);
        if (!tc) {
            glDrawElements(mode, count, type, indices);
            return;
        }
        if (s_tc_lin_ser != s_tg_serial || s_tc_lin_src != s_arr_v.ptr) {
            s_tc_lin_ser = s_tg_serial;
            s_tc_lin_src = s_arr_v.ptr;
            s_tc_lin_gen = 0;
        }
        /* Object-linear coordinates are a pure function of the vertex, and
         * the terrain array is static after load, so this loop runs once per
         * course and then never again. */
        for (; s_tc_lin_gen <= maxidx; s_tc_lin_gen++) {
            GLfloat p[3];
            if (tr_fetch3f(&s_arr_v, s_tc_lin_gen, p)) {
                tr_objlinear(p[0], p[1], p[2],
                             &tc[s_tc_lin_gen * 2], &tc[s_tc_lin_gen * 2 + 1]);
            } else {
                tc[s_tc_lin_gen * 2] = tc[s_tc_lin_gen * 2 + 1] = 0.0f;
            }
        }
    }

    glTexCoordPointer(2, GL_FLOAT, 0, tc);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    glDrawElements(mode, count, type, indices);

    tr_restore_arrays(GL_FALSE, GL_FALSE, GL_TRUE, GL_FALSE);
}

/* ==========================================================================
 * Double-precision entry points
 *
 * sh-elf is built -m4-single: GLfloat maths runs on the FPU, GLdouble maths
 * does not. Narrowing at the boundary is the whole point.
 * ========================================================================== */

void glMultMatrixd(const GLdouble *m)
{
    GLfloat f[16];
    int i;
    for (i = 0; i < 16; i++) {
        f[i] = (GLfloat) m[i];
    }
    glMultMatrixf(f);
}

void glLoadMatrixd(const GLdouble *m)
{
    GLfloat f[16];
    int i;
    for (i = 0; i < 16; i++) {
        f[i] = (GLfloat) m[i];
    }
    glLoadMatrixf(f);
}

void glColor3dv(const GLdouble *v)
{
    glColor4f((GLfloat) v[0], (GLfloat) v[1], (GLfloat) v[2], 1.0f);
}

void glColor4dv(const GLdouble *v)
{
    glColor4f((GLfloat) v[0], (GLfloat) v[1], (GLfloat) v[2], (GLfloat) v[3]);
}

void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a)
{
    glColor4f((GLfloat) r, (GLfloat) g, (GLfloat) b, (GLfloat) a);
}

/* These three route through the tr_ hooks, not GLdc directly, so that a
 * double-precision vertex inside a display list or under texgen behaves the
 * same as a float one. */
void glVertex2dv(const GLdouble *v)
{
    tr_glVertex2f((GLfloat) v[0], (GLfloat) v[1]);
}

void glVertex3dv(const GLdouble *v)
{
    tr_glVertex3f((GLfloat) v[0], (GLfloat) v[1], (GLfloat) v[2]);
}

void glNormal3dv(const GLdouble *v)
{
    tr_glNormal3f((GLfloat) v[0], (GLfloat) v[1], (GLfloat) v[2]);
}

void glTexCoord2d(GLdouble s, GLdouble t)
{
    tr_glTexCoord2f((GLfloat) s, (GLfloat) t);
}

void glTexCoord2dv(const GLdouble *v)
{
    tr_glTexCoord2f((GLfloat) v[0], (GLfloat) v[1]);
}

/* ==========================================================================
 * GL entry point lookup
 * ========================================================================== */

void *SDL_GL_GetProcAddress(const char *proc)
{
    /* Always NULL, and that is the correct answer rather than a stub.
     *
     * The only names the game ever asks for are "glLockArraysEXT" and
     * "glUnlockArraysEXT" (src/gl_util.c:414,416) -- the
     * GL_EXT_compiled_vertex_array extension. GLdc does not implement it:
     * neither symbol is in libGL.a, and there is nothing for it to mean.
     * CVA asks the driver to lock a vertex range so it can reuse transformed
     * results across draw calls; GLdc transforms into the PVR store queues
     * per submission and keeps no such cache, so a no-op glLockArraysEXT
     * would be a lie that costs a call per terrain draw.
     *
     * Returning NULL puts src/gl_util.c:418-431 on its own "extension NOT
     * supported" path, which is code upstream already ships and tests: it
     * leaves glLockArraysEXT_p / glUnlockArraysEXT_p NULL, and both call
     * sites (src/quadtree.cpp:1001,1017) are `if (ptr && ...)` guarded.
     *
     * Consequence for configuration: getparam_use_cva() and
     * getparam_cva_hack() become inert. Toggling either does nothing at all,
     * because the pointer test fails first in both guards. Documented in
     * kb/design-gl.md so nobody goes hunting for a perf difference. */
    (void) proc;
    return NULL;
}

/* ==========================================================================
 * gluScaleImage
 *
 * Only reached from src/textures.c:146, and only for a texture larger than
 * GL_MAX_TEXTURE_SIZE (1024 on PVR2). Bilinear rather than point sampling
 * because the one thing that would hit it is a large UI image, where
 * aliasing would be obvious.
 * ========================================================================== */

static int tr_components(GLenum format)
{
    switch (format) {
    case GL_RGB:             return 3;
    case GL_RGBA:            return 4;
    case GL_LUMINANCE:       return 1;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_ALPHA:           return 1;
    default:                 return 0;
    }
}

GLint gluScaleImage(GLenum format,
                    GLsizei wIn,  GLsizei hIn,  GLenum typeIn,  const void *dataIn,
                    GLsizei wOut, GLsizei hOut, GLenum typeOut, void *dataOut)
{
    const unsigned char *src = (const unsigned char *) dataIn;
    unsigned char *dst = (unsigned char *) dataOut;
    int nc = tr_components(format);
    GLsizei x, y;
    int c;

    if (typeIn != GL_UNSIGNED_BYTE || typeOut != GL_UNSIGNED_BYTE || nc == 0) {
        /* GL_INVALID_ENUM. The game only ever passes GL_UNSIGNED_BYTE with
         * GL_RGB or GL_RGBA (src/textures.c:146-153). */
        TR_WARN_ONCE("gluScaleImage: unsupported format/type");
        return 0x0500;
    }
    if (wIn <= 0 || hIn <= 0 || wOut <= 0 || hOut <= 0) {
        return 0x0501; /* GL_INVALID_VALUE */
    }

    for (y = 0; y < hOut; y++) {
        /* Sample at pixel centres so the edges are not biased. */
        float sy = ((float) y + 0.5f) * (float) hIn / (float) hOut - 0.5f;
        int   y0 = (int) floorf(sy);
        float fy = sy - (float) y0;
        int   y1;

        if (y0 < 0)       { y0 = 0; fy = 0.0f; }
        if (y0 > hIn - 1) { y0 = hIn - 1; fy = 0.0f; }
        y1 = (y0 + 1 < hIn) ? y0 + 1 : y0;

        for (x = 0; x < wOut; x++) {
            float sx = ((float) x + 0.5f) * (float) wIn / (float) wOut - 0.5f;
            int   x0 = (int) floorf(sx);
            float fx = sx - (float) x0;
            int   x1;
            const unsigned char *p00, *p10, *p01, *p11;
            unsigned char *o;

            if (x0 < 0)       { x0 = 0; fx = 0.0f; }
            if (x0 > wIn - 1) { x0 = wIn - 1; fx = 0.0f; }
            x1 = (x0 + 1 < wIn) ? x0 + 1 : x0;

            p00 = src + ((size_t) y0 * wIn + x0) * nc;
            p10 = src + ((size_t) y0 * wIn + x1) * nc;
            p01 = src + ((size_t) y1 * wIn + x0) * nc;
            p11 = src + ((size_t) y1 * wIn + x1) * nc;
            o   = dst + ((size_t) y  * wOut + x) * nc;

            for (c = 0; c < nc; c++) {
                float top = (float) p00[c] + ((float) p10[c] - (float) p00[c]) * fx;
                float bot = (float) p01[c] + ((float) p11[c] - (float) p01[c]) * fx;
                float v   = top + (bot - top) * fy;
                o[c] = (unsigned char) (v + 0.5f);
            }
        }
    }
    return 0;
}
