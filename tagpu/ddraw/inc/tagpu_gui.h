#ifndef TAGPU_GUI_H
#define TAGPU_GUI_H
/* tagpu_gui — the GL UI renderer, Phase E (research/notes/gui-renderer.md).

   THE MODULE, NOT A SURFACE. The engine's UI — the side panel, the top and
   bottom bars, the minimap, chat and dialogs in game, every screen of the
   shell — is mirrored into GL twins of the engine's own surfaces: observer
   detours on the pixel-writing leaves record what was drawn, the engine keeps
   drawing its own 8bpp surface (which stays the oracle and the fallback), and
   the render thread replays the ops into retained twins that are drawn over
   the frame after the world's composite.

   Family: tagpu_gui_hook.c (the observers, the census, the publisher),
   tagpu_gui_surf.c (the twins, the UI atlas, the replay, the layer draw),
   tagpu_gui_snap.c (the gadget-tree snapshot behind `tacli ui`, was
   tagpu_ui.c, contract inc/tagpu_ui.h). One trigger, gamedir/tagpu_gui.on;
   the detours install at DllMain when it exists then; the DRAW follows the
   file live (polled twice a second): delete it and the frame is today's,
   recreate it and every twin re-seeds from the engine's surfaces at the next
   flip. Off also stops publishing, so an idle module is a few ifs.

   Tokens in tagpu_gui.on: `census` (the G15a diff; costs a 1024x768 compare
   per 5 ms), `strict` (the fallback off: a UI pixel the engine drew that we
   have not is painted magenta, the cursor's rect exempt — the harness's mode),
   `norestore` (G15e: the layer without Classic++ art — the UI-only A/B),
   `sharptest` (G17a: 13.2's sharp layer filled with a known pattern, so an
   empty layer is still testable — the harness's mode too), `log`, `pgm`,
   `trace`, `key=N` (census diagnostics, tagpu_gui_hook.c).

   THREADS. The observers and the publisher run on the game thread inside the
   engine's own calls; the twins, the atlas and the draw run on the render
   thread inside the present. They meet only in the queue (tagpu_gui_int.h).

   Every observer calls the original, so the engine's behaviour is
   byte-identical with the module armed. G15b draws the INDEX twin: Classic,
   1:1, palette-resolved at present; the colour twin is G15e. */
#include <windows.h>
#include "tagpu.h"

void tagpu_gui_init(void);                          /* DllMain                */
/* render thread, per present, GL current: poll the trigger, drain the queue
   into the twins, then draw the presented surface's twin over the frame
   (after the world's composite, so UI is above the world) */
void tagpu_gui_present(const TAGPU_FRAME* f);
void tagpu_gui_flush(unsigned int frame_counter);   /* render thread: the heartbeat line */
void tagpu_gui_glreset(void);                       /* the GL context changed */
int  tagpu_gui_installed(void);
int  tagpu_gui_drawing(void);                       /* the trigger says draw  */
unsigned tagpu_gui_flips(void);                     /* the publisher's flip count, game thread */

/* THE CURSOR, G17c (gui-renderer.md 13.5). Erasing the engine's own cursor
   takes two modules, so the decision is taken once and read by both:
   tagpu_overlay.c calls tagpu_gui_cursor_frame FIRST, then the world pass,
   then tagpu_gui_present. Over the panel this module's twin covers the
   engine's cursor once the layer stops discarding its rect; over the world it
   does not, because the composite drops our fragment wherever the engine's
   surface is not the terrain key and a cursor pixel is not the key — so
   tagpu_native.c asks tagpu_gui_cursor_own for the rect and treats it as key.
   A version that only drew ours would ship two cursors over the world.
   tagpu_gui_cursor_own returns 1 when ours is being drawn this frame and
   fills `r` with the engine's rect in GAME pixels (x, y, w, h).

   THE TWO HALVES SHARE THE RECT, NOT THE DECISION [landing review,
   2026-09-09]. Ownership here is latched on the atlas alone; the composite
   applies the exemption only inside `uKey >= 0`, i.e. when the terrain is ours
   and the fill has not stalled. Where our own FBO is empty and the key is off,
   the engine's frame shows through with its cursor while the layer still draws
   ours. Not reachable in the shipped path — the terrain is ours whenever the
   composite runs, and over the panel the twin covers the engine's cursor
   either way — and recorded in gui-renderer.md 17 "Not closed here" rather
   than closed by guessing which module should own the question. */
