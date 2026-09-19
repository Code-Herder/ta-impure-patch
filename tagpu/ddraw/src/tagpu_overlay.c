/* tagpu_overlay.c — per-present entry point of the GPU pass, compiled INTO our
   cnc-ddraw fork (no separate module => no runtime LoadLibrary, which is what
   destabilised TA under wine). Called from render_vk.c before the present
   (render_ogl.c was its other caller until landing 11-2 deleted that lane).
   Runs the file-triggered INPUT service (the other five moved to the game
   thread in landing 10c-1 -- see tagpu_triggers_frame at the end of this file),
   flushes the engine detours, then dispatches the passes (scaffold, native,
   the UI layer, the fps readout). The live roster tacli reads left too, in
   landing 10c-3 -- it is roster_log in tagpu_packet_pub.c.
   tagpu_overlay.off is the kill switch for everything we draw.
   THIS FILE REACHES NO GL ENTRY POINT SINCE 11-5e-1: the glshot capture, the
   `glGetError` probe behind `tagpu_gldbg.on` and the context-change watch are
   all deleted, and it includes no GL header.
   The G1/G2/Phase-A proof markers (corner spinner, mouse dot, per-unit and
   per-piece triangles) were retired 2026-09-02; the log lines they shared stay. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "tagpu_model3do.h"   /* TAGPU_PBMAXPIECE: the piece-count bound */
#include "tagpu_overlay.h"
#include "tagpu_trigger.h"
#include "dd.h"          /* g_ddraw.primary, the fork's own surface        */
#include "screenshot.h"  /* ss_take_screenshot: `tacli shot`, now on the flip */
#include "tagpu_vk.h"       /* tagpu_vk_owns_present(): whether GL may be drawn */
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

static int   s_state = 0;   /* 0=unloaded 1=ready */

static void olog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* THERE IS NOTHING LEFT TO LOAD, and that is what this function now says. It
   used to fetch `glReadPixels`, `glBlitFramebuffer` and `glReadBuffer` for the
   glshot capture below it; the capture is gone (11-5e-1) and no other line in
   this file reaches a GL entry point. It still exists because `s_state` is the
   module's ready latch and `tagpu_overlay_draw` reads it -- a one-line function
   whose one line is the latch, rather than a latch set inline in the middle of
   the dispatch. */
static void init_overlay(void)
{
    s_state = 1;
    olog("tagpu: overlay ready (built into fork)");
}

/* ---- what the roster log reads, and where it comes from ------------------
   Until the frame packet's landing 3 this file walked the engine's unit array
   on the RENDER thread — three times, for the roster log, a piece-tree probe
   and the opt-in write-back — through the same unsynchronised begin/end pair
   the audit names as its open hazard (cross-thread-engine-reads.md §5 row 2).
   All three are gone:

     the roster log     read the packet's units table, and since landing
                        10c-3 it is not in this file at all -- roster_log in
                        tagpu_packet_pub.c, on the game thread, so that
                        renderer=gdi has a roster;
     probe_unit_model   deleted. It dumped one unit's PrimitiveStructs to the
                        log; `tools/tacob pose-check` and `tagpu_posedump.on`
                        both do that from the game thread, against the engine's
                        own reconstruction, and neither needs this;
     writeback_paint    deleted with tagpu_render3do()'s FBO path. It was the
                        Phase B proof: render a unit's posed 3DO and WRITE the
                        pixels back into the engine's composite plane from the
                        render thread. The native pass has drawn units directly
                        since Phase C, and a render-thread store into engine
                        memory is the one thing the exchange exists to remove —
                        landing 2's claim that none remains was true only
                        because this path was off by default. */

/* THE LIVE-STATE LOG MOVED OUT OF HERE [the vulkan-only plan, landing 10c-3].
   `log_units` emitted the three lines `tacli` greps for -- `units:`, the
   roster dump and `mouse:` -- and it lived here, which render_gdi.c never
   reaches. It is `roster_log` in tagpu_packet_pub.c now, on the game thread,
   fed by the packet that file has just filled and gated in milliseconds
   rather than in render frames. */

