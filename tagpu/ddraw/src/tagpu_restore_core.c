/* tagpu_restore_core.c -- the Classic++ restorer's scheduler, with no
   rendering API in it. tagpu_restore_core.h says why the module is split this
   way; research/notes/renderers.md 4c has the decisions the numbers come from,
   and tools/tascene-restore.js is the same driver in the browser lab.

   THE MODEL. unditherer/model.py: 3x3 conv 3->64 + ReLU, ten x (3x3 conv
   64->64 + BatchNorm + ReLU), 3x3 conv 64->3, out = in - net(in), RGB in
   [0,1]. BatchNorm is folded into the weight file (unditherer/weights.py),
   which lays every layer out as the conv pass indexes it: one std140 block of
   mat4 per output channel-tile k, bias first, then a mat4 per (tap, input
   tile). A conv draw binds NK consecutive k-blocks as one uniform range and
   writes NK output tiles through NK colour attachments; the activations
   ping-pong between two array textures, one layer per four channels.

   THE PADDING RULE, the thing that defines the pixels. The unditherer runs a
   tile whose opposite edges agree within 12 levels wrap-padded by 12 to 56x56
   and centre-cropped, and every other tile at 32x32 with zero padding at every
   layer. Here both are one mechanism: every slot has a valid rect and a tap
   outside it reads 0, AT EVERY LAYER -- because zero-padding only the INPUT
   and running unmasked is a different network: layer 2 would read layer 1's
   gutter, which is relu(bias), not 0. FILL wrap-pads inside the rect; OUT
   centre-crops.

   BATCHES, shared with the lab byte for byte (tascene-restore.js
   batchFrames): a frame's padded edge S is max(w, h) + 2 x depth if it wraps.
   A job's queue is taken in order; a batch holds frames of one SIZE CLASS up
   to a square slot grid of min(8, ACT_MAX / class) per side -- then shrunk to
   the smallest square that holds what was taken, since the passes cost by the
   fragment -- and its slot pitch is its largest S. Every slot has its own
   rect, so a 30x25 tree and a 63x60 rock share a batch.

   SLICING. One call per frame issues draws until an estimate of their GPU time
   reaches the budget (12 ms by default), the estimate being the previous
   slice's measured time divided by the work it carried. Draws are the unit: a
   64-tile batch of the full model is 1 (fill) + ceil(16/NK) x 11 + 1 + 1 (out)
   = 47 of them at NK=4. Where the device will not time a slice, it is a fixed,
   conservative draw count instead. No second context, no worker thread: the
   GPU is the only engine, and a slow one restores slower without stalling
   anything. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tagpu_restore_core.h"

#define MAX_PATH_B        260
#define TILEABLE_THR      12.0
#define OPT_FILE          "tagpu_restoreglsl.on"
#define DEFAULT_BUDGET_MS 12.0
#define FIXED_DRAWS       6     /* no timer: draws per slice                   */
#define IDLE_FRAMES       180   /* activations freed after                     */
#define QUEUE_LOG_FRAMES  300   /* a queue's tally, at most                    */
#define STALL_GIVEUP      300   /* slices waited on one result before giving up */

/* the size-class ladder: a batch holds one class; tascene-restore.js's twin */
static const int s_classes[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512 };
#define NCLASSES (int)(sizeof s_classes / sizeof s_classes[0])

static void rlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
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

/* ---- the weight file (unditherer/weights.py) ---- */
static TAGPU_RMODEL s_w;
static TAGPU_ROPT   s_opt;
static int          s_optRead, s_modelOk;

