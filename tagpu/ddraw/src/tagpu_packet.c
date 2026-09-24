/* tagpu_packet.c — the frame packet exchange: the primitive.
   Contract: tagpu_packet.h (consumer), tagpu_packet_pub.h (producer). Design:
   research/notes/frame-packet-exchange.html.

   THE EXCHANGE. N slots with roles, not owners fixed for life: W is the one
   the producer is filling, the consumer holds two or three (READ, PREV and —
   for the frame packet — a SPARE), and one sits in the CELL —
   fresh (published, not yet taken) or stale (returned, waiting to be reused).
   The cell is ONE aligned 32-bit word in our static storage: `idx (3 bits) |
   FRESH (bit 3)`, the reserved bits asserted zero. Each side exchanges a slot
   it holds into the cell and takes whatever was there, so the roles stay a
   permutation of the slots without a lock — and the init below makes them one
   to begin with: zeroed statics would put both threads on slot 0.

   THE ROTATION, AND WHY THERE IS A THIRD CONSUMER SLOT. The engine
   draws several times per sim tick, so consecutive packets often carry the
   same tick; a consumer that handed PREV back on every take would then hold
   two records of one tick and the pose blend would refuse — stepped motion,
   always. What is wanted is: a packet of the SAME tick as the one held
   replaces READ and keeps PREV. That decision needs the incoming record's
   tick, and the exchange has to name the slot it gives back BEFORE it learns
   which slot it gets — so with two held slots the only way to make it is to
   peek at the cell's slot first, which the producer may be refilling at that
   moment (`force` and `stress` both publish past the FRESH gate). A third
   held slot removes the question: SPARE is what goes back, always, and the
   rotation that follows the exchange picks — out of records this thread owns
   and nothing else writes — whether the record just taken displaces READ (same
   tick) or ages the pair by one (a new tick). The permutation is then over
   five slots instead of four, and C2 below is STRONGER for it: the slot handed
   back was last read at least a whole frame before it became the spare.

   ORDERING (x86-TSO). Four orderings the protocol needs, one instruction:
     P1  the payload is globally visible before the index that names it
     P2  the producer's next fill of the slot it RECEIVES stays after the
         exchange that received it
     C1  the consumer's loads of the new slot follow its exchange
     C2  the consumer's last loads of the slot it GIVES BACK precede it
   P1 and C1 hold for plain stores and loads on x86 already; P2 and C2 are
   what `xchg`'s implicit lock adds. The exchange is spelled with the GCC
   builtin at ACQ_REL so the COMPILER honours all four too: mingw's
   InterlockedExchange expands to __sync_lock_test_and_set, whose documented
   contract is acquire-only, and `volatile` orders nothing about the
   non-volatile payload. The signal fences around the head and tail stores
   keep the compiler from hoisting the fill across them; the hardware keeps
   store order on its own.

   HEAD BEFORE, TAIL AFTER. `head_seq` is stored before the fill and
   `tail_seq` after it; the consumer latches the head at acquire and compares
   the tail at frame_end. Both written after the fill would be a blind check:
   with them on either side, any fill that
   overlaps a consumer frame — the one thing the permutation forbids — is
   caught by the consumer that saw it, on the frame it saw it.

   NOTHING MOVES, NOTHING IS FREED. Each slot's address range is reserved
   once (VirtualAlloc MEM_RESERVE, PK_RESERVE per frame slot, which the
   design point's unit tables fit -- see below) and pages are COMMITTED as the
   high-water mark rises,
   by the producer, on the slot it holds as W. A fill that does not fit grows
   the slot and fills it again before it is published (`pkx_publish`); only a
   slot that cannot grow publishes a truncated frame (a bit per table in the
   header), and a commit that fails keeps the current size and retries later,
   never pins it. So no block ever moves, a pointer into a
   slot can never dangle, and a rule-breaking cached pointer reaches a slot's
   current bytes and never freed memory. Not reference counting: a count
   cannot see the raw pointer a module copies.

   WAIT-FREE, BOTH SIDES. Neither exchange can fail or block. A stuck consumer
   costs the producer one relaxed load per attempt (the FRESH gate skips the
   copy); a stuck producer leaves the consumer redrawing its held record,
   which is our memory. The sim never waits on the renderer, and — with the
   threads swapped for the command record — the renderer never waits on the
   sim.

   VIOLATIONS ARE LOUD, NEVER FATAL. Thread identity on both sides, the
   permutation after every exchange, head == tail and the structural bounds
   at acquire, a canary past the capacity, one acquire per frame, the tail at
   frame_end, the CRC under `check`. A record that fails a bound is refused
   for the frame (NULL) and counted; `viol=0` on the heartbeat is the gate.
   The levers, all read once at DLL attach from the gamedir:
     tagpu_packet.off     no slots, no publish, no acquire, no commands; the
                          observer stays in count-only mode so draws/s is
                          still reported
     tagpu_packet.check   CRC-32 of the record in the tail, verified per take
     tagpu_packet.stress  the producer publishes on EVERY in-play draw (the
                          overrun path) with a garbage pre-fill before the
                          real fill, from a one-page initial commit with a
                          dummy table that forces growth; the render thread
                          sleeps 0..50 ms inside each take of a frame packet
     tagpu_packet.poison  the consumer memsets the header of the slot it
                          hands back, so a pointer cached across frames
                          reads 0xDD instead of plausible stale data

   INSTANTIABLE, AND INSTANTIATED TWICE. Every piece of state
   lives in a PKX and the four operations take one. A record is anything
   that starts with the three-dword prefix {head_seq, cap_bytes, used_bytes}
   and ends with {crc, tail_seq}; what lies between is the instance's
   business, checked by its own `valid` callback after the structural checks
   here. The frame packet (s_frame: game thread -> render thread, 20 MB
   slots, FIVE of them, PREV handed out for the pose blend) and the command
   record (s_cmd: render thread -> game thread, 64 KB slots, four of them,
   latest wins with `force`, no PREV) are the two instances. The proof above is written once and holds for
   each, with `nslots`/`holds` the only numbers that differ. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "tagpu_packet.h"
#include "tagpu_packet_pub.h"
#include "crc32.h"
#include "tagpu_log.h"

#define PK_MAXSLOTS   5                    /* the frame instance's 5; cmd's 4 */
#define PK_RESERVE    (20u << 20)          /* address space per FRAME slot    */
#define PK_CMD_RESERVE (64u << 10)         /* ...and per COMMAND slot         */
#define PK_GRAIN      (64u << 10)          /* commit granularity              */
#define PK_PAGE       4096u                /* ...under stress, and for commands: one page */
#define PK_CANARY     0xC0FFEE42u
#define PKX_IDX       0x7u                 /* three index bits (up to 5 slots) */
#define PKX_FRESH     0x8u                 /* set by publish, cleared by acquire */
#define PKX_RESERVED  (~0xFu)
#define PK_HEARTBEAT  300
#define PK_HIST_N     256                  /* 2 us buckets to 512 us, + overflow */
#define PK_LOG_EVERY  64                   /* violations logged: the first, then every this many */
#define PK_PREFIX     12u                  /* head_seq, cap_bytes, used_bytes  */
#define PK_SUFFIX     8u                   /* crc, tail_seq                    */

