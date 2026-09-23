/* tagpu_overlay.c — per-present entry point of the GPU pass, compiled INTO our
   cnc-ddraw fork (no separate module => no runtime LoadLibrary, which is what
   destabilised TA under wine). Called from render_vk.c before the present.
   Runs the camera hold, flushes the engine detours, then dispatches the passes
   (scaffold, native, the UI layer, the fps readout). The file-triggered
   services run on the game thread -- see tagpu_triggers_frame at the end of
   this file -- and the live roster tacli reads is roster_log in
   tagpu_packet_pub.c.
   tagpu_overlay.off is the kill switch for everything we draw. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "tagpu_overlay.h"
#include "tagpu_trigger.h"
#include "dd.h"          /* g_ddraw.primary, the fork's own surface        */
#include "screenshot.h"  /* ss_take_screenshot: `tacli shot`, now on the flip */
#include "tagpu_tracer.h"
#include "tagpu_suppress.h"
#include "tagpu_owndraw.h"
#include "tagpu_fxown.h"
#include "tagpu_featown.h"
#include "tagpu_terrown.h"
#include "tagpu_gui.h"
#include "tagpu_markown.h"
#include "tagpu_scaffold.h"
#include "tagpu_input.h"
#include "tagpu_native.h"
#include "tagpu_peek.h"
#include "tagpu_ui.h"
#include "tagpu_cat.h"
#include "tagpu_scenario.h"
#include "tagpu_weapons.h"
#include "tagpu_zoom.h"
#include "tagpu_reclaim.h"
#include "tagpu_pal.h"
#include "tagpu_surf.h"
#include "tagpu_fps.h"

static int   s_said;        /* the one-shot below has logged */

static void olog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* THERE IS NOTHING TO BRING UP, SO THIS IS A ONE-SHOT LOG AND NOT A
   READINESS LATCH: `s_said` says whether the line has been logged, and there
   is no gate. */
static void say_ready(void)
{
    s_said = 1;
    olog("tagpu: overlay ready (built into fork)");
}

/* NOTHING HERE WALKS THE ENGINE'S UNIT ARRAY, and nothing here stores into
   engine memory: a render-thread store is the one thing the frame packet
   exchange exists to remove (cross-thread-engine-reads.md). The three lines
   `tacli` greps for -- `units:`, the roster dump and `mouse:` -- are
   `roster_log` in tagpu_packet_pub.c, on the game thread, which render_gdi.c
   reaches and this file does not; it is fed by the packet that file has just
   filled and gated in milliseconds rather than in render frames. */

