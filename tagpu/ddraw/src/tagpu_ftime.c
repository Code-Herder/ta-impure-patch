/* tagpu_ftime.c -- the frame-time harness. tagpu_ftime.h has the design and the
   reason it is timestamps rather than scoped queries. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "tagpu_ftime.h"

#define RING     256            /* frames kept for the percentiles            */
#define REPORT_FRAMES 300       /* one line per this many ARMED frames        */
#define POLL_MS  500

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int      s_on;
static DWORD    s_lastPoll;

/* ---- the samples -------------------------------------------------------- */
static double   s_vkRing[RING];
static int      s_vkN;              /* how many of the ring are filled        */
static unsigned s_vkAt;             /* write cursor, free-running             */
static unsigned s_vkTotal;
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
    double v50 = pct(s_vkRing, s_vkN, 0.50), v99 = pct(s_vkRing, s_vkN, 0.99);
    /* ONE LANE, SO ONE FIGURE AND NO RATIO. The `vk/gl` arithmetic this
       function used to print needed samples from both rings; the GL ring was
       filled only by a bracket render_ogl.c called, and landing 4d-1 deleted
       that backend. From then the branch could not be reached, and from THIS
       landing there is no second ring for it to read -- so the comparison the
       gate asks for is a CROSS-BUILD one, this build's figure against an
       earlier build's. tagpu_ftime.h says so at length and is the place to
       look; repeating the history here would be the second copy that goes
       stale. [11-5e-1: the GL half deleted; 4d-1 had already made it inert.]

       `s_vkN` IS AT LEAST 1 HERE, so `pct` is never asked for the percentile
       of an empty ring: the sole caller is `tagpu_ftime_vk_sample`, which
       pushes this frame's sample before it counts the frame. The empty case
       has no branch because it has no path -- the two branches that used to
       stand here were for a silent lane, and a silent lane now means this
       function is not called at all. */
    _snprintf(b, sizeof b,
              "ftime: vk p50 %.3f ms p99 %.3f ms (n=%d/%u)"
              " - one lane in this build, so no ratio: compare this against"
              " an earlier build's figure",
              v50, v99, s_vkN, s_vkTotal);
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
        s_vkN = 0; s_vkAt = 0; s_frames = 0;
        s_vkTotal = 0;
        /* THERE IS NOTHING LEFT IN FLIGHT TO CLEAR, and that is a property of
           the lane rather than an omission. The GL bracket kept up to GLQ
           query pairs pending across a toggle and the landing-6 review found
           them being harvested into the freshly cleared ring -- samples from
           an arbitrarily old scene inside a window that says it threw
           everything away. The Vulkan lane carries no such pool here: the seam
           resolves a slot behind the fence it already waits on and hands the
           figure over as a number, so a sample either arrived before this
           reset or is produced after it. [The GL pool went with the GL half in
           11-5e-1; the finding it fixed is recorded because the shape can
           come back with any future pool.] */
        flog(on ? "ftime: ON - GPU frame time on the Vulkan lane, two timestamps"
                  " a frame, nothing blocks"
                : "ftime: off");
    }
}

int tagpu_ftime_armed(void) { return s_on; }

void tagpu_ftime_vk_sample(double ns)
{
    if (!s_on || ns <= 0.0) return;
    push(s_vkRing, &s_vkN, &s_vkAt, ns / 1.0e6);
    s_vkTotal++;
    /* AND THIS LANE DRIVES THE REPORT, because it is the only lane there is.
       It used to ask `!s_glDrives` first, a flag the GL bracket set when it
       ran, so that exactly one of two live lanes printed. With the bracket
       deleted the flag could only ever be 0 and the term only ever true --
       a test that reads as a choice and is not one. [11-5e-1.] */
    if (++s_frames >= REPORT_FRAMES) { s_frames = 0; report(); }
}

void tagpu_ftime_vk_reset(void)
{
    s_vkN = 0; s_vkAt = 0; s_vkTotal = 0;
}
