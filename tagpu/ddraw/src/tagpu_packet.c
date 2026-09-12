/* tagpu_packet.c — the frame packet exchange: the primitive (landing 1).
   Contract: tagpu_packet.h (consumer), tagpu_packet_pub.h (producer). Design
   and the four reviews it survived: research/notes/frame-packet-exchange.html.

   THE EXCHANGE. Four slots with roles, not owners fixed for life: W is the
   one the game thread is filling, READ and PREV the two the render thread
   holds (this frame's and the previous one's), and the fourth sits in the
   CELL — fresh (published, not yet taken) or stale (returned, waiting to be
   reused). The cell is ONE aligned 32-bit word in our static storage:
   `idx (2 bits) | FRESH (bit 2)`, the reserved bits asserted zero. Each side
   exchanges a slot it holds into the cell and takes whatever was there, so
   the four roles stay a permutation of the four slots without a lock — and
   the init below makes them one to begin with: zeroed statics would put both
   threads on slot 0.

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
   the tail at frame_end. Both written after the fill would be a blind check
   (the protocol review's finding): with them on either side, any fill that
   overlaps a consumer frame — the one thing the permutation forbids — is
   caught by the consumer that saw it, on the frame it saw it.

   NOTHING MOVES, NOTHING IS FREED. Each slot's address range is reserved
   once (VirtualAlloc MEM_RESERVE, 8 MB per frame slot against ~2.3 MB at the
   engine's worst case) and pages are COMMITTED as the high-water mark rises,
   by the producer, on the slot it holds as W. A fill that does not fit
   truncates this frame (a bit per table in the header) and the next publish
   grows the write slot first; a commit that fails keeps the current size and
   retries later, never pins it. So no block ever moves, a pointer into a
   slot can never dangle, and a rule-breaking cached pointer reaches a slot's
   current bytes and never freed memory. Reference counting was rejected: a
   count cannot see the raw pointer a module copies.

   WAIT-FREE, BOTH SIDES. Neither exchange can fail or block. A stuck render
   thread costs the game thread one relaxed load per draw (the FRESH gate
   skips the copy); a stuck game thread leaves the renderer redrawing its
   held packet, which is our memory. The sim never waits on the renderer.

   VIOLATIONS ARE LOUD, NEVER FATAL. Thread identity on both sides, the
   permutation after every exchange, head == tail and the structural bounds
   at acquire, a canary past the capacity, one acquire per frame, the tail at
   frame_end, the CRC under `check`. A packet that fails a bound is refused
   for the frame (NULL) and counted; `viol=0` on the heartbeat is landing 1's
   gate. The levers, all read once at DLL attach from the gamedir:
     tagpu_packet.off     no slots, no publish, no acquire; the observer stays
                          in count-only mode so draws/s is still reported
     tagpu_packet.check   CRC-32 of the slot in the tail, verified per frame
     tagpu_packet.stress  the producer publishes on EVERY in-play draw (the
                          overrun path) with a garbage pre-fill before the
                          real fill, from a one-page initial commit with a
                          dummy table that forces growth; the consumer sleeps
                          0..50 ms inside each take
     tagpu_packet.poison  the consumer memsets the header of the slot it
                          hands back, so a pointer cached across frames
                          reads 0xDD instead of plausible stale data

   INSTANTIABLE. Every piece of state lives in a PKX and the four operations
   take one; the frame packet is the one instance today (s_frame). The
   commands (landing 2) and the wide fog grid (landing 4b) are further
   instances of the same struct, so the proof above is written once. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "tagpu_packet.h"
#include "tagpu_packet_pub.h"
#include "crc32.h"

#define PK_SLOTS      4
#define PK_RESERVE    (8u << 20)           /* address space per slot          */
#define PK_GRAIN      (64u << 10)          /* commit granularity              */
#define PK_PAGE       4096u                /* ...under stress: one page        */
#define PK_CANARY     0xC0FFEE42u
#define PKX_IDX       0x3u                 /* two index bits                   */
#define PKX_FRESH     0x4u                 /* set by publish, cleared by acquire */
#define PKX_RESERVED  (~0x7u)
#define PK_HEARTBEAT  300
#define PK_HIST_N     256                  /* 2 us buckets to 512 us, + overflow */
#define PK_LOG_EVERY  64                   /* violations logged: the first, then every this many */

