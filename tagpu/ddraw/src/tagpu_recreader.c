/* tagpu_recreader.c -- reads a demo recording back (tagpu_recreader.h): the blocks, the header's
   entries, and the world the stream builds, one iteration at a time.

   A block is checked before anything reads it: its header's magic and CRC, its flags, its sizes
   against RECR_BLOCK_MAX, its payload's CRC, and its unpacked size against the header's: a packed
   body is exactly one zstd frame of `raw` bytes, unpacked into a buffer of that size. A block that
   fails ends the reading there, as it ends tarec.py's.

   THE UNIT FRAME (tagpu_reccodec.c). A sectioned stream block decodes to its records through the
   reader's own codec state, which a flagged block (RBF_FRAME_RESET) restarts from the world's
   units and an unflagged one continues. The ORDERING that makes the world the state the writer's
   encoder restarted from: tick_read loads a block only once the previous block's iterations are
   all applied, so at a block the world's units are the copy at the end of the iteration before
   it; a seek, a crossing and a timeline's choice reset the world (world_reset), which invalidates
   the frame, and start at a snapshot or a segment's first stream block, both of which the writer
   follows with a flagged block. A sectioned block met with no valid frame is refused by name,
   never misread.

   SEQUENTIAL READING skips snapshot blocks unread -- not unpacked, their CRC not checked -- since
   a playing from the start never needs one. A SEEK reads them: the index (every block's header)
   finds the last snapshot before the target, whose payload is then checked like any block's, and
   the stream is read on from the iteration after it. A snapshot carries only the cells and the
   wreck records that differ from its segment's first iteration (tagpu_recfmt.h, RB_SNAPSHOT), so
   the reader keeps that grid and that pool, the base and the wreck base, from the segment's first
   iteration it reads (tick_take).

   THE SEGMENTS (tagpu_recfmt.h). The index is every block of every segment, each with its
   segment, found by segs_scan at the open and again by every call that needs it, since a fork
   may have been appended since. A playing reads one CHAIN of timelines, root first, chosen by
   recr_timeline before its first iteration; every segment of it agrees with the first header in
   shape, which is the bound that keeps the world's allocations, sized from the first header,
   valid for every segment's records.
   THE INVARIANT that makes a chain one world: a segment's records are applied only over a world
   reset to that segment's first iteration, or built from that segment's own snapshot over that
   segment's own base. No record of one segment is applied over another segment's world except
   across fork_at, where the child's first iteration replaces the world whole -- RK_GRID whole,
   every image, as any recording's first iteration -- after a reset. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_recreader.h"
#include "tagpu_recmp_history.h"
#include "tagpu_reccodec.h"
#include "zstd.h"
#include "crc32.h"

_Static_assert(RECC_PAYLOAD_MAX == RECR_BLOCK_MAX,
               "a sectioned block decodes to at most what a records block may hold");

/* one block of the file, by its header alone */
typedef struct RecIndexEntry {
    unsigned           kind, flags, first, last, raw, packed;
    unsigned           seg;         /* its segment                                            */
    unsigned long long off;         /* its header's offset in the file                        */
} RecIndexEntry;

/* one segment of the file: a timeline (tagpu_recreader.h) */
typedef struct RecSeg {
    RecTimeline        t;
    RecHeader*         h;           /* its header: the reader's own for the first, else the heap */
    unsigned           i0, n;       /* its blocks in the index, its header not among them      */
    unsigned           nstream;     /* its stream blocks                                       */
    int                ended;       /* an RB_END closes it                                     */
} RecSeg;

typedef struct {
    unsigned epoch, segment, tick, index, done;
    RecMpHistory* history;
    unsigned char* payload;
    size_t bytes, at;
} RecMpRead;

struct RecReader {
    HANDLE             f;
    unsigned long long off;         /* the file offset of the next block                      */
    RecHeader          h;
    RecWorld           w;
    unsigned char*     pk;          /* the block's packed bytes                               */
    unsigned           pkcap;
    unsigned char*     raw;         /* its payload: pk for a stored block, dec.p for a sectioned
                                       one, else a buffer of its own                          */
    size_t             rawn;
    RecCodec*          codec;       /* the stream codec's frames, from the first sectioned block */
    void*              dctx;        /* the ZSTD_DCtx, from the first packed block on           */
    void*              rdctx;       /* the run's streaming ZSTD_DCtx, from the first sectioned
                                       block on: valid exactly while the codec is (block_body) */
    unsigned char*     sec;         /* a packed sectioned block's container ...                */
    size_t             seccap;
    RecBytes           dec;         /* ... and the records it decodes to                       */
    size_t             at;          /* the next record in the payload                         */
    int                ended;       /* the end block, or the file's end, has been read         */
    char               end[160];    /* how the recording ended, once `ended`                   */
    unsigned           end_reason;  /* the end block's RT_END_REASON, RT_LAST and              */
    unsigned           end_last;    /* RT_ITERATIONS, once it has been read; 0 for one it      */
    unsigned           end_iters;   /* does not carry                                         */
    unsigned*          touched;     /* the slots whose `changed` is set                        */
    unsigned           ntouched;
    unsigned           evcap, fcap, clickcap;
    int                snap;        /* the records applied are a snapshot's (snapshot_apply)   */
    unsigned           fbracket;    /* an RK_EV_FEATURE's root + 1, until its END             */
    unsigned           ndeaths;     /* the deaths whose RK_EV_DEATH_END is still to come      */
    uint32_t           session_id;  /* the unit ID of the iteration's last RK_SPRAY_SESSION   */
    unsigned           death[REC_DEATHS_MAX];
    RecYardStep        yards;       /* the step's yards by THE YARD RULE (tagpu_recfmt.h)     */
    struct { unsigned slot, list, total, next; } pend;   /* a list sent in several records   */
    unsigned long long first_off;   /* the file offset of the first block after the header    */
    unsigned char*     base;        /* the grid at the end of a segment's first iteration ...  */
    unsigned char*     wbase;       /* ... and the wreck pool, h.wrecks records ...           */
    int                base_seg;    /* ... the chain's segment base_seg, -1 for none           */
    int                globals_in;  /* an RK_GLOBALS has come, so a delta has an image to change */
    int                proj_in;     /* an RK_PROJ_HEAD has come, so records below hw may come  */
    unsigned           mnext;       /* the next RK_MAPPED's offset                            */
    RecIndexEntry*     idx;         /* every block of every segment but their headers, to the
                                       first torn one, or to an end block no header follows   */
    unsigned           nidx;
    unsigned           scan_epoch;
    RecMpRead          mp_query, mp_apply;
    RecSeg             seg[RECR_TIMELINES];
    unsigned           nseg;
    unsigned           nstates, max_id;
    RecTimelineState   states[REC_TIMELINE_ROWS];
    unsigned long long file_end;
    unsigned           chain_id[RECR_TIMELINES];   /* the chain played, root first, by id ...  */
    unsigned           chain[RECR_TIMELINES];      /* ... and by segment, after every scan     */
    unsigned           nchain;
    unsigned           cur;         /* the chain's segment being read                          */
    int                in_seg;      /* the world holds an iteration of segment chain[cur]      */
    int                read_any;    /* an iteration has been read: the chain is fixed          */
    RecSeekWork        work;        /* the last recr_seek's (recr_seek_work)                   */
    RecSeekWork        decoded;     /* every block_body since the reader opened: its blocks,
                                       packed and raw, which recr_seek's work takes the
                                       difference of                                            */
    const unsigned char* prefix;    /* while snapshot_chain reads a chained snapshot: the one
                                       before it, unpacked, its frame's prefix; else NULL       */
    size_t             nprefix;
};

static const char* const END_REASONS[] = {
    "", "complete: the level ended", "INCOMPLETE: the ring was full",
    "INCOMPLETE: the file could not be written",
    "INCOMPLETE: no memory for a declared table's growth",
    "INCOMPLETE: the level changed without a teardown",
    "INCOMPLETE: the game stopped being a skirmish",
    "INCOMPLETE: an order list could not be recorded",
    "INCOMPLETE: the features were not the ones it began with",
    "INCOMPLETE: the projectile pool was not the one it began with, or its count was past its "
    "records",
    "INCOMPLETE: the explored map was not the one it began with",
    "INCOMPLETE: a later timeline ended it at its last complete block",
    "pruned: a deleted ancestor's retained prefix"
};

/* ---- blocks --------------------------------------------------------------------------------- */

static void payload_free(RecReader* r)
{
    if (r->raw && r->raw != r->pk && r->raw != r->dec.p) free(r->raw);
    r->raw = NULL;
    r->rawn = 0;
    r->at = 0;
}

/* The payload in raw, the caller's from here: a buffer of its own, copied when raw is one the
   reader reuses (pk, dec). raw is left empty. NULL with *n 0 for no payload; NULL with *n set
   for no memory. */
static unsigned char* payload_own(RecReader* r, size_t* n)
{
    unsigned char* p = r->raw;
    *n = r->rawn;
    if (p && (p == r->pk || p == r->dec.p)) {
        p = (unsigned char*)malloc(*n ? *n : 1);
        if (p) memcpy(p, r->raw, *n);
    }
    r->raw = NULL;
    r->rawn = 0;
    r->at = 0;
    return p;
}

static int read_all(HANDLE f, void* p, DWORD n)
{
    DWORD got = 0;
    return ReadFile(f, p, n, &got, NULL) && got == n;
}

static int seek_to(HANDLE f, unsigned long long off)
{
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(f, at, NULL, FILE_BEGIN) != 0;
}

/* the file's next read at `off`, the offset of a block's header */
static int file_at(RecReader* r, unsigned long long off)
{
    if (!seek_to(r->f, off)) return 0;
    r->off = off;
    return 1;
}

/* What this format refuses in a block's flags and sizes (tagpu_recfmt.h, RecBlock), or NULL. */
static const char* block_flags_bad(const RecBlock* b)
{
    if (b->flags & ~RBF_ALL) return "flags it does not know";
    if ((b->flags & RBF_SECTIONS) && b->kind != RB_STREAM)
        return "sections, not being a stream block";
    if ((b->flags & RBF_REFRESH) && b->kind != RB_STREAM)
        return "a refresh, not being a stream block";
    if ((b->flags & RBF_FRAME_RESET) && !(b->flags & RBF_SECTIONS))
        return "a frame reset, not being sectioned";
    if ((b->flags & RBF_SECTIONS) && (b->flags & RBF_STORED))
        return "sections stored, not a chunk of their run";
    if ((b->flags & RBF_CHAINED) && (b->kind != RB_SNAPSHOT || (b->flags & RBF_STORED)))
        return "a chain, not being a packed snapshot";
    if (!(b->flags & RBF_STORED) &&
        (b->kind == RB_HEADER || b->kind == RB_END || b->kind == RB_TIMELINES))
        return "a packed header, end or timelines body, which are always stored";
    if (!(b->flags & RBF_STORED) && (!b->packed || !b->raw)) return "a packed body of no bytes";
    return NULL;
}

/* the zstd decompression context, made at the first packed block a reading meets: NULL, no
   memory */
static void* dctx_of(RecReader* r)
{
    if (!r->dctx) r->dctx = ZSTD_createDCtx();
    return r->dctx;
}

/* the run's, made at the first sectioned block: NULL, no memory */
static void* rdctx_of(RecReader* r)
{
    if (!r->rdctx) r->rdctx = ZSTD_createDCtx();
    return r->rdctx;
}

/* The header of the block at r->off, checked. 1 read; 0 the file's end; -1 a header that cannot
   be read (why). */
static int block_head(RecReader* r, RecBlock* b, char* why, unsigned cap)
{
    DWORD got = 0;
    payload_free(r);
    if (!ReadFile(r->f, b, sizeof *b, &got, NULL)) {
        snprintf(why, cap, "cannot read at byte %llu (%lu)", r->off, GetLastError());
        return -1;
    }
    if (!got) return 0;
    if (got < sizeof *b) {
        snprintf(why, cap, "a block header cut short at byte %llu", r->off);
        return -1;
    }
    if (b->magic != REC_BLOCK_MAGIC || (uint32_t)Crc32_ComputeBuf(0, b, 28) != b->crc_head) {
        snprintf(why, cap, "a torn block header at byte %llu", r->off);
        return -1;
    }
    if (b->packed > RECR_BLOCK_MAX || b->raw > RECR_BLOCK_MAX ||
        ((b->flags & RBF_STORED) && b->packed != b->raw)) {
        snprintf(why, cap, "a block at byte %llu of %u bytes, %u unpacked: past what it may hold",
                 r->off, b->packed, b->raw);
        return -1;
    }
    if (block_flags_bad(b)) {
        snprintf(why, cap, "a block of kind %u at byte %llu with %s", b->kind, r->off,
                 block_flags_bad(b));
        return -1;
    }
    return 1;
}

static const RecHeader* seg_header(const RecReader* r);

/* Whether a sectioned stream block may be read here, before any of it is: its segment has the
   codec, and the block either restarts the frames or continues the ones this reading decoded the
   block before. 1, or -1 (why). */
static int sections_ready(RecReader* r, const RecBlock* b, char* why, unsigned cap)
{
    const RecHeader* h = seg_header(r);
    if (!h->has_codec) {
        snprintf(why, cap, "a sectioned stream block at byte %llu in a segment whose header has no "
                 "RT_CODEC and RT_PHYSICS, or a grid or wreck pool past the codec's bounds", r->off);
        return -1;
    }
    if (!r->codec) r->codec = reccodec_new(&h->codec);
    if (!r->codec) {
        snprintf(why, cap, "no memory for the codec of %u slots, %u projectiles and a %u x %u "
                 "grid, at byte %llu", h->codec.nslots, h->codec.nproj, h->codec.gw, h->codec.gh,
                 r->off);
        return -1;
    }
    if (!(b->flags & RBF_FRAME_RESET) && !reccodec_valid(r->codec)) {
        snprintf(why, cap, "a stream block that continues a frame this reading did not start, "
                 "at byte %llu", r->off);
        return -1;
    }
    return 1;
}

/* A sectioned stream block's container, `sec` its `n` bytes, decoded into raw by the stream
   codec's frames (tagpu_reccodec.c, THE UNIT FRAME, THE POOL FRAME and THE RECORD FRAMES), once
   sections_ready has passed it. 1, or -1 (why). */
static int sections_body(RecReader* r, const RecBlock* b, const unsigned char* sec, size_t n,
                         char* why, unsigned cap)
{
    const RecHeader* h = seg_header(r);
    char w2[160];
    /* ORDERING: the world's units and its pool are the copies at the end of the iteration before
       this block (tick_read loads a block only between iterations), which is what the writer's
       encoder restarted from, and its grid, which every image of the segment read so far built
       (apply), or a snapshot over the base did. The codec was made for the sizes of the first
       segment it met -- slots, projectile pool, grid and wreck pool -- which every segment's
       header equals (segs_scan, shape_differs); h->codec is this segment's own. */
    if (b->flags & RBF_FRAME_RESET) {
        RecFrameWorld fw = { r->h.grid_w, r->h.grid_h, r->w.grid };
        reccodec_restart(r->codec, &h->codec, r->w.unit, r->w.live, r->w.proj, r->w.nproj,
                         r->w.proj_hw, &fw);
    }
    if (!reccodec_decode(r->codec, sec, n, &r->dec, w2, sizeof w2)) {
        snprintf(why, cap, "the sectioned block at byte %llu: %s", r->off, w2);
        return -1;
    }
    r->raw = r->dec.p;
    r->rawn = r->dec.n;
    return 1;
}

/* The payload of the block whose header block_head has just read: checked, unpacked and, when
   sectioned, decoded into raw. 1, or -1 (why). */
static int block_take(RecReader* r, const RecBlock* b, char* why, unsigned cap)
{
    if (b->packed > r->pkcap) {
        unsigned char* n = (unsigned char*)realloc(r->pk, b->packed);
        if (!n) {
            snprintf(why, cap, "no memory for a block of %u bytes", b->packed);
            return -1;
        }
        r->pk = n;
        r->pkcap = b->packed;
    }
    if (b->packed && !read_all(r->f, r->pk, b->packed)) {
        snprintf(why, cap, "a block cut short at byte %llu", r->off);
        return -1;
    }
    if ((uint32_t)Crc32_ComputeBuf(0, r->pk, b->packed) != b->crc_body) {
        snprintf(why, cap, "a block whose payload fails its CRC at byte %llu", r->off);
        return -1;
    }
    /* a chained snapshot unpacks only against the one before it, which snapshot_chain holds, and
       no other block against anything (block_flags_bad: a chain is never stored) */
    if (!(b->flags & RBF_CHAINED) != !r->prefix) {
        snprintf(why, cap, r->prefix ? "a block at byte %llu read as a chain's link, not chained"
                 : "a chained snapshot at byte %llu read without the snapshots before it", r->off);
        return -1;
    }
    if (b->flags & RBF_SECTIONS) {
        /* ONE VALIDITY for the run and the frames (tagpu_recfmt.h, THE RUN AND THE CHAIN): the
           run's decoder has unpacked exactly the chunks of the blocks the codec decoded since its
           restart, because both restart at the same RBF_FRAME_RESET block, both advance only on a
           block both decoded, and every failure of either invalidates the codec -- as a seek, a
           crossing, a records block, a snapshot and every block block_body refuses do. So
           sections_ready's test of the codec is the run's too, and a chunk is unpacked only where
           its run's chunks before it were. */
        if (sections_ready(r, b, why, cap) < 0) return -1;
        if ((size_t)b->raw + 1 > r->seccap) {
            unsigned char* n = (unsigned char*)realloc(r->sec, (size_t)b->raw + 1);
            if (!n) {
                snprintf(why, cap, "no memory for a block of %u bytes", b->raw);
                return -1;
            }
            r->sec = n;
            r->seccap = (size_t)b->raw + 1;
        }
        if (!rdctx_of(r)) {
            snprintf(why, cap, "no memory for zstd, at byte %llu", r->off);
            return -1;
        }
        if (!reccodec_run_unpack(r->rdctx, r->sec, b->raw, r->pk, b->packed,
                                 (b->flags & RBF_FRAME_RESET) != 0)) {
            snprintf(why, cap, "a block at byte %llu that is not its run's next chunk of %u bytes",
                     r->off, b->raw);
            return -1;
        }
        if (sections_body(r, b, r->sec, b->raw, why, cap) < 0) return -1;
    } else if (b->flags & RBF_STORED) {
        r->raw = r->pk;
        r->rawn = b->packed;
    } else {
        unsigned char* out = (unsigned char*)malloc(b->raw);
        if (!out) {
            snprintf(why, cap, "no memory for a block of %u bytes", b->raw);
            return -1;
        }
        if (!dctx_of(r) ||
            !(r->prefix ? reccodec_unpack_chained(r->dctx, out, b->raw, r->pk, b->packed,
                                                  r->prefix, r->nprefix)
                        : reccodec_unpack(r->dctx, out, b->raw, r->pk, b->packed))) {
            free(out);
            if (!r->dctx) snprintf(why, cap, "no memory for zstd, at byte %llu", r->off);
            else snprintf(why, cap, "a block at byte %llu that is not one zstd frame of its %u "
                          "bytes", r->off, b->raw);
            return -1;
        }
        r->raw = out;
        r->rawn = b->raw;
    }
    /* a stream block of records leaves the frame behind: the writer restarts it at the next */
    if (b->kind == RB_STREAM && !(b->flags & RBF_SECTIONS) && r->codec)
        reccodec_invalidate(r->codec);
    r->off += sizeof *b + b->packed;
    r->decoded.blocks++;
    r->decoded.packed += b->packed;
    r->decoded.raw += b->raw;
    return 1;
}

