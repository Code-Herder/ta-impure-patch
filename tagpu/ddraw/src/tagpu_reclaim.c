/* tagpu_reclaim.c — deferred reclamation of the engine memory the render
   thread reads: the per-unit Object3do, and the per-LEVEL model templates.
   Design and proof: research/notes/thread-safe-destruction.md. See the header
   for the one-paragraph version.

   THREADS. Two, and only these two touch the shared state:
     producer/owner = the engine's game thread: every FreeObjectState call
                      (unit death 0x486D9E, wreck destroy 0x42474F, the bulk
                      loop 0x4221C4), the model-template frees inside 0x42DB90
                      (0x42DC01, 0x42DCB6) and the level teardown 0x491B60. It
                      owns the ring outright — enqueue AND drain run here, so
                      the ring itself is single-threaded.
     reader         = the fork's GL render thread: publishes s_started before
                      the overlay driver and s_completed after it returns
                      (render_ogl.c brackets tagpu_overlay_draw), and skips the
                      driver's engine reads while a teardown is in progress.
   The only words that cross threads are the two pass counters and the
   teardown flag, all 32-bit aligned, all written through lock-prefixed
   Interlocked ops on the side that needs the fence (§ORDERING below).

   THE RULE. An entry may be freed once (a) the object is unreachable to new
   gathers — the engine nulls its record pointer the instruction after
   FreeObjectState returns, so by the NEXT call on this thread it is — and (b)
   the reader has completed every pass that could hold it: stamp the entry
   with s_started (read after a full fence, so the null is visible first) and
   free when (LONG)(s_completed - stamp) >= 0. An idle reader (completed ==
   started) passes at once; a reader mid-pass makes us wait for that pass; a
   stuck reader freezes reclamation — the ring fills and overflow LEAKS. Never
   free under doubt: no synchronous free, no spin, on the hot path.

   ORDERING (x86-TSO). The hazard is store->load reordering across the
   handshake, so each side fences its publish: the reader's InterlockedIncrement
   of s_started precedes its engine loads; the game side's MemoryBarrier
   precedes its read of s_started (so the engine's earlier null store is
   globally visible first). s_completed is stored after the reader's loads —
   load->store order is kept by the hardware, and the lock prefix fences it
   anyway. Teardown uses the same shape (set flag, fence, wait for the reader
   to leave) — the Dekker pair that makes "the reader is not inside a pass"
   a fact and not a hope.

   TWO CLASSES, ONE RING. RC_OBJ3DO entries go back through the real
   FreeObjectState; RC_BLOCK entries — a model template's single block, and the
   pointer table itself — through the engine's raw MEM_Free. They share the
   ring, the epoch and the overflow policy because they share the hazard: the
   render thread walks both, and 0x42DB90 returns the templates to the same
   small-block heap. What differs is when they are normally released. An
   Object3do waits for the epoch, because units die while the reader runs. A
   template does not need to: the teardown wrap has already made the reader
   quiescent before the cascade, so the post hook frees the whole queue on that
   guarantee, and the epoch is only the fallback for the timed-out path.

   LIVENESS. The drain runs only from FreeObjectState itself (the engine's
   tick detour at 0x4969D2 exists only while a scenario is being applied, so it
   cannot host one), so the most recent death's object is held until the next
   death or the level ends — one object, and its composite-registry slot,
   for the length of a lull. Harmless: the engine draws from the unit array,
   not from the registry, and the teardown wrap flushes it. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_reclaim.h"
#include "tagpu_detour.h"
#include "dd.h"                            /* g_ddraw.gui_thread_id: the game thread */

#define FREEOBJ_VA    0x0045AAA0u          /* FreeObjectState: stdcall, 1 arg, ret 4      */
#define FREEOBJ_RESUME (FREEOBJ_VA + 5u)
#define TEARDOWN_VA   0x00491B60u          /* level teardown: no stack args               */
#define TEARDOWN_RESUME (TEARDOWN_VA + 5u)
/* The model templates' two frees, both inside 0x42DB90 (one caller, 0x491C21).
   0x42DC01 frees one model's block -- the whole tree, its rest vertices and its
   faces are one allocation -- and 0x42DCB6 frees the pointer table itself.
   Both are `push eax; call MEM_Free; add esp,4`, so the 5-byte call is a clean
   redirect to a cdecl function of ours. The two OTHER MEM_Frees in that body
   (0x42DC23, 0x42DC52) are unit-def fields no pass of ours reads: left alone. */
