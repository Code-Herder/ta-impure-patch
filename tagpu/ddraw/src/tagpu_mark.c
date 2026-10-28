/* tagpu_mark.c — the UI marker pass. See tagpu_mark.h for the split
   between re-drawn health bars and captured everything-else, and
   tagpu_markown.h for how the capture works.

   THE HEALTH BAR, byte for byte (DrawHealthBars 0x46A430, sole caller
   0x469CB9):

     if ((s16)unit->Health[+0x108] <= 0) return;
     DrawBar({x-0x11, y-2, x+0x11, y+2}, gui[0]);          // 35 x 5, black
     w = (u32)(Health << 5) / (u32)def[+0x1FA];            // UNSIGNED divide
     DrawBar({x-0x10, y-1, x-0x10 + w, y+1},               // up to 33 x 3
             Health > 2*(maxHP/3) ? gui[0xA]               // green
           : Health >   (maxHP/3) ? gui[0xE]               // yellow
           :                        gui[0xC]);             // red
     gui[i] = *(u8*)(main + 0xDCB + i);  DrawBar edges are INCLUSIVE.

   and the caller's projection, whose +0x80/+0x20 are baked immediates exactly
   as they are in the feature leaf:

     x = (s16)unit[+0x6C] - eyeX + 0x80
     y = (s16)unit[+0x74] - eyeY - ((s16)unit[+0x70] >> 1) + 0x20 + 0xA

   Which units: the engine walks HotUnits — screen-culled at 1x — and keeps the
   ones the WATCHED player owns with the `damagebars` registry option set. We
   walk the unit array instead and cull to the zoom's effective rect, which is
   the same set at zoom >= 1 and a superset when zoomed out. Enemy units never
   get a bar, in either version.

   Sub-pixel: the engine reads the ROSTER SHORTS (the high words of its 16.16
   positions), so ITS bar steps once per sim tick, while the native unit pass
   interpolates the body between ticks. A bar on the shorts slides against the
   body by up to a whole tick of motion — 1.68 px peak-to-peak at 1x on a
   walking commander at TA's NORMAL game speed, 2.95 px at `gamespeed` 20, and
   `zoom` times either on screen (both measured) — which is the health-bar
   wobble. Stock TA cannot show it, because there the bar and the body are the
   same shorts. The gather takes `tagpu_native_unit_pos()` — the body's own
   anchor, the same one the selection box and the unit-anchored order markers
   already use.

   AND IT DOES NOT FLOOR THAT ANCHOR in the frame's PRE-zoom units, which is
   the wrong grid. The vertex shader scales this pass by `zoom` about the zoom
   centre, so a one-unit quantisation here is `zoom * ss` DEVICE pixels on
   screen, and the bar steps 2-4 px diagonally at max zoom-in while the body
   glides underneath it. The anchor keeps its fraction to the last moment and
   `snap_device` puts it on the device grid, so the step is one device pixel
   at every zoom. (The selection box does not floor in these units either: it
   is a line, and a line's ends are quantised AFTER the zoom, on the line grid
   the walk runs on -- tagpu_line.h.)

   THE BUILD CURSOR and the drag band box are re-drawn for the same reason,
   and it is the reason a captured layer can never fix them: the capture is
   bounded by the OFFSCREEN, which is the size of the screen, while the engine
   projects these two rects at the coordinates `vpwide` made addressable. At
   zoom < 1 those run far past the surface, the engine's own clipper drops the
   rect whole, and nothing reaches our buffer. The band that survived is the
   surface mapped back through the zoom, which on a 1024x768 frame at 0.467x is
   screen [307,785]x[205,563] out of an 896x704 viewport (measured) — pick a
   metal extractor, zoom out, and the green footprint exists nowhere else.
   Both rects are re-derived here from the
   globals the engine reads at `0x469DB4..0x469F23`, byte for byte:

     if (!drawUnits) return;                      // movie recorder only
     if (!(main[0x2CC6] & 8)                      // bit3: band box forced on
         && !(main[0x2CC3] == 0x0E                // cursor mode 14 = placement
              && IsPositionInRect(main+0x37E27,   // the WIDENED viewport rect
                                  main[0x2C76], main[0x2C7A]))) return;
     l = main[0x2C92] - eyeX + 0x80;   r = main[0x2C9E] - eyeX + 0x80;
     t = main[0x2C9A] - (main[0x2C96] >> 1) - eyeY + 0x20;
     b = main[0x2CA6] - (main[0x2CA2] >> 1) - eyeY + 0x20;   // all six DWORDs
     if (r < l) swap;  if (b < t) swap;
     i = main[0x2CC3] == 0x0E ? ((main[0x2CC6] & 0x40) ? 0xA : 4) : 0xF;
     DrawTranspRectangle({l,t,r,b},                 gui[i]);
     DrawTranspRectangle({l+1,t+1,r-1,b-1}, main[0x2CC3] == 0x0E ? gui[i]
                                                                : gui[0]);

   `DrawTranspRectangle 0x4BF8C0` is named for its hollow centre: it is four
   1-px edges through `0x4CC7AB`, a store-only Bresenham whose axis-aligned
   runs count both endpoints, so each edge is one inclusive `put_bar`. The
   `drawUnits` arm is not reproduced — the only caller that passes 0 is TA's
   own movie recorder (`0x4962C2`), which this pass never runs under. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tagpu_opt.h"
#include "tagpu_mark.h"
#include "tagpu_markown.h"
#include "tagpu_native.h"
#include "tagpu_order.h"
#include "tagpu_text.h"
#include "tagpu_glsl.h"
#include "tagpu_line.h"
#include "tagpu_pal.h"
#include "tagpu_vk.h"      /* tagpu_vk_ab_arm, for the A/B claim */
#include "tagpu_packet.h"   /* the frame packet: the view, the tables */
#include "tagpu_recpov.h"   /* REC_CLICK_SHOW: how long a recorded click's ring lasts */
#include "tagpu_log.h"

/* ---- what the marker block reads, and where it comes from ----------------
   This file reads no engine memory at all.
   The unit array walk, the two player-id bytes, the damagebars option, the GUI
   colour table, the build cursor's corners and the dispatched mouse point all
   arrive in the packet, copied by the game thread inside the very draw whose
   markers these are.

   THE OWNER TEST'S PLAYER ID, and it is NOT the one the order driver uses.
   `DrawGameScreen` loads `main+0x2A43` into a local at `0x46967D`/`0x469689` and
   both the health bar (`0x469CA6`) and the group digit (`0x469CC9`) compare the
   unit's `owner->id` against THAT — while the order-marker driver `0x48CC30`
   picks its player range from `main+0x2A42` (`0x48CC3B`). Two different bytes,
   two loops, one block. They are written independently (`0x416B25` and
   `0x416B38`, from two separate reads in the same loader function), so they can
   differ, and a bar loop on `0x2A42` would draw our bars for a different
   player's units than the engine's whenever the two disagree. Which of the pair is "watched" and which "local" is NOT established
   here and the notes disagree with each other about it, so they are named by
   address. [BINARY-VERIFIED 2026-09-05] The packet carries both: `local_player`
   IS 0x2A43, the bar loop's, and `watched` is 0x2A42, the order driver's. */
#define CUR_BUILD    0x0E        /* the cursor mode the footprint belongs to   */

#define GUI_BLACK    0x00
#define GUI_BLOCKED  0x04
#define GUI_GREEN    0x0A
#define GUI_RED      0x0C
#define GUI_YELLOW   0x0E
#define GUI_WHITE    0x0F

#define MVST         8                       /* x,y, u,v, wx,wz, colour, z   */
#define QUADV        6
#define CURSBASE     0
#define MAXCURSV     (8 * QUADV)             /* two rects, four edges each    */
#define BARBASE      (CURSBASE + MAXCURSV)   /* and the bars after those      */
/* The placement's sites (tagpu_place.c, the packet's PK_PLACE): each the
   square's two rects. A bucket of its own, pushed only on a frame that has
   sites, rather than a larger MAXCURSV: the cursor region is pushed whole
   every frame, so a 256-site budget there would be uploaded every frame. */
#define MAXPLACEV    ((int)TAGPU_PK_MAX_PLACE * MAXCURSV)
/* The order-marker buckets (tagpu_order.c). Separate arrays rather than more
   regions of s_verts: the bar region's length is decided at gather time, so
   anything at a FIXED offset behind it would have to be uploaded across the
   whole unused bar budget every frame. Uploaded contiguously after the
   triangles instead, with the draw offsets computed at render. */
/* Sized for tagpu_order.c's MAXORD records at the design point; an overflow
   loses markers and is counted, never written past the end. */
#define MAXORDA      36000                   /* order art verts (6 a frame)  */
#define MAXORDL      18000                   /* order lines, one record each */
/* Text quads: the ShowRanges labels (up to twelve per SELECTED unit with the
   toggle on) and one group digit per watched unit, in ONE bucket — 2 400 quads,
   460 KB. The order gather runs first, so a frame that overruns this loses the
   digits rather than the labels; `s_xover` counts it and `mark: … over=N` says
   so. That ordering is deliberate (it is the engine's own draw order) but it is
   the failure mode to know: ShowRanges over a large selection is the only thing
   that can reach the cap, and it costs the digits first. The size is a digit
   for each of a design-point player's 1 500 units, and the labels besides. */
#define MAXORDX      14400                   /* text verts (6 per quad)      */

static void flog(const char* s)
{
    tagpu_log(s);
}