/* block_take, and on a refusal the codec left invalid: the block was not decoded, whatever the
   reason -- its bytes, its run, memory -- so neither the frames nor the run's decoder, which has
   not unpacked its chunk, may be continued after it (ONE VALIDITY above). */
static int block_body(RecReader* r, const RecBlock* b, char* why, unsigned cap)
{
    if (block_take(r, b, why, cap) > 0) return 1;
    if (r->codec) reccodec_invalidate(r->codec);
    return -1;
}

/* 1 a block, its payload in raw; 0 the file's end; -1 a block that cannot be read (why) */
static int block_read(RecReader* r, RecBlock* b, char* why, unsigned cap)
{
    int k = block_head(r, b, why, cap);
    return k > 0 ? block_body(r, b, why, cap) : k;
}

/* Past the payload of the block whose header block_head has just read, unread. 1, or -1
   (why). */
static int block_skip(RecReader* r, const RecBlock* b, char* why, unsigned cap)
{
    LARGE_INTEGER size;
    if (!GetFileSizeEx(r->f, &size)) {
        snprintf(why, cap, "cannot size the file (%lu)", GetLastError());
        return -1;
    }
    if (r->off + sizeof *b + b->packed > (unsigned long long)size.QuadPart) {
        snprintf(why, cap, "a block cut short at byte %llu", r->off);
        return -1;
    }
    if (!file_at(r, r->off + sizeof *b + b->packed)) {
        snprintf(why, cap, "cannot move past the block at byte %llu (%lu)", r->off,
                 GetLastError());
        return -1;
    }
    return 1;
}

/* ---- the header ----------------------------------------------------------------------------- */

static void text(char* dst, unsigned cap, const unsigned char* p, unsigned n)
{
    unsigned k = 0;
    while (k < n && k + 1 < cap && p[k]) {
        dst[k] = (char)p[k];
        k++;
    }
    dst[k] = 0;
}

static unsigned u32(const unsigned char* p)
{
    unsigned v;
    memcpy(&v, p, 4);
    return v;
}

/* 1 parsed; 0 malformed or of another format (why) */
static int header_parse(RecHeader* h, const unsigned char* b, size_t len, char* why, unsigned cap)
{
    size_t at = 0;
    unsigned seen = 0, i;
    RecCodecEntries ent;
    memset(h, 0, sizeof *h);
    memset(&ent, 0, sizeof ent);
    while (at + 4 <= len) {
        unsigned short tag, n;
        const unsigned char* p = b + at + 4;
        memcpy(&tag, b + at, 2);
        memcpy(&n, b + at + 2, 2);
        if (at + 4 + n > len) {
            snprintf(why, cap, "a header entry past its block");
            return 0;
        }
        switch (tag) {
        case RT_FORMAT:        if (n == 4) { h->format = u32(p); seen |= 1; } break;
        case RT_BUILD:         text(h->build, sizeof h->build, p, n); break;
        case RT_MODE:          if (n == 4) { h->mode = u32(p); seen |= 2; } break;
        case RT_MULTIPLAYER: {
            RecMpFileState state;
            if (h->has_mp || n != sizeof h->mp) {
                snprintf(why, cap, "malformed multiplayer roster"); return 0;
            }
            memcpy(&h->mp, p, sizeof h->mp);
            if (!tagpu_recmp_file_state(&state, &h->mp)) {
                snprintf(why, cap, "invalid multiplayer roster"); return 0;
            }
            h->has_mp = 1;
            break;
        }
        case RT_MP_SEATS:
            if (h->has_mp_seats || n != sizeof h->mp_seats) {
                snprintf(why, cap, "malformed multiplayer seat mapping"); return 0;
            }
            memcpy(&h->mp_seats, p, sizeof h->mp_seats);
            h->has_mp_seats = 1;
            break;
        case RT_MAP:           text(h->map, sizeof h->map, p, n); break;
        case RT_WALLCLOCK:     if (n == 8) memcpy(&h->wallclock, p, 8); break;
        case RT_START:         if (n == 4) h->start = u32(p); break;
        case RT_FRAME:         if (n == 4) { h->has_frame = 1; h->frame = u32(p); } break;
        case RT_SLOTS:         if (n == 4) { h->slots = u32(p); seen |= 4; } break;
        case RT_UNITS_EACH:    if (n == 4) h->units_each = u32(p); break;
        case RT_UNITDEFS:
            if (n == 8) {
                h->has_unitdefs = 1;
                h->ndefs = u32(p);
                h->defs_hash = u32(p + 4);
            }
            break;
        case RT_WEAPONS:
            if (n == 8) {
                h->has_weapons = 1;
                h->nweapons = u32(p);
                h->weapons_hash = u32(p + 4);
            }
            break;
        case RT_FEATURES:
            if (n == 20) {
                h->grid_w = u32(p);
                h->grid_h = u32(p + 4);
                h->wrecks = u32(p + 8);
                h->nfdefs = u32(p + 12);
                h->fdefs_hash = u32(p + 16);
                seen |= 8;
            }
            break;
        case RT_LAYOUT:        text(h->layout, sizeof h->layout, p, n); break;
        case RT_TIMELINE:
            if (n == 12) {
                h->has_timeline = 1;
                h->tl_id = u32(p);
                h->tl_parent = u32(p + 4);
                h->tl_fork_at = u32(p + 8);
            }
            break;
        case RT_GLOBALS:       text(h->globals, sizeof h->globals, p, n); break;
        case RT_CODEC:
        case RT_PHYSICS:
        case RT_AIR:
        case RT_TYPES:
            if (ent.p[reccodec_entry_of(tag)]) {
                snprintf(why, cap, "header entry %u twice", tag);
                return 0;
            }
            ent.p[reccodec_entry_of(tag)] = p;
            ent.n[reccodec_entry_of(tag)] = n;
            break;
        case RT_PROJECTILES:   if (n == 4) { h->proj_cap = u32(p); seen |= 16; } break;
        case RT_HOLD:
            if (h->has_hold) {
                snprintf(why, cap, "header entry %u twice", tag);
                return 0;
            }
            if (!reclossy_hold_parse(&h->hold, p, n, why, cap)) return 0;
            h->has_hold = 1;
            break;
        case RT_SETTINGS_MAP:
            text(h->settings_map, sizeof h->settings_map, p, n);
            h->has_settings_map = 1;
            break;
        case RT_OPTIONS:
            if (n == 20) {
                unsigned short np;
                h->has_options = 1;
                h->lostype = u32(p);
                h->cmdrdeath = u32(p + 4);
                h->difficulty = u32(p + 8);
                h->fixed_starts = u32(p + 12);
                memcpy(&np, p + 16, 2);
                h->nplayers = np;
                h->controlled = p[18];
                h->viewed = p[19];
            }
            break;
        case RT_PLAYER: {
            RecSeat* s;
            if (n < 16 + REC_INFO_BYTES || p[0] >= REC_PLAYERS || h->nseats >= REC_PLAYERS) {
                snprintf(why, cap, "a malformed seat in the header");
                return 0;
            }
            for (i = 0; i < h->nseats; i++)
                if (h->seats[i].index == p[0]) {
                    snprintf(why, cap, "seat %u twice in the header", p[0]);
                    return 0;
                }
            s = &h->seats[h->nseats++];
            s->index = p[0];
            s->controller = p[1];
            s->side = p[2];
            s->logo = p[3];
            memcpy(s->ally, p + 4, 11);
            memcpy(s->info, p + 16, REC_INFO_BYTES);
            s->has_info = 0;
            for (i = 0; i < REC_INFO_BYTES; i++)
                if (s->info[i] != 0xFF) s->has_info = 1;
            text(s->name, sizeof s->name, p + 16 + REC_INFO_BYTES, n - 16 - REC_INFO_BYTES);
            break;
        }
        case RT_TABLE: {
            RecTableInfo* t;
            if (n < 16 || h->ntab >= REC_TABLE_MAX) {
                snprintf(why, cap, "a malformed table in the header");
                return 0;
            }
            t = &h->tab[h->ntab];
            memcpy(&t->index, p, 2);
            t->cls = p[2];
            t->key = p[3];
            t->stride = u32(p + 4);
            memcpy(&t->flags, p + 8, 2);
            t->lhash = u32(p + 12);
            text(t->name, sizeof t->name, p + 16, n - 16);
            for (i = 0; i < h->ntab; i++)
                if (h->tab[i].index == t->index) {
                    snprintf(why, cap, "table %u twice in the header", t->index);
                    return 0;
                }
            if (!t->stride || t->stride > RECR_BLOCK_MAX) {
                snprintf(why, cap, "table %u with rows of %u bytes", t->index, t->stride);
                return 0;
            }
            h->ntab++;
            break;
        }
        default:
            break;      /* RT_WALLCLOCK, RT_LEVER: nothing the replay needs */
        }
        at += (4 + (size_t)n + 3) & ~(size_t)3;
    }
    if ((seen & 1) && !rec_format_supported(h->format)) {
        snprintf(why, cap, "format %u, this build reads %u", h->format, REC_FORMAT);
        return 0;
    }
    if (seen != 31) {
        snprintf(why, cap, "a header without its format, mode, slots, features or projectile "
                 "pool");
        return 0;
    }
    if ((h->mode == 3) != !!h->has_mp || h->has_mp != h->has_mp_seats) {
        snprintf(why, cap, "network mode and contribution roster disagree"); return 0;
    }
    if (h->has_mp && h->has_hold) {
        snprintf(why, cap, "a multiplayer header with RT_HOLD: its stream holds nothing");
        return 0;
    }
    if (h->has_mp) {
        if (!tagpu_recmp_file_roster_valid(&h->mp,&h->mp_seats) ||
            h->slots!=h->mp_seats.slots || h->units_each!=h->mp_seats.each ||
            h->nseats!=h->mp_seats.count) {
            snprintf(why, cap, "contribution mapping disagrees with world allocation or seats"); return 0;
        }
        for (i=0;i<h->mp_seats.count;i++) {
            unsigned k;
            for (k=0;k<h->nseats;k++) if (h->seats[k].index==h->mp_seats.actor[i].seat) break;
            if (k==h->nseats || h->seats[k].controller<1 || h->seats[k].controller>4 ||
                h->mp.actor[i].local!=(h->seats[k].controller==1 || h->seats[k].controller==2)) {
                snprintf(why, cap, "contribution origin has no matching owned seat"); return 0;
            }
        }
    }
    /* a projectile record's index is a u16 (RK_PROJ) */
    if (!h->proj_cap || h->proj_cap > 0xFFFF) {
        snprintf(why, cap, "a projectile pool of %u records", h->proj_cap);
        return 0;
    }
    if (h->slots < 2 || h->slots > 0xFFFF) {
        snprintf(why, cap, "a unit array of %u slots", h->slots);
        return 0;
    }
    /* after the loop: the codec's constants are bounded by RT_SLOTS, RT_FEATURES and
       RT_UNITDEFS, which may follow them */
    ent.hold = h->has_hold != 0;
    if ((ent.p[RECC_E_CODEC] || ent.p[RECC_E_PHYSICS] || ent.p[RECC_E_AIR] || ent.p[RECC_E_TYPES]) &&
        !reccodec_params(&h->codec, h->slots, h->proj_cap, h->grid_w, h->grid_h, h->wrecks,
                         h->has_unitdefs ? h->ndefs : 0, &ent, why, cap))
        return 0;
    h->has_codec = (unsigned)h->codec.usable;
    if (!h->grid_w || h->grid_w > 0xFFFF || !h->grid_h || h->grid_h > 0xFFFF || !h->wrecks ||
        h->wrecks > 0x8000) {
        snprintf(why, cap, "a feature grid of %u x %u cells, or a wreck pool of %u records",
                 h->grid_w, h->grid_h, h->wrecks);
        return 0;
    }
    return 1;
}

/* a u32 entry of a header or end block's payload, 0 when it has none */
static unsigned entry_u32(const unsigned char* b, size_t len, unsigned want)
{
    size_t at = 0;
    while (at + 4 <= len) {
        unsigned short tag, n;
        memcpy(&tag, b + at, 2);
        memcpy(&n, b + at + 2, 2);
        if (at + 4 + n > len) break;
        if (n == 4 && tag == want) return u32(b + at + 4);
        at += (4 + (size_t)n + 3) & ~(size_t)3;
    }
    return 0;
}

static void end_parse(RecReader* r, const unsigned char* b, size_t len)
{
    size_t at = 0;
    unsigned reason = 0, last = 0, iters = 0;
    while (at + 4 <= len) {
        unsigned short tag, n;
        memcpy(&tag, b + at, 2);
        memcpy(&n, b + at + 2, 2);
        if (at + 4 + n > len) break;
        if (n == 4 && tag == RT_END_REASON) reason = u32(b + at + 4);
        if (n == 4 && tag == RT_LAST) last = u32(b + at + 4);
        if (n == 4 && tag == RT_ITERATIONS) iters = u32(b + at + 4);
        at += (4 + (size_t)n + 3) & ~(size_t)3;
    }
    r->end_reason = reason;
    r->end_last = last;
    r->end_iters = iters;
    snprintf(r->end, sizeof r->end, "%s (%u iterations to GameTime %u)",
             reason && reason < sizeof END_REASONS / sizeof END_REASONS[0]
                 ? END_REASONS[reason] : "an end of an unknown reason",
             iters, last);
}

/* ---- the world ------------------------------------------------------------------------------ */

static size_t grid_bytes(const RecReader* r)
{
    return (size_t)r->h.grid_w * r->h.grid_h * REC_CELL_BYTES;
}

/* the header of the chain's segment being read: the first header until the segments are found */
static const RecHeader* seg_header(const RecReader* r)
{
    return r->nseg ? r->seg[r->chain[r->cur]].h : &r->h;
}

/* The world as a segment's first record finds it -- the controllers and the viewer its header's,
   the grid and the wreck pool the bases' when `from_base` (a snapshot's cells and wreck records
   are the ones that differ from them) and else zero (a first iteration carries every cell, and
   every wreck record that is not zero) -- and the reader at no iteration of it. What a seek
   starts from, and what a fork is crossed over. */
static void world_reset(RecReader* r, int from_base)
{
    RecWorld* w = &r->w;
    const RecHeader* h = seg_header(r);
    unsigned i;
    payload_free(r);
    /* the units the frame continues from are gone: the next stream block a reading meets after a
       reset is flagged (a segment's first, or the first after a snapshot) */
    if (r->codec) reccodec_invalidate(r->codec);
    r->in_seg = 0;
    memset(w->live, 0, w->slots);
    memset(w->changed, 0, w->slots);
    for (i = 0; i < w->slots * 2; i++) w->list[i].n = 0;
    memset(w->unit, 0, (size_t)w->slots * REC_UNIT_IMAGE);
    memset(w->player, 0, sizeof w->player);
    memset(w->pchanged, 0, sizeof w->pchanged);
    memset(w->ctl, 0, sizeof w->ctl);
    for (i = 0; i < h->nseats; i++) w->ctl[h->seats[i].index] = h->seats[i].controller;
    w->view[0] = w->view[1] = 0;
    if (h->has_options) {
        w->view[0] = (unsigned char)h->controlled;
        w->view[1] = (unsigned char)h->viewed;
    }
    for (i = 0; i < w->ntab; i++) w->tab[i].size = 0;
    recsprays_empty(w->sprays);
    w->nev = 0;
    w->nfeat = 0;
    w->nclick = 0;
    memset(w->npov, 0, sizeof w->npov);
    memset(w->nsel, 0, sizeof w->nsel);
    if (from_base) {
        memcpy(w->grid, r->base, grid_bytes(r));
        memcpy(w->pool, r->wbase, (size_t)r->h.wrecks * REC_WRECK_BYTES);
    } else {
        memset(w->grid, 0, grid_bytes(r));
        memset(w->pool, 0, (size_t)r->h.wrecks * REC_WRECK_BYTES);
    }
    memset(w->heads, 0, sizeof w->heads);
    memset(w->globals, 0, sizeof w->globals);
    memset(w->proj, 0, (size_t)r->h.proj_cap * REC_PROJ_IMAGE);
    w->nproj = 0;
    w->proj_hw = 0;
    memset(w->mapped, 0, w->mapped_bytes);
    w->mapped_whole = 0;
    r->mnext = 0;
    w->tick = 0;
    w->corrections = 0;
    r->ntouched = 0;
    r->fbracket = 0;
    r->ndeaths = 0;
    rec_yard_end(&r->yards);
    memset(&r->pend, 0, sizeof r->pend);
    r->globals_in = 0;
    r->proj_in = 0;
    r->ended = 0;
}

