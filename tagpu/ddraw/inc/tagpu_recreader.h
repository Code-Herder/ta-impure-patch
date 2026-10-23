#ifndef TAGPU_RECREADER_H
#define TAGPU_RECREADER_H
/* tagpu_recreader -- reads a demo recording back (tagpu_recfmt.h): its blocks, each checked,
   unpacked and, when sectioned, decoded (tagpu_reccodec.h), the header's entries, and the world
   the stream builds, one iteration at a time or from a snapshot at a seek
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S2b, S3a and S3b). tagpu_replay.c
   applies what this reads; this file reads no engine memory, and since it is not on
   thread-split.allow the build refuses an engine address in it.
   tools/replay/tarec.py reads the same records and accepts the same shapes: a change to one is a
   change to the other.

   Every value read here is data. A record whose length, slot, index or offset falls outside what
   its kind allows makes the recording unreadable from there, as the writer refuses the same
   records (tagpu_recwriter.c, apply). What this accepts is only well-formed: the values inside an
   image are bounded again by whoever uses one as an index or a pointer. */

#include <stdint.h>
#include "tagpu_recfmt.h"
#include "tagpu_reccodec.h"
#include "tagpu_reclossy.h"
#include "tagpu_recfile.h"
#include "tagpu_recmp_file.h"
#include "tagpu_recmp_units.h"
#include "tagpu_recmp_named.h"
#include "tagpu_recmp_roots.h"
#include "tagpu_recmp_explored.h"
#include "tagpu_recmp_text.h"
#include "tagpu_recmp_pov.h"

#define RECR_TEXT       64          /* a header string's bytes kept, NUL included            */
#define RECR_NAME       (0x1E + 1)  /* a seat's name: the record's 0x1E bytes, NUL included  */
#define RECR_BLOCK_MAX  (64u << 20) /* the most bytes a block may pack or unpack to, and a
                                       sectioned block decode to                              */
#define RECR_TIMELINES  64          /* the segments a file may hold: a file with more is refused
                                       by name                                                   */
#define RECR_BOUNDARY   2           /* recr_tick: the chain goes on in its next segment         */

/* a seat at the start: an RT_PLAYER entry */
typedef struct RecSeat {
    unsigned char index, controller, side, logo;
    unsigned char ally[11];                 /* the alliance row, record +0x108          */
    unsigned char has_info;                 /* 0 when the record had no PlayerInfo       */
    unsigned char info[REC_INFO_BYTES];
    char          name[RECR_NAME];          /* record +0x2B                              */
} RecSeat;

/* a declared table a recording names by index: an RT_TABLE entry */
typedef struct RecTableInfo {
    unsigned short index, flags;
    unsigned char  cls, key;
    unsigned       stride;
    unsigned       lhash;       /* the layout hash (tagpu_simreg_layout_hash)          */
    char           name[128];
} RecTableInfo;

typedef struct RecHeader {
    unsigned     format, mode, start, slots, units_each;
    unsigned     has_frame, frame;
    unsigned     has_unitdefs, ndefs, defs_hash;
    unsigned     has_weapons, nweapons, weapons_hash;
    unsigned     grid_w, grid_h, wrecks, nfdefs, fdefs_hash;     /* RT_FEATURES             */
    unsigned     has_options, lostype, cmdrdeath, difficulty, fixed_starts;
    unsigned     nplayers, controlled, viewed;
    unsigned     proj_cap;                  /* RT_PROJECTILES: the projectile pool's records  */
    char         build[RECR_TEXT], map[RECR_TEXT], settings_map[RECR_TEXT], layout[RECR_TEXT + 8];
    char         globals[RECR_TEXT + 8];    /* RT_GLOBALS: replay-globals.tsv's SHA-256       */
    unsigned     has_settings_map;
    unsigned     has_wallclock;             /* RT_WALLCLOCK: the start as a FILETIME, UTC, and
                                               the local time's offset from UTC in minutes    */
    uint64_t     wallclock;
    int          utc_offset;
    unsigned     has_timeline, tl_id, tl_parent, tl_fork_at;   /* RT_TIMELINE, a fork's    */
    unsigned     nseats;
    RecSeat      seats[REC_PLAYERS];            /* in the header's order               */
    unsigned     ntab;
    RecTableInfo tab[REC_TABLE_MAX];
    unsigned     has_mp;
    RecMpRecordHeader mp;
    unsigned     has_mp_seats;
    RecMpRoster mp_seats;
    unsigned     has_codec;                 /* RT_CODEC and RT_PHYSICS: a sectioned stream block
                                               may come; without them only records blocks    */
    RecCodecParams codec;                   /* their constants, RT_AIR's and RT_TYPES' (66 KB) */
    unsigned     has_hold;                  /* RT_HOLD: the stream holds the masks' bits between
                                               refreshes; a header without it holds nothing  */
    RecLossyHold hold;
} RecHeader;

