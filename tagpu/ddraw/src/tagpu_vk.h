#ifndef TAGPU_VK_H
#define TAGPU_VK_H
/* tagpu_vk -- the Vulkan backend. Implementation: tagpu_vk.c.

   THIS IS THE PRESENTATION SEAM, and it is the whole of it (Phase G standing
   constraint 3): surface, swapchain, acquire and present live in tagpu_vk.c and
   nowhere else, and it is the only file in the tree that knows a window exists
   on the Vulkan side. When the renderer eventually moves to a 64-bit process,
   this file is what is replaced; nothing above it changes.

   IT READS NO ENGINE STATE AT ALL (standing constraint 1). Every value it needs
   arrives as an argument from `vk_render_main` (render_vk.c), its only
   caller. It is not on `thread-split.allow`
   and must never need to be.

   NEVER CAST A VULKAN HANDLE TO A POINTER (standing constraint 2), store one in
   a `void*`, or key a container on one. Non-dispatchable handles are `uint64_t`
   at 32-bit and real pointers at 64-bit, so code that is correct at 32-bit is
   correct at 64-bit and the reverse breaks in silence. This header deliberately
   exposes no Vulkan type, so a caller cannot get it wrong.

   THE TWO HALVES.

   `tagpu_vk.off` TURNS THE WHOLE FILE OFF, enumeration included, and is the
   control for any A/B against a DLL built before Phase G. **NEITHER LEVER ARMS
   A LANE.** `tagpu_vk.on` under `renderer=gdi` brings nothing up: what the
   lever does on that path is make `tagpu_vk_armed()` true, which ungreys the
   menu's GPU row (the choice applies to a launch on the Vulkan lane). It does NOT get read for its `color=` there --
   `read_lever` is only reached from `tagpu_vk_frame` -- and it does not latch
   the gather mirrors, which ask `tagpu_vk_owns_present()`. Under
   `renderer=vulkan` the lane runs because the renderer choice says so, and
   `tagpu_vk.off` only leaves the device list unrefreshed for that launch, which
   the log says.

   THE BRING-UP. `tagpu_vk_frame` is called once per iteration from
   `vk_render_main`, which is the only backend that drives it. It returns 1 when
   it presented the frame itself, which is what `vk_render_main` uses to know a
   frame reached the screen.

   THE GPU PICKER. The device list is enumerated once per launch by a
   worker thread and CACHED to `tagpu_vk.gpus`; the menu reads that cache at
   DLL attach, because the row's captions have to be inside the generated
   `.GUI` and the archive is written before the engine globs it. So a machine
   whose GPUs changed shows the new list at the NEXT launch -- the same bargain
   the Monitor row already makes for a hot-plugged monitor, and for the same
   reason. The selection is stored BY NAME (`tagpu_vk.cfg`), not by index, so
   adding or removing a card cannot silently re-point it at another one. */

#include <windows.h>

/* THE TWO BOUNDS THE CALLER NEEDS. A device name is canonicalised and truncated
   on the way in (tagpu_vk.c `vk_canon`), because it comes from the driver and
   ends up inside a generated `.GUI` where `|` and `;` are syntax. The caller
   sizes its caption buffer from these and never from `deviceName`. */
#define TAGPU_VK_NAMELEN 32
/* EIGHT, AND THE BOUND IS OURS RATHER THAN THE ENGINE'S. A stage button CAN
   carry more stages than `commongui.stagebuttnN` has art for [MEASURED
   2026-09-15 from the disassembly]: `0x4A8003` is `cmp al,4; jae` onto `mov
   eax,4` and only THEN `sprintf("stagebuttn%d")`, so the art index is CLAMPED
   and a row with more than four stages draws the four-bar plate and works. The
   shipped `UI scale` row carries six stages, which is the same fact from the
   other end. So eight: every device a player is plausibly choosing between
   stays selectable, and past the fourth the plate's bar count saturates while
   the caption -- the half that says which card -- stays right. (`0x4A803C` is
   worth knowing too: a `stages=1` button is rewritten to 2 and flagged, which
   is why the "(not listed yet)" row is also greyed rather than relying on its
   stage count to keep it inert.) */
