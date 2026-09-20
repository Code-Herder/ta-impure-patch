#ifndef TAGPU_FEATOWN_H
#define TAGPU_FEATOWN_H
/* Own the feature draw: skip the engine's one feature leaf 0x46A610 while
   the native feature pass (tagpu_feat.on) supplies the pixels. The code patch
   installs ONCE at DllMain and only when tagpu_featown.on exists then; the
   skip itself follows tagpu_feat.on live, so an engine-vs-ours A/B is a file
   flip with no relaunch. */
void tagpu_featown_init(void);
void tagpu_featown_flush(unsigned int frame_counter);
void tagpu_featown_set_skip(int on);
void tagpu_featown_beat(unsigned int frame_counter);   /* "we drew this frame" */
int  tagpu_featown_installed(void);
#endif
