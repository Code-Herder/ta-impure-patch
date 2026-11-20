/* tagpu_prof.c -- the frame profiler's lanes, rows and summary. tagpu_prof.h has the design. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_prof.h"
#include "tagpu_log.h"

#define MAXSPAN    24
#define MAXDEPTH   8
#define POLL_MS    500u
#define SUMMARY_MS 5000u
#define BUF_BYTES  65536u
#define ROW_MAX    (80u + MAXSPAN * 12u)   /* five counters and MAXSPAN figures, each under 12 bytes */
#define LEVER      "tagpu_prof.on"
#define SAMPLE_LEVER "tagpu_prof_sample.on"

typedef char game_spans_fit[(TAGPU_PG_N <= MAXSPAN) ? 1 : -1];
typedef char render_spans_fit[(TAGPU_PR_N <= MAXSPAN) ? 1 : -1];
typedef char a_row_fits_the_buffer[(ROW_MAX * 4u <= BUF_BYTES) ? 1 : -1];

static const char* const GAME_NAMES[TAGPU_PG_N] = {
    "outside", "frame", "net", "tick_ours", "units", "projectiles", "explosions", "players",
    "features", "particles", "tick_tail", "before_draw", "terrain", "bars", "sort", "sweep",
    "markers", "fog", "hud", "ui_capture", "flip", "publish_world", "publish", "prof"
};
static const char* const RENDER_NAMES[TAGPU_PR_N] = {
    "loop", "gather", "fence", "acquire", "prepare", "unit_upload", "unit_prepare", "record",
    "submit", "present", "prof"
};

typedef struct {
    /* `on` and `owner` are written by the lane's own thread alone, in its frame mark, and are the
       only words another thread reads: what it can get from them is a dropped mark. Everything
       below them is touched only past the owner test. */
    volatile LONG on;
    volatile LONG owner;

    const char* const* names;
    const char* label;
    const char* file;
    int         nspan, prof;

    int           open;                 /* a frame is open                                  */
    /* `cur` and `frame` are also read by the sampler, and only while it holds this lane's thread
       suspended (THE SAMPLER, below) */
    volatile int      cur;
    volatile unsigned frame;
    int           depth, stack[MAXDEPTH];
    LARGE_INTEGER last, start, armed;
    LONGLONG      acc[MAXSPAN];
    unsigned      count, tag;

    HANDLE   h;
    unsigned used;
    char     buf[BUF_BYTES];

    DWORD    polled, sumAt;
    LONGLONG sum[MAXSPAN];
    unsigned sumFrames, sumCount;
} LANE;

static LANE s_lane[TAGPU_PROF_LANES] = {
    { 0, 0, GAME_NAMES,   "game",   "tagpu_prof_game.csv",   TAGPU_PG_N, TAGPU_PG_PROF },
    { 0, 0, RENDER_NAMES, "render", "tagpu_prof_render.csv", TAGPU_PR_N, TAGPU_PR_PROF },
};

static LONGLONG freq(void)
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    return f.QuadPart ? f.QuadPart : 1;
}

int tagpu_prof_wanted(void)
{
    return GetFileAttributesA(LEVER) != INVALID_FILE_ATTRIBUTES;
}

static void flush(LANE* L)
{
    DWORD wrote;
    if (L->h != INVALID_HANDLE_VALUE && L->used) WriteFile(L->h, L->buf, L->used, &wrote, NULL);
    L->used = 0;
}

