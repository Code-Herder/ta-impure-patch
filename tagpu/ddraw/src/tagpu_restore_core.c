/* tagpu_restore_core.c -- the Classic++ restorer's scheduler, with no
   rendering API in it. tagpu_restore_core.h says why the module is split this
   way; research/notes/compute-restorer.md has the decisions the numbers come
   from.

   THE MODEL. unditherer/model.py: 3x3 conv 3->64 + ReLU, ten x (3x3 conv
   64->64 + BatchNorm + ReLU), 3x3 conv 64->3, out = in - net(in), RGB in
   [0,1]. BatchNorm is folded into the weight file (unditherer/weights.py),
   which lays every layer out in blocks of mat4 per output channel-tile k: the
   bias first, then a mat4 per (tap, input tile). The backend repacks that
   into the compute kernel's layout at bring-up; the file is the one format
   the exporter writes and the CPU reference (tagpu_restore_ref.c) reads.

   THE PADDING RULE, the thing that defines the pixels. The unditherer runs a
   tile whose opposite edges agree within 12 levels wrap-padded by 12 to 56x56
   and centre-cropped, and every other tile at 32x32 with zero padding at every
   layer. Here both are one mechanism: every slot has a valid rect and a tap
   outside it reads 0, AT EVERY LAYER -- because zero-padding only the INPUT
   and running unmasked is a different network: layer 2 would read layer 1's
   gutter, which is relu(bias), not 0. FILL wrap-pads inside the rect; OUT
   centre-crops.

   BATCHES: a frame's padded edge S is max(w, h) + 2 x depth if it wraps. A
   job's queue is taken in order; a batch holds frames of one SIZE CLASS up to
   a square slot grid of min(8, ACT_MAX / class) per side -- then shrunk to
   the smallest square that holds what was taken, since the passes cost by the
   texel -- and its slot pitch is its largest S plus one. Every slot has its
   own rect, so a 30x25 tree and a 63x60 rock share a batch.

   SLICING. One call per frame issues dispatches until an estimate of their
   GPU time reaches the budget (12 ms by default), the estimate being the
   previous slice's measured time divided by the work it carried. Dispatches
   are the unit: a batch is one FILL, then each of the model's layers in
   bands of TAGPU_R_BAND rows, then one OUT. Where the device will not time a
   slice, it is a fixed, conservative dispatch count instead. No second
   queue, no worker thread: the GPU is the only engine, and a slow one
   restores slower without stalling anything. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tagpu_restore_core.h"
#include "tagpu_log.h"

#define MAX_PATH_B        260
#define TILEABLE_THR      12.0
#define OPT_FILE          "tagpu_restoreglsl.on"
#define DEFAULT_BUDGET_MS 12.0
#define FIXED_DRAWS       6     /* no timer: dispatches per slice              */
#define FIXED_CAP_DRAWS   2     /* ...and a capped job's, tagpu_rcore_job_budget */
#define IDLE_FRAMES       180   /* activations freed after                     */
#define QUEUE_LOG_FRAMES  300   /* a queue's tally, at most                    */
#define STALL_GIVEUP      300   /* slices waited on one result before giving up */

/* the size-class ladder: a batch holds one class */
static const int s_classes[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512 };
#define NCLASSES (int)(sizeof s_classes / sizeof s_classes[0])

static void rlog(const char* s)
{
    tagpu_log(s);
}
/* "<lane>: <what>", the shape every line in this module has */
static void rlog_2(const char* who, const char* what)
{
    char b[300];
    _snprintf(b, sizeof b, "%s: %s", who, what);
    rlog(b);
}
void tagpu_rcore_log(const char* s) { rlog(s); }
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

/* ---- the weight files (unditherer/weights.py) ---- */
static TAGPU_RMODEL s_w[TAGPU_RM_N];
static int          s_wOk[TAGPU_RM_N];
static const char* const s_wName[TAGPU_RM_N] = { "full", "tiny" };
/* what a missing file costs, for its log line */
static const char* const s_wWithout[TAGPU_RM_N] = {
    "Classic++ stays indexed", "the terrain restores with full" };
static TAGPU_ROPT   s_opt;
static int          s_optRead;

/* `which`'s file into s_w[which]; 1 when it is usable. A model that fails
   keeps no body, and its s_wOk says so. */
