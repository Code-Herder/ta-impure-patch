#ifndef TAGPU_GUI_H
#define TAGPU_GUI_H
/* tagpu_gui -- the engine's UI, observed on the game thread and redrawn by us
   on the render thread (research/notes/gui-renderer.md).

   THE OBSERVERS. Detours on the engine's pixel-writing leaves record what the
   UI draws -- which sprite, which string, at which coordinates, into which
   surface -- while the engine goes on drawing its own 8bpp surface exactly as
   it always did. That record is the engine's UI stated SEMANTICALLY, and the
   publisher hands it to the render thread as an op queue (tagpu_gui_int.h).

   THE LAYER. `tagpu_gui_present` (tagpu_gui_surf.c) drains that queue at every
   present into retained twins, one per engine surface, and fills the
   hand-over `tagpu_vk_gui.c` replays on the device: the presented surface's
   twin is drawn over the world passes, palette-resolved wherever an op wrote
   and discarded everywhere else, under a device-resolution sharp layer whose
   clients are the cursor and the minimap. Nothing of the engine's own frame
   is beneath it: `LAY_FS` declares no sampler for TA's surface.

   THE HOOK ALSO HOSTS THE HARNESS. `tagpu_gui_hook.c`'s `before_flip` is the
   only host of `tagpu_triggers_frame`, so every `tacli` verb in the fork is
   dispatched from inside it. The golden source -- the engine's own composed
   frame, captured by `tagpu_surf_capture` on the game thread -- is not this
   module's and is drawn nowhere (tagpu_vk_surf.c).

   Family: tagpu_gui_hook.c (the observers, the census, the publisher),
   tagpu_gui_surf.c (the twins, the UI atlas, the replay), tagpu_vk_gui.c (the
   Vulkan pass), tagpu_gui_snap.c (the gadget-tree snapshot behind `tacli ui`,
   contract inc/tagpu_ui.h). One trigger, gamedir/tagpu_gui.on;
   the detours install at DllMain when it exists then.

   Tokens in tagpu_gui.on: `census` (the census diff; costs a 1024x768 compare
   per 5 ms), `log`, `pgm`, `trace`, `key=N` (census diagnostics,
   tagpu_gui_hook.c, read at attach). The draw's tokens -- `off`,
   `norestore`, `sharptest`, `nocursor`, `mmbase`, `nominimap`,
   `cursorscale=` -- are tagpu_gui_surf.c's and follow the file live.

   THREADS. The observers and the publisher run on the game thread inside the
   engine's own calls; the render thread drains the queue at every present.
   One producer, one consumer, no lock (tagpu_gui_int.h).

   Every observer calls the original, so the engine's behaviour is
   byte-identical with the module armed. */#include <windows.h>
#include "tagpu.h"

void tagpu_gui_init(void);                          /* DllMain                */
void tagpu_gui_flush(unsigned int frame_counter);   /* render thread: the heartbeat line */
int  tagpu_gui_installed(void);
unsigned tagpu_gui_flips(void);                     /* the publisher's flip count, game thread */
/* WINDOW PROCEDURE: a WM_MOUSEWHEEL the zoom did not take, lParam in the
   engine's space. It only queues the notch; the game thread scrolls the list
   under its point at the next GUI pump (tagpu_gui_hook.c, "the list wheel"). */
void tagpu_gui_wheel(UINT msg, WPARAM wparam, LPARAM lparam);

/* RENDER THREAD, PER PRESENT: poll the trigger, drain the queue into the twins
   and fill the Vulkan pass's hand-over. It APPLIES the op stream -- resolving
   each sprite to its atlas rect and stamping each string from the glyph atlas
   -- and draws nothing itself; `tagpu_vk_gui.c` replays the result on the
   device.

   IT DOES NOT composite the engine's own frame underneath: `LAY_FS` does not
   declare a sampler for TA's surface, so nothing on this path can put an
   engine pixel on the screen. The golden source is captured separately and drawn nowhere. */
void tagpu_gui_present(const TAGPU_FRAME* f);

/* THE CURSOR'S ONE DECISION FOR THIS FRAME, taken before anything reads it
   (gui-renderer.md 13.5). Called from `tagpu_overlay_draw` ahead of the
   world pass and `tagpu_gui_present`, so every reader of the cursor state in
   one frame agrees about it; it latches THIS frame's packet for the whole of
   the UI's render half, which is where the position and the sprite come from.

   The engine's cursor goes into its own surface, which is where the
   reference wants it, and ours is the sharp layer's. */