#define TAGPU_VK_MAXGPU   8

/* ---- the render thread -------------------------------------------------- */

/* Called from `vk_render_main` (render_vk.c), once per iteration after
   `tagpu_overlay_draw`, on the render thread and on no other. Returns 1 when
   Vulkan presented this frame, and 0 every other time, including while the
   bring-up worker is still running and after a bring-up that failed.

   `w`/`h` are the render target's size; a change tears the swapchain down and
   builds it again. `vsync` picks the present mode: FIFO when set, IMMEDIATE
   when it is clear and the surface offers it.

   `frame_counter` is the fork's own monotonic render-thread frame number --
   the SAME number the gathers stamped their hand-overs with earlier in this
   iteration of render_vk.c's loop. It reaches a pass as TAGPU_VKPASS::frame,
   and a pass uses it to refuse a hand-over published on any other frame; see
   tagpu_vk_pass.h for why that refusal is a safety property and not tidiness. */
int tagpu_vk_frame(HWND hwnd, int w, int h, int vsync, unsigned frame_counter);

/* Called from the render thread as it stops -- a mode change or a shutdown,
   both of which invalidate the game window. Tears everything down. Safe to
   call when nothing is up. */
void tagpu_vk_render_stop(void);

/* THE GAME WINDOW IS BEING DESTROYED, told to the lane by the thread that owns
   windows. Called from the fork's wndproc; it watches `WM_DESTROY` and nothing
   else. That one fact cannot be had on the render thread: `g_ddraw.hwnd` is
   nulled only by the IAT-hooked `DestroyWindow`, and only after the real
   destroy. (`dd_Release`'s `memset` of `g_ddraw` clears it too, but that path
   stops and joins the render thread first, so it cannot beat a real destroy.)
   A destroy by any other route would leave the lane presenting on a dead HWND
   until the driver raised `VK_ERROR_SURFACE_LOST_KHR`. This bounds
   that at one frame.

   AN OBSERVER: it returns nothing and swallows nothing. Unlike
   `tagpu_menu_wndproc` it cannot claim a message, so adding it to the chain
   changes no other message's path. */
void tagpu_vk_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

/* Called once from the render thread's start-up, in `vk_render_main`. Kicks
   the enumeration worker that refreshes `tagpu_vk.gpus` for the next launch's
   menu. Returns immediately. */
void tagpu_vk_enum_start(void);

/* 1 when `tagpu_vk.on` is present: the row's "can this bite?" test. */
int tagpu_vk_armed(void);

/* ---- this backend owns the present --------------------------------------- */

/* Called ONCE from the render thread, before its frame loop, by the backend
   that has no other backend beside it (`renderer=vulkan`, `render_vk.c`).
   Two things change and nothing else does:

   * the surface goes on the window `tagpu_vk_frame` is handed: with one backend
     there is nothing to keep it apart from. Measured as route E of
     the coexistence probe -- a top-level window that never had a GL context or a
     pixel format presents, on wine 9.0 and on Proton 11 (roadmap §G19a).
   * `tagpu_vk.on` STOPS ARMING THE LANE, because the renderer choice already
     did. `tagpu_vk.off` stops disarming it for the same reason: with no other
     backend behind it, a disarmed Vulkan lane is a black window rather than a
     fallback. The ON file is read for its `color=`.

   A ONE-WAY LATCH: which backend the process has is settled at `dd.c`'s
   dispatch and cannot change. There is deliberately no way to clear it. The
   `tagpu_vk_owns_present()` tests across the tree read it, and they are what
   the gather mirrors ask to decide whether a consumer exists at all. */
void tagpu_vk_own_present(void);

/* 1 when the latch above is set. Read by the gathers, to decide whether a
   Vulkan consumer exists and their CPU mirrors are worth keeping. Safe from
   any thread. */
int tagpu_vk_owns_present(void);



