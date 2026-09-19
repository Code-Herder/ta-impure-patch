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
       filled only by a bracket render_ogl.c called, and landing 11-2 deleted
       that file. (4d-1 deleted route D, which is what made the two lanes stop
       being live in one process; 11-2 took the backend itself. An earlier
       draft of this sentence conflated them -- the 11-5e-1 review's MEDIUM-3.)
       From then the branch could not be reached, and from THIS
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
        /* THE VULKAN LANE HAS A POOL TOO, AND THIS FUNCTION CANNOT REACH IT.
           An earlier version of this comment said the lane "carries no such
           pool here". It does: `tagpu_vk.c` keeps `tsPool` and
           `tsPend[MAXIMG]`, the write is gated on `tagpu_ftime_armed()` and
           sets `tsPend[fi] = 1`, and the drain -- which is NOT gated -- reads
           any slot whose flag is set and calls `tagpu_ftime_vk_sample`.
           Nothing here clears `tsPend`; only the swapchain's creation and
           `vk_perimage_free` do. [THE 11-5e-1 REVIEW'S MEDIUM-1b, and it was
           right: the deleted GL half kept `s_qPend` for exactly this reason
           and the landing-6 review caught stale pairs being harvested into a
           freshly cleared ring.]

           WHAT ACTUALLY PROTECTS THE RING IS `s_on`, tested at the top of
           `tagpu_ftime_vk_sample`: a pair resolved while the lever is off is
           dropped there rather than stopped at the source. That is a real
           guard and it is why the ring is honest in the steady state -- but it
           is one test in another function, so say it here rather than leave
           the next reader to rediscover it.

           THE RESIDUAL HOLE, NAMED RATHER THAN WAVED AT. This poll runs at
           render_vk.c, BEFORE `tagpu_vk_frame` in the same loop iteration, so
           on the iteration that flips the lever off->on `s_on` is already 1
           when the drain runs. A pair stamped in the PREVIOUS armed window can
           therefore still be accepted into the freshly cleared ring. Pending
           slots drain one per present and two polls are at least POLL_MS
           apart, so it needs fewer than MAXIMG presents in half a second --
           under about five frames a second: a map load, a stalled device, or
           the owed-teardown `vkDeviceWaitIdle`. Measured here at full frame
           rate, an off/on cycle reported `n=256/300` on the next report, i.e.
           the counter restarted cleanly and nothing leaked; that exercises the
           common case and NOT the stall.
           THE BY-CONSTRUCTION FIX IS AN IDENTITY, NOT A TIMING ARGUMENT: stamp
           each pending slot with the arm generation this poll bumps, and have
           the drain accept a pair only when the generation still matches.
           Deliberately not done here -- it belongs to `tagpu_vk.c`'s seam and
           this landing is three GL-free leaf files. */
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

/* The seam calls this when it brings the lane down. `s_frames` goes with the
   ring: it used to be left alone, so a lane that came down at frame 299 of the
   300-frame cadence made the NEXT sample after the re-bring-up trip the report
   and print a p50/p99 "over 256 frames" computed from one. The window and the
   counter over it are one piece of state and are cleared as one.
   [THE 11-5e-1 REVIEW'S LOW-1a; pre-existing, and the collapse of `report()`
   to a single branch is what made the wrong line plausible rather than
   obviously empty.] */
void tagpu_ftime_vk_reset(void)
{
    s_vkN = 0; s_vkAt = 0; s_vkTotal = 0; s_frames = 0;
}
