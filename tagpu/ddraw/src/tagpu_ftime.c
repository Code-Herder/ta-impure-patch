/* tagpu_ftime.c -- the frame-time harness. tagpu_ftime.h has the design and the
   reason it is timestamps rather than scoped queries. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opengl_utils.h"
#include "tagpu_ftime.h"

#define RING     256            /* frames kept per lane for the percentiles   */
#define GLQ      8              /* GL query pairs in flight: the driver is a
                                   few frames deep and a pair is only read
                                   once it says it is available               */
#define REPORT_FRAMES 300       /* one line per this many ARMED frames        */
#define POLL_MS  500

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int      s_on;
static DWORD    s_lastPoll;

/* ---- the GL half -------------------------------------------------------- */
typedef void (APIENTRY* PFN_GENQ)(GLsizei, GLuint*);
typedef void (APIENTRY* PFN_DELQ)(GLsizei, const GLuint*);
typedef void (APIENTRY* PFN_QCTR)(GLuint, GLenum);
typedef void (APIENTRY* PFN_QOBJIV)(GLuint, GLenum, GLint*);
typedef void (APIENTRY* PFN_QOBJUI64)(GLuint, GLenum, GLuint64*);
static PFN_GENQ     x_glGenQueries;
static PFN_DELQ     x_glDeleteQueries;
static PFN_QCTR     x_glQueryCounter;
static PFN_QOBJIV   x_glGetQueryObjectiv;
static PFN_QOBJUI64 x_glGetQueryObjectui64v;
static int          s_glFetched, s_glOk, s_glSaid;

#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) p = (void*)GetProcAddress(gl, n);
    }
    return p;
}

/* a pair of counters, and whether it is waiting to be read */
static GLuint   s_qBeg[GLQ], s_qEnd[GLQ];
static char     s_qPend[GLQ];
static int      s_qSlot;            /* the pair this frame is writing         */
static int      s_qMade;
static int      s_glOpen;           /* `begin` wrote a counter this frame     */

/* ---- the samples -------------------------------------------------------- */
static double   s_glRing[RING], s_vkRing[RING];
static int      s_glN, s_vkN;       /* how many of the ring are filled        */
static unsigned s_glAt, s_vkAt;     /* write cursor, free-running             */
static unsigned s_glTotal, s_vkTotal;
static unsigned s_frames;

static void push(double* ring, int* n, unsigned* at, double v)
{
    ring[*at % RING] = v;
    (*at)++;
    if (*n < RING) (*n)++;
}

