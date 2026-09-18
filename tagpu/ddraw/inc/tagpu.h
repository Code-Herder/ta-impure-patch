/* tagpu.h — shared contract between our cnc-ddraw fork and tagpu.dll.
   The fork fills a TAGPU_FRAME each frame (on its render thread, GL context current)
   and calls tagpu.dll!TagpuPresent just before SwapBuffers.
   SINCE LANDING 10c-1 there is a SECOND filler, on the GAME thread with no GL
   context: tagpu_gui_hook.c's before_flip builds one for the trigger family, so
   that family reaches renderer=gdi. It leaves surface_tex 0 and packet NULL,
   and that is the family's membership test: a consumer that dereferences packet
   cannot be called from it. Landing 10c-2 split tagpu_input.c along exactly that
   line rather than excluding it -- the token half takes this frame, the camera
   hold keeps the render thread's. */
#ifndef TAGPU_H
#define TAGPU_H

#include <windows.h>

#define TAGPU_ABI 1

struct TAGPU_PACKET;             /* tagpu_packet.h; opaque here */

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
    /* G13b: the GL id of the engine's own 8bpp frame, an R8 INDEX texture whose
       texel (x,y) is game pixel (x,y) (0 when not 8bpp / not uploaded). The
       composite reads it to find the pixels the engine still paints inside the
       viewport once we own the terrain — see tagpu_terrown.c. */
    unsigned int surface_tex;
    /* The frame packet exchange (tagpu_packet.h): the packet the driver took
       for this frame and the previous one it still holds, or NULL. Valid until
       the driver's frame_end; a module that caches either past its frame holds
       a slot the producer may be filling — the poison lever exists to catch
       exactly that. Appended, so the struct only grew. */
    const struct TAGPU_PACKET* packet;
    const struct TAGPU_PACKET* packet_prev;
} TAGPU_FRAME;

typedef void (__cdecl *TagpuPresentProc)(const TAGPU_FRAME*);

#endif
