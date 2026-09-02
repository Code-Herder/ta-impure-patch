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
   positions), so a bar steps once per sim tick while the native unit pass
   interpolates its body between ticks. We keep the engine's arithmetic rather
   than smoothing it — a bar is 35 px of flat colour over a unit that moves a
   couple of pixels per frame, and the alternative is a second, differently
   sourced anchor that can disagree with the body's. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "opengl_utils.h"
#include "tagpu_mark.h"
#include "tagpu_markown.h"
#include "tagpu_glsl.h"

/* ---- engine layout ---- */
#define OFF_BEGIN    0x14357     /* unit array base / end (stride 0x118)      */
#define OFF_END      0x1435B
#define OFF_WATCHED  0x2A42      /* u8 watched player id                      */
#define OFF_GAMEOPT  0x37F06     /* bit0 = the registry option "damagebars"   */
#define OFF_GUICOL   0x0DCB      /* GUI colour byte array (GetGuiPaletteColor)*/
#define UNIT_STRIDE  0x118
#define U_XPOS       0x6C        /* s16 world x                               */
#define U_ZPOS       0x70        /* s16 altitude                              */
#define U_YPOS       0x74        /* s16 world z (map depth)                   */
#define U_TYPE       0x92        /* UnitDefStruct*                            */
#define U_HEALTH     0x108       /* s16                                       */
#define U_STATE      0x110       /* bit28 alive, bit14 excluded               */
#define U_OWNER      0xFF        /* u8 player id                              */
#define UD_MAXHP     0x1FA       /* read as a DWORD, as the engine's div does */

#define GUI_BLACK    0x00
#define GUI_GREEN    0x0A
#define GUI_RED      0x0C
#define GUI_YELLOW   0x0E

#define MAXBAR       2048                    /* bars per frame               */
#define MVST         7                       /* x,y, u,v, wx,wz, colour      */
#define QUADV        6
#define BARBASE      (2 * QUADV)             /* verts 0..11 are the two layer
                                                quads; bars follow            */
#define MAXMV        (BARBASE + MAXBAR * 2 * QUADV)

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
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_UNIFORM1F  x_glUniform1f;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_ACTIVETEX  x_glActiveTexture;

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
static unsigned s_armCheck = 0;

int tagpu_mark_armed(unsigned frame_counter)
{
    int was;
    HANDLE h;
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return s_armed > 0;
    s_armCheck = frame_counter;
    was = s_armed;
    s_armed = 0;
    h = CreateFileA("tagpu_mark.on", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        tagpu_markown_set_capture(0);
        tagpu_markown_set_bars(0);
        tagpu_markown_set_selbox(0);
        if (was > 0) flog("mark: disarmed");
        return 0;
    }
    {
        char buf[128]; DWORD n = 0;
        s_log = 0; s_passive = 0; s_bars = 1; s_capture = 1; s_selbox = 1;
        if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
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
                if (last) break;
                p = q + 1;
            }
        }
    }
    CloseHandle(h);
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
    if (was != 1) {
        char b[160];
        _snprintf(b, sizeof b, "mark: ARMED (log=%d passive=%d bars=%d capture=%d "
                  "selbox=%d patched=%d)", s_log, s_passive, s_bars, s_capture,
                  s_selbox, tagpu_markown_installed());
        flog(b);
    }
    return 1;
}

/* ---- GL ---- */
static int    s_state = 0;             /* 0 unloaded, 1 ready, 2 failed       */
static GLuint s_prog, s_vao, s_vbo, s_tex[TAGPU_MARK_NLAYER];
static int    s_texW[TAGPU_MARK_NLAYER], s_texH[TAGPU_MARK_NLAYER];
static GLint  s_uGame, s_uFog, s_uFogOrg, s_uFogDim, s_uZoom, s_uZoomC, s_uKey;

static float s_verts[MAXMV * MVST];
static int   s_nbar;                   /* bars gathered (2 quads each)        */
static int   s_cBar;                   /* counted, whether emitted or not     */

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
    /* a negative u marks the flat path: the vertex carries a palette index
       instead of a texel (health bars), the same convention the effects pass
       uses for its lines */
    "  if (vUV.x < 0.0) {\n"
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