static int load_weights(const char* who, const char* model)
{
    char path[MAX_PATH_B], b[300];
    FILE* f;
    unsigned char hdr[16];
    unsigned depth, ch, ntex, l;
    size_t want;

    if (s_w.body && !strcmp(s_w.name, model)) return 1;
    free(s_w.body); s_w.body = NULL;
    /* beside TotalA.exe, where tacli links it */
    path[0] = 0;
    if (GetModuleFileNameA(NULL, path, sizeof path)) {
        char* p = strrchr(path, '\\');
        if (p && (size_t)(p - path) + 24 < sizeof path) _snprintf(p + 1, 24, "%s.w32.bin", model);
        else path[0] = 0;
    }
    if (!path[0]) _snprintf(path, sizeof path, "%s.w32.bin", model);
    f = fopen(path, "rb");
    if (!f) { _snprintf(b, sizeof b, "%s: no %s -- Classic++ stays indexed", who, path); rlog(b); return 0; }
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "TAW1", 4)) { rlog_2(who, "weight file is not TAW1"); fclose(f); return 0; }
    memcpy(&depth, hdr + 4, 4); memcpy(&ch, hdr + 8, 4); memcpy(&ntex, hdr + 12, 4);
    if (depth < 2 || depth > TAGPU_R_MAXLAYERS || ch < 4 || ch > 256 || (ch & 3) || ntex == 0 || ntex > (1u << 24)) {
        rlog_2(who, "weight header out of range"); fclose(f); return 0;
    }
    s_w.depth = (int)depth; s_w.ch = (int)ch; s_w.ntex = (int)ntex; s_w.kmax = 0;
    for (l = 0; l < depth; l++) {
        if (fread(&s_w.layer[l], 1, 16, f) != 16) { rlog_2(who, "weight header truncated"); fclose(f); return 0; }
        {
            const TAGPU_RLAYER* L = &s_w.layer[l];
            /* every product in 64 bits: a corrupt header must not wrap its way past the bound */
            unsigned long long end = (unsigned long long)L->offset + (unsigned long long)L->kout * L->kstride;
            /* AND BOTH TERMS OF THE BIND OFFSET ARE ALIGNED, which is a bound on
               a value read from a FILE and therefore belongs here rather than at
               the bind site. A conv draw binds the weight block at
               `(offset + group x kstride) x 16` bytes, and Vulkan requires that
               to be a multiple of the device's minUniformBufferOffsetAlignment,
               which the spec caps at 256: the reference setup's device reports
               64 (16 on llvmpipe; measured through winevulkan 2026-09-17).
               `kstride & 15` below already makes the
               group term a multiple of 256; `offset & 15` is what makes the
               base term one. Both shipped models pass it by construction --
               the exporter lays layers back to back from 0, so every offset is
               a running sum of `kout x kstride` and therefore a multiple of 16
               -- so this is a bound on the file, not a fix for the exporter. */
            if (L->jin == 0 || L->kout == 0 || L->kstride == 0 || (L->kstride & 15) ||
                (L->offset & 15) ||
                L->kstride > ntex || L->kout > ntex || end > ntex || L->jin > 64) {
                rlog_2(who, "weight layer table inconsistent"); fclose(f); return 0;
            }
        }
        if ((int)(s_w.layer[l].kstride / 4) > s_w.kmax) s_w.kmax = (int)(s_w.layer[l].kstride / 4);
    }
    want = (size_t)ntex * 16;
    s_w.body = (float*)malloc(want);
    if (!s_w.body || fread(s_w.body, 1, want, f) != want) {
        rlog_2(who, "weight body truncated"); free(s_w.body); s_w.body = NULL; fclose(f); return 0;
    }
    fclose(f);
    strncpy(s_w.name, model, sizeof s_w.name - 1); s_w.name[sizeof s_w.name - 1] = 0;
    _snprintf(b, sizeof b, "%s: %s: %dx%d, %d vec4 (%u KB)", who, path, s_w.depth, s_w.ch, s_w.ntex, (unsigned)(want >> 10));
    rlog(b);
    return 1;
}

/* ---- options (OPT_FILE, read once per process) ---- */
static void read_options(void)
{
    HANDLE h;
    char buf[256]; DWORD n = 0;
    s_opt.tiny = 0; s_opt.fp16 = 0; s_opt.nk = 0; s_opt.log = 0; s_opt.budget = DEFAULT_BUDGET_MS;
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
            if (!lstrcmpiA(p, "tiny")) s_opt.tiny = 1;
            else if (!lstrcmpiA(p, "fp16")) s_opt.fp16 = 1;
            else if (!lstrcmpiA(p, "log")) s_opt.log = 1;
            else if (!strncmp(p, "nk=", 3)) { int k = atoi(p + 3); if (k == 1 || k == 2 || k == 4 || k == 8) s_opt.nk = k; }
            else if (!strncmp(p, "budget=", 7)) { double v = atof(p + 7); if (v >= 0.5 && v <= 100.0) s_opt.budget = v; }
            if (last) break;
            p = q + 1;
        }
    }
    CloseHandle(h);
}