/* 1 when the header's stream holds its cells' unit slots and yard bit (RT_HOLD's RECL_CELLS):
   its playing keeps the engine's own */
static __inline int recr_holds_cells(const RecHeader* h)
{
    return h->has_hold && (h->hold.flags & RECL_CELLS) != 0;
}

/* 1 when the header's stream holds nothing: no RT_HOLD -- its writer ran no lossy filter
   (tagpu_recwriter.c, header_apply: one branch makes the filter and writes RT_HOLD) -- or an
   RT_HOLD with no flag and no held bit, whose filter writes every record as it came
   (tagpu_reclossy.c). Its stream's world is then the writer's exact copy at every iteration,
   not only at a refresh. */
static __inline int recr_holds_nothing(const RecHeader* h)
{
    unsigned i;
    if (!h->has_hold) return 1;
    if (h->hold.flags) return 0;
    for (i = 0; i < REC_UNIT_IMAGE; i++) if (h->hold.unit[i]) return 0;
    for (i = 0; i < REC_ORDER_IMAGE; i++) if (h->hold.node[i]) return 0;
    return 1;
}

/* one node list of a live unit, its node images in list order */
typedef struct RecList { unsigned n, cap; unsigned char* img; } RecList;

/* a declared table's image */
typedef struct RecTable { unsigned index, stride, size, cap; unsigned char* img; } RecTable;

/* an event, or a feature image, of the iteration read last, pointing into the reader's block:
   valid until the next recr_tick */
typedef struct RecEvent { unsigned kind, len; const unsigned char* b; } RecEvent;

/* What changed a slot in the iteration read last (RecWorld.changed). */
#define RECW_IMAGE  0x01    /* its image: an RK_UNIT or an RK_UNIT_DELTA                     */
#define RECW_MAIN   0x02    /* its main list (+0x5C) was sent whole                           */
#define RECW_SUB    0x04    /* its sub list (+0x60)                                           */
#define RECW_BORN   0x08    /* an RK_UNIT: the slot holds a new unit, or a correction's image  */
#define RECW_GONE   0x10    /* an RK_UNIT_GONE: its unit died, and its lists with it           */

/* The recorded world at the end of one iteration's images: every live slot's image and lists,
   the ten players, the controllers and the viewer, the declared tables, the features, the
   globals, the projectile pool, the explored map and the human seat's perspective. */
