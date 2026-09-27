#ifndef TAGPU_RESTORE_CORE_H
#define TAGPU_RESTORE_CORE_H
/* tagpu_restore_core.h -- the Classic++ restorer's API-INDEPENDENT half, and
   the line a rendering backend implements.

   WHY THIS FILE EXISTS, which is a fact about the tree rather than a taste.
   The restorer is two unrelated jobs: an incremental background SCHEDULER
   (job queues, batch formation, a GPU-time budget driven by a smoothed cost
   estimate) and a DISPATCH SEQUENCE. Only the second names an API; the first is
   called from API-free gather halves (`tagpu_rglsl_tileable` from
   tagpu_terr.c and tagpu_gaf.c), and the queues are what every consumer's
   lazy-restore contract is written against.

   WHAT IS ON WHICH SIDE. The core owns everything that does not name an API:
   the weight file, the options, the size-class ladder, `tileable`, the job
   table and its queues, batch formation and the slot table, the pass
   SEQUENCER (fill -> depth x conv bands -> out, with the ping-pong advance),
   the cost model, the budget arithmetic and every counter and log line. A
   backend owns device resources and the calls that dispatch -- and it is told
   WHICH dispatch to make rather than working it out, so it cannot get the
   sequence subtly wrong. The one backend is tagpu_vk_restore.c.

   ONE SCHEDULER PER BACKEND, not one shared. Each backend declares its own
   TAGPU_RSCHED, so a backend's slicing never depends on whether another is
   alive.

   THE MODELS AND THE OPTIONS ARE PROCESS-WIDE: the weight files, one
   `tagpu_restoreglsl.on`, whatever backends are running. A job names the model
   it runs (TAGPU_RM_*). */

#include "tagpu_restoreglsl.h"      /* TAGPU_RGLSL_FRAME, the public contract */

#define TAGPU_R_SLOTCOLS  8
#define TAGPU_R_SLOTROWS  8
#define TAGPU_R_BATCH     (TAGPU_R_SLOTCOLS * TAGPU_R_SLOTROWS)  /* frames per batch, at most */
#define TAGPU_R_ACTMAX    512    /* a slot's side cap, texels                  */
/* terrain 0, features 1, effects 2, units 3, the UI 4, the UI's pictures 5,
   and the backend's two self-test probes: every slot is taken, so another
   consumer raises this */
#define TAGPU_R_MAXJOBS   8
#define TAGPU_R_MAXLAYERS 32
/* THE GRID'S TILING, which is the conv kernel's (tagpu_restore_comp.h CONV): a
   workgroup covers 16 columns and 8, 16 or 32 rows, so a grid padded to a
   multiple of 16 wide and 32 high is covered exactly by every variant. */
#define TAGPU_R_GRIDX     16
#define TAGPU_R_GRIDY     32
/* Rows per conv dispatch. A layer is cut into bands so that one dispatch stays
   short on a slow device and the budget can stop between them; a multiple of
   TAGPU_R_GRIDY, so every band is whole workgroups. */
#define TAGPU_R_BAND      64

/* the scheduler, one per backend; its fields are below */
typedef struct TAGPU_RSCHED_s TAGPU_RSCHED;

/* ---- the models, read from <name>.w32.bin (unditherer/weights.py); their
   ids, TAGPU_RM_*, are tagpu_restoreglsl.h's ---- */

/* one layer, in vec4 texels: output tile k's block starts at offset + k x
   kstride, the bias first, then a mat4 per (tap, input tile) */
typedef struct { unsigned offset, jin, kout, kstride; } TAGPU_RLAYER;

typedef struct {
    int          depth, ch, ntex;
    TAGPU_RLAYER layer[TAGPU_R_MAXLAYERS];
    float*       body;                    /* ntex vec4s, the backend repacks   */
    char         name[16];
} TAGPU_RMODEL;

/* ---- the options, from tagpu_restoreglsl.on ---- */
typedef struct { int log; double budget; } TAGPU_ROPT;

/* Re-read the options and the models, from a backend's own init, so that all
   are picked up once per DEVICE. 0 with the reason in tagpu.log when the full
   model's file is unusable -- Classic++ then stays indexed, which is the
   shipped fallback and not a failure. tiny is not required: a job that asks
   for it without it runs full. */
int                tagpu_rcore_reload(const char* who);
/* tagpu.log, one line, for a backend that wants the core's own sink */
void               tagpu_rcore_log(const char* line);
/* 1 when the full model is loaded. Asks for nothing; `reload` is what tries. */
int                tagpu_rcore_ready(void);
/* model TAGPU_RM_*, NULL when it is not loaded */
const TAGPU_RMODEL* tagpu_rcore_model(int which);
const TAGPU_ROPT*   tagpu_rcore_opt(void);

/* ---- a job, as the core sees it ---- */

/* a queued frame with its padded edge S and its size class; `nb` marks a
   neighbourhood frame, whose `edge` and `nbo` are TAGPU_RNBFRAME's */
