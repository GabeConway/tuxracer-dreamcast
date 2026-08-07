/* Minimal GLdc render test.
 *
 * Exists to answer one question that the game cannot: does GLdc, on this
 * toolchain and this emulator, put ANY pixel on screen? The port reached a
 * state where the game submitted ~405 immediate-mode primitives per frame at a
 * steady 59 fps and the framebuffer was 0/307200 non-black, which has two very
 * different explanations -- a broken compat shim, or a broken GLdc/PVR
 * bring-up. This program has no shim and no game state, so it separates them.
 *
 * It draws, in order:
 *   - a blue clear
 *   - a white quad, immediate mode, in an ortho projection (what the splash
 *     screen and the whole UI use)
 *   - a red quad in the perspective projection the racing view uses
 * then dumps the framebuffer over the console in the same format as
 * dc/src/dc_fbdump.c, so tools/fbdump-to-png.py decodes it unchanged.
 */

#include <kos.h>
#include <stdio.h>
#include <stdint.h>
#include <dc/scif.h>
#include <dc/video.h>
#include <dc/pvr.h>

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glkos.h>

#define FB_W 640
#define FB_H 480
#define STEP 2
#define OUT_W (FB_W / STEP)
#define OUT_H (FB_H / STEP)

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void emit_row(const uint16_t *row) {
    static char out[((OUT_W * 2 + 2) / 3) * 4 + 16];
    const uint8_t *p = (const uint8_t *)row;
    int n = OUT_W * 2, i = 0, o = 0;

    while(i + 2 < n) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i+1] << 8) | p[i+2];
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = b64tab[v & 63];
        i += 3;
    }
    if(i < n) {
        uint32_t v = (uint32_t)p[i] << 16;
        int rem = n - i;
        if(rem == 2) v |= (uint32_t)p[i+1] << 8;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = (rem == 2) ? b64tab[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    printf("FBROW %s\n", out);
}

static void dump(void) {
    static uint16_t row[OUT_W];
    uint32_t off = PVR_GET(PVR_FB_ADDR) & 0x00fffffc;
    const uint16_t *fb = (const uint16_t *)(0xa5000000 + off);
    unsigned nonzero = 0;
    int x, y;

    for(y = 0; y < FB_H; y++)
        for(x = 0; x < FB_W; x++)
            if(fb[y * FB_W + x]) nonzero++;

    printf("FBPROBE fb_addr ptr=%08lx nonzero=%u/%u\n",
           (unsigned long)(uintptr_t)fb, nonzero, (unsigned)(FB_W * FB_H));

    /* If the picture is not where PVR_FB_ADDR says it is, it is somewhere.
     * Sweep all 8 MB of VRAM in 32 KB blocks and print the ones that are not
     * blank, so the next run can read from the right place instead of guessing
     * a third time. */
    {
        const uint16_t *vram = (const uint16_t *)0xa5000000;
        uint32_t blk;
        for(blk = 0; blk < (8u * 1024 * 1024) / 32768; blk++) {
            const uint16_t *p = vram + (blk * 32768) / 2;
            unsigned nz = 0, i;
            for(i = 0; i < 32768 / 2; i++) if(p[i]) nz++;
            if(nz > 64) {
                printf("VRAM blk=%3lu off=%06lx nonzero=%u/16384\n",
                       (unsigned long)blk, (unsigned long)(blk * 32768u), nz);
            }
        }
    }

    printf("FBDUMP begin %dx%d rgb565\n", OUT_W, OUT_H);
    for(y = 0; y < OUT_H; y++) {
        const uint16_t *src = &fb[(y * STEP) * FB_W];
        for(x = 0; x < OUT_W; x++) row[x] = src[x * STEP];
        emit_row(row);
    }
    printf("FBDUMP end\n");
}

int main(int argc, char **argv) {
    int frame;

    (void)argc; (void)argv;

    scif_set_parameters(1562500, 1);
    scif_init();
    dbgio_dev_select("scif");

    printf("TR-DC-HARNESS-BEGIN\n");
    printf("MARK:BOOT_OK\n");

    glKosInit();
    printf("MARK:GLKOSINIT\n");

    for(frame = 0; frame < 30; frame++) {
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* --- ortho pass: the UI / splash-screen projection --- */
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, 640.0, 0.0, 480.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_CULL_FACE);

        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
            glVertex2f( 50.0f,  50.0f);
            glVertex2f(250.0f,  50.0f);
            glVertex2f(250.0f, 250.0f);
            glVertex2f( 50.0f, 250.0f);
        glEnd();

        /* --- perspective pass: the racing-view projection --- */
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(60.0, 640.0 / 480.0, 0.1, 100.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -5.0f);

        glEnable(GL_DEPTH_TEST);
        glColor4f(1.0f, 0.0f, 0.0f, 1.0f);
        glBegin(GL_QUADS);
            glVertex3f( 0.5f, -1.0f, 0.0f);
            glVertex3f( 2.0f, -1.0f, 0.0f);
            glVertex3f( 2.0f,  1.0f, 0.0f);
            glVertex3f( 0.5f,  1.0f, 0.0f);
        glEnd();

        glKosSwapBuffers();
    }

    printf("ASSERT ok reached_dump\n");
    dump();
    printf("TR-DC-HARNESS-END rc=0\n");
    return 0;
}
