#ifndef TAGPU_RESTORE_CORE_H
#define TAGPU_RESTORE_CORE_H
/* tagpu_restore_core.h -- the Classic++ restorer's API-INDEPENDENT half, and
   the line a rendering backend implements.

   WHY THIS FILE EXISTS, which is a fact about the tree rather than a taste.
   `tagpu_restoreglsl.c` was one file doing two unrelated jobs: an incremental
   background SCHEDULER (job queues, batch formation, a GPU-time budget driven
   by a smoothed cost estimate) and a GL DRAW SEQUENCE. The Vulkan-only plan's
   landing 11 deletes `opengl_utils.h`, which that file includes -- but two of
   its exports are called from gather halves that SURVIVE the deletion
   (`tagpu_rglsl_tileable` from tagpu_terr.c and tagpu_gaf.c), and the queues
   are what every consumer's lazy-restore contract is written against. So the
   module could not be deleted with GL and could not stay as it was; the split
   is mandatory. It is done while the GL restorer is still here to diff against
   itself, because after landing 4 that oracle is gone.

   WHAT IS ON WHICH SIDE. The core owns everything that does not name an API:
   the weight file, the options, the size-class ladder, `tileable`, the job
   table and its queues, batch formation, the pass SEQUENCER (fill -> depth x
   conv -> out, with the group/ping-pong advance), the cost model, the budget
   arithmetic and every counter and log line. A backend owns device resources
   and the calls that draw -- and it is told WHICH draw to make rather than
   working it out, so a second backend cannot get the sequence subtly wrong in
   a way no A/B would show.

   ONE SCHEDULER PER BACKEND, not one shared. Each backend declares its own
   TAGPU_RSCHED, so a lane's slicing is bit-identical to what it was before this
   split whether or not another lane is alive. A shared budget would have been
   defensible and would also have changed the GL lane's behaviour the moment a
   second lane came up -- and this split is meant to be pure code motion.

   THE MODEL AND THE OPTIONS ARE PROCESS-WIDE: one weight file, one
   `tagpu_restoreglsl.on`, whatever lanes are running. Both lanes must restore
   with the same model or the two pictures are not comparable. */

#include "tagpu_restoreglsl.h"      /* TAGPU_RGLSL_FRAME, the public contract */

#define TAGPU_R_SLOTCOLS  8
#define TAGPU_R_SLOTROWS  8
#define TAGPU_R_BATCH     (TAGPU_R_SLOTCOLS * TAGPU_R_SLOTROWS)  /* frames per batch, at most */
#define TAGPU_R_ACTMAX    512    /* activation side cap, texels                */
#define TAGPU_R_MAXJOBS   6      /* terrain 0, features 1, effects 2, units 3, the UI 4 */
#define TAGPU_R_MAXNK     8
#define TAGPU_R_MAXLAYERS 32

/* the scheduler, one per backend; its fields are below */
typedef struct TAGPU_RSCHED_s TAGPU_RSCHED;

/* ---- the model, read from <model>.w32.bin (unditherer/weights.py) ---- */

/* one layer, in vec4 texels: `kstride/4` is its mat4s per output tile */
typedef struct { unsigned offset, jin, kout, kstride; } TAGPU_RLAYER;

typedef struct {
    int          depth, ch, ntex, kmax;   /* kmax: mat4s in the widest k-block */
    TAGPU_RLAYER layer[TAGPU_R_MAXLAYERS];
    float*       body;                    /* ntex vec4s, the backend uploads   */
    char         name[16];
} TAGPU_RMODEL;

/* ---- the options, from tagpu_restoreglsl.on ---- */
typedef struct { int tiny, fp16, nk, log; double budget; } TAGPU_ROPT;

/* Re-read the options and the model, from a backend's own init, so that both
   are picked up once per CONTEXT as they were before this split. 0 with the
   reason in tagpu.log when the weight file is unusable -- Classic++ then stays
   indexed, which is the shipped fallback and not a failure. */