/* THE FRAME SLOT'S RESERVE HOLDS THE UNIT-SCALED TABLES AT THE DESIGN POINT.
   The publisher lays the pieces down first and the units, wrecks and anchors
   straight after (tagpu_packet_pub.c), so those are the part a reserve that is
   too small would truncate -- and a truncated unit, piece or wreck table
   refuses the frame's whole unit hand-over. The sum below is every slot of
   TAGPU_PK_DESIGN_SLOTS at stock's worst unit model (36 pieces,
   ARMSCORP/CORSCORP), every record of the wreck pool at stock's worst wreck
   model (19 pieces, armscab_dead; 265 of the 285 3DO features are one piece),
   plus the anchor table: 19.6 MB under the raised limits. What follows them --
   effects, fog grids, minimap -- is bounded by its own caps and truncates on its
   own bit, which costs that layer alone. The reserve is address space, five
   slots of it in a 32-bit process whose largest free block has been logged as
   low as 43.6 MB (gpu-status.md §2.86), so it is sized to the design point
   rather than rounded up; pages are committed as the packets grow
   (`slot_commit`), so a game pays only for what it publishes. */
#define PK_DESIGN_PIECES 36u
#define PK_DESIGN_WRECK_PIECES 19u
#define PK_UNIT_WORST (sizeof(TAGPU_PACKET) + \
    TAGPU_PK_DESIGN_SLOTS * (sizeof(TAGPU_PK_UNIT) + PK_DESIGN_PIECES * sizeof(TAGPU_PK_PIECE)) + \
    TAGPU_PK_MAX_WRECKS * (sizeof(TAGPU_PK_WRECK) + PK_DESIGN_WRECK_PIECES * sizeof(TAGPU_PK_PIECE)) + \
    TAGPU_PK_MAX_ANCHORS * sizeof(TAGPU_PK_ANCHOR) + 64u /* the tables' 4-alignment */)
typedef char pk_reserve_design[(PK_RESERVE >= PK_UNIT_WORST) ? 1 : -1];

/* the record's prefix and suffix, wherever the instance's suffix sits */
#define REC_HEAD(p)     (((uint32_t*)(p))[0])
#define REC_CAP(p)      (((uint32_t*)(p))[1])
#define REC_USED(p)     (((uint32_t*)(p))[2])
#define REC_CRC(m, p)   (*(uint32_t*)((unsigned char*)(p) + (m)->recBytes - 8u))
#define REC_TAIL(m, p)  (*(uint32_t*)((unsigned char*)(p) + (m)->recBytes - 4u))

typedef const char* (*pkx_valid_fn)(const void* rec);              /* NULL = valid */
typedef int         (*pkx_pair_fn)(const void* rec, const void* prev); /* prev may be handed out */
typedef unsigned    (*pkx_fill_fn)(void* rec, void* ctx);
typedef unsigned    (*pkx_tick_fn)(const void* rec);   /* the record's tick, for
                                                          the rotation below    */

typedef struct PKX {
    const char*     name;
    unsigned        recBytes;              /* the record's fixed part: prefix at 0, suffix at the end */
    unsigned        reserve, grain;
    unsigned        nslots;                /* holds + 2 (the cell and W)      */
    unsigned        holds;                 /* slots the consumer keeps: 2, or
                                              3 with a spare for the rotation */
    pkx_valid_fn    valid;
    pkx_pair_fn     pair;                  /* NULL: never hands out a prev    */
    pkx_tick_fn     tick;                  /* NULL: no tick-aware rotation    */
    int             prodByRole;            /* the producer is whoever the driver says (the
                                              render thread, which restarts): recorded, not refused */
    int             consSleep;             /* under stress the consumer sleeps inside its take */
    unsigned char*  slot[PK_MAXSLOTS];     /* reserved base, fixed for life   */
    unsigned        cap[PK_MAXSLOTS];      /* committed bytes minus the canary;
                                              written by whoever holds slot i
                                              as W — the producer — and read
                                              by the consumer only for a slot
                                              it holds, after the exchange   */
    LONG            cell __attribute__((aligned(64)));   /* THE shared word  */
    /* producer only */
    unsigned        write, seq, need;
    DWORD           prodTid;
    /* consumer only */
    unsigned        read, prev, spare;
    uint32_t        frameHead, lastSeq;
    int             inFrame;
    DWORD           consTid;
    /* diagnostics: written by one side, read by the heartbeat on the other;
       aligned dwords, so a stale value is the worst a racy read can get   */
    volatile unsigned cPub, cSkip, cOverrun, cForeign, cGrow, cCommitFail, cTrunc, cRefill, cPViol;
    volatile unsigned cAcq, cTaken, cGap, cViol, cCrcBad, cNoPkt, cSameTick, cPaired;
    volatile unsigned hist[PK_HIST_N + 1];
    unsigned        histPrev[PK_HIST_N + 1];
    /* THE ONE UNRECOVERABLE STATE, AND IT IS A FAIL-STOP. Every index that can
       reach the cell comes from a chain rooted at this instance's own slot
       numbers, so an out-of-range one means the word was corrupted by a bug
       elsewhere in our code — and there is no recovery: the slot just handed
       back is in the cell AND still ours by name, which is the one thing the
       permutation exists to forbid, and the producer may fill it under us.
       The instance stops rather than guessing: nothing further is published or
       taken, the heartbeat says STOPPED, and the engine's own passes come
       back. Unreachable by construction; here so that a bug is loud and not
       a use-after-write. */
    int             fatal;
} PKX;

static PKX s_frame = { "packet" };
static PKX s_cmd   = { "cmd" };
static int s_armed, s_check, s_stress, s_poison;
static LARGE_INTEGER s_freq;
static tagpu_packet_extra_fn s_extra;
/* the render thread's own copy of the last command it posted, for the
   heartbeat's unacked delta (never a read of a slot it has handed over) */
static TAGPU_CMD s_lastPosted;

#define XCHG(m, v)  __atomic_exchange_n(&(m)->cell, (LONG)(v), __ATOMIC_ACQ_REL)
#define PEEK(m)     __atomic_load_n(&(m)->cell, __ATOMIC_RELAXED)

static void plog(const char* s)
{
    tagpu_log(s);
}

static int s_growStress;               /* tagpu_grow.stress (tagpu_packet.h) */

static int lever(const char* file)
{
    return GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES;
}

/* A violation: counted always, logged the first time and then every
   PK_LOG_EVERY, so a broken build cannot fill the disk with one line. */
static void violation(PKX* m, const char* what, unsigned a, unsigned b)
{
    unsigned n = ++m->cViol;
    if (n == 1 || (n % PK_LOG_EVERY) == 0) {
        char buf[240];
        _snprintf(buf, sizeof buf, "%s: VIOLATION #%u %s (%u, %u) read=%u prev=%u cell=%ld",
                  m->name, n, what, a, b, m->read, m->prev, (long)PEEK(m));
        buf[sizeof buf - 1] = 0;
        plog(buf);
    }
}