static int load_weights(const char* who, int which)
{
    TAGPU_RMODEL* w = &s_w[which];
    const char* model = s_wName[which];
    char path[MAX_PATH_B], b[300];
    FILE* f;
    unsigned char hdr[16];
    unsigned depth, ch, ntex, l;
    size_t want;

    if (w->body && !strcmp(w->name, model)) return 1;
    free(w->body); w->body = NULL;
    /* beside TotalA.exe, where tacli links it */
    path[0] = 0;
    if (GetModuleFileNameA(NULL, path, sizeof path)) {
        char* p = strrchr(path, '\\');
        if (p && (size_t)(p - path) + 24 < sizeof path) _snprintf(p + 1, 24, "%s.w32.bin", model);
        else path[0] = 0;
    }
    if (!path[0]) _snprintf(path, sizeof path, "%s.w32.bin", model);
    f = fopen(path, "rb");
    if (!f) { _snprintf(b, sizeof b, "%s: no %s -- %s", who, path, s_wWithout[which]); rlog(b); return 0; }
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "TAW1", 4)) { rlog_2(who, "weight file is not TAW1"); fclose(f); return 0; }
    memcpy(&depth, hdr + 4, 4); memcpy(&ch, hdr + 8, 4); memcpy(&ntex, hdr + 12, 4);
    if (depth < 2 || depth > TAGPU_R_MAXLAYERS || ch < 4 || ch > 256 || (ch & 3) || ntex == 0 || ntex > (1u << 24)) {
        rlog_2(who, "weight header out of range"); fclose(f); return 0;
    }
    w->depth = (int)depth; w->ch = (int)ch; w->ntex = (int)ntex;
    for (l = 0; l < depth; l++) {
        if (fread(&w->layer[l], 1, 16, f) != 16) { rlog_2(who, "weight header truncated"); fclose(f); return 0; }
        {
            const TAGPU_RLAYER* L = &w->layer[l];
            /* every product in 64 bits: a corrupt header must not wrap its way past the bound */
            unsigned long long end = (unsigned long long)L->offset + (unsigned long long)L->kout * L->kstride;
            /* THE BOUNDS THE REPACK AND THE KERNEL INDEX WITH, on values read
               from a FILE. A block holds the bias and 9 x jin mat4s, so
               `kstride` covers 4 + 36 x jin vec4s or the repack would read
               the next block as this one's weights. Layer l reads what l-1
               wrote, so its input tiles are the previous layer's output
               tiles; the first layer reads FILL's one tile (RGB and a zero);
               the last writes one tile, the kernel's LAST output; and no
               layer is wider than `ch`, which sizes the activations. */
            int first = l == 0, last = l + 1 == depth;
            if (L->jin == 0 || L->kout == 0 || L->kstride == 0 ||
                (unsigned long long)L->kstride < 4ull + 36ull * L->jin ||
                L->kstride > ntex || L->kout > ntex || end > ntex ||
                4u * L->jin > ch || 4u * L->kout > ch ||
                (first && L->jin != 1) || (last && L->kout != 1) ||
                (!first && L->jin != w->layer[l - 1].kout)) {
                rlog_2(who, "weight layer table inconsistent"); fclose(f); return 0;
            }
        }
    }
    want = (size_t)ntex * 16;
    w->body = (float*)malloc(want);
    if (!w->body || fread(w->body, 1, want, f) != want) {
        rlog_2(who, "weight body truncated"); free(w->body); w->body = NULL; fclose(f); return 0;
    }
    fclose(f);
    strncpy(w->name, model, sizeof w->name - 1); w->name[sizeof w->name - 1] = 0;
    _snprintf(b, sizeof b, "%s: %s: %dx%d, %d vec4 (%u KB)", who, path, w->depth, w->ch, w->ntex, (unsigned)(want >> 10));
    rlog(b);
    return 1;
}

/* ---- options (OPT_FILE, read once per process) ---- */
static void read_options(void)
{
    HANDLE h;
    char buf[256]; DWORD n = 0;
    s_opt.log = 0; s_opt.budget = DEFAULT_BUDGET_MS;
    h = CreateFileA(OPT_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
        char* p = buf;
        buf[n] = 0;
        while (*p) {
            char* q; int last;
            while (*p && *p <= ' ') p++;
            q = p;
            while (*q && *q > ' ') q++;
            last = (*q == 0);
            *q = 0;
            if (!lstrcmpiA(p, "log")) s_opt.log = 1;
            else if (!strncmp(p, "budget=", 7)) { double v = atof(p + 7); if (v >= 0.5 && v <= 100.0) s_opt.budget = v; }
            if (last) break;
            p = q + 1;
        }
    }
    CloseHandle(h);
}

/* RE-READ ALL, which the backend does from its own init -- so the options
   are picked up once per bring-up, not once per process: a `budget=` edited
   between two bring-ups takes effect. `load_weights` keeps a body it already
   holds, so a reload does not touch the 4 MB file again. */
int tagpu_rcore_reload(const char* who)
{
    int m;
    read_options();
    s_optRead = 1;
    for (m = 0; m < TAGPU_RM_N; m++) s_wOk[m] = load_weights(who, m);
    return s_wOk[TAGPU_RM_FULL];
}

int tagpu_rcore_ready(void) { return s_wOk[TAGPU_RM_FULL]; }

const TAGPU_RMODEL* tagpu_rcore_model(int which)
{
    return which >= 0 && which < TAGPU_RM_N && s_wOk[which] ? &s_w[which] : NULL;
}

