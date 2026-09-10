/* tagpu_mark.c — the UI marker pass (G13d). See tagpu_mark.h for the split
   between re-drawn health bars and captured everything-else, and
   tagpu_markown.h for how the capture works.

   THE HEALTH BAR, byte for byte (DrawHealthBars 0x46A430, sole caller
   0x469CB9, re-read for this gate):

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
   positions), so ITS bar steps once per sim tick — and so did ours until
   2026-09-09, while the native unit pass interpolated the body between ticks.
   That was wrong, and visibly so: the bar and the body slid against each other
   by up to a whole tick of motion, 2.95 px peak-to-peak at 1x on a walking
   commander and `zoom` times that on screen (5.77 px at 2x), which is the
   health-bar wobble. Stock TA cannot show it, because there the bar and the
   body are the same shorts. The gather now takes `tagpu_native_unit_pos()` —
   the body's own anchor, the same one the selection box and the unit-anchored
   order markers already use.

   THE FIX HAD TO BE MADE TWICE, and the second half is the interesting one.
   The first pass took that anchor and FLOORED it, on the argument that the
   selection box floors the same anchor and the two should agree. They did
   agree — with each other, in the frame's PRE-zoom units, which is the wrong
   grid. The vertex shader scales this pass by `zoom` about the zoom centre, so
   a one-unit quantisation here is `zoom * ss` DEVICE pixels on screen, and the
   bar went on stepping 2-4 px diagonally at max zoom-in while the body glided
   underneath it. (The selection box never showed it because it does not
   actually floor in these units: `tagpu_native.c` snaps its corners forward
   through the zoom, floors THERE, and comes back — a device-pixel step.) The
   anchor now keeps its fraction to the last moment and `snap_device` puts it on
   the device grid, so the step is one device pixel at every zoom, and the two
   markers still agree because they are now quantised on the same grid.
   [This note used to argue for the shorts outright: "a bar is 35 px of flat
   colour over a unit that moves a couple of pixels per frame, and the
   alternative is a second, differently sourced anchor that can disagree with
   the body's." The second half had it backwards — `tagpu_native_unit_pos` IS
   the body's anchor — and the first half was a guess the measurement did not
   support.]

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
#include "opengl_utils.h"
#include "tagpu_opt.h"
#include "tagpu_mark.h"
#include "tagpu_markown.h"
#include "tagpu_native.h"
#include "tagpu_order.h"
#include "tagpu_text.h"
#include "tagpu_glsl.h"

/* ---- engine layout ---- */
#define OFF_BEGIN    0x14357     /* unit array base / end (stride 0x118)      */
#define OFF_END      0x1435B
/* THE OWNER TEST'S PLAYER ID, and it is NOT the one the order driver uses.
   `DrawGameScreen` loads `main+0x2A43` into a local at `0x46967D`/`0x469689` and
   both the health bar (`0x469CA6`) and the group digit (`0x469CC9`) compare the
   unit's `owner->id` against THAT — while the order-marker driver `0x48CC30`
   picks its player range from `main+0x2A42` (`0x48CC3B`). Two different bytes,
   two loops, one block. They are written independently (`0x416B25` and
   `0x416B38`, from two separate reads in the same loader function), so they can
   differ, and G13d had this loop on `0x2A42` from the start — meaning our bars
   were drawn for a different player's units than the engine's whenever the two
   disagree. Which of the pair is "watched" and which "local" is NOT established
   here and the notes disagree with each other about it, so they are named by
   address. [BINARY-VERIFIED 2026-09-05] */
#define OFF_BAROWNER 0x2A43      /* u8, the id the bar/digit loop compares to  */
#define OFF_GAMEOPT  0x37F06     /* bit0 = the registry option "damagebars"   */
#define OFF_GUICOL   0x0DCB      /* GUI colour byte array (GetGuiPaletteColor)*/
#define UNIT_STRIDE  0x118
#define U_XPOS       0x6C        /* s16 world x                               */
#define U_ZPOS       0x70        /* s16 altitude                              */
#define U_YPOS       0x74        /* s16 world z (map depth)                   */
#define U_TYPE       0x92        /* UnitDefStruct*                            */
#define U_SQUAD      0xAC        /* group digit; the engine tests all 4 bytes */
#define U_HEALTH     0x108       /* s16                                       */
#define U_STATE      0x110       /* bit28 alive, bit14 excluded               */
#define U_OWNER      0xFF        /* u8 player id                              */
#define UD_MAXHP     0x1FA       /* read as a DWORD, as the engine's div does */
/* the build cursor / band box block, all DWORDs unless noted (0x469E13) */
#define OFF_CURMODE  0x2CC3      /* u8 order/cursor mode; 0x0E = build placement */
#define OFF_MOUSEFL  0x2CC6      /* u8 region flags; bit3 band box, bit6 site OK */
#define OFF_MOUSE_X  0x2C76      /* the dispatched mouse point vpwide repairs  */
#define OFF_MOUSE_Y  0x2C7A
#define OFF_CUR_X1   0x2C92      /* world x of one corner                      */
#define OFF_CUR_H1   0x2C96      /* its altitude — projected as z - alt/2      */
#define OFF_CUR_Z1   0x2C9A      /* its world z                                */
#define OFF_CUR_X2   0x2C9E      /* and the same three for the other corner    */
#define OFF_CUR_H2   0x2CA2
#define OFF_CUR_Z2   0x2CA6
#define OFF_VPRECT   0x37E27     /* L,T,R,B — WIDENED by vpwide at zoom < 1    */
#define CUR_BUILD    0x0E        /* the cursor mode the footprint belongs to   */