/* Commit pages to slot i so that it can hold `need` record bytes, plus the
   canary. Already-committed pages are untouched by MEM_COMMIT (their content
   survives); the canary moves to the new end. 1 when cap[i] >= need. */
static int slot_commit(PKX* m, unsigned i, unsigned need)
{
    unsigned grain = m->grain;
    unsigned want = need + 4u;
    if (want > m->reserve) want = m->reserve;
    want = (want + grain - 1u) & ~(grain - 1u);
    if (want > m->reserve) want = m->reserve;
    if (want <= m->cap[i] + 4u) return m->cap[i] >= need;
    if (!VirtualAlloc(m->slot[i], want, MEM_COMMIT, PAGE_READWRITE)) {
        m->cCommitFail++;
        return 0;
    }
    m->cap[i] = want - 4u;
    *(uint32_t*)(m->slot[i] + m->cap[i]) = PK_CANARY;
    return m->cap[i] >= need;
}

static int pkx_init(PKX* m, unsigned commit0)
{
    unsigned i;
    for (i = 0; i < m->nslots; i++) {
        unsigned char* b = (unsigned char*)VirtualAlloc(NULL, m->reserve, MEM_RESERVE, PAGE_NOACCESS);
        if (!b) return 0;
        m->slot[i] = b;
        m->cap[i] = 0;
        if (!slot_commit(m, i, commit0)) return 0;
        memset(b, 0, m->recBytes);                    /* head_seq 0: never held a record */
    }
    /* the initial permutation: {W, cell, READ, PREV[, SPARE]} = {0, 1, 2, 3[, 4]} */
    m->write = 0; m->read = 2; m->prev = 3; m->spare = (m->holds >= 3) ? 4 : 3;
    m->seq = 0; m->need = m->recBytes;
    __atomic_store_n(&m->cell, 1L, __ATOMIC_RELEASE);  /* slot 1, stale */
    return 1;
}

/* CRC-32 of the slot as published: every byte of [0, used) with the crc and
   tail fields — the last two of the record's fixed part, adjacent — taken as
   zero. The producer computes it with both fields actually zero (before the
   tail is stored); the consumer substitutes the zeros. */
static uint32_t pk_crc(const PKX* m, const void* rec)
{
    static const unsigned char zeros[8];
    const unsigned char* b = (const unsigned char*)rec;
    size_t off = m->recBytes - PK_SUFFIX;
    unsigned long c = Crc32_ComputeBuf(0, b, off);
    c = Crc32_ComputeBuf(c, zeros, 8);
    c = Crc32_ComputeBuf(c, b + off + 8, REC_USED(rec) - (off + 8));
    return (uint32_t)c;
}

/* ------------------------------------------------------- the producer ---- */

static unsigned us_of(const LARGE_INTEGER* t0, const LARGE_INTEGER* t1)
{
    LONGLONG d = t1->QuadPart - t0->QuadPart;
    if (!s_freq.QuadPart || d < 0) return 0;
    return (unsigned)((d * 1000000LL) / s_freq.QuadPart);
}

static int pkx_publish(PKX* m, pkx_fill_fn fill, void* ctx, int force)
{
    void* p;
    LARGE_INTEGER t0, t1;
    LONG old;
    unsigned w, need, us;
    DWORD tid = GetCurrentThreadId();

    if (!m->prodTid) m->prodTid = tid;                /* unregistered: latch */
    else if (tid != m->prodTid) {
        if (!m->prodByRole) { m->cForeign++; return 0; }
        /* by role (the render thread, which is restarted across a display-mode
           change, joined never killed): the new thread inherits W — recorded */
        {
            char b[120];
            _snprintf(b, sizeof b, "%s: producer thread %u -> %u (render thread restarted)",
                      m->name, (unsigned)m->prodTid, (unsigned)tid);
            b[sizeof b - 1] = 0;
            plog(b);
        }
        m->prodTid = tid;
    }

    /* THE FRESH GATE: a record the consumer has not taken is not replaced.
       The engine draws 330..4900 times a second against ~60 presents, so
       this one relaxed load is what most draws cost. `force` (the level-end
       packet; every command record — latest wins) and `stress` (the overrun
       path, on purpose) go past it. */
    if (m->fatal) return 0;
    if (!force && !s_stress && (PEEK(m) & PKX_FRESH)) { m->cSkip++; return 0; }

    w = m->write;
    /* a growth is counted by what it committed, not by whether it reached
       `need`: at the reserve slot_commit commits all of it and still says no */
    if (m->need > m->cap[w]) {
        unsigned was = m->cap[w];
        slot_commit(m, w, m->need);
        if (m->cap[w] != was) m->cGrow++;
    }
    p = m->slot[w];

    QueryPerformanceCounter(&t0);
    if (s_stress) tagpu_pk_fill(p, 0xA5, m->cap[w] < 16384u ? m->cap[w] : 16384u);
    REC_HEAD(p) = ++m->seq;                        /* head BEFORE the payload */
    REC_CAP(p)  = m->cap[w];
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    need = fill(p, ctx);                           /* every engine read is in there */
    /* A FILL THAT DID NOT FIT IS DONE AGAIN, NOT PUBLISHED, WHILE THE SLOT
       CAN GROW TO IT. Published, it is a frame with tables missing — at a
       level's start, when the world tables first outgrow a slot, a frame with
       no fog grid, no shade table and no units, which the native pass draws
       with no fog at all. The slot is still this side's alone (W, not yet
       exchanged), so committing more of its pages and filling it again is
       invisible to the consumer: the head stored above stays, and the tail is
       stored after the fill that is kept. The fill reports the size of the
       whole fill, not the end of the table that ran out (tagpu_packet_pub.c,
       s_fillShort), so one refill fits unless an input the render thread
       publishes (the fx, ghost and minimap wants) changed in between.

       THE LOOP KEYS ON WHAT slot_commit COMMITTED, NOT ON WHAT IT RETURNED.
       Past the reserve it commits the whole reserve and still returns 0
       (`cap < need`); the record must then carry the new capacity, because
       the consumer refuses one whose `cap_bytes` is not the slot's
       (pk_valid), and the fill must be done again at it, or the tables a
       smaller slot cut stay cut. So: no new pages, stop and publish what the
       last fill placed; new pages, record them and fill again; and a commit
       that did not reach `need` makes that fill the last. BOUNDED: every pass
       that fills again has grown `cap[w]` by at least the slot's grain (64 KB,
       one page under `stress`), and the reserve ends it. What is still over
       capacity after it (a commit that failed, a fill larger than the
       reserve) is published cut, and counted in `trunc`. */
    while (need > REC_CAP(p)) {
        unsigned was = m->cap[w];
        int fits;
        if (need > m->need) m->need = need;
        fits = slot_commit(m, w, need);
        if (m->cap[w] == was) break;
        m->cGrow++; m->cRefill++;
        REC_CAP(p) = m->cap[w];
        need = fill(p, ctx);
        if (!fits) break;
    }
    /* our own bounds on what the fill left — a misbehaving fill is a producer
       violation, and the record is still made valid for the consumer */
    if (REC_USED(p) < m->recBytes || REC_USED(p) > REC_CAP(p) || (REC_USED(p) & 3u)) {
        m->cPViol++;
        REC_USED(p) = m->recBytes;
        if (m == &s_frame) { ((TAGPU_PACKET*)p)->font_len = 0; ((TAGPU_PACKET*)p)->stress_len = 0; }
    }
    if (need > REC_CAP(p)) { m->cTrunc++; if (need > m->need) m->need = need; }
    REC_CRC(m, p) = 0; REC_TAIL(m, p) = 0;
    if (s_check) REC_CRC(m, p) = pk_crc(m, p);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    REC_TAIL(m, p) = REC_HEAD(p);                  /* tail AFTER the payload  */
    /* P1: the payload is visible before the index. P2: the next fill of the
       slot we receive stays after this exchange. Both from the ACQ_REL xchg. */
    old = XCHG(m, w | PKX_FRESH);
    if (old & PKX_FRESH) m->cOverrun++;            /* never taken: ours again, counted */
    if ((unsigned)old & PKX_RESERVED) m->cPViol++;
    m->write = (unsigned)old & PKX_IDX;
    QueryPerformanceCounter(&t1);
    us = us_of(&t0, &t1) / 2u;
    m->hist[us < PK_HIST_N ? us : PK_HIST_N]++;
    m->cPub++;
    return 1;
}