typedef struct {
    TAGPU_RGLSL_FRAME f;
    short    S, cls, nb, edge;
    unsigned nbo[8];
} TAGPU_RQF;

typedef struct TAGPU_RCORE {
    int    used, prio, oneshot, failed;
    int    model;                            /* TAGPU_RM_*, loaded: `m` is it  */
    const TAGPU_RMODEL* m;
    char   tag[12];
    void*  owner;                            /* the backend's job, opaque here */
    TAGPU_RQF* q; int qn, qcap;              /* queued, not yet in a batch     */
    /* the batch in flight */
    TAGPU_RQF  bf[TAGPU_R_BATCH]; int bn, bS, bcols, inflight;
    int    pass, band, srcAct;
    /* the run: from the first frame queued while idle to the queue draining */
    int    running, rframes, rwrap, rbatches, rdraws, rslices;
    unsigned sliceMark;                      /* s_slice + 1 of the last slice it drew in */
    unsigned rcall0;                         /* calls when the run began       */
    double rt0, rgpuNs, rgpuUnits, runits;
    /* lifetime tallies for the queues' log line */
    int    tframes, tbatches, tdraws; double tgpuNs; unsigned lastTallySlice;
} TAGPU_RCORE;

/* ---- one slot of a batch, as the shaders read it ----
   tagpu_restore_comp.h's `Slot`, field for field: twenty-four 32-bit words,
   so the std430 array has no padding and this struct is its bytes. The valid
   rect (rw, rh) is slot-local from (0, 0); (ax, ay, sw, sh) the frame in the
   source atlas; `key` its colour key or -1; (dx, dy, border, padR, padB) its
   cell in the destination. `nb` is 1 for a neighbourhood frame, whose `edge`
   and `nbo` are TAGPU_RNBFRAME's. r0, r1 are the record's padding to 96 bytes. */
typedef struct {
    int rw, rh, ax, ay, sw, sh, key, dx, dy, border, padR, padB, nb, edge;
    unsigned nbo[8];
    int r0, r1;
} TAGPU_RSLOT;

/* ---- the dispatch the core asks a backend for ---- */
enum { TAGPU_RDRAW_FILL, TAGPU_RDRAW_CONV, TAGPU_RDRAW_OUT };

typedef struct {
    int          kind;
    TAGPU_RCORE* job;
    /* THE GRID: `cols` x `cols` slots, `pitch` = S + 1 apart, padded to gw x gh
       texels (TAGPU_R_GRIDX / _GRIDY). The pitch is one more than the widest
       rect, so every slot ends in a zero column and a zero row -- the half of
       the padding rule that keeps a 3x3 tap out of its neighbour's rect. */
    int          S, cols, pitch, gw, gh;
    /* The slot table, TAGPU_R_BATCH entries at row x TAGPU_R_SLOTCOLS + col.
       Owned by the core and rebuilt on the FILL of each batch, so FILL is where
       a backend uploads it and the CONV and OUT dispatches of the same batch
       see the same contents -- only one batch is ever in flight. */
    const TAGPU_RSLOT* slot;
    int          nframes;                /* frames in the batch: slots 0.. in order */
    int          srcAct;                 /* CONV / OUT: the ping-pong side read    */
    /* CONV: layer `layer` (`L`), rows y0 .. y0 + rows of the grid */
    int          layer, last, y0, rows;
    const TAGPU_RLAYER* L;
} TAGPU_RDRAWREQ;

/* ---- what a backend implements ---- */
typedef struct {
    const char* name;                        /* log prefix: "restorevk"        */
    /* 1 when the backend's pipelines and tables are up; the core counts the
       step either way and dispatches nothing until it is */
    int    (*ready)(void);
    /* The ping-pong activations for a gw x gh grid, grown between batches
       only. THREE ANSWERS, not two:
         1  ready;
         0  this device cannot -- the job FAILS;
        -1  not yet, ask again next slice.
       The third exists for a backend that must RETIRE the allocation it is
       replacing behind a fence rather than freeing it at once: a Vulkan
       backend's old buffers may still be named by a submitted command buffer,
       and the licensing fact is a slot bitmask reaching zero, which takes a
       few frames. Failing the job for that would be permanent damage from a
       transient condition, and growing anyway would be a use-after-free.
       `form_batch` therefore secures the scratch BEFORE it commits the batch,
       so a -1 leaves the queue exactly as it was. */
    int    (*act_ensure)(int gw, int gh);
    /* release the scratch after an idle spell; 1 = something was actually
       released, which is what the core logs on */
    int    (*act_free)(void);
    /* one dispatch. The core has already decided which; the backend binds and
       dispatches it and nothing else. 0 = it could not, and the job fails. */
    int    (*draw)(const TAGPU_RDRAWREQ* r);
    /* the slice's timing bracket, only ever called while `timer` is on */
    void   (*slice_begin)(int q);
    void   (*slice_end)(int q);
    /* 1 = `*ns` is that slice's GPU time; 0 = the result is not in yet */
    int    (*timer_poll)(int q, double* ns);
    /* stop timing for good: the core calls this when a result has not come
       back for 300 slices, which means the device does not really time */
    void   (*timer_off)(void);
    /* 1 when the backend may dispatch at all this frame -- the renderer switch
       the jobs pause under. The queues keep filling while it is 0. */
    int    (*may_draw)(void);
} TAGPU_RBACKEND;