void tagpu_overlay_draw(const TAGPU_FRAME* f)
{
    if (!f || f->abi!=TAGPU_ABI) return;
    /* the camera hold (tagpu_eye.txt) — must run even at the menus and
       regardless of the overlay's enable state */
    tagpu_input_eye_frame(f);
    /* THE ON-DEMAND TRIGGERS ARE NOT HERE. They are called from the engine's
       own flip (tagpu_gui_hook.c's `before_flip`), on the game thread, so that
       they reach `renderer=gdi` -- which never enters this function at all,
       because `render_gdi.c` makes no `tagpu_` call. See
       `tagpu_triggers_frame` below.

       INPUT IS SPLIT. Its token half, `tagpu_input_frame`, drives the game and
       needs no packet, so it is in that family. What is left above is
       `tagpu_input_eye_frame`, the half that dereferences `f->packet` through
       `do_eye` and whose answer the render thread reads back in
       `tagpu_zoom_frame_end`. The flip has no packet to give it, and on the gdi
       lane there is no `tagpu_cmd_post` to carry that answer anywhere. */
    /* tracer flush: no-op unless the tracer was armed. Runs independently of the
       overlay's own enable state (must precede the tagpu_overlay.off early-return). */
    tagpu_tracer_flush(f->frame_counter);
    /* suppressor flush: no-op unless suppression was armed. */
    tagpu_suppress_flush(f->frame_counter);
    /* own-the-draw flush: no-op unless armed. */
    tagpu_owndraw_flush(f->frame_counter);
    /* effects own-the-draw flush: no-op unless armed at launch. */
    tagpu_fxown_flush(f->frame_counter);
    /* the build ghost's standing request for the packet's builds table: decays
       here, not in its setter, so a render thread that stops polling stops
       charging the publisher for a walk nothing will read. */
    tagpu_native_flush_want(f->frame_counter);
    tagpu_featown_flush(f->frame_counter);
    tagpu_terrown_flush(f->frame_counter);
    tagpu_markown_flush(f->frame_counter);
    tagpu_gui_flush(f->frame_counter);
    /* On EVERY path out of here, including these two: a frame that drew nothing
       zoomed must take the input transform back to 1:1, or `tagpu_overlay.off`
       would leave it bending clicks against the last viewport it saw — menu
       clicks included (tagpu_zoom.h). */
    if (GetFileAttributesA("tagpu_overlay.off")!=INVALID_FILE_ATTRIBUTES)
        { tagpu_zoom_frame_end(); return; }
    if (!s_said) say_ready();
    /* tagpu_reclaim: a level teardown is freeing the MODEL TEMPLATES and the
       per-map arrays the fenced passes still index; sit the rest of this frame
       out. The units themselves come from the packet, so this is not what
       keeps a unit read safe — it is the fence for the assets, which the
       packet does not carry. The pass bracket's end is the caller's, in
       render_vk.c. */
    if (tagpu_reclaim_teardown_active()) { tagpu_zoom_frame_end(); return; }

    /* THE FOUR PASSES BELOW DRAW NOTHING THEMSELVES. `tagpu_scaffold_frame`,
       `tagpu_native_frame`, `tagpu_gui_present` and `tagpu_fps_present` are
       called unconditionally; each gathers its frame and fills a hand-over,
       and its Vulkan pass records the draw from it after this function
       returns. A pass that is not called publishes nothing, so its Vulkan pass
       stands down and SAYS SO, which is a refusal that names itself. */

    /* the palette the screen is shown with, once for every pass that resolves
       an 8-bit index this frame -- the world's and the UI layer's alike
       (tagpu_pal.h). The engine's half comes from this frame's packet; a flag
       otherwise: the first reader below does the work. */
    tagpu_pal_frame(f->packet);
    /* TA'S OWN FRAME -- TAKEN ON THE GAME THREAD, TAKEN HERE. It is THE
       REFERENCE and nothing else: the picture the 1997 software rasteriser
       produced, kept so it can be compared against what we draw. Nothing
       composites it and no lever brings that back.

       THE COPY IS NOT MADE HERE, AND THAT IS THE POINT.
       This is the render thread; the bytes are written by the game thread, in
       TA's rasteriser and in the flip's row loop, under no lock that covers
       them. A copy taken from here could land mid-frame and publish a TORN
       picture as the golden source with nothing marking it. The copy runs
       where the writer runs
       and where the frame is finished: `tagpu_packet_pub.c`'s `after_draw`
       observer on `DrawGameScreen 0x468CF0`, past the flip at `0x46A3DB`, in
       the same call that publishes the packet our passes draw from -- so the
       reference and the state we render come from the same engine call.
       WHICH IS NOT THE SAME AS RECEIVING THEM TOGETHER.
       The two travel on separate gates -- the packet's freshness test and this
       module's `req`/`ack` -- and this thread takes the packet at the top of
       the frame and syncs the snapshot here, after, so a frame can perfectly
       well draw packet D-1 against reference D. `TAGPU_SURFFRAME.stamp` is the
       in-play draw the snapshot was taken at, and it is how a comparison
       CHECKS the pairing; nothing gates on it.

       WHAT IS LEFT HERE IS THE CONSUMER'S HALF, and it runs exactly once
       a frame: take whatever the game thread has answered, record this frame's
       letterboxed viewport (ours, not the engine's), and ask for the next. Two
       atomic loads and a store; `tagpu_surf.h` carries the ownership rule that
       makes the two-buffer hand-over safe by construction rather than by
       timing. `tagpu_vk_surf_prepare` puts the bytes on the device;
       `tagpu_vk_surf_engine_view` is how a pass asks for them. */
    tagpu_surf_sync(f);

    /* THE VIEW FOR THIS FRAME, once, before any pass reads the eye: the zoom
       level (the levers, the wheel's ease), the cursor anchor's step against
       this frame's packet, and the predicted eye every pass — the scaffold
       and the native pass alike — draws from (tagpu_zoom.h). Called here and
       nowhere else, so no two passes can draw one frame from two eyes. */
    tagpu_zoom_read_lever(f->packet);

    /* scene-depth scaffold debug overlay (tagpu_scaffold.on). Its gather is
       the pass: this builds the scaffold and publishes it, and
       tagpu_vk_scaffold.c draws it. */
    tagpu_scaffold_frame(f);

    /* native unit pass (tagpu_native.on) — needs this frame's scaffold.
       Its arm poll, view, fog and palette copies, four world gathers and
       vertex emission are the pass; it stops after the gathers and hands
       each of them over for the Vulkan world passes to draw. */
    tagpu_native_frame(f);

    /* the cursor's ONE decision for this frame, before the world pass
       reads it (tagpu_gui.h). The state is read once, latching THIS FRAME'S
       PACKET for the whole of the UI's render half, so every reader of it in
       this frame agrees. */
    tagpu_gui_cursor_frame(f->packet);

    /* THE UI LAYER, AND IT IS THE DRAIN RATHER THAN A DRAW. This applies
       the op stream the game thread published -- resolving each sprite to its
       atlas rect, stamping each string from the glyph atlas, keeping the twin
       bookkeeping level -- and fills `TAGPU_GUIHAND` for `tagpu_vk_gui.c` to
       replay on the device. There is no `gl*` call in this path; the drawing
       is the Vulkan pass's.

       THERE IS NO COMPOSITE. The engine's own frame is not a layer of this
       draw: `LAY_FS` does not sample it, the sampler is not declared, and the
       golden source is captured for comparison and drawn nowhere. What is
       drawn is ours -- the twins we replay and the device-resolution sharp
       layer above them.

       Runs in the shell too: the native pass returns early there, this does
       not. */
    tagpu_gui_present(f);

    /* The frame-rate readout, ABOVE the UI layer: it is a diagnostic drawn over
       the finished frame and must not be hidden by the side panel or a dialog.
       Off unless the render-options screen's FPS row (the settings store) or
       the `tagpu_fps.on` lever turns it on -- see tagpu_fps.c for why this is
       not cnc-ddraw's own OSD. Like the scaffold, it builds and does not draw:
       the averaging window, the font latch and the quads are the pass, and
       tagpu_vk_fps.c draws them. */
    tagpu_fps_present(f);

    /* If the native pass did not publish a view this frame, nothing zoomed was
       drawn, so the input path goes back to 1:1 (tagpu_zoom.h). Every early
       return above does the same. */
    tagpu_zoom_frame_end();
}

