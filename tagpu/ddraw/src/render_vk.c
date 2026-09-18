/* render_vk.c -- the Vulkan renderer backend (the vulkan-only plan, landing 4a).
 *
 * WHAT THIS PART IS, AND WHAT IT IS NOT. It gives the Vulkan lane a render
 * thread of its own and presents into `g_ddraw.hwnd`, and that is all. It does
 * NOT call `tagpu_overlay_draw`, so no pass's gather half runs and the frame is
 * the seam's clear colour and nothing else. That is deliberate and it is
 * measurable on its own: the question this part answers is whether a swapchain
 * lives on the game's own window inside the game, which nothing has run yet.
 *
 * The parts after it: 4b runs the gathers and stands the GL draws down, 4c adds
 * the `ss` offscreen target and moves TA's surface upload into the backend, 4d
 * deletes route D's window. See research/notes/vulkan-only-plan.md landing 4.
 *
 * WHY THE PRESENT WORKS ON A WINDOW ROUTE A CALLED DEAD. `tools/vkcoexist.c`
 * route A -- a GL context current on an HWND, then Vulkan on the same one --
 * reads "API ok, pixels dead" on wine 9.0, and that verdict is about GL: its
 * VULKAN half presented 10 of 10 frames. This backend creates no GL context, so
 * the half that failed is not in the process. Measured as route E, 98.5 % of the
 * window showing Vulkan's own frame against a GL and a GDI control at the same
 * 98.5 % -- roadmap Phase G / G19a has the table.
 *
 * AND WHY THE FALLBACK IS ALLOWED TO BE LATE. `ogl_render_main` hands the
 * session to `gdi_render_main` when GL will not come up and this does the same
 * on `tagpu_vk_failed()`. That is only safe because route F measured it: GDI
 * still reaches the screen after a surface has existed on the HWND, so
 * winevulkan's takeover is specific to GL's drawable rather than to the window.
 * Had it not been, the fallback would have had to be taken before the surface
 * was ever created and a later failure would have been terminal.
 *
 * WHAT IT DELIBERATELY DOES NOT TOUCH IN dd.c, checked site by site 2026-09-17
 * rather than assumed -- every one of these is a WGL workaround that a Vulkan
 * swapchain must not inherit, and adding `|| renderer == vk_render_main` to any
 * of them would be a silent bug:
 *
 *   dd.c:849, :994   `nonexclusive = TRUE` -- stops WGL going fullscreen
 *                    exclusive. A swapchain is windowed by construction.
 *   dd.c:1119        `render.height++` and `opengl_y_align = 1` -- a scanline
 *                    added so the driver cannot take exclusive mode, and the
 *                    viewport shift that pays for it. `opengl_y_align` is read
 *                    in render_ogl.c and nowhere else; a Vulkan frame that
 *                    inherited it would be one pixel tall too many and offset.
 *   dd.c:1264, :1353 `ogl_create()` on the window, GDI on failure. The Vulkan
 *                    bring-up is the render thread's own and needs no hook here.
 *   dd.c:1524        `SetPixelFormat`. Already gated on the GL backend, and
 *                    route E's measurement is of a window without one.
 *   dd.c:1787        `ogl_release()`. Nothing to release; `tagpu_vk_render_stop`
 *                    runs below, on the thread that owns the lane.
 */

#include <windows.h>
#include "fps_limiter.h"
#include "dd.h"
#include "render_vk.h"
#include "render_gdi.h"
#include "tagpu_vk.h"
#include "debug.h"
#include "config.h"


