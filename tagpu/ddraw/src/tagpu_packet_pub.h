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
   latches the colour. Replaces tagpu_text_snapshot(): no render-thread code
   dereferences an engine font any more. */
void tagpu_packet_pub_font_snapshot(void);
/* Game thread, from reclaim_teardown_post after the level generation was
   bumped: publish a header-only packet with in_game = 0 and the new
   generation, forced past the FRESH gate, so the renderer stops drawing the
   dead level over the menus and the loading screen. */
void tagpu_packet_pub_level_end(unsigned level_gen);
#endif