const TAGPU_ROPT* tagpu_rcore_opt(void)
{
    if (!s_optRead) { read_options(); s_optRead = 1; }
    return &s_opt;
}

/* ---- tileable: classical.is_tileable on palette colours ---- */
int tagpu_rglsl_tileable(const unsigned char* px, int w, int h, const unsigned char* pal, int key)
{
    double lr = 0.0, tb = 0.0;
    int i, c;
    if (key >= 0) {
        for (i = 0; i < h; i++) if (px[i * w] == key || px[i * w + w - 1] == key) return 0;
        for (i = 0; i < w; i++) if (px[i] == key || px[(h - 1) * w + i] == key) return 0;
    }
    for (i = 0; i < h; i++) {
        const unsigned char* a = pal + px[i * w] * 4;
        const unsigned char* b = pal + px[i * w + w - 1] * 4;
        for (c = 0; c < 3; c++) lr += abs((int)a[c] - (int)b[c]);
    }
    for (i = 0; i < w; i++) {
        const unsigned char* u = pal + px[i] * 4;
        const unsigned char* d = pal + px[(h - 1) * w + i] * 4;
        for (c = 0; c < 3; c++) tb += abs((int)u[c] - (int)d[c]);
    }
    return lr / (3.0 * h) < TILEABLE_THR && tb / (3.0 * w) < TILEABLE_THR;
}

/* ---- the jobs ---- */
void tagpu_rcore_job_drop(TAGPU_RCORE* j)
{
    j->qn = 0; j->bn = 0; j->inflight = 0; j->pass = 0; j->band = 0; j->srcAct = 0;
    j->running = 0;
}

void tagpu_rcore_fail_all(TAGPU_RSCHED* s)
{
    int i;
    for (i = 0; i < TAGPU_R_MAXJOBS; i++)
        if (s->jobs[i].used) { s->jobs[i].failed = 1; tagpu_rcore_job_drop(&s->jobs[i]); }
}

/* tagpu_restore_core.h. THE ACTIVATIONS NEED NOTHING: they are scratch that
   the batch in flight owns, and the next batch's FILL overwrites them behind
   the backend's slice barrier, as it does after any batch. */
int tagpu_rcore_job_remap(TAGPU_RCORE* j, int (*map)(void* ctx, TAGPU_RGLSL_FRAME* f),
                          void* ctx, int* kept, int* requeued, int* dropped)
{
    int i, k = 0, b = 0;
    const int nb = j && j->used && j->inflight ? j->bn : 0;
    *kept = 0; *requeued = 0; *dropped = 0;
    if (!j || !j->used || j->failed) return 1;
    if (j->qn + nb > j->qcap) {
        TAGPU_RQF* nq = (TAGPU_RQF*)realloc(j->q, (size_t)(j->qn + nb) * sizeof *nq);
        if (!nq) return 0;
        j->q = nq; j->qcap = j->qn + nb;
    }
    for (i = 0; i < j->qn; i++) {
        if (!j->q[i].nb && map(ctx, &j->q[i].f)) j->q[k++] = j->q[i];
        else (*dropped)++;
    }
    for (i = 0; i < nb; i++) {
        if (!j->bf[i].nb && map(ctx, &j->bf[i].f)) j->bf[b++] = j->bf[i];
        else (*dropped)++;
    }
    /* the batch at the head, in the order it was taken: it was the oldest */
    if (b > 0) {
        memmove(j->q + b, j->q, (size_t)k * sizeof *j->q);
        memcpy(j->q, j->bf, (size_t)b * sizeof *j->q);
    }
    j->qn = k + b;
    if (nb > 0) { j->bn = 0; j->inflight = 0; j->pass = 0; j->band = 0; j->srcAct = 0; }
    *kept = k; *requeued = b;
    return 1;
}

TAGPU_RCORE* tagpu_rcore_job_new(TAGPU_RSCHED* s, const char* tag, int prio,
                                 int oneshot, int model, void* owner)
{
    TAGPU_RCORE* j = NULL;
    int i;
    char b[160];
    /* NO MODEL, NO JOB, and this guard is the header's promise rather than a
       precaution. Without it a backend that creates jobs before loading the
       weights gets a live job with `depth == 0`, and the sequencer then walks
       off the end of the model: FILL sets `pass = 1`, `pass <= depth` is
       `1 <= 0`, so the next draw falls into the OUT branch and evaluates
       `layer[depth - 1]` -- `layer[-1]`, which is the four ints in front of the
       array, read as an input-tile count. The guard lives here and not in a
       backend's init because this is the interface every backend shares, and
       the header promises it ("or the model is unusable"). */
    if (!tagpu_rcore_ready()) {
        _snprintf(b, sizeof b, "%s: %s: no model loaded, so no job", s->be->name, tag ? tag : "job");
        rlog(b);
        return NULL;
    }
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) if (!s->jobs[i].used) { j = &s->jobs[i]; break; }
    if (!j) { _snprintf(b, sizeof b, "%s: no free job slot", s->be->name); rlog(b); return NULL; }
    if (!tagpu_rcore_model(model)) {
        _snprintf(b, sizeof b, "%s: %s: model %d is not loaded, so full", s->be->name, tag ? tag : "job", model);
        rlog(b);
        model = TAGPU_RM_FULL;
    }
    memset(j, 0, sizeof *j);
    j->used = 1; j->prio = prio; j->oneshot = oneshot; j->owner = owner;
    j->model = model; j->m = &s_w[model];
    strncpy(j->tag, tag ? tag : "job", sizeof j->tag - 1);
    j->lastTallySlice = s->slice;
    return j;
}