void tagpu_gui_cursor_frame(const struct TAGPU_PACKET* packet);

/* THE ENGINE'S CURSORS PULSE (gui-renderer.md 13.5, 17 and 23): the move
   cursor cycles 27x27 to 35x35, one pixel per side per step, so a rect taken
   from one frame's sprite can be one animation step behind the sprite the
   engine blits next. Anything that bounds the cursor needs the cursor table's
   animation extent, not a margin. */
struct TAGPU_PACKET;

/* WHETHER THE PACKET CARRIES THE THREE MINIMAP SURFACES. The sharp minimap
   raises it from its own frame — once per present, whenever it
   would draw — and `tagpu_gui_flush`'s watchdog drops it after 90 silent
   frames, the same shape tagpu_fxown uses for the effect tables. It costs the
   publisher an ew x eh x 3 interleave per publish when set, and at k = 1 the
   sharp minimap is deliberately the engine's own, so an unarmed frame must not
   pay for it. Written on the render thread, read on the game thread; one
   writer, no ordering owed — a frame either side of the change costs one frame
   of the engine's own minimap. */
void tagpu_gui_set_want_minimap(int on, unsigned int frame_counter);
int  tagpu_gui_want_minimap(void);
/* THE LEVEL'S MINIMAP PICTURE IS ACKNOWLEDGED, NOT ASSUMED. The publisher puts
   it in a packet and the mailbox is latest-wins: a packet the render thread
   never takes is a counted statistic, not an error, so "it went into one
   packet" is not "the consumer has it". The render half raises this to
   `level_gen + 1` the moment it has copied that level's picture, and the
   publisher keeps sending until it does — which is normally one extra packet
   and never more than the frames it takes the consumer to run once. 0 = nothing
   held. Written on the render thread, read on the game thread; one writer,
   monotone within a level. */
void     tagpu_gui_set_minimap_have(unsigned level_gen_plus_1);
unsigned tagpu_gui_minimap_have(void);

/* ===========================================================================
   THE VULKAN PASS'S HAND-OVER — the op stream this present applied, copied
   while it was live.

   WHY A COPY AND NOT A POINTER INTO THE QUEUE. render_vk.c's iteration is
   `tagpu_packet_acquire` -> `tagpu_overlay_draw` (inside which
   `tagpu_gui_present` drains) -> `tagpu_packet_frame_end` -> `tagpu_vk_frame`.
   `drain()` advances the queue's arena tail PER OP, so the moment it returns
   the game thread may overwrite the bytes those ops point into -- and the
   Vulkan pass does not run until two calls later. So the render half copies
   what the port needs, as it applies each op, into an arena of its own, as
   tagpu_terr.c copies its fog grid out of the packet for the same reason.

   IT IS OPT IN. Nothing is copied until `tagpu_gui_mirror_want(1)` is called,
   which the Vulkan pass does when its lever is armed and undoes when it is
   not, so an unarmed session pays nothing at all. That is the only reason
   copying an op stream that can run to 20 000 ops a present is affordable.

   THIS IS NOT THE PRODUCER'S QUEUE STRUCT. `TAGPU_PUBOP` is private to the
   tagpu_gui_* family (tagpu_gui_int.h) and describes what the GAME thread
   published; this describes what the RENDER half actually did with it, which
   is the only thing the Vulkan pass has to reproduce. Keeping them separate
   means a change to the queue cannot silently change the port's contract.  */

/* EITHER ENGINE REMAP TABLE'S SHAPE, AND THE SHAPE IS THE BOUND. The lighten
   table `globals+0xC8` and PALETTE.SHD `globals+0xC4` are each 32 rows of 256
   (`0x4BA610` / `0x4BA660` allocate 0x2000 bytes apiece). `0x4CC8DF` indexes
   the lighten table `[(row << 8) | dst]` with `dst` zero-extended, and
   `0x4BF4D0` clamps its row to `<= 0x1F` (`0x4BF595`) or `>= -0x20`
   (`0x4BF572`), so no writer can reach a 33rd row -- the same argument
   `tagpu_packet_pub.c`'s `shd_snapshot` and `lht_snapshot` make for the
   world's copies. */
#define TAGPU_GUI_TABLE_ROWS  32u

