/* tagpu_ftime.c -- the frame-time harness. tagpu_ftime.h has the design and the
   reason it is timestamps rather than scoped queries. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "tagpu_ftime.h"
#include "tagpu_log.h"

#define RING     256            /* frames kept for the percentiles            */
#define REPORT_FRAMES 300       /* one line per this many ARMED frames        */
#define POLL_MS  500

static void flog(const char* s)
{
    tagpu_log(s);
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
    /* ONE LANE, SO ONE FIGURE AND NO RATIO. The comparison the gate asks for
       is a CROSS-BUILD one, this build's figure against an earlier build's.
       tagpu_ftime.h says so at length and is the place to look.

       `s_vkN` IS AT LEAST 1 HERE, so `pct` is never asked for the percentile
       of an empty ring: the sole caller is `tagpu_ftime_vk_sample`, which
       pushes this frame's sample before it counts the frame. The empty case
       has no branch because it has no path -- a silent lane means this
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
        /* THE VULKAN LANE HAS A POOL, AND THIS FUNCTION CANNOT REACH IT.
           `tagpu_vk.c` keeps `tsPool` and
           `tsPend[MAXIMG]`, the write is gated on `tagpu_ftime_armed()` and
           sets `tsPend[fi] = 1`, and the drain -- which is NOT gated -- reads
           any slot whose flag is set and calls `tagpu_ftime_vk_sample`.
           Nothing in THIS file can clear `tsPend`. What does: `vk_perimage`
           zeroes the whole array when the per-image state is built,
           `vk_perimage_free` zeroes it slot by slot on the way down, and the
           drain clears its OWN slot as it reads it -- which is the mechanism
           the "one per present" below depends on, so it belongs in this list
           rather than a paragraph away.

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
           apart, so the window is open only while fewer than `s_vk.nimg`
           presents fit in POLL_MS. **`s_vk.nimg`, NOT MAXIMG**: the slot index
           is `s_vk.frame % s_vk.nimg`, and `nimg` is whatever the swapchain
           reports, merely CLAMPED to MAXIMG (8). At three or four images that
           is six to eight frames a second, and at the clamp sixteen -- so the
           honest statement is "single figures to the low tens". Reachable
           at: a map load, a stalled device, the owed-teardown
           `vkDeviceWaitIdle`.

           MEASURED AT FULL FRAME RATE, an off/on cycle reported `n=256/300` on
           the next report, so `s_frames` and `s_vkTotal` were cleared -- a
           counter that had not restarted would report at some k < 300. **That
           is all it shows.** It does NOT show that nothing leaked: a stale
           pair would simply be one of the 300 and the line would read the
           same. The measurement is structurally blind to the leak, which is
           why this window is named here rather than treated as covered.
           THE BY-CONSTRUCTION FIX IS AN IDENTITY, NOT A TIMING ARGUMENT: stamp
           each pending slot with the arm generation this poll bumps, and have
           the drain accept a pair only when the generation still matches.
           Deliberately not done here -- it belongs to `tagpu_vk.c`'s seam. */
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
    /* AND THIS LANE DRIVES THE REPORT, because it is the only lane there is. */
    if (++s_frames >= REPORT_FRAMES) { s_frames = 0; report(); }
}

/* The seam calls this when it brings the lane down. `s_frames` goes with the
   ring: left alone, a lane that came down at frame 299 of the 300-frame
   cadence would make the NEXT sample after the re-bring-up trip the report and
   print a p50/p99 "over 256 frames" computed from one. The window and the
   counter over it are one piece of state and are cleared as one. */
void tagpu_ftime_vk_reset(void)
{
    s_vkN = 0; s_vkAt = 0; s_vkTotal = 0; s_frames = 0;
}