static int world_init(RecReader* r, char* why, unsigned cap)
{
    RecWorld* w = &r->w;
    unsigned i;
    w->slots = r->h.slots;
    w->live = (unsigned char*)calloc(w->slots, 1);
    w->changed = (unsigned char*)calloc(w->slots, 1);
    w->list = (RecList*)calloc((size_t)w->slots * 2, sizeof *w->list);
    w->unit = (uint32_t*)VirtualAlloc(NULL, (size_t)w->slots * REC_UNIT_IMAGE,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    r->touched = (unsigned*)malloc((size_t)w->slots * sizeof *r->touched);
    r->yards.st = (unsigned char*)calloc(w->slots, 1);
    r->yards.touched = (unsigned*)calloc(w->slots, sizeof *r->yards.touched);
    w->ntab = r->h.ntab;
    w->tab = (RecTable*)calloc(w->ntab ? w->ntab : 1, sizeof *w->tab);
    w->grid = (unsigned char*)VirtualAlloc(NULL, grid_bytes(r), MEM_COMMIT | MEM_RESERVE,
                                           PAGE_READWRITE);
    r->base = (unsigned char*)VirtualAlloc(NULL, grid_bytes(r), MEM_COMMIT | MEM_RESERVE,
                                           PAGE_READWRITE);
    w->pool = (unsigned char*)calloc(r->h.wrecks, REC_WRECK_BYTES);
    r->wbase = (unsigned char*)calloc(r->h.wrecks, REC_WRECK_BYTES);
    w->proj = (unsigned char*)calloc(r->h.proj_cap, REC_PROJ_IMAGE);
    w->mapped_bytes = REC_MAPPED_BYTES(r->h.grid_w, r->h.grid_h);
    w->mapped = (unsigned char*)malloc(w->mapped_bytes ? w->mapped_bytes : 1);
    w->sel = (RecSelEntry*)calloc((size_t)REC_PLAYERS * REC_SEL_MAX, sizeof *w->sel);
    w->sprays = recsprays_begin(w->slots);
    if (!w->live || !w->changed || !w->list || !w->unit || !r->touched || !r->yards.st ||
        !r->yards.touched || !w->tab || !w->grid ||
        !r->base || !w->pool || !r->wbase || !w->proj || !w->mapped || !w->sel || !w->sprays) {
        snprintf(why, cap, "no memory for the world of %u slots and %u x %u cells", w->slots,
                 r->h.grid_w, r->h.grid_h);
        return 0;
    }
    for (i = 0; i < w->ntab; i++) {
        w->tab[i].index = r->h.tab[i].index;
        w->tab[i].stride = r->h.tab[i].stride;
    }
    world_reset(r, 0);
    return 1;
}

static RecTable* table_of(RecWorld* w, unsigned index)
{
    unsigned i;
    for (i = 0; i < w->ntab; i++)
        if (w->tab[i].index == index) return &w->tab[i];
    return NULL;
}

static int grow(unsigned char** p, unsigned* cap, size_t bytes)
{
    unsigned char* n;
    if (bytes <= *cap) return 1;
    n = (unsigned char*)realloc(*p, bytes);
    if (!n) return 0;
    *p = n;
    *cap = (unsigned)bytes;
    return 1;
}

static void touch(RecReader* r, unsigned slot, unsigned what)
{
    if (!r->w.changed[slot]) r->touched[r->ntouched++] = slot;
    r->w.changed[slot] |= (unsigned char)what;
}

/* the dwords a delta names: its bitmap's set bits, none past `dwords` */
static int delta_count(const unsigned char* bits, unsigned nbytes, unsigned dwords, unsigned* n)
{
    unsigned i, c = 0;
    for (i = 0; i < nbytes * 8; i++)
        if (bits[i >> 3] >> (i & 7) & 1) {
            if (i >= dwords) return 0;
            c++;
        }
    *n = c;
    return 1;
}

static void delta_apply(uint32_t* img, const unsigned char* bits, unsigned dwords,
                        const unsigned char* vals)
{
    unsigned i, k = 0;
    for (i = 0; i < dwords; i++)
        if (bits[i >> 3] >> (i & 7) & 1) memcpy(&img[i], vals + 4 * k++, 4);
}


/* An RK_CELLS, bounded by the grid: 1 well-formed. */
static int cells_ok(const RecReader* r, const unsigned char* b, unsigned len)
{
    unsigned short h[4];
    if (len < 8) return 0;
    memcpy(h, b, 8);
    return h[2] && h[2] <= REC_CELLS_MAX && !h[3] && len == 8u + h[2] * REC_CELL_BYTES &&
           (unsigned)h[0] + h[2] <= r->h.grid_w && h[1] < r->h.grid_h;
}

static int list_add(RecEvent** l, unsigned* n, unsigned* cap, unsigned kind, unsigned len,
                    const unsigned char* b)
{
    if (*n == *cap) {
        unsigned c = *cap ? *cap * 2 : 256;
        RecEvent* e = (RecEvent*)realloc(*l, c * sizeof *e);
        if (!e) return 0;
        *l = e;
        *cap = c;
    }
    (*l)[*n].kind = kind;
    (*l)[*n].len = len;
    (*l)[*n].b = b;
    (*n)++;
    return 1;
}

/* a segment whose stream writes its nano sprays as sessions (RT_HOLD's RECL_SPRAYS) */
static int sprays_on(const RecHeader* h)
{
    return h->has_hold && (h->hold.flags & RECL_SPRAYS);
}

/* An event of the step, by the brackets' grammar (tagpu_recwriter.c, feature_record): a feature
   call's holds no other and cells only when it is the tick's, which is inside none; a death's
   holds its explosion's feature calls, in a multiplayer recording its corpse's (FR_CORPSE, inside
   a death only), and may hold deaths; a draw is inside one, FS_SOURCE inside the tick only, of
   a segment whose playing keeps the engine's slots: a multiplayer one, or one that holds its
   cells (recr_holds_cells); a script slice and an owner's query are inside none, and name a slot
   of the level's; a yard names one too, outside every feature call, in a death or not, where THE
   YARD RULE lets it stand (tagpu_recfmt.h), which the creates and deaths feed. 1 taken, 0
   malformed. */
static int event_take(RecReader* r, unsigned kind, const unsigned char* b, unsigned len)
{
    RecWorld* w = &r->w;
    const RecHeader* h = seg_header(r);   /* a takeover's segment is a skirmish's */
    unsigned id;
    switch (kind) {
    case RK_EV_NET_CREATE: {
        RecNetCreate create;
        unsigned i, slot;
        if (h->mode != 3 || !h->has_mp_seats ||
            !h->has_unitdefs || !rec_event_len(kind, len)) return 0;
        memcpy(&create, b, sizeof create);
        slot = create.unit - 1;
        if (slot < 1 || slot >= w->slots || !create.type || create.type >= h->ndefs ||
            create.owner >= REC_PLAYERS || create.water > 4 || create.reserved ||
            !(create.unfinished >= 0.0f && create.unfinished <= 1.0f)) return 0;
        for (i = 0; i < h->mp_seats.count; i++) {
            const RecMpSeat* seat = &h->mp_seats.actor[i];
            if (seat->seat == create.owner && slot >= seat->first && slot <= seat->last) break;
        }
        if (i == h->mp_seats.count) return 0;
        break;
    }
    case RK_EV_FEATURE:
        if (r->fbracket || (b[0] == FR_TICK && r->ndeaths) ||
            (b[0] == FR_CORPSE && (!r->ndeaths || h->mode != 3)) ||
            b[1] > REC_PHASE_LATE || b[2] || b[3] ||
            !rec_root_len(b[0], len))
            return 0;
        r->fbracket = b[0] + 1u;
        break;
    case RK_EV_FEATURE_END:
        if (len != 8 || r->fbracket != b[0] + 1u || b[1] || b[2] || b[3]) return 0;
        r->fbracket = 0;
        break;
    case RK_EV_DEATH:
        if (r->fbracket || r->ndeaths == REC_DEATHS_MAX || !rec_event_len(kind, len) ||
            !rec_yard_take(&r->yards, kind, b, w->live, w->slots, r->death, r->ndeaths))
            return 0;
        memcpy(&r->death[r->ndeaths++], b, 4);
        break;
    case RK_EV_DEATH_END:
        if (len != 4 || !r->ndeaths) return 0;
        memcpy(&id, b, 4);
        if (id != r->death[r->ndeaths - 1]) return 0;
        r->ndeaths--;
        break;
    case RK_EV_DRAW:
        if ((!r->fbracket && !r->ndeaths) || !rec_event_len(kind, len) || b[0] >= FS_SITES ||
            b[1] || b[2] || b[3] ||
            (b[0] == FS_SOURCE && (r->fbracket != FR_TICK + 1 ||
                                   !(h->mode == 3 || recr_holds_cells(h)))))
            return 0;
        break;
    case RK_CELLS:
        if (r->fbracket != FR_TICK + 1 || !cells_ok(r, b, len)) return 0;
        break;
    case RK_EV_SLICE:
        if (r->fbracket || r->ndeaths || len != 4) return 0;
        memcpy(&id, b, 4);
        if (id - 1u < 1 || id - 1u >= w->slots) return 0;
        break;
    case RK_EV_QUERY:
        if (r->fbracket || r->ndeaths || len != 8 || b[4] >= REC_QUERIES || b[5] || b[6] || b[7])
            return 0;
        memcpy(&id, b, 4);
        if (id - 1u < 1 || id - 1u >= w->slots) return 0;
        break;
    case RK_EV_YARD:
        if (r->fbracket || len != 8 || b[4] > 1 || b[5] || b[6] || b[7]) return 0;
        memcpy(&id, b, 4);
        if (id - 1u < 1 || id - 1u >= w->slots ||
            !rec_yard_take(&r->yards, kind, b, w->live, w->slots, r->death, r->ndeaths))
            return 0;
        break;
    default:
        if (kind < RK_EV_CREATE || kind > RK_EV_SPRAY || !rec_event_len(kind, len) ||
            !rec_yard_take(&r->yards, kind, b, w->live, w->slots, r->death, r->ndeaths))
            return 0;
        /* the images come before the events: the units are this iteration's */
        if (kind == RK_EV_SPRAY && sprays_on(h))
            recsprays_spray(w->sprays, w->tick, b, w->unit, w->live);
        break;
    }
    return list_add(&w->ev, &w->nev, &r->evcap, kind, len, b);
}

/* An RK_ORDERS: the first record of a list starts it, and the rest must be its own, in order,
   until every node has come (tagpu_recwriter.c, list_record). 1 applied, 0 malformed. */
static int list_record(RecReader* r, const unsigned char* b, unsigned len)
{
    RecWorld* w = &r->w;
    unsigned short slot, total, first;
    unsigned list, k;
    RecList* l;
    if (len < 8 || (len - 8) % REC_ORDER_IMAGE) return 0;
    memcpy(&slot, b, 2);
    memcpy(&total, b + 4, 2);
    memcpy(&first, b + 6, 2);
    list = b[2];
    k = (len - 8) / REC_ORDER_IMAGE;
    if (list > 1 || b[3] || slot < 1 || slot >= w->slots || !w->live[slot] ||
        total > REC_ORDERS_MAX || k > REC_ORDERS_CHUNK || first + k > total || (total && !k))
        return 0;
    l = &w->list[(size_t)slot * 2 + list];
    if (!first) {
        if (r->pend.next < r->pend.total) return 0;
        r->pend.slot = slot;
        r->pend.list = list;
        r->pend.total = total;
        r->pend.next = 0;
        if (!grow(&l->img, &l->cap, (size_t)total * REC_ORDER_IMAGE)) return 0;
        l->n = total;
    } else if (r->pend.slot != slot || r->pend.list != list || r->pend.total != total ||
               r->pend.next != first) {
        return 0;
    }
    r->pend.next = first + k;
    memcpy(l->img + (size_t)first * REC_ORDER_IMAGE, b + 8, (size_t)k * REC_ORDER_IMAGE);
    touch(r, slot, list ? RECW_SUB : RECW_MAIN);
    return 1;
}

/* A sample, `tick` its GameTime, as the seat's latest: later than the one it replaces. */
static int pov_take(RecWorld* w, const unsigned char* b, unsigned tick)
{
    RecPov p;
    unsigned s;
    memcpy(&p, b, sizeof p);
    s = p.player;
    if (s >= REC_PLAYERS || !rec_pov_ok(&p) || tick > w->tick ||
        (w->npov[s] && w->pov_tick[s][w->npov[s] - 1] >= tick))
        return 0;
    if (w->npov[s] == 2) {
        w->pov[s][0] = w->pov[s][1];
        w->pov_tick[s][0] = w->pov_tick[s][1];
        w->npov[s] = 1;
    }
    w->pov[s][w->npov[s]] = p;
    w->pov_tick[s][w->npov[s]] = tick;
    w->npov[s]++;
    return 1;
}

/* THE PERSPECTIVE (tagpu_recpov.h): a skirmish segment's, outside every bracket, as
   the writer has them (tagpu_recwriter.c, pov_record). RK_POV and RK_CLICK are a stream's, RK_POV_AT
   a snapshot's; a selection's slots lie inside the seat's block as its player image, applied
   before it, names it. 1 applied, 0 malformed. */
static int pov_record(RecReader* r, unsigned kind, const unsigned char* b, unsigned len)
{
    RecWorld* w = &r->w;
    const RecHeader* h = seg_header(r);
    if (h->has_mp || r->fbracket || r->ndeaths) return 0;
    switch (kind) {
    case RK_POV:
        return !r->snap && len == sizeof(RecPov) && pov_take(w, b, w->tick);
    case RK_POV_AT:
        return r->snap && len == REC_POV_AT_BYTES && pov_take(w, b + 4, u32(b));
    case RK_CLICK: {
        RecClick c;
        if (r->snap || len != sizeof c) return 0;
        memcpy(&c, b, sizeof c);
        if (c.player >= REC_PLAYERS || !rec_click_ok(&c)) return 0;
        return list_add(&w->click, &w->nclick, &r->clickcap, kind, len, b);
    }
    case RK_SELECT: {
        RecSel head;
        unsigned lo, hi;
        long prev = -1;
        if (!rec_sel_head(b, len, h->units_each, &head) || !rec_sel_whole(&head) ||
            head.player >= REC_PLAYERS)
            return 0;
        if (head.count && (!rec_sel_block((const unsigned char*)w->player[head.player], w->slots,
                                          &lo, &hi) ||
                           !rec_sel_entries_ok(b + sizeof head, head.count, lo, hi, &prev)))
            return 0;
        memcpy(w->sel + (size_t)head.player * REC_SEL_MAX, b + sizeof head,
               (size_t)head.count * sizeof(RecSelEntry));
        w->nsel[head.player] = head.count;
        return 1;
    }
    default:
        return 0;
    }
}

/* tick_read's `events` once the iteration's spray sessions have begun: they end it */
#define IT_SESSIONS 2

/* One record of an iteration after its tick. 1 applied, 0 malformed. */
static int apply(RecReader* r, unsigned kind, const unsigned char* b, unsigned len, int* events)
{
    RecWorld* w = &r->w;
    unsigned short id = 0;
    unsigned n = 0, off = 0;
    RecTable* t;
    if (r->pend.next < r->pend.total && kind != RK_ORDERS) return 0;
    if (*events == IT_SESSIONS && kind != RK_SPRAY_SESSION) return 0;
    if (len >= 2) memcpy(&id, b, 2);
    /* a click follows the step's events, and may follow the images directly */
    if (kind == RK_CLICK) return pov_record(r, kind, b, len);
    /* an event by the writer's rule, rec_section_of: the same predicate sets its `evented` */
    if (rec_section_of(kind) == RCS_EVENTS || (kind == RK_CELLS && *events)) {
        if (!event_take(r, kind, b, len)) return 0;
        *events = 1;
        return 1;
    }
    /* the spray sessions follow the iteration's last other record, outside every bracket, slots
       ascending (tagpu_recfmt.h, RK_SPRAY_SESSION); a stream's only */
    if (kind == RK_SPRAY_SESSION) {
        uint32_t unit;
        if (!sprays_on(seg_header(r)) || r->snap || r->fbracket || r->ndeaths || len < 4) return 0;
        memcpy(&unit, b, 4);
        if ((*events == IT_SESSIONS && unit <= r->session_id) ||
            !recsprays_session(w->sprays, b, len))
            return 0;
        r->session_id = unit;
        *events = IT_SESSIONS;
        return 1;
    }
    /* the images come before the events of the step after them (tagpu_recfmt.h, RK_TICK) */
    if (*events) return 0;
    switch (kind) {
    case RK_CELLS: {
        unsigned short h[4];
        if (!cells_ok(r, b, len)) return 0;
        memcpy(h, b, 8);
        memcpy(w->grid + ((size_t)h[1] * r->h.grid_w + h[0]) * REC_CELL_BYTES, b + 8,
               (size_t)h[2] * REC_CELL_BYTES);
        return list_add(&w->feat, &w->nfeat, &r->fcap, kind, len, b);
    }
    case RK_WRECK:
        if (len != 4 + REC_WRECK_BYTES || id >= r->h.wrecks || b[2] || b[3]) return 0;
        memcpy(w->pool + (size_t)id * REC_WRECK_BYTES, b + 4, REC_WRECK_BYTES);
        return list_add(&w->feat, &w->nfeat, &r->fcap, kind, len, b);
    case RK_FEATURES:
        if (len != 16) return 0;
        memcpy(w->heads, b, 16);
        return list_add(&w->feat, &w->nfeat, &r->fcap, kind, len, b);
    case RK_UNIT:
        if (len != 4 + REC_UNIT_IMAGE || id < 1 || id >= w->slots) return 0;
        memcpy(w->unit + (size_t)id * REC_UNIT_DWORDS, b + 4, REC_UNIT_IMAGE);
        w->live[id] = 1;
        touch(r, id, RECW_IMAGE | RECW_BORN);
        return 1;
    case RK_UNIT_DELTA:
        if (len < REC_UNIT_DELTA_HEAD || id < 1 || id >= w->slots || !w->live[id] ||
            !delta_count(b + 2, REC_UNIT_BITS, REC_UNIT_DWORDS, &n) ||
            len != REC_UNIT_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply(w->unit + (size_t)id * REC_UNIT_DWORDS, b + 2, REC_UNIT_DWORDS,
                    b + REC_UNIT_DELTA_HEAD);
        touch(r, id, RECW_IMAGE);
        return 1;
    case RK_UNIT_GONE:
        if (len != 4 || id < 1 || id >= w->slots) return 0;
        w->live[id] = 0;
        w->list[(size_t)id * 2].n = 0;
        w->list[(size_t)id * 2 + 1].n = 0;
        touch(r, id, RECW_GONE);
        return 1;
    case RK_PLAYER:
        if (len != 4 + REC_PLAYER_IMAGE || id >= REC_PLAYERS) return 0;
        memcpy(w->player[id], b + 4, REC_PLAYER_IMAGE);
        w->pchanged[id] = 1;
        return 1;
    case RK_PLAYER_DELTA:
        if (len < REC_PLAYER_DELTA_HEAD || id >= REC_PLAYERS ||
            !delta_count(b + 2, REC_PLAYER_BITS, REC_PLAYER_DWORDS, &n) ||
            len != REC_PLAYER_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply(w->player[id], b + 2, REC_PLAYER_DWORDS, b + REC_PLAYER_DELTA_HEAD);
        w->pchanged[id] = 1;
        return 1;
    case RK_TABLE_SIZE:
        if (len != 8 || !(t = table_of(w, id))) return 0;
        memcpy(&n, b + 4, 4);
        if (n % t->stride || n > RECR_BLOCK_MAX || !grow(&t->img, &t->cap, n)) return 0;
        if (n > t->size) memset(t->img + t->size, 0, n - t->size);
        t->size = n;
        return 1;
    case RK_TABLE_SPAN:
        if (len < 8 || !(t = table_of(w, id))) return 0;
        memcpy(&off, b + 4, 4);
        n = len - 8;
        if (off > t->size || n > t->size - off) return 0;
        memcpy(t->img + off, b + 8, n);
        return 1;
    case RK_ORDERS:
        return list_record(r, b, len);
    case RK_CONTROLLER:
        if (len != 4 || b[0] >= REC_PLAYERS || b[2] || b[3]) return 0;
        w->ctl[b[0]] = b[1];
        return 1;
    case RK_VIEWER:
        if (len != 4 || b[2] || b[3]) return 0;
        w->view[0] = b[0];
        w->view[1] = b[1];
        return 1;
    case RK_GLOBALS:
        if (len != sizeof w->globals) return 0;
        memcpy(w->globals, b, len);
        r->globals_in = 1;
        return 1;
    case RK_GLOBALS_DELTA:
        if (len < REC_GLOBALS_DELTA_HEAD || !r->globals_in ||
            !delta_count(b, REC_GLOBALS_BITS, REC_GLOBALS_DWORDS, &n) ||
            len != REC_GLOBALS_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply(w->globals, b, REC_GLOBALS_DWORDS, b + REC_GLOBALS_DELTA_HEAD);
        return 1;
    case RK_PROJ_HEAD: {
        unsigned v[2];
        if (len != 8) return 0;
        memcpy(v, b, 8);
        if (v[0] > v[1] || v[1] > r->h.proj_cap) return 0;
        /* Records past the extent are zero in every copy -- the recording machine's pool, the
           writer's (proj_head) and tarec's -- so an extent that shrinks zeroes the records it
           gives up here too. A recording machine's extent only grows within a level; a head
           correction is what can lower it. */
        if (v[1] < w->proj_hw)
            memset(w->proj + (size_t)v[1] * REC_PROJ_IMAGE, 0,
                   (size_t)(w->proj_hw - v[1]) * REC_PROJ_IMAGE);
        w->nproj = v[0];
        w->proj_hw = v[1];
        r->proj_in = 1;
        return 1;
    }
    case RK_PROJ:
        if (len != 4 + REC_PROJ_IMAGE || b[2] || b[3] || !r->proj_in || id >= w->proj_hw)
            return 0;
        memcpy(w->proj + (size_t)id * REC_PROJ_IMAGE, b + 4, REC_PROJ_IMAGE);
        return 1;
    case RK_PROJ_DELTA:
        if (len < REC_PROJ_DELTA_HEAD || !r->proj_in || id >= w->proj_hw ||
            !delta_count(b + 2, REC_PROJ_BITS, REC_PROJ_DWORDS, &n) ||
            len != REC_PROJ_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply((uint32_t*)(w->proj + (size_t)id * REC_PROJ_IMAGE), b + 2, REC_PROJ_DWORDS,
                    b + REC_PROJ_DELTA_HEAD);
        return 1;
    case RK_MAPPED: {
        /* the map whole, from offset 0 in order, once a recording and once a snapshot */
        unsigned off;
        if (len < 5 || len > 4 + REC_MAPPED_CHUNK) return 0;
        memcpy(&off, b, 4);
        if (off != r->mnext || len - 4 > w->mapped_bytes - off) return 0;
        memcpy(w->mapped + off, b + 4, len - 4);
        r->mnext += len - 4;
        w->mapped_whole = r->mnext == w->mapped_bytes;
        return 1;
    }
    case RK_MAPPED_RUNS: {
        /* every run checked before any is applied, so a malformed record changes nothing */
        unsigned at, v[2], pass;
        if (!w->mapped_whole || !len || len > REC_MAPPED_RUNS) return 0;
        for (pass = 0; pass < 2; pass++) {
            for (at = 0; at < len; at += (8 + v[1] + 3) & ~3u) {
                if (len - at < 8) return 0;
                memcpy(v, b + at, 8);
                if (!v[1] || v[1] > len - at - 8 || v[0] > w->mapped_bytes ||
                    v[1] > w->mapped_bytes - v[0])
                    return 0;
                if (pass) memcpy(w->mapped + v[0], b + at + 8, v[1]);
            }
        }
        return 1;
    }
    case RK_CORRECT:
        if (len != 4) return 0;
        w->corrections++;
        return 1;
    case RK_POV:
    case RK_POV_AT:
    case RK_SELECT:
        return pov_record(r, kind, b, len);
    default:
        return 0;   /* the ring's own kinds are never in a block, nor a dump's RK_IMAGE */
    }
}

/* An iteration is whole only when no order list it began is still owed chunks -- list_record
   sets the list's count from its first chunk, so a list cut short would leave the rest of its
   nodes unwritten -- no feature bracket is still open, and the explored map is whole: the first
   iteration and every snapshot carry it whole (cap_mapped). 1, or -1 with `why`. */
static int iteration_done(const RecReader* r, char* why, unsigned cap)
{
    if (!r->w.mapped_whole) {
        snprintf(why, cap, "an iteration without the explored map whole, before byte %llu",
                 r->off);
        return -1;
    }
    if (r->pend.next < r->pend.total) {
        snprintf(why, cap, "an order list cut short in the block before byte %llu", r->off);
        return -1;
    }
    if (r->fbracket || r->ndeaths) {
        snprintf(why, cap, "a feature call's or a death's events cut short in the block before "
                 "byte %llu", r->off);
        return -1;
    }
    if (r->yards.held) {
        snprintf(why, cap, "a yard of a unit not live whose create does not follow in its step, "
                 "in the block before byte %llu (tagpu_recfmt.h, THE YARD RULE)", r->off);
        return -1;
    }
    return 1;
}

/* The iteration read is whole (iteration_done), and the sprays its spray sessions imply are put
   after its events, slots ascending (tagpu_reclossy.c, THE SPRAY SESSIONS): 1, or -1 with `why`
   for a session the filter would never have left open. */
static int tick_end(RecReader* r, char* why, unsigned cap)
{
    RecWorld* w = &r->w;
    const unsigned char* b;
    unsigned n, i;
    int k;
    if (iteration_done(r, why, cap) < 0) return -1;
    if (!sprays_on(seg_header(r))) return 1;
    k = recsprays_imply(w->sprays, w->tick, w->unit, w->live, &b, &n);
    if (k <= 0) {
        snprintf(why, cap, !k ? "a spray session at GameTime %u predicts from no single spray "
                 "of its unit, or follows a unit not live, in the block before byte %llu" :
                 "no memory for the sprays of GameTime %u (before byte %llu)", w->tick, r->off);
        return -1;
    }
    for (i = 0; i < n; i++)
        if (!list_add(&w->ev, &w->nev, &r->evcap, RK_EV_SPRAY, 4 + REC_SPRAY_BODY,
                      b + (size_t)i * (4 + REC_SPRAY_BODY))) {
            snprintf(why, cap, "no memory for the events of GameTime %u", w->tick);
            return -1;
        }
    return 1;
}

static int tick_read(RecReader* r, char* why, unsigned cap)
{
    RecWorld* w = &r->w;
    unsigned i;
    int began = 0, events = 0;
    for (i = 0; i < r->ntouched; i++) w->changed[r->touched[i]] = 0;
    r->ntouched = 0;
    memset(w->pchanged, 0, sizeof w->pchanged);
    w->nev = 0;
    w->nfeat = 0;
    w->nclick = 0;
    rec_yard_end(&r->yards);            /* iteration_done has checked the last step's */
    for (;;) {
        RecHdr hd;
        const unsigned char* b;
        if (r->at >= r->rawn) {
            RecBlock blk;
            int k;
            /* a stream block ends where an iteration does (tagpu_recwriter.c, iteration_done) */
            if (began) return tick_end(r, why, cap);
            if (r->ended) {
                snprintf(why, cap, "%s", r->end);
                return 0;
            }
            k = block_head(r, &blk, why, cap);
            if (k < 0) return -1;
            if (!k) {
                r->ended = 1;
                snprintf(r->end, sizeof r->end, "INCOMPLETE: the file ends without its end block");
                snprintf(why, cap, "%s", r->end);
                return 0;
            }
            if (blk.kind != RB_STREAM && blk.kind != RB_END) {
                if (block_skip(r, &blk, why, cap) < 0) return -1;   /* a snapshot: recr_seek's */
                /* the writer closed every spray session at the snapshot's iteration and emptied
                   its table: a reader that starts here holds none either */
                if (blk.kind == RB_SNAPSHOT) recsprays_empty(w->sprays);
                continue;
            }
            if (block_body(r, &blk, why, cap) < 0) return -1;
            if (blk.kind == RB_END) {
                r->ended = 1;
                end_parse(r, r->raw, r->rawn);
                payload_free(r);
                snprintf(why, cap, "%s", r->end);
                return 0;
            }
            continue;
        }
        if (r->at + 4 > r->rawn) {
            snprintf(why, cap, "a record header cut short in the block before byte %llu", r->off);
            return -1;
        }
        memcpy(&hd, r->raw + r->at, 4);
        if (r->at + 4 + hd.len > r->rawn) {
            snprintf(why, cap, "a record cut short in the block before byte %llu", r->off);
            return -1;
        }
        b = r->raw + r->at + 4;
        if (hd.kind == RK_TICK) {
            /* the next iteration's: left for the next call */
            if (began) return tick_end(r, why, cap);
            if (hd.len != 4 || r->pend.next < r->pend.total) {
                snprintf(why, cap, "a malformed tick in the block before byte %llu", r->off);
                return -1;
            }
            memcpy(&w->tick, b, 4);
            began = 1;
        } else if (!began || !apply(r, hd.kind, b, hd.len, &events)) {
            snprintf(why, cap, "a malformed record of kind %u at GameTime %u, in the block before "
                     "byte %llu", hd.kind, w->tick, r->off);
            return -1;
        }
        r->at += (4 + (size_t)hd.len + 3) & ~(size_t)3;
    }
}

/* An iteration read, and what reading it makes true: the world holds an iteration of the chain's
   segment `cur`, and the bases are that segment's. They are taken at a segment's first iteration
   read: recr_open and recr_timeline start at the root's, a seek starts at a segment's first
   iteration or reads it before a snapshot of that segment, and a crossing starts at the child's,
   each over world_reset(r, 0). Its grid images are every cell (the feature tick's cells inside its
   events are not applied to the grid).
   INVARIANT: both bases are the writer's (tagpu_recwriter.c, iteration_done) by construction --
   each side's are the pool and the grid built from zero by the RK_WRECKs and the images of the
   segment's first iteration alone, an image being an RK_CELLS before the iteration's first event
   on both sides (apply; the writer's feature_record), each index bounded by the same h.wrecks and
   grid size, which every segment of the chain shares. */
static int tick_take(RecReader* r, char* why, unsigned cap)
{
    int k = tick_read(r, why, cap);
    if (k > 0) {
        r->in_seg = 1;
        r->read_any = 1;
        if (r->base_seg != (int)r->cur) {
            memcpy(r->base, r->w.grid, grid_bytes(r));
            memcpy(r->wbase, r->w.pool, (size_t)r->h.wrecks * REC_WRECK_BYTES);
            r->base_seg = (int)r->cur;
        }
    }
    return k;
}

/* the chain's segment i, and the last GameTime it covers: its child's fork_at - 1, or its own last
   iteration when it is the chain's last */
static const RecSeg* chain_seg(const RecReader* r, unsigned i) { return &r->seg[r->chain[i]]; }

static unsigned cover_end(const RecReader* r, unsigned i)
{
    return i + 1 < r->nchain ? chain_seg(r, i + 1)->t.fork_at - 1 : chain_seg(r, i)->t.last;
}

int recr_tick(RecReader* r, char* why, unsigned cap)
{
    /* THE BOUNDARY: the next iteration is the child's, which only a seek applies (recr_seek) */
    if (r->in_seg && r->cur + 1 < r->nchain) {
        const RecTimeline* c = &chain_seg(r, r->cur + 1)->t;
        if ((unsigned long long)r->w.tick + 1 >= c->fork_at) {
            snprintf(why, cap, "timeline %u continues from GameTime %u", c->id, c->fork_at);
            return RECR_BOUNDARY;
        }
    }
    return tick_take(r, why, cap);
}

/* ---- the segments --------------------------------------------------------------------------- */

/* THE SCAN'S THREE OUTCOMES for a block: WHOLE; TORN, what an interrupted append leaves -- the file
   ends inside the block, or its bytes fail their magic or a CRC and nothing but zeros follows
   them, as a write cut short by a crash leaves the file's tail; and anything else, which is not a
   tear. That is DAMAGED -- the bytes fail and something follows them -- or UNREADABLE: a read, an
   allocation or an inflate of bytes that passed their CRC failed. Only a torn block may be cut
   (recr_append_point): the writer appends one block after another, so only the file's last
   block can be torn, and a cut at any other block would delete the complete blocks after it. */
enum { BLK_UNREADABLE = -1, BLK_TORN = 0, BLK_WHOLE = 1 };

/* 1 when every byte of the file from `from` to `size` is zero (none included), 0 when one is not,
   -1 when they cannot be read. The file's position moves. */
static int zero_tail(RecReader* r, unsigned long long from, unsigned long long size)
{
    unsigned char buf[4096];
    if (from >= size) return 1;
    if (!seek_to(r->f, from)) return -1;
    while (from < size) {
        DWORD n = (DWORD)(size - from < sizeof buf ? size - from : sizeof buf), i;
        if (!read_all(r->f, buf, n)) return -1;
        for (i = 0; i < n; i++)
            if (buf[i]) return 0;
        from += n;
    }
    return 1;
}

/* The CRC of the payload of the block whose header `b` is at `off`, read in pieces and never
   inflated: BLK_WHOLE when it matches, BLK_TORN when it does not, BLK_UNREADABLE when the bytes
   cannot be read. The file's position moves. */
static int crc_stream(RecReader* r, unsigned long long off, const RecBlock* b)
{
    unsigned char buf[16384];
    unsigned left = b->packed;
    uint32_t crc = 0;
    if (!seek_to(r->f, off + sizeof *b)) return BLK_UNREADABLE;
    while (left) {
        DWORD n = left < sizeof buf ? left : (DWORD)sizeof buf;
        if (!read_all(r->f, buf, n)) return BLK_UNREADABLE;
        crc = (uint32_t)Crc32_ComputeBuf(crc, buf, n);
        left -= n;
    }
    return crc == b->crc_body ? BLK_WHOLE : BLK_TORN;
}

/* The payload of the block whose header `b` is at `off`, checked and unpacked into a buffer of its
   own (*out, *n), so the reading's payload -- which the world's events point into -- is left
   alone. BLK_WHOLE; BLK_TORN when the bytes fail their CRC; BLK_UNREADABLE (why) when they cannot
   be read, the buffer cannot be had, the block's flags are refused -- sectioned or chained among
   them, which none of the kinds read here may be -- or bytes that passed their CRC are not one
   zstd frame of the size the header names. The file's position moves. No block read here is
   counted in a seek's work (recr_seek_work): a seek decodes through block_body alone. */
static int body_get(RecReader* r, unsigned long long off, const RecBlock* b, unsigned char** out,
                    size_t* n, char* why, unsigned cap)
{
    unsigned char* pk;
    unsigned char* raw;
    *out = NULL;
    if (block_flags_bad(b) || (b->flags & (RBF_SECTIONS | RBF_CHAINED))) {
        snprintf(why, cap, "a block of kind %u at byte %llu with %s", b->kind, off,
                 block_flags_bad(b) ? block_flags_bad(b) : (b->flags & RBF_SECTIONS) ? "sections"
                 : "a chain, read alone");
        return BLK_UNREADABLE;
    }
    pk = (unsigned char*)malloc(b->packed ? b->packed : 1);
    if (!pk) {
        snprintf(why, cap, "no memory for the block at byte %llu", off);
        return BLK_UNREADABLE;
    }
    if (!seek_to(r->f, off + sizeof *b) || (b->packed && !read_all(r->f, pk, b->packed))) {
        free(pk);
        snprintf(why, cap, "cannot read the block at byte %llu (%lu)", off, GetLastError());
        return BLK_UNREADABLE;
    }
    if ((uint32_t)Crc32_ComputeBuf(0, pk, b->packed) != b->crc_body) {
        free(pk);
        return BLK_TORN;
    }
    if (b->flags & RBF_STORED) {
        *out = pk;
        *n = b->packed;
        return BLK_WHOLE;
    }
    raw = (unsigned char*)malloc(b->raw);
    if (!raw || !dctx_of(r) || !reccodec_unpack(r->dctx, raw, b->raw, pk, b->packed)) {
        if (!raw || !r->dctx)
            snprintf(why, cap, "no memory to unpack the block at byte %llu", off);
        else
            snprintf(why, cap, "the block at byte %llu is not one zstd frame of its %u bytes", off,
                     b->raw);
        free(raw);
        free(pk);
        return BLK_UNREADABLE;
    }
    free(pk);
    *out = raw;
    *n = b->raw;
    return BLK_WHOLE;
}

/* The block at `at` failed its magic or a CRC, and its bytes end at `after` (`at` itself when its
   header failed): torn when only zeros follow, else damaged. A reading stops at either; an append
   (`tail`) stops at a torn one and refuses a damaged one. 0 stop here; -1 refuse (why). */
static int failed_block(RecReader* r, unsigned long long at, unsigned long long after,
                        unsigned long long size, int tail, char* why, unsigned cap)
{
    int k;
    if (!tail) return 0;
    k = zero_tail(r, after, size);
    if (k > 0) return 0;
    if (k < 0)
        snprintf(why, cap, "cannot read the file after byte %llu (%lu)", after, GetLastError());
    else
        snprintf(why, cap, "the block at byte %llu is damaged and data follows it", at);
    return -1;
}

/* a segment's first, last and end from its blocks in `ix`, after the last was dropped */
static void seg_tally(RecSeg* s, const RecIndexEntry* ix)
{
    unsigned j;
    s->nstream = 0;
    s->t.first = s->t.last = 0;
    s->t.end_off = s->t.first_off;
    for (j = s->i0; j < s->i0 + s->n; j++) {
        if (ix[j].kind == RB_STREAM) {
            if (!s->nstream++) s->t.first = ix[j].first;
            s->t.last = ix[j].last;
        }
        s->t.end_off = ix[j].off + sizeof(RecBlock) + ix[j].packed;
    }
}

static void segs_free(RecSeg* sg, unsigned n)
{
    unsigned i;
    for (i = 1; i < n; i++) free(sg[i].h);
}

/* the segment of timeline `id`, or RECR_TIMELINES */
static unsigned seg_index(const RecReader* r, unsigned id)
{
    unsigned i;
    for (i = 0; i < r->nseg; i++)
        if (r->seg[i].t.id == id) return i;
    return RECR_TIMELINES;
}

/* THE SCAN: every block header from byte 8, checked as a reading checks it -- the magic, the
   header's CRC, the sizes against RECR_BLOCK_MAX, the payload inside the file -- to the first
   block that fails, or to an RB_END that no valid RB_HEADER follows. A header's and an end block's
   payloads are read and checked too: a fork's header names its timeline, an end its reason. A
   stream or snapshot block's payload is not read here: a reading checks its CRC when it reads it
   (block_body) and stops there. With `tail`, for an append (recr_append_point), the last block's
   payload is checked as well and a torn one is not kept, and a block that fails is accepted only
   as a tear (THE SCAN'S THREE OUTCOMES): a damaged block refuses the append. Without it, a
   reading stops at a damaged block as at a torn one. A block that cannot be read refuses either.
   Bounded by the file's size and by RECR_TIMELINES; the bounds of recr_timelines refuse the file
   by name. On success the index, the segments and the chain's segments are the file's as it is
   now; on a failure they are as they were. The file's next read is put back where it was. 1, or
   -1 (why). */
static int segs_scan(RecReader* r, int tail, char* why, unsigned cap)
{
    RecMpHistory* mp = NULL;
    RecSeg* sg = (RecSeg*)calloc(RECR_TIMELINES, sizeof *sg);
    RecIndexEntry* ix = NULL;
    unsigned nix = 0, room = 0, ns = 0, i, j;
    unsigned long long off = 8, size, metadata_off = 0;
    LARGE_INTEGER fs;
    int ok = 0, after_end = 1;
    RecTimelineState states[REC_TIMELINE_ROWS];
    unsigned nstates = 0, max_id = 0;
    char w2[160];
    if (!sg) {
        snprintf(why, cap, "no memory for the recording's segments");
        return -1;
    }
    if (!GetFileSizeEx(r->f, &fs)) {
        snprintf(why, cap, "cannot size the file (%lu)", GetLastError());
        goto done;
    }
    size = (unsigned long long)fs.QuadPart;
    while (off + sizeof(RecBlock) <= size) {       /* a header the file ends inside is torn */
        RecBlock b;
        RecSeg* s;
        unsigned char* p;
        size_t n = 0;
        int k;
        if (!seek_to(r->f, off) || !read_all(r->f, &b, sizeof b)) {
            snprintf(why, cap, "cannot read at byte %llu (%lu)", off, GetLastError());
            goto done;
        }
        if (b.magic != REC_BLOCK_MAGIC || (uint32_t)Crc32_ComputeBuf(0, &b, 28) != b.crc_head) {
            if (failed_block(r, off, off, size, tail, why, cap) < 0) goto done;
            break;
        }
        if (b.packed > RECR_BLOCK_MAX || b.raw > RECR_BLOCK_MAX ||
            ((b.flags & RBF_STORED) && b.packed != b.raw) || block_flags_bad(&b)) {
            /* a header that passed its CRC: what the writer wrote, not a tear */
            if (tail) {
                snprintf(why, cap, "a block of kind %u at byte %llu of %u bytes, %u unpacked: %s",
                         b.kind, off, b.packed, b.raw,
                         block_flags_bad(&b) ? block_flags_bad(&b) : "past what it may hold");
                goto done;
            }
            break;
        }
        if (off + sizeof b + b.packed > size) break;      /* torn: the file ends in its payload */
        if (b.kind == RB_TIMELINES) {
            k = body_get(r, off, &b, &p, &n, why, cap);
            if (k == BLK_UNREADABLE) goto done;
            if (k == BLK_TORN) {
                if (failed_block(r, off, off + sizeof b + b.packed, size, tail, why, cap) < 0)
                    goto done;
                break;
            }
            if (!ns || !recmeta_parse(states, &nstates, p, (unsigned)n)) {
                free(p);
                snprintf(why, cap, "invalid timeline metadata at byte %llu", off);
                goto done;
            }
            free(p);
            metadata_off = off;
            off += sizeof b + b.packed;
            continue;
        }
        if (after_end) {
            /* a segment begins: the file's first block, or a header right after an end */
            if (b.kind != RB_HEADER) {
                /* the writer begins every segment with its header: anything else is no tear */
                if (tail && ns) {
                    snprintf(why, cap, "a block of kind %u at byte %llu follows an end block",
                             b.kind, off);
                    goto done;
                }
                break;
            }
            if (ns == RECR_TIMELINES) {
                snprintf(why, cap, "it holds more than %u timelines", RECR_TIMELINES);
                goto done;
            }
            s = &sg[ns];
            if (!ns) {
                s->h = &r->h;               /* recr_open has read and checked the first */
            } else {
                k = body_get(r, off, &b, &p, &n, why, cap);
                if (k == BLK_UNREADABLE) goto done;
                if (k == BLK_TORN) {
                    if (failed_block(r, off, off + sizeof b + b.packed, size, tail, why, cap) < 0)
                        goto done;
                    break;
                }
                s->h = (RecHeader*)malloc(sizeof *s->h);
                if (!s->h) {
                    free(p);
                    snprintf(why, cap, "no memory for a timeline's header");
                    goto done;
                }
                if (!header_parse(s->h, p, n, w2, sizeof w2)) {
                    free(p);
                    ns++;
                    snprintf(why, cap, "the segment at byte %llu: %s", off, w2);
                    goto done;
                }
                free(p);
                if (!s->h->has_timeline) {
                    ns++;
                    snprintf(why, cap, "the segment at byte %llu names no timeline", off);
                    goto done;
                }
                /* the world and the unit frame are allocated by the first header's slots */
                if (s->h->slots != r->h.slots) {
                    ns++;
                    snprintf(why, cap, "the segment at byte %llu has %u unit slots, not the "
                             "recording's %u", off, s->h->slots, r->h.slots);
                    goto done;
                }
                s->t.id = s->h->tl_id;
                s->t.parent = s->h->tl_parent;
                s->t.fork_at = s->h->tl_fork_at;
            }
            ns++;
            s->t.difficulty = s->h->difficulty;
            for (i = 0; i < s->h->nseats; i++)
                if (s->h->seats[i].controller == 2) s->t.has_ai = 1;
            s->t.header_off = off;
            s->t.first_off = s->t.end_off = off + sizeof b + b.packed;
            s->i0 = nix;
            tagpu_recmp_history_end(mp); mp=NULL;
            if (s->h->has_mp && !(mp=tagpu_recmp_history_begin(&s->h->mp,&s->h->mp_seats,s->h->grid_w,s->h->grid_h))) {
                snprintf(why, cap, "invalid contribution roster or no memory for history"); goto done;
            }
            after_end = 0;
            off = s->t.first_off;
            continue;
        }
        s = &sg[ns - 1];
        if (s->h->mode == 3 && !s->h->has_mp) {
            snprintf(why, cap, "network recording has no contribution roster"); goto done;
        }
        if (b.kind == RB_CONTRIBUTIONS) {
            k = body_get(r, off, &b, &p, &n, why, cap);
            if (k == BLK_UNREADABLE) goto done;
            if (k == BLK_TORN) {
                if (failed_block(r, off, off + sizeof b + b.packed, size, tail, why, cap) < 0)
                    goto done;
                break;
            }
            if (!s->h->has_mp || !tagpu_recmp_history_block(mp, p, (unsigned)n)) {
                free(p); snprintf(why, cap, "invalid contribution block at byte %llu", off); goto done;
            }
            free(p);
        }
        if (b.kind == RB_HEADER) {
            snprintf(why, cap, "a header block at byte %llu inside timeline %u", off, s->t.id);
            goto done;
        }
        if (b.kind == RB_END) {
            k = body_get(r, off, &b, &p, &n, why, cap);
            if (k == BLK_UNREADABLE) goto done;
            if (k == BLK_TORN) {                        /* the segment has no end */
                if (failed_block(r, off, off + sizeof b + b.packed, size, tail, why, cap) < 0)
                    goto done;
                break;
            }
            s->t.end_reason = entry_u32(p, n, RT_END_REASON);
            {
                unsigned at = 0, found = 0;
                while (at + 4 <= n) {
                    unsigned short tag, len;
                    memcpy(&tag, p + at, 2); memcpy(&len, p + at + 2, 2);
                    if (len > n - at - 4) break;
                    if (tag == RT_MP_END) {
                        RecMpRecordEnd end;
                        if (!s->h->has_mp || found || len != sizeof end) break;
                        memcpy(&end, p + at + 4, sizeof end);
                        if (!tagpu_recmp_history_footer(mp, &end)) break;
                        found = 1;
                    }
                    at += (4u + len + 3u) & ~3u;
                }
                if (found != !!s->h->has_mp || at != n) {
                    free(p); snprintf(why, cap, "missing or invalid contribution end proof"); goto done;
                }
            }
            s->ended = 1;
            free(p);
        }
        if (nix == room) {
            unsigned c = room ? room * 2 : 256;
            RecIndexEntry* q = (RecIndexEntry*)realloc(ix, c * sizeof *q);
            if (!q) {
                snprintf(why, cap, "no memory for the index of the recording's blocks");
                goto done;
            }
            ix = q;
            room = c;
        }
        ix[nix].kind = b.kind;
        ix[nix].flags = b.flags;
        ix[nix].first = b.first;
        ix[nix].last = b.last;
        ix[nix].raw = b.raw;
        ix[nix].packed = b.packed;
        ix[nix].seg = ns - 1;
        ix[nix].off = off;
        nix++;
        s->n++;
        if (b.kind == RB_STREAM) {
            if (!s->nstream++) s->t.first = b.first;
            s->t.last = b.last;
        }
        off += sizeof b + b.packed;
        s->t.end_off = off;
        if (b.kind == RB_END) after_end = 1;
    }
    if (tail && nix && ix[nix - 1].kind != RB_END) {
        /* the last block's payload, which the walk did not read: torn, it is not kept, and what
           follows it is nothing or a tear the walk stopped at */
        RecBlock b;
        const RecIndexEntry* e = &ix[nix - 1];
        int k;
        if (!seek_to(r->f, e->off) || !read_all(r->f, &b, sizeof b) ||
            (k = crc_stream(r, e->off, &b)) == BLK_UNREADABLE) {
            snprintf(why, cap, "cannot read the block at byte %llu (%lu)", e->off,
                     GetLastError());
            goto done;
        }
        if (k == BLK_TORN) {
            /* Metadata is not indexed. A valid table after this payload is data following
               damage, not an interrupted append; never cut through it. */
            if (failed_block(r, e->off, e->off + sizeof b + b.packed, size, tail, why, cap) < 0)
                goto done;
            off = e->off;
            sg[e->seg].n--;
            nix--;
            seg_tally(&sg[e->seg], ix);
        }
    }
    if (!ns) {
        snprintf(why, cap, "it has no header block");
        goto done;
    }
    /* THE BOUNDS: an id names one segment, and a parent an earlier one with a lower id, so a
       chain followed by parents ends at the original within RECR_TIMELINES steps */
    for (i = 1; i < ns; i++) {
        int found = 0;
        for (j = 0; j < i; j++) {
            if (sg[j].t.id == sg[i].t.id) {
                snprintf(why, cap, "timeline %u twice in the file", sg[i].t.id);
                goto done;
            }
            if (sg[j].t.id == sg[i].t.parent) found = 1;
        }
        if (!found || sg[i].t.parent >= sg[i].t.id) {
            snprintf(why, cap, "timeline %u names parent %u, which is not an earlier timeline",
                     sg[i].t.id, sg[i].t.parent);
            goto done;
        }
    }
    for (i = 0; i < ns; i++) {
        RecTimeline* t = &sg[i].t;
        if (t->id == UINT32_MAX) {
            snprintf(why, cap, "timeline ID exhausted");
            goto done;
        }
        t->state = REC_TL_LIVE;
        t->last_used = sg[i].h->wallclock;
        if (t->id > max_id) max_id = t->id;
        int named = 0;
        for (j = 0; j < nstates; j++) if (states[j].id == t->id) {
            named = 1;
            t->state = states[j].state;
            t->last_used = ((uint64_t)states[j].used_high << 32) | states[j].used_low;
        }
        if (metadata_off && t->header_off < metadata_off && !named) {
            snprintf(why, cap, "timeline %u missing from its metadata table", t->id);
            goto done;
        }
    }
    for (i = 0; i < nstates; i++) {
        int found = 0;
        if (states[i].id > max_id) max_id = states[i].id;
        for (j = 0; j < ns; j++) if (states[i].id == sg[j].t.id) found = 1;
        if (!found && states[i].state == REC_TL_LIVE) {
            snprintf(why, cap, "live timeline %u has no segment", states[i].id);
            goto done;
        }
    }
    ok = 1;
done:
    if (ok) {
        unsigned old = r->nseg;
        RecSeg keep[RECR_TIMELINES];
        memcpy(keep, r->seg, sizeof keep);
        memcpy(r->seg, sg, ns * sizeof *sg);
        r->nseg = ns;
        for (i = 0; ok && i < r->nchain; i++) {
            r->chain[i] = seg_index(r, r->chain_id[i]);
            if (r->chain[i] == RECR_TIMELINES) {
                snprintf(why, cap, "timeline %u is no longer in the file", r->chain_id[i]);
                ok = 0;
            }
        }
        if (ok) {
            segs_free(keep, old);
            free(r->idx);
            r->idx = ix;
            r->nidx = nix;
            r->scan_epoch++;
            r->file_end = off;
            r->max_id = max_id;
            r->nstates = nstates;
            memcpy(r->states, states, nstates * sizeof *states);
        } else {
            memcpy(r->seg, keep, sizeof keep);
            r->nseg = old;
            for (i = 0; i < r->nchain; i++) r->chain[i] = seg_index(r, r->chain_id[i]);
            segs_free(sg, ns);
            free(ix);
        }
    } else {
        segs_free(sg, ns);
        free(ix);
    }
    free(sg);
    tagpu_recmp_history_end(mp);
    if (!seek_to(r->f, r->off)) {
        snprintf(why, cap, "cannot return to byte %llu (%lu)", r->off, GetLastError());
        return -1;
    }
    return ok ? 1 : -1;
}

int recr_timelines(RecReader* r, RecTimeline* out, unsigned cap, char* why, unsigned wcap)
{
    unsigned i;
    if (segs_scan(r, 0, why, wcap) < 0) return -1;
    for (i = 0; i < r->nseg && i < cap; i++) out[i] = r->seg[i].t;
    return (int)r->nseg;
}

const RecTimeline* recr_selected(const RecReader* r)
{
    return &chain_seg(r, r->nchain - 1)->t;
}

/* the first field in which a segment's header differs from the first header, or NULL */
static const char* shape_differs(const RecHeader* a, const RecHeader* b)
{
    unsigned i;
    if (a->slots != b->slots) return "unit slots";
    if (a->units_each != b->units_each) return "units a player";
    if (a->has_mp && b->has_mp && (memcmp(&a->mp,&b->mp,sizeof a->mp) ||
        memcmp(&a->mp_seats,&b->mp_seats,sizeof a->mp_seats))) return "multiplayer source/seat mapping";
    /* a skirmish chain's segments all hold their cells or none do: the playing keeps the engine's
       slots, or the recording's, from its first step (tagpu_replay.c, THE UNIT SLOTS AND THE YARD
       BIT), and a skirmish chain has no crossing that changes it */
    if (!a->has_mp && !b->has_mp && recr_holds_cells(a) != recr_holds_cells(b)) return "held cells";
    /* the codec is made for the first segment's RT_HOLD, which picks its orders frame's scheme */
    if (a->has_hold != b->has_hold) return "RT_HOLD";
    if (a->has_unitdefs != b->has_unitdefs || a->ndefs != b->ndefs || a->defs_hash != b->defs_hash)
        return "unit types";
    if (a->has_weapons != b->has_weapons || a->nweapons != b->nweapons ||
        a->weapons_hash != b->weapons_hash)
        return "weapons";
    if (a->grid_w != b->grid_w || a->grid_h != b->grid_h) return "feature grid";
    if (a->wrecks != b->wrecks) return "wreck pool";
    if (a->nfdefs != b->nfdefs || a->fdefs_hash != b->fdefs_hash) return "feature defs";
    if (a->proj_cap != b->proj_cap) return "projectile pool";
    if (strcmp(a->layout, b->layout)) return "exclusion file (replay-layout.tsv)";
    if (strcmp(a->globals, b->globals)) return "positive list (replay-globals.tsv)";
    if (strcmp(a->settings_map, b->settings_map)) return "map";
    if (a->nseats != b->nseats) return "seats";
    for (i = 0; i < a->nseats; i++)
        if (a->seats[i].index != b->seats[i].index) return "seats";
    if (a->ntab != b->ntab) return "declared tables";
    for (i = 0; i < a->ntab; i++) {
        const RecTableInfo* x = &a->tab[i];
        const RecTableInfo* y = &b->tab[i];
        if (x->index != y->index || x->stride != y->stride || x->cls != y->cls ||
            x->key != y->key || x->flags != y->flags || x->lhash != y->lhash ||
            strcmp(x->name, y->name))
            return "declared tables";
    }
    return NULL;
}

int recr_timeline(RecReader* r, unsigned id, char* why, unsigned cap)
{
    unsigned path[RECR_TIMELINES], n = 0, i, s;
    if (r->read_any) {
        snprintf(why, cap, "a timeline is chosen before the first iteration");
        return -1;
    }
    if (segs_scan(r, 0, why, cap) < 0) return -1;
    s = seg_index(r, id);
    if (s == RECR_TIMELINES) {
        snprintf(why, cap, "the recording has no timeline %u", id);
        return 0;
    }
    if (r->seg[s].t.state == REC_TL_DELETED) {
        snprintf(why, cap, "timeline %u is deleted", id);
        return 0;
    }
    /* by parents to the original: each parent is an earlier segment (segs_scan's bounds) */
    for (;;) {
        path[n++] = s;
        if (!s) break;
        s = seg_index(r, r->seg[s].t.parent);
    }
    for (i = 0; i + 1 < n; i++) {
        const RecSeg* p = &r->seg[path[n - 1 - i]];
        const RecSeg* c = &r->seg[path[n - 2 - i]];
        const char* field;
        if (!p->nstream || !c->nstream || p->t.first >= c->t.fork_at ||
            c->t.fork_at - 1 > p->t.last || c->t.first != c->t.fork_at) {
            snprintf(why, cap, "timeline %u, from GameTime %u, forks from timeline %u at GameTime "
                     "%u, outside its GameTime %u to %u", c->t.id, c->t.first, p->t.id,
                     c->t.fork_at, p->t.first, p->t.last);
            return 0;
        }
        field = shape_differs(c->h, &r->h);
        if (field) {
            snprintf(why, cap, "timeline %u differs from the recording in its %s", c->t.id, field);
            return 0;
        }
    }
    for (i = 0; i < n; i++) {
        r->chain[i] = path[n - 1 - i];
        r->chain_id[i] = r->seg[r->chain[i]].t.id;
    }
    r->nchain = n;
    r->cur = 0;
    r->base_seg = -1;
    world_reset(r, 0);
    if (!file_at(r, r->first_off)) {
        snprintf(why, cap, "cannot move to byte %llu (%lu)", r->first_off, GetLastError());
        return -1;
    }
    return 1;
}

int recr_append_point(RecReader* r, unsigned at, RecAppend* a, char* why, unsigned cap)
{
    const RecSeg* last;
    unsigned i, keep, hold;
    int cut;
    memset(a, 0, sizeof *a);
    if (segs_scan(r, 1, why, cap) < 0) return 0;
    last = &r->seg[r->nseg - 1];
    /* an unfinished fork with no complete iteration: its id never named a complete segment */
    cut = r->nseg > 1 && !last->ended && !last->nstream;
    keep = r->nseg - (cut ? 1u : 0u);
    for (hold = 0; hold < r->nchain; hold++) {
        const RecSeg* s = chain_seg(r, hold);
        if (r->chain[hold] < keep && s->nstream && at >= s->t.first && at <= cover_end(r, hold))
            break;
    }
    if (hold == r->nchain) {
        snprintf(why, cap, "GameTime %u is outside the timeline played", at);
        return 0;
    }
    /* At a chain segment's first iteration the world played is that segment's, but `at` (the
       iteration before) is its parent's: the fork would name the parent and replace the parent's
       iteration there, which is not the world it was made from. */
    if (hold + 1 < r->nchain && at + 1 == chain_seg(r, hold + 1)->t.fork_at) {
        snprintf(why, cap, "GameTime %u is timeline %u's first iteration; take over after it",
                 at + 1, chain_seg(r, hold + 1)->t.id);
        return 0;
    }
    if (keep >= RECR_TIMELINES) {
        snprintf(why, cap, "the recording holds %u timelines, the most a file may", keep);
        return 0;
    }
    a->off = cut ? last->t.header_off : r->file_end;
    if (r->max_id >= UINT32_MAX - 1) {
        snprintf(why, cap, "timeline ID exhausted");
        return 0;
    }
    a->next_id = r->max_id + 1;
    a->evict = UINT32_MAX;
    {
        unsigned live = 0;
        uint64_t oldest = UINT64_MAX, now = recfile_now();
        unsigned selected = recr_selected(r)->id;
        for (i = 0; i < keep; i++) {
            const RecTimeline* t = &r->seg[i].t;
            RecTimelineState* row = &a->states[a->nstates++];
            *row = (RecTimelineState){t->id, (uint8_t)t->state, {0},
                                     (uint32_t)t->last_used, (uint32_t)(t->last_used >> 32)};
            if (t->id && t->state == REC_TL_LIVE) {
                live++;
                if (t->id != selected && (a->evict == UINT32_MAX || t->last_used < oldest)) {
                    oldest = t->last_used;
                    a->evict = t->id;
                }
            }
        }
        if (live < REC_FORK_LIMIT) a->evict = UINT32_MAX;
        else if (a->evict == UINT32_MAX || live > REC_FORK_LIMIT) {
            snprintf(why, cap, "the live fork set cannot be bounded to %u", REC_FORK_LIMIT);
            return 0;
        }
        for (i = 0; i < a->nstates; i++)
            if (a->states[i].id == a->evict) a->states[i].state = REC_TL_DELETED;
        /* The new ID exceeds the previous high watermark, so its row carries that watermark
           even when an unfinished tail is cut. No absent tombstone is needed in this table. */
        a->states[a->nstates++] = (RecTimelineState){a->next_id, REC_TL_LIVE, {0},
                                                   (uint32_t)now, (uint32_t)(now >> 32)};
    }
    a->parent = chain_seg(r, hold)->t.id;
    a->end_open = !cut && !last->ended;
    if (a->end_open) {
        a->open_id = last->t.id;
        a->open_last = last->t.last;
    }
    return 1;
}

unsigned recr_refresh_at(RecReader* r, unsigned T, RecTimeline* seg, char* why, unsigned cap)
{
    const RecSeg* s;
    RecLossyBlock* b;
    unsigned hold, j, n = 0, R;
    if (segs_scan(r, 0, why, cap) < 0) return 0;
    for (hold = 0; hold < r->nchain; hold++) {
        s = chain_seg(r, hold);
        if (s->nstream && T >= s->t.first && T <= cover_end(r, hold)) break;
    }
    if (hold == r->nchain) {
        snprintf(why, cap, "GameTime %u is outside the timeline played", T);
        return 0;
    }
    s = chain_seg(r, hold);
    if (seg) *seg = s->t;
    b = (RecLossyBlock*)malloc(s->nstream * sizeof *b);
    if (!b) {
        snprintf(why, cap, "no memory for the segment's %u stream blocks", s->nstream);
        return 0;
    }
    for (j = s->i0; j < s->i0 + s->n; j++)
        if (r->idx[j].kind == RB_STREAM && n < s->nstream) {
            b[n].first = r->idx[j].first;
            b[n].last = r->idx[j].last;
            b[n].refresh = (r->idx[j].flags & RBF_REFRESH) != 0;
            n++;
        }
    R = reclossy_refresh_at(b, n, s->t.first, T);
    free(b);
    return R;
}

int recr_same_file(const RecReader* r, void* f)
{
    BY_HANDLE_FILE_INFORMATION a, b;
    return GetFileInformationByHandle(r->f, &a) && GetFileInformationByHandle(f, &b) &&
           a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

static void mp_read_free(RecMpRead* m)
{
    tagpu_recmp_history_end(m->history);
    free(m->payload);
    memset(m,0,sizeof *m);
}

static int mp_read(RecReader* r, RecMpRead* m, uint64_t* ordinal,
                    RecMpApplyVisit visit, void* ctx, char* why, unsigned cap)
{
    const RecSeg* s;
    unsigned segment;
    int ok=1;
    if (!r->in_seg) return 0;
    segment=r->chain[r->cur]; s=&r->seg[segment];
    if (!s->h->has_mp) return 0;
    if (!m->history || m->epoch!=r->scan_epoch || m->segment!=segment ||
        r->w.tick<m->tick || (ordinal &&
        *ordinal!=tagpu_recmp_history_ledger(m->history)->records)) {
        mp_read_free(m);
        m->history=tagpu_recmp_history_begin(&s->h->mp,&s->h->mp_seats,s->h->grid_w,s->h->grid_h);
        if (!m->history) { snprintf(why,cap,"no memory for multiplayer authority"); return -1; }
        m->epoch=r->scan_epoch; m->segment=segment; m->tick=0;
        m->index=s->i0; m->done=0;
    }
    while (!m->done) {
        RecMpRecord where;
        RecMpHeader packet;
        const unsigned char* body;
        if (m->at==m->bytes) {
            const RecIndexEntry* ix;
            RecBlock b;
            free(m->payload); m->payload=NULL; m->bytes=m->at=0;
            while (m->index<s->i0+s->n && r->idx[m->index].kind!=RB_CONTRIBUTIONS)
                m->index++;
            if (m->index==s->i0+s->n) break;
            ix=&r->idx[m->index++];
            if (!seek_to(r->f,ix->off) || !read_all(r->f,&b,sizeof b) ||
                b.magic!=REC_BLOCK_MAGIC || (uint32_t)Crc32_ComputeBuf(0,&b,28)!=b.crc_head ||
                b.kind!=RB_CONTRIBUTIONS || b.raw!=ix->raw || b.packed!=ix->packed ||
                b.raw>RECR_BLOCK_MAX || b.packed>RECR_BLOCK_MAX ||
                ((b.flags&RBF_STORED) && b.raw!=b.packed) ||
                body_get(r,ix->off,&b,&m->payload,&m->bytes,why,cap)!=BLK_WHOLE) {
                ok=-1; break;
            }
        }
        if (m->bytes-m->at<sizeof where+sizeof packet) { ok=-1; break; }
        memcpy(&where,m->payload+m->at,sizeof where);
        memcpy(&packet,m->payload+m->at+sizeof where,sizeof packet);
        if (packet.bytes<sizeof packet || packet.bytes>RECMP_PACKET_MAX ||
            packet.bytes>m->bytes-m->at-sizeof where) { ok=-1; break; }
        if (where.phase==RECMP_RECORD_PROOF) { m->done=1; break; }
        if (where.tick>r->w.tick) break;
        body=m->payload+m->at+sizeof where+sizeof packet;
        if (!tagpu_recmp_history_record(m->history,&where,&packet,body,packet.bytes-sizeof packet)) {
            ok=-1; break;
        }
        m->at+=sizeof where+packet.bytes;
        if (ordinal && where.ordinal>*ordinal) {
            visit(ctx,&where,&packet,body,packet.bytes-sizeof packet);
            *ordinal=where.ordinal;
        }
    }
    /* Either authority cursor preserves the world's next file read. */
    if (!seek_to(r->f,r->off)) ok=-1;
    if (ok<0) {
        mp_read_free(m);
        snprintf(why,cap,"invalid multiplayer authority at GameTime %u",r->w.tick);
    } else m->tick=r->w.tick;
    return ok;
}

static int mp_sync(RecReader* r, char* why, unsigned cap)
{
    return mp_read(r,&r->mp_query,NULL,NULL,NULL,why,cap);
}

int recr_mp_apply(RecReader* r, uint64_t* ordinal, RecMpApplyVisit visit, void* ctx,
                    char* why, unsigned cap)
{
    int k;
    if (!ordinal || !visit) {
        snprintf(why,cap,"multiplayer application cursor and visitor are required");
        return -1;
    }
    /* Validate the complete available prefix before delivering anything. Queries
       and application use separate histories so neither can skip the other's
       envelopes, even when a query already advanced to this world tick. */
    k=mp_sync(r,why,cap);
    if (k<=0) return k;
    if (*ordinal>tagpu_recmp_history_ledger(r->mp_query.history)->records) {
        snprintf(why,cap,"multiplayer application ordinal is beyond GameTime %u",r->w.tick);
        return -1;
    }
    return mp_read(r,&r->mp_apply,ordinal,visit,ctx,why,cap);
}

int recr_mp_player(RecReader* r, unsigned origin, RecMpPlayerState* out, char* why, unsigned cap)
{
    int k=mp_sync(r,why,cap);
    return k<=0 ? k : tagpu_recmp_history_player(r->mp_query.history,origin,out);
}

int recr_mp_unit(RecReader* r, unsigned slot, RecMpUnitState* out, char* why, unsigned cap)
{
    int k=mp_sync(r,why,cap);
    return k<=0 ? k : tagpu_recmp_history_unit(r->mp_query.history,slot,out);
}

int recr_mp_roots(RecReader* r, unsigned origin, RecMpRootsState* out, char* why, unsigned cap)
{
    int k=mp_sync(r,why,cap);
    return k<=0 ? k : tagpu_recmp_history_roots(r->mp_query.history,origin,out);
}

int recr_mp_named(RecReader* r, unsigned origin, RecMpNamedState* out, char* why, unsigned cap)
{
    int k=mp_sync(r,why,cap);
    return k<=0 ? k : tagpu_recmp_history_named(r->mp_query.history,origin,out);
}

int recr_mp_text(RecReader* r, unsigned origin, RecMpTextState* out, char* why, unsigned cap)
{
    int k=mp_sync(r,why,cap);
    return k<=0 ? k : tagpu_recmp_history_text(r->mp_query.history,origin,out);
}

int recr_mp_orders(RecReader* r, unsigned slot, unsigned list, RecMpOrderState* out,
                     void* image, unsigned capacity, char* why, unsigned cap)
{
    int k=mp_sync(r,why,cap);
    return k<=0 ? k : tagpu_recmp_history_orders(r->mp_query.history,slot,list,out,image,capacity);
}

/* ---- seeking -------------------------------------------------------------------------------- */

/* A chained snapshot's records into raw, `e` its entry in the index: the snapshots of its segment
   before it back to their key, unpacked forward, each the next one's prefix (tagpu_recfmt.h, THE
   RUN AND THE CHAIN). 1, or -1 (why).
   BOUNDS: each link is the RB_SNAPSHOT before the last in the same segment, and at most
   REC_CHAIN_MAX are followed -- a longer chain, or a chained snapshot with no snapshot before it
   in its segment, is refused -- so a seek unpacks at most REC_CHAIN_MAX + 1 snapshots and holds
   two at once. Each block's header is read again and must be the one indexed, and each body
   passes its CRC and the frame's checks.
   THE COUNT: every link is read by block_head and decoded by block_body, the one place a seek's
   decoded blocks are counted (recr_seek_work), so a chain of k links is k blocks of the seek's
   work, the key's included. */
static int snapshot_chain(RecReader* r, const RecIndexEntry* e, char* why, unsigned cap)
{
    const RecSeg* s = &r->seg[e->seg];
    unsigned link[REC_CHAIN_MAX + 1], n = 0, at = (unsigned)(e - r->idx), j;
    unsigned char* prev = NULL;
    size_t nprev = 0;
    link[n++] = at;
    while (r->idx[at].flags & RBF_CHAINED) {
        for (j = at; j > s->i0 && r->idx[j - 1].kind != RB_SNAPSHOT; j--) {}
        if (j == s->i0) {
            snprintf(why, cap, "the chained snapshot of GameTime %u has no snapshot before it in "
                     "its segment", r->idx[at].last);
            return -1;
        }
        if (n == REC_CHAIN_MAX + 1) {
            snprintf(why, cap, "the snapshot of GameTime %u chains past %u snapshots to its key",
                     e->last, REC_CHAIN_MAX);
            return -1;
        }
        at = j - 1;
        link[n++] = at;
    }
    while (n--) {
        const RecIndexEntry* x = &r->idx[link[n]];
        RecBlock b;
        int k;
        if (!file_at(r, x->off)) {
            free(prev);
            snprintf(why, cap, "cannot move to byte %llu (%lu)", x->off, GetLastError());
            return -1;
        }
        k = block_head(r, &b, why, cap);
        if (k <= 0 || b.kind != x->kind || b.flags != x->flags || b.packed != x->packed ||
            b.raw != x->raw) {
            free(prev);
            if (k >= 0) snprintf(why, cap, "the snapshot at byte %llu is not the one indexed",
                                 x->off);
            return -1;
        }
        r->prefix = prev;
        r->nprefix = nprev;
        k = block_body(r, &b, why, cap);
        r->prefix = NULL;
        r->nprefix = 0;
        free(prev);
        if (k < 0) return -1;
        prev = payload_own(r, &nprev);
        if (!prev) {
            snprintf(why, cap, "no memory for the snapshot at byte %llu", x->off);
            return -1;
        }
    }
    r->raw = prev;
    r->rawn = nprev;
    r->at = 0;
    return 1;
}

/* The snapshot block whose header block_head has just read, applied over the reset world: a TICK
   at the block's GameTime, then images only (tagpu_recfmt.h, RB_SNAPSHOT). `e` is its entry in the
   index, which a chained snapshot needs and a dump's has none of. 1, or -1 (why). */
static int snapshot_records(RecReader* r, const RecBlock* blk, const RecIndexEntry* e, char* why,
                            unsigned cap)
{
    RecWorld* w = &r->w;
    int events = 0;
    if (!(blk->flags & RBF_CHAINED) || !e) {
        if (block_body(r, blk, why, cap) < 0) return -1;
    } else {
        unsigned long long after = e->off + sizeof *blk + blk->packed;
        if (snapshot_chain(r, e, why, cap) < 0) return -1;
        if (!file_at(r, after)) {
            snprintf(why, cap, "cannot move to byte %llu (%lu)", after, GetLastError());
            return -1;
        }
    }
    while (r->at < r->rawn) {
        RecHdr hd;
        const unsigned char* b;
        if (r->at + 4 > r->rawn) {
            snprintf(why, cap, "a record header cut short in the snapshot of GameTime %u",
                     blk->last);
            return -1;
        }
        memcpy(&hd, r->raw + r->at, 4);
        if (r->at + 4 + hd.len > r->rawn) {
            snprintf(why, cap, "a record cut short in the snapshot of GameTime %u", blk->last);
            return -1;
        }
        b = r->raw + r->at + 4;
        if (!r->at) {
            if (hd.kind != RK_TICK || hd.len != 4 || u32(b) != blk->last) {
                snprintf(why, cap, "the snapshot of GameTime %u does not begin with its tick",
                         blk->last);
                return -1;
            }
            w->tick = blk->last;
        } else if (r->h.has_frame &&
                   (hd.kind == RK_IMAGE || hd.kind == RK_SIGHT || hd.kind == RK_COUNTS ||
                    hd.kind == RK_COUNT_NAME)) {
            /* Comparison evidence in a dump, not restored simulation state. The record's
               length has been bounded by the checked block above. */
        } else if (hd.kind == RK_TICK || !apply(r, hd.kind, b, hd.len, &events) || events) {
            snprintf(why, cap, "a malformed record of kind %u in the snapshot of GameTime %u",
                     hd.kind, blk->last);
            return -1;
        }
        r->at += (4 + (size_t)hd.len + 3) & ~(size_t)3;
    }
    if (!r->rawn) {
        snprintf(why, cap, "the snapshot of GameTime %u is empty", blk->last);
        return -1;
    }
    if (iteration_done(r, why, cap) < 0) return -1;
    /* its records point into the payload: the stream's next iteration replaces them */
    w->nfeat = 0;
    payload_free(r);
    /* the world was rebuilt: the stream block after a snapshot restarts the frame */
    if (r->codec) reccodec_invalidate(r->codec);
    return 1;
}

static int snapshot_apply(RecReader* r, const RecBlock* blk, const RecIndexEntry* e, char* why,
                          unsigned cap)
{
    int k;
    r->snap = 1;
    k = snapshot_records(r, blk, e, why, cap);
    r->snap = 0;
    return k;
}

int recr_seek(RecReader* r, unsigned T, unsigned lead, RecVisit visit, void* ctx, char* why,
              unsigned cap)
{
    RecWorld* w = &r->w;
    const RecIndexEntry* snap = NULL;
    const RecSeekWork d0 = r->decoded;
    unsigned i, j, hold, from = 0, prev;
    int read, k;
    memset(&r->work, 0, sizeof r->work);
    r->work.target = T;
    if (segs_scan(r, 0, why, cap) < 0) return -1;      /* a fork may have been appended since */
    if (!chain_seg(r, 0)->nstream) {
        snprintf(why, cap, "the recording holds no complete stream block");
        return 0;
    }
    for (hold = 0; hold < r->nchain; hold++) {
        const RecSeg* s = chain_seg(r, hold);
        if (s->nstream && T >= s->t.first && T <= cover_end(r, hold)) break;
    }
    if (hold == r->nchain) {
        snprintf(why, cap, "GameTime %u is outside the recording, which holds GameTime %u to %u",
                 T, chain_seg(r, 0)->t.first, cover_end(r, r->nchain - 1));
        return 0;
    }
    /* The last snapshot of the chain more than `lead` iterations before T (strictly before it with
       no lead), inside its segment's part of the chain: a snapshot at S holds S's images, and the
       events of the step after them are in the stream block before it (tagpu_recfmt.h,
       RB_SNAPSHOT). */
    for (i = 0; i <= hold; i++) {
        const RecSeg* s = chain_seg(r, i);
        for (j = s->i0; j < s->i0 + s->n; j++)
            if (r->idx[j].kind == RB_SNAPSHOT && r->idx[j].last <= cover_end(r, i) &&
                (unsigned long long)r->idx[j].last + lead < T) {
                snap = &r->idx[j];
                from = i;
            }
    }
    if (snap) {
        RecBlock blk;
        const RecSeg* s = chain_seg(r, from);
        const RecIndexEntry* next = snap + 1;
        /* the stream goes on from S + 1 within the segment, unless the chain leaves it at S; the
           writer starts it there (tagpu_recwriter.c, iteration_done). A multiplayer recording's
           contributions blocks may stand between the two, and tick_read steps over them as it
           steps over every block that is not the stream's */
        while (next < r->idx + s->i0 + s->n && next->kind == RB_CONTRIBUTIONS) next++;
        if (snap->last < cover_end(r, from) &&
            (next >= r->idx + s->i0 + s->n || next->kind != RB_STREAM ||
             next->first != snap->last + 1)) {
            snprintf(why, cap, "the snapshot of GameTime %u is not followed by the stream from "
                     "GameTime %u", snap->last, snap->last + 1);
            return -1;
        }
        /* the snapshot's base is its own segment's first iteration: read it first */
        if (r->base_seg != (int)from) {
            r->cur = from;
            world_reset(r, 0);
            if (!file_at(r, s->t.first_off)) {
                snprintf(why, cap, "cannot move to byte %llu (%lu)", s->t.first_off,
                         GetLastError());
                return -1;
            }
            k = tick_take(r, why, cap);
            r->work.iterations++;
            r->work.base = 1;
            if (k <= 0) {
                if (!k) snprintf(why, cap, "timeline %u's first iteration cannot be read",
                                 s->t.id);
                return -1;
            }
        }
        r->work.from = snap->last;
        r->work.from_snapshot = 1;
        r->cur = from;
        world_reset(r, 1);
        if (!file_at(r, snap->off)) {
            snprintf(why, cap, "cannot move to byte %llu (%lu)", snap->off, GetLastError());
            return -1;
        }
        k = block_head(r, &blk, why, cap);
        if (k <= 0 || blk.kind != RB_SNAPSHOT || blk.last != snap->last) {
            if (k >= 0) snprintf(why, cap, "the snapshot at byte %llu is not the one indexed",
                                 snap->off);
            return -1;
        }
        if (snapshot_apply(r, &blk, snap, why, cap) < 0) return -1;
        r->in_seg = 1;
    } else {
        /* the first segment of the chain whose part reaches T - lead, from its first iteration */
        unsigned lo = T > lead ? T - lead : 0;
        for (from = 0; from < hold && cover_end(r, from) < lo; from++) {}
        r->work.from = chain_seg(r, from)->t.first;
        r->cur = from;
        world_reset(r, 0);
        if (!file_at(r, chain_seg(r, from)->t.first_off)) {
            snprintf(why, cap, "cannot move to byte %llu (%lu)", chain_seg(r, from)->t.first_off,
                     GetLastError());
            return -1;
        }
    }
    /* each iteration's events replace the last's, so T's are the ones left pending */
    for (read = snap != NULL, prev = w->tick;; read = 1, prev = w->tick) {
        if (r->in_seg && r->cur < hold && w->tick >= cover_end(r, r->cur)) {
            /* THE CROSSING: the world holds the segment's last iteration before its child's
               fork_at, and the child's first iteration replaces it whole, over a reset world
               (the invariant at the head of this file); it is the child's base too */
            r->cur++;
            world_reset(r, 0);
            if (!file_at(r, chain_seg(r, r->cur)->t.first_off)) {
                snprintf(why, cap, "cannot move to byte %llu (%lu)",
                         chain_seg(r, r->cur)->t.first_off, GetLastError());
                return -1;
            }
            read = 0;
        }
        k = tick_take(r, why, cap);
        r->work.iterations++;
        if (k <= 0) {
            if (!k) snprintf(why, cap, "the recording ends at GameTime %u, before GameTime %u",
                             w->tick, T);
            return -1;
        }
        if (read && w->tick <= prev) {
            snprintf(why, cap, "the stream goes from GameTime %u back to %u", prev, w->tick);
            return -1;
        }
        if (r->cur < hold && w->tick > cover_end(r, r->cur)) {
            snprintf(why, cap, "timeline %u has no iteration at GameTime %u, where timeline %u "
                     "forks from it", chain_seg(r, r->cur)->t.id, cover_end(r, r->cur),
                     chain_seg(r, r->cur + 1)->t.id);
            return -1;
        }
        if (visit && w->tick <= T) visit(ctx, w);
        if (w->tick == T) {
            r->work.blocks = r->decoded.blocks - d0.blocks;
            r->work.packed = r->decoded.packed - d0.packed;
            r->work.raw = r->decoded.raw - d0.raw;
            return 1;
        }
        if (w->tick > T) {
            snprintf(why, cap, "the stream has no iteration at GameTime %u: it goes from %u to %u",
                     T, prev, w->tick);
            return -1;
        }
    }
}

const RecSeekWork* recr_seek_work(const RecReader* r)
{
    return &r->work;
}

/* ---- open and close ------------------------------------------------------------------------- */

static RecReader* open_handle(HANDLE f, const char* path, char* why, unsigned cap, int dump)
{
    RecReader* r = (RecReader*)calloc(1, sizeof *r);
    char magic[8];
    RecBlock b;
    int k;
    if (!r) {
        snprintf(why, cap, "no memory");
        CloseHandle(f);
        return NULL;
    }
    /* Shared for writing too: a takeover opens the file it plays for writing while this reader
       holds it (tagpu_replay.c, TK0). A block is whole or not by its two CRCs and every walk here
       stops at the first torn one, so a reader of a file being appended reads its complete blocks
       only. One writer at a time is the writers' own share mode's doing (FILE_SHARE_READ only),
       not this reader's. */
    r->f = f;
    if (!read_all(r->f, magic, 8) || memcmp(magic, REC_MAGIC, 8)) {
        snprintf(why, cap, "%s is not a recording", path);
        goto fail;
    }
    r->off = 8;
    k = block_read(r, &b, why, cap);
    if (k <= 0 || b.kind != RB_HEADER) {
        if (k >= 0) snprintf(why, cap, "%s has no header block", path);
        goto fail;
    }
    if (!header_parse(&r->h, r->raw, r->rawn, why, cap) || !world_init(r, why, cap)) goto fail;
    payload_free(r);
    r->first_off = r->off;
    if (dump) {
        if (!r->h.has_frame || r->h.frame == UINT32_MAX || r->h.start != r->h.frame + 1 ||
            block_head(r, &b, why, cap) != 1 || b.kind != RB_SNAPSHOT ||
            b.first != r->h.start || b.last != r->h.start) {
            snprintf(why, cap, "not a complete frame dump");
            goto fail;
        }
        if (snapshot_apply(r, &b, NULL, why, cap) != 1) goto fail;
        return r;
    }
    if (r->h.has_frame) { snprintf(why, cap, "a frame dump is not a recording"); goto fail; }
    r->nchain = 1;                  /* the original, until recr_timeline chooses another */
    r->base_seg = -1;
    if (segs_scan(r, 0, why, cap) < 0) goto fail;
    return r;
fail:
    recr_close(r);
    return NULL;
}

RecReader* recr_open(const char* path, char* why, unsigned cap)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, cap, "cannot open %s (%lu)", path, GetLastError());
        return NULL;
    }
    return open_handle(f, path, why, cap, 0);
}

int recr_format_of(const char* path)
{
    RecReader* r = (RecReader*)calloc(1, sizeof *r);
    char magic[8], why[160];
    RecBlock b;
    int k = -1;
    if (!r) return -1;
    r->f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (r->f != INVALID_HANDLE_VALUE && read_all(r->f, magic, 8) && !memcmp(magic, REC_MAGIC, 8)) {
        r->off = 8;
        if (block_read(r, &b, why, sizeof why) > 0 && b.kind == RB_HEADER) {
            /* the format entry is read before any check that can refuse the header */
            if (header_parse(&r->h, r->raw, r->rawn, why, sizeof why)) k = 1;
            else if (r->h.format && !rec_format_supported(r->h.format)) k = 0;
        }
    }
    recr_close(r);
    return k;
}

int recr_root_key(void* f, unsigned key[2])
{
    unsigned char head[8 + sizeof(RecBlock)];
    RecBlock b;
    DWORD got = 0;
    key[0] = key[1] = 0;
    if (!seek_to(f, 0) || !ReadFile(f, head, sizeof head, &got, NULL)) return -1;
    if (got != sizeof head || memcmp(head, REC_MAGIC, 8)) return 0;
    memcpy(&b, head + 8, sizeof b);
    if (b.magic != REC_BLOCK_MAGIC || (uint32_t)Crc32_ComputeBuf(0, &b, 28) != b.crc_head ||
        b.kind != RB_HEADER)
        return 0;
    key[0] = b.crc_body;
    key[1] = b.crc_head;
    return 1;
}

RecReader* recr_dump_world(const char* path, char* why, unsigned cap)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, cap, "cannot open frame dump (%lu)", GetLastError());
        return NULL;
    }
    return open_handle(f, path, why, cap, 1);
}