struct TAGPU_PACKET;
/* DID THE SHARP LAYER REALLY DRAW OUR CURSOR IN THE PRESENT JUST ENDED?
   Take-and-clear, called ONCE per frame from the render_ogl.c frame bracket
   and nowhere else; the answer goes straight to tagpu_cursown_publish, which
   is what decides whether the engine's own cursor blit is skipped.

   IT IS TAKE-AND-CLEAR, AND THAT IS THE FAIL-OPEN. The latch is set only at
   the tail of a successful sharp_cursor, and reading it clears it — so a frame
   that never reaches the GL UI's present at all answers 0, and the engine's
   cursor comes back. tagpu_overlay_draw returns early for `tagpu_overlay.off`,
   for a GL init that failed and for a level teardown, and those returns are
   ABOVE tagpu_gui_present: a flag published from inside the present would keep
   its last value across every one of them and suppress the engine's cursor
   while ours was not being drawn — no cursor at all, for as long as the
   condition lasted, with `tagpu_overlay.off` (the A/B lever whose whole job is
   to hand the frame back) the worst case. Publishing from the bracket instead
   means the caller cannot forget a path it does not know about.

   WHY THE ANSWER IS LATCHED RATHER THAN READ LIVE: the engine draws its cursor
   INSIDE the flip, on another thread, while our present is running. A flag
   cleared at the start of a present and set at its end leaves a window exactly
   one present wide in which the engine's draw is not suppressed — which is one
   engine cursor per frame, i.e. the bug, wearing the counters of a fix. */
int tagpu_gui_cursor_drew_take(void);
/* ON A SHELL FRAME THERE IS NO CURSOR STATE, and that is a deliberate,
   NAMED consequence of landing 4c rather than an oversight. The cursor's
   position and sprite used to be read live out of the graphics globals here,
   on the render thread, every present — in play and on the menus alike. They
   are packet fields now, and the publisher only publishes from the in-play
   gate, so a shell frame holds the out-of-game packet and this returns "no
   rect": we do not own the cursor there and the layer does not erase the
   engine's, so the ENGINE's own cursor is what the player sees on the menus.

   At k = 1 that is the same art; at k > 1 it is the engine's 10x20 sprite
   blown up instead of ours at the device's resolution, which is what G17c
   improved. Closing it needs a channel the shell can publish on, and "never
   publish outside the 0x4969D2 gate" is a rule of the plan's — so it is left
   here, stated, for the landing that builds one.

   AND THE RECT CAN BE ONE ANIMATION STEP BEHIND, which the 120-stop `strict`
   walk measured rather than reasoned about. The engine's cursors PULSE -- the
   move cursor cycles 27x27, 29x29, 31x31, 33x33, 35x35, one pixel per side per
   step, on this build and on the one before it alike. The rect below is the
   packet's, taken inside DrawGameScreen, and the engine blits its cursor onto
   the primary AFTER that: so the surface the layer composites against can hold
   the next step, and where that step is larger its outermost ring is neither
   drawn by the sharp layer nor exempt. On the move cursor's four-way arrow
   that ring is four isolated tips -- 4 pixels, at two of forty-five in-game
   stops, against 0 on landing 3's DLL. With the fallback on they carry the
   engine's own cursor art, so what a frame shows is the union of two adjacent
   steps of one sprite. THE FIX IS A BOUND, NOT A MARGIN: the rect must cover
   the sprite the engine will blit NEXT, which means publishing the cursor
   table's animation extent (per-process and immutable, so a constant once
   read) -- and nothing has established that table's shape. Padding by a guess
   is the timing argument CLAUDE.md refuses. GUI renderer 23 has it. */
void tagpu_gui_cursor_frame(const struct TAGPU_PACKET* packet);
int  tagpu_gui_cursor_own(float* r);