#define TMPLFREE_VA   0x0042DC01u          /* MEM_Free(one Model3DONode block)            */
#define TMPLTAB_VA    0x0042DCB6u          /* MEM_Free(the model-pointer table)           */
#define MEMFREE_VA    0x004D85A0u          /* MEM_Free: cdecl, 1 arg, caller cleans       */
static const unsigned char FREEOBJ_STOLEN[5]  = { 0x53, 0x8B, 0x5C, 0x24, 0x08 }; /* push ebx; mov ebx,[esp+8] */
static const unsigned char TEARDOWN_STOLEN[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }; /* mov eax,[0x511DE8]        */

#define RC_RING_SIZE 4096u                 /* a whole level's objects fit, for the case
                                              where the teardown keeps deferring (below) */
#define RC_RING_MASK (RC_RING_SIZE - 1u)
#define RC_TEARDOWN_WAIT_MS 1000u

#define RC_OBJ3DO 0                        /* free via the real FreeObjectState  */
#define RC_BLOCK  1                        /* free via the engine's raw MEM_Free  */
typedef struct { void* obj; LONG stamp; int stamped; int kind; } RC_ENTRY;

static RC_ENTRY s_ring[RC_RING_SIZE];      /* game thread only                            */
static unsigned s_head, s_tail;            /* game thread only; free-running, & MASK      */
static volatile unsigned char s_defer;     /* the free-detour flag: 1 = enqueue, 0 = real */
static volatile LONG s_started, s_completed;   /* reader publishes; game thread reads     */
static volatile LONG s_teardown;           /* game thread sets; reader reads              */
static volatile LONG s_levelGen;           /* game thread bumps, once per teardown; the
                                              render thread keys its model-template
                                              caches on it (see the header)             */
static void (__stdcall *s_real_free)(void*);   /* trampoline into the real body           */
static void (__cdecl   *s_mem_free)(void*) = (void (__cdecl*)(void*))MEMFREE_VA;
static int   s_installed;
static int   s_tmplArmed;                  /* the two template redirects are in       */
static int   s_quiesced;                   /* this teardown's pre hook proved the
                                              reader idle, so the post hook may free  */
static volatile DWORD s_owner_tid;         /* the game thread, when the fork has not
                                              recorded it yet: the first caller           */
static int   s_foreign_logged;

/* counters: written on the game thread, read racily by the render thread's log */
static volatile unsigned s_cDeferred, s_cDrained, s_cOverflow, s_cForeign,
                         s_cFlushed, s_cHeld, s_cHigh, s_cTeardowns,
                         s_cTmpl, s_cTmplFreed, s_cTmplLeaked;

static void rlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* Every entry is freed through here, so the two classes can share one ring,
   one epoch and one overflow policy. */
static void entry_free(RC_ENTRY* e)
{
    if (e->kind == RC_BLOCK) s_mem_free(e->obj);
    else                     s_real_free(e->obj);
}

/* Push one pointer, or LEAK it if the ring is full: never free under doubt (a
   reader may hold it) and never spin (the reader may be gone). 1 = queued. */
static int ring_push(void* p, int kind)
{
    RC_ENTRY* e;
    if (s_tail - s_head >= RC_RING_SIZE) { s_cOverflow++; return 0; }
    e = &s_ring[s_tail & RC_RING_MASK];
    e->obj = p; e->stamp = 0; e->stamped = 0; e->kind = kind;
    s_tail++;
    if (s_tail - s_head > s_cHigh) s_cHigh = s_tail - s_head;
    return 1;
}

/* ---------------------------------------------------------- game thread ---- */

/* Called by the stub for EVERY FreeObjectState while s_defer is set, with the
   object the engine wants freed. Drains what became safe, then queues this. */
