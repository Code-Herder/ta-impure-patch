#ifndef TAGPU_TERROWN_H
#define TAGPU_TERROWN_H
/* Own the terrain draw: skip the engine's terrain pass 0x483FA0 while the
   native terrain pass (tagpu_terr.on) supplies the pixels, and skip the fog
   overlay 0x4848E0 with it (we reproduce the fog rule ourselves since G13c,
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
/* bumped by every successful fill. The composite watches it stall: if the game
   thread draws frames that never reach 0x483FA0 (a screen other than the world),
   the engine's surface stops carrying our key and inverting against it would
   black out that screen. A stall simply stops the inversion, which leaves our
   own FBO covering the viewport — always safe, and self-correcting. */
unsigned tagpu_terrown_fill_seq(void);
#endif