/* ---- arming ---- */
static int s_armed = -1;
static int s_log = 0, s_passive = 0, s_bars = 1, s_selbox = 1;
static int s_cursor = 1, s_digits = 1;
static unsigned s_armCheck = 0;

#define MK_ABFILE  "tagpu_mark.ab"
static int s_ab, s_abDone, s_abFrame;
static int s_abTaking;

int tagpu_mark_armed(unsigned frame_counter)
{
    int was;
    char buf[128];
    int n;
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return s_armed > 0;
    s_armCheck = frame_counter;
    was = s_armed;
    s_armed = 0;
    n = tagpu_opt_read("tagpu_mark.on", buf, sizeof buf);
    if (n < 0) {
        tagpu_markown_set_bars(0);
        tagpu_markown_set_selbox(0);
        tagpu_markown_set_cursor(0);
        tagpu_markown_set_digits(0);
        /* the order markers draw in THIS pass's buckets, so a disarmed mark
           pass has to hand them back at once rather than wait 90 frames for
           the watchdog to notice nothing is being drawn */
        tagpu_markown_set_orders(0);
        if (was > 0) flog("mark: disarmed");
        return 0;
    }
    /* the A/B lever, on the same beat as the arm (tagpu_fx.c's shape) */
    s_ab = GetFileAttributesA(MK_ABFILE) != INVALID_FILE_ATTRIBUTES;
    if (!s_ab) s_abDone = 0;
    {
        s_log = 0; s_passive = 0; s_bars = 1; s_selbox = 1;
        s_cursor = 1; s_digits = 1;
        if (n > 0) {
            char* p = buf;
            buf[n] = 0;
            while (*p) {
                char* q;
                int last;
                while (*p && *p <= ' ') p++;
                q = p;
                while (*q && *q > ' ') q++;
                last = (*q == 0);
                *q = 0;
                if (!lstrcmpiA(p, "log")) s_log = 1;
                else if (!lstrcmpiA(p, "passive")) s_passive = 1;
                else if (!lstrcmpiA(p, "nobars")) s_bars = 0;
                else if (!lstrcmpiA(p, "noselbox")) s_selbox = 0;
                else if (!lstrcmpiA(p, "nocursor")) s_cursor = 0;
                else if (!lstrcmpiA(p, "nodigits")) s_digits = 0;
                if (last) break;
                p = q + 1;
            }
        }
    }
    s_armed = 1;
    /* Arming only ever hands markers BACK — taking them is done from the render,
       which is the only place that knows the pass is really running. This
       function is reached at the menus too, where nothing draws, and turning the
       capture on from here would have the watchdog turn it off 90 frames later
       and this poll turn it on again, forever. (`passive` is the A/B lever:
       gather and count, but leave every marker to the engine; the capture and
       the bar skip are separate because taking the bars over without drawing
       them would simply lose them.) */
    if (s_passive || !s_bars) tagpu_markown_set_bars(0);
    if (s_passive || !s_selbox) tagpu_markown_set_selbox(0);
    if (s_passive || !s_cursor) tagpu_markown_set_cursor(0);
    if (s_passive || !s_digits) tagpu_markown_set_digits(0);
    if (was != 1) {
        char b[176];
        _snprintf(b, sizeof b, "mark: ARMED (log=%d passive=%d bars=%d "
                  "selbox=%d cursor=%d digits=%d patched=%d)", s_log, s_passive,
                  s_bars, s_selbox, s_cursor, s_digits,
                  tagpu_markown_installed());
        flog(b);
    }
    return 1;
}

/* ---- the gather's buckets ---- */

/* THE TWO BUCKETS THAT SCALE WITH THE UNIT COUNT ARE GROWN, NOT FIXED: the
   health bars (two quads per unit of the watched player) and the selection
   rects (four line records per selected unit). `tagpu_mark_gather` grows both to
   the packet's unit count before anything is emitted into them, and only the
   render thread touches either, so no pointer into one outlives a realloc.
   Each bucket is written from its own index 0; the render pushes the cursor
   rects' whole BARBASE vertices and then the bars, so in the pushed block
   the bars begin at BARBASE. */
static float  s_verts[BARBASE * MVST];  /* the cursor rects                   */
static float  s_place[MAXPLACEV * MVST]; /* the placement's sites' rects      */
static float* s_barv;  static unsigned s_barvCap;  /* bars, in vertices       */
static float  s_orda[MAXORDA * MVST];   /* order markers: art quads           */
static float  s_ordl[MAXORDL * MVST];   /* order markers: line records        */
static float  s_ordx[MAXORDX * MVST];   /* text quads: labels, then digits    */
static float* s_sel;   static unsigned s_selCap;   /* selection rects, lines  */
static int    s_saidGrow;
static int   s_nsel, s_selover;        /* their lines / rects refused, a frame */
static int   s_lfar;                   /* lines past TAGPU_LINE_FAR           */
static int   s_nbar;                   /* bars gathered (2 quads each)        */
static int   s_cBar;                   /* counted, whether emitted or not     */
static int   s_ncurs;                  /* build cursor / band box verts       */
static int   s_nplace;                 /* placement site verts, this frame    */
static int   s_cPlace;                 /* placement sites drawn, this frame   */
static int   s_cPlaceRed, s_cPlaceTake; /* of them red, and yellow (tagpu_place.h) */
static int   s_norda, s_nordl;         /* order art verts / lines, this frame */
static int   s_nordx;                  /* text verts, this frame              */
/* Where the ShowRanges labels end and the group digits begin. The engine draws
   the labels from inside the order driver at `0x469BFC` and the digit at
   `0x469CF9`, i.e. on either side of the health bars, so the bucket is drawn as
   two ranges rather than one. The order gather runs first, so the split is just
   the count after it. */
static int   s_nordxOrd;
static double s_px;                    /* one SCREEN pixel in game-frame units */
static double s_zoom, s_zcx, s_zcy;    /* the transform the text snap inverts  */
/* DrawLine's clip rect -- the viewport, inclusive -- and the surface, in
   line-grid pixels, latched by the gather for put_line (tagpu_line_clip) */
static int    s_clipL, s_clipT, s_clipR, s_clipB, s_clipW, s_clipH;
static TAGPU_LINE_AB s_lab;            /* an A/B frame's lines before the clip */
static int    s_ss;
static int   s_ntext, s_xover;         /* strings drawn / quads refused        */

/* THE SHADER SOURCES ARE A BUILD INPUT, NOT CODE THIS FILE RUNS -- this file
   references none of them. `tools/spirv-gen.py` reads
   them out of the PREPROCESSED translation unit, so they have to stay here, at
   file scope, spelled exactly `static const char* NAME =`: its `_DECL` regex
   wants the `=` straight after the name, which rules out an
   `__attribute__((unused))` and is why the warning is turned off around them
   instead. Deleting them is not an option either -- `make` fails with "the
   manifest names tagpu_mark::VS and the source does not have it". */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec2 aWorld;\n"
    "layout(location=3) in float aCol;\n"
    "layout(location=4) in float aDepth;\n"
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "out vec2 vUV; out vec2 vWorld; out float vCol;\n"
    "void main(){\n"
    /* the same scale-about-the-view-centre every world pass uses, so a marker
       tracks the unit it belongs to at any zoom */
    "  vec2 p = (aPos - uZoomC) * uZoom + uZoomC;\n"
    /* markers are the frame's top layer, drawn with the depth test off, and
       carry 0 here; the selection rects alone carry a real z and test it, and
       they are lines, drawn by LVS below -- see tagpu_mark_emit_selbox */
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0, aDepth, 1.0);\n"
    "  vUV = aUV; vWorld = aWorld; vCol = aCol;\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; in vec2 vWorld; in float vCol;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uLayer;\n"
    "uniform sampler2D uPal;\n"
    "uniform int uKey;\n"
    "uniform int uText;\n"
    TAGPU_GLSL_FOG_UNIFORMS
    TAGPU_GLSL_FOG_FN
    "void main(){\n"
    /* Markers drawn before the engine's fog overlay are darkened by it and
       vanish where it paints solid black — terrain already paints that black,
       so discarding is what the engine's frame would show. The post-fog layer
       is drawn with uFog 0 and skips all of it, exactly as the engine never
       darkens the build cursor. */
    TAGPU_GLSL_FOG_DISCARD
    "  int pi;\n"
    "  float a = 1.0;\n"
    /* Text is a COVERAGE mask, not a palette image: tagpu_text.c rasterises TA's
       glyphs with (fg,bg,transparent) = (255,0,0), so the texel says only whether
       the glyph covers this fragment and the colour comes from the vertex — which
       keeps the atlas colour-free and lets the same string be drawn in any
       colour for one raster. */
    "  if (uText != 0) {\n"
    "    if (texture(uLayer, vUV).r < 0.5) discard;\n"
    "    pi = int(vCol * 255.0 + 0.5);\n"
    /* a negative u marks the flat path: the vertex carries a palette index
       instead of a texel (health bars) */
    "  } else if (vUV.x < 0.0) {\n"
    "    pi = int(vCol * 255.0 + 0.5);\n"
    "  } else {\n"
    "    pi = int(texture(uLayer, vUV).r * 255.0 + 0.5);\n"
    "    if (pi == uKey) discard;\n"
    /* the order art: a palette-index atlas whose vertices carry the opacity
       in the colour slot, the one draw this pass blends */
    "    a = vCol;\n"
    "  }\n"
    /* THE GREY BAND: the RGB rule every world pass takes (renderers.md 2.6).
       LFS below carries the same lines. */
    "  vec3 rgb = texelFetch(uPal, ivec2(pi, 0), 0).rgb;\n"
    TAGPU_GLSL_FOG_GREY_RGB("rgb")
    "  frag = vec4(rgb, a);\n"
    "}\n";