/* one DrawBar rect, edges INCLUSIVE — so the quad's far edge is +1 */
static void put_bar(int* nv, int l, int t, int r, int b, int colidx,
                    float wx, float wz)
{
    float x0 = (float)l, y0 = (float)t, x1 = (float)(r + 1), y1 = (float)(b + 1);
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

int tagpu_mark_gather(const TAGPU_FXVIEW* v)
{
    const char* ta = v->ta;
    const char *beg, *end, *u;
    const unsigned char* gui;
    int watched, nv = BARBASE;

    s_nbar = 0; s_cBar = 0;
    if (s_armed != 1 || !s_bars) return 0;
    /* The bar skip is a byte in the engine's code path; without the patch the
       engine is still drawing bars itself and ours would be a second set at a
       second position. Refuse rather than double-draw. */
    if (!tagpu_markown_installed()) return 0;
    if (!(*(const unsigned char*)(ta + OFF_GAMEOPT) & 1)) return 0;  /* damagebars */

    beg = *(const char* const*)(ta + OFF_BEGIN);
    end = *(const char* const*)(ta + OFF_END);
    if (!ptr_ok(beg) || !ptr_ok(end) || end <= beg) return 0;
    if ((size_t)(end - beg) > (size_t)UNIT_STRIDE * 20000) return 0;
    watched = *(const unsigned char*)(ta + OFF_WATCHED);
    gui = (const unsigned char*)(ta + OFF_GUICOL);

    for (u = beg + UNIT_STRIDE; u < end && s_cBar < MAXBAR; u += UNIT_STRIDE) {
        unsigned st = *(const unsigned*)(u + U_STATE);
        const char* def;
        int hp, maxhp, x, y, third, w, col;
        float wx, wz;
        if (!(st & 0x10000000u) || (st & 0x4000u)) continue;
        if (*(const unsigned char*)(u + U_OWNER) != (unsigned)watched) continue;
        hp = *(const short*)(u + U_HEALTH);
        if (hp <= 0) continue;
        def = *(const char* const*)(u + U_TYPE);
        if (!ptr_ok(def)) continue;
        maxhp = *(const int*)(def + UD_MAXHP);
        if (maxhp <= 0) continue;                 /* the engine's div would trap */

        x = *(const short*)(u + U_XPOS) - v->eyeX + 0x80;
        y = *(const short*)(u + U_YPOS) - v->eyeY
            - (*(const short*)(u + U_ZPOS) >> 1) + 0x20 + 0x0A;
        /* cull to the ZOOM's rect, not the engine's: at zoom < 1 the frame
           shows units the engine's own HotUnits list has already dropped */
        if (x + 0x12 < v->evpL || x - 0x12 > v->evpL + v->evw ||
            y + 3 < v->evpT || y - 3 > v->evpT + v->evh) continue;
        s_cBar++;
        if (s_passive) continue;      /* the A/B lever: count, let the engine draw */

        /* the fog is sampled at the unit's own anchor, in the projected world
           space the engine's screen grid is built in (tagpu_fx.h) */
        wx = (float)*(const short*)(u + U_XPOS);
        wz = (float)(*(const short*)(u + U_YPOS) - (*(const short*)(u + U_ZPOS) >> 1));

        put_bar(&nv, x - 0x11, y - 2, x + 0x11, y + 2, gui[GUI_BLACK], wx, wz);
        /* (Health << 5) / maxHP as an UNSIGNED divide, and the thirds through
           the same maxHP/3 the engine's reciprocal multiply produces */
        w = (int)(((unsigned)(hp << 5)) / (unsigned)maxhp);
        third = (int)((unsigned)maxhp / 3u);
        col = hp > 2 * third ? gui[GUI_GREEN]
            : hp > third     ? gui[GUI_YELLOW]
            :                  gui[GUI_RED];
        put_bar(&nv, x - 0x10, y - 1, x - 0x10 + w, y + 1, col, wx, wz);
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
    int i, total;

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
    }

    x_glActiveTexture(GL_TEXTURE0);
    for (i = 0; i < TAGPU_MARK_NLAYER; i++) {
        have[i] = (!s_passive && tagpu_markown_layer(i, &lay[i])) ? 1 : 0;
        if (have[i]) have[i] = upload_layer(i, &lay[i]);
        if (have[i]) layer_quad(v, i * QUADV, &lay[i]);
    }
    if (!have[0] && !have[1] && s_nbar == 0) return;

    glUseProgram(s_prog);
    x_glUniform2f(s_uGame, (float)v->gw, (float)v->gh);
    x_glUniform1f(s_uZoom, v->zoom > 0.0f ? v->zoom : 1.0f);
    x_glUniform2f(s_uZoomC, v->zoomCx, v->zoomCy);
    if (s_uFogOrg >= 0) x_glUniform2f(s_uFogOrg, (float)v->fogOrgX, (float)v->fogOrgY);
    if (s_uFogDim >= 0) x_glUniform2f(s_uFogDim, (float)v->fogCols, (float)v->fogRows);
    glUniform1i(s_uKey, tagpu_markown_key());
    x_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, palTex);
    x_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, v->fogTex);
    x_glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, v->fogLut);
    x_glActiveTexture(GL_TEXTURE0);

    total = BARBASE + s_nbar;
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)total * MVST * 4, s_verts,
                 GL_STREAM_DRAW);

    /* the engine's own order inside the block: order markers first, then the
       health bars over them (0x469BFC before 0x469CB9), and the build cursor
       last of all — after the fog overlay, which is why it carries no fog */
    if (have[TAGPU_MARK_PREFOG]) {
        glUniform1i(s_uFog, v->fogMode & 1);
        glBindTexture(GL_TEXTURE_2D, s_tex[TAGPU_MARK_PREFOG]);
        x_glDrawArrays(GL_TRIANGLES, TAGPU_MARK_PREFOG * QUADV, QUADV);
    }
    if (s_nbar) {
        glUniform1i(s_uFog, v->fogMode & 1);
        x_glDrawArrays(GL_TRIANGLES, BARBASE, s_nbar);
    }
    if (have[TAGPU_MARK_POSTFOG]) {
        glUniform1i(s_uFog, 0);
        glBindTexture(GL_TEXTURE_2D, s_tex[TAGPU_MARK_POSTFOG]);
        x_glDrawArrays(GL_TRIANGLES, TAGPU_MARK_POSTFOG * QUADV, QUADV);
    }

    if (s_log) {
        static unsigned last = 0;
        if (v->frame_counter - last >= 120) {
            char b[192];
            last = v->frame_counter;
            _snprintf(b, sizeof b,
                "mark: bars=%d prefog=%s postfog=%s key=%d vp=(%d,%d %dx%d) zoom=%.2f%s",
                s_cBar,
                have[TAGPU_MARK_PREFOG] ? "captured" : "-",
                have[TAGPU_MARK_POSTFOG] ? "captured" : "-",
                tagpu_markown_key(), v->vpL, v->vpT, v->vw, v->vh, v->zoom,
                s_passive ? " (passive: engine still drawing)" : "");
            flog(b);
        }
    }
}