/* the frame packet's fill, in the producer's own type */
typedef struct { tagpu_packet_fill_fn fn; void* ctx; } FRAME_FILL;
static unsigned frame_fill(void* rec, void* ctx)
{
    FRAME_FILL* a = (FRAME_FILL*)ctx;
    return a->fn((TAGPU_PACKET*)rec, a->ctx);
}

int tagpu_packet_publish(tagpu_packet_fill_fn fill, void* ctx, int force)
{
    FRAME_FILL a;
    if (!s_armed || !fill) return 0;
    a.fn = fill; a.ctx = ctx;
    return pkx_publish(&s_frame, frame_fill, &a, force);
}

void tagpu_packet_producer(unsigned long tid) { s_frame.prodTid = (DWORD)tid; }

/* the command record's fill: the caller's record between the prefix and the
   suffix, through the volatile copy like every other payload store; its
   cmd_seq is the primitive's own publish counter, so the acknowledgement the
   packet echoes names a post exactly */
static unsigned cmd_fill(void* rec, void* ctx)
{
    const TAGPU_CMD* c = (const TAGPU_CMD*)ctx;
    TAGPU_CMD* d = (TAGPU_CMD*)rec;
    tagpu_pk_copy((unsigned char*)d + PK_PREFIX, (const unsigned char*)c + PK_PREFIX,
                  sizeof(TAGPU_CMD) - PK_PREFIX - PK_SUFFIX);
    d->cmd_seq    = d->head_seq;
    d->used_bytes = sizeof(TAGPU_CMD);
    return sizeof(TAGPU_CMD);
}

int tagpu_cmd_post(const TAGPU_CMD* c)
{
    int ok;
    if (!s_armed || !c) return 0;
    ok = pkx_publish(&s_cmd, cmd_fill, (void*)c, 1 /* latest wins */);
    if (ok) { s_lastPosted = *c; s_lastPosted.cmd_seq = s_cmd.seq; }
    return ok;
}

/* ------------------------------------------------------- the consumer ---- */

/* One area inside the record: 4-aligned, after the header, and inside the
   bytes the fill said it used. Written once because six things need it. */
static int area_ok(const TAGPU_PACKET* p, unsigned off, unsigned len)
{
    if (!len) return 1;
    if (off & 3u) return 0;
    if (off < sizeof(TAGPU_PACKET) || off > p->used_bytes) return 0;
    return len <= p->used_bytes - off;
}
static int table_ok(const TAGPU_PACKET* p, unsigned off, unsigned n, unsigned stride)
{
    if (!n) return 1;
    if (n > 0x100000u) return 0;                 /* a count is a loop bound      */
    return area_ok(p, off, n * stride);
}
/* one unit's or wreck's piece run, inside the PIECES TABLE rather than merely
   inside the record: a run that pointed at the units table would read a unit's
   bytes as a pose */
static int run_ok(const TAGPU_PACKET* p, unsigned off, unsigned n)
{
    unsigned base = p->off_pieces, len = p->n_pieces * (unsigned)sizeof(TAGPU_PK_PIECE);
    if (!p->n_pieces) return 0;
    if (off < base || (off - base) % sizeof(TAGPU_PK_PIECE)) return 0;
    if (off - base > len) return 0;
    return n * (unsigned)sizeof(TAGPU_PK_PIECE) <= len - (off - base);
}

