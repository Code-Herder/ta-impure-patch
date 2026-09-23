/* tagpu.h — the frame every pass receives.
   Two fillers: render_vk.c on the render thread, once per presented frame, and
   tagpu_gui_hook.c's before_flip on the GAME thread for the trigger family, so
   that family reaches renderer=gdi too. The game-thread one leaves packet NULL,
   and that is the family's membership test: a consumer that dereferences packet
   cannot be called from it. tagpu_input.c is split along exactly that line --
   the token half takes this frame, the camera hold keeps the render thread's. */
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
    /* The frame packet exchange (tagpu_packet.h): the packet the driver took
       for this frame and the previous one it still holds, or NULL. Valid until
       the driver's frame_end; a module that caches either past its frame holds
       a slot the producer may be filling — the poison lever exists to catch
       exactly that. */
    const struct TAGPU_PACKET* packet;
    const struct TAGPU_PACKET* packet_prev;
} TAGPU_FRAME;

#endif
