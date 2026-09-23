/* render_vk.c -- the Vulkan renderer backend. It owns a render thread of its
 * own, runs the seam (`tagpu_overlay_draw`) once per frame and presents into
 * `g_ddraw.hwnd` through `tagpu_vk_frame`. See
 * research/notes/vulkan-only-plan.md landing 4.
 *
 * WHY A VULKAN SURFACE ON THE GAME'S HWND PRESENTS. The coexistence probe's
 * "API ok, pixels dead" verdict on wine 9.0 was about an OpenGL context sharing
 * the HWND: the Vulkan half presented 10 of 10 frames. No OpenGL context exists
 * in this process. Measured as route E, 98.5 % of the window showing Vulkan's
 * own frame, the same 98.5 % as the GDI control -- roadmap Phase G / G19a has
 * the table.
 *
 * AND WHY THE FALLBACK IS ALLOWED TO BE LATE. This hands the session to
 * `gdi_render_main` on `tagpu_vk_failed()`. That is only safe because route F
 * measured it: GDI still reaches the screen after a surface has existed on the
 * HWND, so winevulkan's takeover is specific to an OpenGL drawable rather than
 * to the window.
 * Had it not been, the fallback would have to be taken before the surface
 * was ever created and a later failure would be terminal.
 */

#include <windows.h>
#include "fps_limiter.h"
#include "dd.h"
#include "render_vk.h"
#include "render_gdi.h"
#include "debug.h"
#include "config.h"
#include "tagpu.h"
#include "tagpu_overlay.h"
#include "tagpu_packet.h"
#include "tagpu_reclaim.h"
#include "tagpu_menu.h"
#include "tagpu_vk.h"
#include "tagpu_ftime.h"


/* THE FRAME NUMBER OUTLIVES THE THREAD, and it has to. `dd_SetDisplayMode`
   joins this thread (`dd.c:747`, INFINITE) and `dd.c:1459` creates a new one on
   the next mode change, so a counter in automatic storage would restart at 0 --
   and every hand-over in the tree tests freshness by EXACT EQUALITY on this
   number (`tagpu_terr.c`'s "THE STAMP IS WHAT MAKES THE POINTERS ABOVE SAFE",
   and the same test in tagpu_fx.c and tagpu_posedraw.c). Equality against a
   counter that restarts is not a freshness test: a record published on the
   frame the old thread died, never consumed, would match again the same number
   of frames into the new thread's life and hand a pass pointers into buffers
   that were freed and rebuilt in between.

   SO IT IS A FILE STATIC. */
static unsigned s_frames = 0;