void tagpu_rcore_job_free(TAGPU_RSCHED* s, TAGPU_RCORE* j)
{
    int i, any = 0;
    if (!j || !j->used) return;
    free(j->q);
    /* a timing result still in flight must not credit its time to whoever
       takes this slot next */
    for (i = 0; i < 2; i++) if (s->qJob[i] == j) s->qJob[i] = NULL;
    memset(j, 0, sizeof *j);
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) any |= s->jobs[i].used;
    /* the last job takes the scratch with it; the programs stay for the next map */
    if (!any && s->be->act_free) s->be->act_free();
}

/* the pad a queued frame's rect has on every side: a neighbourhood's window
   (tagpu_rcore_job_add_nbhd), a tiling frame's wrap, or none */
static int frame_pad(const TAGPU_RCORE* j, const TAGPU_RQF* p)
{
    return p->nb ? j->m->depth + p->f.border : p->f.wrap ? j->m->depth : 0;
}

/* room for `count` more queued frames; 0 out of memory */
static int queue_room(TAGPU_RSCHED* s, TAGPU_RCORE* j, int count)
{
    if (j->qn + count > j->qcap) {
        int cap = j->qcap ? j->qcap : 256;
        TAGPU_RQF* nq;
        while (cap < j->qn + count) cap *= 2;
        nq = (TAGPU_RQF*)realloc(j->q, (size_t)cap * sizeof *nq);
        if (!nq) { char b[64]; _snprintf(b, sizeof b, "%s: out of memory", s->be->name); rlog(b); return 0; }
        j->q = nq; j->qcap = cap;
    }
    return 1;
}

/* the frame at j->q[qn], filled by the caller, classed and taken -- or not,
   with the reason in tagpu.log: 1 taken */
static int queue_take(TAGPU_RSCHED* s, TAGPU_RCORE* j)
{
    TAGPU_RQF* p = &j->q[j->qn];
    int S, c;
    S = (p->f.w > p->f.h ? p->f.w : p->f.h) + 2 * frame_pad(j, p);
    if (S > TAGPU_R_ACTMAX && p->f.wrap) { p->f.wrap = 0; S = p->f.w > p->f.h ? p->f.w : p->f.h; }
    if (S > TAGPU_R_ACTMAX) {
        char b[160];
        _snprintf(b, sizeof b, "%s: %s: %dx%d frame exceeds the %d-texel slot, left indexed",
                  s->be->name, j->tag, p->f.w, p->f.h, TAGPU_R_ACTMAX);
        rlog(b);
        return 0;
    }
    for (c = 0; c < NCLASSES && s_classes[c] < S; c++) ;
    p->S = (short)S; p->cls = (short)c;
    j->qn++;
    return 1;
}

/* the run's start, once a call has queued something while the job was idle */
static void run_begin(TAGPU_RSCHED* s, TAGPU_RCORE* j)
{
    int i;
    if (j->running) return;
    j->running = 1; j->rframes = 0; j->rwrap = 0; j->rbatches = 0; j->rdraws = 0; j->rslices = 0;
    j->rt0 = now_ms(); j->rgpuNs = 0.0; j->rgpuUnits = 0.0; j->runits = 0.0;
    j->rcall0 = s->calls;
    if (j->oneshot) {
        char b[200];
        int w = 0, n = 0;
        for (i = 0; i < j->qn; i++) { w += j->q[i].f.wrap ? 1 : 0; n += j->q[i].nb ? 1 : 0; }
        _snprintf(b, sizeof b, "%s: %s: job started: %d frames (%d wrap-padded, %d neighbourhoods), model %s %dx%d",
                  s->be->name, j->tag, j->qn, w, n, j->m->name, j->m->depth, j->m->ch);
        rlog(b);
    }
}

int tagpu_rcore_job_add(TAGPU_RSCHED* s, TAGPU_RCORE* j,
                        const TAGPU_RGLSL_FRAME* frames, int count)
{
    int i, added = 0;
    if (!j || !j->used || j->failed || !frames || count <= 0) return 0;
    if (!queue_room(s, j, count)) return 0;
    for (i = 0; i < count; i++) {
        TAGPU_RQF* p = &j->q[j->qn];
        if (frames[i].w <= 0 || frames[i].h <= 0) continue;
        memset(p, 0, sizeof *p);
        p->f = frames[i];
        added += queue_take(s, j);
    }
    if (added) run_begin(s, j);
    return added;
}

