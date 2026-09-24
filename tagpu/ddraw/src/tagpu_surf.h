#ifndef TAGPU_SURF_H
#define TAGPU_SURF_H

#include "tagpu_overlay.h"      /* TAGPU_FRAME: the frame this snapshot belongs to */

/* TA'S OWN FRAME — THE GOLDEN SOURCE, captured on the thread that draws it.

   WHAT THIS IS FOR. The engine rasterises its whole screen into an 8-bit
   surface and, since the clean cut, not one pixel of it reaches the display:
   everything the player sees is drawn by our own passes. What the engine
   produces is kept anyway, as the REFERENCE — the picture the 1997 software
   rasteriser drew, in the same process, on the same state, so that anything of
   ours can be diffed against it. `tagpu_vk_surf.c` puts it on the device;
   `tagpu_vk_surf_engine_view` hands the view out. It is a reference and never
   a layer, and there is no lever that makes it one.

   ---------------------------------------------------------------------------
   THE ORDERING, WHICH IS THE WHOLE OF WHY THIS FILE IS SHAPED LIKE THIS

   The bytes live in the fork's DirectDraw primary. TA rasterises into it
   through a Lock/Unlock pair that enters NO critical section, and
   `FlipOffscreenToPrimary 0x4C63A0` copies the engine's offscreen onto it row
   by row — all on the GAME thread. A copy out of it on the RENDER thread, from
   `tagpu_overlay_draw`, has nothing sequencing the two. `g_ddraw.cs` does not
   help: that section is a LIFETIME ARGUMENT FOR THE POINTER — it serialises
   `dds_Flip`'s swap and `dds_SetPalette` — and never a bound on the BYTES.
   The failing interleaving is concrete: the render thread reaches the row loop
   while the game thread is inside the flip between its terrain rows and its
   side-panel rows; the copy takes new terrain over last frame's panel, the
   byte comparison says "changed", the serial advances, and a TORN picture is
   uploaded as the golden source with nothing marking it.

   SO THE COPY RUNS ON THE WRITER'S OWN THREAD, at a point where the frame is
   complete by construction: `tagpu_packet_pub.c`'s `after_draw` observer on
   `DrawGameScreen 0x468CF0`, on the in-play draw only. The flip is INSIDE that
   function (`0x46A3DB`, 34 bytes before its `ret` at `0x46A3FD`), so at the
   observer's `after` the primary holds the frame the engine has just presented,
   finished, on the thread that finished it. No lock is needed for the bytes and
   none is claimed: the writer is not running, because the writer is us.

   AND IT PAIRS THE REFERENCE WITH THE PACKET. That same call publishes the
   frame packet our passes draw from, so the golden source and the state our
   renderer renders are THE SAME ENGINE FRAME by construction — a stronger
   property than "untorn", and the reason this hook was chosen over the flip's
   own entry (which would give the previous frame, complete but one behind).

   WHAT THAT COSTS IN COVERAGE, SAID PLAINLY: `DrawGameScreen` is in-play only —
   the shell never calls it (`tagpu_packet_pub.c`, the cursor channel's "WHY NOT
   THE FLIP"). So there is NO golden source in the menus, the loading screen or
   any other shell frame. `tacli shot` is the answer there: it reads the same
   primary from `before_flip` on the game thread, one flip after the arm, which
   is the last COMPLETED frame.

   TWO MORE CONFIGURATIONS CAPTURE NOTHING, and in both the render thread is
   left holding a request that will never be served; both are benign. Under the GDI backend the render
   half never runs at all, so no request is ever made and the game thread's
   gate returns on its first compare — zero cost. Under the publisher's
   `s_countOnly` the `after_draw` observer is not registered, so the render
   thread asks and the game thread never answers: `asked` climbs by one, `ack`
   never moves, every later sync takes the in-flight branch, and the reference
   stays absent. Nothing spins, nothing blocks and nothing leaks; the heartbeat
   shows it as `asked=1 captured=0`.

   ---------------------------------------------------------------------------
   THE HAND-OVER: TWO BUFFERS AND ONE OWNERSHIP RULE

   A copy on the game thread only moves the race rather than removing it
   unless the reader is ordered too, so the snapshot is double-buffered and the
   two threads never touch one buffer:

     * `hold` names the buffer the RENDER thread owns. The game thread writes
       only `1 - hold` and reads `hold` only to compare bytes against the last
       snapshot — two readers of one buffer is not a hazard; a reader and a
       writer is.
     * `req` (render to game) and `ack` (game to render) are counters. A capture
       is IN FLIGHT exactly while `req != ack`.
     * The render thread changes `hold` ONLY when nothing is in flight, and only
       inside `tagpu_surf_sync`. It then bumps `req` with a RELEASE store, so the
       game thread cannot observe the request ahead of the new `hold`.
     * The game thread captures ONLY when a request is outstanding (an ACQUIRE
       load of `req`), fills `1 - hold`, and publishes with a RELEASE store of
       `ack`.

   Therefore: the buffer the render thread reads is never the buffer the game
   thread writes, and `hold` never moves under a capture. That is a bound and an
   ordering, not a window — no probe, no timeout, no "the other thread has
   almost always finished". `volatile` is not the mechanism; the `__atomic`
   acquire/release pair is, for the reason `tagpu_packet.c` states at length.

   AND IT IS NOT RE-ENTRANT. The producer would break
   if a second capture could begin while one was mid-copy on the same thread:
   the inner publish would let the render thread flip `hold`, and the outer
   would go on writing the buffer the render thread now owns. It cannot happen.
   `tagpu_surf_capture` runs from `after_draw`, which runs only for the return
   address `VA_DRAW_RET_INPLAY` -- one call site, `0x4969CD` in the frame
   callback, which `DrawGameScreen` does not call -- and `after_draw` pops the
   return stack BEFORE it captures, so a nested in-play draw would still produce
   two captures strictly one after the other, each complete before the next
   begins. Nothing in `tagpu_surf_capture` can re-enter the engine. The
   publisher counts what would falsify this (`deep=` in its heartbeat; 0 over
   every session measured).

   IT ALSO BOUNDS THE COST, THOUGH NOT BY PIXEL COUNT. The game thread copies
   at most once per render-thread frame, because a capture needs a request and a
   request needs a sync, and a render thread that is behind costs the game
   thread nothing at all. But what each copy costs is `w*h` copied plus `w*h`
   compared, on TA's lockstep game thread: the 52-59 us the notes quote is
   1024x768, and the figure scales with the pixels. At the
   `TAGPU_SURF_MAXDIM` ceiling it would be a 16 MiB copy and a 16 MiB compare
   per in-play draw. The heartbeat prints `us avg/max` so it is never a guess.

   THE SNAPSHOT'S LIFETIME IS THE LEVEL'S, AND THE LEVEL IS A PROPERTY OF THE
   SNAPSHOT. `tagpu_surf_level_end` is called from the same teardown that ends
   the packet's level and bumps one counter; the game thread stamps each capture
   with the counter it was taken under; the render thread will adopt only a
   capture stamped with the counter it has acted on, and `tagpu_surf_frame`
   serves only a snapshot stamped with the CURRENT one. So the reference cannot
   outlive the level it was taken from and be diffed against the next.

   IT IS A STAMP RATHER THAN A FLAG BECAUSE A FLAG COULD NOT BE CLEARED IN
   TIME. The producer's half runs inside `tagpu_overlay_draw`,
   below its `tagpu_reclaim_teardown_active()` early return — a level teardown,
   which is exactly the event the drop is for — while the consumer runs from the
   Vulkan frame record and shares no such return. Testing the data on the
   reader's own path is what makes the drop reachable at all.

   IT IS NOT AN ENGINE READ, and so this file is not on
   `tagpu/ddraw/thread-split.allow`. `g_ddraw.primary` is the FORK's own
   DirectDraw surface object, not the game's memory at an absolute address. */

