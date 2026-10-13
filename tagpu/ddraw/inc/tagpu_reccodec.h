#ifndef TAGPU_RECCODEC_H
#define TAGPU_RECCODEC_H
/* tagpu_reccodec.h -- the demo recorder's stream codec: a stream block as a sectioned container
   whose unit frame replaces every RK_UNIT_DELTA, whose pool frame replaces every RK_PROJ_HEAD,
   RK_PROJ and RK_PROJ_DELTA, and whose record frames (tagpu_recframe.h) make the bodies of the
   cells, the orders, the events and the state, and the zstd of a packed block: a frame of its
   own, a run's chunk or a snapshot's chained frame (tagpu_reccodec.c says how; tagpu_recfmt.h
   holds the bytes). Portable C99: no Windows, no engine
   address, no writable static; each RecCodec is one decoder's or encoder's state, owned by one
   thread. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_recfmt.h"

#define RECC_LANES_MAX   128          /* RT_CODEC's rows at most                                 */
#define RECC_COUNTS_MAX  8            /* its COUNT rows at most                                  */
#define RECC_AIR_MAX     1024         /* RT_AIR's rows at most                                   */
#define RECC_TYPES_MAX   2048         /* RT_TYPES' rows at most                                  */
#define RECC_TICKS_MAX   256          /* a sectioned block's iterations at most                  */
#define RECC_KINDS_MAX   65536u       /* an iteration's kinds pattern's bytes at most: past it,
                                         records                                                */
#define RECC_KINDS_MRU   16           /* the patterns a run keeps to refer back to              */
#define RECC_J2_MTF      4096         /* the lane masks a run keeps to refer back to (THE LISTING) */
#define RECC_FRAME_CAP   (16u << 20)  /* a frame's bytes at most: past it, records               */
#define RECC_PAYLOAD_MAX (64u << 20)  /* a sectioned block's payload at most: the reader's block
                                         bound, RECR_BLOCK_MAX                                   */
#define RECC_SHOTS_MAX   4096         /* the RK_EV_SHOT bodies an iteration keeps as references  */
#define RECC_WEAPON_IDS  4096         /* the weapon IDs the pool frame keeps a last birth for:
                                         the raised build's TAGPU_LIM_WEAPONS                    */
#define RECC_GRID_MAX    (1u << 22)   /* the feature grid's cells at most: past it, records. A
                                         shift, never the literal, which the thread-split rule
                                         reads as an engine address                             */
#define RECC_WRECKS_MAX  65536u       /* the wreck pool's records at most: past it, records     */

/* reccodec_stream_block's outcomes: RECC_NOMEM, zstd out of memory for the run, is counted
   among the declined blocks, being no fault of the codec's */
enum { RECC_SECTIONS = 1, RECC_DECLINED, RECC_MISMATCH, RECC_NOMEM };

/* A segment's codec constants, parsed from its header's RT_SLOTS, RT_PROJECTILES, RT_FEATURES,
   RT_CODEC, RT_PHYSICS, RT_AIR and RT_TYPES. `usable` is set when the header has both RT_CODEC and
   RT_PHYSICS and its grid and wreck pool are within the codec's bounds: a segment where it is not
   may hold only records stream blocks. */
typedef struct RecCodecParams {
    unsigned   nslots, nproj, nlanes;
    unsigned   gw, gh, nwrecks;       /* RT_FEATURES: the grid's columns and rows, the wreck pool */
    RecLaneRow lane[RECC_LANES_MAX];
    int32_t    gravity;
    uint8_t    sea, sight;
    uint32_t   bcols;                 /* RT_PHYSICS: the sort buckets' columns, 0 for none        */
    unsigned   nair;
    RecAirRow  air[RECC_AIR_MAX];
    uint8_t    slope[REC_SLOPES];     /* RT_TYPES: the ground speed's slope table ...             */
    unsigned   ntypes;                /* ... and its rows                                         */
    RecTypeRow types[RECC_TYPES_MAX];
    int        usable;
} RecCodecParams;

/* A growable buffer: `oom` is set once an allocation failed, and stays set until its owner
   clears it (reccodec_restart, for a codec's own buffers). */
typedef struct RecBytes { unsigned char* p; size_t n, cap; int oom; } RecBytes;

typedef struct RecCodec RecCodec;

/* The header's codec entries: RT_CODEC, RT_PHYSICS, RT_AIR and RT_TYPES, each its bytes and their
   count, a NULL pointer when the entry is absent. */