/* WHETHER THE PACKET CARRIES THE THREE MINIMAP SURFACES (landing 4c). The
   sharp minimap raises it from its own frame — once per present, whenever it
   would draw — and `tagpu_gui_flush`'s watchdog drops it after 90 silent
   frames, the same shape tagpu_fxown uses for the effect tables. It costs the
   publisher an ew x eh x 3 interleave per publish when set, and at k = 1 the
   sharp minimap is deliberately the engine's own, so an unarmed frame must not
   pay for it. Written on the render thread, read on the game thread; one
   writer, no ordering owed — a frame either side of the change costs one frame
   of the engine's own minimap. */
void tagpu_gui_set_want_minimap(int on, unsigned int frame_counter);
int  tagpu_gui_want_minimap(void);
/* THE LEVEL'S MINIMAP PICTURE IS ACKNOWLEDGED, NOT ASSUMED (landing 4c, and its
   review). The publisher puts it in a packet and the mailbox is latest-wins:
   a packet the render thread never takes is a counted statistic, not an error,
   so "it went into one packet" is not "the consumer has it". The render half
   raises this to `level_gen + 1` the moment it has copied that level's picture,
   and the publisher keeps sending until it does — which is normally one extra
   packet and never more than the frames it takes the consumer to run once.
   0 = nothing held. Written on the render thread, read on the game thread; one
   writer, monotone within a level. */
void     tagpu_gui_set_minimap_have(unsigned level_gen_plus_1);
unsigned tagpu_gui_minimap_have(void);

/* ==================================================================== G19f ==
   THE VULKAN LANE'S HAND-OVER — the op stream this present applied, copied
   while it was live.

   WHY A COPY AND NOT A POINTER INTO THE QUEUE. render_ogl.c's iteration is
   `tagpu_packet_acquire` -> `tagpu_overlay_draw` (inside which
   `tagpu_gui_present` drains) -> `tagpu_packet_frame_end` -> `tagpu_vk_frame`.
   `drain()` advances the queue's arena tail PER OP, so the moment it returns
   the game thread may overwrite the bytes those ops point into -- and the
   Vulkan lane does not run until two calls later. A pass reading
   `g_guiq.arena + aoff` would be tagpu_terr.c's fog-grid bug on a 16 MB
   buffer. So the render half copies what the port needs, as it applies each
   op, into an arena of its own. tagpu_terr.c keeps the buffer glTexImage2D was
   handed and tagpu_gaf.c grows a CPU mirror for exactly the same reason.

   IT IS OPT IN. Nothing is copied until `tagpu_gui_mirror_want(1)` is called,
   which the Vulkan pass does when its lever is armed and undoes when it is
   not, so an unarmed session pays nothing at all. That is the only reason
   copying an op stream that can run to 20 000 ops a present is affordable.

   THIS IS NOT THE PRODUCER'S QUEUE STRUCT. `TAGPU_PUBOP` is private to the
   tagpu_gui_* family (tagpu_gui_int.h) and describes what the GAME thread
   published; this describes what the RENDER half actually did with it, which
   is the only thing a second backend has to reproduce. Keeping them separate
   means a change to the queue cannot silently change the port's contract.  */

enum { TAGPU_GUICOL_DST = 1, TAGPU_GUICOL_ON = 2 };   /* TAGPU_GUIOP::col */

enum {
    TAGPU_GUIOP_SEED = 1,   /* the surface's bytes, whole: `arena` at aoff    */
    TAGPU_GUIOP_FREE,       /* the surface is gone                            */
    TAGPU_GUIOP_CLEAR,      /* coverage 0 over the box                        */
    TAGPU_GUIOP_PIXELS,     /* the box's bytes at aoff                        */
    TAGPU_GUIOP_SPRITE,     /* a keyed GAF quad; the atlas rect is RESOLVED   */
    TAGPU_GUIOP_COPY,       /* twin -> twin, the source's box at (sl, st)     */
    TAGPU_GUIOP_RESET,      /* forget every twin                              */
    TAGPU_GUIOP_STRING,     /* TA's own glyphs, stamped into the twin         */
    TAGPU_GUIOP_BAR,        /* landing 8a: the box filled with palette index
                               `fg`, fully covered. No arena bytes.            */
    TAGPU_GUIOP_RECT        /* landing 8b: the box's four INCLUSIVE EDGES in
                               palette index `fg`, one pixel wide, interior
                               untouched. No arena bytes.                      */
};