DWORD WINAPI vk_render_main(void)
{
    /* HOW MANY TIMES A LANE THAT HAD COME UP MAY FAIL AND BE BROUGHT BACK
       before the session gives up on Vulkan. A BOUND, not a timeout: each
       retry is a real re-bring-up (~400 ms measured), so a device that is
       genuinely gone reaches GDI in about a second and a transient stall costs
       one rebuild. See the loop for why the two cases are not the same case. */
#define VK_RETRY_BUDGET 3

    unsigned fc = 0;
    int gave_up = 0, came_up = 0, retries = 0;
    DWORD timeout;

    /* A QUARTER SECOND BEFORE ANYTHING ELSE. The render thread is
       started from the game thread's mode set-up and the window's client rect
       is not final until that has finished; a swapchain built against the
       intermediate size would be rebuilt on the first `VK_SUBOPTIMAL_KHR`
       anyway, so this costs nothing and saves the churn. */
    Sleep(250);

    fpsl_init();

    /* THE LATCH FIRST, BEFORE ANYTHING IN THE SEAM RUNS. After it the surface
       goes on the window handed to `tagpu_vk_frame`, route D's window is never
       created, and the levers stop deciding because `renderer=vulkan` already
       did. See tagpu_vk.h.

       IT MUST PRECEDE `tagpu_vk_enum_start`, and that is an ordering rather
       than a preference: the enumeration's own `tagpu_vk.off` check asks the
       latch whether that file is allowed to stop anything, so a latch set
       afterwards would let a stale OFF file skip the device list AND log that
       it had stopped the whole module -- on a launch where the lane then runs. */
    tagpu_vk_own_present();

    /* The GPU row, once per launch and on a thread of its own: it runs whether
       or not this backend is the one selected, because the row outlives the
       lane, and it must not happen under the loader lock (tagpu_vk.h). */
    tagpu_vk_enum_start();

    timeout = g_config.minfps > 0 ? g_ddraw.minfps_tick_len : INFINITE;

    while (g_ddraw.render.run &&
        (g_config.minfps < 0 || WaitForSingleObject(g_ddraw.render.sem, timeout) != WAIT_FAILED) &&
        g_ddraw.render.run)
    {
#if _DEBUG
        dbg_draw_frame_info_start();
#endif

        fpsl_frame_start();

        /* `g_ddraw.render.clear_screen` IS DELIBERATELY NOT TOUCHED. Only
           `render_gdi.c` consumes that flag, because a backend that clears its
           own target every frame has nothing to do with it -- and the seam
           clears the whole swapchain image on every frame it presents.

           And consuming it would be WRONG rather than merely idle: the lane
           presents nothing while the bring-up runs (~420 ms measured), so a
           mode change that set the flag in that window would have its clear
           eaten by a frame that painted nothing. Left standing, the flag is
           still there for the GDI fallback below, which is the one path that
           owes it. */

        /* ONE NUMBER FOR THE WHOLE FRAME, TAKEN BEFORE ANYTHING USES IT. Every
           pass's hand-over is stamped with `TAGPU_FRAME::frame_counter` and the
           Vulkan pass refuses one published on any other frame -- a safety
           property and not tidiness (tagpu_vk_pass.h) -- so the driver below
           and the present call must be given the SAME value, and a
           post-increment inside either call would silently differ by one. */
        fc = s_frames++;

        /* THE DRIVER. `tagpu_overlay_draw` is the single per-frame entry point
           for everything this fork adds: input injection, the trigger-file
           readers, the own-draw flushes, the palette, the zoom's one read of
           the eye, the roster logs. */
        {
            TAGPU_FRAME f;
            f.struct_size   = sizeof(f);
            f.abi           = TAGPU_ABI;
            f.game_width    = g_ddraw.width;
            f.game_height   = g_ddraw.height;
            f.vp_x          = g_ddraw.render.viewport.x;
            f.vp_y          = g_ddraw.render.viewport.y;
            f.vp_w          = g_ddraw.render.viewport.width;
            f.vp_h          = g_ddraw.render.viewport.height;
            f.win_width     = g_ddraw.render.width;
            f.win_height    = g_ddraw.render.height;
            f.hwnd          = g_ddraw.hwnd;
            f.hdc           = g_ddraw.render.hdc;
            f.frame_counter = fc;
            f.bpp           = g_ddraw.bpp;
            /* The frame packet, taken ONCE here and handed to every pass
               through the struct; both pointers die at `frame_end`, which is
               unconditional for the reason the reclaim bracket's is. */
            f.packet        = tagpu_packet_acquire(&f.packet_prev);
            tagpu_reclaim_pass_begin();
            tagpu_overlay_draw(&f);
            tagpu_reclaim_pass_end(f.frame_counter);
            tagpu_packet_frame_end(f.frame_counter);
            /* The render-options screen's deferred cfg write, off the game
               thread because TA is lockstep. Nothing happens on a frame with
               no click. */
            tagpu_menu_present();
        }

        /* THE FRAME-TIME LEVER IS POLLED HERE OR NOWHERE. `tagpu_ftime_poll`
           is what sets the module's `s_on`, and this is its one caller: without
           it the whole instrument is inert for the session -- no Vulkan
           timestamps written (`tagpu_vk.c` gates them on
           `tagpu_ftime_armed()`), every sample discarded, and not one line in
           the log.

           WHAT IT CAN AND CANNOT ANSWER is in tagpu_ftime.h: this build's GPU
           time against a previous build's. */
        tagpu_ftime_poll();
        if (tagpu_vk_frame(g_ddraw.hwnd, g_ddraw.render.width, g_ddraw.render.height,
                           g_config.vsync, fc))
            came_up = 1;

        /* THE LOOP'S OWN EXIT WINS OVER A FAILURE, and the order is free. A
           frame that both fails and finds `render.run` clear is a shutdown or a
           mode change, not a verdict on the lane -- and honouring the failure
           first would latch `g_ddraw.renderer` to GDI, so a MODE CHANGE would
           restart the render thread as GDI for good. */
        if (!g_ddraw.render.run)
            break;

        /* A LANE THAT HAS GIVEN UP IS A FACT, NOT A FRAME COUNT -- but WHICH
           fact matters. ST_FAILED covers both "the bring-up cannot work" and "a
           live lane hit an error", and only the first is a reason to give the
           session to software rendering: the second includes a one-second fence
           or acquire timeout and a single refused allocation, either of which
           can happen to a lane that has been presenting for ten minutes.

           `gave_up` is latched rather than re-asked because
           `tagpu_vk_render_stop` below takes the lane back to ST_OFF and the
           answer would be lost. */
        if (tagpu_vk_failed()) {
            if (!came_up) { gave_up = 1; break; }
            if (retries >= VK_RETRY_BUDGET) {
                TRACE("     Vulkan lane failed %d times after coming up - giving up\n",
                      retries);
                gave_up = 1;
                break;
            }
            retries++;
            tagpu_vk_retry();
        }

#if _DEBUG
        dbg_draw_frame_info_end();
#endif

        fpsl_frame_end();
    }

    /* Every exit from the loop is a mode change, a shutdown or a lane that
       failed, and all three invalidate what the surface was made on -- so the
       lane comes down here, on the thread that owns it, rather than being left
       to discover a dead window. */
    tagpu_vk_render_stop();

    if (gave_up)
    {
        /* THE HAND-OVER TO GDI. The renderer pointer is moved first so
           nothing downstream still believes this backend is live, then GDI is
           called and never returns. The warning text GDI prints is the fork's
           own.

           NOT COVERED, and it is a real loss: the GPU row's retry. Once the
           session is on GDI the Vulkan lane is down for the process's life,
           because this thread is GDI's now, so picking another device cannot
           bring it back. The alternative -- sitting in this loop presenting
           nothing while the player hunts for a device that works -- leaves
           them with no picture at all, which is worse. */
        TRACE("     Vulkan backend did not come up - switched to GDI renderer\n");
        g_ddraw.show_driver_warning = TRUE;
        g_ddraw.renderer = gdi_render_main;
        gdi_render_main();
    }

    return TRUE;
}