static void __cdecl reclaim_enqueue(void* obj)
{
    DWORD tid = GetCurrentThreadId();
    DWORD owner = g_ddraw.gui_thread_id;   /* the fork records the game (GUI) thread at
                                              window creation, long before any death   */
    LONG  snap;
    unsigned i;

    if (!owner) {                          /* not yet known: the first caller is it     */
        if (!s_owner_tid) s_owner_tid = tid;
        owner = s_owner_tid;
    }
    if (tid != owner) {
        /* not the game thread: the ring is single-threaded, so this one object
           takes the stock path (freed now). Counted and logged once; never seen
           in stock TA — every caller is a sim or teardown path. */
        s_cForeign++;
        if (!s_foreign_logged) {
            char b[120];
            s_foreign_logged = 1;
            _snprintf(b, sizeof b, "reclaim: FreeObjectState from thread %lu (game thread %lu): freed synchronously",
                      (unsigned long)tid, (unsigned long)owner);
            rlog(b);
        }
        s_real_free(obj);
        return;
    }

    /* 1. drain. Every entry already here was enqueued by an earlier call on
          this thread, so the engine has nulled its record since (program
          order). Fence, then read the reader's pass counter: the fence makes
          those nulls globally visible before any pass we stamp with could
          start, so a pass that starts after the stamp cannot gather them. */
    MemoryBarrier();
    snap = s_started;
    for (i = s_head; i != s_tail; i++) {
        RC_ENTRY* e = &s_ring[i & RC_RING_MASK];
        if (!e->stamped) { e->stamp = snap; e->stamped = 1; }
    }
    /* stamps are non-decreasing from head to tail (s_started is monotone and
       later entries are stamped at later calls), so stop at the first that
       the reader has not yet completed */
    while (s_head != s_tail) {
        RC_ENTRY* e = &s_ring[s_head & RC_RING_MASK];
        if (!e->stamped || (LONG)(s_completed - e->stamp) < 0) break;
        entry_free(e);
        if (e->kind == RC_BLOCK) s_cTmplFreed++; else s_cDrained++;
        s_head++;
    }

    /* 2. queue this one. Its null happens the instant we return; it is
          stamped by the next call. Full ring = leak this object: never free
          it (a reader may hold it) and never spin (the reader may be gone). */
    if (ring_push(obj, RC_OBJ3DO)) s_cDeferred++;
}

/* THE MODEL TEMPLATES, from inside the teardown cascade (game thread).
   0x42DB90's two MEM_Free call sites are redirected here, so a template block
   is queued instead of returned to the heap and the render thread cannot be
   walking memory the allocator has taken back. It is the same epoch the
   Object3do class uses, and for the same reason -- but the release is normally
   NOT the epoch at all: the post hook frees the whole queue outright whenever
   the pre hook proved the reader idle, because the reader has then been refused
   every pass since, which is a fact rather than a race. Only the timed-out
   (`busy`) path falls back to the stamp, and it can only be later.

   NOT stamped here, deliberately: the engine nulls the slot at 0x42DC15 the
   instruction AFTER this returns (and the table pointer at 0x42DCD8), so a pass
   could still reach the block through main+0x14377 until then. The ring's rule
   -- stamp at the NEXT drain, never at the push -- is exactly the rule that
   makes that safe, which is why this class reuses it rather than inventing one.

   No drain from here: the cascade is mid-flight, and calling FreeObjectState
   re-entrantly from inside 0x42DB90 would be a behaviour the engine never has. */
static void __cdecl reclaim_template_free(void* p)
{
    if (!p) return;
    if (GetCurrentThreadId() != (g_ddraw.gui_thread_id ? g_ddraw.gui_thread_id
                                                       : s_owner_tid)) {
        s_cForeign++;                      /* never seen: 0x42DB90 is game-thread only */
        s_mem_free(p);
        return;
    }
    s_cTmpl++;
    if (!ring_push(p, RC_BLOCK)) s_cTmplLeaked++;   /* full ring: leak, never free */
}

/* Level teardown, on entry to 0x491B60 (game thread). Hold the reader off and
   wait for it to leave its pass. If it does: free everything queued while the
   composite registry those frees walk is still alive, and let the cascade —
   hundreds of frees, every unit through 0x485980 -> 0x4864B0 -> 0x4866D0 and
   the wreck loop 0x4221C4 — free synchronously, which is safe because the
   reader is refused a pass until the post hook. If it does NOT leave in time
   (a frame over a second, or a stuck render thread) the reader may still hold
   pointers, so nothing may be freed: the queue is KEPT and deferral stays on
   through the cascade. Those objects drain at the next game's first deaths;
   that is safe because FreeObjectState skips the registry walk once the
   teardown has nulled *(main+0x1437B) (0x42DCA3), and a held block cannot be
   recycled to a new object while we still hold it. */
