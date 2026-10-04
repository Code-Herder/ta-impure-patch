#ifndef TAGPU_WINDOW_H
#define TAGPU_WINDOW_H
#include <windows.h>

/* The resize policy belongs to the actual window, including intervals with no
   DirectDraw object. Attachment runs on its owner thread. A subclass observes
   destruction even while dd_Release has restored the engine's window procedure. */
int tagpu_window_attach(HWND hwnd);

/* A nonzero ID for this window lifetime; zero once destruction starts. IDs
   never wrap or repeat in the process. GetProp reads no engine or proxy memory. */
LONG tagpu_window_generation(HWND hwnd);
#endif