/* ---- THE LINE PROGRAM: one rule for every line ----
   Every line this pass draws -- the order lines, the range arcs, the build
   sites and the selection rects -- is tagpu_line.h's
   rule: `DrawLine`'s walk (`0x4CC7AB`) on the line grid, the world target's
   own pixels, thickened to ss pixels across its minor axis; at ss = 1 that is
   the engine's line exactly. A line primitive cannot promise any of it: even
   Vulkan's BRESENHAM mode lets an implementation deviate by one unit, so the
   pixels would be the GPU's.

   SO THE PRIMITIVE IS A BAND AND THE FRAGMENT STAGE DECIDES. One instance a
   line: `aPos` and `aUV` are its two ends, the CENTRES of their line-grid
   pixels in game units (tagpu_line.h, decided on the CPU by `put_line`
   below), and the vertex stage draws a quad around the segment between them
   (`taBand`, tagpu_glsl.h, which states the coverage bound). The fragment
   stage takes its line-grid pixel and keeps it only when `taLineKeeps` says
   the rule lights it -- integer arithmetic throughout, so the set cannot
   differ between two GPUs.

   `uGrid` is (line-grid width | ss << 16, line-grid height, target width,
   target height), set per draw by tagpu_vk_mark.c from the extent it records
   into: on a frame the offscreen target refused the world is drawn into the
   swapchain image at whatever scale that is. The samplers and the first five
   uniforms are FS's, in FS's order, so the two programs share one descriptor
   layout and one uniform window: `uGrid` lands after them at offset 32. */
static const char* LVS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"     /* end A: its line-grid pixel centre */
    "layout(location=1) in vec2 aUV;\n"      /* end B                         */
    "layout(location=2) in vec2 aWorld;\n"
    "layout(location=3) in float aCol;\n"
    "layout(location=4) in float aDepth;\n"
    "uniform vec2 uGame;\n"
    "out vec2 vWorld; flat out float vCol; flat out vec4 vLine;\n"
    TAGPU_GLSL_BAND_FN
    "void main(){\n"
    "  float t;\n"
    "  vec2 p = taBand(aPos, aUV, gl_VertexID, t);\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0, aDepth, 1.0);\n"
    "  vWorld = aWorld; vCol = aCol; vLine = vec4(aPos, aUV);\n"
    "}\n";
static const char* LFS =
    "#version 330 core\n"
    "in vec2 vWorld; flat in float vCol; flat in vec4 vLine;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uLayer;\n"
    "uniform sampler2D uPal;\n"
    "uniform int uKey;\n"
    "uniform int uText;\n"
    TAGPU_GLSL_FOG_UNIFORMS
    "uniform ivec4 uGrid;\n"
    TAGPU_GLSL_FOG_FN
    TAGPU_GLSL_LINE_FN
    "void main(){\n"
    /* gl_FragCoord has its origin top-left under Vulkan, which is the
       engine's own y -- the lane draws with no flip */
    "  if (!taLineKeeps(gl_FragCoord.xy, vLine, uGrid))\n"
    "    discard;\n"
    TAGPU_GLSL_FOG_DISCARD
    "  int pi = int(vCol * 255.0 + 0.5);\n"
    /* the grey band exactly as FS takes it */
    "  vec3 rgb = texelFetch(uPal, ivec2(pi, 0), 0).rgb;\n"
    TAGPU_GLSL_FOG_GREY_RGB("rgb")
    "  frag = vec4(rgb, 1.0);\n"
    "}\n";
#pragma GCC diagnostic pop

/* THE SHADER SOURCES ABOVE ARE THE SOURCE OF TRUTH for the Vulkan shaders.
   The build reads them out of this file (tools/spirv-gen.py, the `mark` and `mark_line` entries
   of its PROGRAMS table), translates them, and `make` fails if
   `inc/spirv/tagpu_mark.spv.h` has drifted from them. They are GLSL strings
   and nothing compiles them at run time. The gather below fills
   the vertex arrays and `mk_push` / `mk_draw` publish them to
   `tagpu_vk_mark.c`, which is what draws them with exactly these shaders. */

/* ---- the health-bar gather ---- */

/* one vertex at index `i` of the bucket `base` */
static void put_at(float* base, int i, float x, float y, float u, float v,
                   float wx, float wz, float col)
{
    float* o = base + (size_t)i * MVST;
    o[0] = x; o[1] = y; o[2] = u; o[3] = v; o[4] = wx; o[5] = wz; o[6] = col;
    o[7] = 0.0f;                       /* the top layer: no depth test reads it */
}

static void put_ord(float* base, int i, float x, float y, float wx, float wz,
                    float col)
{
    /* u < 0 is the flat path — the vertex carries a palette index rather than
       a texel, the convention the health bars share */
    put_at(base, i, x, y, -1.0f, -1.0f, wx, wz, col);
}

/* ONE LINE RECORD, the instance LVS draws: the two ends' line-grid pixels
   AFTER the wheel zoom (tagpu_line.h's rule, with this frame's zoom and ss
   from the gather) and after DrawLine's clip to the viewport
   (tagpu_line_clip, the rect the gather latched), carried as their centres,
   then the fog cell, the colour and the depth. The ends go in the (x, y) and
   (u, v) slots of the vertex layout, so a line record and a triangle vertex
   share one stride and one buffer. 0 for a line the clip leaves nothing of,
   and for one past TAGPU_LINE_FAR, which is counted in `s_lfar`.

   `nudge` is {x0, y0, x1, y1} in whole GAME pixels of the zoomed frame, NULL
   for none: each end is moved by nudge * ss line-grid pixels AFTER the zoom
   has quantised it (tagpu_line.h, "the nudge"). The bound is tested after
   the move, so a nudged end is held to TAGPU_LINE_FAR like any other. */
static int put_line(float* o, double x0, double y0, double x1, double y1,
                    const int nudge[4], float wx, float wz, float col, float depth)
{
    int ax, ay, bx, by;
    if (!tagpu_line_px(x0, y0, s_zoom, s_zcx, s_zcy, s_ss, TAGPU_LINE_FAR, &ax, &ay) ||
        !tagpu_line_px(x1, y1, s_zoom, s_zcx, s_zcy, s_ss, TAGPU_LINE_FAR, &bx, &by) ||
        (nudge && !tagpu_line_nudge(&ax, &ay, &bx, &by, nudge, s_ss))) {
        s_lfar++;
        return 0;
    }
    tagpu_line_ab_add(&s_lab, ax, ay, bx, by, (int)(col * 255.0f + 0.5f));
    if (!tagpu_line_clip(&ax, &ay, &bx, &by, s_clipL, s_clipT, s_clipR, s_clipB,
                         s_clipW, s_clipH))
        return 0;
    o[0] = tagpu_line_centre(ax, s_ss); o[1] = tagpu_line_centre(ay, s_ss);
    o[2] = tagpu_line_centre(bx, s_ss); o[3] = tagpu_line_centre(by, s_ss);
    o[4] = wx; o[5] = wz; o[6] = col; o[7] = depth;
    return 1;
}

/* 0 only when the bucket is full, which the caller counts; a line the bound
   refuses is counted here and is not the caller's overflow */
int tagpu_mark_emit_line(float x0, float y0, float x1, float y1,
                         const int nudge[4], int colidx, float wx, float wz)
{
    if (s_nordl >= MAXORDL) return 0;
    if (put_line(s_ordl + (size_t)s_nordl * MVST, x0, y0, x1, y1, nudge, wx, wz,
                 (float)colidx / 255.0f, 0.0f))
        s_nordl++;
    return 1;
}

/* THE SELECTION RECT, emitted by the native pass's unit loop, which is the one
   place that knows both the unit's depth key and the engine's projection of its
   bounds -- the same division of labour as the order markers, whose geometry
   tagpu_order.c computes and this pass draws.

   OURS UNDER EXACTLY THE CONDITIONS markown's `mark_selbox` is told so
   (`tagpu_markown_set_selbox(s_selbox)` in tagpu_mark_render): armed, not
   passive, not `noselbox`. The engine's own box still lands in its reference
   surface -- `mark_selbox` suppresses it only when tagpu_native says every box
   was emitted, and nothing on this lane says so (see `s_selComplete` there) --
   but that surface reaches no screen, so the two cannot double-draw.

   `s_armed` is this module's own 30-frame poll and the caller runs between
   this frame's gather and its render, which is where the bucket is reset and
   published; `tagpu_mark_frame` resets it too, so a frame whose gather did not
   run cannot publish another frame's rects.

   The corners are engine pixel centres (tagpu_native.c `selbox_emit`), and
   four line records follow them round. A ZERO-LENGTH EDGE IS ONE PIXEL, NOT
   NOTHING: a root piece with fewer than three vertices bounds to the bare
   origin, so all four corners coincide, and the engine's DrawLine takes its
   dx == 0 column fill and plots the one pixel (`0x4CC83B`, `inc ecx`). Both
   ends then land on one line-grid pixel, `taOnLine` keeps it and its
   thickening, and `taBand` draws a square round it. */