int tagpu_rcore_job_add_nbhd(TAGPU_RSCHED* s, TAGPU_RCORE* j,
                             const TAGPU_RNBFRAME* frames, int count)
{
    int i, added = 0;
    if (!j || !j->used || j->failed || !frames || count <= 0) return 0;
    if (!queue_room(s, j, count)) return 0;
    for (i = 0; i < count; i++) {
        const TAGPU_RGLSL_FRAME* f = &frames[i].f;
        TAGPU_RQF* p = &j->q[j->qn];
        const int a = j->m->depth + f->border;
        /* THE BOUND the shaders' addressing rests on (tagpu_restore_comp.h
           nbAt): every window texel is in the centre or in ONE neighbour, and
           every cell texel OUT reads is in the window */
        if (f->w <= 0 || f->h <= 0) continue;
        if (f->border < 0 || a > f->w || a > f->h || f->padR || f->padB) {
            char b[200];
            _snprintf(b, sizeof b, "%s: %s: a %dx%d neighbourhood with border %d and slack %d,%d does not fit"
                                   " a %d-texel window, left indexed",
                      s->be->name, j->tag, f->w, f->h, f->border, f->padR, f->padB, a);
            rlog(b);
            continue;
        }
        memset(p, 0, sizeof *p);
        p->f = *f;
        p->f.wrap = 0; p->f.key = -1;
        p->nb = 1;
        p->edge = (short)(frames[i].edge & (TAGPU_RNB_L | TAGPU_RNB_R | TAGPU_RNB_T | TAGPU_RNB_B));
        memcpy(p->nbo, frames[i].nbo, sizeof p->nbo);
        added += queue_take(s, j);
    }
    if (added) run_begin(s, j);
    return added;
}

/* THE MIRROR RULE at the map's edge: cell -1 is cell 0 flipped, cell w is
   cell w - 1 -- the map reflected with its edge texel repeated. A window
   reaches one neighbour at most, so one reflection is all there is. */
static int nb_reflect(int x, int w) { return x < 0 ? -1 - x : x >= w ? 2 * w - 1 - x : x; }