#define GUI_BLACK    0x00
#define GUI_BLOCKED  0x04
#define GUI_GREEN    0x0A
#define GUI_RED      0x0C
#define GUI_YELLOW   0x0E
#define GUI_WHITE    0x0F

#define MAXBAR       2048                    /* bars per frame               */
#define MVST         7                       /* x,y, u,v, wx,wz, colour      */
#define QUADV        6
#define CURSBASE     (TAGPU_MARK_NLAYER * QUADV)  /* the captured layer quad
                                                comes first; the cursor next  */
#define MAXCURSV     (8 * QUADV)             /* two rects, four edges each    */
#define BARBASE      (CURSBASE + MAXCURSV)   /* and the bars after those      */
#define MAXMV        (BARBASE + MAXBAR * 2 * QUADV)
/* The order-marker buckets (tagpu_order.c). Separate arrays rather than more
   regions of s_verts: the bar region's length is decided at gather time, so
   anything at a FIXED offset behind it would have to be uploaded across the
   whole unused bar budget every frame. Uploaded contiguously after the
   triangles instead, with the draw offsets computed at render. */
#define MAXORDT      12000                   /* order triangle verts (dots)  */
#define MAXORDL      12000                   /* order line verts (2 per line)*/
/* Text quads: the ShowRanges labels (up to twelve per SELECTED unit with the
   toggle on) and one group digit per watched unit, in ONE bucket — 800 quads,
   134 KB. The order gather runs first, so a frame that overruns this loses the
   digits rather than the labels; `s_xover` counts it and `mark: … over=N` says
   so. That ordering is deliberate (it is the engine's own draw order) but it is
   the failure mode to know: ShowRanges over a large selection is the only thing
   that can reach the cap, and it costs the digits first. */
#define MAXORDX      4800                    /* text verts (6 per quad)      */

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum,GLint,GLsizei);
typedef void (APIENTRY *PFN_UNIFORM1F)(GLint,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_LINEWIDTH)(GLfloat);
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_UNIFORM1F  x_glUniform1f;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_ACTIVETEX  x_glActiveTexture;
static PFN_LINEWIDTH  x_glLineWidth;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

/* ---- arming ---- */
static int s_armed = -1;
static int s_log = 0, s_passive = 0, s_bars = 1, s_capture = 1, s_selbox = 1;
static int s_cursor = 1, s_digits = 1;
static unsigned s_armCheck = 0;

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
        tagpu_markown_set_capture(0);
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
    {
        s_log = 0; s_passive = 0; s_bars = 1; s_capture = 1; s_selbox = 1;
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
                else if (!lstrcmpiA(p, "nocapture")) s_capture = 0;
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
    if (s_passive || !s_capture) tagpu_markown_set_capture(0);
    if (s_passive || !s_bars) tagpu_markown_set_bars(0);
    if (s_passive || !s_selbox) tagpu_markown_set_selbox(0);
    if (s_passive || !s_cursor) tagpu_markown_set_cursor(0);
    if (s_passive || !s_digits) tagpu_markown_set_digits(0);
    if (was != 1) {
        char b[176];
        _snprintf(b, sizeof b, "mark: ARMED (log=%d passive=%d bars=%d capture=%d "
                  "selbox=%d cursor=%d digits=%d patched=%d)", s_log, s_passive,
                  s_bars, s_capture, s_selbox, s_cursor, s_digits,
                  tagpu_markown_installed());
        flog(b);
    }
    return 1;
}

/* ---- GL ---- */
static int    s_state = 0;             /* 0 unloaded, 1 ready, 2 failed       */
static GLuint s_prog, s_vao, s_vbo, s_tex[TAGPU_MARK_NLAYER];
static int    s_texW[TAGPU_MARK_NLAYER], s_texH[TAGPU_MARK_NLAYER];
static GLint  s_uGame, s_uFog, s_uFogOrg, s_uFogDim, s_uZoom, s_uZoomC, s_uKey;
static GLint  s_uTextM;

static float s_verts[MAXMV * MVST];
static float s_ordt[MAXORDT * MVST];   /* order markers: filled triangles     */
static float s_ordl[MAXORDL * MVST];   /* order markers: GL_LINES             */
static float s_ordx[MAXORDX * MVST];   /* text quads: labels, then digits     */
static int   s_nbar;                   /* bars gathered (2 quads each)        */
static int   s_cBar;                   /* counted, whether emitted or not     */
static int   s_ncurs;                  /* build cursor / band box verts       */
static int   s_nordt, s_nordl;         /* order marker verts, this frame      */
static int   s_nordx;                  /* text verts, this frame              */
/* Where the ShowRanges labels end and the group digits begin. The engine draws
   the labels from inside the order driver at `0x469BFC` and the digit at
   `0x469CF9`, i.e. on either side of the health bars, so the bucket is drawn as
   two ranges rather than one. The order gather runs first, so the split is just
   the count after it. */