int                tagpu_rcore_reload(const char* who);
/* tagpu.log, one line, for a backend that wants the core's own sink */
void               tagpu_rcore_log(const char* line);
/* 1 when a model is loaded. Asks for nothing; `reload` is what tries. */
int                tagpu_rcore_ready(void);
const TAGPU_RMODEL* tagpu_rcore_model(void);
const TAGPU_ROPT*   tagpu_rcore_opt(void);

/* Settle NK and WMAX from the device's limits: `nk` is the most k-blocks one
   draw can bind, clamped down to a power of two, and overridden by `nk=N` when
   that is no larger. 0, with the reason logged, when not even one k-block fits
   the uniform block -- the lane then cannot restore at all. */
int tagpu_rcore_pick_nk(TAGPU_RSCHED* s, int maxUniformBlockBytes,
                        int maxAttachments);

/* ---- a job, as the core sees it ---- */

/* a queued frame with its padded edge S and its size class */
typedef struct { TAGPU_RGLSL_FRAME f; short S, cls; } TAGPU_RQF;

typedef struct TAGPU_RCORE {
    int    used, prio, oneshot, failed;
    char   tag[12];
    void*  owner;                            /* the backend's job, opaque here */
    TAGPU_RQF* q; int qn, qcap;              /* queued, not yet in a batch     */
    /* the batch in flight */
    TAGPU_RQF  bf[TAGPU_R_BATCH]; int bn, bS, bcols, inflight;
    int    pass, group, srcAct;
    /* the run: from the first frame queued while idle to the queue draining */
    int    running, rframes, rwrap, rbatches, rdraws, rslices;
    unsigned sliceMark;                      /* s_slice + 1 of the last slice it drew in */
    unsigned rcall0;                         /* calls when the run began       */
    double rt0, rgpuNs, rgpuUnits, runits;
    /* lifetime tallies for the queues' log line */
    int    tframes, tbatches, tdraws; double tgpuNs; unsigned lastTallySlice;
} TAGPU_RCORE;

/* ---- the draw the core asks a backend for ---- */
enum { TAGPU_RDRAW_FILL, TAGPU_RDRAW_CONV, TAGPU_RDRAW_OUT };

typedef struct {
    int          kind;
    TAGPU_RCORE* job;
    int          S, TW, TH;            /* slot pitch, and the target extent    */
    /* The per-slot tables, TAGPU_R_SLOTCOLS x TAGPU_R_SLOTROWS RGBA32F texels,
       laid out as the shaders index them. Owned by the core and rebuilt on the
       FILL of each batch, so FILL is where a backend uploads them and the CONV
       and OUT draws of the same batch see the same contents. They are handed to
       every kind for that reason -- a backend that keeps no upload of its own
       may read them on any draw -- and the GL backend uploads on FILL only. */
    const float* rect;                 /* padded w,h at .zw                    */
    const float* src;                   /* atlas x,y,w,h                        */
    const float* key;                   /* colour key index at .x, -1 = opaque  */
    /* OUT: `nv` vertices of 8 floats -- x,y, dx,dy, slotCol,slotRow, w,h */
    const float* verts; int nv;
    /* CONV: the layer, the output tiles this draw writes, and the ping-pong */
    const TAGPU_RLAYER* L;
    int          group, n, srcAct, relu;
} TAGPU_RDRAWREQ;

