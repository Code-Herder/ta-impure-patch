#ifndef TAGPU_PACKET_PUB_H
#define TAGPU_PACKET_PUB_H
/* tagpu_packet_pub.h — the frame packet exchange, PRODUCER side.

   Included only by publisher files: tagpu_packet_pub.c (the observer and the
   fill), tagpu_reclaim.c (the out-of-game packet from its teardown post hook)
   and tagpu_markown.c (the font copy at hook 8). A render-thread file that
   includes this header is on the wrong side of the split and the build rule
   (tools/thread-split-check.sh) treats the include like an engine read. */
#include "tagpu_packet.h"

/* ---- the primitive's producer entry (tagpu_packet.c), game thread only ---- */
/* Fill and publish one packet into the slot the producer holds. `fill`
   writes everything between head_seq and crc — the primitive stores the head
   before calling it, the crc and the tail after — and returns the byte count
   it NEEDED (>= what it used): a need past `p->cap_bytes` means it truncated
   this frame, and the primitive grows the slot before the next fill. `force`
   ignores the FRESH gate (the level-end packet must land whatever the cell
   holds); an in-play publish leaves it 0, so a packet the renderer has not
   taken is never replaced and the copy costs nothing until it is.
   Returns 1 when a packet was published. */
typedef unsigned (*tagpu_packet_fill_fn)(TAGPU_PACKET* p, void* ctx);
int tagpu_packet_publish(tagpu_packet_fill_fn fill, void* ctx, int force);
/* The producer's thread, registered once at install (the game thread, which
   DllMain runs on). A publish from any other thread is refused and counted
   as `foreign`; without a registration the first publisher would be latched,
   and a stray first call would then refuse the real one for the session. */
void tagpu_packet_producer(unsigned long tid);

/* ---- the command record's consumer entry, GAME thread only ---- */
/* At the top of every in-play draw, from the observer's `before`: take the
   latest command record if the render thread posted one, else keep the one
   held. Returns the record the game thread now holds — NULL before the first
   post, when the exchange is off, or when the record fails its bounds (a
   violation, counted) — valid until tagpu_cmd_done(), which must follow on
   every path, like the frame packet's frame_end. The same slot ownership
   proof as the packet's, with the threads swapped: the render thread never
   waits for this, and a render thread that stops posting leaves the game
   thread re-applying the last record's LEVELS (the zoom, the hold) — the
   same standing as the render thread ceasing to write those words itself. */
const TAGPU_CMD* tagpu_cmd_take(void);
void             tagpu_cmd_done(void);

/* PLAIN STORES, BY CONSTRUCTION. The exchange's ordering argument (P1: the
   payload is visible before the index) rests on the payload being ordinary
   stores that `xchg`'s lock orders — and the toolchain targets i686 without
   SSE2, so the seq-cst fence it emits is a `lock or`, which orders ordinary
   stores and NOT non-temporal ones. The CRT's memcpy/memset are free to use
   non-temporal moves for large blocks; these two are not, and every byte a
   fill writes into a slot goes through them. `volatile` keeps the compiler
   from turning the loop back into a memcpy call. Slots are small (kilobytes),
   so the cost is nothing measurable. */
static __inline void tagpu_pk_copy(void* dst, const void* src, unsigned n)
{
    volatile unsigned char* d = (volatile unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    unsigned i;
    for (i = 0; i < n; i++) d[i] = s[i];
}
static __inline void tagpu_pk_fill(void* dst, unsigned char v, unsigned n)
{
    volatile unsigned char* d = (volatile unsigned char*)dst;
    unsigned i;
    for (i = 0; i < n; i++) d[i] = v;
}

/* ---- the frame packet's publisher (tagpu_packet_pub.c) ---- */
/* DLL attach, AFTER tagpu_menu_init(): the DrawGameScreen observer chains
   onto the menu's (its `before` never hijacks, ours must for `after`), plus
   the loader-thread observer that settles the thread's identity in the log.
   Byte-matched, all-or-nothing; `tagpu_packet.off` leaves the observer in
   count-only mode (no hijack, no publish) so the heartbeat still reports
   draws per second for the cost A/B. */
void tagpu_packet_pub_init(void);
/* Game thread, at hook 8 (0x469BD7) from markown's stub: the instant the
   engine's marker block begins, with the font and text colour it will use.
   Copies the font's header and its 95 printable glyphs into the packet's
   font area whenever the font pointer or its header signature changed, and
   latches the colour. No render-thread code dereferences an engine font. */
void tagpu_packet_pub_font_snapshot(void);
/* Game thread, from reclaim_teardown_post after the level generation was
   bumped: publish a header-only packet with in_game = 0 and the new
   generation, forced past the FRESH gate, so the renderer stops drawing the
   dead level over the menus and the loading screen. */
void tagpu_packet_pub_level_end(unsigned level_gen);
/* GAME THREAD. The publisher's own in-play draw counter — the number of draws
   it has seen through the 0x4969D2 gate. A game-thread observer that runs
   INSIDE the draw stamps it, and `fill_fog` (which runs in the same draw's
   `after`) compares: a stamp that is not this draw's means that observer did
   not run, which is how the publisher tells "this is current" from "this is
   whatever was there when I last owned the site". Nothing else may use it as a
   clock; it counts draws, not frames and not ticks. */
unsigned tagpu_packet_pub_draw_seq(void);
/* GAME THREAD. The publisher's own level generation — the one the packet
   carries and every consumer keys on. A game-thread observer that latches
   per-level state stamps it beside the draw counter, so the latch cannot
   outlive its level. */
unsigned tagpu_packet_pub_level_gen(void);
/* GAME THREAD. 1 when SOMETHING moves the generation above on a level end --
   this module's own observer on the teardown 0x491B60, or `tagpu_reclaim`'s
   wrap, which calls `tagpu_packet_pub_level_end` from a hard-wired stub whether
   or not THIS module is armed. It is deliberately NOT `s_levelEndBy != 0`: that
   is assigned only when the publisher itself is armed, so under
   `tagpu_packet.off` it reads 0 while the generation moves correctly, and a
   consumer keyed on it refuses everything for the session.
   A caller using the generation as a SAFETY argument must check this and refuse
   when it is 0 -- 0 means no ordering is available, which is a reason to refuse
   a read rather than to take it. */
int tagpu_packet_pub_level_tracked(void);

/* GAME THREAD: 1 once this level's FIRST in-play packet has published, 0 again
   from the level end. It is the producer-side reading of the packet's `in_game`,
   and it is the gate a producer uses to ask "is a level actually on screen" --
   not `level_gen`, which answers "has a boundary happened", and not `load_flags`,
   which is for the log.

   IT IS TRUE ONLY AFTER THE ENGINE HAS DRAWN THE LEVEL AT LEAST ONCE, and that
   ordering is the engine's rather than ours: the in-play packet publishes from
   the observer on DrawGameScreen's in-play call site, which the engine installs
   at 0x498342, and 0x497CE0 installs it BEFORE it paints at 0x49842A. So a
   caller gated on this can never run ahead of the engine's own first paint of
   the level's chrome. The GUI chrome re-emit (tagpu_gui_hook.c) gates on it. */
int tagpu_packet_pub_level_open(void);
#endif