typedef struct RecWorld {
    unsigned       slots;       /* slots 1 .. slots - 1 hold units, as the engine's array     */
    unsigned       tick;        /* the GameTime of the images it holds                        */
    unsigned char* live;        /* [slots]                                                    */
    unsigned char* changed;     /* [slots]: RECW_* by the iteration read last                 */
    uint32_t*      unit;        /* [slots * REC_UNIT_DWORDS]                                  */
    RecList*       list;        /* [slots * 2]: the main list, then the sub list              */
    uint32_t       player[REC_PLAYERS][REC_PLAYER_DWORDS];
    unsigned char  pchanged[REC_PLAYERS];   /* a player's image changed in the iteration     */
    unsigned char  ctl[REC_PLAYERS];
    unsigned char  view[2];     /* main+0x2A42 and main+0x2A43 on the recording machine       */
    unsigned       ntab;
    RecTable*      tab;         /* [ntab], the header's tables in its order                    */
    unsigned       nev;         /* the events of the step after the images, in engine order: a
                                   feature root's bracket, RK_EV_FEATURE to its END, holds the
                                   draws it made, the tick's cells and what it nested; then
                                   the sprays the spray sessions imply, slots ascending       */
    RecEvent*      ev;
    RecSprays*     sprays;      /* the spray sessions' table (tagpu_reclossy.h), read with
                                   RT_HOLD's RECL_SPRAYS: what the implied sprays come from   */
    unsigned char* grid;        /* [grid_w * grid_h] cell images, row-major                   */
    unsigned char* pool;        /* [wrecks] record images                                     */
    int32_t        heads[4];    /* the pool's three heads and the reproduction cursor         */
    unsigned       nfeat;       /* the iteration's RK_CELLS, RK_WRECK and RK_FEATURES, in the
                                   order they came: what changed in the grid and the pool     */
    RecEvent*      feat;
    uint32_t       globals[REC_GLOBALS_DWORDS];   /* the globals image (tagpu_replay_globals.h) */
    unsigned char* proj;        /* [proj_cap] projectile record images, REC_PROJ_IMAGE each:
                                   [proj_hw, proj_cap) are zero, as on the recording machine   */
    unsigned       nproj;       /* the pool's count, main+0x141F3                             */
    unsigned       proj_hw;     /* its written extent                                         */
    unsigned char* mapped;      /* [mapped_bytes] the explored map *(main+0x14273)            */
    unsigned       mapped_bytes;   /* REC_MAPPED_BYTES of the header's grid                   */
    int            mapped_whole;   /* its RK_MAPPED records have all come                     */
    unsigned       corrections; /* RK_CORRECTs read: the recording machine's own repairs      */
    /* THE PERSPECTIVE (tagpu_recpov.h): a skirmish's human seat's, as its RK_POV, RK_SELECT and
       RK_CLICK records leave it, and as a snapshot's RK_POV_AT and RK_SELECT rebuild it */
    unsigned       npov[REC_PLAYERS];          /* a seat's samples held: 0, 1 or 2               */
    RecPov         pov[REC_PLAYERS][2];        /* ... the earlier first, the latest last          */
    unsigned       pov_tick[REC_PLAYERS][2];   /* ... and the GameTime each was taken             */
    unsigned       nsel[REC_PLAYERS];          /* a seat's selection, ascending by slot ...       */
    RecSelEntry*   sel;                        /* ... [REC_PLAYERS * REC_SEL_MAX], seat-major    */
    unsigned       nclick;      /* the iteration's clicks, in the order they came: after the
                                   step's events, so each is a click of the frames between this
                                   capture point and the next                                 */
    RecEvent*      click;
} RecWorld;

typedef struct RecReader RecReader;

/* Opens a recording, reads its header block and finds its segments (tagpu_recfmt.h). NULL, with
   the reason in `why`, when the file cannot be read, is not a recording of this format, or its
   segments break the bounds recr_timelines states. It shares the file for reading and writing:
   a block is whole or not by its two CRCs, and every walk stops at the first torn one, so a
   reader of a file being appended reads its complete blocks only. The timeline played is the
   original (0) until recr_timeline chooses another. */
RecReader* recr_open(const char* path, char* why, unsigned cap);
/* A frame dump's single complete snapshot, already applied; never a sequential recording.
   Its comparison-only sight, count and raw-memory records are skipped, never copied to engine
   memory. The returned world is labelled frame+1. Caller closes it with recr_close. */
RecReader* recr_dump_world(const char* path, char* why, unsigned cap);
/* the file's first header: every segment of a chain agrees with it in shape (recr_timeline) */
const RecHeader* recr_header(const RecReader* r);
RecWorld* recr_world(RecReader* r);

