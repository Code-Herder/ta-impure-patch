#ifndef TAGPU_VK_H
#define TAGPU_VK_H
/* tagpu_vk -- the Vulkan backend (Phase G / G19). Implementation: tagpu_vk.c.

   THIS IS THE PRESENTATION SEAM, and it is the whole of it (Phase G standing
   constraint 3): surface, swapchain, acquire and present live in tagpu_vk.c and
   nowhere else, and it is the only file in the tree that knows a window exists
   on the Vulkan side. When the renderer eventually moves to a 64-bit process,
   this file is what is replaced; nothing above it changes.

   IT READS NO ENGINE STATE AT ALL (standing constraint 1). Every value it needs
   arrives as an argument from `ogl_render`. It is not on `thread-split.allow`
   and must never need to be.

   NEVER CAST A VULKAN HANDLE TO A POINTER (standing constraint 2), store one in
   a `void*`, or key a container on one. Non-dispatchable handles are `uint64_t`
   at 32-bit and real pointers at 64-bit, so code that is correct at 32-bit is
   correct at 64-bit and the reverse breaks in silence. This header deliberately
   exposes no Vulkan type, so a caller cannot get it wrong.

   THE TWO HALVES.

   `tagpu_vk.off` TURNS THE WHOLE FILE OFF, enumeration included, and is the
   control for any A/B against a DLL built before Phase G: the lane is gated on
   `tagpu_vk.on`, but the GPU enumeration is NOT (it outlives the lane), so
   without this there would be no way to ask for the old behaviour exactly.

   G19a -- the bring-up. `tagpu_vk_frame` is called from the render thread
   immediately before `SwapBuffers`. It returns 1 when it presented the frame
   itself, and the caller then does NOT swap: the two backends must not both
   present to one window in one frame. Armed by `tagpu_vk.on` beside the exe; it
   is NOT on the play-defaults table, because GL stays the default through
   Phase G. With the lever absent the first statement returns 0 and the GL path
   is byte-identical to the build without this file.

   G19b -- the GPU picker. The device list is enumerated once per launch by a
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
/* EIGHT, AND THE BOUND IS OURS RATHER THAN THE ENGINE'S. This said four, on
   the theory that a stage button cannot carry more stages than
   `commongui.stagebuttnN` has art for. That is not what the engine does
   [MEASURED 2026-09-15 from the disassembly, after a review challenged it]:
   `0x4A8003` is `cmp al,4; jae` onto `mov eax,4` and only THEN
   `sprintf("stagebuttn%d")`, so the art index is CLAMPED and a row with more
   than four stages draws the four-bar plate and works. The shipped `UI scale`
   row has carried six stages all along, which is the same fact from the other
   end. So eight: every device a player is plausibly choosing between stays
   selectable, and past the fourth the plate's bar count saturates while the
   caption -- the half that says which card -- stays right.
   (`0x4A803C` is worth knowing too: a `stages=1` button is rewritten to 2 and
   flagged, which is why the "(not listed yet)" row is also greyed rather than
   relying on its stage count to keep it inert.) */
#define TAGPU_VK_MAXGPU   8

/* ---- G19a: the render thread ------------------------------------------- */

/* Called from `ogl_render`, immediately before the GL swap, on the render
   thread and on no other. Returns 1 when Vulkan presented this frame -- the
   caller must then skip `SwapBuffers` -- and 0 every other time, including
   every frame while the lever is off, while the bring-up worker is still
   running, and after a bring-up that failed.

   `w`/`h` are the render target's size; a change tears the swapchain down and
   builds it again. `vsync` picks the present mode, matching what the GL lane
   does with `wglSwapIntervalEXT`.

   `frame_counter` is the fork's own monotonic render-thread frame number --
   the SAME number the GL lane stamped its hand-overs with earlier in this
   iteration of render_ogl.c's loop. It reaches a pass as TAGPU_VKPASS::frame,
   and a pass uses it to refuse a hand-over published on any other frame; see
   tagpu_vk_pass.h for why that refusal is a safety property and not tidiness.
   [ADDED BY THE G19e RE-REVIEW, 2026-09-15.] */
int tagpu_vk_frame(HWND hwnd, int w, int h, int vsync, unsigned frame_counter);

/* Called from the render thread as it stops -- a mode change or a shutdown,
   both of which invalidate the game window. Tears everything down. Safe to
   call when nothing is up. */
void tagpu_vk_render_stop(void);

/* ROUTE D's window, on the thread that owns windows. `tagpu_vk_frame` posts
   WM_TAGPU_VK to the GAME window and this observer, called from the fork's
   wndproc, creates, moves and destroys the Vulkan window there -- because a
   window whose messages nobody pumps deadlocks anything that sends it one. It
   also follows the owner's WM_WINDOWPOSCHANGED, which is how the Vulkan window
   stays over the client area without the render thread polling geometry.

   AN OBSERVER: it returns nothing and swallows nothing. Unlike
   `tagpu_menu_wndproc` it cannot claim a message, so adding it to the chain
   changes no other message's path. */
#define WM_TAGPU_VK (WM_APP + 144)     /* wParam = create | destroy */
void tagpu_vk_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

/* Called once from the render thread's start-up. Kicks the enumeration worker
   that refreshes `tagpu_vk.gpus` for the next launch's menu, whether or not the
   Vulkan lane is armed -- the GPU row is the player-facing half of Phase G and
   must work under the GL lane too. Returns immediately. */
void tagpu_vk_enum_start(void);

/* 1 when `tagpu_vk.on` is present: the row's "can this bite?" test. */
int tagpu_vk_armed(void);

/* ---- landing 4: this backend owns the present ---------------------------- */

/* Called ONCE from the render thread, before its frame loop, by the backend
   that has no other backend beside it (`renderer=vulkan`, `render_vk.c`).
   Two things change and nothing else does:

   * the surface goes on the window `tagpu_vk_frame` is handed, and ROUTE D'S
     WINDOW IS NEVER CREATED. Every line of that machinery exists because "two
     backends must not both present to one window in one frame"; with one
     backend there is nothing to separate. Measured as route E in
     `tools/vkcoexist.c` -- a top-level window that never had a GL context or a
     pixel format presents, on wine 9.0 and on Proton 11 (roadmap §G19a).
   * `tagpu_vk.on` STOPS ARMING THE LANE, because the renderer choice already
     did. `tagpu_vk.off` stops disarming it for the same reason: with no GL
     lane behind it, a disarmed Vulkan lane is a black window rather than a
     fallback. The ON file is still read for its `color=`.

   A ONE-WAY LATCH: which backend the process has is settled at `dd.c`'s
   dispatch and cannot change, and a flag that could go back would allow a
   surface on the game window and a route D window at once. There is
   deliberately no way to clear it. */
void tagpu_vk_own_present(void);

/* 1 when the bring-up has given up (ST_FAILED) -- a fact the backend can act
   on rather than a frame count it has to guess. The owning backend hands the
   session to `gdi_render_main` on this, which route F measured as still
   reaching the screen after a surface has been attempted on the window.

   NOT the same question as `tagpu_vk_gpu_active() < 0`, which reads -1 for a
   lane that is still starting too. */
int tagpu_vk_failed(void);

/* ---- G19b: the menu ------------------------------------------------------ */

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