/* THE HAND-OVER'S REMAP TABLE: 96 rows of 256, which is what a
   `TAGPU_GUIOP_TINT`'s `fg` indexes.

     rows  0..31  the lighten table as the engine holds it -- what a FOCUS
                  edge (`0x4BF7B0` -> `0x4CC8DF`) remaps through, row = level.
     rows 32..95  one row per BOX-SHADER level L = -32..31 (`0x4BF4D0`), at
                  `TAGPU_GUI_SHADE_BOX + L + 32`, precomputed by the publisher
                  from the two engine tables.

   WHY THE BOX SHADER GETS ROWS OF ITS OWN rather than an index into the two
   raw tables: `0x4BF4D0` reads its destination SIGNED (`movsx ebx,BYTE PTR
   [ecx]` at `0x4BF5E8`), so a byte 0x80..0xFF lands 256 bytes back -- in the
   PREVIOUS row of the same table. Its effective remap for one level is
   therefore one 256-byte row, `T[r][i]` for i < 128 and `T[r-1][i]` above, and
   carrying that row keeps the shader a plain table lookup with nothing about
   signedness in it. For r = 0 the upper half reads the heap in front of the
   table, which nothing can reproduce: those two levels (-32 and 0) are never
   published, and their rows' upper halves are zero and unaddressed. */
#define TAGPU_GUI_SHADE_BOX   32u
#define TAGPU_GUI_SHADE_ROWS  (TAGPU_GUI_TABLE_ROWS + 64u)
#define TAGPU_GUI_SHADE_BYTES (TAGPU_GUI_SHADE_ROWS * 256u)

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
    TAGPU_GUIOP_BAR,        /* the box filled with palette index `fg`, fully
                               covered. No arena bytes.                        */
    TAGPU_GUIOP_RECT,       /* the box's four INCLUSIVE EDGES in palette index
                               `fg`, one pixel wide, interior untouched. No
                               arena bytes.                                    */
    TAGPU_GUIOP_TINT        /* the box REMAPPED THROUGH ROW `fg` of
                               `TAGPU_GUIHAND::shade` --
                               `idx = shade[fg * 256 + idx]`, coverage
                               unchanged. No arena bytes. One pixel thick for a
                               focus edge, any size for the box shader
                               `0x4BF4D0` (a list's selected row, the dimming
                               under a modal screen).

                               THE ONE OP IN THE STREAM THAT READS ITS OWN
                               DESTINATION, which is what a consumer has to
                               plan for rather than discover: the box must be
                               snapshotted before the draw that rewrites it,
                               because sampling an attachment a draw is writing
                               is undefined in Vulkan (the same rule that
                               makes `TAGPU_GUIOP_COPY` refuse a self-copy).

                               AND THE ORDER OF THESE OPS IS LOAD-BEARING where
                               no other kind's is: the four edges of one focus
                               rectangle SHARE THEIR CORNERS, so each corner is
                               remapped twice and applying them out of order,
                               or in parallel from one snapshot, gives a
                               different picture at four pixels per rectangle.
                               `0x4BF7B0`'s own order is top, right, bottom,
                               left.                                          */
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
    /* ---- STRING. The three colour arguments of 0x4CCF60 as BYTES, and
       `sl`/`st` are the PEN the render half started from -- already past the
       font's own y offset, which the blitter subtracts.
       `aoff`/`alen` carry `nglyph` cells of four `short` each: the atlas x, y,
       w and h the render half RESOLVED. They are carried rather than looked up
       again for a harder reason than the sprite's: `twin_string` resolves
       against an atlas that can REPACK mid-string -- it retries once for
       exactly that -- so a second lookup here could name texels that have
       moved since. */
    unsigned char  fg, bg, tr;
    unsigned short nglyph;

    /* ---- CLASSIC++. WHAT THE RENDER HALF DID ABOUT COLOUR, carried rather
       than re-derived, and here the reason is a lifetime rather than a moving
       input: whether the drain (`twin_sprite`, `twin_copy`) gave the
       destination a colour attachment depends on `s_colValid`, which the
       render half settles in `restore_step` BEFORE the drain -- so a consumer asking again later would be asking a
       different question about the same frame.
         TAGPU_GUICOL_DST   the destination twin HAS a colour attachment after
                            this op (the drain gave it one, or it already had
                            one)
         TAGPU_GUICOL_ON    the program's own `uRestored` (SPRITE) or
                            `uSrcHasCol` (COPY) was 1; on a SEED with bytes
                            (a decoded asset) or a PIXELS (a transformed
                            stamp), the bytes are a PICTURE the consumer
                            restores in its own store and colours the box from
                            once the restore has landed
       Both are 0 on every op of a session that is not restoring, which is what
       makes this field free when Classic++ is off. */
    unsigned char  col;

    /* THE ATLAS RECT THE RENDER HALF RESOLVED, not one this pass looks up
       again: a second lookup could answer differently after a repack. Valid
       for SPRITE. */
    float          u0, v0, u1, v1;
} TAGPU_GUIOP;

