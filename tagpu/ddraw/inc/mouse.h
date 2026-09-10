#ifndef MOUSE_H
#define MOUSE_H

void mouse_lock();
void mouse_unlock();
LRESULT CALLBACK mouse_hook_proc(int Code, WPARAM wParam, LPARAM lParam);

extern BOOL g_mouse_locked;

/* tagpu (G17b): the ONE client-area -> engine-logical transform, shared by
   wndproc's button cases and the harness's device-space injection so the two
   cannot drift. 1 = the point was inside the letterboxed viewport; outside it
   the outputs come back as the centre of the engine's screen, as they always
   have. */
int mouse_client_to_game(int cx, int cy, int* gx, int* gy);

/* tagpu (G17c): the last CLIENT-AREA point a real message carried, which is
   the pointer at device resolution rather than the engine's logical grid.
   mouse_last_client returns 0 when none is known -- before the first message,
   outside the letterboxed viewport, and after an injected click, which moves
   the engine's cursor with no pointer behind it. The GL UI renderer draws its
   cursor from this and falls back to the engine's own position when it is
   absent (gui-renderer.md 13.5). */
void mouse_note_client(int cx, int cy);
void mouse_forget_client(void);
int  mouse_last_client(int* cx, int* cy);

/* tagpu: TRUE when the game must not move or fence the real pointer
   (trigger file tagpu_nowarp.on next to the exe). */
BOOL tagpu_mouse_nowarp(void);
extern HHOOK g_mouse_hook;
extern HOOKPROC g_mouse_proc;

#endif