static int   s_nordxOrd;
static double s_px;                    /* one SCREEN pixel in game-frame units */
static double s_zoom, s_zcx, s_zcy;    /* the transform the text snap inverts  */
static int    s_ss;
static int   s_ntext, s_xover;         /* strings drawn / quads refused        */

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec2 aWorld;\n"
    "layout(location=3) in float aCol;\n"
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "out vec2 vUV; out vec2 vWorld; out float vCol;\n"
    "void main(){\n"
    /* the same scale-about-the-view-centre every world pass uses, so a marker
       tracks the unit it belongs to at any zoom */
    "  vec2 p = (aPos - uZoomC) * uZoom + uZoomC;\n"
    /* markers are the frame's top layer: depth 0, drawn with the test off */
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0, 0.0, 1.0);\n"
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
    /* Text is a COVERAGE mask, not a palette image: tagpu_text.c rasterises TA's
       glyphs with (fg,bg,transparent) = (255,0,0), so the texel says only whether
       the glyph covers this fragment and the colour comes from the vertex — which
       keeps the atlas colour-free and lets the same string be drawn in any
       colour for one raster. */
    "  if (uText != 0) {\n"
    "    if (texture(uLayer, vUV).r < 0.5) discard;\n"
    "    pi = int(vCol * 255.0 + 0.5);\n"
    /* a negative u marks the flat path: the vertex carries a palette index
       instead of a texel (health bars), the same convention the effects pass
       uses for its lines */
    "  } else if (vUV.x < 0.0) {\n"
    "    pi = int(vCol * 255.0 + 0.5);\n"
    "  } else {\n"
    "    pi = int(texture(uLayer, vUV).r * 255.0 + 0.5);\n"
    "    if (pi == uKey) discard;\n"
    "  }\n"
    TAGPU_GLSL_FOG_SHADE("pi")
    "  frag = vec4(texelFetch(uPal, ivec2(pi, 0), 0).rgb, 1.0);\n"
    "}\n";

static GLuint mksh(GLenum t, const char* src)
{
    GLuint sh = glCreateShader(t);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL); glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; glGetShaderInfoLog(sh, sizeof lg, NULL, lg);
               flog("mark: shader FAILED:"); flog(lg); s_state = 2; }
    return sh;
}

static void init_gl(void)
{
    GLuint vs, fs;
    GLint ok = 0;
    x_glDrawArrays = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glUniform1f  = (PFN_UNIFORM1F) getgl("glUniform1f");
    x_glUniform2f  = (PFN_UNIFORM2F) getgl("glUniform2f");
    x_glActiveTexture = (PFN_ACTIVETEX)getgl("glActiveTexture");
    x_glLineWidth  = (PFN_LINEWIDTH) getgl("glLineWidth");
    if (!x_glDrawArrays || !x_glUniform1f || !x_glUniform2f || !x_glActiveTexture) {
        flog("mark: missing GL proc"); s_state = 2; return;
    }
    vs = mksh(GL_VERTEX_SHADER, VS); fs = mksh(GL_FRAGMENT_SHADER, FS);
    if (s_state == 2) return;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs); glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) { flog("mark: link FAILED"); s_state = 2; return; }
    glDeleteShader(vs); glDeleteShader(fs);
    s_uGame   = glGetUniformLocation(s_prog, "uGame");
    s_uFog    = glGetUniformLocation(s_prog, "uFog");
    s_uFogOrg = glGetUniformLocation(s_prog, "uFogOrg");
    s_uFogDim = glGetUniformLocation(s_prog, "uFogDim");
    s_uZoom   = glGetUniformLocation(s_prog, "uZoom");
    s_uZoomC  = glGetUniformLocation(s_prog, "uZoomC");
    s_uKey    = glGetUniformLocation(s_prog, "uKey");
    s_uTextM  = glGetUniformLocation(s_prog, "uText");
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "uLayer"),   0);
    glUniform1i(glGetUniformLocation(s_prog, "uPal"),     1);
    glUniform1i(glGetUniformLocation(s_prog, "uFogGrid"), 2);
    glUniform1i(glGetUniformLocation(s_prog, "uFogLUT"),  3);
    glUseProgram(0);

    glGenVertexArrays(1, &s_vao); glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo); glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof s_verts, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, MVST * 4, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, MVST * 4, (void*)8);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, MVST * 4, (void*)16);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, MVST * 4, (void*)24);
    glBindVertexArray(0);

    memset(s_tex, 0, sizeof s_tex);
    memset(s_texW, 0, sizeof s_texW);
    memset(s_texH, 0, sizeof s_texH);
    s_state = 1;
    flog("mark: GL ready");
}