/* ---- THE SHARP LAYER -----------------------------------------------------
   One RGBA8 at DEVICE resolution, row 0 the viewport's TOP, cleared to
   (0,0,0,0) every frame and composited above the mirror where its alpha says
   it has coverage. It is not a twin and not an op stream: it has three clients
   drawn in a FIXED order -- the harness's flat quads, the minimap (base, then
   its view box), then the cursor, which covers the minimap where the two
   overlap -- so what crosses is the short ORDERED
   list of quads they produced, at most `TAGPU_GUI_SDRAW_MAX` of them.

   EVERY FIELD HERE IS RESOLVED BY THE RENDER HALF AND NONE IS RE-DERIVED,
   which on this client is not a nicety: `sharp_cursor` takes the pointer
   position from `mouse_last_client()` AT RECORD TIME, and the Vulkan pass runs
   later in the same iteration of render_vk.c's loop -- so re-reading it would
   place the cursor where the mouse has moved to since. Same rule as the
   sprite's atlas rect and the string's glyph cells.
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
       none presented. */
    unsigned             presented;
    int                  surfW, surfH;

    /* The UI atlas's texels, as bytes. `atlasSerial` says when they last
       moved, so the port uploads on a change and not per frame --
       tagpu_feat.h's `atlasSerial` exactly. */
    const unsigned char* atlas;
    int                  atlasDim, atlasRows;
    unsigned             atlasSerial;

    /* ---- THE RESTORED UI ATLAS, and these five fields are its route. The UI
       atlas arms `tagpu_gaf_atlas_restore_vk` like the feature and effects
       atlases do, so what crosses is the WORK and not the picture: a list of
       rectangles the consuming pass paints into a restored image of its own.
       The shape is `tagpu_feat.h`'s, field for field, and the same three rules
       hold --
         - `restoreFrames` aliases the producer's `rlist`, whose address is
           fixed for the life of the atlas (one `malloc`, no `realloc`), so a
           consumer may hold it for the frame;
         - `restoreGen` is a CURSOR RESET and nothing else: while it is
           unchanged the consumer takes the tail past what it has taken, and
           when it moves the job is rebuilt from index 0;
         - `restoreBlanks` counts the generations that BLANKED the destination,
           so "keep what you have" cannot be believed over an atlas the
           producer cleared in the same frame. */
    const struct TAGPU_RGLSL_FRAME_S* restoreFrames;  /* NULL = nothing to restore */
    int                  restoreN;
    unsigned             restoreGen;
    int                  restoreRepaint;
    unsigned             restoreBlanks;

    /* EVERY COLOUR TWIN WAS INVALIDATED SINCE THE LAST FRAME THIS MOVED. The
       presented palette moved out from under the restored art and settled
       somewhere else, so `restore_step` frees the job, re-arms it against the
       new palette and clears every colour twin whole. It happens BEFORE the
       drain, so a consumer applies it before this frame's ops. This is the
       only thing that says a colour twin was invalidated. Monotone, never
       reset. */
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

    /* THE REMAP TABLE: `TAGPU_GUI_SHADE_ROWS` rows of 256 bytes, row major
       (the layout is above), and the only thing a `TAGPU_GUIOP_TINT` needs
       beyond its box.
       NULL until a `PK_SHADE` has been drained, which the producer publishes
       ahead of the first tint of a batch -- so a tint op and a null table
       cannot both be in one hand-over, and a port that finds them together is
       looking at a bug rather than at a state to cope with.

       `shadeSerial` is the atlas's rule, not the palette's: it moves when the
       BYTES move, which for this table is once a session in practice (the
       engine fills both source tables at init) and whenever the engine hands
       out a different pointer. Uploading on a change rather than per frame is the point of
       carrying a serial at all. */
    const unsigned char* shade;
    unsigned             shadeSerial;

    /* ---- the composite's uniforms, as the render half settled them ---- */
    int   vpKey;
    float vpL, vpT, vpW, vpH;       /* the TRUE viewport, from the packet     */
    float scaleX, scaleY;           /* k, and the ramp's width with it        */
    float hud[4];
    int   vpX, vpY, vpW_gl, vpH_gl; /* the viewport the composite draws into  */

    /* WHAT THIS RECORD DOES NOT CARRY, counted rather than dropped silently.
       A frame with any of these is refused whole, in the shape every world
       pass refuses what it has no copy of: drawing the rest would be a
       different picture. */
    /* THIS RECORD DOES NOT CARRY THIS FRAME'S OPS AND REPLAYING IT WILL NOT
       CATCH THE STORE UP. Published rather than withheld, because withholding
       is indistinguishable from "the pass is not armed" and leaves the consumer
       believing it is level when it is a frame behind -- silently, for the rest
       of the session. Two causes today: the mirror's op array or arena refused
       to grow mid-frame, and the glyph atlas REPACKED between one string op
       being recorded and the end of the drain, which invalidates the cells of
       every string already recorded in this frame. The consumer's answer is the
       behind state, not a refusal. */
    int   lost;
    int   otherOps;                 /* ops still not carried, if any ever are */
    /* `uColOn` AS THE RENDER HALF SETS IT: the presented surface has a colour
       twin AND the palette-validity rule (gui-renderer.md 3.4) says it may be
       read this frame. */
    int   colourTwins;
    int   sharpOn;                  /* the sharp layer had COVERAGE this frame
                                       -- not merely that it exists, which it
                                       does on every frame once it is made */

    /* the A/B claim: nonzero on the ONE frame `tagpu_gui.ab` latched it, the
       frame the Vulkan pass captures. */
    int   ab;
} TAGPU_GUIHAND;