/* The frame packet's own bounds, after the structural ones below. */
static const char* frame_valid(const void* rec)
{
    const TAGPU_PACKET* p = (const TAGPU_PACKET*)rec;
    unsigned i;
    if (p->in_game > 1u) return "in_game";
    if (p->pal_ok > 1u) return "pal_ok";
    if (p->font_len) {
        if (p->font_len > TAGPU_PK_FONT_MAX || (p->font_off & 3u) ||
            p->font_off < sizeof(TAGPU_PACKET) || p->font_off > p->used_bytes ||
            p->font_len > p->used_bytes - p->font_off)
            return "font area";
        if (p->font_rows == 0) return "font rows";
        for (i = 0; i < TAGPU_PK_NGLYPH; i++) {
            const TAGPU_PK_GLYPH* g = &p->font_glyph[i];
            unsigned len;
            if (!g->w) continue;
            len = TAGPU_PK_GLYPH_HDR + (((unsigned)p->font_rows * g->w + 7u) >> 3);
            if (g->off > p->font_len || len > p->font_len - g->off) return "glyph past the font area";
        }
    }
    if (p->stress_len) {
        if ((p->stress_off & 3u) || p->stress_off < sizeof(TAGPU_PACKET) ||
            p->stress_off > p->used_bytes || p->stress_len > p->used_bytes - p->stress_off)
            return "stress area";
    }
    if (p->shd_len) {
        if (p->shd_len != TAGPU_PK_SHD_BYTES || !area_ok(p, p->shd_off, p->shd_len))
            return "shade table area";
    }
    /* THE FOUR WORLD TABLES. Each is checked ONCE, here, against
       the record's own committed extent, so every consumer indexes with its
       `n_` and nothing else. `area_ok` does the 4-alignment, the "starts after
       the header" and the "off + len does not wrap and fits in used_bytes"
       in one place; the counts are then bounded against what the engine's own
       fields allow, because a count is what a loop runs to. */
    if (!table_ok(p, p->off_units, p->n_units, sizeof(TAGPU_PK_UNIT))) return "units table";
    if (!table_ok(p, p->off_pieces, p->n_pieces, sizeof(TAGPU_PK_PIECE))) return "pieces table";
    if (!table_ok(p, p->off_wrecks, p->n_wrecks, sizeof(TAGPU_PK_WRECK))) return "wrecks table";
    if (!table_ok(p, p->off_anchors, p->n_anchors, sizeof(TAGPU_PK_ANCHOR))) return "anchors table";
    /* the build-orders table (the build ghost). Its consumer walks it with the
       raw pointer and no cap of its own, which is this line's whole point:
       every table a consumer indexes by `n_` alone is checked HERE. */
    if (!table_ok(p, p->off_builds, p->n_builds, sizeof(TAGPU_PK_BUILD))) return "builds table";
    if (p->n_units && p->unit_slots && p->n_units > p->unit_slots) return "more units than slots";
    /* the publisher's own table caps, which the render thread sizes its
       per-frame arrays from -- a count past them is not a packet it wrote */
    if (p->n_units > TAGPU_PK_MAX_UNITS) return "more units than the table holds";
    if (p->n_wrecks > TAGPU_PK_MAX_WRECKS) return "more wrecks than the table holds";
    if (p->n_builds > TAGPU_PK_MAX_BUILDS) return "more builds than slots";
    if (p->n_anchors && p->anch_cols > 0 && p->anch_rows > 0 &&
        p->n_anchors > (unsigned)p->anch_cols * (unsigned)p->anch_rows) return "more anchors than cells";
    if (p->anch_cols < 0 || p->anch_rows < 0 ||
        p->anch_cols > 0x10000 || p->anch_rows > 0x10000) return "anchor rect";
    /* Every piece run inside the pieces area, every cargo link a packet index,
       every model id inside the model table's own length: the three indices a
       consumer follows without a second thought. One pass over the units, at
       ~100 bytes each — 28 us for 281 units, and it is what lets every pass
       drop its own filters. */
    {
        const TAGPU_PK_UNIT* u = tagpu_pk_units(p);
        const TAGPU_PK_WRECK* w = tagpu_pk_wrecks(p);
        unsigned k;
        for (k = 0; k < p->n_units; k++) {
            if (u[k].piece_n) {
                if (u[k].piece_n > TAGPU_PK_MAXPIECE) return "unit piece count";
                if (!run_ok(p, u[k].piece_off, u[k].piece_n)) return "unit piece run";
            }
            if (u[k].model_id && p->udef_count && u[k].model_id >= p->udef_count) return "unit model id";
            if (u[k].cargo_first >= 0 && (unsigned)u[k].cargo_first >= p->n_units) return "cargo_first";
            if (u[k].cargo_next  >= 0 && (unsigned)u[k].cargo_next  >= p->n_units) return "cargo_next";
            if (u[k].base_piece != 0xFFFFu && u[k].base_piece >= u[k].nparts) return "base piece";
        }
        for (k = 0; k < p->n_wrecks; k++) {
            if (w[k].piece_n) {
                if (w[k].piece_n > TAGPU_PK_MAXPIECE) return "wreck piece count";
                if (!run_ok(p, w[k].piece_off, w[k].piece_n)) return "wreck piece run";
            }
            if (w[k].base_piece != 0xFFFFu && w[k].base_piece >= w[k].nparts) return "wreck base piece";
        }
    }
    /* THE FOUR EFFECT TABLES. Each count is bounded by what the
       ENGINE's own array allows — 300 projectile slots, 300 explosion records,
       100 debris slots — so a consumer's loop can never run past the array the
       publisher walked even if the record were corrupt. The particle table's
       cap is ours, and its per-layer counts have to add up to it or the
       consumer's layer walk would read another layer's entries. */
    if (!table_ok(p, p->off_proj,   p->n_proj,   sizeof(TAGPU_PK_PROJ)))   return "projectiles table";
    if (!table_ok(p, p->off_expl,   p->n_expl,   sizeof(TAGPU_PK_EXPL)))   return "explosions table";
    if (!table_ok(p, p->off_debris, p->n_debris, sizeof(TAGPU_PK_DEBRIS))) return "debris table";
    if (!table_ok(p, p->off_part,   p->n_part,   sizeof(TAGPU_PK_PART)))   return "particles table";
    if (p->n_proj   > TAGPU_PK_MAX_PROJ)   return "more projectiles than slots";
    if (p->n_expl   > TAGPU_PK_MAX_EXPL)   return "more explosions than records";
    if (p->n_debris > TAGPU_PK_MAX_DEBRIS) return "more debris than slots";
    if (p->n_part   > TAGPU_PK_MAX_PART)   return "more particles than the cap";
    if (p->lht_len && (p->lht_len != TAGPU_PK_LHT_BYTES || !area_ok(p, p->lht_off, p->lht_len)))
        return "lighten table area";
    {
        unsigned k, sum = 0;
        for (k = 0; k < TAGPU_PK_NLAYER; k++) {
            if (p->part_n[k] > p->n_part) return "particle layer count";
            sum += p->part_n[k];
        }
        if (sum != p->n_part) return "particle layer counts do not sum";
    }
    /* THE PARTICLE TABLE'S TWO INDEX BYTES. `kind` indexes a consumer's
       six-row class table and `layer` a ten-row one; every index a consumer
       forms out of this packet is bounded here, these two included. */
    {
        const TAGPU_PK_PART* q = tagpu_pk_part(p);
        unsigned k;
        for (k = 0; k < p->n_part; k++) {
            if (q[k].kind >= TAGPU_PK_NPARTKIND) return "particle kind";
            if (q[k].layer >= TAGPU_PK_NLAYER) return "particle layer";
        }
    }
    /* THE TWO FOG GRIDS. The bound a consumer needs is not a cap
       on the dimensions — it is that the bytes it was given hold every index it
       can form. `len == cols * rows * 2`, checked here against the record's own
       extent, IS that bound: the largest index is cols*rows - 1 and the area is
       exactly cols*rows entries long. The dimension ceiling below is a sanity
       filter on two numbers read out of engine memory, not the safety
       argument. */
    if (p->fog_len || p->fog_cols || p->fog_rows) {
        if (p->fog_cols <= 0 || p->fog_rows <= 0 ||
            p->fog_cols > TAGPU_PK_FOG_DIMCAP || p->fog_rows > TAGPU_PK_FOG_DIMCAP)
            return "fog grid dims";
        if (p->fog_len != (unsigned)p->fog_cols * (unsigned)p->fog_rows * 2u ||
            !area_ok(p, p->fog_off, p->fog_len))
            return "fog grid area";
    }
    if (p->fogw_len || p->fogw_cols || p->fogw_rows) {
        if (p->fogw_cols <= 0 || p->fogw_rows <= 0 ||
            p->fogw_cols > TAGPU_PK_FOG_DIMCAP || p->fogw_rows > TAGPU_PK_FOG_DIMCAP)
            return "wide fog grid dims";
        if (p->fogw_len != (unsigned)p->fogw_cols * (unsigned)p->fogw_rows * 2u ||
            !area_ok(p, p->fogw_off, p->fogw_len))
            return "wide fog grid area";
    }
    /* THE UI's TWO MINIMAP AREAS, bounded the same way the fog grids
       are: the length has to be exactly what the dimensions describe, so the
       largest index a consumer can form is inside the bytes it was handed. */
    if (p->mm_len || p->mm_w || p->mm_h) {
        if (p->mm_w <= 0 || p->mm_h <= 0 ||
            p->mm_w > TAGPU_PK_MM_DIMCAP || p->mm_h > TAGPU_PK_MM_DIMCAP)
            return "minimap surface dims";
        if (p->mm_len != (unsigned)p->mm_w * (unsigned)p->mm_h * 3u ||
            !area_ok(p, p->mm_off, p->mm_len))
            return "minimap surface area";
    }
    if (p->mmpic_len || p->mmpic_w || p->mmpic_h) {
        if (p->mmpic_w <= 0 || p->mmpic_h <= 0 ||
            p->mmpic_w > TAGPU_PK_MM_DIMCAP || p->mmpic_h > TAGPU_PK_MM_DIMCAP)
            return "minimap picture dims";
        if (p->mmpic_len != (unsigned)p->mmpic_w * (unsigned)p->mmpic_h ||
            !area_ok(p, p->mmpic_off, p->mmpic_len))
            return "minimap picture area";
    }
    if (p->mm_live > 1u) return "mm_live";
    return NULL;
}