/* Reads the next iteration into the world: its images, and its events into `ev`.
   1 read; 0 the recording has no more iterations (`why` says how it ended); -1 unreadable from
   here (`why` says where), and the world is as the last iteration left it; RECR_BOUNDARY the
   world holds the chosen chain's last iteration before a fork, and the chain goes on in the next
   timeline (`why` names it and its fork_at): nothing is read and the world is unchanged, and a
   seek to fork_at crosses there (recr_seek). A snapshot block met on the way is skipped unread:
   only a seek reads one. */
int recr_tick(RecReader* r, char* why, unsigned cap);

/* Puts the world at iteration T of the chosen chain, as a recr_tick that read T leaves it: T's
   images, and the events of the step after them in `ev`; recr_tick then goes on from T + 1. It
   builds from the last snapshot before T over its segment's base grid (that segment's first
   iteration's, which it reads first when the reader does not hold it), or from a segment's first
   iteration when no snapshot precedes T, then reads the stream up to T, crossing each fork on
   the way: at a timeline's fork_at the child's first iteration replaces the world whole.
   1 the world holds T; 0 T is outside the chain -- before its first iteration or after the
   last complete block's -- checked before anything changes, so the reader is as it was (`why`
   names the chain's first and last GameTime); -1 unreadable (`why` says where), and the
   world is undefined until a seek that returns 1.
   With `visit`, the snapshot it builds from is the last one more than `lead` iterations before T
   (S + lead < T), and `visit(ctx, world)` is called once each iteration of the stream after it
   has been read, T's included and both sides of a fork, so a caller can follow what the
   iterations before T did. */
typedef void (*RecVisit)(void* ctx, const RecWorld* w);
int recr_seek(RecReader* r, unsigned T, unsigned lead, RecVisit visit, void* ctx, char* why,
              unsigned cap);
/* What the reader's last recr_seek did, valid when it returned 1: the GameTime T it was asked for
   (`target`); the iteration it built from (`from`), the snapshot's GameTime S (`from_snapshot`
   1) or the first iteration of the segment it read from (0); and the iterations it read
   (`iterations`), each read of an iteration counted, a snapshot's base segment's first included,
   so iterations <= T - from + 1. tools/replay/tarec.py's seek_start computes `from` from the file
   alone. The blocks it decoded (`blocks`: read, checked and unpacked, each decode counted), their
   bytes in the file (`packed`) and unpacked (`raw`), each the block header's, and of them `base`,
   1 when a snapshot's segment's first iteration was read first because the reader did not hold
   its base: the work tools/replay/tareplay.py's W3 checks against the file's own blocks, which no
   load changes. A chained snapshot counts every link from its key, each a block decoded. A run's
   chunk counts as its block: its header's packed and raw, the chunk in the file and the
   container it unpacks to, not the run's window. A seek decodes each run it reads from its
   RBF_FRAME_RESET block on, since it starts only where one begins -- after a snapshot, at a
   segment's first iteration -- so no chunk is unpacked twice or left out. tarec.py's seek_blocks
   derives the same from the file. */
typedef struct RecSeekWork {
    unsigned target, from, from_snapshot, iterations;
    unsigned blocks, base;
    unsigned long long packed, raw;
} RecSeekWork;
const RecSeekWork* recr_seek_work(const RecReader* r);

void recr_close(RecReader* r);

/* What a list of recordings shows of one, without building its world: the header's map and seats,
   the first recorded GameTime, the last (the end block's, else the last whole stream block's), and
   why it ended (RecEnd; 0 when the file has no end block, as one cut short has none). */
typedef struct RecSummary {
    unsigned format, start, last, end_reason, nseats;
    char     map[RECR_TEXT];
    char     name[REC_PLAYERS][RECR_NAME];      /* the seats' names, in the header's order    */
} RecSummary;

/* 1 read; 0 the file cannot be read or is not a recording of this format (why). */
int recr_summary(const char* path, RecSummary* s, char* why, unsigned cap);
/* What `path` is to this build, for the prune (tagpu_recfile.h): 1 a recording it reads, 0 one of
   our recordings in a format it does not (its magic, header block and format entry read), -1
   anything else, a file it cannot open or read among them. */
