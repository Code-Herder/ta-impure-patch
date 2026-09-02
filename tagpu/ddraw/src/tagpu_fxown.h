#ifndef TAGPU_FXOWN_H
#define TAGPU_FXOWN_H
/* Own the effects draw: skip the engine's projectile pass and the draw
   leaves of its explosion pass while the native effects pass (tagpu_fx.on)
   supplies the pixels. Code patches install ONCE at DllMain, only when
   tagpu_fxown.on exists then; the skip itself follows tagpu_fx.on live. */
void tagpu_fxown_init(void);
void tagpu_fxown_flush(unsigned int frame_counter);
void tagpu_fxown_set_skip(int on);
void tagpu_fxown_beat(unsigned int frame_counter);   /* "we drew this frame" */
int  tagpu_fxown_installed(void);
#endif
