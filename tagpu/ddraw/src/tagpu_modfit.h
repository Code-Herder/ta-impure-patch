#ifndef TAGPU_MODFIT_H
#define TAGPU_MODFIT_H
/* tagpu_modfit -- what a base mod holds at a path, read from its files: is a mutator still built on the file it replaces? (tagpu_modcat.h)

   Files only: no engine memory, no render state, so any one thread may hold a MODFIT. The answer is the first source holding the path as a
   file, in the order the mount gives them: mods\<base>\<base>.hpi, its originals, then the folder's archives by the engine's patterns and
   order (InitTAHPIAry 0x41D4C0). The base "none" is the folder's own game, every archive retail's patterns find; a drop-in exe that renames the first pattern
   mounts one this does not read. A loose file is not looked at:
   see research/notes/modules/mods.md, *Mutators*. */
#include <stdint.h>

typedef struct MODFIT MODFIT;

/* Under a mod, `original` says which of the folder's archives can mount, and those the base hides are left out too. Nothing bounds how many
   archives a folder holds, as nothing does in the engine. NULL only when memory runs out. */
MODFIT* tagpu_modfit_open(const char* base, int (*original)(const char* name));
void    tagpu_modfit_close(MODFIT* f);
/* MODCAT_HASH, with the MODFIT as ctx. */
int     tagpu_modfit_hash(void* f, const char* path, uint8_t out[32]);

#endif