int recr_format_of(const char* path);
/* THE ROOT KEY: what tells one recording from another at the same name for the whole life of the
   file, as its size does not. It is the root segment's RB_HEADER block, the file's first, right
   after REC_MAGIC, whose bytes nothing rewrites: a fork, a touch and a deletion's timeline table
   are written after the last complete block (file_append, recr_manage), which a playable root
   segment's header always precedes, and a compaction or an export copies the root header first
   and unchanged (file_compact: copy_block, needed[0]). Its RT_WALLCLOCK, the start to 100 ns, makes
   it one recording's. key[0] is the block's crc_body, key[1] its crc_head. Read through `f` from
   byte 0, which moves its file pointer, so any handle with read access serves, an exclusive one
   included. 1 read; 0 the file does not begin with a whole header block (not a recording, or one
   cut before its first block's header ends), the key zero; -1 the file could not be read. */
int recr_root_key(void* f, unsigned key[2]);
/* The start `path`'s header records (RT_WALLCLOCK), as a FILETIME in UTC, for the prune's order:
   1 read, from a recording of any format whose header carries the entry as this build writes
   it; 0 read without one: a file that is not one of our recordings (its first 8 bytes are not
   the magic), or a header without the entry at 12 bytes; -1 not read: no memory, the file
   cannot be opened, a read fails, or its header block cannot be read whole. */
int recr_wallclock_of(const char* path, FILETIME* utc);

/* A TIMELINE is a segment of the file (tagpu_recfmt.h): the original, id 0, or a fork recorded
   from a takeover. A CHAIN is a timeline and its ancestors, root first: segment s covers
   [s.first, c.fork_at - 1] for its child c on the chain, and the chosen timeline [first, last]. */
typedef struct RecTimeline {
    unsigned state;
    uint64_t last_used;
    unsigned id, parent, fork_at;      /* RT_TIMELINE; 0, 0, 0 for the original                     */
    unsigned first, last;              /* its first and last complete iteration; 0, 0 for none      */
    unsigned end_reason;               /* RT_END_REASON of its end block; 0 when it has none        */
    unsigned difficulty, has_ai;       /* its RT_OPTIONS difficulty; an RT_PLAYER of controller 2   */
    unsigned controlled;               /* its RT_OPTIONS controlled player (RecHeader.controlled),
                                          main+0x2A42 when the recorder wrote the header: in a
                                          takeover's fork the seat it was played as. READ: the
                                          takeover's tk_live stores P there, then tk_end arms the
                                          fork, whose header header_build writes at its first
                                          capture point (tagpu_recorder.c); the forks lane's
                                          `hud state` check compares every fork's seat with P   */
    unsigned long long header_off;     /* its RB_HEADER's byte                                      */
    unsigned long long first_off;      /* the block after its header                                */
    unsigned long long end_off;        /* the byte after its last complete block                    */
} RecTimeline;

/* Where a fork is appended (recr_append_point) */
typedef struct RecAppend {
    unsigned long long off;            /* the byte after the file's last complete block, or before
                                          an unfinished segment with no complete iteration (cut)   */
    unsigned next_id;                  /* one more than every kept segment's id                     */
    unsigned parent;                   /* the chain's segment holding GameTime `at` (the argument)  */
    int      end_open;                 /* the last segment kept has no end block                    */
    unsigned nstates, evict;           /* UINT32_MAX: no eviction */
    RecTimelineState states[REC_TIMELINE_ROWS];
    unsigned open_id, open_last;       /* ... its id and its last complete iteration                */
} RecAppend;

/* The file's segments, found afresh (the file may have grown): at most `cap` into `out`. The
   number of segments, or -1 (why) when the file cannot be read or breaks a bound: more than
   RECR_TIMELINES segments, a segment after the first without RT_TIMELINE, an id used twice, or
   a parent that is not an earlier segment with a lower id. */
int recr_timelines(RecReader* r, RecTimeline* out, unsigned cap, char* why, unsigned wcap);
/* Chooses timeline `id` to play, before the first iteration is read. 1 chosen, the reader at the
   root's first iteration; 0 refused (why): no such timeline, a fork outside its parent's
   iterations, or a segment whose header differs in shape from the first's; -1 an iteration has
   been read already, or the file cannot be read. */