void tagpu_overlay_draw(const TAGPU_FRAME* f)
{
    if (!f || f->abi!=TAGPU_ABI) return;
    /* the camera hold (tagpu_eye.txt) — must run even at the menus and
       regardless of the overlay's enable state */
    tagpu_input_eye_frame(f);
    /* THE ON-DEMAND TRIGGERS MOVED OUT OF HERE [landing 10c]. They are called
       from the engine's own flip now (tagpu_gui_hook.c's `before_flip`), on the
       game thread, so that they reach `renderer=gdi` -- which never enters this
       function at all, because `render_gdi.c` makes no `tagpu_` call. See
       `tagpu_triggers_frame` below.

       INPUT WENT WITH THEM IN 10c-2, but only its token half: `tagpu_input_frame`
       drives the game and needs no packet, so it is in the family now. What is
       left above is `tagpu_input_eye_frame`, the half that dereferences
       `f->packet` through `do_eye` and whose answer the render thread reads back
       in `tagpu_zoom_frame_end`. The flip still has no packet to give it, and on
       the gdi lane there is no `tagpu_cmd_post` to carry that answer anywhere. */
    /* THE DISPLAY-MODE / GL-CONTEXT-CHANGE BLOCK IS GONE, AND IT WENT BECAUSE
       NOTHING IN THIS BUILD CAN MAKE A GL CONTEXT CURRENT ON THIS THREAD. It
       polled `wglGetCurrentContext` once a frame and, on seeing the handle
       change, cleared `s_state` and called six modules' `*_glreset` so that no
       pass bound an id belonging to a context the fork had thrown away (a
       stale FBO bind cleared the real backbuffer black -- found during the
       1024x768 resolution test, and the reason the block was written).

       The handle it watched was the fork's own, created by render_ogl.c, which
       landing 11-2 deleted. `wglCreateContext`, `wglMakeCurrent` and
       `SetPixelFormat` appear in no source of this build [masked scan,
       11-5e-1], so `cur` is 0 on every call, `s_ctx` starts 0 and the inner
       `if (s_ctx)` has no first time. Keeping it would be keeping a watchman
       for a door that is not in the building.

       WHAT THIS MAKES CALLERLESS is the point of recording it: the five other
       resets it named -- `tagpu_native_glreset`, `tagpu_scaffold_glreset`,
       `tagpu_r3d_glreset`, `tagpu_gui_glreset`, `tagpu_fps_glreset` -- had this
       as their ONLY call site, and each is now an entry point of the kind
       11-5e exists to find. They are not deleted here: their files carry live
       GL state this landing does not touch. [The vulkan-only plan, 11-5e-1.] */
    /* G4 tracer flush: no-op unless the tracer was armed. Runs independently of the
       overlay's own enable state (must precede the tagpu_overlay.off early-return). */
    tagpu_tracer_flush(f->frame_counter);
    /* G5 suppressor flush: no-op unless suppression was armed. */
    tagpu_suppress_flush(f->frame_counter);
    /* Phase B own-the-draw flush: no-op unless armed. */
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
       (or a GL context change) would leave it bending clicks against the last
       viewport it saw — menu clicks included (tagpu_zoom.h). */
    if (GetFileAttributesA("tagpu_overlay.off")!=INVALID_FILE_ATTRIBUTES)
        { tagpu_zoom_frame_end(); return; }
    if (s_state==0) init_overlay();
    if (s_state!=1) { tagpu_zoom_frame_end(); return; }
    /* tagpu_reclaim: a level teardown is freeing the MODEL TEMPLATES and the
       per-map arrays the fenced passes still index; sit the rest of this frame
       out. The units themselves come from the packet since landing 3, so this
       is no longer what keeps a unit read safe — it is the fence for the
       assets, which the packet does not carry.
       The pass bracket's end stays in the caller (render_ogl.c). */
    if (tagpu_reclaim_teardown_active()) { tagpu_zoom_frame_end(); return; }

    /* EVERYTHING ABOVE THIS POINT IS API-INDEPENDENT AND RUNS ON EVERY BACKEND.
       The four entry points below that DRAW are the whole of landing 4b's work,
       and until each has been taught to publish its hand-over without drawing,
       none of them may be called on a backend with no GL context.

       WHY NOT JUST LET THE GL CALLS BE NO-OPS. Because that is not a pass
       standing down, it is a pass relying on undefined behaviour: any of these
       that reads GL state back -- a shader compile status, an FBO completeness
       check, `glGetIntegerv` -- would branch on whatever the loader returns
       with no context current, and the failure would be a wrong picture rather
       than an error. A pass that is not called publishes nothing, so its Vulkan
       twin stands down and SAYS SO, which is a refusal that names itself.

       THE GATE HAS FINISHED MOVING INWARD. It was one test here, taken away
       one pass per commit as each learned to gather without drawing:
       `tagpu_scaffold_frame`, `tagpu_fps_present` and `tagpu_native_frame` in
       4b-1 and 4b-2, and `tagpu_gui_present` in 4b-3. All four are called
       unconditionally below and each gates its own GL objects, uploads, draws
       and state restore, so there is no variable here any more -- the reason
       the paragraphs above are kept is that they are the argument for why the
       gate is a gate at each of those sites rather than an absent context
       quietly doing nothing.
       [The vulkan-only plan, landings 4b-1, 4b-2 and 4b-3.] */

    /* the palette the screen is shown with, once for every pass that resolves
       an 8-bit index this frame -- the world's and the UI layer's alike
       (tagpu_pal.h). The engine's half comes from this frame's packet; a flag
       otherwise: the first reader below does the work. */
    tagpu_pal_frame(f->packet);
    /* TA'S OWN FRAME, ONCE, BEFORE ANY PASS GATHERS. It is the bottom layer of
       the composite -- everything of ours is drawn over it and what we do not
       draw is what the player still sees -- and taking it here means every
       consumer in this frame gets the same bytes and the same palette. Two
       passes reading the engine's surface at two instants is how the lanes end
       up compositing different moments of one frame. Cheap and silent when
       there is no 8-bit primary. [The vulkan-only plan, landing 4c-1.] */
    tagpu_surf_take(f);

    /* THE VIEW FOR THIS FRAME, once, before any pass reads the eye: the zoom
       level (the levers, the wheel's ease), the cursor anchor's step against
       this frame's packet, and the predicted eye every pass — the scaffold
       and the native pass alike — draws from (tagpu_zoom.h). Called here and
       nowhere else, so no two passes can draw one frame from two eyes. */
    tagpu_zoom_read_lever(f->packet);

    /* G12a: scene-depth scaffold debug overlay (tagpu_scaffold.on). Own GL
       state block; leaves program/VAO at 0.
       CALLED ON BOTH LANES: its gather is the pass and is API-independent, and
       it gates its own upload and draw on `tagpu_vk_owns_present` -- so under
       `renderer=vulkan` this builds the scaffold, publishes it and lets the
       Vulkan twin draw it. */
    tagpu_scaffold_frame(f);

    /* G17c: the cursor's ONE decision for this frame, before the world pass
       reads it (tagpu_gui.h). Both the composite below and the UI layer after
       it erase the engine's cursor from the same rect, and they can only agree
       if the state is read once — which since landing 4c means latching THIS
       FRAME'S PACKET here, for the whole of the GL UI's render half. */
    tagpu_gui_cursor_frame(f->packet);

    /* G12b: native unit pass (tagpu_native.on) — needs this frame's scaffold.
       CALLED ON BOTH LANES since 4b-2: its arm poll, view, fog and palette
       copies, four world gathers and vertex emission are the pass, and it gates
       its own GL. On the vulkan-only lane it stops after the gathers and calls
       the renders that have been taught to hand over without drawing — the GL
       world composite has no counterpart there until 4c. */
    tagpu_native_frame(f);

    /* Phase E: the UI layer — the presented surface's twin, drawn over the
       world's composite (UI above the world; the engine's own pixels stay
       the fallback beneath). Runs in the shell too: the native pass returns
       early there, this does not. */
    tagpu_gui_present(f);

    /* The frame-rate readout, ABOVE the UI layer: it is a diagnostic drawn over
       the finished frame and must not be hidden by the side panel or a dialog.
       Off unless `tagpu_fps.on` is there, which the render-options screen's FPS
       row writes -- see tagpu_fps.c for why this is not cnc-ddraw's own OSD.
       CALLED ON BOTH LANES, like the scaffold: the averaging window, the font
       latch and the quads are the pass, and it gates its own draw. */
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
   is a handful of GetFileAttributes calls per five flips — the same cost they
   had on the render thread, moved. */
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
       the reason this whole family moved: a lane that can be OBSERVED but not
       CLICKED is still not a drivable one.

       THE POSITION IN THIS LIST DOES NOT MATTER, and an earlier draft of this
       comment claimed it did [landing review]. Every token leaves by
       PostMessageA or SendInput and nothing is dispatched before `before_flip`
       returns, so nothing this call emits can be observed by the other five in
       the same tick: first and last are indistinguishable. Written down so the
       next reader does not preserve an ordering constraint that does not
       exist. */
    tagpu_input_frame(f);
    /* THE ENGINE-SURFACE SCREENSHOT (tagpu_shot.trigger) -- `tacli shot`.
       It lived in render_ogl.c's present loop, so it answered on the GL lane
       and NOWHERE else: gdi reaches no tagpu_ call at all and render_vk.c
       polls no trigger file, which is why the verb was measured failing on
       both (the vulkan-only plan, landing 10c-3's review). It is not a GL
       capture -- `ss_take_screenshot(g_ddraw.primary)` reads the fork's own
       DirectDraw primary -- so nothing tied it to that lane but where it was
       written.

       THE THREAD IS ALREADY PROVEN FOR IT. keyboard.c:96 and :102 call the
       same function from the game thread on the PrintScreen path, which is
       this thread: `before_flip` runs on the game thread inside the engine's
       flip. So this is the context ss_take_screenshot already has a caller
       in, not a new one it has to be made safe for. */
    /* SERVICE BEFORE ARM, AND THAT ORDER IS THE WHOLE MECHANISM. This runs at
       the ENTRY of the engine's flip, so the primary here holds the frame the
       PREVIOUS flip presented -- which is why capturing in the same pass that
       saw the trigger returned frame N-1 (landing 11-2's review, MEDIUM-1).
       Servicing first and arming second puts at least one flip between the
       two, so the picture is a frame the engine presented AFTER the trigger was seen. The
       gap is one pass of this family, NOT one flip: the 16 ms gate above means
       P and P+1 can be many flips apart.

       The host is this function and not `dds_Unlock` (tried, and reverted by
       the same review's round 2): that branch is gated on `g_ddraw.render.run`,
       which the WINDOW thread clears on deactivate, minimise, a fullscreen
       toggle and a mode change, and it is reached only when the flip takes its
       DirectDraw arm at 0x4C6475 -- the GDI BitBlt arm at 0x4C63C0 never enters
       it. An arm could therefore wait for an arbitrary later frame, or for
       none. Here there is no such gate: this family runs on every flip that
       passes its 16 ms window, whichever arm the flip takes, and it is above
       `before_flip`'s `s_opsLive` return so a bare `tacli launch` has it too.
       That is arm-independence of the FAMILY, not of the frame -- only the
       DirectDraw arm writes our primary -- but a build taking the other arm
       would show this fork no frame anywhere, so it is not this verb's gap. */
    ss_shot_service(g_ddraw.primary);
    if (GetFileAttributesA("tagpu_shot.trigger") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("tagpu_shot.trigger");
        ss_shot_arm();
    }
}