/* `prev` is handed out only when it is from the same level, in game, and of a
   DIFFERENT tick — the pair the pose blend runs over has to span two ticks or
   the weight is meaningless. The rotation above is what makes that the common
   case; this is the gate that makes it a guarantee, so a consumer never has to
   test it and a blend can never divide by a zero tick span. */
static int frame_pair(const void* rec, const void* prev)
{
    const TAGPU_PACKET* p = (const TAGPU_PACKET*)rec;
    const TAGPU_PACKET* q = (const TAGPU_PACKET*)prev;
    return q->in_game && p->in_game && q->level_gen == p->level_gen && q->tick != p->tick;
}

/* the record's tick, for the rotation: the frame packet has one, the command
   record has none (it is a level, not a sample, and it never hands out a prev) */
static unsigned frame_tick(const void* rec) { return ((const TAGPU_PACKET*)rec)->tick; }

/* The command record's bounds: a level the game thread will apply as a
   float must be a number in the lever's own band; the hold is clamped by
   the game thread into the camera's range, so it needs only to be finite. */
static const char* cmd_valid(const void* rec)
{
    const TAGPU_CMD* c = (const TAGPU_CMD*)rec;
    if (c->used_bytes != sizeof(TAGPU_CMD)) return "cmd size";
    if (!(c->zoom >= 0.05f && c->zoom <= 16.0f)) return "cmd zoom";
    if (c->live > 1u || c->hold_on > 1u || c->drop_follow > 1u) return "cmd flags";
    /* the hold is clamped at its source (tagpu_input.c) so that a wild number
       in the file cannot refuse the whole record and with it every other
       command; this bound is the backstop, not the gate */
    if (c->hold_x < -0x1000000 || c->hold_x > 0x1000000 ||
        c->hold_y < -0x1000000 || c->hold_y > 0x1000000) return "cmd hold";
    return NULL;
}

/* The structural bounds, on a slot the consumer holds. `cap` is the
   committed capacity the producer recorded for this slot before handing it
   over — read from our own table, never from the record, because a record
   whose header is garbage would otherwise send this probe to an uncommitted
   page. Every offset and length in the header is checked against it. */
static const char* pk_valid(const PKX* m, const void* rec, unsigned cap)
{
    if (REC_HEAD(rec) != REC_TAIL(m, rec)) return "head != tail";
    if (REC_CAP(rec) != cap) return "cap_bytes != the slot's capacity";
    if (cap < m->recBytes || cap > m->reserve - 4u) return "cap out of range";
    if (*(const uint32_t*)((const unsigned char*)rec + cap) != PK_CANARY) return "canary";
    if (REC_USED(rec) < m->recBytes || REC_USED(rec) > cap || (REC_USED(rec) & 3u))
        return "used_bytes";
    return m->valid ? m->valid(rec) : NULL;
}

static const void* pkx_acquire(PKX* m, const void** prev)
{
    const void* p;
    const char* bad;
    DWORD tid = GetCurrentThreadId();

    if (prev) *prev = NULL;
    if (m->fatal) return NULL;
    /* the render thread is restarted across a display-mode change (joined,
       never killed, after its last SwapBuffers): ownership is by role, so the
       new thread simply inherits READ/PREV; the id is recorded, not asserted */
    if (m->consTid != tid) {
        if (m->consTid) {
            char b[120];
            _snprintf(b, sizeof b, "%s: consumer thread %u -> %u (thread restarted)",
                      m->name, (unsigned)m->consTid, (unsigned)tid);
            b[sizeof b - 1] = 0;
            plog(b);
        }
        m->consTid = tid;
    }
    if (m->inFrame) violation(m, "acquire twice in one frame", m->cAcq, 0);
    m->inFrame = 1;
    m->cAcq++;

    if (PEEK(m) & PKX_FRESH) {
        /* WHAT GOES BACK IS THE OLDEST SLOT THIS THREAD HOLDS, and with two
           held slots that is PREV — there is no spare to keep, and the
           rotation below has nothing to choose. (`spare` is the third slot's
           name and is only maintained when there is a third: an instance with
           two that handed `spare` back would give the same index every time,
           because nothing would advance it.) */
        unsigned give = (m->holds >= 3) ? m->spare : m->prev, got;
        LONG old;
        /* C2: our last loads of `give` were at least a frame ago — with three
           held slots, the frame BEFORE the one in which it became the spare —
           and they precede this exchange. The poison makes a pointer kept past
           its frame loud. */
        if (s_poison) tagpu_pk_fill(m->slot[give], 0xDD, m->recBytes);
        old = XCHG(m, give);
        got = (unsigned)old & PKX_IDX;
        if (!((unsigned)old & PKX_FRESH)) violation(m, "FRESH vanished between peek and exchange", (unsigned)old, give);
        if ((unsigned)old & PKX_RESERVED) violation(m, "reserved bits set in the cell", (unsigned)old, 0);
        if (got >= m->nslots) {
            violation(m, "slot index out of range — the exchange is STOPPED", got, m->nslots);
            m->fatal = 1;
            m->inFrame = 0;
            m->frameHead = 0;
            return NULL;
        }
        if (got == give || got == m->read || (m->holds >= 3 && got == m->prev)) {
            /* A SECOND FAIL-STOP, and for the same reason as the one above.
               The cell held a slot THIS THREAD ALREADY HOLDS, so the producer
               wrote a slot it did not own: the partition that makes the whole
               exchange safe — producer's W, consumer's held set, the cell —
               is broken, and carrying on would leave our held set with a
               duplicate and one slot owned by nobody, i.e. a reader and the
               writer in the same bytes. Counting it and continuing would be a
               timing argument dressed as recovery. */
            violation(m, "permutation broken — the exchange is STOPPED", got, give);
            m->fatal = 1;
            m->inFrame = 0;
            m->frameHead = 0;
            return NULL;
        }
        /* THE ROTATION. Every slot named here is one this thread holds, and
           the tick it reads is out of a record no other thread writes (the
           producer's W is not among them), so the choice is a fact rather than
           a peek. It is made before the bounds check below on purpose: the
           check decides whether the record is DRAWN this frame, the rotation
           only which of our own slots is recycled next, and a record that
           fails its bounds is counted and refused either way. */
        if (m->holds >= 3) {
            if (m->tick && REC_HEAD(m->slot[m->read]) &&
                m->tick(m->slot[got]) == m->tick(m->slot[m->read])) {
                m->spare = m->read;          /* same tick: displace READ, keep PREV */
                m->cSameTick++;
            } else {
                m->spare = m->prev;          /* a new tick: age the pair by one    */
                m->prev  = m->read;
            }
            m->read = got;
        } else {
            m->prev = m->read;
            m->read = got;
        }
        m->cTaken++;
        /* C1: our loads of the new slot follow the exchange (and depend on
           its result). Under stress, hold the slot for a while first. */
        if (s_stress && m->consSleep) Sleep((DWORD)(rand() % 51));
    }
    p = m->slot[m->read];
    if (!REC_HEAD(p)) { m->cNoPkt++; m->frameHead = 0; return NULL; }
    bad = pk_valid(m, p, m->cap[m->read]);
    if (bad) {
        violation(m, bad, REC_HEAD(p), REC_USED(p));
        m->frameHead = 0;
        return NULL;
    }
    if (m->lastSeq) {
        if (REC_HEAD(p) > m->lastSeq + 1u) m->cGap += REC_HEAD(p) - m->lastSeq - 1u;
        else if (REC_HEAD(p) < m->lastSeq) violation(m, "head_seq went backwards", REC_HEAD(p), m->lastSeq);
    }
    m->lastSeq = REC_HEAD(p);
    m->frameHead = REC_HEAD(p);                    /* latched for frame_end   */
    if (prev && m->pair) {
        const void* q = m->slot[m->prev];
        if (q != p && REC_HEAD(q) && m->pair(p, q) && !pk_valid(m, q, m->cap[m->prev])) {
            *prev = q;
            m->cPaired++;
        }
    }
    return p;
}