void tagpu_mark_glreset(void)
{
    s_state = 0;
    tagpu_text_glreset();
    memset(s_tex, 0, sizeof s_tex);      /* the ids died with the context */
    memset(s_texW, 0, sizeof s_texW);
    memset(s_texH, 0, sizeof s_texH);
}

/* ---- the health-bar gather ---- */

static void put_vert(int i, float x, float y, float u, float v, float wx, float wz,
                     float col)
{
    float* o = s_verts + (size_t)i * MVST;
    o[0] = x; o[1] = y; o[2] = u; o[3] = v; o[4] = wx; o[5] = wz; o[6] = col;
}

/* the order buckets' own writer: same vertex, a different array */
static void put_at(float* base, int i, float x, float y, float u, float v,
                   float wx, float wz, float col)
{
    float* o = base + (size_t)i * MVST;
    o[0] = x; o[1] = y; o[2] = u; o[3] = v; o[4] = wx; o[5] = wz; o[6] = col;
}

static void put_ord(float* base, int i, float x, float y, float wx, float wz,
                    float col)
{
    /* u < 0 is the flat path — the vertex carries a palette index rather than
       a texel, the convention the health bars and the effects lines share */
    put_at(base, i, x, y, -1.0f, -1.0f, wx, wz, col);
}

int tagpu_mark_emit_line(float x0, float y0, float x1, float y1,
                         int colidx, float wx, float wz)
{
    float c = (float)colidx / 255.0f;
    if (s_nordl + 2 > MAXORDL) return 0;
    put_ord(s_ordl, s_nordl + 0, x0, y0, wx, wz, c);
    put_ord(s_ordl, s_nordl + 1, x1, y1, wx, wz, c);
    s_nordl += 2;
    return 1;
}

int tagpu_mark_emit_tri(float x0, float y0, float x1, float y1,
                        float x2, float y2, int colidx, float wx, float wz)
{
    float c = (float)colidx / 255.0f;
    if (s_nordt + 3 > MAXORDT) return 0;
    put_ord(s_ordt, s_nordt + 0, x0, y0, wx, wz, c);
    put_ord(s_ordt, s_nordt + 1, x1, y1, wx, wz, c);
    put_ord(s_ordt, s_nordt + 2, x2, y2, wx, wz, c);
    s_nordt += 3;
    return 1;
}

/* SNAP A POINT ONTO THE DEVICE PIXEL GRID, THROUGH THE ZOOM — and do it by
   pre-image rather than by rounding the vertex.

   The vertex shader applies `(p - zoomC) * zoom + zoomC` and then a viewport
   `ss` device pixels to the game-frame unit, so the snap is: take the post-zoom
   position, round it onto the 1/ss grid, and hand back the point that
   transforms to it. Snapping the point we hand the shader instead — which is
   what the health bars did until 2026-09-09 — quantises the marker in the
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

   CONSTANT SCREEN SIZE, like the waypoint crosshair and unlike the health bars:
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

/* one DrawBar rect, edges INCLUSIVE — so the quad's far edge is +1.

   The float form is for the rects anchored to a UNIT, whose anchor is snapped
   onto the device grid by `snap_device` and is therefore not an integer in
   game-frame units at any zoom but 1x. `put_bar` keeps the integer signature
   for the rects that are the engine's own — the build-cursor footprint and the
   drag band — whose positions come from engine integers and stay bit-exact. */
static void put_barf(int* nv, float l, float t, float r, float b, int colidx,
                     float wx, float wz)
{
    float x0 = l, y0 = t, x1 = r + 1.0f, y1 = b + 1.0f;
    float c = (float)colidx / 255.0f;
    int i = *nv;
    put_vert(i + 0, x0, y0, -1.0f, -1.0f, wx, wz, c);
    put_vert(i + 1, x1, y0, -1.0f, -1.0f, wx, wz, c);
    put_vert(i + 2, x0, y1, -1.0f, -1.0f, wx, wz, c);
    put_vert(i + 3, x1, y0, -1.0f, -1.0f, wx, wz, c);
    put_vert(i + 4, x1, y1, -1.0f, -1.0f, wx, wz, c);
    put_vert(i + 5, x0, y1, -1.0f, -1.0f, wx, wz, c);
    *nv = i + QUADV;
}

static void put_bar(int* nv, int l, int t, int r, int b, int colidx,
                    float wx, float wz)
{
    put_barf(nv, (float)l, (float)t, (float)r, (float)b, colidx, wx, wz);
}

/* one DrawTranspRectangle: the four 1-px edges of the rect in one flat colour,
   every corner inclusive. Normalised because the inner rect is the outer inset
   by 1 and a footprint under 3 px across inverts it — which the engine's own
   line drawer also resolves by swapping the endpoints. */
static void put_outline(int* nv, int l, int t, int r, int b, int col,
                        float wx, float wz)
{
    int k;
    if (r < l) { k = l; l = r; r = k; }
    if (b < t) { k = t; t = b; b = k; }
    put_bar(nv, l, t, r, t, col, wx, wz);          /* top    */
    put_bar(nv, l, b, r, b, col, wx, wz);          /* bottom */
    put_bar(nv, l, t, l, b, col, wx, wz);          /* left   */
    put_bar(nv, r, t, r, b, col, wx, wz);          /* right  */
}