/* The header block, then every block's header to the end block, whose payload alone is read: the
   walk index_build makes, with the same checks, stopping at the first torn or cut block. */
int recr_summary(const char* path, RecSummary* s, char* why, unsigned cap)
{
    RecReader* r = (RecReader*)calloc(1, sizeof *r);
    char magic[8];
    RecBlock b;
    LARGE_INTEGER fs;
    unsigned long long off, size;
    unsigned i;
    int k, ok = 0;
    memset(s, 0, sizeof *s);
    if (!r) {
        snprintf(why, cap, "no memory");
        return 0;
    }
    r->f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (r->f == INVALID_HANDLE_VALUE) {
        snprintf(why, cap, "cannot open %s (%lu)", path, GetLastError());
        goto done;
    }
    if (!read_all(r->f, magic, 8) || memcmp(magic, REC_MAGIC, 8)) {
        snprintf(why, cap, "%s is not a recording", path);
        goto done;
    }
    r->off = 8;
    k = block_read(r, &b, why, cap);
    if (k <= 0 || b.kind != RB_HEADER) {
        if (k >= 0) snprintf(why, cap, "%s has no header block", path);
        goto done;
    }
    if (!header_parse(&r->h, r->raw, r->rawn, why, cap)) goto done;
    payload_free(r);
    s->format = r->h.format;
    s->start = s->last = r->h.start;
    lstrcpynA(s->map, r->h.map, sizeof s->map);
    for (i = 0; i < r->h.nseats; i++) lstrcpynA(s->name[i], r->h.seats[i].name, RECR_NAME);
    s->nseats = r->h.nseats;
    if (r->h.has_mp) {
        if (segs_scan(r, 0, why, cap) < 0) goto done;
        s->last = r->seg[0].t.last ? r->seg[0].t.last : s->start;
        s->end_reason = r->seg[0].t.end_reason;
        ok = 1;
        goto done;
    }
    if (!GetFileSizeEx(r->f, &fs)) {
        snprintf(why, cap, "cannot size the file (%lu)", GetLastError());
        goto done;
    }
    size = (unsigned long long)fs.QuadPart;
    for (off = r->off; off + sizeof b <= size; off += sizeof b + b.packed) {
        if (!file_at(r, off) || !read_all(r->f, &b, sizeof b) || b.magic != REC_BLOCK_MAGIC ||
            (uint32_t)Crc32_ComputeBuf(0, &b, 28) != b.crc_head || b.packed > RECR_BLOCK_MAX ||
            b.raw > RECR_BLOCK_MAX || block_flags_bad(&b) || off + sizeof b + b.packed > size)
            break;
        if (b.kind == RB_END) {
            if (block_body(r, &b, why, cap) > 0) {
                end_parse(r, r->raw, r->rawn);
                s->end_reason = r->end_reason;
                if (r->end_last) s->last = r->end_last;
            }
            break;
        }
        if (b.kind == RB_STREAM && b.last > s->last) s->last = b.last;
    }
    ok = 1;
done:
    recr_close(r);
    return ok;
}