/* Unlink `tagpu_<tag>_vk.ppm`, where `tag` is one of "scaffold", "fps", "terr",
   "feat", "fx", "mark", "posedraw". (There is no "gui" row:
   `tagpu_vk_ab_arm("gui")` returns 0, and that lever file is read by nobody.)
   A pass calls this AT THE INSTANT IT LATCHES A CLAIM and
   nowhere else -- the placement is the whole guarantee, and tagpu_vk.c states it
   at length. Safe whether or not the lane is up.

   RETURNS 1 ONLY WHEN THE TARGET IS GONE, and a pass must not claim the Vulkan
   half on anything else: a file that could not be removed -- held open by a
   reader, or read-only -- would otherwise be diffed as this arming's capture,
   which is the exact failure the unlink exists to prevent. */
int tagpu_vk_ab_arm(const char* tag);

/* The bound device's `maxImageDimension2D`, or 0 while no device is up --
   the bound a pass sizing an atlas must fit.
   0 MEANS "NOT YET", NEVER A LIMIT: the lane takes ~200 ms to come up while the
   gathers run from the first frame, so a caller must refuse the frame and ask
   again rather than treat 0 as a bound. */
int tagpu_vk_max_image_dim(void);

/* The bound device's `maxUniformBufferRange`, or 0 while no device is up --
   the most one uniform binding may cover. 0 means "not yet", never a limit. */
int tagpu_vk_max_uniform_range(void);

/* 1 when the lane has given up (ST_FAILED) -- a fact the backend can act on
   rather than a frame count it has to guess.

   NOT the same question as `tagpu_vk_gpu_active() < 0`, which reads -1 for a
   lane that is still starting too.

   AND NOT THE SAME QUESTION AS "THE BRING-UP CANNOT WORK", which is the one a
   fallback wants. ST_FAILED is also published when a swapchain REBUILD is
   refused and when a present goes fatal -- a one-second fence or acquire
   timeout, an allocation refused. The caller must therefore decide whether
   this lane had ever come up, and use `tagpu_vk_retry` for the case where it
   had: handing the session to GDI on any ST_FAILED would turn one hiccup
   after ten minutes of play into software rendering for the rest of the
   process. */
int tagpu_vk_failed(void);

/* Put a lane that failed back to ST_OFF, so the next frame brings it up again;
   1 when it did. The POLICY -- how many times that is worth doing before the
   session gives up on Vulkan -- belongs to the backend, because what to do
   about a dead lane is a property of the renderer and not of the seam. */
int tagpu_vk_retry(void);

/* ---- the menu ----------------------------------------------------------- */

/* Read `tagpu_vk.gpus` into the name table. Plain file I/O and nothing else --
   it is called from `tagpu_menu_init` at DLL attach, under the loader lock,
   where creating a Vulkan instance (and so loading an ICD) is exactly the
   LoadLibrary-from-DllMain that field-notes forbids. */
void tagpu_vk_names_init(void);

/* How many devices the cache lists; 0 when there is no cache yet. */
int tagpu_vk_gpu_count(void);

/* Device `i`'s name, or "" out of range. Valid for the process's life. */
const char* tagpu_vk_gpu_name(int i);

/* The index the row should sit at when the player has never chosen: the first
   DISCRETE_GPU the cache records, else 0. */
int tagpu_vk_gpu_default(void);

/* The index the stored choice names, matched BY NAME against the cache, or
   `tagpu_vk_gpu_default()` when nothing is stored or the stored name is not
   among the devices present any more. */
int tagpu_vk_gpu_stored(void);

/* The index of the device ACTUALLY bound by the live Vulkan lane, or -1 when
   nothing is bound. This is what lets the row be verified rather than trusted:
   the menu plates this when it is >= 0 and the request only when it is not. */
int tagpu_vk_gpu_active(void);

/* The row's click, from the game thread: record the request. Takes effect on
   the render thread's next frame, which rebuilds the device. Persisting it is
   `tagpu_vk_gpu_store`, called from the render thread with the deferred write. */
void tagpu_vk_gpu_select(int i);

/* Write the request to `tagpu_vk.cfg`. RENDER THREAD ONLY -- it is a file
   write, and TA is lockstep, so it rides `tagpu_menu_present` exactly as the
   Classic++ cfg does. */
void tagpu_vk_gpu_store(void);

#endif
