#ifndef TAGPU_MODFIT_H
#define TAGPU_MODFIT_H
/* tagpu_modfit -- what a base mod holds at a path, read from its files: is a mutator still built on the file it replaces? (tagpu_modcat.h)

   Files only: no engine memory, no render state, so any one thread may hold a MODFIT. The answer is the first source holding the path as a
   file, in the order the mount gives them: mods\<base>\<base>.hpi, its originals, then the folder's archives. The base "none" is the folder's
   own game, its archives as the engine would mount them. A loose file is not looked at: see research/notes/modules/mods.md, *Mutators*. */
#include <stdint.h>

#define MODFIT_NAME 64
#define MODFIT_ARCS 64

typedef struct MODFIT MODFIT;

/* `names` the folder's archives in mount order; those the base hides are left out here. NULL only when memory runs out. */
MODFIT* tagpu_modfit_open(const char* base, const char (*names)[MODFIT_NAME], int n);
void    tagpu_modfit_close(MODFIT* f);
/* MODCAT_HASH, with the MODFIT as ctx. */
int     tagpu_modfit_hash(void* f, const char* path, uint8_t out[32]);
/* The archives the engine's mount patterns find in this folder, in its order (InitTAHPIAry 0x41D4C0): how many went into `names`. */
int     tagpu_modfit_folder(char (*names)[MODFIT_NAME], int max);

#endif