/* the build-cursor footprint and the drag band box — the whole of
   `0x469DB4..0x469F23`, transcribed in the header comment */
static void gather_cursor(const TAGPU_FXVIEW* v)
{
    const char* ta = v->ta;
    const unsigned char* gui = (const unsigned char*)(ta + OFF_GUICOL);
    const int* vp = (const int*)(ta + OFF_VPRECT);
    int mode, fl, l, t, r, b, idx, outer, inner, nv = CURSBASE;
    float wx, wz;

    s_ncurs = 0;
    if (s_armed != 1 || !s_cursor || s_passive) return;
    /* Without the redirect the engine is still drawing its own pair and ours
       would be a second, differently placed one. Refuse rather than double. */
    if (!tagpu_markown_installed()) return;

    fl   = *(const unsigned char*)(ta + OFF_MOUSEFL);
    mode = *(const unsigned char*)(ta + OFF_CURMODE);
    if (!(fl & 8)) {
        int mx, my;
        if (mode != CUR_BUILD) return;
        mx = *(const int*)(ta + OFF_MOUSE_X);
        my = *(const int*)(ta + OFF_MOUSE_Y);
        /* IsPositionInRect 0x4B6720 — inclusive on all four edges. Read the
           FIELD, not tagpu_vpwide_true_rect: while zoomed out that field is
           deliberately wider, and it is exactly that width which lets a
           placement in the outer ring pass the gate at all. */
        if (mx < vp[0] || mx > vp[2] || my < vp[1] || my > vp[3]) return;
    }

    l = *(const int*)(ta + OFF_CUR_X1) - v->eyeX + 0x80;
    r = *(const int*)(ta + OFF_CUR_X2) - v->eyeX + 0x80;
    t = *(const int*)(ta + OFF_CUR_Z1) - (*(const int*)(ta + OFF_CUR_H1) >> 1)
        - v->eyeY + 0x20;
    b = *(const int*)(ta + OFF_CUR_Z2) - (*(const int*)(ta + OFF_CUR_H2) >> 1)
        - v->eyeY + 0x20;
    /* the globals are live sim state read from the render thread, so a rect
       that could not be one is dropped rather than turned into a quad */
    if (l < -0x100000 || l > 0x100000 || r < -0x100000 || r > 0x100000 ||
        t < -0x100000 || t > 0x100000 || b < -0x100000 || b > 0x100000) return;

    /* the colour pair keys off the MODE, not off which arm of the gate let it
       through — the engine picks it from main+0x2CC3 both times */
    if (mode == CUR_BUILD) {
        idx   = (fl & 0x40) ? GUI_GREEN : GUI_BLOCKED;
        outer = gui[idx];
        inner = outer;
    } else {
        outer = gui[GUI_WHITE];
        inner = gui[GUI_BLACK];
    }

    /* NORMALISE FIRST, THEN INSET — the engine's order, and the two are not
       interchangeable. `0x469E92`/`0x469EA0` swap the pairs and only then does
       `0x469ECA..0x469EDD` inc/dec the SWAPPED values for the inner rect. Insetting
       first and letting put_outline swap afterwards turns the inset into an
       OUTSET whenever the stored rect runs right-to-left or bottom-to-top —
       the black inner outline lands one pixel outside the white one. A band box
       dragged up-left is exactly that case, and the G13n A/B only covered a
       down-right drag. */
    if (r < l) { int k = l; l = r; r = k; }
    if (b < t) { int k = t; t = b; b = k; }

    /* the layer is drawn with the fog off, so this is only the world point the
       rect's own corner maps to — the same projection layer_quad uses */
    wx = (float)(l - v->vpL + v->eyeX);
    wz = (float)(t - v->vpT + v->eyeY);
    put_outline(&nv, l, t, r, b, outer, wx, wz);
    put_outline(&nv, l + 1, t + 1, r - 1, b - 1, inner, wx, wz);
    s_ncurs = nv - CURSBASE;
}

