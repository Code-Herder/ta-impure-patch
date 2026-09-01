#ifndef MOUSE_H
#define MOUSE_H

void mouse_lock();
void mouse_unlock();
LRESULT CALLBACK mouse_hook_proc(int Code, WPARAM wParam, LPARAM lParam);

extern BOOL g_mouse_locked;

/* tagpu: TRUE when the game must not move or fence the real pointer
   (trigger file tagpu_nowarp.on next to the exe). */
BOOL tagpu_mouse_nowarp(void);
extern HHOOK g_mouse_hook;
extern HOOKPROC g_mouse_proc;

#endif