/* ---- the scheduler, one instance per backend ---- */
struct TAGPU_RSCHED_s {
    const TAGPU_RBACKEND* be;
    TAGPU_RCORE  jobs[TAGPU_R_MAXJOBS];
    unsigned slice;                      /* slices issued since the last lost  */
    unsigned calls;                      /* frames stepped, ready or not       */
    int      idle;                       /* consecutive slices with nothing    */
    /* THE SELF-TEST'S GATE: while it is set only jobs of NEGATIVE prio (the
       backend's probes) are picked, and every other job queues. The backend
       sets it before its probes exist and clears it on their verdict. */
    int      gate;
    /* the budget: double-buffered timing, so nothing ever waits on a result */
    int      qHave[2], qFrame, qStall, timer;
    double   qUnits[2], nsPerUnit;
    TAGPU_RCORE* qJob[2];
    /* the table the dispatch requests point at */
    TAGPU_RSLOT slot[TAGPU_R_BATCH];
};

/* A job slot running model `model` (TAGPU_RM_*; one that is not loaded runs
   full, and says so), with the backend's own job hung off `owner`. NULL when
   the table is full or the full model is unusable. The backend creates its
   resources AFTER this returns and calls `tagpu_rcore_job_free` if it cannot. */
TAGPU_RCORE* tagpu_rcore_job_new(TAGPU_RSCHED* s, const char* tag, int prio,
                                 int oneshot, int model, void* owner);
void tagpu_rcore_job_free(TAGPU_RSCHED* s, TAGPU_RCORE* j);
/* Queue frames (copied), classed and edge-padded. The count actually taken. */
int  tagpu_rcore_job_add(TAGPU_RSCHED* s, TAGPU_RCORE* j,
                         const TAGPU_RGLSL_FRAME* frames, int count);
/* Queue neighbourhood frames (copied). The window a slot restores reaches
   a = depth + border texels past the tile on every side -- the depth makes the
   tile exact, the border its ring -- so a frame is refused, with a line in
   tagpu.log, when that is more than one neighbour away (a > the tile's side)
   or when it carries alignment slack. The count actually taken. */
int  tagpu_rcore_job_add_nbhd(TAGPU_RSCHED* s, TAGPU_RCORE* j,
                              const TAGPU_RNBFRAME* frames, int count);
/* tagpu_rcore_nb_frame, which builds a neighbourhood frame from a map, is
   declared beside the frame's type in tagpu_restoreglsl.h: the consumers that
   build them include only that. */
/* Drop everything queued or in flight; the backend clears the destination. */
void tagpu_rcore_job_drop(TAGPU_RCORE* j);
/* Every job fails, as the self-test's failure has them do: queues dropped,
   nothing more dispatched, and each consumer reads `failed` and stays indexed. */
void tagpu_rcore_fail_all(TAGPU_RSCHED* s);
/* THE LAYOUT MOVED UNDER THE JOB -- an atlas repack whose cells the consumer
   carries on the device, source and destination alike. `map` rewrites a frame
   to its new rect and answers 1, or answers 0 for a frame whose entry the
   repack dropped. Every queued frame is mapped, and the unmapped are dropped.
   THE BATCH IN FLIGHT IS TAKEN BACK OUT: its FILL read the old rects, so its
   frames go back to the head of the queue, mapped, and are restored from the
   start -- nothing in flight spans the move. None of them was painted yet, so
   nothing is restored twice. A NEIGHBOURHOOD FRAME IS DROPPED, never mapped:
   `map` moves one rect and its neighbours are eight more. `kept` is the
   queued frames kept, `requeued` the batch's, `dropped` both kinds dropped.
   1 done; 0 when the queue could not grow to take the batch back, and then
   nothing has changed and the caller drops the job. */
int  tagpu_rcore_job_remap(TAGPU_RCORE* j, int (*map)(void* ctx, TAGPU_RGLSL_FRAME* f),
                           void* ctx, int* kept, int* requeued, int* dropped);

/* One slice: issue dispatches for the active job until the budget is spent.
   Counts the call whether or not it dispatches. */
void tagpu_rcore_step(TAGPU_RSCHED* s);

/* Everything the backend owned died with its device: forget the jobs without
   freeing device resources. The backend clears its own handles. */
void tagpu_rcore_lost(TAGPU_RSCHED* s);

#endif
