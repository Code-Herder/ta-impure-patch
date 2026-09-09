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

/* tagpu: TRUE when the game must not move or fence the real pointer
   (trigger file tagpu_nowarp.on next to the exe). */
BOOL tagpu_mouse_nowarp(void);
extern HHOOK g_mouse_hook;
extern HOOKPROC g_mouse_proc;

#endif