/* ---- what a backend implements ---- */
typedef struct {
    const char* name;                        /* log prefix: "restoreglsl"      */
    /* 1 when the backend's programs and tables are up: the core counts a step
       either way, so a pass can tell nothing stepped, but draws nothing */
    int    (*ready)(void);
    /* The ping-pong activation arrays at `side` texels square, grown between
       batches only. THREE ANSWERS, not two:
         1  ready;
         0  this device cannot (no float render target) -- the job FAILS;
        -1  not yet, ask again next slice.
       The third exists for a backend that must RETIRE the allocation it is
       replacing behind a fence rather than freeing it at once: a Vulkan
       backend's old arrays may still be named by a submitted command buffer,
       and the licensing fact is a slot bitmask reaching zero, which takes a
       few frames. Failing the job for that would be permanent damage from a
       transient condition, and growing anyway would be a use-after-free.
       `form_batch` therefore secures the scratch BEFORE it commits the batch,
       so a -1 leaves the queue exactly as it was. */
    int    (*act_ensure)(int side);
    /* release the scratch after an idle spell; 1 = something was actually
       released, which is what the core logs on */
    int    (*act_free)(void);
    /* one draw. The core has already decided which; the backend binds and
       draws it and nothing else. 0 = it could not, and the job fails. */
    int    (*draw)(const TAGPU_RDRAWREQ* r);
    /* the slice's timing bracket, only ever called while `timer` is on */
    void   (*slice_begin)(int q);
    void   (*slice_end)(int q);
    /* 1 = `*ns` is that slice's GPU time; 0 = the result is not in yet */
    int    (*timer_poll)(int q, double* ns);
    /* stop timing for good: the core calls this when a result has not come
       back for 300 slices, which means the device does not really time */
    void   (*timer_off)(void);
    /* the state a slice disturbs and the caller expects back (GL's enables and
       write masks; a Vulkan backend has nothing to do here). `slice` is the
       slice about to run, for the backends that report first-slice diagnostics. */
    void   (*state_push)(unsigned slice);
    void   (*state_pop)(unsigned slice);
    /* 1 when the lane may draw at all this frame -- the renderer switch the
       jobs pause under. The queues keep filling while it is 0. */
    int    (*may_draw)(void);
} TAGPU_RBACKEND;

/* ---- the scheduler, one instance per backend ---- */
struct TAGPU_RSCHED_s {
    const TAGPU_RBACKEND* be;
    TAGPU_RCORE  jobs[TAGPU_R_MAXJOBS];
    int      nk, wmax;
    unsigned slice;                      /* slices issued in this context      */
    unsigned calls;                      /* frames stepped, ready or not       */
    int      idle;                       /* consecutive slices with nothing    */
    /* the budget: double-buffered timing, so nothing ever waits on a result */
    int      qHave[2], qFrame, qStall, timer;
    double   qUnits[2], nsPerUnit;
    TAGPU_RCORE* qJob[2];
    /* the tables the draw requests point at */
    float    rect[TAGPU_R_BATCH * 4], src[TAGPU_R_BATCH * 4], key[TAGPU_R_BATCH * 4];
    float    verts[TAGPU_R_BATCH * 6 * 8];
};

/* A job slot, with the backend's own job hung off `owner`. NULL when the table
   is full or the model is unusable. The backend creates its resources AFTER
   this returns and calls `tagpu_rcore_job_free` if it cannot. */
TAGPU_RCORE* tagpu_rcore_job_new(TAGPU_RSCHED* s, const char* tag, int prio,
                                 int oneshot, void* owner);
void tagpu_rcore_job_free(TAGPU_RSCHED* s, TAGPU_RCORE* j);
/* Queue frames (copied), classed and edge-padded. The count actually taken. */
int  tagpu_rcore_job_add(TAGPU_RSCHED* s, TAGPU_RCORE* j,
                         const TAGPU_RGLSL_FRAME* frames, int count);
/* Drop everything queued or in flight; the backend clears the destination. */
void tagpu_rcore_job_drop(TAGPU_RCORE* j);

/* One slice: issue draws for the active job until the budget is spent. The
   backend's `state_push`/`state_pop` bracket it. Counts the call whether or
   not it draws -- `tagpu_rglsl_calls` is how a pass learns nothing stepped. */
void tagpu_rcore_step(TAGPU_RSCHED* s);

/* Everything the backend owned died with its context: forget the jobs without
   freeing device resources. The backend clears its own ids. */
void tagpu_rcore_lost(TAGPU_RSCHED* s);

#endif