int tagpu_rcore_nb_frame(TAGPU_RNBFRAME* o, int c, int r, int w, int h,
                         unsigned (*org)(void* ctx, int x, int y), void* ctx,
                         int tw, int th, int dx, int dy, int border)
{
    static const signed char dir[8][2] = { {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                           {1, 0}, {-1, 1}, {0, 1}, {1, 1} };
    unsigned o0;
    int k;
    if (c < 0 || r < 0 || c >= w || r >= h) return 0;
    memset(o, 0, sizeof *o);
    o0 = org(ctx, c, r);
    o->f.ax = (int)(o0 & 0xFFFFu); o->f.ay = (int)(o0 >> 16);
    o->f.w = tw; o->f.h = th;
    o->f.dx = dx; o->f.dy = dy; o->f.border = border;
    o->f.key = -1;
    o->edge = (c == 0 ? TAGPU_RNB_L : 0) | (c == w - 1 ? TAGPU_RNB_R : 0) |
              (r == 0 ? TAGPU_RNB_T : 0) | (r == h - 1 ? TAGPU_RNB_B : 0);
    for (k = 0; k < 8; k++)
        o->nbo[k] = org(ctx, nb_reflect(c + dir[k][0], w), nb_reflect(r + dir[k][1], h));
    return 1;
}

/* THE GRID of a batch of `cols` x `cols` slots of edge S: the pitch is S + 1
   (tagpu_restore_core.h, TAGPU_RDRAWREQ), padded to the kernel's tiling */
static int grid_w(int cols, int S)
{
    int g = cols * (S + 1);
    return (g + TAGPU_R_GRIDX - 1) / TAGPU_R_GRIDX * TAGPU_R_GRIDX;
}
static int grid_h(int cols, int S)
{
    int g = cols * (S + 1);
    return (g + TAGPU_R_GRIDY - 1) / TAGPU_R_GRIDY * TAGPU_R_GRIDY;
}

/* take the next batch off the queue: the head's size class, up to the grid
   that class allows, in queue order; the rest close up behind */
/* 1 = a batch is in flight, 0 = the job just failed, -1 = the backend needs a
   slice to settle and the queue is UNTOUCHED.

   IT CHOOSES, THEN SECURES THE SCRATCH, THEN COMMITS -- in that order, and the
   order is the point. The queue compaction is destructive, so calling
   `act_ensure` after it would leave a -1 with the batch already taken out of a
   queue it has to go back into. Two passes over the queue with the same
   predicate pick the same frames, so the batch is identical to the one a
   single destructive pass would produce; nothing is committed until the
   scratch exists. */
static int form_batch(TAGPU_RSCHED* s, TAGPU_RCORE* j)
{
    int cls, cols, cap, i, k = 0, S = 0, taken = 0, ok;
    if (j->qn == 0) return 0;
    cls = j->q[0].cls;
    cols = TAGPU_R_ACTMAX / s_classes[cls]; if (cols > TAGPU_R_SLOTCOLS) cols = TAGPU_R_SLOTCOLS;
    cap = cols * cols;
    /* choose */
    j->bn = 0;
    for (i = 0; i < j->qn; i++) {
        if (j->q[i].cls == cls && j->bn < cap) {
            j->bf[j->bn++] = j->q[i];
            if (j->q[i].S > S) S = j->q[i].S;
        }
    }
    /* the grid is the smallest square that holds the batch: the passes cost
       by the texel, and a queue's two-frame batch must not pay for 64 */
    for (cols = 1; cols * cols < j->bn; cols++) ;
    /* secure */
    ok = s->be->act_ensure(grid_w(cols, S), grid_h(cols, S));
    if (ok < 0) { j->bn = 0; return -1; }        /* the queue is as it was */
    if (!ok) { j->failed = 1; tagpu_rcore_job_drop(j); return 0; }
    /* commit: the same predicate, so the same frames */
    for (i = 0; i < j->qn; i++) {
        if (j->q[i].cls == cls && taken < cap) { taken++; continue; }
        j->q[k++] = j->q[i];
    }
    j->qn = k;
    j->bS = S; j->bcols = cols;
    j->pass = 0; j->band = 0; j->srcAct = 0;
    j->inflight = 1;
    return 1;
}

/* ONE DISPATCH of the job's in-flight batch. The core decides WHICH, builds
   the slot table, computes its cost in work units (texels x input tiles x
   output tiles for a conv band) and only then hands it to the backend; the
   sequencer and the cost model stay here so a second backend cannot disagree
   with the first about either. */
static double issue_draw(TAGPU_RSCHED* s, TAGPU_RCORE* j)
{
    int S = j->bS, cols = j->bcols;
    TAGPU_RDRAWREQ r;
    double units;

    memset(&r, 0, sizeof r);
    r.job = j; r.S = S; r.cols = cols; r.pitch = S + 1;
    r.gw = grid_w(cols, S); r.gh = grid_h(cols, S);
    r.slot = s->slot; r.nframes = j->bn;

    if (j->pass == 0) {
        int t;
        memset(s->slot, 0, sizeof s->slot);
        for (t = 0; t < TAGPU_R_BATCH; t++) s->slot[t].key = -1;
        for (t = 0; t < j->bn; t++) {
            const TAGPU_RGLSL_FRAME* f = &j->bf[t].f;
            TAGPU_RSLOT* o = &s->slot[(t / cols) * TAGPU_R_SLOTCOLS + (t % cols)];
            int p = frame_pad(j, &j->bf[t]);
            o->rw = f->w + 2 * p; o->rh = f->h + 2 * p;
            o->ax = f->ax; o->ay = f->ay; o->sw = f->w; o->sh = f->h;
            o->key = f->key;
            o->dx = f->dx; o->dy = f->dy; o->border = f->border;
            o->padR = f->padR; o->padB = f->padB;
            o->nb = j->bf[t].nb; o->edge = j->bf[t].edge;
            memcpy(o->nbo, j->bf[t].nbo, sizeof o->nbo);
        }
        r.kind = TAGPU_RDRAW_FILL;
        if (!s->be->draw(&r)) { j->failed = 1; tagpu_rcore_job_drop(j); return 0.0; }
        j->pass = 1; j->band = 0; j->srcAct = 0;
        units = (double)r.gw * r.gh;
    } else if (j->pass <= j->m->depth) {
        const TAGPU_RLAYER* L = &j->m->layer[j->pass - 1];
        int last = j->pass == j->m->depth;
        r.kind = TAGPU_RDRAW_CONV;
        r.layer = j->pass - 1; r.L = L; r.last = last; r.srcAct = j->srcAct;
        r.y0 = j->band * TAGPU_R_BAND;
        r.rows = r.gh - r.y0 < TAGPU_R_BAND ? r.gh - r.y0 : TAGPU_R_BAND;
        if (!s->be->draw(&r)) { j->failed = 1; tagpu_rcore_job_drop(j); return 0.0; }
        /* the last layer's output tile is padded to two (the kernel's LAST
           computes eight channels), and the cost says what the GPU was asked */
        units = (double)r.gw * r.rows * L->jin * (last ? 2 : L->kout);
        j->band++;
        if (j->band * TAGPU_R_BAND >= r.gh) { j->band = 0; j->srcAct = 1 - j->srcAct; j->pass++; }
    } else {
        int t;
        r.kind = TAGPU_RDRAW_OUT;
        r.srcAct = j->srcAct;
        if (!s->be->draw(&r)) { j->failed = 1; tagpu_rcore_job_drop(j); return 0.0; }
        units = (double)j->bn * (S + 2) * (S + 2);
        j->rbatches++; j->tbatches++;
        for (t = 0; t < j->bn; t++) { j->rframes++; j->tframes++; if (j->bf[t].f.wrap) j->rwrap++; }
        if (s_opt.log) {
            char b[200];
            _snprintf(b, sizeof b, "%s: %s: batch %d (S%d, %dx%d slots, %d frames) issued at slice %u, %d queued",
                      s->be->name, j->tag, j->rbatches, S, cols, cols, j->bn, s->slice, j->qn);
            rlog(b);
        }
        j->inflight = 0; j->bn = 0; j->pass = 0;
    }
    j->rdraws++; j->tdraws++;
    return units;
}

void tagpu_rcore_job_budget(TAGPU_RCORE* j, int prio, double capMs)
{
    if (!j || !j->used) return;
    j->prio = prio;
    j->cap = capMs > 0.0 ? capMs : 0.0;
}

/* 1 when a job other than `j` has frames queued and may be picked */
static int others_waiting(const TAGPU_RSCHED* s, const TAGPU_RCORE* j)
{
    int i;
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) {
        const TAGPU_RCORE* o = &s->jobs[i];
        if (o == j || !o->used || o->failed || o->qn == 0) continue;
        if (s->gate && o->prio >= 0) continue;
        if (o->capOut == s->slice + 1) continue;
        return 1;
    }
    return 0;
}