enum { RECC_E_CODEC, RECC_E_PHYSICS, RECC_E_AIR, RECC_E_TYPES, RECC_E_N };
typedef struct RecCodecEntries {
    const void* p[RECC_E_N];
    unsigned    n[RECC_E_N];
} RecCodecEntries;
/* The entry of header tag `tag` among them, or -1 when it is not one */
static __inline int reccodec_entry_of(unsigned tag)
{
    return tag == RT_CODEC ? RECC_E_CODEC : tag == RT_PHYSICS ? RECC_E_PHYSICS
         : tag == RT_AIR ? RECC_E_AIR : tag == RT_TYPES ? RECC_E_TYPES : -1;
}

/* The header's entries as codec constants: 1, or 0 with `why` naming what was malformed or past
   which bound. nslots is RT_SLOTS (2..65535), nproj RT_PROJECTILES (1..65535); gw, gh and nwrecks
   are RT_FEATURES' first three, which leave the codec unusable -- not the header unreadable --
   unless 1 <= gw, gh <= 65535, gw * gh <= RECC_GRID_MAX and 1 <= nwrecks <= RECC_WRECKS_MAX;
   ndefs is RT_UNITDEFS' count (0 without it), which bounds RT_TYPES' rows and types. When it
   returns 1 with `usable` 0, `why` says why. */
int  reccodec_params(RecCodecParams* p, unsigned nslots, unsigned nproj, unsigned gw, unsigned gh,
                     unsigned nwrecks, unsigned ndefs, const RecCodecEntries* e, char* why,
                     unsigned cap);
/* A codec sized for the params' slots, pool, grid and wreck pool, which every restart's params
   must match. NULL: no memory, or a size past its bound. */
RecCodec* reccodec_new(const RecCodecParams* p);
void reccodec_free(RecCodec* c);
/* The world the record frames restart from (reccodec_restart): the reader's grid at the end of
   the iteration before a reset block -- gw * gh cells of REC_CELL_BYTES, row-major -- the same
   bytes in the writer's encoder, its check decoder and every reader (tagpu_reccodec.c, RESET
   POINTS). The caller owns it. */
typedef struct RecFrameWorld {
    unsigned gw, gh;
    const unsigned char* grid;
} RecFrameWorld;

/* Every frame restarts: the unit and pool frames from one world -- units[nslots *
   REC_UNIT_DWORDS], live[nslots], and the projectile pool `pool` (nproj * REC_PROJ_IMAGE bytes,
   the records [hw, nproj) read as zero) with its count and extent, any of which may be the
   codec's own copy -- the cells frame from the world's grid, the other record frames empty, and
   its working buffers empty with their failures cleared. Params of other sizes than
   reccodec_new's, or a count past the extent or an extent past the pool, leave it invalid; so
   does a world not of the codec's grid, or NULL. */
void reccodec_restart(RecCodec* c, const RecCodecParams* p, const uint32_t* units,
                      const unsigned char* live, const unsigned char* pool, unsigned count,
                      unsigned hw, const RecFrameWorld* world);
/* The encoder's world: the grid of every image it was handed since its segment began, coded or
   declined. It is meaningful only for a codec that encoded every block of its segment (the
   writer's E). `out->grid` points into the codec and lasts as long as the codec; its bytes are
   this world until the codec's next reccodec_encode (plain_apply's images included) or
   reccodec_restart. */
void reccodec_world(const RecCodec* c, RecFrameWorld* out);
void reccodec_invalidate(RecCodec* c);
/* a + b as IEEE singles, rounded to nearest even, in integers (the unit frame's ledger rule) */
uint32_t reccodec_f32_add(uint32_t a, uint32_t b);
int  reccodec_valid(const RecCodec* c);
const uint32_t* reccodec_units(const RecCodec* c);   /* its copy, nslots * REC_UNIT_DWORDS    */
const unsigned char* reccodec_live(const RecCodec* c);
/* its copy of the pool, nproj * REC_PROJ_IMAGE bytes, zero from *hw on */
const unsigned char* reccodec_pool(const RecCodec* c, unsigned* count, unsigned* hw);
/* A payload of records as a sectioned container in `sec`: RECC_SECTIONS, or RECC_DECLINED when
   the frames do not carry it (sec is then undefined). Either way the copies end as the payload
   leaves the units and the pool; after RECC_DECLINED the frames need a restart. */
int  reccodec_encode(RecCodec* e, const unsigned char* payload, size_t n, RecBytes* sec);
/* A sectioned container back to its records in `payload`: 1, or 0 with `why` (the codec is then
   invalid). */