static void __cdecl reclaim_teardown_pre(void)
{
    DWORD t0;
    unsigned n = 0, busy = 0;
    char b[200];

    InterlockedExchange(&s_teardown, 1);       /* fence: visible before we look */
    t0 = GetTickCount();
    while (s_completed != s_started) {         /* the reader is inside a pass */
        if (GetTickCount() - t0 > RC_TEARDOWN_WAIT_MS) { busy = 1; break; }
        Sleep(0);
    }
    s_quiesced = !busy;
    if (!busy) {
        while (s_head != s_tail) { entry_free(&s_ring[s_head & RC_RING_MASK]); s_head++; n++; }
        s_cFlushed += n;
        s_defer = 0;                           /* the cascade frees synchronously */
    } else {
        s_cHeld++;                             /* deferral stays on; nothing is freed */
    }
    s_cTeardowns++;
    _snprintf(b, sizeof b,
              busy ? "reclaim: level teardown (gen %u -> %u): reader still in its pass after %u ms — %u queued object(s) KEPT, the cascade's frees deferred"
                   : "reclaim: level teardown (gen %u -> %u): flushed %u queued object(s), reader idle; the cascade frees synchronously",
              (unsigned)s_levelGen, (unsigned)s_levelGen + 1u,
              busy ? RC_TEARDOWN_WAIT_MS : n, (unsigned)(s_tail - s_head));
    rlog(b);
}

/* After 0x491B60 returns (or tail-jumps to 0x450DD0, which also returns with
   a plain ret): deferral back on, bump the level generation, then release the
   reader.

   THE GENERATION IS BUMPED HERE AND NOT IN THE PRE HOOK, and the difference is
   a bug rather than a preference. The render thread is NOT stopped by
   `pass_begin` -- `render_ogl.c` ignores its return value -- it is stopped by
   `tagpu_overlay.c`'s `teardown_active()` gate, and a pass that got past that
   gate before the flag was set runs on while the pre hook waits for it. That
   pass reaches `tagpu_native_frame` (thirteen lines and two subsystems later,
   one of them file I/O) and would there see a generation bumped in the pre
   hook, drop the template caches, and REFILL THEM IN THE SAME FRAME from
   templates the cascade has not freed yet -- because the game thread is still
   blocked waiting for that very pass. The stamp would then match for the rest
   of the process and the caches would never drop again: exactly the stale-
   template bug the generation exists to prevent, with an extra step.

   Bumped here, nothing can repopulate between the free and the bump: the
   overlay gate refuses every frame for the whole teardown, and the release
   below is ordered after the increment (both are interlocked, so the render
   thread cannot observe the release without the new generation). It is still
   unconditional -- the pre hook's `busy` path keeps the queue and frees nothing
   of ours, but 0x42DB90 frees the templates either way.

   This runs on both of 0x491B60's exits: the tail-jump at 0x491C54 goes to
   0x450DD0, which takes no stack argument and has a single `ret` at 0x450E19,
   so the stub's call returns and this hook is reached. */
static void __cdecl reclaim_teardown_post(void)
{
    /* THE TEMPLATES THE CASCADE JUST QUEUED GO BACK HERE, and the argument is a
       fact rather than a window. When the pre hook returned with the reader
       idle it had already published s_teardown, so every pass_begin since has
       quiesced at once and tagpu_overlay_draw has returned before its first
       engine read: no pass in this interval can be holding a template pointer,
       and none could have started holding one. So the queue is freed outright,
       the heap gets the level's models back before the next one loads, and
       nothing depends on how long anything took.

       When the pre hook timed out instead (`busy`), the reader may genuinely
       still be inside a pass that is reading templates -- that is what the
       timeout means -- so nothing is freed here. Those blocks stay on the ring
       and leave it through the ordinary epoch drain, stamped at the next
       FreeObjectState and freed once the reader has completed a pass past the
       stamp. Later, never sooner; and if the reader never runs again they are
       leaked, which is the module's standing answer to doubt.

       This runs BEFORE the generation bump only for tidiness -- the reader is
       refused passes either way until the flag clears below. */
    if (s_quiesced) {
        unsigned n = 0;
        while (s_head != s_tail) { entry_free(&s_ring[s_head & RC_RING_MASK]); s_head++; n++; }
        s_cFlushed += n;
        if (n) {
            char b[140];
            _snprintf(b, sizeof b,
                      "reclaim: teardown post: freed %u block(s) the cascade queued "
                      "(%u model template(s) this session), reader quiesced throughout",
                      n, s_cTmpl);
            rlog(b);
        }
    } else if (s_tmplArmed) {
        rlog("reclaim: teardown post: reader was still in its pass — the cascade's "
             "template blocks are KEPT and leave by the epoch, not here");
    }
    s_quiesced = 0;
    s_defer = s_installed ? 1 : 0;
    InterlockedIncrement(&s_levelGen);
    InterlockedExchange(&s_teardown, 0);
}

