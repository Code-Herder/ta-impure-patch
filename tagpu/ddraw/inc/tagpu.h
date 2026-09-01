/* tagpu.h — shared contract between our cnc-ddraw fork and tagpu.dll.
   The fork fills a TAGPU_FRAME each frame (on its render thread, GL context current)
   and calls tagpu.dll!TagpuPresent just before SwapBuffers. */
#ifndef TAGPU_H
#define TAGPU_H

#include <windows.h>

#define TAGPU_ABI 1

typedef struct TAGPU_FRAME {
    unsigned int struct_size;    /* sizeof(TAGPU_FRAME) — ABI guard              */
    unsigned int abi;            /* TAGPU_ABI                                     */
    int   game_width, game_height;   /* g_ddraw.width/height (logical, e.g. 640x480) */
    int   vp_x, vp_y, vp_w, vp_h;    /* letterboxed game viewport in window px       */
    int   win_width, win_height;     /* g_ddraw.render.width/height (drawable)       */
    void* hwnd;                      /* g_ddraw.hwnd                                 */
    void* hdc;                       /* g_ddraw.render.hdc                           */
    unsigned int frame_counter;      /* monotonic, maintained by the fork            */
    int   bpp;                       /* g_ddraw.bpp (8 for TA)                        */
} TAGPU_FRAME;

typedef void (__cdecl *TagpuPresentProc)(const TAGPU_FRAME*);

#endif