int recr_timeline(RecReader* r, unsigned id, char* why, unsigned cap);
/* the chosen timeline */
const RecTimeline* recr_selected(const RecReader* r);
/* The chain that plays, root first: its timelines' ids, at most `cap` into `ids`; their count.
   It reads only the reader's own record of the chain, never the file. */
unsigned recr_chain_ids(const RecReader* r, unsigned* ids, unsigned cap);
/* 1 when the segment the world was last read from carries a multiplayer roster: a multiplayer
   file's own segment, never a takeover's timeline, which a skirmish level records */
int recr_segment_mp(const RecReader* r);
/* 1 when that segment holds its cells (recr_holds_cells): every skirmish segment a writer of
   this format makes (REC_SKIRMISH_HOLDS), a multiplayer file's takeover timeline among them */
int recr_segment_cells(const RecReader* r);
/* The controllers of the seats of the segment the world was last read from, by record:
   ctl[index] its RT_PLAYER controller for each seat that header names, 0 for every other
   record. 1, or 0 before an iteration is read (ctl all 0). A copy: the segment's header is
   freed by the next scan of the file's segments (segs_scan), which most reader calls run. */
int recr_segment_controllers(const RecReader* r, unsigned char ctl[REC_PLAYERS]);
/* Where a fork at `at` is appended, found afresh: the caller holds the file open for writing
   first, so no other writer can change it until the fork's writer has used this (tagpu_replay.c,
   TK0). A torn last block is not kept, an unfinished last segment with no complete iteration is
   cut whole, and the parent is the chosen chain's segment that holds `at`. 1, or 0 (why): `at` is
   outside the chain, `at + 1` is a chain segment's first iteration, the file holds
   RECR_TIMELINES segments, a block is damaged with data after it, or the file cannot be read. */
int recr_append_point(RecReader* r, unsigned at, RecAppend* a, char* why, unsigned cap);
/* 1 when the file HANDLE `f` is the file the reader holds open: one volume, one file index */
int recr_same_file(const RecReader* r, void* f);
/* The last refresh iteration at or before T of the chosen chain, found afresh from the index: the
   largest `last` <= T of a stream block with RBF_REFRESH in the chain's segment that covers T, or
   that segment's first iteration (tagpu_reclossy.h, reclossy_refresh_at), and with `seg` that
   segment's RecTimeline. At a refresh the stream's world is the writer's exact copy, which is
   where a takeover of a segment that holds something starts (tagpu_replay.c, tk_bound;
   recr_exact_at). The GameTime, or 0 (why) when T is outside the chain or the file cannot be
   read. */
unsigned recr_refresh_at(RecReader* r, unsigned T, RecTimeline* seg, char* why, unsigned cap);
/* Whether the chosen chain's segment that covers T holds nothing (recr_holds_nothing), found
   afresh as recr_refresh_at finds it, with `seg` (NULL for none) that segment's RecTimeline: 1 it
   holds nothing, so every iteration of it is exact; 0 it holds something, so only its refreshes
   are; -1 (why) T is outside the chain or the file cannot be read. */
int recr_exact_at(RecReader* r, unsigned T, RecTimeline* seg, char* why, unsigned cap);

/* Authority at the currently read world tick. Rebuild after seek/segment/index
   changes; advancing queries consume only new application records. Unknown stays
   unknown; late proof records never publish. 1 found, 0 unknown, -1 invalid (why).
   Player images use recorder row coordinates and retain source provenance.
   These return copied normalized values, never write engine state. */
int recr_mp_player(RecReader* r, unsigned origin, RecMpPlayerState* out, char* why, unsigned cap);
int recr_mp_explored(RecReader* r, unsigned origin, RecMpExploreState* out, void* image, unsigned capacity,
                      char* why, unsigned cap);
/* An actor's last two camera/cursor samples and its last whole selection (tagpu_recmp_pov.h);
   a NULL `entries` asks for the selection's metadata only, else `capacity` holds every entry. */