/* -------------------------------------------------------- render thread ---- */

int tagpu_reclaim_pass_begin(void)
{
    if (!s_installed) return 1;
    InterlockedIncrement(&s_started);          /* fence: "in a pass" before we look */
    if (s_teardown) {
        InterlockedExchange(&s_completed, s_started);   /* no engine reads this frame */
        return 0;
    }
    return 1;
}

void tagpu_reclaim_pass_end(unsigned frame_counter)
{
    static unsigned last;
    if (!s_installed) return;
    InterlockedExchange(&s_completed, s_started);       /* after the last engine read */
    if (frame_counter - last >= 300) {
        char b[200];
        last = frame_counter;
        _snprintf(b, sizeof b,
                  "reclaim: def=%u drn=%u queued=%u hw=%u ovf=%u foreign=%u flushed=%u held=%u teardowns=%u tmpl=%u/%u/%u pass=%ld",
                  s_cDeferred, s_cDrained, (unsigned)(s_tail - s_head), s_cHigh, s_cOverflow,
                  s_cForeign, s_cFlushed, s_cHeld, s_cTeardowns,
                  s_cTmpl, s_cTmplFreed, s_cTmplLeaked, (long)s_completed);
        rlog(b);
    }
}

int tagpu_reclaim_teardown_active(void) { return s_installed && s_teardown; }

unsigned tagpu_reclaim_level_gen(void) { return (unsigned)s_levelGen; }

int tagpu_reclaim_armed(void) { return s_installed; }

/* --------------------------------------------------------------- install ---- */

/* Redirect one `call MEM_Free` to `fn`. The rel32 is computed against the
   CALL SITE, not against the buffer it is built in -- a rel32 encoded against
   a stack address is a wild call, and this project has already paid for that
   once (field notes: tagpu_detour_rel takes the address it will live at).
   Refuses unless the site really is `E8 <rel to 0x4D85A0>`, so a different exe
   arms nothing. */
static int redirect_memfree(unsigned int va, void (__cdecl *fn)(void*))
{
    unsigned char b[5];
    int rel;
    if (*(const unsigned char*)(size_t)va != 0xE8) return 0;
    memcpy(&rel, (const void*)(size_t)(va + 1), 4);
    if ((unsigned)(va + 5 + rel) != MEMFREE_VA) return 0;
    b[0] = 0xE8;
    rel = (int)((unsigned)(size_t)fn - (va + 5));
    memcpy(b + 1, &rel, 4);
    return tagpu_detour_write(va, b, 5);
}

/* The free detour. Same shape as tagpu_detour_leaf_call (flag-gated: call our
   function with arg1 and `ret 4` while set, else the stolen prologue and a
   jump back), built here because the drain needs the address of that stolen
   tail as a callable trampoline into the real body:
       cmp byte [s_defer],0 ; jz real            (7+2)
       pushad ; push [esp+0x24] ; call enqueue   (1+4+5)
       add esp,4 ; popad ; ret 4                 (3+1+3)
     real: <5 stolen> ; jmp 0x45AAA5             (5+5)   <- s_real_free
   Called `push obj; call real`, the stolen `mov ebx,[esp+8]` reads obj and
   the body's own `ret 4` returns to us: a normal stdcall. */
static unsigned char* build_free_stub(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    p = tagpu_detour_cmp_flag(p, &s_defer);
    *p++ = 0x74; *p++ = 0x11;                                   /* jz +17            */
    *p++ = 0x60;                                                /* pushad            */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x24;         /* push [esp+0x24]   */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)&reclaim_enqueue); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                      /* add esp,4         */
    *p++ = 0x61;                                                /* popad             */
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;                      /* ret 4             */
    s_real_free = (void (__stdcall*)(void*))p;                  /* real:             */
    memcpy(p, FREEOBJ_STOLEN, 5); p += 5;
    *p++ = 0xE9; tagpu_detour_rel(p, FREEOBJ_RESUME); p += 4;
    return s;
}

