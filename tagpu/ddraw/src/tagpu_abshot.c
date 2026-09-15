/* tagpu_abshot.c -- the GL half of a Phase G A/B. Contract: tagpu_abshot.h. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "opengl_utils.h"
#include "tagpu_abshot.h"

static void alog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* The five this file needs that opengl_utils.h does not already carry. GL 1.1
   is not offered by wglGetProcAddress on every driver, so opengl32 is asked
   second -- the fork's own `getgl` shape. */
typedef void      (APIENTRY *PFN_GETFLOATV)(GLenum, GLfloat*);
typedef GLboolean (APIENTRY *PFN_ISENABLED)(GLenum);
typedef void      (APIENTRY *PFN_DISABLE)(GLenum);
typedef void      (APIENTRY *PFN_CLEARCOLOR)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void      (APIENTRY *PFN_READPIXELS)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
static PFN_GETFLOATV   x_glGetFloatv;
static PFN_ISENABLED   x_glIsEnabled;
static PFN_DISABLE     x_glDisable;
static PFN_CLEARCOLOR  x_glClearColor;
static PFN_READPIXELS  x_glReadPixels;
static int s_ready;                 /* 0 unresolved, 1 ready, 2 refused once  */

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

/* ONE MESSAGE NAMING THE ONE THAT IS MISSING, and one latch so it is not
   retried every poll for the session. */
static int init_gl(void)
{
    const char* miss = NULL;
    char msg[192];
    if (s_ready) return s_ready == 1;
    x_glGetFloatv  = (PFN_GETFLOATV)  getgl("glGetFloatv");
    x_glIsEnabled  = (PFN_ISENABLED)  getgl("glIsEnabled");
    x_glDisable    = (PFN_DISABLE)    getgl("glDisable");
    x_glClearColor = (PFN_CLEARCOLOR) getgl("glClearColor");
    x_glReadPixels = (PFN_READPIXELS) getgl("glReadPixels");
    if (!x_glGetFloatv)       miss = "glGetFloatv";
    else if (!x_glIsEnabled)  miss = "glIsEnabled";
    else if (!x_glDisable)    miss = "glDisable";
    else if (!x_glClearColor) miss = "glClearColor";
    else if (!x_glReadPixels) miss = "glReadPixels";
    else if (!glClear)        miss = "glClear";
    else if (!glEnable)       miss = "glEnable";
    else if (!glGetIntegerv)  miss = "glGetIntegerv";
    else if (!glPixelStorei)  miss = "glPixelStorei";
    if (miss) {
        _snprintf(msg, sizeof msg, "abshot: this context has no %s - no A/B "
                  "capture can be taken on it", miss);
        msg[sizeof msg - 1] = 0;
        alog(msg);
        s_ready = 2;
        return 0;
    }
    s_ready = 1;
    return 1;
}

void tagpu_abshot_begin(TAGPU_ABSHOT* s)
{
    if (!s) return;
    s->live = 0;
    if (!init_gl()) return;
    s->live = 1;
    x_glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear);
    s->scissor = x_glIsEnabled(GL_SCISSOR_TEST);
    s->pack = 4;                       /* the initial value, if the get fails */
    glGetIntegerv(GL_PACK_ALIGNMENT, &s->pack);
    x_glDisable(GL_SCISSOR_TEST);
    x_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* A binary PPM of an RGBA readback, rows turned over.

   glReadPixels hands back the BOTTOM row first and a PPM's first row is the
   TOP one, so the rows are written backwards. Getting this wrong produces a
   capture that differs from the Vulkan one in every drawn pixel and in nothing
   else, which reads as a Y-flip bug in the port rather than in the oracle. */
static void write_ppm(const char* path, int w, int h, const unsigned char* rgba)
{
    FILE* fp;
    int y, x;
    unsigned char* line;
    /* THE ROW BUFFER BEFORE THE FILE. A failed allocation after the header is
       written leaves a header-only PPM on disk, which reads as a broken writer
       rather than as a machine out of memory. */
    line = (unsigned char*)malloc((size_t)w * 3);
    if (!line) { alog("abshot: no memory for a row of the capture"); return; }
    if (!(fp = fopen(path, "wb"))) { alog("abshot: could not open the capture file"); free(line); return; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    /* ONE fwrite A ROW: this is the render thread and a stdio call per pixel is
       two million of them at 1080p. */
    for (y = h - 1; y >= 0; y--) {
        const unsigned char* row = rgba + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            line[x * 3 + 0] = row[(size_t)x * 4 + 0];
            line[x * 3 + 1] = row[(size_t)x * 4 + 1];
            line[x * 3 + 2] = row[(size_t)x * 4 + 2];
        }
        fwrite(line, 1, (size_t)w * 3, fp);
    }
    free(line);
    fclose(fp);
}

void tagpu_abshot_end(TAGPU_ABSHOT* s, const char* path, const char* tag)
{
    GLint vp[4] = { 0, 0, 0, 0 };
    unsigned char* buf;
    char msg[192];

    if (!s || !s->live) return;
    s->live = 0;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0 || vp[2] > 8192 || vp[3] > 8192) {
        _snprintf(msg, sizeof msg, "%s: the A/B found no sane viewport - nothing captured", tag);
        msg[sizeof msg - 1] = 0;
        alog(msg);
    } else if ((buf = (unsigned char*)malloc((size_t)vp[2] * vp[3] * 4)) != NULL) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        x_glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, buf);
        write_ppm(path, vp[2], vp[3], buf);
        free(buf);
        _snprintf(msg, sizeof msg, "%s: A/B wrote %s, %dx%d", tag, path, (int)vp[2], (int)vp[3]);
        msg[sizeof msg - 1] = 0;
        alog(msg);
    } else {
        _snprintf(msg, sizeof msg, "%s: no memory for the A/B capture", tag);
        msg[sizeof msg - 1] = 0;
        alog(msg);
    }

    /* EVERYTHING GOES BACK, including GL_PACK_ALIGNMENT: 4 is the initial value
       every other reader in the fork assumes, and leaving it at 1 is the same
       kind of leak from a lever into play state as the clear colour was. */
    glPixelStorei(GL_PACK_ALIGNMENT, s->pack);
    x_glClearColor(s->clear[0], s->clear[1], s->clear[2], s->clear[3]);
    if (s->scissor) glEnable(GL_SCISSOR_TEST);
}