static void summary(LANE* L)
{
    char b[1600];
    LONGLONG all = 0;
    int i, n;
    if (!L->sumFrames) return;
    for (i = 0; i < L->nspan; i++) all += L->sum[i];
    n = _snprintf(b, sizeof b, "prof: %s %u frames, %u ticks, %.2f ms a frame:", L->label,
                  L->sumFrames, L->sumCount,
                  (double)all * 1000.0 / (double)freq() / (double)L->sumFrames);
    for (i = 0; i < L->nspan && n > 0 && n < (int)sizeof b - 48; i++) {
        /* a span under half a percent of the window is left out of the line; the rows have it */
        if (L->sum[i] * 200 < all) continue;
        n += _snprintf(b + n, sizeof b - n, " %s %.2f (%d%%)", L->names[i],
                       (double)L->sum[i] * 1000.0 / (double)freq() / (double)L->sumFrames,
                       (int)(all ? L->sum[i] * 100 / all : 0));
    }
    b[sizeof b - 1] = 0;
    tagpu_log(b);
    memset(L->sum, 0, sizeof L->sum);
    L->sumFrames = L->sumCount = 0;
}

static void arm(LANE* L)
{
    char path[MAX_PATH];
    int i;
    _snprintf(path, sizeof path, "%s%s", tagpu_log_dir(), L->file);
    path[sizeof path - 1] = 0;
    L->h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    L->used = (unsigned)_snprintf(L->buf, ROW_MAX, "frame,start_ms,total_us,count,tag");
    for (i = 0; i < L->nspan; i++)
        L->used += (unsigned)_snprintf(L->buf + L->used, 32, ",%s", L->names[i]);
    L->buf[L->used++] = '\n';
    L->open = 0;
    L->frame = 0;
    memset(L->sum, 0, sizeof L->sum);
    L->sumFrames = L->sumCount = 0;
    L->sumAt = GetTickCount();
    QueryPerformanceCounter(&L->armed);
    tagpu_logf("prof: %s lane ON - one row a frame in %s%s", L->label, path,
               L->h == INVALID_HANDLE_VALUE ? " (the file did not open: the summary lines only)" : "");
}

static void disarm(LANE* L)
{
    flush(L);
    if (L->h != INVALID_HANDLE_VALUE) CloseHandle(L->h);
    L->h = INVALID_HANDLE_VALUE;
    summary(L);
    tagpu_logf("prof: %s lane off after %u frames", L->label, L->frame);
}

/* The lane's thread, at its frame boundary. The owner is named before the lane is switched on, so
   a mark that sees it on tests the thread that is arming it. */
static void lever(LANE* L)
{
    DWORD now = GetTickCount();
    int on;
    if (L->polled && now - L->polled < POLL_MS) return;
    L->polled = now ? now : 1;
    on = tagpu_prof_wanted();
    if (on == (int)L->on) return;
    if (on) {
        InterlockedExchange(&L->owner, (LONG)GetCurrentThreadId());
        arm(L);
        InterlockedExchange(&L->on, 1);
    } else {
        InterlockedExchange(&L->on, 0);
        disarm(L);
    }
}

static void row(LANE* L, const LARGE_INTEGER* now)
{
    const LONGLONG f = freq();
    LONGLONG total = now->QuadPart - L->start.QuadPart;
    DWORD t;
    int i;
    if (L->used + ROW_MAX > BUF_BYTES) flush(L);
    L->used += (unsigned)_snprintf(L->buf + L->used, 80, "%u,%.3f,%u,%u,%u", L->frame,
                                   (double)(L->start.QuadPart - L->armed.QuadPart) * 1000.0 / (double)f,
                                   (unsigned)(total * 1000000 / f), L->count, L->tag);
    for (i = 0; i < L->nspan; i++) {
        L->used += (unsigned)_snprintf(L->buf + L->used, 12, ",%u", (unsigned)(L->acc[i] * 1000000 / f));
        L->sum[i] += L->acc[i];
    }
    L->buf[L->used++] = '\n';
    L->sumFrames++;
    L->sumCount += L->count;
    t = GetTickCount();
    if (t - L->sumAt >= SUMMARY_MS) { L->sumAt = t; summary(L); }
}

