/* arttest.c -- prints every frame tagpu_art.c hands out, for tests/arttest.py.

   One line per frame: style, entry, code, w, h, hotx, hoty, key and the FNV-1a hash of
   its indices. arttest.py prints the same from the PNGs in art/ui/ through artbake.py's
   own reader and compares the two, so a bake and a reader that disagree cannot both pass. */
#include <windows.h>
#include <stdio.h>
#include "tagpu_art.h"

HMODULE g_ddraw_module;            /* tagpu_log.c names it; NULL is this exe */

int main(void)
{
    const TAGPU_ART_ENTRY* e;
    int i, f;

    if (!tagpu_art_init()) { puts("NO ART"); return 1; }
    for (i = 0; (e = tagpu_art_at(i)) != NULL; i++)
        for (f = 0; f < e->nframes; f++) {
            const TAGPU_ART_FRAME* fr = &e->frames[f];
            unsigned h = 2166136261u, k, n = (unsigned)fr->w * fr->h;
            for (k = 0; k < n; k++) h = (h ^ fr->pix[k]) * 16777619u;
            printf("%s %s %u %u %u %d %d %u %08x\n", e->style, e->name, fr->code, fr->w, fr->h,
                   fr->hotx, fr->hoty, fr->key, h);
        }
    return 0;
}