/* Ask the render half to keep the mirror above. Render thread only. */
void tagpu_gui_mirror_want(int on);

/* THE ONE FACT THAT TRAVELS THE OTHER WAY about Classic++ colour: the consuming
   pass says whether it holds a restored UI atlas with at least one painted
   frame in it. Until it does, no op may carry `TAGPU_GUICOL_ON` -- such an op
   is a frame the consumer has to refuse whole, and the store would thrash for
   the two seconds a first restore takes rather than the art simply arriving
   late. Called from the consumer's prepare, which runs later in the same
   iteration of the render loop than the producer's present, so the producer
   reads it one frame old; that delays turning colour ON and can do nothing
   else. Render thread only, like everything else on this pass.

   `settled` COUNTS THE TIMES THE RESTORE WENT QUIET having painted something
   new, and it is the other half of the same fact. A twin takes its colour at
   the moment the art is DRAWN, so a sprite drawn while its atlas entry was
   still unrestored keeps indexed pixels for as long as nothing redraws it --
   which in game is for ever, the sidebar being drawn once per selection
   change. Each settle is the producer's cue to ask the engine for one repaint
   (`g_guiq.colarm`), after which the art is drawn against an atlas that HAS
   been restored. It converges because the set of atlas entries a screen uses
   is finite: a repaint that introduces no new entry produces no new settle. */
void tagpu_gui_col_ready(int have, unsigned settled);

/* ...AND THE SAME CUE FOR THE PICTURE STORE: how many times the consumer's
   picture restore (a backdrop, a transformed stamp -- `TAGPU_GUICOL_ON` on a
   SEED or a PIXELS op) went quiet having finished a picture drawn for the
   first time. Each one asks the engine for one repaint, so a copy out of a
   backdrop, and a stamp in a sidebar the engine draws once, are drawn again
   against the restored picture. It is not the atlas's budgeted ask: a settle
   needs a picture whose content the store has not held since it last evicted
   it, so a repaint that draws nothing new asks for nothing, and a store too
   small for a screen cannot turn eviction into a repaint loop. Render thread
   only. */
void tagpu_gui_pic_settled(unsigned settled);

/* ASK THE PRODUCER FOR A FRESH START, through the very flag the drain raises
   for itself. The mirror's consumer keeps a twin store of its own, and any
   frame it refuses leaves that store behind the render half's -- so the next op
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