/* ---- THE SAMPLER ---------------------------------------------------------------------------

   WHAT THE MARKS CANNOT SPLIT, COUNTED INSTEAD OF TIMED. A span like the unit tick is one loop
   over every unit, and its stages run once a unit: a clock read at each would cost more than the
   stage. So a second thread stops the game thread about a thousand times a second, notes where
   it stands, and lets it go. A sample is the open span, the instruction pointer, and the values
   on the thread's stack that lie inside the exe's code section: its return addresses among them,
   from which tools/profsample.py tells which stage of a span the thread was in. A share of a span's
   samples times the span's measured time is an estimate of that stage's time.

   WHAT MAKES STOPPING ANOTHER THREAD SAFE HERE:
   - BETWEEN THE SUSPEND AND THE RESUME THE SAMPLER TAKES NO LOCK. The game thread may be stopped
     inside the heap, the log or the loader, holding what any allocation, log line or file write
     would wait for. In that window there is one system call, the context read, and after it only
     reads of the stopped thread's memory and writes into this thread's own buffer.
   - THE STACK IS READ INSIDE ITS BOUNDS, WHILE ITS THREAD CANNOT MOVE. The stack pointer the
     context gives is a value until it is found aligned and inside the stack's committed range,
     which the thread's own information block gives; a thread that is suspended does not unwind,
     free or grow its stack, so the range read is the one tested.
   - THE THREAD IS THE LANE'S OWNER, by a handle the owner made for itself at its frame mark, and
     the block is the one it read there. Both are the game thread's, which lives as long as the
     process does.

   The lever is `tagpu_prof_sample.on`, beside the profiler's own. The thread is made once, at the
   first frame mark that sees both, and idles while either is absent. Its file and buffer are its
   own. */
#define SAMP_IN    8            /* innermost stack hits kept                              */
#define SAMP_OUT   52           /* outermost stack hits kept                              */
#define SAMP_MAXSTACK (256u * 1024u)
#define SAMP_BUF   2048u        /* samples a write                                        */

typedef struct {
    unsigned      frame, eip, esp;
    unsigned char span, nin, nout, flags;      /* flags bit 0: the stack was not scanned */
    unsigned      in[SAMP_IN];                 /* from the stack pointer up              */
    unsigned      out[SAMP_OUT];               /* from the stack's base down             */
} SAMPLE;
typedef char a_sample_is_256_bytes[(sizeof(SAMPLE) == 256) ? 1 : -1];

static struct {
    HANDLE   thread;            /* the game thread, for suspend and context            */
    NT_TIB*  tib;               /* its stack's bounds                                  */
    unsigned exe, exeEnd;       /* the exe's code section                              */
    LONG     started;
} s_samp;

/* an image's size from its own header; `base` is a loaded module */
static unsigned image_size(const unsigned char* base)
{
    const IMAGE_DOS_HEADER* d = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* n = (const IMAGE_NT_HEADERS*)(base + d->e_lfanew);
    return n->OptionalHeader.SizeOfImage;
}

/* A loaded module's first executable section, from its section table: where its code is. The
   exe's data lies in the same image, and a stack is full of pointers to it. */