/* THE TRIGGER FAMILY'S ONE ENTRY POINT — contract in tagpu_trigger.h.
   Called from the game thread, once per engine flip, on every renderer.
   Each of these throttles itself (most on `frame_counter % 5`) and does
   nothing at all until its `.trigger` file appears, so the steady-state cost
   is a handful of GetFileAttributes calls per five flips. */
void tagpu_triggers_frame(const TAGPU_FRAME* f)
{
    if (!f || f->abi != TAGPU_ABI) return;
    /* on-demand memory reads (tagpu_peek.trigger) — no-op unless triggered, and
       must run at the menus too: switch effects land before the first game */
    tagpu_peek_frame(f->frame_counter);
    /* on-demand weapon-slot dump (tagpu_weapons.trigger) — the A/B oracle for
       the extra-weapons module; read-only, runs armed or not. */
    tagpu_weapons_frame(f->frame_counter);
    /* on-demand GUI snapshot (tagpu_ui.trigger) — the read half of `tacli ui`.
       The menus are exactly where it earns its keep, so like peek it must run
       before any game exists. */
    tagpu_ui_frame(f);
    /* on-demand unit/feature catalogues (tagpu_units.trigger,
       tagpu_features.trigger) — validation layer 2 for `tacli scenario`. */
    tagpu_cat_frame(f);
    /* on-demand situation applier (tagpu_scenario.trigger) and the engine
       switches (tagpu_switches.trigger). Detection and reporting live here; the
       creation pass runs from that module's own Game_MainLoopTick detour, never
       mid-render. Switches must reach the menus too, like peek. */
    tagpu_scenario_frame(f);
    /* in-process input injection (tagpu_keys.txt) — the token half only, and
       the reason this family runs on the flip: a lane that can be OBSERVED
       but not CLICKED is not a drivable one.

       THE POSITION IN THIS LIST DOES NOT MATTER. Every token leaves by
       PostMessageA or SendInput and nothing is dispatched before `before_flip`
       returns, so nothing this call emits can be observed by the other five in
       the same tick: first and last are indistinguishable. Written down so the
       next reader does not preserve an ordering constraint that does not
       exist. */
    tagpu_input_frame(f);
    /* THE ENGINE-SURFACE SCREENSHOT (tagpu_shot.trigger) -- `tacli shot`.
       It is here because the flip is what every renderer reaches: gdi
       reaches no tagpu_ call at all and render_vk.c polls no trigger file.
       `ss_take_screenshot(g_ddraw.primary)` reads the fork's own DirectDraw
       primary, not what the Vulkan backend presented.

       THE THREAD IS ALREADY PROVEN FOR IT. keyboard.c:96 and :102 call the
       same function from the game thread on the PrintScreen path, which is
       this thread: `before_flip` runs on the game thread inside the engine's
       flip. So this is the context ss_take_screenshot already has a caller
       in, not a new one it has to be made safe for. */
    /* SERVICE BEFORE ARM, AND THAT ORDER IS THE WHOLE MECHANISM. This runs at
       the ENTRY of the engine's flip, so the primary here holds the frame the
       PREVIOUS flip presented -- so capturing in the same pass that saw the
       trigger would return frame N-1.
       Servicing first and arming second puts at least one flip between the
       two, so the picture is a frame the engine presented AFTER the trigger was seen. The
       gap is one pass of this family, NOT one flip: the 16 ms gate above means
       P and P+1 can be many flips apart.

       The host is this function and not `dds_Unlock`: that branch is gated on
       `g_ddraw.render.run`, which the WINDOW thread clears on deactivate,
       minimise, a fullscreen toggle and a mode change, and it is reached only
       when the flip takes its DirectDraw arm at 0x4C6475 -- the GDI BitBlt arm
       at 0x4C63C0 never enters it. An arm could therefore wait for an arbitrary
       later frame, or for none. Here there is no such gate: this family runs on
       every flip that passes its 16 ms window, whichever arm the flip takes,
       and it is above `before_flip`'s `s_opsLive` return so a bare `tacli
       launch` has it too. That is arm-independence of the FAMILY, not of the
       frame -- only the DirectDraw arm writes our primary -- but a build taking
       the other arm would show this fork no frame anywhere, so it is not this
       verb's gap. */
    ss_shot_service(g_ddraw.primary);
    if (GetFileAttributesA("tagpu_shot.trigger") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("tagpu_shot.trigger");
        ss_shot_arm();
    }
}