/* the job whose batch is in flight, else the lowest prio with a queue that is
   not at its cap this slice. Under the self-test's gate a job of non-negative
   prio is not a candidate at all. */
static TAGPU_RCORE* pick_job(TAGPU_RSCHED* s)
{
    TAGPU_RCORE* best = NULL;
    int i;
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) {
        TAGPU_RCORE* j = &s->jobs[i];
        if (j->used && !j->failed && j->inflight && (!s->gate || j->prio < 0)) return j;
    }
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) {
        TAGPU_RCORE* j = &s->jobs[i];
        if (!j->used || j->failed || j->qn == 0) continue;
        if (s->gate && j->prio >= 0) continue;
        if (j->capOut == s->slice + 1) continue;     /* at its cap this slice */
        if (!best || j->prio < best->prio) best = j;
    }
    return best;
}

/* a run drained: the terrain's "done" line, or a queue's tally */
static void job_drained(TAGPU_RSCHED* s, TAGPU_RCORE* j)
{
    char b[420];
    double wall = now_ms() - j->rt0;
    unsigned frames = s->calls - j->rcall0 + 1;     /* this frame included */
    j->running = 0;
    if (j->oneshot) {
        /* fps = frames the game drew while the run lasted (every step call,
           drawn in or not) over the wall; the GPU figure is the slices this
           job started, so its share says how much of the work it timed */
        _snprintf(b, sizeof b, "%s: %s: done: %d frames (%d wrap-padded) in %d batches, %d dispatches in %d of %u frames = %.0f ms wall since begin (%.1f fps while restoring); GPU %.0f ms measured over %.0f%% of the work; %s %dx%d fp32 budget %.0f ms",
                  s->be->name, j->tag, j->rframes, j->rwrap, j->rbatches, j->rdraws, j->rslices, frames, wall,
                  wall > 0.0 ? 1000.0 * frames / wall : 0.0, j->rgpuNs / 1e6,
                  j->runits > 0.0 ? 100.0 * j->rgpuUnits / j->runits : 0.0,
                  j->m->name, j->m->depth, j->m->ch, s_opt.budget);
        rlog(b);
    } else if (s_opt.log || s->slice - j->lastTallySlice >= QUEUE_LOG_FRAMES) {
        j->lastTallySlice = s->slice;
        _snprintf(b, sizeof b, "%s: %s: queue drained: %d frames in %d batches this run, %u frames from the first queued to the last painted (%.0f ms, %d slices drawn in); %d frames, %d batches, %d dispatches so far",
                  s->be->name, j->tag, j->rframes, j->rbatches, frames, wall, j->rslices,
                  j->tframes, j->tbatches, j->tdraws);
        rlog(b);
    }
}