DWORD WINAPI vk_render_main(void)
{
    unsigned frames = 0;
    int gave_up = 0;
    DWORD timeout;

    /* THE SAME QUARTER SECOND `ogl_render_main` TAKES. The render thread is
       started from the game thread's mode set-up and the window's client rect
       is not final until that has finished; a swapchain built against the
       intermediate size would be rebuilt on the first `VK_SUBOPTIMAL_KHR`
       anyway, so this costs nothing and saves the churn. */
    Sleep(250);

    fpsl_init();

    /* The GPU row, once per launch and on a thread of its own: it runs whether
       or not this backend is the one selected, because the row outlives the
       lane, and it must not happen under the loader lock (tagpu_vk.h). */
    tagpu_vk_enum_start();

    /* THE LATCH, BEFORE THE FIRST FRAME AND ONCE. After this the surface goes
       on the window handed to `tagpu_vk_frame` and route D's window is never
       created; `tagpu_vk.on` stops arming the lane because `renderer=vulkan`
       already did. See tagpu_vk.h. */
    tagpu_vk_own_present();

    timeout = g_config.minfps > 0 ? g_ddraw.minfps_tick_len : INFINITE;

    while (g_ddraw.render.run &&
        (g_config.minfps < 0 || WaitForSingleObject(g_ddraw.render.sem, timeout) != WAIT_FAILED) &&
        g_ddraw.render.run)
    {
#if _DEBUG
        dbg_draw_frame_info_start();
#endif

        fpsl_frame_start();

        /* `g_ddraw.render.clear_screen` IS DELIBERATELY NOT TOUCHED, and the
           reason is worth recording because the first version of this file did
           consume it. Only `render_gdi.c` consumes that flag; `render_ogl.c`
           never reads it, because a backend that clears its own target every
           frame has nothing to do with it. This one is in the same position --
           the seam clears the whole swapchain image on every frame it presents.

           And consuming it would have been WRONG rather than merely idle: the
           lane presents nothing while the bring-up runs (~420 ms measured), so
           a mode change that set the flag in that window would have had its
           clear eaten by a frame that painted nothing. Left standing, the flag
           is still there for the GDI fallback below, which is the one path that
           owes it. */

        /* `frames` is post-incremented so the number passed is this frame's,
           counting from 0. WHEN 4b WIRES THE GATHERS, the same number has to
           reach `TAGPU_FRAME::frame_counter` FIRST and this call second: a
           Vulkan pass refuses a hand-over stamped with any other frame, which
           is a safety property and not tidiness (tagpu_vk_pass.h). The GL lane
           gets that ordering by filling the struct earlier in its loop and
           passing `g_tagpu_frames - 1u` here. */
        tagpu_vk_frame(g_ddraw.hwnd, g_ddraw.render.width, g_ddraw.render.height,
                       g_config.vsync, frames++);

        /* A LANE THAT HAS GIVEN UP IS A FACT, NOT A FRAME COUNT. `tagpu_vk.c`
           publishes ST_FAILED after every path the bring-up could have
           succeeded on, so there is nothing to wait for and nothing to guess.
           Latched here because `tagpu_vk_render_stop` below takes the lane back
           to ST_OFF and the answer would be lost. */
        if (tagpu_vk_failed()) { gave_up = 1; break; }

        if (!g_ddraw.render.run)
            break;

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
        /* THE SAME HAND-OVER `ogl_render_main` MAKES, and the same shape: the
           renderer pointer is moved first so nothing downstream still believes
           this backend is live, then GDI is called and never returns. The
           warning text GDI prints is the fork's own.

           NOT COVERED, and it is a real loss: the GPU row's retry. While the
           lane was a lever beside GL, a failed bring-up could be retried by
           picking another device (`lane_gen() != s_choiceSeen`). Once the
           session is on GDI the Vulkan lane is down for the process's life,
           because this thread is GDI's now. The alternative -- sitting in this
           loop presenting nothing while the player hunts for a device that
           works -- leaves them with no picture at all, which is worse. */
        TRACE("     Vulkan backend did not come up - switched to GDI renderer\n");
        g_ddraw.show_driver_warning = TRUE;
        g_ddraw.renderer = gdi_render_main;
        gdi_render_main();
    }

    return TRUE;
}
