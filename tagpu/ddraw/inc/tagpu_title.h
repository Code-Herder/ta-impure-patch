/* tagpu_title — the window title says which tree's DLL is running.
 *
 * Several instances of the same 1997 binary sit on the desktop at once, one per
 * worktree, and the stock title, "Total Annihilation", is the same on every
 * window whichever build it runs. tacli writes a label into
 * tagpu_title.txt in the gamedir (the branch of the tree it was run from, which
 * is also the tree whose ddraw.dll it pinned, or --title) and we append it:
 *
 *     Total Annihilation - worktree-gpu_render
 *
 * The file is the ONLY source. No file, no change — a game launched outside
 * tacli keeps the stock title. That matters because tacli finds the client
 * window BY TITLE (Instance.window()), so the two halves have to agree about
 * the exact string.
 */
#ifndef TAGPU_TITLE_H
#define TAGPU_TITLE_H

#include <windows.h>

/* Compose "<base> - <tagpu_title.txt>" onto hwnd.
 *
 * `base` is the unsuffixed title, and only the FIRST call for a given hwnd is
 * believed: dd.c re-reads it off the live window inside a gate that re-opens
 * (see tagpu_title.c), so a later call would hand back our own composed title
 * and stack a second suffix. Idempotent and cheap (one file open, and no window
 * write when the title is already right), so it is safe on every
 * dd_SetCooperativeLevel. */
void tagpu_title_apply(HWND hwnd, const char* base);

#endif /* TAGPU_TITLE_H */
