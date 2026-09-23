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
/* the particle layers (tagpu_sfx.on): one more detour, its own skip byte */
void tagpu_fxown_set_skip_sfx(int on);
void tagpu_fxown_beat_sfx(unsigned int frame_counter);
/* WHETHER THE PACKET'S PUBLISHER FILLS THE EFFECT TABLES. The
   render thread raises these from its arming check — once per gathered frame,
   PASSIVE OR NOT, because a passive pass still counts and logs what it would
   have drawn — and `tagpu_fxown_flush` drops them after 90 silent present
   frames, the same watchdog the two skip bytes already stand on. They are our
   own bytes, one writer each, and they cost the publisher a whole walk of the
   engine's effect arrays when set: an unarmed pass must not pay for it. */
void tagpu_fxown_set_want(int fx, int sfx, unsigned int frame_counter);
int  tagpu_fxown_want_fx(void);
int  tagpu_fxown_want_sfx(void);
#endif
