#ifndef TAGPU_FXOWN_H
#define TAGPU_FXOWN_H
/* The engine draws no effect on a Vulkan launch: its projectile pass, the
   draw leaves of its explosion pass and its particle-layer walker are removed
   at DllMain, byte-match guarded, unless the launch is the tagpu_gdi.on oracle
   (tagpu_fxown.c has the argument). */
void tagpu_fxown_init(void);
void tagpu_fxown_flush(unsigned int frame_counter);
/* WHETHER THE PACKET'S PUBLISHER FILLS THE EFFECT TABLES. The
   render thread raises these from its arming check — once per gathered frame,
   PASSIVE OR NOT, because a passive pass still counts and logs what it would
   have drawn — and `tagpu_fxown_flush` drops them after 90 silent present
   frames. They are our own bytes, one writer each, and they cost the publisher
   a whole walk of the engine's effect arrays when set: an unarmed pass must not
   pay for it. */
void tagpu_fxown_set_want(int fx, int sfx, unsigned int frame_counter);
int  tagpu_fxown_want_fx(void);
int  tagpu_fxown_want_sfx(void);
#endif