int tagpu_mark_gather(const TAGPU_FXVIEW* v)
{
    const char* ta = v->ta;
    const char *beg, *end, *u;
    const unsigned char* gui;
    int watched, nv = BARBASE;

    s_nbar = 0; s_cBar = 0; s_nordt = 0; s_nordl = 0; s_nordx = 0;
    s_nordxOrd = 0; s_ntext = 0; s_xover = 0;
    /* before anything emits: tagpu_order.c's labels come through
       tagpu_mark_emit_text, which sizes its quads with this */
    /* one font for the whole frame, before anything asks the atlas for a string */
    tagpu_text_frame();
    s_px  = 1.0 / (double)(v->zoom > 0.0f ? v->zoom : 1.0f);
    s_zoom = v->zoom > 0.0f ? (double)v->zoom : 1.0;
    s_zcx = (double)v->zoomCx; s_zcy = (double)v->zoomCy;
    s_ss  = v->ss > 0 ? v->ss : 1;
    /* first, and outside every gate below: neither the cursor nor the order
       markers are health bars, and neither `damagebars` nor `nobars` has
       anything to say about them */
    gather_cursor(v);
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
    /* The bar skip is a byte in the engine's code path and the digit skip is a
       redirected call site; without the patches the engine is still drawing
       both itself and ours would be a second set at a second position. Refuse
       rather than double-draw. */
    if (!tagpu_markown_installed()) return 0;
    if (!(*(const unsigned char*)(ta + OFF_GAMEOPT) & 1)) return 0;  /* damagebars */

    beg = *(const char* const*)(ta + OFF_BEGIN);
    end = *(const char* const*)(ta + OFF_END);
    if (!ptr_ok(beg) || !ptr_ok(end) || end <= beg) return 0;
    if ((size_t)(end - beg) > (size_t)UNIT_STRIDE * 20000) return 0;
    watched = *(const unsigned char*)(ta + OFF_BAROWNER);
    gui = (const unsigned char*)(ta + OFF_GUICOL);

    for (u = beg + UNIT_STRIDE; u < end && s_cBar < MAXBAR; u += UNIT_STRIDE) {
        unsigned st = *(const unsigned*)(u + U_STATE);
        const char* def;
        int hp, maxhp, third, w, col;
        float x, y;                     /* the anchor, in game-frame units     */
        float fpx, fpa, fpd;            /* world x, ALTITUDE, map depth        */
        float wx, wz;
        if (!(st & 0x10000000u) || (st & 0x4000u)) continue;
        if (*(const unsigned char*)(u + U_OWNER) != (unsigned)watched) continue;

        /* THE ANCHOR IS THE BODY'S, NOT THE ENGINE'S SHORTS. `tagpu_native_unit_pos`
           hands back the very sample the unit pass drew this unit from, so the bar
           cannot disagree with the model it sits over. Reading the shorts here
           instead — which is what this loop did until 2026-09-09 — pinned the bar to
           the SIM rate while the body glided at present rate, and the two then slid
           against each other by up to a whole tick of motion every tick: measured on
           a walking commander at 1920x1080, 0.561 px rms and 2.95 px peak-to-peak at
           1x, and exactly `zoom` times that on screen, because bars are emitted
           unzoomed and the vertex shader scales them (5.77 px p2p at 2x). That is the
           health-bar wobble; the selection box never had it, because
           `tagpu_native.c`'s selbox has always taken this same anchor.

           THE CALL IS SAFE HERE BY CONSTRUCTION, not by timing: `tagpu_mark_gather`
           is called from inside the unit pass's own gather (`tagpu_native.c`), on the
           same thread, in the same frame, AFTER the walk that fills the sub-pixel
           table — and the accessor refuses any sample that does not still describe
           this unit's current 16.16 position, so a recycled slot falls through.

           AND THE ANCHOR KEEPS ITS FRACTION. Flooring it — which is what this loop
           did between 2026-09-09 and the fix below — quantises the bar in the
           frame's PRE-zoom units, so the bar steps `zoom` game pixels at a time
           while the body glides continuously underneath it: the residual reads as a
           2-4 px diagonal twitch at max zoom-in, which is what the owner saw after
           the first fix. `snap_device` puts the anchor on the DEVICE grid instead,
           the same rule the selection rect and the glyph atlas already follow, so
           the step is one device pixel at every zoom.

           WITH NO SAMPLE the engine's own integer arithmetic is kept term for term
           — `(s16)` of the 16.16 is a floor and `sar 1` floors the already-floored
           height a second time — so at 1x, where `snap_device` is the identity on an
           integer, the unit pass disarmed or `tagpu_subpix.off` still produces
           exactly the bytes this loop produced before any of this work. */
        if (tagpu_native_unit_pos(u, &fpx, &fpa, &fpd)) {
            x  = fpx - (float)v->eyeX + 128.0f;
            y  = fpd - (float)v->eyeY - fpa * 0.5f + 32.0f + 10.0f;
            wx = fpx;
            wz = fpd - fpa * 0.5f;
        } else {
            int px = *(const short*)(u + U_XPOS);   /* world x            */
            int pa = *(const short*)(u + U_ZPOS);   /* altitude  (U_ZPOS) */
            int pd = *(const short*)(u + U_YPOS);   /* map depth (U_YPOS) */
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
            unsigned squad = *(const unsigned*)(u + U_SQUAD);
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

        hp = *(const short*)(u + U_HEALTH);
        if (hp <= 0) continue;
        def = *(const char* const*)(u + U_TYPE);
        if (!ptr_ok(def)) continue;
        maxhp = *(const int*)(def + UD_MAXHP);
        if (maxhp <= 0) continue;                 /* the engine's div would trap */
        s_cBar++;
        if (s_passive) continue;      /* the A/B lever: count, let the engine draw */

        put_barf(&nv, x - 17.0f, y - 2.0f, x + 17.0f, y + 2.0f, gui[GUI_BLACK],
                 wx, wz);
        /* (Health << 5) / maxHP as an UNSIGNED divide, and the thirds through
           the same maxHP/3 the engine's reciprocal multiply produces */
        w = (int)(((unsigned)(hp << 5)) / (unsigned)maxhp);
        third = (int)((unsigned)maxhp / 3u);
        col = hp > 2 * third ? gui[GUI_GREEN]
            : hp > third     ? gui[GUI_YELLOW]
            :                  gui[GUI_RED];
        put_barf(&nv, x - 16.0f, y - 1.0f, x - 16.0f + (float)w, y + 1.0f, col,
                 wx, wz);
    }
    s_nbar = nv - BARBASE;
    return s_cBar;
}

/* ---- render ---- */

/* upload one captured layer into its texture; 0 if it cannot be shown */
static int upload_layer(int i, const TAGPU_MARKLAYER* L)
{
    if (!s_tex[i]) {
        glGenTextures(1, &s_tex[i]);
        if (!s_tex[i]) return 0;
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
        /* NEAREST: the texel IS a palette index and interpolating two of them
           produces a colour that is in neither */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_texW[i] = 0; s_texH[i] = 0;
    } else {
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* the capture buffer is the engine's surface pitch wide; upload the
       viewport window out of it without a staging copy */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, L->pitch);
    if (L->w != s_texW[i] || L->h != s_texH[i]) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, L->w, L->h, 0,
                     GL_RED, GL_UNSIGNED_BYTE, L->pix);
        s_texW[i] = L->w; s_texH[i] = L->h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, L->w, L->h,
                        GL_RED, GL_UNSIGNED_BYTE, L->pix);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return 1;
}