int tagpu_mark_emit_selbox(const float px[4], const float py[4], int colidx,
                           float wx, float wz, float depth)
{
    float c = (float)colidx / 255.0f;
    int k;
    if (s_armed != 1 || s_passive || !s_selbox) return 0;
    if ((unsigned)s_nsel + 4u > s_selCap) { s_selover++; return 0; }
    for (k = 0; k < 4; k++) {
        int k2 = (k + 1) & 3;
        if (put_line(s_sel + (size_t)s_nsel * MVST, px[k], py[k], px[k2], py[k2],
                     NULL, wx, wz, c, depth))
            s_nsel++;
    }
    return 1;
}

/* SNAP A POINT ONTO THE DEVICE PIXEL GRID, THROUGH THE ZOOM — and do it by
   pre-image rather than by rounding the vertex.

   The vertex shader applies `(p - zoomC) * zoom + zoomC` and then a viewport
   `ss` device pixels to the game-frame unit, so the snap is: take the post-zoom
   position, round it onto the 1/ss grid, and hand back the point that
   transforms to it. Snapping the point we hand the shader instead quantises
   the marker in the
   frame's PRE-zoom units, so its step on screen is `zoom * ss` device pixels
   rather than one, and a marker anchored to a smoothly moving body then jerks
   against it by that much every time the anchor crosses a boundary. That is a
   whole 4 px at 4x with the body gliding underneath it.

   `tagpu_native.c`'s selection rect does the same thing for the same reason
   (its corners, "forward through the zoom, floor, and back"); this is the
   marker pass's copy of that rule. */
static void snap_device(float* x, float* y)
{
    double gx = ((double)*x - s_zcx) * s_zoom + s_zcx;
    double gy = ((double)*y - s_zcy) * s_zoom + s_zcy;
    gx = floor(gx * s_ss + 0.5) / s_ss;
    gy = floor(gy * s_ss + 0.5) / s_ss;
    *x = (float)((gx - s_zcx) / s_zoom + s_zcx);
    *y = (float)((gy - s_zcy) / s_zoom + s_zcy);
}

/* One string, in the palette index the caller names, anchored at the (x, y) the
   engine would have passed `DrawTextCustomFont`.

   CONSTANT SCREEN SIZE, like the order art and unlike the health bars:
   the glyphs are a bitmap font, so scaling the quad with the zoom would be the
   magnified 1997 art this port exists to stop — a label two pixels tall at
   0.25x and a blocky one at 4x. `s_px` is one screen pixel in game-frame units,
   and the vertex shader's zoom multiplies it back out.

   The vertical anchor is the engine's: `0x4CCF60` puts the string's first pixel
   row at `y - (s8)font+0x02`, not at y. */
int tagpu_mark_emit_text(float x, float y, const char* s, int colidx,
                         float wx, float wz)
{
    int ax, ay, w, h, yoff, aw = 1, ah = 1;
    float u0, v0, u1, v1, x1, y0, y1, c;
    int i = s_nordx;

    if (s_nordx + QUADV > MAXORDX) return 0;
    if (!tagpu_text_place(s, &ax, &ay, &w, &h, &yoff)) return 0;
    tagpu_text_dims(&aw, &ah);
    u0 = (float)ax / (float)aw;          v0 = (float)ay / (float)ah;
    u1 = (float)(ax + w) / (float)aw;    v1 = (float)(ay + h) / (float)ah;
    y0 = y - (float)((double)yoff * s_px);

    /* SNAP TO THE DEVICE PIXEL GRID, and do it by pre-image rather than by
       rounding the vertex. Text is the one thing in this pass that is a bitmap:
       every other primitive here is geometry and wants its fraction, but a
       glyph whose quad starts half a pixel off has each 1-px stroke resolved
       across two device pixels and reads as a grey smear. (Measured against the
       engine's own label at 1x, ss=2: 66 pure-white pixels in "build distance"
       against its 183, at the same position and size.)

       `snap_device` is that snap (it is shared with the health bars). The
       quad's SIZE needs no such care — the text is constant screen size, so it
       is `w` game-frame units after the zoom whatever the zoom is, and
       `w * ss` device pixels is an integer. */
    snap_device(&x, &y0);
    x1 = x + (float)((double)w * s_px);
    y1 = y0 + (float)((double)h * s_px);
    c  = (float)colidx / 255.0f;
    put_at(s_ordx, i + 0, x,  y0, u0, v0, wx, wz, c);
    put_at(s_ordx, i + 1, x1, y0, u1, v0, wx, wz, c);
    put_at(s_ordx, i + 2, x,  y1, u0, v1, wx, wz, c);
    put_at(s_ordx, i + 3, x1, y0, u1, v0, wx, wz, c);
    put_at(s_ordx, i + 4, x1, y1, u1, v1, wx, wz, c);
    put_at(s_ordx, i + 5, x,  y1, u0, v1, wx, wz, c);
    s_nordx += QUADV;
    s_ntext++;
    return 1;
}

/* One frame of the order art, its hotspot at (x, y). CONSTANT SCREEN SIZE, as
   the text is: the frame is `w` game pixels after the zoom whatever the zoom,
   so one texel is one screen pixel and at 1x the frame is the engine's blit.
   Its corner goes on the device grid by pre-image (`snap_device`), for the
   text's reason: a bitmap half a pixel off resolves every texel across two
   device pixels. The colour slot carries the opacity (FS's art path). */
int tagpu_mark_emit_art(float x, float y, const struct TAGPU_ORDART_FRAME* f,
                        float alpha, float wx, float wz)
{
    float x0, y0, x1, y1, u0, v0, u1, v1;
    int i = s_norda;
    if (s_norda + QUADV > MAXORDA) return 0;
    x0 = x - (float)((double)f->hx * s_px);
    y0 = y - (float)((double)f->hy * s_px);
    snap_device(&x0, &y0);
    x1 = x0 + (float)((double)f->w * s_px);
    y1 = y0 + (float)((double)f->h * s_px);
    u0 = (float)f->x / (float)TAGPU_ORDART_W;          v0 = (float)f->y / (float)TAGPU_ORDART_H;
    u1 = (float)(f->x + f->w) / (float)TAGPU_ORDART_W; v1 = (float)(f->y + f->h) / (float)TAGPU_ORDART_H;
    put_at(s_orda, i + 0, x0, y0, u0, v0, wx, wz, alpha);
    put_at(s_orda, i + 1, x1, y0, u1, v0, wx, wz, alpha);
    put_at(s_orda, i + 2, x0, y1, u0, v1, wx, wz, alpha);
    put_at(s_orda, i + 3, x1, y0, u1, v0, wx, wz, alpha);
    put_at(s_orda, i + 4, x1, y1, u1, v1, wx, wz, alpha);
    put_at(s_orda, i + 5, x0, y1, u0, v1, wx, wz, alpha);
    s_norda += QUADV;
    return 1;
}

/* one DrawBar rect, edges INCLUSIVE — so the quad's far edge is +1.

   The float form is for the rects anchored to a UNIT, whose anchor is snapped
   onto the device grid by `snap_device` and is therefore not an integer in
   game-frame units at any zoom but 1x. `put_bar` keeps the integer signature
   for the rects that are the engine's own — the build-cursor footprint and the
   drag band — whose positions come from engine integers and stay bit-exact. */
static void put_barf(float* base, int* nv, float l, float t, float r, float b,
                     int colidx, float wx, float wz)
{
    float x0 = l, y0 = t, x1 = r + 1.0f, y1 = b + 1.0f;
    float c = (float)colidx / 255.0f;
    int i = *nv;
    put_at(base, i + 0, x0, y0, -1.0f, -1.0f, wx, wz, c);
    put_at(base, i + 1, x1, y0, -1.0f, -1.0f, wx, wz, c);
    put_at(base, i + 2, x0, y1, -1.0f, -1.0f, wx, wz, c);
    put_at(base, i + 3, x1, y0, -1.0f, -1.0f, wx, wz, c);
    put_at(base, i + 4, x1, y1, -1.0f, -1.0f, wx, wz, c);
    put_at(base, i + 5, x0, y1, -1.0f, -1.0f, wx, wz, c);
    *nv = i + QUADV;
}

static void put_bar(float* base, int* nv, int l, int t, int r, int b, int colidx,
                    float wx, float wz)
{
    put_barf(base, nv, (float)l, (float)t, (float)r, (float)b, colidx, wx, wz);
}

/* one DrawTranspRectangle: the four 1-px edges of the rect in one flat colour,
   every corner inclusive. Normalised because the inner rect is the outer inset
   by 1 and a footprint under 3 px across inverts it — which the engine's own
   line drawer also resolves by swapping the endpoints. */
static void put_outline(float* base, int* nv, int l, int t, int r, int b, int col,
                        float wx, float wz)
{
    int k;
    if (r < l) { k = l; l = r; r = k; }
    if (b < t) { k = t; t = b; b = k; }
    put_bar(base, nv, l, t, r, t, col, wx, wz);          /* top    */
    put_bar(base, nv, l, b, r, b, col, wx, wz);          /* bottom */
    put_bar(base, nv, l, t, l, b, col, wx, wz);          /* left   */
    put_bar(base, nv, r, t, r, b, col, wx, wz);          /* right  */
}

/* Does the cursor layer belong to US this frame? This is the ONE place that
   question is answered: gather_cursor asks it, and so does every client that
   draws a twin of something in that layer -- `tagpu_native.c`'s build ghost
   above all.

   `tagpu_markown_installed()` is NOT part of the answer, for the reason the
   bar gate below has no such test: the engine's own pair goes to the reference
   surface, not the screen, so it is not a second, differently placed one.
   `passive` is, because that lever's whole job is to hand the draw back. */
int tagpu_mark_cursor_ours(void)
{
    return s_armed == 1 && s_cursor && !s_passive;
}