/* ---------------------------------------------------------------------------
   `tagpu_surfdump.on` -- THE ORACLE, one shot, self-deleting, two halves

   The golden source has no consumer in the tree, so there is otherwise no way
   to look at it and no way to check the claim above. Drop the file in the
   instance directory and the next capture answers both questions in the log:

     * `surf: re-read check at draw N: 0 byte(s) of M differ` -- the GAME thread
       reads the primary a second time and compares it against the copy it just
       took. The claim is that nothing else is writing those bytes at
       this point; 0 is that claim measured, and it must hold with the game in
       motion, not only on a settled screen. A non-zero answer means the site is
       wrong.
     * `tagpu_surf.ppm` -- the snapshot resolved through its own palette, written
       by the render thread from the buffer it holds. On a settled scene it must
       be pixel-identical to `tacli shot`, which reads the same primary from the
       other game-thread hook (the flip's entry, i.e. the previous completed
       frame).

   Both are one shot and nothing about the steady state changes. It is an oracle
   rather than instrumentation, which is why it is here and not behind a counter.
   --------------------------------------------------------------------------- */

/* The widest surface this module will copy. A bound on an allocation and on a
   memcpy, so it is stated here and re-checked where it is used rather than
   trusted across a file boundary. */
#define TAGPU_SURF_MAXDIM 4096

