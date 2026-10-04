# Borderless shell-to-game handoff — window shrink and presentation restart

**Investigated 2026-10-04; unresolved.** The reported symptom is the desktop appearing after
loading, as though a second fullscreen window opens. Wine reproduction confirms two
discontinuities on the **same** game window: an engine resize escapes the proxy between
DirectDraw objects, and presentation is destroyed and rebuilt. No fix is included.

## Reproduction and controls

The reference setup used an isolated Xvfb display, 1280×1024, without a window manager or
compositor; the normal Vulkan renderer and play defaults, Vsync on, Two Continents, and
1024×768 Screen Size. From `SINGLE → Options → Visuals`, select Fullscreen, return to
`SINGLE → Skirmish`, then Start. The shell and loading screen remain logically 640×480,
presented in the 1280×1024 borderless window. Input goes through `tools/tacli`.

Record the **isolated display**, at 60 fps, rather than one window: a window grab cannot
prove that desktop pixels around a shrinking window become visible. Set its root background
to a known colour and observe root/child Create, Destroy, Map, Unmap and Configure events.
Keep ConfigureNotify's own width/height: querying the window after the event can see the
subsequent full-size rect and conceal the intermediate size.

| Run | Result after loading |
|---|---|
| Unmodified Vulkan, 1024×768, first entry | 34 essentially black capture frames, 0.567 s, before the game |
| Unmodified Vulkan, 1024×768, second entry | 32 essentially black frames, 0.533 s; one frame exposes the known root background over 37.8% of the downsampled capture |
| Instrumented Vulkan, 1024×768 | Full device teardown/rebuild on the same HWND; final capture has 36 essentially black frames, 0.600 s |
| Instrumented GDI, 1024×768 | Desktop exposure in two capture frames and 0.500 s essentially black; no Vulkan surface exists |
| Instrumented Vulkan, 640×480 | No end-of-load DirectDraw recreation; no root-background frame and no long gap. Two short black intervals are 0.033 and 0.067 s |

The capture metric is the fraction of pixels with all channels below 8 after downsampling to
160×128; “essentially black” means over 98%. Times are capture-frame counts, not a guarantee
of physical display latency. The source tested was `5e5fb6a6`. Relevant window and backend
sources are unchanged in local main at `faabb488`.

## The desktop exposure: the resize guard loses its owner

**DISASSEMBLED**, pristine executable, and **MEASURED** with temporary boundary logs:

1. Once loading is complete, `0x49836D`/`0x498380` compare current physical screen size with
   `main+0x37F1B/+0x37F1F`, the desired logical game size. A differing size enters `0x49838C`.
2. The outer `SetWindowPos` at `0x4983D1` requests 1024×768 with flags 4. It is swallowed:
   `fake_SetWindowPos` still sees the game HWND and a nonzero `g_ddraw.ref`.
3. `NewTAScreen 0x4B5940`, called at `0x4983EA`, stores the desired size in
   `TAProgram+0xD4/+0xD8` and calls `DDrawDeviceCreateAndCaps 0x4B5510` at `0x4B5971`.
4. That function first calls `0x4B4FF0` at `0x4B5574`. Its final COM Release call at
   `0x4B5061`, return address `0x4B5064`, releases `TAProgram+0x84`, the DirectDraw object.
   Our `dd_Release` stops and joins the renderer, releases its DC, restores the original
   window procedure, and zeros **all of `g_ddraw`**. The game HWND itself survives.
5. Before creating the replacement DirectDraw object, the engine calls `SetWindowPos` again
   at `0x4B55D7`, return address `0x4B55DD`: same HWND, 1024×768, flags 6
   (`SWP_NOMOVE | SWP_NOZORDER`). `fake_SetWindowPos` sees **ref=0 and hwnd=NULL**, so its
   guard is skipped and the real resize runs.
6. The replacement DirectDraw setup calls `SetDisplayMode` at `0x4B5639`, return address
   `0x4B563C`; `dd_SetDisplayMode` sizes the window back to its 1280×1024 monitor.

The final X trace records the same top-level client `0xA00001` shrinking to 1024×768 and
returning to 1280×1024 **13.37 ms** later. There is no top-level Unmap or Destroy event during
the transition. The Win32 HWND remains `00020058`. The geometric desktop exposure is
independent of Vulkan; the GDI control reproduces it too.

## The delay: a DirectDraw lifetime also ends presentation

`dd_Release` reaching zero stops the render thread. `vk_render_main` exits through
`tagpu_vk_render_stop → vk_down`, which drains the device and destroys the pass objects,
swapchain, device, Win32 surface and Vulkan instance. Wine's surface child is replaced on
the next bring-up; that child is not a second top-level game window.

The final probe's `GetTickCount` timestamps, in milliseconds:

| Boundary | Tick | Offset from final DirectDraw Release |
|---|---:|---:|
| Global DirectDraw ref reaches zero | 597365534 | 0 |
| Vulkan teardown starts | 597365564 | 30 |
| Old Win32 Vulkan surface destroyed | 597365613 | 79 |
| Engine resize escapes the guard | 597365625 | 91 |
| New render thread enters | 597365640 | 106 |
| Render thread's `Sleep(250)` ends | 597365890 | 356 |
| Bring-up worker starts | 597365893 | 359 |
| New Win32 Vulkan surface created | 597365969 | 435 |
| First successful new present | 597366153 | 619 |

The worker logs a 196 ms bring-up; the remainder includes scheduling and work up to the first
present. The earlier Vulkan probe measured 592 ms from teardown entry to first present.
`gdi_render_main` has its own unconditional `Sleep(500)`, explaining the GDI control's half
second gap. These sleeps are existing startup behaviour, not proposed correctness guarantees.

The logged proxy state remains `windowed=1, fullscreen=1`, its borderless configuration;
the exclusive-display branches are not taken. No game-window hide/minimize/destroy call
occurs at the handoff. The first new successful Vulkan present ends the measured long gap;
the evidence does not require a later world/GUI initialization stall to explain it.

## Fix boundary and remaining verification

Window ownership and the presented image need lifetimes that survive an engine DirectDraw
recreation. Keep the resize policy tied to the real game-window lifetime, including the
interval with no DirectDraw object. Preserve a full-size opaque presented frame until a
replacement frame is ready. If presentation is kept alive, its ownership and the boundary
with the reset DirectDraw state need an explicit ordering: merely skipping the join leaves a
thread reading a critical section and state that `dd_Release` destroys. Reducing a sleep does
not repair the window-ownership gap or preserve the last frame.

This investigation does **not** establish the reported several-second duration on a live
composited desktop. The isolated display reproduces the resize, desktop exposure and
0.5–0.6 s presentation interruption; compositor amplification remains unmeasured. If logical
Screen Size equals the borderless monitor size, the intermediate resize asks for the same
dimensions, so desktop exposure by shrinkage is not established for that configuration.
Presentation teardown still follows a shell-to-game logical mode change.

Captures, X events, complete run logs, disassembly and the removed instrumentation patch are
retained under the main checkout's `_local/investigations/fullscreen-handoff-2026-10-04/`.
Temporary instrumentation is removed, the normal DLL rebuilt, and the test instance and
isolated display stopped. The fault remains present; this is a diagnosis, not a fix.
