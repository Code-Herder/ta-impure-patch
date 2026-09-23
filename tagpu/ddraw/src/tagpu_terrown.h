#ifndef TAGPU_TERROWN_H
#define TAGPU_TERROWN_H
/* Own the terrain draw: skip the engine's terrain pass 0x483FA0 while the
   native terrain pass (tagpu_terr.on) supplies the pixels, and skip the fog
   overlay 0x4848E0 with it (we reproduce the fog rule ourselves,
   and its shade remap would rewrite our key fill into grey blobs).

   Two things make this gate different from featown:

   1. The terrain repaint is WHY the engine's offscreen never needs clearing.
      Suppressing it outright would leave last frame's garbage under the
      overlays the engine still draws. So the skip path does not just return —
      it fills the viewport rect of the offscreen with one palette index, the
      KEY. Every other index in that rect is then, by construction, something
      the engine drew after us (health bars, nanoframe wireframes, the build
      cursor, chat), which the composite in tagpu_native.c refuses to cover.

   2. The fog overlay is lazy: its first act is to rebuild the screen fog grid
      (0x4843C0) and set LosType bit3. Our own fog rule reads that grid, so the
      skip path replicates the rebuild exactly rather than dropping it.

   Both patches install ONCE at DllMain and only when tagpu_terrown.on exists
   then; the skip follows tagpu_terr.on live, so an engine-vs-ours A/B is a
   file flip with no relaunch. */
void tagpu_terrown_init(void);
void tagpu_terrown_flush(unsigned int frame_counter);
void tagpu_terrown_set_skip(int on);
void tagpu_terrown_beat(unsigned int frame_counter);   /* "we drew this frame" */
int  tagpu_terrown_installed(void);
/* 1 once the engine has actually run a key-filled frame under the current
   skip — the composite must not invert on the frame the skip is first set,
   when the engine's surface still holds a real terrain blit */
int  tagpu_terrown_filled(void);
/* The render thread's request -- 1 while it wants the engine's terrain and
   fog skipped. Read by the game thread's latch below, never by the stubs. */
int  tagpu_terrown_request(void);
/* GAME THREAD ONLY: decide, for the draws until the next latch, whether the
   engine's terrain and fog run or are skipped -- the one byte both stubs test.
   The packet publisher calls it at the top of every in-play draw AFTER the
   viewport rect is settled, with `request || tagpu_vpwide_wide()`, and at the
   level end with `tagpu_vpwide_wide()`, which keeps "the rect is wide => the
   engine's terrain blit is skipped" true at every moment (see
   `g_terrown_own`). */
void tagpu_terrown_latch(int own);
/* GAME THREAD, from the packet's publisher. The eye the ENGINE's own fog grid
   was last anchored at, latched inside the fog site the moment its builder ran
   (0x4843C0 recomputes the origin from those two words itself). 0 when this
   fork has never seen that builder run — terrain ownership disarmed, so the
   engine calls it where we cannot observe — and the publisher then falls back
   to the packet's own eye. */
int tagpu_terrown_fog_eye(int* x, int* y);
/* GAME THREAD, from the packet's publisher. 1 while our fog observer ran in
   THIS in-play draw — i.e. while terrain ownership is on and the site is ours.
   0 means the engine is calling its own fog builder where we cannot see it, and
   every fog answer either module holds is from whenever we last owned it. */
int tagpu_terrown_fog_site_live(void);
#endif
