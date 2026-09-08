#ifndef TAGPU_OPT_H
#define TAGPU_OPT_H
/* tagpu_opt -- the play defaults: what is on when the player says nothing.

   Every pass is armed by a file beside TotalA.exe, `tagpu_<x>.on`, whose contents are
   its tokens, and renderers.md 2.10 makes those files the store the in-game menu will
   drive when it exists. Until it does, this table stands in for it: a pass on the table
   is ON, with the tokens listed in tagpu_opt.c, whenever its .on file is absent. The
   files keep their meaning --

     tagpu_<x>.on    exists: the pass is on and these are its tokens, as ever
     tagpu_<x>.off   exists (and no .on): a default-on pass is off
     tagpu_defaults.off  exists: the whole table is off -- every pass opt-in again,
                     which is what tacli's instances use, so a measurement arms
                     exactly what it names and a stock launch stays a control.

   A pass off the table (the instrumentation triggers, the knob files, the `.off`
   levers) is untouched: it reads its file as before. */

/* 1 when the pass is on: its .on file exists, or its default applies. */
int tagpu_opt_on(const char* onfile);

/* The pass's tokens into buf (NUL-terminated, at most cap-1 bytes): the file's
   contents when it exists, the table's tokens when the default applies. Returns
   the byte count, or -1 when the pass is off. */
int tagpu_opt_read(const char* onfile, char* buf, unsigned cap);

/* One line to tagpu.log saying which defaults apply at attach. */
void tagpu_opt_init(void);

#endif