static void layer_quad(const TAGPU_FXVIEW* v, int base, const TAGPU_MARKLAYER* L)
{
    /* screen and world differ by a pure translation here, so the quad's world
       coordinates interpolate exactly across it and the fog rule lands on the
       same cells the engine's overlay would have */
    float x0 = (float)L->x, y0 = (float)L->y;
    float x1 = (float)(L->x + L->w), y1 = (float)(L->y + L->h);
    float wx0 = x0 - (float)v->vpL + (float)v->eyeX;
    float wz0 = y0 - (float)v->vpT + (float)v->eyeY;
    float wx1 = x1 - (float)v->vpL + (float)v->eyeX;
    float wz1 = y1 - (float)v->vpT + (float)v->eyeY;
    put_vert(base + 0, x0, y0, 0.0f, 0.0f, wx0, wz0, 0.0f);
    put_vert(base + 1, x1, y0, 1.0f, 0.0f, wx1, wz0, 0.0f);
    put_vert(base + 2, x0, y1, 0.0f, 1.0f, wx0, wz1, 0.0f);
    put_vert(base + 3, x1, y0, 1.0f, 0.0f, wx1, wz0, 0.0f);
    put_vert(base + 4, x1, y1, 1.0f, 1.0f, wx1, wz1, 0.0f);
    put_vert(base + 5, x0, y1, 0.0f, 1.0f, wx0, wz1, 0.0f);
}