typedef struct TAGPU_GUIOP {
    unsigned char  kind;            /* TAGPU_GUIOP_*                          */
    unsigned char  ck;              /* sprite: the colour key                 */
    unsigned short fw, fh;          /* sprite: the frame's size               */
    unsigned       surf, src;       /* destination / copy source, by pixel base */
    short          l, t, r, b;      /* destination box, INCLUSIVE, surface px */
    short          sl, st;          /* copy: source top-left; sprite: dst pos */
    int            w, h;            /* seed: the surface's geometry           */
    unsigned       aoff, alen;      /* into TAGPU_GUIHAND::arena              */
    /* ---- STRING (landing 2). The three colour arguments of 0x4CCF60 as
       BYTES, and `sl`/`st` are the PEN the GL lane started from -- already
       past the font's own y offset, which the blitter subtracts.
       `aoff`/`alen` carry `nglyph` cells of four `short` each: the atlas x, y,
       w and h the GL lane RESOLVED. They are carried rather than looked up
       again for a harder reason than the sprite's: `twin_string` resolves
       against an atlas that can REPACK mid-string -- it retries once for
       exactly that -- so a second lookup here could name texels that have
       moved since, and the A/B would be comparing two atlases. */
    unsigned char  fg, bg, tr;
    unsigned short nglyph;

    /* ---- CLASSIC++ (landing 4). WHAT THE GL LANE DID ABOUT COLOUR, carried
       for the fourth time on this pass rather than re-derived, and here the
       reason is a lifetime rather than a moving input: whether `twin_colour`
       made the destination's colour attachment depends on `s_colValid` and on
       `s_atlas.rgb`, both of which the render half settles in `restore_step`
       BEFORE the drain -- so a consumer asking again later would be asking a
       different question about the same frame.
         TAGPU_GUICOL_DST   the destination twin HAS a colour attachment after
                            this op (`twin_colour` ran, or it already had one)
         TAGPU_GUICOL_ON    the program's own `uRestored` (SPRITE) or
                            `uSrcHasCol` (COPY) was 1
       Both are 0 on every op of a session that is not restoring, which is what
       makes this landing free when Classic++ is off. */
    unsigned char  col;

    /* THE ATLAS RECT THE GL LANE RESOLVED, not one this pass looks up again.
       "The port must not re-derive the pass's inputs" (roadmap, How a ported
       pass is A/B'd): a second lookup could answer differently after a repack
       and the A/B would then be comparing two atlases. Valid for SPRITE. */
    float          u0, v0, u1, v1;
} TAGPU_GUIOP;

/* ---- THE SHARP LAYER (landing 3) ----------------------------------------
   One RGBA8 at DEVICE resolution, row 0 the viewport's TOP, cleared to
   (0,0,0,0) every frame and composited above the mirror where its alpha says
   it has coverage. It is not a twin and not an op stream: it has three clients
   drawn in a FIXED order -- the harness's flat quads, the cursor, then the
   minimap (base, then its view box) -- so what crosses is the short ORDERED
   list of quads they produced, at most `TAGPU_GUI_SDRAW_MAX` of them.

   EVERY FIELD HERE IS RESOLVED BY THE GL LANE AND NONE IS RE-DERIVED, which
   on this client is not a nicety: `sharp_cursor` takes the pointer position
   from `mouse_last_client()` AT DRAW TIME, and the Vulkan pass runs later in
   the same iteration of render_ogl.c's loop -- so re-reading it would place
   the cursor where the mouse has moved to since. Same rule as the sprite's
   atlas rect and the string's glyph cells, and the third landing it decides.
   The minimap's box (the packet's, through the HUD map and the `hq8` scale)
   and its view rect (`main+0x142CB`, four edges at one GAME pixel each, in the
   colour the palette gives `mm_viewcol`) are resolved for the same reason. */
enum { TAGPU_GUISK_FLAT = 1,    /* SHARP_FS: one colour, no sampler          */
       TAGPU_GUISK_CURSOR,      /* CURS_FS:  the UI atlas + the palette      */
       TAGPU_GUISK_MM };        /* MM_FS:    the picture, the engine's pair  */

