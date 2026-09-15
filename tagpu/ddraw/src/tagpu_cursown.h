#ifndef TAGPU_CURSOWN_H
#define TAGPU_CURSOWN_H
/* tagpu_cursown.h — the engine's cursor BLIT, skipped while ours is on screen.

   The fourth of the own-the-draw modules (terrown, featown, fxown, markown),
   and the smallest: it patches no function, only the single `call` that puts
   the cursor's pixels on a surface, at each of the four sites that do it.

   WHY THE ENGINE'S CURSOR HAD TO GO. The world composite paints our frame
   wherever the engine's surface holds the terrain key and discards elsewhere,
   so anything the engine draws after the key fill shows through — its cursor
   included. Erasing that cursor by its rect cannot work: the rect the packet
   carries is read inside DrawGameScreen and the engine blits its cursor later,
   inside the flip, so the rect is a frame of pointer motion behind the sprite
   actually composited, and the cursors PULSE (27, 29 … 35 px), so the outer
   ring leaks even when the pointer is still. Both were reported as ghosts.

   WHY A CALL SITE. See tagpu_detour.c's tagpu_detour_call_site: the engine's
   draw is not self-contained. 0x4C67C0 fills the saved-background descriptor,
   writes +0x1B6/+0x1BA and calls the background SAVE before it blits, and its
   caller RESTORES that background unconditionally afterwards. Taking the
   function over leaves the restore unpaired and freezes the position words
   every consumer of ours still reads; taking the blit over leaves all of it
   running and removes exactly the pixels. */

/* Install, at DllMain, on the game thread. Byte-matched and all-or-nothing per
   site; a site that does not match arms nothing and says so in tagpu.log. */
void tagpu_cursown_init(void);

/* THE PUBLICATION, from the render thread, ONCE per frame, from the frame
   bracket in render_ogl.c — NOT from inside the GL UI's present.

   `oursDrawn` is "the present that just ended really drew our cursor". It must
   be published from the bracket because the bracket is the only point every
   path through tagpu_overlay_draw reaches: the overlay returns early for
   `tagpu_overlay.off`, for a failed GL init and for a level teardown, and a
   flag left at its last value across any of those suppresses the engine's
   cursor while ours is not being drawn — no cursor at all, for as long as the
   condition lasts. That is the failure mode this signature exists to prevent,
   and it is why the argument is passed in rather than read out of the GUI
   module: the caller cannot forget a path it does not know about. */
void tagpu_cursown_publish(int oursDrawn);

/* for the heartbeat: how many of the sites are armed, out of how many, and
   the flag as last published.
   NO skip counter: counting one would mean a callback in the stub, and the
   stub staying a compare and a `ret` is most of why this patch is safe. What
   the suppression is doing is measured from the outside instead — the engine's
   +0x1B6/+0x1BA still TRACK the pointer under this design (the position writes
   are upstream of the blit), so the oracle is the screen, not a counter. */
void tagpu_cursown_stats(int* armed, int* ofN, int* skipping);
#endif