void tagpu_rcore_step(TAGPU_RSCHED* s)
{
    double allowed, spent = 0.0;
    int q, ndraw = 0, i;
    TAGPU_RCORE* j;
    char b[300];

    s->calls++;
    if (!s->be->ready()) return;
    /* LAST slice's GPU time -> the cost estimate. The query before the newest
       is the one read, so nothing here ever waits on a result. */
    if (s->timer) {
        q = (s->qFrame + 1) & 1;
        if (s->qHave[q]) {
            double ns = 0.0;
            int got = s->be->timer_poll(q, &ns);
            if (got > 0) {
                s->qStall = 0;
                s->qHave[q] = 0;
                if (s->qUnits[q] > 0.0) {
                    double est = ns / s->qUnits[q];
                    s->nsPerUnit = s->nsPerUnit > 0.0 ? 0.5 * (s->nsPerUnit + est) : est;
                    if (s->qJob[q] && s->qJob[q]->used) {
                        s->qJob[q]->rgpuNs += ns; s->qJob[q]->rgpuUnits += s->qUnits[q];
                        s->qJob[q]->tgpuNs += ns;
                    }
                }
            } else if (++s->qStall < STALL_GIVEUP) {
                return;                        /* the GPU is a frame behind: add nothing */
            } else {
                /* a result that never comes is a device that does not really
                   time: fixed slices from here rather than wait forever */
                _snprintf(b, sizeof b, "%s: the GPU timer never completed; fixed slices from here", s->be->name);
                rlog(b);
                if (s->be->timer_off) s->be->timer_off();
                s->timer = 0; s->qHave[0] = s->qHave[1] = 0;
            }
        }
    }
    if (s->be->may_draw && !s->be->may_draw()) return;   /* paused: the queues keep filling */
    j = pick_job(s);
    if (!j) {
        /* Nothing to do: after a while the scratch goes and the queues stay.
           The backend is ASKED every idle frame past the threshold rather than
           the core tracking whether a scratch exists -- `act_free` answers 0
           when there is nothing to release, which is what the log hangs off,
           and keeping the "is there one" question on the side that owns the
           memory is what stops the two copies of it drifting. */
        if (++s->idle >= IDLE_FRAMES) {
            for (i = 0; i < TAGPU_R_MAXJOBS; i++) if (s->jobs[i].used && s->jobs[i].inflight) return;
            if (s->be->act_free && s->be->act_free() && s_opt.log) {
                _snprintf(b, sizeof b, "%s: idle: activations freed", s->be->name);
                rlog(b);
            }
        }
        return;
    }
    s->idle = 0;
    allowed = (s->timer && s->nsPerUnit > 0.0) ? s_opt.budget * 1e6 / s->nsPerUnit : 0.0;

    q = s->qFrame & 1;
    if (s->timer) { s->be->slice_begin(q); s->qJob[q] = j; }
    do {
        /* every batch boundary re-picks, so a higher-priority job that
           gained work takes the budget as soon as the batch in flight lands */
        if (!j->inflight) {
            j = pick_job(s);
            if (!j) break;
            if (!j->inflight) {
                int fb = form_batch(s, j);
                if (fb < 0) break;               /* the backend needs a slice to settle */
                if (!fb) continue;               /* it just failed: another */
            }
        }
        if (j->sliceMark != s->slice + 1) { j->sliceMark = s->slice + 1; j->rslices++; }
        {
            double u = issue_draw(s, j);
            spent += u; j->runits += u;
            if (j->cap > 0.0) {
                if (j->capSlice != s->slice + 1) { j->capSlice = s->slice + 1; j->capUnits = 0.0; j->capDraws = 0; }
                j->capUnits += u; j->capDraws++;
            }
        }
        ndraw++;
        if (!j->inflight && j->qn == 0 && j->running) job_drained(s, j);
        if (s->timer) { if (s->nsPerUnit <= 0.0 && ndraw >= 2) break; if (spent >= allowed) break; }
        else if (ndraw >= FIXED_DRAWS) break;
        /* A CAPPED JOB STOPS AT ITS CAP, and what it spent is counted across
           the slice, so re-picking it after a batch lands does not reset it.
           At the cap it is passed over for the rest of the slice (`pick_job`)
           and the slice goes on for the other jobs, or ends when there are
           none. Its batch in flight is the exception while another job waits:
           `pick_job` returns it first, and that batch is all that stands
           between the other and the GPU. */
        if (j->cap > 0.0 && j->capSlice == s->slice + 1 &&
            (s->timer ? s->nsPerUnit > 0.0 && j->capUnits >= j->cap * 1e6 / s->nsPerUnit
                      : j->capDraws >= FIXED_CAP_DRAWS))
            j->capOut = s->slice + 1;
        if (j->capOut == s->slice + 1 && (j->inflight ? !others_waiting(s, j) : !pick_job(s)))
            break;
    } while (1);
    if (s->timer) { s->be->slice_end(q); s->qUnits[q] = spent; s->qHave[q] = 1; s->qFrame++; }
    s->slice++;
}

void tagpu_rcore_lost(TAGPU_RSCHED* s)
{
    int i;
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) {
        free(s->jobs[i].q);
        memset(&s->jobs[i], 0, sizeof s->jobs[i]);
    }
    s->qHave[0] = s->qHave[1] = 0; s->qJob[0] = s->qJob[1] = NULL;
    s->qFrame = 0; s->qStall = 0; s->nsPerUnit = 0.0;
    s->slice = 0; s->idle = 0; s->timer = 0; s->gate = 0;
}