int  reccodec_decode(RecCodec* d, const unsigned char* sec, size_t n, RecBytes* payload,
                     char* why, unsigned cap);
/* zstd level 3 into `out`: 1 when the frame is smaller than the bytes, else 0 (store them). */
int  reccodec_pack(void* cctx, RecBytes* out, const unsigned char* src, size_t n);
/* One zstd frame of exactly `raw` bytes, no more and no trailing byte, whose window is at most
   2^27: 1, else 0. */
int  reccodec_unpack(void* dctx, unsigned char* dst, size_t raw, const unsigned char* src,
                     size_t packed);
/* The same of a chained snapshot's frame, whose prefix is the `nprefix` bytes before it
   (tagpu_recfmt.h, THE RUN AND THE CHAIN); a prefix past RECC_PAYLOAD_MAX is refused. */
int  reccodec_unpack_chained(void* dctx, unsigned char* dst, size_t raw, const unsigned char* src,
                             size_t packed, const unsigned char* prefix, size_t nprefix);
/* A snapshot's records packed at level 19, against `prefix` (nprefix bytes; NULL for a key), with
   a window that covers both: 1, or 0 when zstd failed or the two pass 2^27 together. */
int  reccodec_pack_chained(void* cctx, RecBytes* out, const unsigned char* src, size_t n,
                           const unsigned char* prefix, size_t nprefix);
/* A sectioned stream block's packed bytes as the next chunk of its run's frame, `dctx` the run's
   own streaming decoder: with `reset` the run begins here and its header is checked and the
   decoder restarted, else the chunk continues what `dctx` decoded last. `dst` holds raw + 1
   bytes. 1 when the chunk gives exactly `raw` bytes from all of its input, the frame not ended;
   else 0, and the run is over: only a reset block decodes on `dctx` after it. */
int  reccodec_run_unpack(void* dctx, unsigned char* dst, size_t raw, const unsigned char* src,
                         size_t packed, int reset);

/* The writer's codec: an encoder E, a check decoder D, the zstd contexts and the scratch they
   share, kept between blocks; one a segment. */
typedef struct RecStream {
    RecCodecParams p;
    RecCodec*  enc;
    RecCodec*  chk;
    int        reset;                 /* the next block restarts the frames and the run          */
    void*      cctx;                  /* ZSTD_CCtx at level 3: a block packed alone              */
    void*      dctx;                  /* ZSTD_DCtx: its check, and a chained snapshot's          */
    void*      zrun;                  /* ZSTD_CCtx at level 19: the run, then a snapshot's pack  */
    void*      drun;                  /* ZSTD_DCtx: the run's check, chunk by chunk              */
    RecBytes   sec, pk, back, out;
    RecBytes   prev;                  /* the segment's last snapshot's records: the next one's
                                         prefix, once `chain` is set                             */
    int        chain;                 /* prev holds a snapshot of this segment                   */
    unsigned   links;                 /* the chained snapshots since the last key                */
    unsigned   sectioned, declined, mismatched;
    unsigned   nomem;                 /* of the declined, those RECC_NOMEM wrote as records      */
    unsigned   snapshots, keys;
    /* A TEST LEVER, 0 from reccodec_stream_begin; the writer sets it from the recorder's
       tagpu_rec.decline (tagpu_recorder.h, tagpu_rec_lever_init). Nonzero, the stream's blocks
       2, 7, 12, ... are declined by the encoder's own decline, so the fallback and the restart
       after it run in a fixture although no real recording declines a block. */
    int        decline;
} RecStream;
int  reccodec_stream_begin(RecStream* s, const RecCodecParams* p);   /* 0: no memory           */
void reccodec_stream_end(RecStream* s);
/* One stream block's body: its flags, and the bytes to write after its RecBlock (which stay valid
   until the next call). Returns the outcome. */
int  reccodec_stream_block(RecStream* s, const unsigned char* payload, size_t n,
                           unsigned* flags, const unsigned char** body, size_t* bn);
/* One snapshot's body, its `n` bytes of records chained to the segment's last snapshot or packed
   as a key: its flags and the bytes to write after its RecBlock, valid until the next call. The
   next stream block restarts the frames and the run. 0: no memory to keep the records as the
   next one's prefix -- the body is still whole, and the next snapshot is a key. */
int  reccodec_snapshot(RecStream* s, const unsigned char* raw, size_t n, unsigned* flags,
                       const unsigned char** body, size_t* bn);

#endif
