#ifndef TAGPU_FEATOWN_H
#define TAGPU_FEATOWN_H
/* The engine draws no feature on a Vulkan launch: its one feature leaf
   0x46A610 is removed at DllMain, byte-match guarded, unless the launch is the
   tagpu_gdi.on oracle (tagpu_featown.c has the argument). */
void tagpu_featown_init(void);
#endif