static void code_range(const unsigned char* base, unsigned* lo, unsigned* hi)
{
    const IMAGE_DOS_HEADER* d = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* n = (const IMAGE_NT_HEADERS*)(base + d->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(n);
    unsigned i;
    *lo = *hi = 0;
    for (i = 0; i < n->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        *lo = (unsigned)(size_t)base + sec[i].VirtualAddress;
        *hi = *lo + sec[i].Misc.VirtualSize;
        return;
    }
}

/* One sample of the lane's thread. Nothing in here may take a lock: see above. */
static int sample(LANE* L, SAMPLE* s)
{
    CONTEXT c;
    unsigned lo, hi, p, hits = 0, ring[SAMP_OUT], k;
    if (SuspendThread(s_samp.thread) == (DWORD)-1) return 0;
    c.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(s_samp.thread, &c)) { ResumeThread(s_samp.thread); return 0; }
    s->frame = L->frame;
    s->span  = (unsigned char)L->cur;
    s->eip   = c.Eip;
    s->esp   = c.Esp;
    s->nin = s->nout = s->flags = 0;
    lo = (unsigned)(size_t)s_samp.tib->StackLimit;
    hi = (unsigned)(size_t)s_samp.tib->StackBase;
    if ((c.Esp & 3u) || c.Esp < lo || c.Esp >= hi || hi - c.Esp > SAMP_MAXSTACK) {
        s->flags = 1;
    } else {
        for (p = c.Esp; p + 4u <= hi; p += 4u) {
            const unsigned v = *(const unsigned*)(size_t)p;
            if (v < s_samp.exe || v >= s_samp.exeEnd) continue;
            if (hits < SAMP_IN) s->in[hits] = v;
            else ring[(hits - SAMP_IN) % SAMP_OUT] = v;
            hits++;
        }
    }
    ResumeThread(s_samp.thread);
    s->nin = (unsigned char)(hits < SAMP_IN ? hits : SAMP_IN);
    if (hits > SAMP_IN) {
        const unsigned n = hits - SAMP_IN;
        s->nout = (unsigned char)(n < SAMP_OUT ? n : SAMP_OUT);
        for (k = 0; k < s->nout; k++) s->out[k] = ring[(n - 1u - k) % SAMP_OUT];
    }
    return 1;
}

static DWORD WINAPI sampler(void* arg)
{
    LANE* L = &s_lane[TAGPU_PROF_GAME];
    static SAMPLE buf[SAMP_BUF];
    HANDLE h = INVALID_HANDLE_VALUE;
    unsigned used = 0, total = 0;
    DWORD polled = 0, began = 0, wrote;
    int active = 0;
    (void)arg;
    for (;;) {
        const DWORD now = GetTickCount();
        if (now - polled >= 250u) {
            const int want = L->on && GetFileAttributesA(SAMPLE_LEVER) != INVALID_FILE_ATTRIBUTES;
            polled = now;
            if (want && !active) {
                char path[MAX_PATH], head[1024];
                HMODULE self = NULL;
                int n, i;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(size_t)sampler, &self);
                _snprintf(path, sizeof path, "%stagpu_prof_samples.bin", tagpu_log_dir());
                path[sizeof path - 1] = 0;
                h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
                /* one line of text, then the samples: the images' ranges and the span names */
                n = _snprintf(head, sizeof head, "TAPS1 record=%u in=%u out=%u code=%08X+%X dll=%08X+%X spans=",
                              (unsigned)sizeof(SAMPLE), (unsigned)SAMP_IN, (unsigned)SAMP_OUT,
                              s_samp.exe, s_samp.exeEnd - s_samp.exe,
                              (unsigned)(size_t)self, self ? image_size((const unsigned char*)self) : 0u);
                for (i = 0; i < L->nspan && n > 0 && n < (int)sizeof head - 40; i++)
                    n += _snprintf(head + n, sizeof head - n, "%s%s", i ? "," : "", L->names[i]);
                if (n > 0 && n < (int)sizeof head - 1) head[n++] = '\n';
                if (h != INVALID_HANDLE_VALUE && n > 0) WriteFile(h, head, (DWORD)n, &wrote, NULL);
                used = total = 0;
                began = now;
                active = 1;
                tagpu_logf("prof: sampler ON - the game thread's position about 1000 times a second in %s%s",
                           path, h == INVALID_HANDLE_VALUE ? " (the file did not open)" : "");
            } else if (!want && active) {
                if (h != INVALID_HANDLE_VALUE) {
                    if (used) WriteFile(h, buf, used * sizeof(SAMPLE), &wrote, NULL);
                    CloseHandle(h);
                }
                h = INVALID_HANDLE_VALUE;
                active = 0;
                tagpu_logf("prof: sampler off after %u samples in %u ms", total, (unsigned)(now - began));
            }
        }
        if (!active) { Sleep(100); continue; }
        Sleep(1);
        if (sample(L, &buf[used])) { used++; total++; }
        if (used == SAMP_BUF) {
            if (h != INVALID_HANDLE_VALUE) WriteFile(h, buf, used * sizeof(SAMPLE), &wrote, NULL);
            used = 0;
        }
    }
    return 0;
}