/* RE-READ BOTH, which the backend does from its own init -- so the options
   are picked up once per bring-up, not once per process: a `tiny` or `budget=`
   edited between two bring-ups takes effect. `load_weights` re-reads only when
   the model NAME changed, so flipping `tiny` reloads and a plain reload does
   not touch the 4 MB body. */
int tagpu_rcore_reload(const char* who)
{
    read_options();
    s_optRead = 1;
    s_modelOk = load_weights(who, s_opt.tiny ? "tiny" : "full");
    return s_modelOk;
}

int tagpu_rcore_ready(void) { return s_modelOk; }

const TAGPU_RMODEL* tagpu_rcore_model(void) { return &s_w; }

const TAGPU_ROPT* tagpu_rcore_opt(void)
{
    if (!s_optRead) { read_options(); s_optRead = 1; }
    return &s_opt;
}

int tagpu_rcore_pick_nk(TAGPU_RSCHED* s, int maxUniformBlockBytes, int maxAttachments)
{
    char b[200];
    int kbytes, nk;
    if (!s_modelOk) return 0;
    kbytes = s_w.kmax * 64;
    nk = kbytes > 0 ? maxUniformBlockBytes / kbytes : 0;
    if (nk > maxAttachments) nk = maxAttachments;
    if (nk > TAGPU_R_MAXNK) nk = TAGPU_R_MAXNK;
    nk = nk >= 8 ? 8 : nk >= 4 ? 4 : nk >= 2 ? 2 : nk >= 1 ? 1 : 0;
    if (!nk) {
        _snprintf(b, sizeof b, "%s: uniform block %d < one k-block (%d)",
                  s->be->name, maxUniformBlockBytes, kbytes);
        rlog(b);
        return 0;
    }
    if (s_opt.nk && s_opt.nk <= nk) nk = s_opt.nk;
    s->nk = nk; s->wmax = nk * s_w.kmax;
    return nk;
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
    j->qn = 0; j->bn = 0; j->inflight = 0; j->pass = 0; j->group = 0; j->srcAct = 0;
    j->running = 0;
}

TAGPU_RCORE* tagpu_rcore_job_new(TAGPU_RSCHED* s, const char* tag, int prio,
                                 int oneshot, void* owner)
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
    memset(j, 0, sizeof *j);
    j->used = 1; j->prio = prio; j->oneshot = oneshot; j->owner = owner;
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

int tagpu_rcore_job_add(TAGPU_RSCHED* s, TAGPU_RCORE* j,
                        const TAGPU_RGLSL_FRAME* frames, int count)
{
    int i, added = 0;
    if (!j || !j->used || j->failed || !frames || count <= 0) return 0;
    if (j->qn + count > j->qcap) {
        int cap = j->qcap ? j->qcap : 256;
        TAGPU_RQF* nq;
        while (cap < j->qn + count) cap *= 2;
        nq = (TAGPU_RQF*)realloc(j->q, (size_t)cap * sizeof *nq);
        if (!nq) { char b[64]; _snprintf(b, sizeof b, "%s: out of memory", s->be->name); rlog(b); return 0; }
        j->q = nq; j->qcap = cap;
    }
    for (i = 0; i < count; i++) {
        TAGPU_RQF* p = &j->q[j->qn];
        int S, c;
        if (frames[i].w <= 0 || frames[i].h <= 0) continue;
        p->f = frames[i];
        S = (p->f.w > p->f.h ? p->f.w : p->f.h) + (p->f.wrap ? 2 * s_w.depth : 0);
        if (S > TAGPU_R_ACTMAX && p->f.wrap) { p->f.wrap = 0; S = p->f.w > p->f.h ? p->f.w : p->f.h; }
        if (S > TAGPU_R_ACTMAX) {
            char b[160];
            _snprintf(b, sizeof b, "%s: %s: %dx%d frame exceeds the %d-texel slot, left indexed",
                      s->be->name, j->tag, p->f.w, p->f.h, TAGPU_R_ACTMAX);
            rlog(b);
            continue;
        }
        for (c = 0; c < NCLASSES && s_classes[c] < S; c++) ;
        p->S = (short)S; p->cls = (short)c;
        j->qn++; added++;
    }
    if (added && !j->running) {
        j->running = 1; j->rframes = 0; j->rwrap = 0; j->rbatches = 0; j->rdraws = 0; j->rslices = 0;
        j->rt0 = now_ms(); j->rgpuNs = 0.0; j->rgpuUnits = 0.0; j->runits = 0.0;
        j->rcall0 = s->calls;
        if (j->oneshot) {
            char b[200];
            int w = 0;
            for (i = 0; i < j->qn; i++) w += j->q[i].f.wrap ? 1 : 0;
            _snprintf(b, sizeof b, "%s: %s: job started: %d frames (%d wrap-padded), model %dx%d",
                      s->be->name, j->tag, j->qn, w, s_w.depth, s_w.ch);
            rlog(b);
        }
    }
    return added;
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
       by the fragment, and a queue's two-frame batch must not pay for 64 */
    for (cols = 1; cols * cols < j->bn; cols++) ;
    /* secure */
    ok = s->be->act_ensure(cols * S);
    if (ok < 0) { j->bn = 0; return -1; }        /* the queue is as it was */
    if (!ok) { j->failed = 1; tagpu_rcore_job_drop(j); return 0; }
    /* commit: the same predicate, so the same frames */
    for (i = 0; i < j->qn; i++) {
        if (j->q[i].cls == cls && taken < cap) { taken++; continue; }
        j->q[k++] = j->q[i];
    }
    j->qn = k;
    j->bS = S; j->bcols = cols;
    j->pass = 0; j->group = 0; j->srcAct = 0;
    j->inflight = 1;
    return 1;
}