/* The teardown wrap. 0x491B60 takes nothing on the stack (six bare `call`
   sites) and has two exits — `ret` at 0x491C59, and a tail-jump at 0x491C54
   to 0x450DD0 when 0x435100 returns 3, which itself reads no stack argument
   and returns with a plain `ret` at 0x450E19 — so its body can be CALLed
   through the stolen tail and code run after it returns either way:
       pushad ; call pre ; popad                 (1+5+1)
       call tail                                 (5)   the real teardown
       pushad ; call post ; popad                (1+5+1)
       ret                                       (1)   to the original caller
     tail: <5 stolen> ; jmp 0x491B65             (5+5)
   pushad/popad hand the body the caller's registers and hand the caller the
   body's, eax included. */
static unsigned char* build_teardown_stub(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    unsigned char* tail;
    if (!s) return NULL;
    tail = s + 20;
    *p++ = 0x60;                                                /* pushad            */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)&reclaim_teardown_pre); p += 4;
    *p++ = 0x61;                                                /* popad             */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)tail); p += 4;   /* call tail */
    *p++ = 0x60;                                                /* pushad            */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)&reclaim_teardown_post); p += 4;
    *p++ = 0x61;                                                /* popad             */
    *p++ = 0xC3;                                                /* ret               */
    /* p == tail */
    memcpy(p, TEARDOWN_STOLEN, 5); p += 5;
    *p++ = 0xE9; tagpu_detour_rel(p, TEARDOWN_RESUME); p += 4;
    return s;
}

void tagpu_reclaim_init(void)
{
    unsigned char *sf, *st;
    if (GetFileAttributesA("tagpu_reclaim.off") != INVALID_FILE_ATTRIBUTES) {
        rlog("reclaim: disabled by tagpu_reclaim.off — Object3do frees are immediate (the render thread races them)");
        return;
    }
    if (memcmp((void*)FREEOBJ_VA, FREEOBJ_STOLEN, 5) != 0 ||
        memcmp((void*)TEARDOWN_VA, TEARDOWN_STOLEN, 5) != 0) {
        rlog("reclaim: NOT armed — engine bytes differ at 0x45AAA0 / 0x491B60");
        return;
    }
    /* build both before landing either, so a failure arms nothing */
    sf = build_free_stub();
    st = build_teardown_stub();
    if (!sf || !st) { rlog("reclaim: NOT armed — stub allocation failed"); return; }
    /* the wrap first: inert on its own (pre sees an empty ring); then the
       detour, which is what changes behaviour. s_defer stays 0 until both
       are in, so an early call through the stub takes the real path. */
    if (!tagpu_detour_land(TEARDOWN_VA, st, 5)) { rlog("reclaim: NOT armed — could not write 0x491B60"); return; }
    if (!tagpu_detour_land(FREEOBJ_VA, sf, 5))  { rlog("reclaim: NOT armed — could not write 0x45AAA0 (teardown wrap is in, inert)"); return; }
    s_installed = 1;
    s_defer = 1;
    /* Last, and independently: the module is worth having without it, so a
       refusal here is logged rather than fatal. Both sites or neither. */
    if (redirect_memfree(TMPLFREE_VA, &reclaim_template_free)) {
        if (redirect_memfree(TMPLTAB_VA, &reclaim_template_free)) {
            s_tmplArmed = 1;
        } else {
            unsigned char b[5];                /* put the first one back */
            int rel = (int)(MEMFREE_VA - (TMPLFREE_VA + 5));
            b[0] = 0xE8; memcpy(b + 1, &rel, 4);
            tagpu_detour_write(TMPLFREE_VA, b, 5);
        }
    }
    rlog(s_tmplArmed
         ? "reclaim: ARMED FreeObjectState@0x45AAA0 -> deferred (quiescence, game-thread drain), "
           "teardown@0x491B60 -> hold reader + flush, model templates@0x42DC01/0x42DCB6 -> deferred; ring=4096"
         : "reclaim: ARMED FreeObjectState@0x45AAA0 -> deferred (quiescence, game-thread drain), "
           "teardown@0x491B60 -> hold reader + flush; model-template frees NOT deferred "
           "(0x42DC01/0x42DCB6 are not calls to MEM_Free on this exe); ring=4096");
}