/* The game lane's thread, at its frame mark with the lane on: the first time both levers are
   there it names itself to the sampler and starts it. */
static void sampler_start(void)
{
    const unsigned char* exe;
    HANDLE t;
    if (s_samp.started || GetFileAttributesA(SAMPLE_LEVER) == INVALID_FILE_ATTRIBUTES) return;
    s_samp.started = 1;
    exe = (const unsigned char*)GetModuleHandleA(NULL);
    code_range(exe, &s_samp.exe, &s_samp.exeEnd);
    /* this thread's information block, whose first member is its NT_TIB: fs:[0x18] is its own
       linear address */
    __asm__("movl %%fs:0x18, %0" : "=r"(s_samp.tib));
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_samp.thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        tagpu_log("prof: sampler NOT started - no handle to the game thread");
        return;
    }
    t = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else tagpu_log("prof: sampler NOT started - the thread was not made");
}

void tagpu_prof_frame(int lane, int span, unsigned tag)
{
    LANE* L;
    LARGE_INTEGER now, done;
    const LONG tid = (LONG)GetCurrentThreadId();
    if ((unsigned)lane >= TAGPU_PROF_LANES) return;
    L = &s_lane[lane];
    if ((unsigned)span >= (unsigned)L->nspan) return;
    lever(L);
    if (!L->on) return;
    if (lane == TAGPU_PROF_GAME && !s_samp.started) sampler_start();
    QueryPerformanceCounter(&now);
    if (L->open && L->owner == tid) {
        L->acc[L->cur] += now.QuadPart - L->last.QuadPart;
        row(L, &now);
    }
    InterlockedExchange(&L->owner, tid);
    memset(L->acc, 0, sizeof L->acc);
    L->cur = span;
    L->depth = 0;
    L->count = 0;
    L->tag = tag;
    L->start = now;
    L->frame++;
    L->open = 1;
    /* the row and its write belong to the frame they delay: the one that starts here */
    QueryPerformanceCounter(&done);
    L->acc[L->prof] = done.QuadPart - now.QuadPart;
    L->last = done;
}

/* the lane, when this thread owns it and a frame is open; NULL otherwise */
static LANE* mine(int lane)
{
    LANE* L;
    if ((unsigned)lane >= TAGPU_PROF_LANES) return NULL;
    L = &s_lane[lane];
    if (!L->on || L->owner != (LONG)GetCurrentThreadId() || !L->open) return NULL;
    return L;
}

static void charge(LANE* L)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    L->acc[L->cur] += now.QuadPart - L->last.QuadPart;
    L->last = now;
}

void tagpu_prof_mark(int lane, int span)
{
    LANE* L = mine(lane);
    if (!L || (unsigned)span >= (unsigned)L->nspan) return;
    charge(L);
    L->cur = span;
}

void tagpu_prof_stamp(int lane, int ended, int next)
{
    LANE* L = mine(lane);
    if (!L || (unsigned)ended >= (unsigned)L->nspan || (unsigned)next >= (unsigned)L->nspan) return;
    L->cur = ended;
    charge(L);
    L->cur = next;
}

void tagpu_prof_enter(int lane, int span)
{
    LANE* L = mine(lane);
    if (!L || (unsigned)span >= (unsigned)L->nspan) return;
    charge(L);
    /* past MAXDEPTH the span to come back to is not kept, and the leave keeps the one it finds */
    if (L->depth < MAXDEPTH) L->stack[L->depth] = L->cur;
    L->depth++;
    L->cur = span;
}

void tagpu_prof_leave(int lane)
{
    LANE* L = mine(lane);
    if (!L || !L->depth) return;
    charge(L);
    L->depth--;
    if (L->depth < MAXDEPTH) L->cur = L->stack[L->depth];
}

void tagpu_prof_count(int lane)
{
    LANE* L = mine(lane);
    if (L) L->count++;
}
