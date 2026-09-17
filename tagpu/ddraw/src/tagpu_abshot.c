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

/* The seven this file needs that opengl_utils.h does not already carry (five
   until G19e's second world pass added the depth clear). GL 1.1 is not offered
   by wglGetProcAddress on every driver, so opengl32 is asked second -- the
   fork's own `getgl` shape. */
typedef void      (APIENTRY *PFN_GETFLOATV)(GLenum, GLfloat*);
typedef GLboolean (APIENTRY *PFN_ISENABLED)(GLenum);
typedef void      (APIENTRY *PFN_DISABLE)(GLenum);
typedef void      (APIENTRY *PFN_CLEARCOLOR)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void      (APIENTRY *PFN_READPIXELS)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef void      (APIENTRY *PFN_DEPTHMASK)(GLboolean);
typedef void      (APIENTRY *PFN_CLEARDEPTH)(GLdouble);
static PFN_GETFLOATV   x_glGetFloatv;
static PFN_ISENABLED   x_glIsEnabled;
static PFN_DISABLE     x_glDisable;
static PFN_CLEARCOLOR  x_glClearColor;
static PFN_READPIXELS  x_glReadPixels;
static PFN_DEPTHMASK   x_glDepthMask;
static PFN_CLEARDEPTH  x_glClearDepth;
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
    x_glDepthMask  = (PFN_DEPTHMASK)  getgl("glDepthMask");
    x_glClearDepth = (PFN_CLEARDEPTH) getgl("glClearDepth");
    if (!x_glGetFloatv)       miss = "glGetFloatv";
    else if (!x_glIsEnabled)  miss = "glIsEnabled";
    else if (!x_glDisable)    miss = "glDisable";
    else if (!x_glClearColor) miss = "glClearColor";
    else if (!x_glReadPixels) miss = "glReadPixels";
    else if (!x_glDepthMask)  miss = "glDepthMask";
    else if (!x_glClearDepth) miss = "glClearDepth";
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

void tagpu_abshot_begin(TAGPU_ABSHOT* s, unsigned flags)
{
    GLbitfield bits = GL_COLOR_BUFFER_BIT;
    if (!s) return;
    s->live = 0;
    s->flags = flags;
    if (!init_gl()) return;
    s->live = 1;
    x_glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear);
    s->cleardepth = 1.0f;              /* the initial value, if the get fails */
    x_glGetFloatv(GL_DEPTH_CLEAR_VALUE, &s->cleardepth);
    s->scissor = x_glIsEnabled(GL_SCISSOR_TEST);
    s->depthmask = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &s->depthmask);
    s->pack = 4;                       /* the initial value, if the get fails */
    glGetIntegerv(GL_PACK_ALIGNMENT, &s->pack);
    /* THE CLEAR IS ALWAYS UNSCISSORED. A scissor an earlier pass left on would
       black a rectangle and leave the rest of the frame in the capture. */
    x_glDisable(GL_SCISSOR_TEST);
    x_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    if (flags & TAGPU_ABSHOT_DEPTH) {
        bits |= GL_DEPTH_BUFFER_BIT;
        /* masked by the depth write mask, so it is forced for the clear alone */
        x_glDepthMask(GL_TRUE);
        x_glClearDepth(1.0);
    }
    glClear(bits);
    if (flags & TAGPU_ABSHOT_DEPTH)
        x_glDepthMask(s->depthmask ? GL_TRUE : GL_FALSE);
    /* AND THE SCISSOR GOES BACK BEFORE THE PASS DRAWS when the caller asked,
       because from here to `end` is the pass itself and a lever that changes
       how the pass draws is measuring something else. */
    if ((flags & TAGPU_ABSHOT_SCISSOR) && s->scissor) glEnable(GL_SCISSOR_TEST);
}

/* A binary PPM of an RGBA readback.

   glReadPixels hands back the BOTTOM row of the read framebuffer first and a
   PPM's first row is the TOP one, so by default the rows are written backwards.
   Getting this wrong produces a capture that differs from the Vulkan one in
   every drawn pixel and in nothing else, which reads as a Y-flip bug in the
   port rather than in the oracle.

   `topdown` says the framebuffer already holds the game frame top row first, in
   which case the reversal is what would turn it over -- TAGPU_ABSHOT_TOPDOWN in
   the header has the argument, and which passes set it and why. */
/* 1 only when the whole capture reached the disk. A caller that claims the
   Vulkan half of an A/B on a GL half that was never written would have
   tools/vk-ab.py diff against a missing -- or, worse, a STALE -- _gl.ppm and
   report a port failure that is really a capture failure. */
static int write_ppm(const char* path, int w, int h, const unsigned char* rgba,
                    int topdown)
{
    FILE* fp;
    int i, x;
    unsigned char* line;
    /* THE ROW BUFFER BEFORE THE FILE. A failed allocation after the header is
       written leaves a header-only PPM on disk, which reads as a broken writer
       rather than as a machine out of memory. */
    line = (unsigned char*)malloc((size_t)w * 3);
    if (!line) { alog("abshot: no memory for a row of the capture"); return 0; }
    if (!(fp = fopen(path, "wb"))) { alog("abshot: could not open the capture file"); free(line); return 0; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    /* ONE fwrite A ROW: this is the render thread and a stdio call per pixel is
       two million of them at 1080p. */
    for (i = 0; i < h; i++) {
        int y = topdown ? i : (h - 1 - i);
        const unsigned char* row = rgba + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            line[x * 3 + 0] = row[(size_t)x * 4 + 0];
            line[x * 3 + 1] = row[(size_t)x * 4 + 1];
            line[x * 3 + 2] = row[(size_t)x * 4 + 2];
        }
        fwrite(line, 1, (size_t)w * 3, fp);
    }
    free(line);
    /* fclose IS part of the answer: a short write on a full disk surfaces here
       and nowhere else, and a truncated PPM is exactly the stale-looking half
       this return value exists to refuse. */
    return fclose(fp) == 0;
}

int tagpu_abshot_end(TAGPU_ABSHOT* s, const char* path, const char* tag)
{
    GLint vp[4] = { 0, 0, 0, 0 };
    unsigned char* buf;
    char msg[192];
    int ok = 0;

    /* `live` 0 means `begin` never ran -- init_gl refused an entry point -- so
       there is no state to put back and, above all, no capture on the disk. */
    if (!s || !s->live) return 0;
    s->live = 0;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0 || vp[2] > 8192 || vp[3] > 8192) {
        _snprintf(msg, sizeof msg, "%s: the A/B found no sane viewport - nothing captured", tag);
        msg[sizeof msg - 1] = 0;
        alog(msg);
    } else if ((buf = (unsigned char*)malloc((size_t)vp[2] * vp[3] * 4)) != NULL) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        x_glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, buf);
        ok = write_ppm(path, vp[2], vp[3], buf,
                       (s->flags & TAGPU_ABSHOT_TOPDOWN) != 0);
        free(buf);
        if (ok)
            _snprintf(msg, sizeof msg, "%s: A/B wrote %s, %dx%d", tag, path, (int)vp[2], (int)vp[3]);
        else
            _snprintf(msg, sizeof msg, "%s: the A/B capture did not reach %s", tag, path);
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
    if (s->flags & TAGPU_ABSHOT_DEPTH) x_glClearDepth((GLdouble)s->cleardepth);
    /* the scissor may already be back (TAGPU_ABSHOT_SCISSOR); enabling an
       enabled capability is not an error and this is the one path that runs
       for every caller */
    if (s->scissor) glEnable(GL_SCISSOR_TEST);
    return ok;
}