/* The square's gate, the head of `0x469DB4..0x469F23`: the band box bit, or
   cursor mode 14 with the dispatched mouse inside the rect the engine can NAME
   (the packet's vp_addr, the field 0x37E27 as the game thread left it after its
   own widening — inclusive L, T, R, B, exactly what IsPositionInRect 0x4B6720
   tests against). Not the true viewport: while zoomed out that rect is
   deliberately wider, and it is exactly that width which lets a placement in
   the outer ring pass the gate at all. */
static int square_gate(const TAGPU_PACKET* pk)
{
    const int* vp = pk->vp_addr;
    int mx = pk->mouse[0], my = pk->mouse[1];
    if (pk->region_flags & 8) return 1;
    if (pk->cursor_mode != CUR_BUILD) return 0;
    return mx >= vp[0] && mx <= vp[2] && my >= vp[1] && my <= vp[3];
}

/* One square, the rest of `0x469DB4..0x469F23`: the rect `rect` (two corners as {x,
   altitude, z}, the engine's own order at 0x2C92..0x2CA6) projected as the
   engine projects it, then the outer outline and the inner one inset by 1.
   0 for a rect that could not be one, which is dropped rather than turned into
   a quad. */
static int square_emit(const TAGPU_FXVIEW* v, float* base, int* nv, const int32_t* rect,
                       int outer, int inner)
{
    int l, t, r, b;
    float wx, wz;
    l = rect[0] - v->eyeX + 0x80;
    r = rect[3] - v->eyeX + 0x80;
    t = rect[2] - (rect[1] >> 1) - v->eyeY + 0x20;
    b = rect[5] - (rect[4] >> 1) - v->eyeY + 0x20;
    if (l < -0x100000 || l > 0x100000 || r < -0x100000 || r > 0x100000 ||
        t < -0x100000 || t > 0x100000 || b < -0x100000 || b > 0x100000) return 0;

    /* NORMALISE FIRST, THEN INSET — the engine's order, and the two are not
       interchangeable. `0x469E92`/`0x469EA0` swap the pairs and only then does
       `0x469ECA..0x469EDD` inc/dec the SWAPPED values for the inner rect. Insetting
       first and letting put_outline swap afterwards turns the inset into an
       OUTSET whenever the stored rect runs right-to-left or bottom-to-top —
       the black inner outline lands one pixel outside the white one. A band box
       dragged up-left is exactly that case. */
    if (r < l) { int k = l; l = r; r = k; }
    if (b < t) { int k = t; t = b; b = k; }

    /* the layer is drawn with the fog off, so this is only the world point the
       rect's own corner maps to — the same projection layer_quad uses */
    wx = (float)(l - v->vpL + v->eyeX);
    wz = (float)(t - v->vpT + v->eyeY);
    put_outline(base, nv, l, t, r, b, outer, wx, wz);
    put_outline(base, nv, l + 1, t + 1, r - 1, b - 1, inner, wx, wz);
    return 1;
}

/* the build-cursor footprint and the drag band box — the whole of
   `0x469DB4..0x469F23`, transcribed in the header comment */
static void gather_cursor(const TAGPU_FXVIEW* v)
{
    /* EVERY WORD HERE IS THE PACKET'S: the
       GUI colour bytes, the two mode bytes, the dispatched mouse point and the
       build cursor's two world corners, all copied by the game thread inside
       the draw that is about to use them.

       THE GATE IS tagpu_mark_cursor_ours(), NOT AN OPEN-CODED COPY OF IT, so
       that a client drawing a twin of the build square cannot arm on a
       different answer than the square itself. The build ghost is that client
       (tagpu_native.c, ghost_pass): on a different answer -- the marker pass
       off, or `mark.on=nocursor` -- the translucent building would track the
       pointer while our square did not, and the engine's own square is drawn
       at its UNZOOMED 1x projection, so at any zoom != 1 the two would be in
       different places. */
    const TAGPU_PACKET* pk = v->packet;
    const unsigned char* gui;
    int mode, outer, inner, nv = CURSBASE;

    s_ncurs = 0;
    if (!tagpu_mark_cursor_ours()) return;
    if (!pk || !square_gate(pk)) return;
    gui = pk->gui_col;
    mode = pk->cursor_mode;

    /* the colour pair keys off the MODE, not off which arm of the gate let it
       through — the engine picks it from main+0x2CC3 both times */
    if (mode == CUR_BUILD) {
        /* a multi-building placement's sites replace the square (gather_place) */
        if (tagpu_pk_place_replaces(pk)) return;
        outer = gui[(pk->region_flags & 0x40) ? GUI_GREEN : GUI_BLOCKED];
        inner = outer;
    } else {
        outer = gui[GUI_WHITE];
        inner = gui[GUI_BLACK];
    }
    if (square_emit(v, s_verts, &nv, pk->build_rect, outer, inner))
        s_ncurs = nv - CURSBASE;
}

/* A MULTI-BUILDING PLACEMENT'S SITES, each drawn exactly as the build square
   would be drawn there: the square's own gate in mode 14, its projection, its
   drop, its normalise-then-inset, and both outlines in the GUI colour the site
   carries (green, red or yellow — tagpu_place.h), as the engine's mode-14 pair
   takes one colour for both. The packet's acquire bounds every site's colour
   to those three indices, its coordinates to +-TAGPU_PK_PLACE_COORD and the
   count to the bucket's TAGPU_PK_MAX_PLACE. */
static void gather_place(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    const TAGPU_PK_PLACE* pl;
    unsigned i;
    int nv = 0;

    s_nplace = 0; s_cPlace = 0; s_cPlaceRed = 0; s_cPlaceTake = 0;
    if (!tagpu_mark_cursor_ours()) return;
    if (!pk || pk->cursor_mode != CUR_BUILD || !square_gate(pk)) return;
    pl = tagpu_pk_place(pk);
    if (!pl) return;
    for (i = 0; i < pk->n_place && nv + MAXCURSV <= MAXPLACEV; i++) {
        int c = pk->gui_col[pl[i].colour];
        if (!square_emit(v, s_place, &nv, pl[i].rect, c, c)) continue;
        s_cPlace++;
        s_cPlaceRed += pl[i].colour == TAGPU_PK_PLACE_BLOCKED;
        s_cPlaceTake += pl[i].colour == TAGPU_PK_PLACE_TOGGLE;
    }
    s_nplace = nv;
}

/* ---- THE REPLAY'S OVERLAYS ----------------------------------------------
   A playback's recorded cursors and clicks, out of the packet's PK_OVERLAY table
   (tagpu_replay.c, THE OVERLAYS, builds it on the game thread from the
   recording; the selections are the native pass's rectangles, from
   PK_UNIT.rsel). Drawn last and WITH THE FOG OFF, as the build cursor is: a
   recorded player's pointer is not a thing the viewer's sight hides.

   THE PROJECTION is the engine's, selbox_emit's: x - eyeX + vpL and
   z - floor(h / 2) - eyeY + vpT, in the frame's pre-zoom units. Each shape's
   offsets are multiplied by `s_px`, one screen pixel in those units, so it
   keeps its screen size at every zoom, and its anchor goes on the device grid
   by `snap_device`. A cursor is a filled arrow, 18 screen px tall, with a
   one-pixel outline in the GUI's black; a click a 16-segment ring in its
   player's colour whose radius shrinks from 16 to 4 screen px over the
   REC_CLICK_SHOW ticks it lasts.

   BOUND: the coordinates are data. An item whose anchor lies more than
   OV_REACH screen px outside the zoom's effective rect is dropped before any
   vertex is made, so no triangle carries a far coordinate, and every line
   goes through put_line's TAGPU_LINE_FAR bound and the viewport clip. The
   buckets hold the acquire's TAGPU_PK_MAX_OVERLAY items of either kind. */
#define OV_ARROW_N   7
static const float OV_ARROW[OV_ARROW_N][2] = {
    { 0, 0 }, { 0, 15 }, { 4, 11 }, { 7, 17 }, { 9, 16 }, { 6, 10 }, { 11, 10 } };
static const unsigned char OV_ARROW_TRI[5][3] = {
    { 0, 1, 2 }, { 0, 2, 5 }, { 0, 5, 6 }, { 2, 3, 4 }, { 2, 4, 5 } };
#define OV_SEG       16
#define OV_REACH     32
#define MAXOVT       ((int)TAGPU_PK_MAX_OVERLAY * 5 * 3)      /* arrow triangle verts     */
#define MAXOVL       ((int)TAGPU_PK_MAX_OVERLAY * OV_SEG)     /* rings or arrow outlines  */
static float s_ovt[MAXOVT * MVST];
static float s_ovl[MAXOVL * MVST];
static int   s_novt, s_novl, s_novlRing;   /* the rings' lines come first in s_ovl */

static void ov_line(double x0, double y0, double x1, double y1, int col, float wx, float wz)
{
    if (s_novl < MAXOVL &&
        put_line(s_ovl + (size_t)s_novl * MVST, x0, y0, x1, y1, NULL, wx, wz,
                 (float)col / 255.0f, 0.0f))
        s_novl++;
}

/* An item's anchor in the frame's pre-zoom units, on the device grid, and the world point the
   vertex carries; 0 for one past OV_REACH, which makes no vertex. */