typedef struct PKX {
    const char*     name;
    unsigned char*  slot[PK_SLOTS];        /* reserved base, fixed for life   */
    unsigned        cap[PK_SLOTS];         /* committed bytes minus the canary;
                                              written by whoever holds slot i
                                              as W — the producer — and read
                                              by the consumer only for a slot
                                              it holds, after the exchange   */
    LONG            cell __attribute__((aligned(64)));   /* THE shared word  */
    /* game thread only */
    unsigned        write, seq, need;
    DWORD           prodTid;
    /* render thread only */
    unsigned        read, prev;
    uint32_t        frameHead, lastSeq;
    int             inFrame;
    DWORD           consTid;
    /* diagnostics: written by one side, read by the heartbeat on the other;
       aligned dwords, so a stale value is the worst a racy read can get   */
    volatile unsigned cPub, cSkip, cOverrun, cForeign, cGrow, cCommitFail, cTrunc, cPViol;
    volatile unsigned cAcq, cTaken, cGap, cViol, cCrcBad, cNoPkt;
    volatile unsigned hist[PK_HIST_N + 1];
    unsigned        histPrev[PK_HIST_N + 1];
    unsigned        cViolLogged;
} PKX;

static PKX s_frame = { "packet" };
static int s_armed, s_check, s_stress, s_poison;
static LARGE_INTEGER s_freq;
static tagpu_packet_extra_fn s_extra;

#define XCHG(m, v)  __atomic_exchange_n(&(m)->cell, (LONG)(v), __ATOMIC_ACQ_REL)
#define PEEK(m)     __atomic_load_n(&(m)->cell, __ATOMIC_RELAXED)

static void plog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

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

/* Commit pages to slot i so that it can hold `need` packet bytes, plus the
   canary. Already-committed pages are untouched by MEM_COMMIT (their content
   survives); the canary moves to the new end. 1 when cap[i] >= need. */
