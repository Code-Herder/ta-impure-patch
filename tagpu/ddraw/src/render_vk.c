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
 * WHAT IT DELIBERATELY DOES NOT INHERIT FROM THE GL BACKEND. Every
 * `g_ddraw.renderer == ogl_render_main` test IN THE TREE, checked site by site
 * 2026-09-17 rather than assumed -- each is a WGL workaround that a Vulkan
 * swapchain must not inherit, and adding `|| renderer == vk_render_main` to any
 * of them would be a silent bug. Line numbers are post-change:
 *
 *   dd.c:850, :995   `nonexclusive = TRUE` -- stops WGL going fullscreen
 *                    exclusive. Its ONE consumer is dd.c:1120, so leaving these
 *                    GL-only means exactly "no extra scanline".
 *   dd.c:1120        `render.height++` and `opengl_y_align = 1` -- a scanline
 *                    added so the driver cannot take exclusive mode, and the
 *                    viewport shift that pays for it. `opengl_y_align` is read
 *                    in render_ogl.c and nowhere else; a Vulkan frame that
 *                    inherited it would be one pixel tall too many and offset.
 *   dd.c:1265, :1354 `ogl_create()` on the window, GDI on failure. The Vulkan
 *                    bring-up is the render thread's own and needs no hook here.
 *   dd.c:1525        `SetPixelFormat`. Already gated on the GL backend, and
 *                    route E's measurement is of a window without one. The HDC
 *                    beside it is taken unconditionally, which is what leaves
 *                    the GDI fallback below a valid one.
 *   dd.c:1788        `ogl_release()`. Nothing to release, and dd.c joins the
 *                    render thread before reaching it, so `tagpu_vk_render_stop`
 *                    below has already run.
 *
 * AND THE TWO OUTSIDE dd.c, because scoping the audit to one file was itself a
 * gap the landing review found -- one of them is on this backend's PER-FRAME
 * path:
 *
 *   fps_limiter.c:153  reached from `fpsl_frame_end()` every frame. It is the
 *                    one site where the new backend does not merely skip GL
 *                    behaviour but takes a DIFFERENT branch
 *                    (`fpsl_dwm_flush() || fpsl_wait_for_vblank()` rather than
 *                    the vblank wait alone). `!IsWine()` makes it inert on the
 *                    reference setup, so it is not a live defect here -- but it
 *                    is a Windows-7-DWM workaround and the Vulkan present does
 *                    its own pacing, so GL-only is right on purpose and not by
 *                    accident.
 *   winapi_hooks.c:2039  `fake_DestroyWindow` -> `ogl_release()`. Nothing to
 *                    release. That same function is the ONLY writer that nulls
 *                    `g_ddraw.hwnd`, which is why `tagpu_vk.c`'s `s_ownGone`
 *                    latch exists rather than trusting `hwnd != s_owner`.
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
#include "tagpu_cursown.h"
#include "tagpu_gui.h"
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
   of frames into the new thread's life and hand a twin pointers into buffers
   that were freed and rebuilt in between.

   A FILE STATIC IS THE FIX AND IT MATCHES THE GL LANE, whose `g_tagpu_frames`
   (`render_ogl.c:47`) is a file static for the same reason. One backend is
   chosen per process and can only degrade to GDI, so the two counters can never
   both be live and cannot diverge. [FROM THE 4b-1 LANDING REVIEW, 2026-09-18.]
   */
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

    /* THE SAME QUARTER SECOND `ogl_render_main` TAKES. The render thread is
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

        /* ONE NUMBER FOR THE WHOLE FRAME, TAKEN BEFORE ANYTHING USES IT. Every
           pass's hand-over is stamped with `TAGPU_FRAME::frame_counter` and the
           Vulkan twin refuses one published on any other frame -- a safety
           property and not tidiness (tagpu_vk_pass.h) -- so the driver below
           and the present call must be given the SAME value, and a
           post-increment inside either call would silently differ by one. The
           GL lane gets the same ordering by filling the struct earlier in its
           loop and passing `g_tagpu_frames - 1u`. */
        fc = s_frames++;

        /* THE DRIVER. `tagpu_overlay_draw` is the single per-frame entry point
           for everything this fork adds, and most of it is already
           API-independent: input injection, the trigger-file readers, the
           own-draw flushes, the palette, the zoom's one read of the eye, the
           roster logs. The four entry points inside it that draw with GL --
           scaffold, native, GUI and fps -- stand down one per commit of this
           landing, each with its own A/B row; until a pass has stood down it
           simply draws nothing here, because there is no GL context for it to
           draw into. */
        {
            TAGPU_FRAME f;
            f.struct_size   = sizeof(f);
            f.abi           = TAGPU_ABI;
            f.game_width    = g_ddraw.width;
            f.game_height   = g_ddraw.height;
            f.vp_x          = g_ddraw.render.viewport.x;
            /* AND NO `opengl_y_align`, which is the one field the GL lane adds
               to and this one must not. It is the extra scanline `dd.c:1120`
               gives WGL so the driver cannot take exclusive mode; a Vulkan
               frame carrying it would be offset by a pixel. It is also why a
               previous-build A/B is only valid where that align is 0 -- see
               render_vk.h. */
            f.vp_y          = g_ddraw.render.viewport.y;
            f.vp_w          = g_ddraw.render.viewport.width;
            f.vp_h          = g_ddraw.render.viewport.height;
            f.win_width     = g_ddraw.render.width;
            f.win_height    = g_ddraw.render.height;
            f.hwnd          = g_ddraw.hwnd;
            f.hdc           = g_ddraw.render.hdc;
            f.frame_counter = fc;
            f.bpp           = g_ddraw.bpp;
            /* THERE IS NO GL TEXTURE, so this is 0 and every consumer of it
               must stand down rather than sample name 0. It is the engine's own
               8-bit frame as an R8 index texture, uploaded by the GL backend
               before any pass runs; the backend taking that upload over is 4c,
               and until then `tagpu_terrown.c` and anything else reading it has
               nothing. 0 is what the fork already hands a non-8bpp frame, so
               the value is in-contract rather than a sentinel of ours. */
            f.surface_tex   = 0;
            /* The frame packet, taken ONCE here and handed to every pass
               through the struct; both pointers die at `frame_end`, which is
               unconditional for the reason the reclaim bracket's is. */
            f.packet        = tagpu_packet_acquire(&f.packet_prev);
            tagpu_reclaim_pass_begin();
            tagpu_overlay_draw(&f);
            /* Published HERE and nowhere else: this is the only point every
               path through the driver reaches, and a flag published inside the
               UI present would keep its last value across the driver's three
               early returns -- leaving the engine's cursor suppressed while
               ours was not drawn, which is no cursor at all. */
            tagpu_cursown_publish(tagpu_gui_cursor_drew_take());
            tagpu_reclaim_pass_end(f.frame_counter);
            tagpu_packet_frame_end(f.frame_counter);
            /* The render-options screen's deferred cfg write, off the game
               thread because TA is lockstep. Nothing happens on a frame with
               no click. */
            tagpu_menu_present();
        }

        /* THE FRAME-TIME LEVER IS POLLED HERE OR NOWHERE. [FOUND BY THE 4d-1
           LANDING REVIEW.] `tagpu_ftime_poll` is what sets the module's `s_on`,
           and until this line it was called from exactly one place --
           `render_ogl.c` -- which after 4d-1 never drives this lane. So under
           `renderer=vulkan` the whole instrument was inert for the session: no
           Vulkan timestamps written (`tagpu_vk.c` gates them on
           `tagpu_ftime_armed()`), every sample discarded, and not one line in
           the log. The gate clause it exists for, "frame time no worse than
           GL", is still open, so the landing that deleted the other lane must
           not also delete the only way to measure this one.

           WHAT IT CAN AND CANNOT ANSWER NOW is in tagpu_ftime.h: the two-lane
           ratio is gone with route D, and what is left is this build's GPU time
           against a previous build's. */
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
           [FROM THE LANDING REVIEW, 2026-09-17. The first version handed the
           session to GDI on any ST_FAILED, which made one hiccup permanent.]

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

    /* THE PRODUCER'S OWN TEARDOWN CLEARS IT, by construction: the thread that
       is the only writer of the cursor-ownership flag clears it as it stops
       writing. Leaving it set would keep the engine's cursor blit skipped for a
       session that has no cursor of ours to put in its place -- no pointer at
       all, which is the fail-closed shape this design exists to avoid. */
    tagpu_cursown_publish(0);

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
