# Capture workflows — surface timing, locked sessions and camera holds

## Current contracts checked against the source

Checked 2026-10-04. These are capture and driving instructions; no renderer or simulation
code changes accompany this documentation pass.

| Path | Contract and source |
|---|---|
| Trigger clock | `before_flip` in `tagpu_gui_hook.c` admits the trigger family only when at least `TRIG_MS = 16` ms have passed. It is evaluated on engine flips, so a stalled engine cannot supply a regular 16 ms cadence. |
| Surface shot | `tagpu_triggers_frame` in `tagpu_overlay.c` services the pending arm before checking `tagpu_shot.trigger`. `ss_shot_service` in `screenshot.c` reads the proxy's primary surface on the next eligible family pass. It does not read the Vulkan frame. |
| Screenshot names | `ss_take_screenshot` uses wall-clock seconds, then collision suffixes `_2` through `_99`. Moving a PNG out allows its name to be reused within the same second. A burst reader must detect output relative to the directory at each request and require a complete, decodable PNG before consuming it. |
| CLI polling | `cmd_shot` in `tools/tacli` waits in 250 ms increments. A direct-trigger loop's throughput is not the CLI verb's throughput. |
| Zoom lever | `tagpu_zoom_read_lever` in `tagpu_zoom.c` rereads `tagpu_zoom.txt` on each active render-frame call. Use an atomic file replacement to avoid partial writes. |
| Camera hold | `tagpu_input_eye_frame` checks hold presence every 15 render frames. Once enabled, it calls `do_eye` every render frame; that reads integer world coordinates and invalidates the hold immediately on a failed open, read or parse. `tagpu_zoom_apply` applies the command on the game thread before drawing. The 15-frame discovery gate is not the active position-update rate. |
| Starting speed | `seed_regstore` in `tools/tacli` sets `gamespeed=10` only when seeding a new store. An existing store is retained. `tacli registry` edits a stopped instance's own store. |

The screenshot arm/service ordering can miss short-lived loading frames. For loading and
present-level faults, capture the actual presented frame. A surface shot can inspect engine
UI while the native world viewport contains a key fill; it cannot verify our terrain, units
or other Vulkan passes.

## Inherited observations and their limits

The unfinished capture notes dated 2026-09-29 report the following on the reference setup.
The notes did not identify their raw clips or timing logs. These observations are preserved
as reports, not rerun measurements or general guarantees:

- At 1024×768 in game, a loop requesting another surface shot as soon as the previous PNG
  appeared reportedly obtained distinct frames every 67 ms, about 15/s, for 75 s. This was
  a custom direct-trigger loop. It does not establish a current maximum rate or a rate for
  `tacli shot`.
- With `LockedHint=yes`, `xwd -id`, `import -window` and ffmpeg `-window_id` reportedly
  returned black content while the game continued and surface shots remained usable. The
  xwd file was 3.1 MB; the 30 s window video reportedly had mean luma 0.0 in every frame.
  Earlier skill text asserted xwd worked while locked. Neither observation supports a
  universal locked-session guarantee: inspect image content, not just file size or session
  metadata.
- A private 3840×2160 Xvfb Classic++ recording reportedly presented at 7.4 fps and advanced
  about 2.9 simulation ticks/s with `gamespeed=1`. The rate is specific to that experiment.
  Sample actual ticks and frames separately before choosing an offline encoding rate.
- The old close-up notes used viewport-centre screen coordinates `(1984,1080)` and a world
  offset `(1856,1048)` at 3840×2160, with `screen_y = z - altitude/2`, and reported a match
  within 2 px. They also described one-grid-pixel laser width and a 33-unit beam without a
  duration override. Those calibrations were not verified in this pass and are not current
  instructions; recalibrate viewport coordinates and inspect beam geometry on the chosen
  renderer, HUD and zoom.

The erroneous eight-frame screenshot gate and the claim that an active eye hold only reads
every 15 frames are replaced by the source contracts above. The useful workflow does not
depend on scripts or paths from another repository.

## Selecting a capture without the human desktop

Use a window-client grab on the instance's own display for ordinary presented-frame work.
Check the actual client dimensions: the logical surface size, a `--window` size and a
borderless monitor size may differ. Validate a representative frame before starting a long
recording, especially when the live session is locked or switched away.

If a live-window grab is black, use `tacli shot` only for the engine surface or UI. For the
Vulkan result, launch an owned instance on an isolated display and validate a window or
whole-display grab there. This avoids recording the human desktop; it does not prove live
compositor behaviour or establish a frame rate.

A desktop-exposure investigation needs the isolated display's root in the recording: a
window-only grab cannot show the desktop uncovered by an intermediate resize. Use a known
root colour, record at 60 fps, and pair the capture with timestamped Create, Destroy, Map,
Unmap and Configure events. Keep ConfigureNotify's own width and height; querying the
window afterwards can already see the next size. An isolated capture does not establish
the live desktop compositor's transition timing.

For a close-up, calibrate the viewport and measure high-zoom tracking steps. An active hold
updates each render frame but uses integer world coordinates, so screen-space motion can
still have visible steps. A pinned camera and an offline crop are alternatives; a crop does
not reconstruct frames that were never captured. Keep the in-game cursor on the sidebar
through tacli input and leave the human's pointer alone.