void tagpu_mark_render(const TAGPU_FXVIEW* v, unsigned int palTex)
{
    TAGPU_MARKLAYER lay[TAGPU_MARK_NLAYER];
    int have[TAGPU_MARK_NLAYER];
    int i, total, textBase = 0;
    unsigned int textTex = 0;

    if (s_state == 0) init_gl();
    if (s_state != 1 || s_armed != 1) return;
    /* The heartbeat says "this pass ran", NOT "this pass drew something": a
       frame with no bars and no markers is the ordinary case, and letting the
       watchdog read that as a dead pass would hand the draw back and take it
       again 30 frames later, forever. */
    tagpu_markown_beat(v->frame_counter);
    if (!s_passive) {
        tagpu_markown_set_capture(s_capture);
        tagpu_markown_set_bars(s_bars);
        tagpu_markown_set_selbox(s_selbox);
        tagpu_markown_set_cursor(s_cursor);
        tagpu_markown_set_digits(s_digits);
    }

    x_glActiveTexture(GL_TEXTURE0);
    for (i = 0; i < TAGPU_MARK_NLAYER; i++) {
        have[i] = (!s_passive && tagpu_markown_layer(i, &lay[i])) ? 1 : 0;
        if (have[i]) have[i] = upload_layer(i, &lay[i]);
        if (have[i]) layer_quad(v, i * QUADV, &lay[i]);
    }
    if (!have[TAGPU_MARK_POSTFOG] && s_nbar == 0 && s_ncurs == 0 &&
        s_nordt == 0 && s_nordl == 0 && s_nordx == 0) return;

    glUseProgram(s_prog);
    x_glUniform2f(s_uGame, (float)v->gw, (float)v->gh);
    x_glUniform1f(s_uZoom, v->zoom > 0.0f ? v->zoom : 1.0f);
    x_glUniform2f(s_uZoomC, v->zoomCx, v->zoomCy);
    if (s_uFogOrg >= 0) x_glUniform2f(s_uFogOrg, (float)v->fogOrgX, (float)v->fogOrgY);
    if (s_uFogDim >= 0) x_glUniform2f(s_uFogDim, (float)v->fogCols, (float)v->fogRows);
    glUniform1i(s_uKey, tagpu_markown_key());
    glUniform1i(s_uTextM, 0);
    x_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, palTex);
    x_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, v->fogTex);
    x_glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, v->fogLut);
    x_glActiveTexture(GL_TEXTURE0);

    total = BARBASE + s_nbar;
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* one orphan, then each bucket at its own offset: the order buckets live
       in their own arrays (see MAXORDT) and are packed in behind the
       triangles, so their first vertex moves with the bar count */
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(total + s_nordt + s_nordl + s_nordx) * MVST * 4,
                 NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)total * MVST * 4, s_verts);
    if (s_nordt)
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)total * MVST * 4,
                        (GLsizeiptr)s_nordt * MVST * 4, s_ordt);
    if (s_nordl)
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(total + s_nordt) * MVST * 4,
                        (GLsizeiptr)s_nordl * MVST * 4, s_ordl);
    if (s_nordx)
        glBufferSubData(GL_ARRAY_BUFFER,
                        (GLintptr)(total + s_nordt + s_nordl) * MVST * 4,
                        (GLsizeiptr)s_nordx * MVST * 4, s_ordx);
    textBase = total + s_nordt + s_nordl;

    /* The engine's own order inside the block: order markers and their
       ShowRanges labels first (`0x469BFC`), then the health bars over them
       (`0x469CB9`), then the group digit over those (`0x469CF9`), and the build
       cursor last of all — after the fog overlay, which is why it carries no
       fog. */
    if (s_nordt || s_nordl) {
        /* one SCREEN pixel of line, which is `ss` device pixels of the
           supersampled target — the same rule the effects pass uses, and what
           keeps a native-res marker a hairline at 4x instead of a 1997 pixel
           blown up to sixteen */
        glUniform1i(s_uFog, v->fogMode & 1);
        if (s_nordt) x_glDrawArrays(GL_TRIANGLES, total, s_nordt);
        if (s_nordl) {
            if (x_glLineWidth) x_glLineWidth((GLfloat)(v->ss > 0 ? v->ss : 1));
            x_glDrawArrays(GL_LINES, total + s_nordt, s_nordl);
        }
    }
    if (s_nordx) {
        textTex = tagpu_text_tex();       /* binds it, and uploads if dirty */
        if (textTex) {
            glUniform1i(s_uFog, v->fogMode & 1);
            glUniform1i(s_uTextM, 1);
            if (s_nordxOrd)
                x_glDrawArrays(GL_TRIANGLES, textBase, s_nordxOrd);
        }
    }
    if (s_nbar) {
        glUniform1i(s_uTextM, 0);
        glUniform1i(s_uFog, v->fogMode & 1);
        x_glDrawArrays(GL_TRIANGLES, BARBASE, s_nbar);
    }
    if (textTex && s_nordx > s_nordxOrd) {
        /* the digits, over the bars, out of the same atlas — bound again
           because the bar draw above did not touch unit 0's binding but the
           mode uniform did */
        glUniform1i(s_uTextM, 1);
        glUniform1i(s_uFog, v->fogMode & 1);
        x_glDrawArrays(GL_TRIANGLES, textBase + s_nordxOrd, s_nordx - s_nordxOrd);
    }
    glUniform1i(s_uTextM, 0);
    if (have[TAGPU_MARK_POSTFOG]) {
        glUniform1i(s_uFog, 0);
        glBindTexture(GL_TEXTURE_2D, s_tex[TAGPU_MARK_POSTFOG]);
        x_glDrawArrays(GL_TRIANGLES, TAGPU_MARK_POSTFOG * QUADV, QUADV);
    }
    /* last, and with the fog off for the same reason the layer above has it
       off: the engine draws these two rects after its fog overlay and never
       darkens them */
    if (s_ncurs) {
        glUniform1i(s_uFog, 0);
        x_glDrawArrays(GL_TRIANGLES, CURSBASE, s_ncurs);
    }

    if (s_log) {
        static unsigned last = 0;
        if (v->frame_counter - last >= 120) {
            char b[320];
            int nstr = 0, ndrop = 0;
            last = v->frame_counter;
            tagpu_text_stats(&nstr, &ndrop);
            _snprintf(b, sizeof b,
                "mark: bars=%d cursor=%d ordtri=%d ordline=%d text=%d(lab=%d) "
                "atlas=%d/%d over=%d postfog=%s key=%d vp=(%d,%d %dx%d) "
                "zoom=%.2f%s",
                s_cBar, s_ncurs / QUADV, s_nordt / 3, s_nordl / 2,
                s_ntext, s_nordxOrd / QUADV, nstr, ndrop, s_xover,
                have[TAGPU_MARK_POSTFOG] ? "captured" : "-",
                tagpu_markown_key(), v->vpL, v->vpT, v->vw, v->vh, v->zoom,
                s_passive ? " (passive: engine still drawing)" : "");
            flog(b);
        }
    }
}