static int cmpd(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* the p-th percentile of a copy of the ring, 0 when there is nothing in it */
static double pct(const double* ring, int n, double p)
{
    static double tmp[RING];
    int i;
    if (n <= 0) return 0.0;
    for (i = 0; i < n; i++) tmp[i] = ring[i];
    qsort(tmp, (size_t)n, sizeof tmp[0], cmpd);
    i = (int)(p * (double)(n - 1) + 0.5);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return tmp[i];
}

static void report(void)
{
    char b[300];
    double g50 = pct(s_glRing, s_glN, 0.50), g99 = pct(s_glRing, s_glN, 0.99);
    double v50 = pct(s_vkRing, s_vkN, 0.50), v99 = pct(s_vkRing, s_vkN, 0.99);
    /* THE RATIO IS ONLY PRINTED WHEN BOTH LANES HAVE SAMPLES, and it is the
       gate's own question: below 1.00 the Vulkan lane's frame is cheaper. With
       one lane silent it is not a small ratio, it is no comparison at all. */
    if (s_glN > 0 && s_vkN > 0 && g50 > 0.0)
        _snprintf(b, sizeof b,
                  "ftime: gl p50 %.3f ms p99 %.3f ms (n=%d/%u) | vk p50 %.3f ms p99 %.3f ms (n=%d/%u)"
                  " | vk/gl p50 %.3f p99 %.3f",
                  g50, g99, s_glN, s_glTotal, v50, v99, s_vkN, s_vkTotal,
                  v50 / g50, g99 > 0.0 ? v99 / g99 : 0.0);
    else
        _snprintf(b, sizeof b,
                  "ftime: gl p50 %.3f ms p99 %.3f ms (n=%d/%u) | vk %s"
                  " - no ratio: one lane has no samples",
                  g50, g99, s_glN, s_glTotal,
                  s_vkN > 0 ? "sampling" : "SILENT (lane not armed, or no timestamps)");
    b[sizeof b - 1] = 0;
    flog(b);
}

/* ---- the lever ---------------------------------------------------------- */
void tagpu_ftime_poll(void)
{
    DWORD now = GetTickCount();
    if (s_lastPoll && now - s_lastPoll < POLL_MS) return;
    s_lastPoll = now ? now : 1;
    {
        int on = GetFileAttributesA("tagpu_ftime.on") != INVALID_FILE_ATTRIBUTES;
        if (on == s_on) return;
        s_on = on;
        /* A CHANGE OF LEVER THROWS THE SAMPLES AWAY. Percentiles over a window
           that spans "armed" and "not armed" describe neither, and the ring is
           the only thing anyone reads. */
        s_glN = s_vkN = 0; s_glAt = s_vkAt = 0; s_frames = 0;
        s_glTotal = s_vkTotal = 0;
        /* AND THE PAIRS STILL IN FLIGHT, which the first version left pending:
           `gl_begin` returns above the harvest while the lever is off, so up to
           GLQ pairs survived the toggle and the first frames after a re-arm
           harvested them into the freshly cleared ring -- samples from an
           arbitrarily old scene, at an arbitrarily old resolution, inside a
           window that says it threw everything away.
           [FOUND 2026-09-16, the landing-6 review.] */
        memset(s_qPend, 0, sizeof s_qPend);
        s_glOpen = 0;
        flog(on ? "ftime: ON - GPU frame time per lane, two timestamps each, nothing blocks"
                : "ftime: off");
    }
}

int tagpu_ftime_armed(void) { return s_on; }

void tagpu_ftime_gl_begin(void)
{
    int i;
    if (!s_on) return;
    if (!s_glFetched) {
        s_glFetched = 1;
        x_glGenQueries          = (PFN_GENQ)getgl("glGenQueries");
        x_glDeleteQueries       = (PFN_DELQ)getgl("glDeleteQueries");
        x_glQueryCounter        = (PFN_QCTR)getgl("glQueryCounter");
        x_glGetQueryObjectiv    = (PFN_QOBJIV)getgl("glGetQueryObjectiv");
        x_glGetQueryObjectui64v = (PFN_QOBJUI64)getgl("glGetQueryObjectui64v");
        s_glOk = x_glGenQueries && x_glQueryCounter &&
                 x_glGetQueryObjectiv && x_glGetQueryObjectui64v;
        if (!s_glOk && !s_glSaid) {
            s_glSaid = 1;
            /* ARB_timer_query is an EXTENSION on the 3.2 context this fork
               creates, not core -- tagpu_restoreglsl.c says the same of
               GL_TIME_ELAPSED. Without it there is no GL figure and therefore
               no ratio, which the report says rather than printing a half. */
            flog("ftime: no glQueryCounter/glGetQueryObjectui64v - ARB_timer_query is absent, "
                 "so the GL lane cannot be timed and no comparison is possible");
        }
    }
    if (!s_glOk) {
        /* THE REPORT STILL HAS TO COME OUT. It is called from `gl_end`, which
           returns on `!s_glOk` -- so the "no ratio: one lane has no samples"
           line the comment above promises could never be printed in exactly the
           case it was written for, and a session without ARB_timer_query said
           nothing at all after its one-shot line. The Vulkan half goes on
           sampling, so there IS something to report; it is just not a ratio.
           [FOUND 2026-09-16, the landing-6 review.] */
        if (++s_frames >= REPORT_FRAMES) { s_frames = 0; report(); }
        return;
    }
    if (!s_qMade) {
        x_glGenQueries(GLQ, s_qBeg);
        x_glGenQueries(GLQ, s_qEnd);
        for (i = 0; i < GLQ; i++) if (!s_qBeg[i] || !s_qEnd[i]) break;
        if (i != GLQ) {
            /* GIVE THE NAMES BACK AND STOP ASKING. The first version returned
               with `s_qMade` still 0, so the next frame generated 2*GLQ more
               names over the top of these and leaked 16 a frame for ever. One
               failure here means the driver will not give us queries; say so
               once and stand down for the session rather than retry per frame.
               [FOUND 2026-09-16, the landing-6 review.] */
            if (x_glDeleteQueries) {
                x_glDeleteQueries(GLQ, s_qBeg);
                x_glDeleteQueries(GLQ, s_qEnd);
            }
            memset(s_qBeg, 0, sizeof s_qBeg);
            memset(s_qEnd, 0, sizeof s_qEnd);
            s_glOk = 0;
            if (!s_glSaid) { s_glSaid = 1; flog("ftime: glGenQueries gave no usable names - the GL lane cannot be timed"); }
            return;
        }
        s_qMade = 1;
    }
    /* HARVEST FIRST, AND ONLY WHAT THE DRIVER SAYS IS THERE. Every pending pair
       is offered, not just the oldest: a driver may finish them out of the
       order we issued them and a strict queue would stall behind one. */
    for (i = 0; i < GLQ; i++) {
        GLint ready = 0;
        GLuint64 t0 = 0, t1 = 0;
        if (!s_qPend[i]) continue;
        x_glGetQueryObjectiv(s_qEnd[i], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) continue;
        x_glGetQueryObjectiv(s_qBeg[i], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) continue;
        x_glGetQueryObjectui64v(s_qBeg[i], GL_QUERY_RESULT, &t0);
        x_glGetQueryObjectui64v(s_qEnd[i], GL_QUERY_RESULT, &t1);
        s_qPend[i] = 0;
        if (t1 > t0) { push(s_glRing, &s_glN, &s_glAt, (double)(t1 - t0) / 1.0e6); s_glTotal++; }
    }
    /* a free pair for this frame, or skip the frame rather than wait for one */
    for (i = 0; i < GLQ; i++) if (!s_qPend[i]) break;
    if (i == GLQ) { s_glOpen = 0; return; }
    s_qSlot = i;
    x_glQueryCounter(s_qBeg[i], GL_TIMESTAMP);
    s_glOpen = 1;
}

void tagpu_ftime_gl_end(void)
{
    if (!s_on || !s_glOk || !s_qMade || !s_glOpen) return;
    s_glOpen = 0;
    x_glQueryCounter(s_qEnd[s_qSlot], GL_TIMESTAMP);
    s_qPend[s_qSlot] = 1;
    if (++s_frames >= REPORT_FRAMES) { s_frames = 0; report(); }
}

void tagpu_ftime_vk_sample(double ns)
{
    if (!s_on || ns <= 0.0) return;
    push(s_vkRing, &s_vkN, &s_vkAt, ns / 1.0e6);
    s_vkTotal++;
}

void tagpu_ftime_vk_reset(void)
{
    s_vkN = 0; s_vkAt = 0; s_vkTotal = 0;
}