const RecHeader* recr_header(const RecReader* r) { return &r->h; }

int recr_segment_mp(const RecReader* r) { return r->in_seg && seg_header(r)->has_mp; }

int recr_segment_cells(const RecReader* r) { return r->in_seg && recr_holds_cells(seg_header(r)); }

/* 1 the header is of this format */
static int info_header(RecInfo* o, const unsigned char* b, size_t len)
{
    size_t at = 0;
    unsigned format = 0;
    while (at + 4 <= len) {
        unsigned short tag, n;
        const unsigned char* p = b + at + 4;
        memcpy(&tag, b + at, 2);
        memcpy(&n, b + at + 2, 2);
        if (at + 4 + n > len) break;
        if (tag == RT_FORMAT && n == 4) format = u32(p);
        else if (tag == RT_GAME_VERSION) text(o->version, sizeof o->version, p, n);
        else if (tag == RT_MODPRINT && n == 8) {
            o->modprint = u32(p);
            o->archives = u32(p + 4);
            o->has_modprint = 1;
        } else if (tag == RT_ARCHIVE && n >= 12 && o->narchives < REC_ARCHIVES) {
            o->archive[o->narchives].size = u32(p);
            memcpy(&o->archive[o->narchives].mtime, p + 4, 8);
            text(o->archive[o->narchives].name, RECR_TEXT, p + 12, n - 12u);
            o->narchives++;
        } else if (tag == RT_PLAYER && n >= 1 && p[0] < REC_PLAYERS)
            o->seat[p[0]].present = 1;
        at += (4 + (size_t)n + 3) & ~(size_t)3;
    }
    return rec_format_supported(format);
}