/* ONE DRAW of the job's in-flight batch. The core decides WHICH, builds the
   tables it needs, computes its cost in work units (texels x input tiles x NK)
   and only then hands it to the backend; the sequencer and the cost model stay
   here so a second backend cannot disagree with the first about either. */
static double issue_draw(TAGPU_RSCHED* s, TAGPU_RCORE* j)
{
    int S = j->bS, cols = j->bcols, TW = cols * S, TH = cols * S;
    TAGPU_RDRAWREQ r;
    double units;

    memset(&r, 0, sizeof r);
    r.job = j; r.S = S; r.TW = TW; r.TH = TH;

    if (j->pass == 0) {
        int t;
        memset(s->rect, 0, sizeof s->rect); memset(s->src, 0, sizeof s->src);
        for (t = 0; t < TAGPU_R_BATCH; t++) {
            s->key[t * 4] = -1.f;
            s->key[t * 4 + 1] = s->key[t * 4 + 2] = s->key[t * 4 + 3] = 0.f;
        }
        for (t = 0; t < j->bn; t++) {
            const TAGPU_RGLSL_FRAME* f = &j->bf[t].f;
            int p = f->wrap ? s_w.depth : 0;
            int x = (t / cols) * TAGPU_R_SLOTCOLS + (t % cols);   /* the 8x8 table's texel */
            s->rect[x * 4 + 2] = (float)(f->w + 2 * p); s->rect[x * 4 + 3] = (float)(f->h + 2 * p);
            s->src[x * 4] = (float)f->ax; s->src[x * 4 + 1] = (float)f->ay;
            s->src[x * 4 + 2] = (float)f->w; s->src[x * 4 + 3] = (float)f->h;
            s->key[x * 4] = (float)f->key;
        }
        r.kind = TAGPU_RDRAW_FILL;
        r.rect = s->rect; r.src = s->src; r.key = s->key;
        if (!s->be->draw(&r)) { j->failed = 1; tagpu_rcore_job_drop(j); return 0.0; }
        j->pass = 1; j->group = 0; j->srcAct = 0;
        units = (double)TW * TH;
    } else if (j->pass <= s_w.depth) {
        const TAGPU_RLAYER* L = &s_w.layer[j->pass - 1];
        int n = (int)L->kout - j->group; if (n > s->nk) n = s->nk;
        r.kind = TAGPU_RDRAW_CONV;
        r.rect = s->rect;
        r.L = L; r.group = j->group; r.n = n; r.srcAct = j->srcAct;
        r.relu = j->pass < s_w.depth ? 1 : 0;
        if (!s->be->draw(&r)) { j->failed = 1; tagpu_rcore_job_drop(j); return 0.0; }
        /* the cost is the FULL NK the draw was set up for, not the `n` tiles
           the tail of a layer actually needs: the estimate has to describe
           what the GPU was asked to do or the budget drifts on every layer
           whose kout is not a multiple of NK */
        units = (double)TW * TH * L->jin * s->nk;
        j->group += s->nk;
        if (j->group >= (int)L->kout) { j->group = 0; j->srcAct = 1 - j->srcAct; j->pass++; }
    } else {
        int t, nv = 0;
        for (t = 0; t < j->bn; t++) {
            const TAGPU_RGLSL_FRAME* f = &j->bf[t].f;
            float x0 = (float)(f->dx - f->border), y0 = (float)(f->dy - f->border);
            float x1 = (float)(f->dx + f->w + f->border + f->padR), y1 = (float)(f->dy + f->h + f->border + f->padB);
            float sc = (float)(t % cols), sr = (float)(t / cols);
            float xs[6] = { x0, x1, x0, x1, x1, x0 }, ys[6] = { y0, y0, y1, y0, y1, y1 };
            int k;
            for (k = 0; k < 6; k++) {
                float* o = s->verts + (size_t)nv * 8;
                o[0] = xs[k]; o[1] = ys[k]; o[2] = (float)f->dx; o[3] = (float)f->dy;
                o[4] = sc; o[5] = sr; o[6] = (float)f->w; o[7] = (float)f->h;
                nv++;
            }
        }
        r.kind = TAGPU_RDRAW_OUT;
        r.rect = s->rect; r.src = s->src; r.key = s->key;
        r.verts = s->verts; r.nv = nv; r.srcAct = j->srcAct;
        if (!s->be->draw(&r)) { j->failed = 1; tagpu_rcore_job_drop(j); return 0.0; }
        units = (double)j->bn * (S + 2) * (S + 2) * s_w.layer[s_w.depth - 1].jin;
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

/* the job whose batch is in flight, else the lowest prio with a queue */
static TAGPU_RCORE* pick_job(TAGPU_RSCHED* s)
{
    TAGPU_RCORE* best = NULL;
    int i;
    for (i = 0; i < TAGPU_R_MAXJOBS; i++)
        if (s->jobs[i].used && !s->jobs[i].failed && s->jobs[i].inflight) return &s->jobs[i];
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) {
        TAGPU_RCORE* j = &s->jobs[i];
        if (!j->used || j->failed || j->qn == 0) continue;
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
        _snprintf(b, sizeof b, "%s: %s: done: %d frames (%d wrap-padded) in %d batches, %d draws in %d of %u frames = %.0f ms wall since begin (%.1f fps while restoring); GPU %.0f ms measured over %.0f%% of the work; %dx%d %s NK=%d budget %.0f ms",
                  s->be->name, j->tag, j->rframes, j->rwrap, j->rbatches, j->rdraws, j->rslices, frames, wall,
                  wall > 0.0 ? 1000.0 * frames / wall : 0.0, j->rgpuNs / 1e6,
                  j->runits > 0.0 ? 100.0 * j->rgpuUnits / j->runits : 0.0,
                  s_w.depth, s_w.ch, s_opt.fp16 ? "fp16" : "fp32", s->nk, s_opt.budget);
        rlog(b);
    } else if (s_opt.log || s->slice - j->lastTallySlice >= QUEUE_LOG_FRAMES) {
        j->lastTallySlice = s->slice;
        _snprintf(b, sizeof b, "%s: %s: queue drained: %d frames in %d batches this run, %u frames from the first queued to the last painted (%.0f ms, %d slices drawn in); %d frames, %d batches, %d draws so far",
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

    s->be->state_push(s->slice);
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
        }
        ndraw++;
        if (!j->inflight && j->qn == 0 && j->running) job_drained(s, j);
        if (s->timer) { if (s->nsPerUnit <= 0.0 && ndraw >= 2) break; if (spent >= allowed) break; }
        else if (ndraw >= FIXED_DRAWS) break;
    } while (1);
    if (s->timer) { s->be->slice_end(q); s->qUnits[q] = spent; s->qHave[q] = 1; s->qFrame++; }
    s->slice++;
    s->be->state_pop(s->slice);
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
    s->slice = 0; s->idle = 0; s->timer = 0;
}