static int ov_anchor(const TAGPU_FXVIEW* v, const TAGPU_PK_OVERLAY* o, float* x, float* y,
                     float* wz)
{
    const double reach = OV_REACH * s_px;
    const double z = (double)o->z - floor((double)o->h * 0.5);
    const double px = (double)o->x - v->eyeX + v->vpL, py = z - v->eyeY + v->vpT;
    if (px < v->evpL - reach || px > v->evpL + v->evw + reach ||
        py < v->evpT - reach || py > v->evpT + v->evh + reach)
        return 0;
    *x = (float)px;
    *y = (float)py;
    *wz = (float)z;
    snap_device(x, y);
    return 1;
}

static void gather_overlay(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    const TAGPU_PK_OVERLAY* ov = tagpu_pk_overlay(pk);
    unsigned i;
    int k, j;
    float x, y, wz;
    s_novt = s_novl = s_novlRing = 0;
    if (!ov) return;
    /* the rings first, so a cursor is drawn over a click it has just made */
    for (i = 0; i < pk->n_overlay; i++) {
        const TAGPU_PK_OVERLAY* o = &ov[i];
        unsigned age = o->age < REC_CLICK_SHOW ? o->age : REC_CLICK_SHOW - 1;
        double r = (4.0 + 12.0 * (double)(REC_CLICK_SHOW - age) / REC_CLICK_SHOW) * s_px;
        if (o->kind != TAGPU_PK_OV_CLICK || !ov_anchor(v, o, &x, &y, &wz)) continue;
        for (k = 0; k < OV_SEG; k++) {
            double a0 = 6.283185307179586 * k / OV_SEG, a1 = 6.283185307179586 * (k + 1) / OV_SEG;
            ov_line(x + r * cos(a0), y + r * sin(a0), x + r * cos(a1), y + r * sin(a1), o->colour,
                    (float)o->x, wz);
        }
    }
    s_novlRing = s_novl;
    for (i = 0; i < pk->n_overlay; i++) {
        const TAGPU_PK_OVERLAY* o = &ov[i];
        if (o->kind != TAGPU_PK_OV_CURSOR || !ov_anchor(v, o, &x, &y, &wz)) continue;
        for (k = 0; k < 5 && s_novt + 3 <= MAXOVT; k++)
            for (j = 0; j < 3; j++) {
                const float* c = OV_ARROW[OV_ARROW_TRI[k][j]];
                put_ord(s_ovt, s_novt++, x + (float)(c[0] * s_px), y + (float)(c[1] * s_px),
                        (float)o->x, wz, (float)o->colour / 255.0f);
            }
        for (k = 0; k < OV_ARROW_N; k++) {
            const float* a = OV_ARROW[k];
            const float* b = OV_ARROW[(k + 1) % OV_ARROW_N];
            ov_line(x + a[0] * s_px, y + a[1] * s_px, x + b[0] * s_px, y + b[1] * s_px,
                    pk->gui_col[GUI_BLACK], (float)o->x, wz);
        }
    }
}

/* Grow one of the two unit-scaled buckets to `need` vertices. Grow only,
   doubling; 0 leaves the bucket and its capacity as they were. */
static int grow_verts(float** p, unsigned* cap, unsigned need)
{
    float* q;
    unsigned want;
    if (tagpu_grow_stress()) {
        /* the lever: a fresh block every frame, exactly `need` long */
        free(*p); *p = NULL; *cap = 0;
        if (!need) return 1;
        *p = (float*)malloc((size_t)need * MVST * sizeof(float));
        if (!*p) return 0;
        *cap = need;
        return 1;
    }
    if (need <= *cap) return 1;
    want = *cap ? *cap : 1024;
    while (want < need) {
        if (want > 0x7FFFFFFFu / 2u / MVST / sizeof(float)) return 0;
        want *= 2;
    }
    q = (float*)realloc(*p, (size_t)want * MVST * sizeof(float));
    if (!q) return 0;
    *p = q; *cap = want;
    return 1;
}

int tagpu_mark_gather(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    const TAGPU_PK_UNIT* uu;
    const unsigned char* gui;
    unsigned ui;
    int watched, nv = 0;

    s_nbar = 0; s_cBar = 0; s_norda = 0; s_nordl = 0; s_nordx = 0;
    s_nsel = 0; s_selover = 0; s_lfar = 0;
    s_nordxOrd = 0; s_ntext = 0; s_xover = 0;
    s_novt = 0; s_novl = 0; s_novlRing = 0;
    s_nplace = 0; s_cPlace = 0; s_cPlaceRed = 0; s_cPlaceTake = 0;
    if (!pk || !pk->in_game) return 0;
    /* BEFORE ANYTHING EMITS: a bar per unit at most, a selection rect per unit
       at most, and the unit count is bounded by the packet's own table. A
       bucket that would not grow keeps its old size, and the caps below then
       count what they refuse. */
    if (!grow_verts(&s_barv, &s_barvCap, pk->n_units * 2u * QUADV) ||
        !grow_verts(&s_sel, &s_selCap, pk->n_units * 4u)) {
        if (!s_saidGrow) {
            s_saidGrow = 1;
            flog("mark: a unit-scaled marker bucket would not grow to the packet's "
                 "unit count - markers past its old size are refused and counted");
        }
    }
    /* before anything emits: tagpu_order.c's labels come through
       tagpu_mark_emit_text, which sizes its quads with this */
    /* one font for the whole frame, before anything asks the atlas for a string:
       the packet's glyph copy, keyed on its generation (tagpu_text.h) */
    tagpu_text_frame(v->packet);
    s_px  = 1.0 / (double)(v->zoom > 0.0f ? v->zoom : 1.0f);
    s_zoom = v->zoom > 0.0f ? (double)v->zoom : 1.0;
    s_zcx = (double)v->zoomCx; s_zcy = (double)v->zoomCy;
    s_ss  = v->ss > 0 ? v->ss : 1;
    /* the viewport and the surface on the line grid (tagpu_line.h) */
    tagpu_line_rect(v->vpL, v->vpT, v->vw, v->vh, s_ss,
                    &s_clipL, &s_clipT, &s_clipR, &s_clipB);
    s_clipW = v->gw * s_ss; s_clipH = v->gh * s_ss;
    /* the lines of a frame the A/B may claim, kept before the clip for the
       oracle; the claim in tagpu_mark_render writes them */
    tagpu_line_ab_begin(&s_lab, s_ab && !s_abDone);
    /* first, and outside every gate below: neither the cursor nor the order
       markers are health bars, and neither `damagebars` nor `nobars` has
       anything to say about them */
    gather_cursor(v);
    gather_place(v);
    if (s_armed == 1 && !s_passive) gather_overlay(v);
    if (s_armed == 1 && !s_passive) tagpu_order_gather(v);
    else if (s_passive) tagpu_markown_set_orders(0);
    tagpu_order_frame_done(v);
    /* everything emitted so far is a ShowRanges label, and the engine draws
       those UNDER the health bars */
    s_nordxOrd = s_nordx;
    /* One walk for both: the engine draws the bar and the group digit from the
       SAME loop (`0x469CB9` then `0x469CF9`), behind the same `damagebars`
       gate, so `nobars` may not take the digits with it. */
    if (s_armed != 1 || (!s_bars && !s_digits)) return 0;
    /* NO DOUBLE-DRAW REFUSAL, for the reason the terrain pass's emit gate has
       none (gpu-status 2.81): nothing of the engine's reaches the screen --
       its bars land in the reference surface and nowhere else -- so there is
       no pair to make. `markown` is still WANTED here, and it is on the
       defaults table, but as the PRODUCER of the snapshots this pass and
       `tagpu_order.c` draw from, not as the thing that earns us the right to
       draw. */
    if (!(pk->game_opt & 1)) return 0;                  /* damagebars */

    watched = pk->local_player;      /* 0x2A43: the bar loop's, see the header */
    gui = pk->gui_col;
    uu = tagpu_pk_units(pk);

    for (ui = 0; ui < pk->n_units && (unsigned)s_cBar < s_barvCap / (2u * QUADV); ui++) {
        const TAGPU_PK_UNIT* u = &uu[ui];
        int hp, maxhp, third, w, col;
        float x, y;                     /* the anchor, in game-frame units     */
        float fpx, fpa, fpd;            /* world x, ALTITUDE, map depth        */
        float wx, wz;
        if (u->owner != (unsigned)watched) continue;

        /* THE BAR SITS ON WHOEVER DREW THE BODY. That is the invariant, and it needs
           two branches because two different things draw units.

           OURS. `tagpu_native_owns_unit` is the very predicate the unit pass gathers
           on (`tagpu_native.c`, "units are gathered only by the unit pass"), so when
           it holds the body was placed from `tagpu_native_unit_pos` — the
           interpolated sub-pixel sample when there is one, that unit's own raw
           fractional 16.16 when there is not (`tagpu_subpix.off`, or a slot the walk
           skipped). Either way the bar takes the same number as the model under it.

           THE ENGINE'S. When it does not hold, the unit pass skipped this unit and
           the ENGINE drew the body, from `(s16)` reads of the same 16.16 — a floor.
           The bar has to floor with it, or it sits up to a whole game pixel off a
           body we did not place. `markown` has suppressed the engine's own bars
           globally by then, so this is not a rare path: a unit the type filter
           rejects still needs a bar from us. (A nanoframe is not one: the native
           pass owns every nanoframe.)

           THE BRANCH IS ON THE OWNERSHIP BIT, NOT ON THE ACCESSOR'S RESULT. The
           accessor returns 1 whenever its POINTER checks pass and hands back the
           raw fraction when the table holds no sample — it never reports "no
           sample" — so a branch on it alone would never reach the integer arm,
           and every engine-drawn unit would get a bar up to a pixel off its body.

           NEITHER CALL READS ENGINE MEMORY. The ownership answer is the PUBLISHER's — one bit in the
           packet, taken on the game thread with the def in hand — so the unit
           pass, this loop and the composite wipe act on one answer instead of
           three reads of the same bytes. `tagpu_mark_gather` is still called
           from inside the unit pass's own gather, on the same thread, in the
           same frame, AFTER the walk that fills the sub-pixel table, so the
           sample it finds is this frame's; the accessor refuses any sample that
           does not still describe this unit's current 16.16 position, so a
           recycled slot falls through to its own fraction.

           AND THE ANCHOR KEEPS ITS FRACTION TO THE LAST MOMENT. Flooring it
           quantises the bar in the frame's PRE-zoom units, so the bar steps `zoom`
           game pixels at a time while the body glides continuously underneath it:
           4 px at 4x, 8 px at ZOOM_MAX, a diagonal twitch at max zoom-in.
           `snap_device` puts the anchor on the
           DEVICE grid instead, the same rule the selection rect and the glyph atlas
           already follow, so the step is 1/ss of a displayed pixel at every zoom.
           It runs on BOTH branches: on the engine's integers it is the identity at
           1x, so engine parity there is exact, and away from 1x it only makes an
           already sim-rate anchor land crisply. */
        if ((u->flags & TAGPU_PK_U_NATIVE) &&
            tagpu_native_unit_pos(u, &fpx, &fpa, &fpd)) {
            x  = fpx - (float)v->eyeX + 128.0f;
            y  = fpd - (float)v->eyeY - fpa * 0.5f + 32.0f + 10.0f;
            wx = fpx;
            wz = fpd - fpa * 0.5f;
        } else {
            /* the engine reads these as the HIGH WORD of the 16.16 triple —
               a floor — and the bar has to floor with it */
            int px = (int)(short)(u->pos[0] >> 16);   /* world x   */
            int pa = (int)(short)(u->pos[1] >> 16);   /* altitude  */
            int pd = (int)(short)(u->pos[2] >> 16);   /* map depth */
            x  = (float)(px - v->eyeX + 0x80);
            y  = (float)(pd - v->eyeY - (pa >> 1) + 0x20 + 0x0A);
            wx = (float)px;
            wz = (float)(pd - (pa >> 1));
        }
        /* cull to the ZOOM's rect, not the engine's: at zoom < 1 the frame
           shows units the engine's own HotUnits list has already dropped */
        if (x + 0x12 < v->evpL || x - 0x12 > v->evpL + v->evw ||
            y + 3 < v->evpT || y - 3 > v->evpT + v->evh) continue;

        /* one snap for the bar and the group digit both, so the two cannot part
           company. (The digit's own snap inside `tagpu_mark_emit_text` is over the
           glyph's baseline, four rows below this, and is idempotent on a point
           already on the grid.) The fog is sampled at the UNSNAPPED anchor, in the
           projected world space the engine's screen grid is built in (tagpu_fx.h) —
           it is a world lookup, and the snap is a screen-space concern. */
        snap_device(&x, &y);

        /* THE GROUP DIGIT, `0x469CD1..0x469CF9`. Two things about it are the
           engine's and neither is obvious: the squad tag is tested as a DWORD
           (`mov ecx,[edi+0xac]; test ecx,ecx`) and only then used as a byte, so
           a unit whose 0xAD..0xAF are set shows a '0'; and the digit sits at
           `sy + 0x0E` where the BAR is at `sy + 0x0A`, i.e. four rows lower
           than `y` here. No health test — the engine's leaf returns early on a
           dead unit but the digit is drawn from the caller. */
        if (s_digits && !s_passive) {
            unsigned squad = u->squad;
            if (squad) {
                char d[2];
                int tc = tagpu_text_colour();
                d[0] = (char)('0' + (unsigned char)squad);
                d[1] = 0;
                if (!tagpu_mark_emit_text(x, y + 4.0f, d,
                                          (tc >= 0 && tc < 256) ? tc : gui[GUI_WHITE],
                                          wx, wz))
                    s_xover++;
            }
        }
        if (!s_bars) continue;

        hp = u->health;
        if (hp <= 0) continue;
        if (u->type_row == 0xFFFFu) continue;     /* no def resolved for it     */
        maxhp = u->max_health;
        if (maxhp <= 0) continue;                 /* the engine's div would trap */
        s_cBar++;
        if (s_passive) continue;      /* the A/B lever: count, let the engine draw */

        put_barf(s_barv, &nv, x - 17.0f, y - 2.0f, x + 17.0f, y + 2.0f, gui[GUI_BLACK],
                 wx, wz);
        /* (Health << 5) / maxHP as an UNSIGNED divide, and the thirds through
           the same maxHP/3 the engine's reciprocal multiply produces */
        w = (int)(((unsigned)(hp << 5)) / (unsigned)maxhp);
        third = (int)((unsigned)maxhp / 3u);
        col = hp > 2 * third ? gui[GUI_GREEN]
            : hp > third     ? gui[GUI_YELLOW]
            :                  gui[GUI_RED];
        put_barf(s_barv, &nv, x - 16.0f, y - 1.0f, x - 16.0f + (float)w, y + 1.0f, col,
                 wx, wz);
    }
    s_nbar = nv;
    return s_cBar;
}