static void info_end(RecInfo* o, const unsigned char* b, size_t len)
{
    size_t at = 0;
    o->ended = 1;
    while (at + 4 <= len) {
        unsigned short tag, n;
        const unsigned char* p = b + at + 4;
        memcpy(&tag, b + at, 2);
        memcpy(&n, b + at + 2, 2);
        if (at + 4 + n > len) break;
        if (tag == RT_GAME_END && n == 4) o->game_end = u32(p);
        else if (tag == RT_SEAT_END && n == 12 && p[0] < REC_PLAYERS) {
            o->seat[p[0]].result = p[1];
            o->seat[p[0]].commander = u32(p + 4);
            o->seat[p[0]].defeat = u32(p + 8);
        }
        at += (4 + (size_t)n + 3) & ~(size_t)3;
    }
}

static void info_minimap(RecInfo* o, const unsigned char* b, size_t len)
{
    unsigned short v[4];
    if (len < REC_MINIMAP_HEAD) return;
    memcpy(v, b, 8);
    if ((size_t)v[0] * v[1] != len - REC_MINIMAP_HEAD || len - REC_MINIMAP_HEAD > REC_MINIMAP_MAX)
        return;
    o->mm_w = v[0];
    o->mm_h = v[1];
    o->mm_cols = v[2];
    o->mm_rows = v[3];
    memcpy(o->minimap, b + REC_MINIMAP_HEAD, len - REC_MINIMAP_HEAD);
}