int recr_mp_pov(RecReader* r, unsigned origin, RecMpPovState* out, char* why, unsigned cap);
int recr_mp_select(RecReader* r, unsigned origin, RecMpSelectState* out, RecSelEntry* entries,
                   unsigned capacity, char* why, unsigned cap);
int recr_mp_unit(RecReader* r, unsigned slot, RecMpUnitState* out, char* why, unsigned cap);
/* Latest applied batch for inspection; every root remains in the ordered ledger. */
int recr_mp_roots(RecReader* r, unsigned origin, RecMpRootsState* out, char* why, unsigned cap);
int recr_mp_named(RecReader* r, unsigned origin, RecMpNamedState* out, char* why, unsigned cap);
int recr_mp_text(RecReader* r, unsigned origin, RecMpTextState* out, char* why, unsigned cap);
int recr_mp_orders(RecReader* r, unsigned slot, unsigned list, RecMpOrderState* out,
                     void* image, unsigned capacity, char* why, unsigned cap);

typedef void (*RecMpApplyVisit)(void* ctx, const RecMpRecord* where,
                                const RecMpHeader* packet, const void* body, unsigned bytes);
/* Visit every application envelope after *ordinal through the current world
   tick, in recorded order. Both clocks, source sequence and batch bytes stay
   intact; proof tails never visit. Arguments are borrowed until the callback
   returns; callbacks must not call into this reader. Set *ordinal to zero for a
   new reconstruction, including after seeking backwards. A copied-state query
   cannot consume this cursor. Repeating an acknowledged prefix visits nothing.
   1 success, 0 before a world/non-multiplayer, -1 invalid (why). Each completed
   callback advances *ordinal even if a later read fails, so retry cannot repeat
   an already delivered batch. An ordinal beyond this world's prefix is refused
   before any callback. No engine state is read or written here. */
int recr_mp_apply(RecReader* r, uint64_t* ordinal, RecMpApplyVisit visit, void* ctx,
                    char* why, unsigned cap);

/* What the Replays browser shows of a recording, read from its header, its minimap block and its
   end block alone (the stream's blocks are skipped unread). */
typedef struct RecInfoSeat {
    unsigned char present;      /* the header seats this record (RT_PLAYER)                   */
    unsigned char result;       /* RecResult; REC_RESULT_UNKNOWN without an end block           */
    unsigned      commander;    /* the GameTime its first commander died, 0 none                */
    unsigned      defeat;       /* the GameTime its last unit died, 0 none                      */
} RecInfoSeat;
typedef struct RecInfo {
    char          version[RECR_TEXT];       /* RT_GAME_VERSION, "" when absent                   */
    unsigned      modprint;                 /* RT_MODPRINT: compare with tagpu_rec_mod_print()  */
    int           has_modprint;
    unsigned      archives;                 /* the archives mounted, RT_MODPRINT's count         */
    unsigned      narchives;                /* RT_ARCHIVE entries (at most REC_ARCHIVES)          */
    struct { char name[RECR_TEXT]; unsigned size; unsigned long long mtime; } archive[REC_ARCHIVES];
    int           ended;                    /* an end block was read                             */
    unsigned      game_end;                 /* RT_GAME_END                                       */
    RecInfoSeat   seat[REC_PLAYERS];
    /* the minimap: w x h palette indices, rows of w, of which [0, 2*cols) x [0, 2*rows) is
       shown; w = 0 when the recording has none. The indices live in this struct. */
    unsigned      mm_w, mm_h, mm_cols, mm_rows;
    unsigned char minimap[REC_MINIMAP_MAX];
} RecInfo;
/* 1 read (a recording without an end block, cut short, still reads: `ended` 0); 0 the file
   cannot be read or is not a recording of this format (why). Any thread; it opens the file
   read-only, shared. It reads each block's 32-byte header to reach the end block, the stream's
   bodies unread. RecInfo is about 71 KB: not for a small stack. */
int recr_info(const char* path, RecInfo* out, char* why, unsigned cap);

#endif