/* ---- render ---- */

/* ---- THE HAND-OVER ------------------------------------------------------
   Built by `tagpu_mark_render` as it walks the buckets. The vertex block is
   ONE concatenation in one order, so every `first` recorded here indexes that
   block directly rather than being re-derived from the counts and left free to
   disagree with it. */
static float*        s_mkV;    static int s_mkVn, s_mkVcap;
static TAGPU_MKDRAW  s_mkD[TAGPU_MK_MAXDRAW]; static int s_mkDn;
static TAGPU_MKHAND  s_mkPub;
static int           s_mkHave, s_mkDropped;
static unsigned      s_mkFrame;

static int mk_room(int need)
{
    float* q;
    int c = s_mkVcap;
    if (need <= c) return 1;
    c = c ? c * 2 : 4096;
    while (c < need) c *= 2;
    q = (float*)realloc(s_mkV, (size_t)c * sizeof(float));
    if (!q) return 0;
    s_mkV = q; s_mkVcap = c;
    return 1;
}

/* append `n` vertices of MVST floats, returning the FIRST vertex index -- which
   is what the matching `mk_draw` records as its own `first` */
static int mk_push(const float* v, int n)
{
    int at = s_mkVn;
    if (n <= 0) return at;
    if (!mk_room((s_mkVn + n) * MVST)) { s_mkDropped = 1; return at; }
    memcpy(s_mkV + (size_t)s_mkVn * MVST, v, (size_t)n * MVST * sizeof(float));
    s_mkVn += n;
    return at;
}

static void mk_draw(int first, int count, int lines, int text, int fog, int tex,
                    int depth)
{
    if (count <= 0) return;
    if (s_mkDn >= TAGPU_MK_MAXDRAW) { s_mkDropped = 1; return; }
    s_mkD[s_mkDn].first = first; s_mkD[s_mkDn].count = count;
    s_mkD[s_mkDn].lines = lines; s_mkD[s_mkDn].text = text;
    s_mkD[s_mkDn].fog = fog;     s_mkD[s_mkDn].tex = tex;
    s_mkD[s_mkDn].depth = depth;
    s_mkD[s_mkDn].blend = tex == TAGPU_MK_TEX_ART;
    s_mkDn++;
}

void tagpu_mark_frame(unsigned frame_counter)
{
    s_mkFrame = frame_counter;
    s_mkHave = 0;
    s_nsel = 0;           /* emitted after the gather, so reset here as well */
}

int tagpu_mark_handover(TAGPU_MKHAND* out, unsigned now)
{
    if (!s_mkHave || !out || s_mkPub.frame != now) return 0;
    *out = s_mkPub;
    s_mkHave = 0;
    return 1;
}

/* NO ENGINE PIXEL: every marker this pass draws (the bars, the selection and
   band-box rects, the order lines and dots, the range circles, the group
   digit, the labels) is re-derived from engine STATE and drawn as our own
   geometry out of our own atlas. */