int recr_info(const char* path, RecInfo* out, char* why, unsigned cap)
{
    RecReader* r = (RecReader*)calloc(1, sizeof *r);
    char magic[8];
    RecBlock b;
    LARGE_INTEGER size;
    int k, ok = 0;
    memset(out, 0, sizeof *out);
    if (!r) {
        snprintf(why, cap, "no memory");
        return 0;
    }
    r->f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
    if (r->f == INVALID_HANDLE_VALUE) {
        snprintf(why, cap, "cannot open %s (%lu)", path, GetLastError());
        free(r);
        return 0;
    }
    if (!read_all(r->f, magic, 8) || memcmp(magic, REC_MAGIC, 8)) {
        snprintf(why, cap, "%s is not a recording", path);
        goto done;
    }
    r->off = 8;
    k = block_read(r, &b, why, cap);
    if (k <= 0 || b.kind != RB_HEADER) {
        if (k >= 0) snprintf(why, cap, "%s has no header block", path);
        goto done;
    }
    if (!header_parse(&r->h, r->raw, r->rawn, why, cap)) goto done;
    if (!info_header(out, r->raw, r->rawn)) {
        snprintf(why, cap, "%s is not a recording of format %u", path, REC_FORMAT);
        goto done;
    }
    if (r->h.has_mp) {
        unsigned long long first = r->off;
        if (segs_scan(r, 0, why, cap) < 0 || !file_at(r, first)) goto done;
    }
    ok = 1;
    if (!GetFileSizeEx(r->f, &size)) goto done;
    /* the minimap follows the header; the end block is the last: what lies between is skipped */
    while ((k = block_head(r, &b, why, cap)) > 0) {
        if (b.kind == RB_MINIMAP || b.kind == RB_END) {
            if (block_body(r, &b, why, cap) < 0) break;
            if (b.kind == RB_MINIMAP) info_minimap(out, r->raw, r->rawn);
            else {
                info_end(out, r->raw, r->rawn);
                break;
            }
        } else if (r->off + sizeof b + b.packed > (unsigned long long)size.QuadPart ||
                   !file_at(r, r->off + sizeof b + b.packed))
            break;
    }
done:
    recr_close(r);
    return ok;
}
RecWorld* recr_world(RecReader* r) { return &r->w; }

