#ifndef KEYBOARD_H
#define KEYBOARD_H


void keyboard_hook_init();
void keyboard_hook_exit();
LRESULT CALLBACK keyboard_hook_proc(int Code, WPARAM wParam, LPARAM lParam);

extern HHOOK g_keyboard_hook;

#endif