static int slot_commit(PKX* m, unsigned i, unsigned need)
{
    unsigned grain = s_stress ? PK_PAGE : PK_GRAIN;
    unsigned want = need + 4u;
    if (want > PK_RESERVE) want = PK_RESERVE;
    want = (want + grain - 1u) & ~(grain - 1u);
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
    for (i = 0; i < PK_SLOTS; i++) {
        unsigned char* b = (unsigned char*)VirtualAlloc(NULL, PK_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
        if (!b) return 0;
        m->slot[i] = b;
        m->cap[i] = 0;
        if (!slot_commit(m, i, commit0)) return 0;
        memset(b, 0, sizeof(TAGPU_PACKET));           /* head_seq 0: never held a packet */
    }
    /* the initial permutation: {W, cell, READ, PREV} = {0, 1, 2, 3} */
    m->write = 0; m->read = 2; m->prev = 3;
    m->seq = 0; m->need = sizeof(TAGPU_PACKET);
    __atomic_store_n(&m->cell, 1L, __ATOMIC_RELEASE);  /* slot 1, stale */
    return 1;
}

/* CRC-32 of the slot as published: every byte of [0, used) with the crc and
   tail fields — the last two of the header, adjacent — taken as zero. The
   producer computes it with both fields actually zero (before the tail is
   stored); the consumer substitutes the zeros. */
static uint32_t pk_crc(const TAGPU_PACKET* p)
{
    static const unsigned char zeros[8];
    const unsigned char* b = (const unsigned char*)p;
    size_t off = offsetof(TAGPU_PACKET, crc);
    unsigned long c = Crc32_ComputeBuf(0, b, off);
    c = Crc32_ComputeBuf(c, zeros, 8);
    c = Crc32_ComputeBuf(c, b + off + 8, p->used_bytes - (off + 8));
    return (uint32_t)c;
}

/* ------------------------------------------------------- the producer ---- */

static unsigned us_of(const LARGE_INTEGER* t0, const LARGE_INTEGER* t1)
{
    LONGLONG d = t1->QuadPart - t0->QuadPart;
    if (!s_freq.QuadPart || d < 0) return 0;
    return (unsigned)((d * 1000000LL) / s_freq.QuadPart);
}

static int pkx_publish(PKX* m, tagpu_packet_fill_fn fill, void* ctx, int force)
{
    TAGPU_PACKET* p;
    LARGE_INTEGER t0, t1;
    LONG old;
    unsigned w, need, us;
    DWORD tid = GetCurrentThreadId();

    if (!m->prodTid) m->prodTid = tid;                /* unregistered: latch (a bare instance) */
    else if (tid != m->prodTid) { m->cForeign++; return 0; }

    /* THE FRESH GATE: a packet the renderer has not taken is not replaced.
       The engine draws 330..4900 times a second against ~60 presents, so
       this one relaxed load is what most draws cost. `force` (the level-end
       packet) and `stress` (the overrun path, on purpose) go past it. */
    if (!force && !s_stress && (PEEK(m) & PKX_FRESH)) { m->cSkip++; return 0; }

    w = m->write;
    if (m->need > m->cap[w] && slot_commit(m, w, m->need)) m->cGrow++;
    p = (TAGPU_PACKET*)m->slot[w];

    QueryPerformanceCounter(&t0);
    if (s_stress) tagpu_pk_fill(p, 0xA5, m->cap[w] < 16384u ? m->cap[w] : 16384u);
    p->head_seq  = ++m->seq;                       /* head BEFORE the payload */
    p->cap_bytes = m->cap[w];
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    need = fill(p, ctx);                           /* every engine read is in there */
    /* our own bounds on what the fill left — a misbehaving fill is a producer
       violation, and the packet is still made valid for the consumer */
    if (p->used_bytes < sizeof(TAGPU_PACKET) || p->used_bytes > p->cap_bytes || (p->used_bytes & 3u)) {
        m->cPViol++;
        p->used_bytes = sizeof(TAGPU_PACKET);
        p->font_len = 0; p->stress_len = 0;
    }
    if (need > p->cap_bytes) { m->cTrunc++; if (need > m->need) m->need = need; }
    p->crc = 0; p->tail_seq = 0;
    if (s_check) p->crc = pk_crc(p);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    p->tail_seq = p->head_seq;                     /* tail AFTER the payload  */
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

int tagpu_packet_publish(tagpu_packet_fill_fn fill, void* ctx, int force)
{
    if (!s_armed || !fill) return 0;
    return pkx_publish(&s_frame, fill, ctx, force);
}

void tagpu_packet_producer(unsigned long tid) { s_frame.prodTid = (DWORD)tid; }

/* ------------------------------------------------------- the consumer ---- */

/* The structural bounds, on a slot the consumer holds. `cap` is the
   committed capacity the producer recorded for this slot before handing it
   over — read from our own table, never from the packet, because a packet
   whose header is garbage would otherwise send this probe to an uncommitted
   page. Every offset and length in the header is checked against it. */
static const char* pk_valid(const TAGPU_PACKET* p, unsigned cap)
{
    unsigned i;
    if (p->head_seq != p->tail_seq) return "head != tail";
    if (p->cap_bytes != cap) return "cap_bytes != the slot's capacity";
    if (cap < sizeof(TAGPU_PACKET) || cap > PK_RESERVE - 4u) return "cap out of range";
    if (*(const uint32_t*)((const unsigned char*)p + cap) != PK_CANARY) return "canary";
    if (p->used_bytes < sizeof(TAGPU_PACKET) || p->used_bytes > cap || (p->used_bytes & 3u))
        return "used_bytes";
    if (p->in_game > 1u) return "in_game";
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
    return NULL;
}

static const TAGPU_PACKET* pkx_acquire(PKX* m, const TAGPU_PACKET** prev)
{
    const TAGPU_PACKET* p;
    const char* bad;
    DWORD tid = GetCurrentThreadId();

    if (prev) *prev = NULL;
    /* the render thread is restarted across a display-mode change (joined,
       never killed, after its last SwapBuffers): ownership is by role, so the
       new thread simply inherits READ/PREV; the id is recorded, not asserted */
    if (m->consTid != tid) {
        if (m->consTid) {
            char b[120];
            _snprintf(b, sizeof b, "%s: consumer thread %u -> %u (render thread restarted)",
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
        unsigned give = m->prev, got;
        LONG old;
        /* C2: our last loads of `give` were last frame's, before this
           exchange. The poison makes a pointer kept past its frame loud. */
        if (s_poison) tagpu_pk_fill(m->slot[give], 0xDD, sizeof(TAGPU_PACKET));
        old = XCHG(m, give);
        got = (unsigned)old & PKX_IDX;
        if (!((unsigned)old & PKX_FRESH)) violation(m, "FRESH vanished between peek and exchange", (unsigned)old, give);
        if ((unsigned)old & PKX_RESERVED) violation(m, "reserved bits set in the cell", (unsigned)old, 0);
        if (got == give || got == m->read) violation(m, "permutation broken", got, give);
        m->prev = m->read;
        m->read = got;
        m->cTaken++;
        /* C1: our loads of the new slot follow the exchange (and depend on
           its result). Under stress, hold the slot for a while first. */
        if (s_stress) Sleep((DWORD)(rand() % 51));
    }
    p = (const TAGPU_PACKET*)m->slot[m->read];
    if (!p->head_seq) { m->cNoPkt++; m->frameHead = 0; return NULL; }
    bad = pk_valid(p, m->cap[m->read]);
    if (bad) {
        violation(m, bad, p->head_seq, p->used_bytes);
        m->frameHead = 0;
        return NULL;
    }
    if (m->lastSeq) {
        if (p->head_seq > m->lastSeq + 1u) m->cGap += p->head_seq - m->lastSeq - 1u;
        else if (p->head_seq < m->lastSeq) violation(m, "head_seq went backwards", p->head_seq, m->lastSeq);
    }
    m->lastSeq = p->head_seq;
    m->frameHead = p->head_seq;                    /* latched for frame_end   */
    if (prev) {
        const TAGPU_PACKET* q = (const TAGPU_PACKET*)m->slot[m->prev];
        if (q != p && q->head_seq && q->in_game && p->in_game && q->level_gen == p->level_gen &&
            !pk_valid(q, m->cap[m->prev]))
            *prev = q;
    }
    return p;
}

const TAGPU_PACKET* tagpu_packet_acquire(const TAGPU_PACKET** prev)
{
    if (!s_armed) { if (prev) *prev = NULL; return NULL; }
    return pkx_acquire(&s_frame, prev);
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
    char b[640];
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
                  "packet: pub=%u skip=%u overrun=%u foreign=%u acq=%u taken=%u gap=%u grow=%u commitfail=%u trunc=%u viol=%u pviol=%u crcbad=%u nopkt=%u"
                  " | pub/s=%.0f taken/s=%.1f pubus p50=%u p99=%s%u",
                  m->cPub, m->cSkip, m->cOverrun, m->cForeign, m->cAcq, m->cTaken, m->cGap,
                  m->cGrow, m->cCommitFail, m->cTrunc, m->cViol, m->cPViol, m->cCrcBad, m->cNoPkt,
                  secs > 0.0 ? (double)(pubs - lastPub) / secs : 0.0,
                  secs > 0.0 ? (double)(taken - lastTaken) / secs : 0.0,
                  p50, p99 >= PK_HIST_N * 2u ? ">" : "", p99);
    if (n < 0 || n >= (int)sizeof b) n = (int)sizeof b - 1;
    if (p) {
        double tps = (have && secs > 0.0 && p->tick >= lastTick) ? (double)(p->tick - lastTick) / secs : 0.0;
        int k = _snprintf(b + n, sizeof b - (size_t)n,
                          " | seq=%u tick=%u tps=%.2f speed=%d paused=%u in_game=%u gen=%u flags=0x%04X eye=(%d,%d) vp=(%d,%d,%d,%d) flips=%u font=%u/%uB fg=%d trunc=%u used=%u/%u",
                          p->head_seq, p->tick, tps, p->game_speed, (unsigned)p->paused, p->in_game, p->level_gen,
                          (unsigned)p->load_flags, p->eye[0], p->eye[1], p->vp[0], p->vp[1], p->vp[2], p->vp[3], p->gui_flips,
                          p->font_gen, p->font_len, p->text_fg, p->truncated, p->used_bytes, p->cap_bytes);
        if (k < 0 || n + k >= (int)sizeof b) n = (int)sizeof b - 1; else n += k;
        lastTick = p->tick;
    } else {
        int k = _snprintf(b + n, sizeof b - (size_t)n, " | no packet held");
        if (k < 0 || n + k >= (int)sizeof b) n = (int)sizeof b - 1; else n += k;
    }
    if (s_extra && n < (int)sizeof b - 1) s_extra(b + n, (unsigned)(sizeof b - (size_t)n), secs);
    b[sizeof b - 1] = 0;
    plog(b);
    last = fc; lastQpc = now; lastPub = pubs; lastTaken = taken; have = 1;
}

static void pkx_frame_end(PKX* m, unsigned fc)
{
    if (GetCurrentThreadId() != m->consTid) violation(m, "frame_end on another thread", (unsigned)GetCurrentThreadId(), (unsigned)m->consTid);
    if (!m->inFrame) violation(m, "frame_end without acquire", fc, 0);
    m->inFrame = 0;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    if (m->frameHead) {
        const TAGPU_PACKET* p = (const TAGPU_PACKET*)m->slot[m->read];
        if (p->head_seq != m->frameHead || p->tail_seq != m->frameHead)
            violation(m, "our slot was rewritten during our frame", p->head_seq, m->frameHead);
        else if (s_check && p->crc && pk_crc(p) != p->crc) {
            m->cCrcBad++;
            violation(m, "CRC mismatch", p->crc, pk_crc(p));
        }
    }
    heartbeat(m, fc);
}

void tagpu_packet_frame_end(unsigned frame_counter)
{
    /* off: no slot to check, but the heartbeat still prints — the publisher's
       count-only observer feeds it draws/s, which is the cost A/B's other arm */
    if (!s_armed) { heartbeat(&s_frame, frame_counter); return; }
    pkx_frame_end(&s_frame, frame_counter);
}

void tagpu_packet_set_extra(tagpu_packet_extra_fn fn) { s_extra = fn; }

int tagpu_packet_armed(void) { return s_armed; }

/* ------------------------------------------------------------- attach ---- */

void tagpu_packet_init(void)
{
    char b[240];
    s_check  = lever("tagpu_packet.check");
    s_stress = lever("tagpu_packet.stress");
    s_poison = lever("tagpu_packet.poison");
    QueryPerformanceFrequency(&s_freq);       /* the heartbeat's clock, armed or not */
    if (lever("tagpu_packet.off")) {
        plog("packet: disabled by tagpu_packet.off — no slots, nothing published or taken; "
             "every string through tagpu_text_place draws nothing (the group digits, the ShowRanges "
             "labels, the FPS readout); the DrawGameScreen observer stays in count-only mode so "
             "draws/s is still reported");
        return;
    }
    if (!pkx_init(&s_frame, s_stress ? PK_PAGE : PK_GRAIN)) {
        plog("packet: NOT armed — could not reserve or commit the four slots (nothing is "
             "published or taken; the reservation stays as it is)");
        return;
    }
    s_armed = 1;
    _snprintf(b, sizeof b,
              "packet: ARMED %d slots x %u MB reserved, %u KB committed each; W=0 cell=1(stale) READ=2 PREV=3; "
              "check=%d stress=%d poison=%d (frame-packet-exchange, landing 1)",
              PK_SLOTS, PK_RESERVE >> 20, s_frame.cap[0] >> 10, s_check, s_stress, s_poison);
    b[sizeof b - 1] = 0;
    plog(b);
}
