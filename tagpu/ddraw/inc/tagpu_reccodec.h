#ifndef TAGPU_RECCODEC_H
#define TAGPU_RECCODEC_H
/* tagpu_reccodec.h -- the demo recorder's stream codec: a stream block as a sectioned container
   whose unit frame replaces every RK_UNIT_DELTA, whose pool frame replaces every RK_PROJ_HEAD,
   RK_PROJ and RK_PROJ_DELTA, and whose record frames (tagpu_recframe.h) make the bodies of the
   cells, the orders, the events and the state, and the zstd frame of a packed block
   (tagpu_reccodec.c says how; tagpu_recfmt.h holds the bytes). Portable C99: no Windows, no engine
   address, no writable static; each RecCodec is one decoder's or encoder's state, owned by one
   thread. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_recfmt.h"

#define RECC_LANES_MAX   128          /* RT_CODEC's rows at most                                 */
#define RECC_COUNTS_MAX  8            /* its COUNT rows at most                                  */
#define RECC_AIR_MAX     1024         /* RT_AIR's rows at most                                   */
#define RECC_TICKS_MAX   256          /* a sectioned block's iterations at most                  */
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

/* reccodec_stream_block's outcomes */
enum { RECC_SECTIONS = 1, RECC_DECLINED, RECC_MISMATCH };

/* A segment's codec constants, parsed from its header's RT_SLOTS, RT_PROJECTILES, RT_FEATURES,
   RT_CODEC, RT_PHYSICS and RT_AIR. `usable` is set when the header has both RT_CODEC and
   RT_PHYSICS and its grid and wreck pool are within the codec's bounds: a segment where it is not
   may hold only records stream blocks. */
typedef struct RecCodecParams {
    unsigned   nslots, nproj, nlanes;
    unsigned   gw, gh, nwrecks;       /* RT_FEATURES: the grid's columns and rows, the wreck pool */
    RecLaneRow lane[RECC_LANES_MAX];
    int32_t    gravity;
    uint8_t    sea, sight;
    unsigned   nair;
    RecAirRow  air[RECC_AIR_MAX];
    int        usable;
} RecCodecParams;

/* A growable buffer: `oom` is set once an allocation failed, and stays set until its owner
   clears it (reccodec_restart, for a codec's own buffers). */
typedef struct RecBytes { unsigned char* p; size_t n, cap; int oom; } RecBytes;

typedef struct RecCodec RecCodec;

/* The header's entries (a NULL pointer: the entry is absent) as codec constants: 1, or 0 with
   `why` naming what was malformed or past which bound. nslots is RT_SLOTS (2..65535), nproj
   RT_PROJECTILES (1..65535); gw, gh and nwrecks are RT_FEATURES' first three, which leave the
   codec unusable -- not the header unreadable -- unless 1 <= gw, gh <= 65535, gw * gh <=
   RECC_GRID_MAX and 1 <= nwrecks <= RECC_WRECKS_MAX. When it returns 1 with `usable` 0, `why`
   says why. */
int  reccodec_params(RecCodecParams* p, unsigned nslots, unsigned nproj, unsigned gw, unsigned gh,
                     unsigned nwrecks, const void* codec, unsigned ncodec, const void* physics,
                     unsigned nphysics, const void* air, unsigned nair, char* why, unsigned cap);
/* A codec sized for the params' slots, pool, grid and wreck pool, which every restart's params
   must match. NULL: no memory, or a size past its bound. */
RecCodec* reccodec_new(const RecCodecParams* p);
void reccodec_free(RecCodec* c);
/* Every frame restarts: the unit and pool frames from one world -- units[nslots *
   REC_UNIT_DWORDS], live[nslots], and the projectile pool `pool` (nproj * REC_PROJ_IMAGE bytes,
   the records [hw, nproj) read as zero) with its count and extent, any of which may be the
   codec's own copy -- and the record frames empty, with its working buffers empty and their
   failures cleared. Params of other sizes than reccodec_new's, or a count past the extent or an
   extent past the pool, leave it invalid. */
void reccodec_restart(RecCodec* c, const RecCodecParams* p, const uint32_t* units,
                      const unsigned char* live, const unsigned char* pool, unsigned count,
                      unsigned hw);
void reccodec_invalidate(RecCodec* c);
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

/* The writer's codec: an encoder E, a check decoder D, the zstd contexts and the scratch they
   share, kept between blocks. */
typedef struct RecStream {
    RecCodecParams p;
    RecCodec*  enc;
    RecCodec*  chk;
    int        reset;                 /* the next block restarts the frames                      */
    void*      cctx;                  /* ZSTD_CCtx                                               */
    void*      dctx;                  /* ZSTD_DCtx                                               */
    RecBytes   sec, pk, back, out;
    unsigned   sectioned, declined, mismatched;
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

#endif