const TAGPU_PACKET* tagpu_packet_acquire(const TAGPU_PACKET** prev)
{
    const void* q = NULL;
    const void* p;
    if (!s_armed) { if (prev) *prev = NULL; return NULL; }
    p = pkx_acquire(&s_frame, prev ? &q : NULL);
    if (prev) *prev = (const TAGPU_PACKET*)q;
    return (const TAGPU_PACKET*)p;
}

const TAGPU_CMD* tagpu_cmd_take(void)
{
    if (!s_armed) return NULL;
    return (const TAGPU_CMD*)pkx_acquire(&s_cmd, NULL);
}

static void heartbeat(PKX* m, unsigned fc)
{
    static unsigned last, lastPub, lastTick, lastTaken;
    static LARGE_INTEGER lastQpc;
    static int have;
    LARGE_INTEGER now;
    double secs = 0.0;
    unsigned i, total = 0, acc = 0, p50 = 0, p99 = 0, pubs, taken;
    const TAGPU_PACKET* p = m->frameHead ? (const TAGPU_PACKET*)m->slot[m->read] : NULL;
    char b[1700];
    int n;

    if (have && fc - last < PK_HEARTBEAT) return;
    QueryPerformanceCounter(&now);
    if (have && s_freq.QuadPart)
        secs = (double)(now.QuadPart - lastQpc.QuadPart) / (double)s_freq.QuadPart;
    /* the publish-time histogram over the interval, 2 us per bucket */
    for (i = 0; i <= PK_HIST_N; i++) { unsigned h = m->hist[i]; total += h - m->histPrev[i]; }
    for (i = 0; i <= PK_HIST_N; i++) {
        unsigned h = m->hist[i], d = h - m->histPrev[i];
        m->histPrev[i] = h;
        acc += d;
        if (!p50 && total && acc * 2u >= total) p50 = i * 2u;
        if (!p99 && total && acc * 100u >= total * 99u) p99 = i * 2u;
    }
    pubs = m->cPub; taken = m->cTaken;
    n = _snprintf(b, sizeof b,
                  "packet:%s pub=%u skip=%u overrun=%u foreign=%u acq=%u taken=%u gap=%u grow=%u commitfail=%u trunc=%u refill=%u viol=%u pviol=%u crcbad=%u nopkt=%u"
                  " | pub/s=%.0f taken/s=%.1f pubus p50=%u p99=%s%u",
                  m->fatal ? " STOPPED" : "",
                  m->cPub, m->cSkip, m->cOverrun, m->cForeign, m->cAcq, m->cTaken, m->cGap,
                  m->cGrow, m->cCommitFail, m->cTrunc, m->cRefill, m->cViol, m->cPViol, m->cCrcBad, m->cNoPkt,
                  secs > 0.0 ? (double)(pubs - lastPub) / secs : 0.0,
                  secs > 0.0 ? (double)(taken - lastTaken) / secs : 0.0,
                  p50, p99 >= PK_HIST_N * 2u ? ">" : "", p99);
    if (n < 0 || n >= (int)sizeof b) n = (int)sizeof b - 1;
    if (p) {
        double tps = (have && secs > 0.0 && p->tick >= lastTick) ? (double)(p->tick - lastTick) / secs : 0.0;
        int k = _snprintf(b + n, sizeof b - (size_t)n,
                          " | seq=%u tick=%u tps=%.2f speed=%d paused=%u in_game=%u gen=%u flags=0x%04X eye=(%d,%d) vp=(%d,%d,%d,%d) addr=(%d,%d,%d,%d) z=%.3f pal=%u gamma=%.3f flips=%u font=%u/%uB fg=%d trunc=%u used=%u/%u"
                          " | units=%u pieces=%u wrecks=%u anchors=%u(%dx%d) dup=%u pair=%u same=%u",
                          p->head_seq, p->tick, tps, p->game_speed, (unsigned)p->paused, p->in_game, p->level_gen,
                          (unsigned)p->load_flags, p->eye[0], p->eye[1], p->vp[0], p->vp[1], p->vp[2], p->vp[3],
                          p->vp_addr[0], p->vp_addr[1], p->vp_addr[2], p->vp_addr[3], p->zoom_applied, p->pal_ok, p->gamma,
                          p->gui_flips, p->font_gen, p->font_len, p->text_fg, p->truncated, p->used_bytes, p->cap_bytes,
                          p->n_units, p->n_pieces, p->n_wrecks, p->n_anchors, p->anch_cols, p->anch_rows,
                          p->unit_dup, m->cPaired, m->cSameTick);
        if (k < 0 || n + k >= (int)sizeof b) n = (int)sizeof b - 1; else n += k;
        lastTick = p->tick;
    } else {
        int k = _snprintf(b + n, sizeof b - (size_t)n, " | no packet held");
        if (k < 0 || n + k >= (int)sizeof b) n = (int)sizeof b - 1; else n += k;
    }
    /* the other direction: posts and takes, and how far the render thread's
       cumulative delta is ahead of what the last packet acknowledged. The
       consumer-side counters are the game thread's; a stale dword is the
       worst this read can get. */
    {
        int k = _snprintf(b + n, sizeof b - (size_t)n,
                          " | cmd: post=%u take=%u new=%u overrun=%u viol=%u nocmd=%u seq=%u ack=%u unacked=(%d,%d) cum=(%d,%d) epoch=%u/%u z=%.3f live=%u hold=%u",
                          s_cmd.cPub, s_cmd.cAcq, s_cmd.cTaken, s_cmd.cOverrun, s_cmd.cViol, s_cmd.cNoPkt,
                          s_lastPosted.cmd_seq, p ? p->cmd_ack_seq : 0u,
                          p ? s_lastPosted.cum_dx - p->cmd_ack_dx : 0, p ? s_lastPosted.cum_dy - p->cmd_ack_dy : 0,
                          s_lastPosted.cum_dx, s_lastPosted.cum_dy, s_lastPosted.epoch, p ? p->cmd_epoch : 0u,
                          s_lastPosted.zoom, s_lastPosted.live, s_lastPosted.hold_on);
        if (k < 0 || n + k >= (int)sizeof b) n = (int)sizeof b - 1; else n += k;
    }
    if (s_extra && n < (int)sizeof b - 1) s_extra(b + n, (unsigned)(sizeof b - (size_t)n), secs);
    b[sizeof b - 1] = 0;
    plog(b);
    last = fc; lastQpc = now; lastPub = pubs; lastTaken = taken; have = 1;
}