void recr_close(RecReader* r)
{
    unsigned i;
    if (!r) return;
    payload_free(r);
    mp_read_free(&r->mp_query);
    mp_read_free(&r->mp_apply);
    if (r->f && r->f != INVALID_HANDLE_VALUE) CloseHandle(r->f);
    free(r->pk);
    reccodec_free(r->codec);
    ZSTD_freeDCtx((ZSTD_DCtx*)r->dctx);
    ZSTD_freeDCtx((ZSTD_DCtx*)r->rdctx);
    free(r->sec);
    free(r->dec.p);
    if (r->w.list)
        for (i = 0; i < r->w.slots * 2; i++) free(r->w.list[i].img);
    for (i = 0; i < r->w.ntab && r->w.tab; i++) free(r->w.tab[i].img);
    free(r->w.list);
    free(r->w.tab);
    free(r->w.live);
    free(r->w.changed);
    free(r->w.ev);
    free(r->w.feat);
    free(r->w.click);
    free(r->w.sel);
    recsprays_free(r->w.sprays);
    free(r->w.pool);
    free(r->w.proj);
    free(r->w.mapped);
    free(r->touched);
    free(r->yards.st);
    free(r->yards.touched);
    free(r->idx);
    segs_free(r->seg, r->nseg);
    if (r->w.unit) VirtualFree(r->w.unit, 0, MEM_RELEASE);
    if (r->w.grid) VirtualFree(r->w.grid, 0, MEM_RELEASE);
    if (r->base) VirtualFree(r->base, 0, MEM_RELEASE);
    free(r->wbase);
    free(r);
}

/* ---- timeline maintenance ------------------------------------------------------------------ */

static int write_all(HANDLE f, const void* p, DWORD n)
{
    DWORD done;
    return WriteFile(f, p, n, &done, NULL) && done == n;
}

static int stored_block(HANDLE f, unsigned kind, unsigned last, const void* p, unsigned n)
{
    RecBlock b = {0};
    b.magic = REC_BLOCK_MAGIC;
    b.kind = (uint16_t)kind;
    b.flags = RBF_STORED;
    b.first = b.last = last;
    b.raw = b.packed = n;
    b.crc_body = (uint32_t)Crc32_ComputeBuf(0, p, n);
    b.crc_head = (uint32_t)Crc32_ComputeBuf(0, &b, 28);
    return write_all(f, &b, sizeof b) && write_all(f, p, n);
}

static int maintenance_end(HANDLE f, unsigned reason, unsigned last)
{
    uint32_t e[4] = {(4u << 16) | RT_END_REASON, reason, (4u << 16) | RT_LAST, last};
    return stored_block(f, RB_END, last, e, sizeof e);
}

static unsigned metadata_rows(const RecReader* r, RecTimelineState* rows)
{
    unsigned i, n = 0, high = 0;
    for (i = 0; i < r->nseg; i++) {
        const RecTimeline* t = &r->seg[i].t;
        rows[n++] = (RecTimelineState){t->id, (uint8_t)t->state, {0},
                                      (uint32_t)t->last_used, (uint32_t)(t->last_used >> 32)};
        if (t->id > high) high = t->id;
    }
    if (high < r->max_id)
        rows[n++] = (RecTimelineState){r->max_id, REC_TL_DELETED, {0}, 0, 0};
    return n;
}

/* Copy the original block, after checking the payload CRC. Maintenance must never publish a
   previously unchecked corrupt stream or snapshot as a clean compacted file. */
static int copy_block(RecReader* r, HANDLE out, unsigned long long off, char* why, unsigned cap)
{
    RecBlock b;
    unsigned char buf[32768];
    unsigned left;
    if (!seek_to(r->f, off) || !read_all(r->f, &b, sizeof b) ||
        crc_stream(r, off, &b) != BLK_WHOLE || !seek_to(r->f, off) ||
        !read_all(r->f, &b, sizeof b) || !write_all(out, &b, sizeof b)) goto fail;
    left = b.packed;
    while (left) {
        unsigned n = left > sizeof buf ? sizeof buf : left;
        if (!read_all(r->f, buf, n) || !write_all(out, buf, n)) goto fail;
        left -= n;
    }
    return 1;
fail:
    snprintf(why, cap, "cannot copy the block at byte %llu (%lu)", off, GetLastError());
    return 0;
}

static int file_compact(RecReader* r, const char* path, const char* output, unsigned chosen,
                        char* why, unsigned cap)
{
    unsigned needed[RECR_TIMELINES] = {0}, cutoff[RECR_TIMELINES] = {0};
    RecTimelineState rows[REC_TIMELINE_ROWS];
    unsigned i, j, n = 0, high = 0, dead = 0;
    char tmp[MAX_PATH];
    HANDLE f = INVALID_HANDLE_VALUE;
    int exporting = output && output[0], ok = 0;
    const char* target = exporting ? output : path;
    for (i = 0; i < r->nseg; i++) {
        const RecTimeline* t = &r->seg[i].t;
        if (t->state == REC_TL_DELETED) dead++;
        if ((exporting && t->id == chosen) || (!exporting && t->state == REC_TL_LIVE)) {
            unsigned s = i;
            needed[s] = 1;
            while (s) {
                unsigned parent = seg_index(r, r->seg[s].t.parent);
                if (cutoff[parent] < r->seg[s].t.fork_at)
                    cutoff[parent] = r->seg[s].t.fork_at;
                needed[parent] = 1;
                s = parent;
            }
        }
    }
    if (!exporting && !dead) {
        snprintf(why, cap, "no dead segments to compact");
        return 1;
    }
    /* The root header is the format envelope even for a file whose last live timeline was
       deleted. No deleted timeline is selectable, including this empty root. */
    needed[0] = 1;
    if (strlen(target) + 4 >= sizeof tmp) {
        snprintf(why, cap, "the temporary path is too long");
        return 0;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", target);
    f = CreateFileA(tmp, GENERIC_WRITE | DELETE, FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE || !write_all(f, REC_MAGIC, 8)) goto io_fail;
    for (i = 0; i < r->nseg; i++) {
        RecSeg* s = &r->seg[i];
        const RecTimeline* t = &s->t;
        unsigned last = 0;
        int whole = exporting ? t->id == chosen : t->state == REC_TL_LIVE;
        int ended = 0;
        if (!needed[i]) continue;
        if (!copy_block(r, f, t->header_off, why, cap)) goto done;
        for (j = s->i0; j < s->i0 + s->n; j++) {
            const RecIndexEntry* e = &r->idx[j];
            /* A crossing stream block stays whole. Keeping it makes the prefix a valid
               stream without recompressing records, and contains every child boundary.
               A pruned segment keeps its blocks before its cutoff, a prefix of the segment,
               and every block is copied unchanged: so every run and every snapshot chain
               kept keeps its beginning -- the RBF_FRAME_RESET block and the key before it --
               which is why a compacted file stays readable (tagpu_recfmt.h, THE RUN AND THE
               CHAIN). */
            if (!whole && (!cutoff[i] || e->kind == RB_END || e->first >= cutoff[i])) continue;
            if (!copy_block(r, f, e->off, why, cap)) goto done;
            if (e->kind == RB_STREAM) last = e->last;
            if (e->kind == RB_END) ended = 1;
        }
        if (!ended && !maintenance_end(f, whole ? REC_END_UNFINISHED : REC_END_PRUNED, last))
            goto io_fail;
        rows[n++] = (RecTimelineState){t->id, whole ? REC_TL_LIVE : REC_TL_DELETED, {0},
                                      (uint32_t)t->last_used, (uint32_t)(t->last_used >> 32)};
        if (t->id > high) high = t->id;
    }
    if (high < r->max_id) rows[n++] = (RecTimelineState){r->max_id, REC_TL_DELETED, {0}, 0, 0};
    if (!stored_block(f, RB_TIMELINES, 0, rows, n * sizeof *rows) || !FlushFileBuffers(f))
        goto io_fail;
    /* Every Impure writer holds the same persistent path lock from before its scan through
       its last mutation. That ordering remains held when the source closes: MoveFileEx cannot
       replace an open destination on Windows/Wine, even one sharing DELETE. No cooperating
       writer can append in the close-to-replace interval. Other readers retain their ordinary
       handles and refuse replacement; a later close retries compaction. The temp is complete
       and flushed before the atomic replacement, so a crash leaves the old or the new file. */
    CloseHandle(r->f);
    r->f = INVALID_HANDLE_VALUE;
    if (!MoveFileExA(tmp, target, MOVEFILE_WRITE_THROUGH |
                    (exporting ? 0 : MOVEFILE_REPLACE_EXISTING))) goto io_fail;
    snprintf(why, cap, "%s %u; %u segments kept", exporting ? "exported timeline" :
             "compacted after timeline", chosen, n);
    ok = 1;
    goto done;
io_fail:
    snprintf(why, cap, "cannot %s the recording (%lu)", exporting ? "export" : "compact",
             GetLastError());
done:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return ok;
}

int recr_manage(const char* path, unsigned op, unsigned id, const char* output,
                char* why, unsigned cap)
{
    HANDLE f, lock;
    RecReader* r;
    RecTimelineState rows[REC_TIMELINE_ROWS];
    unsigned n, s;
    int ok = 0;
    lock = recfile_lock(path, why, cap);
    if (!lock) return 0;
    f = CreateFileA(path, op == REC_FILE_EXPORT ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, cap, "maintenance skipped: cannot write %s (%lu)", path, GetLastError());
        recfile_unlock(&lock);
        return 0;
    }
    r = open_handle(f, path, why, cap, 0);       /* the reader owns the locked handle from here */
    if (!r) { recfile_unlock(&lock); return 0; }
    if (segs_scan(r, 1, why, cap) < 0) goto done;
    s = seg_index(r, id);
    if (op != REC_FILE_COMPACT && (s == RECR_TIMELINES || r->seg[s].t.state != REC_TL_LIVE)) {
        snprintf(why, cap, "timeline %u is absent or deleted", id);
        goto done;
    }
    if (op == REC_FILE_EXPORT) {
        if (recr_timeline(r, id, why, cap) != 1) goto done;
        ok = file_compact(r, path, output, id, why, cap);
        goto done;
    }
    if (op == REC_FILE_TOUCH || op == REC_FILE_DELETE) {
        uint64_t now = recfile_now();
        if (op == REC_FILE_DELETE) r->seg[s].t.state = REC_TL_DELETED;
        else r->seg[s].t.last_used = now;
        n = metadata_rows(r, rows);
        if (!seek_to(f, r->file_end) || !SetEndOfFile(f) ||
            !stored_block(f, RB_TIMELINES, 0, rows, n * sizeof *rows) || !FlushFileBuffers(f)) {
            snprintf(why, cap, "cannot write the timeline table (%lu)", GetLastError());
            goto done;
        }
    }
    ok = file_compact(r, path, NULL, id, why, cap);
    if (op == REC_FILE_TOUCH || op == REC_FILE_DELETE) {
        char detail[160];
        snprintf(detail, sizeof detail, "%s", why);
        snprintf(why, cap, "%s timeline %u%s%s", op == REC_FILE_TOUCH ? "touched" : "deleted",
                 id, ok ? "; " : "; compaction deferred: ", detail);
        ok = 1;              /* the complete metadata block is already committed */
    }
done:
    recr_close(r);
    recfile_unlock(&lock);
    return ok;
}

int recr_mp_explored(RecReader* r, unsigned origin, RecMpExploreState* out, void* image, unsigned capacity,
                      char* why, unsigned cap)
{
    int k=mp_read(r,&r->mp_query,NULL,NULL,NULL,why,cap);
    return k<=0 ? k : tagpu_recmp_history_explored(r->mp_query.history,origin,out,image,capacity);
}

int recr_mp_pov(RecReader* r, unsigned origin, RecMpPovState* out, char* why, unsigned cap)
{
    int k=mp_sync(r,why,cap);
    return k<=0 ? k : tagpu_recmp_history_pov(r->mp_query.history,origin,out);
}

int recr_mp_select(RecReader* r, unsigned origin, RecMpSelectState* out, RecSelEntry* entries,
                   unsigned capacity, char* why, unsigned cap)
{
    int k=mp_sync(r,why,cap);
    return k<=0 ? k : tagpu_recmp_history_select(r->mp_query.history,origin,out,entries,capacity);
}