typedef struct TAGPU_GUISDRAW {
    int   kind;
    float dst[4];               /* x0, y0, x1, y1 in LAYER pixels           */
    float uv[4];                /* CURSOR: the resolved atlas rect; MM: 0..1 */
    float col[4];               /* FLAT: rgba, already through the palette   */
    int   ck;                   /* CURSOR: the atlas entry's colour key      */
} TAGPU_GUISDRAW;

/* 2 harness quads + 1 cursor + 1 minimap base + 4 view-box edges = 8. Doubled,
   because a bound wants room and this one is carried BY VALUE. */
#define TAGPU_GUI_SDRAW_MAX 16

typedef struct TAGPU_GUIHAND {
    /* THE FRAME THIS WAS PUBLISHED ON. `tagpu_gui_handover` refuses any other:
       every pointer in here aliases a buffer this module reuses next present. */
    unsigned frame;

    const TAGPU_GUIOP*   ops;       /* in the order the drain applied them    */
    unsigned             nops;
    const unsigned char* arena;     /* seed / pixels / sprite payloads        */
    unsigned             alen;

    /* THE SURFACE THE COMPOSITE DRAWS, and its geometry. 0 when the drain left
       none presented, which is a frame the GL lane drew nothing on either. */
    unsigned             presented;
    int                  surfW, surfH;

    /* The UI atlas's texels, as bytes: a second backend cannot read a GL
       texture. `atlasSerial` says when they last moved, so the port uploads on
       a change and not per frame -- tagpu_feat.h's `atlasSerial` exactly. */
    const unsigned char* atlas;
    int                  atlasDim, atlasRows;
    unsigned             atlasSerial;

    /* ---- THE RESTORED UI ATLAS (landing 4), RGBA8, same dim and same shelf.
       IT IS THE RESTORER'S OUTPUT AND NOT AN INPUT ANYONE CAN RE-DERIVE: the
       five shaders that produce it are the one thing G19c did not translate,
       so the port does not run them -- it is handed the texels the GL lane's
       job already painted, read back out of the twin on the frames that job
       painted on and on no others (`tagpu_gaf_atlas_mirror_rgb`). That is what
       unblocks this landing: a second backend needs the TEXELS, never the
       producer. Alpha is "this texel has restored colour", exactly as it is to
       `SPR_FS` and `LAY_FS`; NULL while nothing is restored. */
    const unsigned char* atlasRgb;
    int                  atlasRgbRows;
    unsigned             atlasRgbSerial;

    /* EVERY COLOUR TWIN WAS INVALIDATED SINCE THE LAST FRAME THIS MOVED. The
       presented palette moved out from under the restored art and settled
       somewhere else, so `restore_step` frees the job, re-arms it against the
       new palette and clears every colour twin whole. It happens BEFORE the
       drain, so a consumer applies it before this frame's ops -- and it cannot
       be inferred from `atlasRgbSerial`, which also moves for an ordinary
       paint. Monotone, never reset. */
    unsigned             colRearm;

    /* ---- the sharp layer's draws, BY VALUE. 16 x 60 bytes is small enough
       to copy and it retires the whole question of what they point into: the
       op arena is reallocated per present, and this list is not in it. */
    int                  sharpW, sharpH;   /* 0 = no layer this frame        */
    int                  nsdraw;
    TAGPU_GUISDRAW       sdraw[TAGPU_GUI_SDRAW_MAX];

    /* the minimap's own two textures. The picture is the TNT's 252-px base
       ALREADY RESOLVED through the presented palette (it is sampled as colour
       and not as an index -- interpolating palette indices is meaningless, and
       at 1 < k < 2 the box is smaller than the picture, so it is a downsample);
       `mmPicGen` moves on a map load and when the palette serial does.
       The pair is the engine's OWN two 126-px minimap surfaces, RGB8 and not
       RG8: `MM_FS` reads three channels -- r the fogged base, g the unfogged
       one, b the composite the engine drew its dots and arcs into. */
    const unsigned char* mmPic;
    int                  mmPicW, mmPicH;
    unsigned             mmPicGen;
    const unsigned char* mmEng;
    int                  mmEngW, mmEngH;

    /* the GLYPH atlas, which `tagpu_text.c` already keeps as bytes.
       `glyphSerial` IS THE CONTENT SERIAL AND NOT THE REPACK GENERATION. It
       moves whenever the atlas's bytes move, an ordinary new glyph included;
       `tagpu_text_glyph_gen()` moves only on a repack and is NOT carried here,
       because the one thing a consumer would do with it -- decide whether the
       cells it was handed are still valid -- is settled on this side by
       `lost` below. Keying an upload on the generation uploads once and then
       misses every glyph seen afterwards. */
    const unsigned char* glyphs;
    int                  glyphW, glyphH;
    unsigned             glyphSerial;

    const unsigned char* pal;       /* 256 x RGBA8, tagpu_pal_live()          */
    unsigned             palSerial;

    /* THE ENGINE'S OWN FRAME, which the composite samples as its bottom layer
       and its stale-mirror guard compares against. The GL lane reads it as
       `f->surface_tex`; a Vulkan lane cannot, so the bytes are copied here
       from the fork's primary under `g_ddraw.cs` -- the same lock and the same
       argument tagpu_pal.c makes for the palette (the game thread NULLs the
       primary inside that section). NULL when there is none this frame, which
       is what turns the guard off. 8bpp, `engPitch` bytes a row. */
    const unsigned char* eng;
    int                  engW, engH, engPitch;

    /* ---- the composite's uniforms, as draw_layer set them ---- */
    int   strict, key, vpKey;
    float vpL, vpT, vpW, vpH;       /* the TRUE viewport, from the packet     */
    float curEng[4];                /* the engine cursor's rect to erase      */
    int   curOurs, guard;
    float scaleX, scaleY;           /* k, and the ramp's width with it        */
    float hud[4];
    int   vpX, vpY, vpW_gl, vpH_gl; /* the GL viewport the composite drew into */

    /* WHAT THIS LANDING DOES NOT CARRY, counted rather than dropped silently.
       A frame with any of these is refused whole, in the shape every world
       pass refuses what it has no copy of: drawing the rest would be a
       different picture and the A/B would call it a rasteriser difference. */
    /* THIS RECORD DOES NOT CARRY THIS FRAME'S OPS AND REPLAYING IT WILL NOT
       CATCH THE STORE UP. Published rather than withheld, because withholding
       is indistinguishable from "the lane is not armed" and leaves the consumer
       believing it is level when it is a frame behind -- silently, for the rest
       of the session. Two causes today: the mirror's op array or arena refused
       to grow mid-frame, and the glyph atlas REPACKED between one string op
       being recorded and the end of the drain, which invalidates the cells of
       every string already recorded in this frame. The consumer's answer is the
       behind state, not a refusal. */
    int   lost;
    int   otherOps;                 /* ops still not carried, if any ever are */
    /* `uColOn` AS `draw_layer` SET IT: the presented surface has a colour twin
       AND the palette-validity rule (gui-renderer.md 3.4) says it may be read
       this frame. Landing 3 and before could only stand the composite down on
       it; landing 4 composites it. */
    int   colourTwins;
    int   sharpOn;                  /* the sharp layer had COVERAGE this frame
                                       -- not merely that it exists, which it
                                       does on every frame once it is made */

    /* 1 on the ONE frame the GL half captured its half of the A/B. */
    int   ab;
} TAGPU_GUIHAND;

/* Ask the render half to keep the mirror above. Render thread only. */
void tagpu_gui_mirror_want(int on);

/* ASK THE PRODUCER FOR A FRESH START, through the very flag the GL consumer
   raises for itself. The mirror's consumer keeps a twin store of its own, and
   any frame it refuses leaves that store behind the GL one -- so the next op
   naming a twin it never made would apply to nothing, silently, for the rest
   of the session. This is the same request `drain()` makes when a sprite
   arrives without its bytes: the producer publishes a RESET and re-seeds every
   surface, and both stores start level again. Render thread only. */
void tagpu_gui_mirror_reseed(void);

/* 0 when there is nothing to hand over, when this frame's has already been
   taken, or when the standing one was published on a different frame than
   `now` -- the fork's monotonic render-thread counter, which a Vulkan pass has
   as TAGPU_VKPASS::frame. Render thread only. */
int tagpu_gui_handover(TAGPU_GUIHAND* out, unsigned now);
#endif