static void pkx_frame_end(PKX* m, unsigned fc)
{
    if (m->fatal) { m->inFrame = 0; return; }
    if (GetCurrentThreadId() != m->consTid) violation(m, "frame_end on another thread", (unsigned)GetCurrentThreadId(), (unsigned)m->consTid);
    if (!m->inFrame) violation(m, "frame_end without acquire", fc, 0);
    m->inFrame = 0;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    if (m->frameHead) {
        const void* p = m->slot[m->read];
        if (REC_HEAD(p) != m->frameHead || REC_TAIL(m, p) != m->frameHead)
            violation(m, "our slot was rewritten during our frame", REC_HEAD(p), m->frameHead);
        else if (s_check && REC_CRC(m, p) && pk_crc(m, p) != REC_CRC(m, p)) {
            m->cCrcBad++;
            violation(m, "CRC mismatch", REC_CRC(m, p), pk_crc(m, p));
        }
    }
}

void tagpu_packet_frame_end(unsigned frame_counter)
{
    /* off: no slot to check, but the heartbeat still prints — the publisher's
       count-only observer feeds it draws/s, which is the cost A/B's other arm */
    if (!s_armed) { heartbeat(&s_frame, frame_counter); return; }
    pkx_frame_end(&s_frame, frame_counter);
    heartbeat(&s_frame, frame_counter);
}

void tagpu_cmd_done(void)
{
    if (!s_armed) return;
    pkx_frame_end(&s_cmd, 0);
}

void tagpu_packet_set_extra(tagpu_packet_extra_fn fn) { s_extra = fn; }

int tagpu_packet_armed(void) { return s_armed; }

int tagpu_grow_stress(void) { return s_growStress; }

/* ------------------------------------------------------------- attach ---- */

static void pkx_setup(PKX* m, const char* name, unsigned recBytes, unsigned reserve, unsigned grain,
                      unsigned holds, pkx_valid_fn valid, pkx_pair_fn pair, pkx_tick_fn tick,
                      int prodByRole, int consSleep)
{
    m->name = name; m->recBytes = recBytes; m->reserve = reserve; m->grain = grain;
    m->holds = holds; m->nslots = holds + 2;
    m->valid = valid; m->pair = pair; m->tick = tick;
    m->prodByRole = prodByRole; m->consSleep = consSleep;
}

void tagpu_packet_init(void)
{
    char b[300];
    s_check  = lever("tagpu_packet.check");
    s_stress = lever("tagpu_packet.stress");
    s_poison = lever("tagpu_packet.poison");
    s_growStress = lever("tagpu_grow.stress");
    if (s_growStress) plog("packet: tagpu_grow.stress - every unit-scaled render array moves every frame");
    QueryPerformanceFrequency(&s_freq);       /* the heartbeat's clock, armed or not */
    if (lever("tagpu_packet.off")) {
        plog("packet: disabled by tagpu_packet.off — no slots, nothing published, taken or applied: "
             "no world pass draws (every one reads the packet's view), every string through "
             "tagpu_text_place draws nothing, the zoom's commands are not applied (the engine keeps "
             "its own camera range, viewport rect and scroll rate); the DrawGameScreen observer stays "
             "in count-only mode so draws/s is still reported");
        return;
    }
    pkx_setup(&s_frame, "packet", sizeof(TAGPU_PACKET), PK_RESERVE, s_stress ? PK_PAGE : PK_GRAIN,
              3 /* READ, PREV, SPARE: the tick-aware rotation */,
              frame_valid, frame_pair, frame_tick, 0, 1);
    pkx_setup(&s_cmd, "cmd", sizeof(TAGPU_CMD), PK_CMD_RESERVE, PK_PAGE,
              2 /* no pair, no rotation */, cmd_valid, NULL, NULL, 1, 0);
    if (!pkx_init(&s_frame, s_stress ? PK_PAGE : PK_GRAIN) || !pkx_init(&s_cmd, PK_PAGE)) {
        plog("packet: NOT armed — could not reserve or commit the slots (nothing is "
             "published, taken or applied; the reservation stays as it is)");
        return;
    }
    s_armed = 1;
    _snprintf(b, sizeof b,
              "packet: ARMED %u slots x %u MB reserved, %u KB committed each, header %u B "
              "(unit %u B, piece %u B, wreck %u B, anchor %u B); "
              "commands: %u slots x %u KB, %u KB committed each, record %u B; "
              "W=0 cell=1(stale) READ=2 PREV=3 SPARE=4; check=%d stress=%d poison=%d "
              "(frame-packet-exchange, landing 3)",
              s_frame.nslots, PK_RESERVE >> 20, s_frame.cap[0] >> 10, (unsigned)sizeof(TAGPU_PACKET),
              (unsigned)sizeof(TAGPU_PK_UNIT), (unsigned)sizeof(TAGPU_PK_PIECE),
              (unsigned)sizeof(TAGPU_PK_WRECK), (unsigned)sizeof(TAGPU_PK_ANCHOR),
              s_cmd.nslots, PK_CMD_RESERVE >> 10, s_cmd.cap[0] >> 10, (unsigned)sizeof(TAGPU_CMD),
              s_check, s_stress, s_poison);
    b[sizeof b - 1] = 0;
    plog(b);
}
