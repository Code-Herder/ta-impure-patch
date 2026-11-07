#ifndef TAGPU_MODMEM_H
#define TAGPU_MODMEM_H
/* tagpu_modmem -- the exe's large-address bit, which must be in its header before the system loads the process (research/notes/modules/mods.md, *4 GB*).

   A running exe can be renamed but not overwritten, so the new file is written whole beside it first and the two renames follow: at no point is a partly
   written file named TotalA.exe. A key that identifies the exe ignores this bit, so every setup that knows the exe still does.

   Files only, no engine memory. */
#include <stddef.h>

/* Whether the exe on disk has the bit. 1, 0, or -1 when it could not be read. */
int  tagpu_modmem_exe_has_bit(void);
/* The exe made as a catalog entry's MODCAT_MEMORY_* asks: 4GB sets the bit and keeps the old file as TotalA.exe.pre-4gb, BIT_OFF puts that back, LEAVE does
   nothing, so the bit stays on after a mod that set it. 1 when the exe is as asked, changed or not; 0 with the reason in `why` and the exe as it was. */
int  tagpu_modmem_apply(int memory, char* why, size_t nwhy);
/* At start-up: the file BIT_OFF moved aside, which the process before this one was still running from. */
void tagpu_modmem_sweep(void);

#endif