/* WHAT WAS TAKEN, and where it goes. One struct rather than eight out-params,
   which is the shape `tagpu_feat_handover` settled on for the same reason.

   EVERY POINTER IN IT IS VALID FOR THE RENDER-THREAD FRAME THAT ASKED, and for
   no longer — the same rule the frame packet's consumer lives under. The buffer
   it names is the one this thread holds, and it stops holding it at the next
   `tagpu_surf_sync`. */
typedef struct {
    /* `w * h` 8-bit indices, TIGHTLY PACKED: the copy removes the primary's
       pitch, so a consumer uploads `w * h` and has no row stride of its own to
       get wrong. */
    const unsigned char* bytes;
    int                  w, h;
    /* 256 four-byte entries, R,G,B,255 — the palette the screen is being shown
       with, read from the primary's own palette object INSIDE THE SAME
       CRITICAL SECTION AS THE BYTES, so the indices and the table they resolve
       through cannot be a frame apart. (Not from `tagpu_pal.c`: that is a
       different instant, and every entry point of that module is render-thread
       only.) */
    const unsigned char* pal;
    /* Bumped only when the bytes CHANGED against the previous snapshot, for a
       consumer that uploads to a device and wants to skip an upload it already
       holds. TA redraws its whole screen far less often than we present. */
    unsigned             serial;
    /* AND THE PALETTE HAS ITS OWN, because it moves INDEPENDENTLY of the bytes
       and a consumer that gated both on `serial` would freeze the colours. A
       fade is exactly that case -- one picture held still while the table runs
       down to black -- and so is a gamma change over a static screen. */
    unsigned             palSerial;
    /* WHICH ENGINE DRAW THIS IS, from the publisher's own in-play draw counter.
       A diff that wants to say WHAT it compared has the number here; a consumer
       that sees it standing still knows the game thread has answered nothing
       since. It is evidence, never a gate. */
    unsigned             stamp;
    /* WHERE IT GOES, in window pixels: the RENDER thread's letterboxed viewport
       for the frame that asked. It describes our window, not the engine's
       frame, which is why it is taken on that side and not with the bytes. */
    int                  dx, dy, dw, dh;
} TAGPU_SURFFRAME;

/* RENDER THREAD, once a frame, from `tagpu_overlay_draw` before any pass
   gathers: take whatever the game thread has answered, record this frame's
   viewport, and ask for the next snapshot. Cheap and silent — two atomic loads
   and a store on a frame with nothing new. */
void tagpu_surf_sync(const TAGPU_FRAME* f);

/* GAME THREAD, post-flip, in-play frames only, from the packet publisher's
   `after_draw`: answer an outstanding request by copying the primary. `stamp`
   is the publisher's in-play draw number, carried into the snapshot. Returns at
   once when nothing has been asked for, which is what bounds the cost. */
void tagpu_surf_capture(unsigned stamp);

/* GAME THREAD, from the same teardown that ends the packet's level: what is
   held belongs to a level that no longer exists. The drop itself happens on the
   render thread, at its next `sync`. */
void tagpu_surf_level_end(void);

/* This frame's reference, or 0 when there is none — nothing captured yet, a
   non-8bpp mode, no primary, a geometry outside the bound above, a frame with
   no viewport, or a level that has ended. Render thread. */
int tagpu_surf_frame(TAGPU_SURFFRAME* out);

#endif