void tagpu_mark_render(const TAGPU_FXVIEW* v)
{
    int total, textBase = 0, selBase = 0, ovtBase, ovlBase, placeBase;

    /* THIS PASS BUILDS THE RECORD THE TWIN DRAWS FROM, and nothing else:
       `mk_push` and `mk_draw` are the record and are pure CPU. */

    s_mkVn = 0; s_mkDn = 0; s_mkDropped = 0;
    if (s_armed != 1) return;
    /* The heartbeat says "this pass ran", NOT "this pass drew something": a
       frame with no bars and no markers is the ordinary case, and letting the
       watchdog read that as a dead pass would hand the draw back and take it
       again 30 frames later, forever. */
    tagpu_markown_beat(v->frame_counter);
    if (!s_passive) {
        tagpu_markown_set_bars(s_bars);
        tagpu_markown_set_selbox(s_selbox);
        tagpu_markown_set_cursor(s_cursor);
        tagpu_markown_set_digits(s_digits);
    }

    if (s_nbar == 0 && s_ncurs == 0 && s_nplace == 0 && s_nsel == 0 &&
        s_norda == 0 && s_nordl == 0 && s_nordx == 0 && s_novt == 0 && s_novl == 0) return;

    /* ---- the A/B's lever ---- */
    {
        int taking = s_ab && !s_abDone;
        /* NO `ss` BOUND ON THIS A/B. The Vulkan lane captures its gw*ss by
           gh*ss world target, so this pass is measurable at every `ss`, including
           the configuration it actually ships in. The refusal belongs to the lane
           that can see the target: if there is none that frame, tagpu_vk.c says
           so by name and captures nothing. [tagpu_vk_world.h.] */
        /* THE ARMING, which is the whole of it. */
        s_abTaking = taking;
    }

    total = BARBASE + s_nbar;
    textBase = total + s_norda + s_nordl;

    /* ONE CONCATENATION, PUSHED ONCE, so every `first` below indexes the block
       tagpu_vk_mark.c receives. Pushed here rather than per draw because the
       buckets are contiguous and re-slicing them per draw is how the offsets
       and the geometry drift apart. The record IS the vertex block. */
    mk_push(s_verts, BARBASE);
    if (s_nbar) mk_push(s_barv, s_nbar);
    if (s_norda) mk_push(s_orda, s_norda);
    if (s_nordl) mk_push(s_ordl, s_nordl);
    if (s_nordx) mk_push(s_ordx, s_nordx);
    selBase = textBase + s_nordx;
    if (s_nsel) mk_push(s_sel, s_nsel);
    ovtBase = selBase + s_nsel;
    if (s_novt) mk_push(s_ovt, s_novt);
    ovlBase = ovtBase + s_novt;
    if (s_novl) mk_push(s_ovl, s_novl);
    placeBase = ovlBase + s_novl;
    if (s_nplace) mk_push(s_place, s_nplace);

    /* THE SELECTION RECTS FIRST, and depth-tested: the engine draws each one
       inside the unit sweep, immediately before its own unit (`0x4699EB`,
       `0x469B8A`), so it is under that unit and every later row and over what
       came before -- which is what the unit pass's depth keys already encode,
       so the test reproduces the order the sweep had. Everything after it in
       this list is drawn over the finished world, as the engine draws it after
       the sweeps. Fogged like the bars: the fog overlay comes after the sweep. */
    if (s_nsel) {
        mk_draw(selBase, s_nsel, 1, 0, v->fogMode & 1, TAGPU_MK_TEX_NONE, 1);
    }

    /* The engine's own order inside the block: order markers and their
       ShowRanges labels first (`0x469BFC`), then the health bars over them
       (`0x469CB9`), then the group digit over those (`0x469CF9`), and the build
       cursor last of all — after the fog overlay, which is why it carries no
       fog. */
    if (s_norda || s_nordl) {
        /* the art first, under every order line. The engine interleaves them
           per record in mask-bit order (`0x439B30`: build rect, dots, target
           circle, sprite), so its lines can fall either side of its art; one
           art draw cannot, and art under lines is the order that hides the
           least of a waypoint (ui-markers.md §6.5). The lines are LVS's
           instances, `ss` target pixels wide -- the rule the effects lines and
           the nanoframe wire follow too (tagpu_line.h) */
        if (s_norda) {
            mk_draw(total, s_norda, 0, 0, v->fogMode & 1, TAGPU_MK_TEX_ART, 0);
        }
        if (s_nordl) {
            mk_draw(total + s_norda, s_nordl, 1, 0, v->fogMode & 1, TAGPU_MK_TEX_NONE, 0);
        }
    }
    if (s_nordx) {
        /* tagpu_vk_mark.c gets the atlas bytes through the hand-over
           (`text`/`textGen`/`textW`/`textH`) and uploads its own */
        if (s_nordxOrd) {
            mk_draw(textBase, s_nordxOrd, 0, 1, v->fogMode & 1, TAGPU_MK_TEX_TEXT, 0);
        }
    }
    if (s_nbar) {
        mk_draw(BARBASE, s_nbar, 0, 0, v->fogMode & 1, TAGPU_MK_TEX_NONE, 0);
    }
    if (s_nordx > s_nordxOrd) {
        /* the digits, over the bars, out of the same atlas — a SECOND record
           rather than one widened range because the bars are drawn between the
           two, which is the engine's own order (the labels come out of the
           order driver at 0x469BFC and the digit at 0x469CF9). */
        mk_draw(textBase + s_nordxOrd, s_nordx - s_nordxOrd, 0, 1,
                v->fogMode & 1, TAGPU_MK_TEX_TEXT, 0);
    }
    /* last, and with the fog off: the engine draws these two rects after its
       fog overlay and never darkens them, and a placement's sites are the
       squares that replace its build square */
    if (s_ncurs) {
        mk_draw(CURSBASE, s_ncurs, 0, 0, 0, TAGPU_MK_TEX_NONE, 0);
    }
    if (s_nplace) {
        mk_draw(placeBase, s_nplace, 0, 0, 0, TAGPU_MK_TEX_NONE, 0);
    }
    /* the replay's overlays, over everything and with the fog off: the click rings, then the
       cursor arrows' fills, then their outlines */
    if (s_novlRing) mk_draw(ovlBase, s_novlRing, 1, 0, 0, TAGPU_MK_TEX_NONE, 0);
    if (s_novt) mk_draw(ovtBase, s_novt, 0, 0, 0, TAGPU_MK_TEX_NONE, 0);
    if (s_novl > s_novlRing)
        mk_draw(ovlBase + s_novlRing, s_novl - s_novlRing, 1, 0, 0, TAGPU_MK_TEX_NONE, 0);

    if (s_abTaking) {
        /* THE A/B CLAIM. `tagpu_vk_ab_arm` unlinks the target `_vk.ppm` at the instant the claim latches, which
           is what makes the file on the disk this arming's rather than an
           earlier run's; diff it against a capture from another BUILD. */
        s_abFrame = tagpu_vk_ab_arm("mark");
        s_abDone = 1;
        s_abTaking = 0;
        /* and the frame's lines as put_line had them before the clip, with
           the rect it clipped to (tagpu_line.h) */
        tagpu_line_ab_write(&s_lab, "mark", s_clipW, s_clipH, s_ss,
                            s_clipL, s_clipT, s_clipR, s_clipB);
    }

    /* PUBLISHED ONLY IF THE RECORD IS THE WHOLE DRAW. A list short of what the
       gather above found would have the Vulkan lane draw a marker layer missing
       a bucket, which is a different picture rather than an absent one. */
    if (!s_mkDropped && s_mkDn > 0) {
        memset(&s_mkPub, 0, sizeof s_mkPub);
        s_mkPub.frame = s_mkFrame;
        s_mkPub.verts = s_mkV; s_mkPub.nvert = s_mkVn;
        s_mkPub.draws = s_mkD; s_mkPub.ndraw = s_mkDn;
        s_mkPub.text = tagpu_text_atlas(&s_mkPub.textGen);
        tagpu_text_dims(&s_mkPub.textW, &s_mkPub.textH);
        s_mkPub.art = tagpu_order_art(&s_mkPub.artKey);
        s_mkPub.artW = TAGPU_ORDART_W; s_mkPub.artH = TAGPU_ORDART_H;
        /* the three tagpu_vk_mark.c binds as textures, as bytes -- same shapes
           tagpu_fx.h uses for the same three */
        s_mkPub.pal = tagpu_pal_engine(); s_mkPub.palSerial = tagpu_pal_engine_serial();
        s_mkPub.fogGrid = (v->fogMode & 1) ? v->fogGrid : NULL;
        s_mkPub.fogGridCols = v->fogCols; s_mkPub.fogGridRows = v->fogRows;
        s_mkPub.key = tagpu_markown_key();
        s_mkPub.gw = (float)v->gw; s_mkPub.gh = (float)v->gh;
        s_mkPub.zoom = v->zoom > 0.0f ? v->zoom : 1.0f;
        s_mkPub.zoomCx = v->zoomCx; s_mkPub.zoomCy = v->zoomCy;
        s_mkPub.fogOrgX = (float)v->fogOrgX; s_mkPub.fogOrgY = (float)v->fogOrgY;
        s_mkPub.fogCols = (float)v->fogCols; s_mkPub.fogRows = (float)v->fogRows;
        s_mkPub.ss = v->ss > 0 ? v->ss : 1.0f;
        s_mkPub.vpL = v->vpL; s_mkPub.vpT = v->vpT;
        s_mkPub.vw = v->vw;   s_mkPub.vh = v->vh;
        s_mkPub.scissorOn = tagpu_native_scissor_on();
        s_mkPub.ab = s_abFrame; s_abFrame = 0;
        s_mkHave = 1;
    }

    if (s_log) {
        static unsigned last = 0;
        if (v->frame_counter - last >= 120) {
            char b[320];
            int nstr = 0, ndrop = 0;
            last = v->frame_counter;
            tagpu_text_stats(&nstr, &ndrop);
            _snprintf(b, sizeof b,
                "mark: bars=%d sel=%d selover=%d cursor=%d place=%d placered=%d placetake=%d ordart=%d "
                "ordline=%d far=%d text=%d(lab=%d) "
                "atlas=%d/%d over=%d key=%d vp=(%d,%d %dx%d) "
                "zoom=%.2f%s",
                s_cBar, s_nsel / 4, s_selover, s_ncurs / QUADV, s_cPlace, s_cPlaceRed, s_cPlaceTake, s_norda / QUADV,
                s_nordl, s_lfar,
                s_ntext, s_nordxOrd / QUADV, nstr, ndrop, s_xover,
                tagpu_markown_key(), v->vpL, v->vpT, v->vw, v->vh, v->zoom,
                s_passive ? " (passive: engine still drawing)" : "");
            flog(b);
        }
    }
}
