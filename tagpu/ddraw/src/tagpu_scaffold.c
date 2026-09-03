/* tagpu_scaffold.c — G12a: the scene-depth scaffold (Phase D, native-res pass).

   The engine has no screen depth plane (terrain-depth.md §4): scene order is a
   painter's sweep keyed on the 16-px map-tile row (§3.3). This module rebuilds
   that key per frame, from live engine data only, as a viewport-sized byte
   buffer ("the scaffold"):

       0                 = free (terrain / flat features / nothing) = FAR
       3 + relRow*4      = a TALL feature's silhouette pixel (def Height >= 10),
                           stamped at its anchor-row key; larger = nearer

   A ground unit's compare value is 1 + relRow*4 (units draw before features
   within a row, so a feature of the SAME row occludes the unit; a unit one row
   lower occludes that feature). relRow is relative to the sweep's first row
   (eyeY>>4 - 16), exactly the engine's own bucket origin.

   Debug outputs (G12a exit evidence):
     - colour overlay: scaffold pixels tinted far(blue)->near(red), 55% alpha,
       drawn over the live frame (capture with tagpu_glshot.trigger);
     - per-unit occlusion PREDICTION in tagpu.log ("scaffold: uNNN ... occl=P%"):
       fraction of the unit's composite rect covered by scaffold pixels nearer
       than the unit's row key. The engine frame must agree.

   RESOLUTION RULE (Phase D constraint): nothing here is 640x480 — the viewport
   rect (main+0x37E27..), view dims (main+0x37E37/3B) and sweep dims
   (main+0x1424B/4F) are read live every frame. Armed by tagpu_scaffold.on. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "opengl_utils.h"
#include "tagpu_scaffold.h"
#include "tagpu_vpwide.h"

/* ---- engine layout (terrain-depth.md, binary-verified) ---- */
#define TA_MAINPP    0x00511DE8u
#define OFF_EYEX     0x1431F
#define OFF_EYEY     0x14323
#define OFF_VP_L     0x37E27   /* viewport rect on the offscreen: l,t,r,b (int) */
#define OFF_VP_T     0x37E2B
#define OFF_VIEW_W   0x37E37   /* view W/H in px (int)                          */
#define OFF_VIEW_H   0x37E3B
#define OFF_MAP_W16  0x14233   /* map W/H in 16-px tiles (int)                  */
#define OFF_MAP_H16  0x14237
#define OFF_SWEEP_C  0x1424B   /* sweep cols = viewTilesX+0xC (int)             */
#define OFF_SWEEP_R  0x1424F   /* sweep rows = viewTilesY+0x20 (int)            */
#define OFF_FEATMAP  0x14287   /* FeatureStruct grid, stride 0xD                */
#define OFF_FEATDEF  0x1426F   /* FeatureDef array, stride 0x100                */
#define OFF_BEGIN    0x14357
#define OFF_END      0x1435B
#define UNIT_STRIDE  0x118
#define U_STATE      0x110
#define U_XPOS       0x6C
#define U_ZPOS       0x70      /* altitude */
#define U_YPOS       0x74      /* world Z (map depth) — THE sort key source */
#define U_OBJ3DO     0x9E
#define U_TYPE       0x92
#define O3_COMPOSITE 0x10
#define FEAT_STRIDE  0x0D
#define FT_HEIGHT    0x04      /* u8 tile height                                */
#define FT_DEFIDX    0x08      /* u16; <0xFFFB = live feature anchor            */
#define FD_STRIDE    0x100
#define FD_FOOTX     0x94      /* u16 footprint (16-px tiles)                   */
#define FD_FOOTZ     0x96
#define FD_BODYSEQ   0xAC      /* GAF sequence (static body frames)             */
#define FD_HEIGHT    0xFA      /* u8; >=10 = tall (defers to the row sweep)     */
#define GF_WIDTH     0x00      /* GAFFrame header                               */
#define GF_HEIGHT    0x02
#define GF_HOTX      0x04
#define GF_HOTY      0x06
#define GF_CKEY      0x08
#define GF_COMPRESSED 0x09
#define GF_PTRCOLOR  0x10
#define SEQ_NFRAMES  0x00      /* GAF anim entry: u16 frame count               */
#define SEQ_FRAMES   0x28      /* -> inline GAFFrame[] table, stride 0x18       */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void slog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* GL entries (G1 lesson: GL1.1 via opengl32, the rest via wgl) */
typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum,GLint,GLsizei);
typedef void (APIENTRY *PFN_DISABLE)(GLenum);
typedef void (APIENTRY *PFN_BLENDFUNC)(GLenum,GLenum);
typedef void (APIENTRY *PFN_UNIFORM4F)(GLint,GLfloat,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM1F)(GLint,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM1I)(GLint,GLint);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_DISABLE    x_glDisable;
static PFN_BLENDFUNC  x_glBlendFunc;
static PFN_UNIFORM4F  x_glUniform4f;
static PFN_UNIFORM1F  x_glUniform1f;
static PFN_UNIFORM1I  x_glUniform1i;
static PFN_ACTIVETEX  x_glActiveTexture;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

static int    s_state = 0;         /* 0=unloaded 1=ready 2=failed         */
void tagpu_scaffold_glreset(void);
static void serr(const char* tag);
static int    s_armed = -1;        /* re-checked every 30 frames          */
static GLuint s_prog, s_vao, s_vbo, s_tex;
static GLint  s_uRect, s_uRows;
static unsigned char* s_buf = 0;   /* viewport-sized scaffold, malloc'd   */
static int    s_bw = 0, s_bh = 0;  /* current buffer dims                 */
static int    s_texW = 0, s_texH = 0;
static int    s_lastR0 = 0, s_lastRows = 0;
static unsigned s_lastFrame = 0;

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 p;\n"           /* unit quad 0..1 */
    "uniform vec4 uRect;\n"                     /* NDC x0,y0,x1,y1 */
    "out vec2 uv;\n"
    "void main(){ uv=p;\n"
    "  gl_Position=vec4(mix(uRect.x,uRect.z,p.x), mix(uRect.y,uRect.w,p.y),0.,1.); }\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    "uniform sampler2D uScaf; uniform float uRows;\n"
    "void main(){ float v = texture(uScaf, uv).r * 255.0;\n"
    "  if (v < 2.5) discard;\n"                 /* 0 = far/free */
    "  float rel = (v - 3.0) / 4.0;\n"
    "  float t = clamp(rel / max(uRows - 1.0, 1.0), 0.0, 1.0);\n"
    "  vec3 c = mix(vec3(0.10,0.35,1.00), vec3(1.00,0.15,0.10), t);\n"
    "  frag = vec4(c, 0.55); }\n";

static void init_gl(void)
{
    x_glDrawArrays    = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glDisable       = (PFN_DISABLE)   getgl("glDisable");
    x_glBlendFunc     = (PFN_BLENDFUNC) getgl("glBlendFunc");
    x_glUniform4f     = (PFN_UNIFORM4F) getgl("glUniform4f");
    x_glUniform1f     = (PFN_UNIFORM1F) getgl("glUniform1f");
    x_glUniform1i     = (PFN_UNIFORM1I) getgl("glUniform1i");
    x_glActiveTexture = (PFN_ACTIVETEX) getgl("glActiveTexture");
    if (!x_glDrawArrays || !x_glDisable || !x_glBlendFunc || !x_glUniform4f ||
        !x_glUniform1f || !x_glUniform1i || !x_glActiveTexture)
    { slog("scaffold: missing GL proc"); s_state = 2; return; }

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS, NULL); glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FS, NULL); glCompileShader(fs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (ok) glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; glGetShaderInfoLog(fs, sizeof lg, NULL, lg);
               slog("scaffold: shader FAILED:"); slog(lg); s_state = 2; return; }
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs); glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) { slog("scaffold: link FAILED"); s_state = 2; return; }
    glDeleteShader(vs); glDeleteShader(fs);
    s_uRect = glGetUniformLocation(s_prog, "uRect");
    s_uRows = glGetUniformLocation(s_prog, "uRows");
    GLint uScaf = glGetUniformLocation(s_prog, "uScaf");

    const float quad[] = { 0,0, 1,0, 0,1, 1,1 };
    glGenVertexArrays(1, &s_vao); glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo); glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);

    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glUseProgram(s_prog);
    x_glUniform1i(uScaf, 0);
    glUseProgram(0);
    s_state = 1;
    slog("scaffold: GL ready");
}

/* Stamp one opaque-mask pixel run helper */
static void stamp_px(int bx, int by, unsigned char depth)
{
    if (bx < 0 || by < 0 || bx >= s_bw || by >= s_bh) return;
    s_buf[(size_t)by * s_bw + bx] = depth;
}

/* Stamp a GAF frame's opaque silhouette at (x0,y0) = top-left in buffer coords.
   Handles raw (Compressed=0) and TA-RLE (Compressed=1) colour planes. Returns
   0 if the plane was unreadable (caller falls back to a footprint stamp). */
static int stamp_gaf(const unsigned char* g, int x0, int y0, unsigned char depth)
{
    int w  = *(const unsigned short*)(g + GF_WIDTH);
    int h  = *(const unsigned short*)(g + GF_HEIGHT);
    unsigned char ck   = *(const unsigned char*)(g + GF_CKEY);
    unsigned char comp = *(const unsigned char*)(g + GF_COMPRESSED);
    const unsigned char* px = *(const unsigned char* const*)(g + GF_PTRCOLOR);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    if (!ptr_ok(px)) return 0;

    if (comp == 0) {
        if (IsBadReadPtr(px, (SIZE_T)w * h)) return 0;
        for (int y = 0; y < h; y++) {
            const unsigned char* row = px + (size_t)y * w;
            for (int x = 0; x < w; x++)
                if (row[x] != ck) stamp_px(x0 + x, y0 + y, depth);
        }
        return 1;
    }
    /* TA GAF RLE: per row u16 byte count, then codes:
       b&1 -> skip (b>>1) transparent px; b&2 -> next byte repeated (b>>2)+1;
       else -> (b>>2)+1 literal bytes follow. */
    const unsigned char* p = px;
    for (int y = 0; y < h; y++) {
        if (IsBadReadPtr(p, 2)) return 0;
        int rowlen = *(const unsigned short*)p;  p += 2;
        if (rowlen < 0 || rowlen > 4096 || IsBadReadPtr(p, rowlen)) return 0;
        const unsigned char* q = p; int x = 0;
        while (q < p + rowlen && x < w) {
            unsigned char b = *q++;
            if (b & 1) x += b >> 1;
            else if (b & 2) {
                int n = (b >> 2) + 1;
                if (q >= p + rowlen) break;
                unsigned char v = *q++;
                if (v != ck) for (int i = 0; i < n && x + i < w; i++)
                                 stamp_px(x0 + x + i, y0 + y, depth);
                x += n;
            } else {
                int n = (b >> 2) + 1;
                for (int i = 0; i < n && q < p + rowlen; i++) {
                    unsigned char v = *q++;
                    if (v != ck && x < w) stamp_px(x0 + x, y0 + y, depth);
                    x++;
                }
            }
        }
        p += rowlen;
    }
    return 1;
}

/* A readable GAFFrame header with sane dims, or NULL. */
static const unsigned char* frame_sane(const unsigned char* g)
{
    if (!ptr_ok(g) || IsBadReadPtr(g, 0x18)) return 0;
    int w = *(const unsigned short*)(g + GF_WIDTH);
    int h = *(const unsigned short*)(g + GF_HEIGHT);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    return g;
}

/* Resolve a feature def's static body GAF frame 0 (engine:
   GAF_SequenceIndex2Frame(*(def+0xAC), 0)). Layout per Phase B RE: anim entry
   +0x00 u16 nframes, +0x28 -> frame table (inline GAFFrame[] stride 0x18; we
   also tolerate a pointer table). NULL if unreadable. */
static const unsigned char* feat_frame0(const char* def)
{
    const char* seq = *(const char* const*)(def + FD_BODYSEQ);
    if (!ptr_ok(seq) || IsBadReadPtr(seq, 0x2C)) return 0;
    int nf = *(const unsigned short*)(seq + SEQ_NFRAMES);
    if (nf <= 0 || nf > 512) return 0;
    const unsigned char* tab = *(const unsigned char* const*)(seq + SEQ_FRAMES);
    const unsigned char* g = frame_sane(tab);            /* inline frame 0 */
    if (!g && tab && !IsBadReadPtr(tab, 4))              /* or a pointer table */
        g = frame_sane(*(const unsigned char* const*)tab);
    return g;
}

/* One-shot whole-map feature census: where are the TALL features? Logs each
   def in use (height, footprint, count, first anchor in world px) so the
   session can steer the camera to a tall one for the G12a occlusion proof. */
static int s_census = 0;
static void census(const char* fmap, const char* fdef, int mapW, int mapH)
{
    static unsigned short cnt[1024];
    static int firstx[1024], firsty[1024];
    memset(cnt, 0, sizeof cnt);
    int anchors = 0;
    for (int row = 0; row < mapH; row++) {
        const char* trow = fmap + (size_t)row * mapW * FEAT_STRIDE;
        if (IsBadReadPtr(trow, (SIZE_T)mapW * FEAT_STRIDE)) return;
        for (int col = 0; col < mapW; col++) {
            unsigned idx = *(const unsigned short*)(trow + col * FEAT_STRIDE + FT_DEFIDX);
            if (idx >= 0xFFFB || idx >= 1024) continue;
            if (!cnt[idx]) { firstx[idx] = col * 16; firsty[idx] = row * 16; }
            if (cnt[idx] < 0xFFFF) cnt[idx]++;
            anchors++;
        }
    }
    { char b[96]; _snprintf(b, sizeof b, "census: %d feature anchors on %dx%d map",
                            anchors, mapW, mapH); slog(b); }
    for (int i = 0; i < 1024; i++) {
        if (!cnt[i]) continue;
        const char* def = fdef + (size_t)i * FD_STRIDE;
        if (IsBadReadPtr(def, FD_STRIDE)) continue;
        int h = *(const unsigned char*)(def + FD_HEIGHT);
        const char* nm = "?";
        const char* np = *(const char* const*)(def + 0x00);
        if (ptr_ok(np) && !IsBadReadPtr(np, 16) && np[0] >= 0x20 && np[0] < 0x7F) nm = np;
        char b[200]; _snprintf(b, sizeof b,
            "census: def=%d h=%d foot=%dx%d n=%d %s first=(%d,%d)",
            i, h,
            *(const short*)(def + FD_FOOTX),
            *(const short*)(def + FD_FOOTZ),
            cnt[i], h >= 10 ? "TALL" : "flat", firstx[i], firsty[i]);
        slog(b);
        (void)nm;
        if (h >= 10) {                       /* raw window for the offset question */
            const unsigned char* d8 = (const unsigned char*)def;
            _snprintf(b, sizeof b,
                "census: def=%d raw90=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X seq=%p anim=%p",
                i, d8[0x90],d8[0x91],d8[0x92],d8[0x93], d8[0x94],d8[0x95],d8[0x96],d8[0x97],
                d8[0x98],d8[0x99],d8[0x9A],d8[0x9B], d8[0x9C],d8[0x9D],d8[0x9E],d8[0x9F],
                *(void* const*)(def + FD_BODYSEQ), *(void* const*)(def + 0xCC));
            slog(b);
        }
    }
}

void tagpu_scaffold_frame(const TAGPU_FRAME* f)
{
    if (s_state == 2) return;
    if (s_armed < 0 || (f->frame_counter % 30) == 0)
        s_armed = GetFileAttributesA("tagpu_scaffold.on") != INVALID_FILE_ATTRIBUTES;
    if (!s_armed) return;
    if (s_state == 0) init_gl();
    if (s_state != 1) return;
    serr("s-entry");

    char* ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) return;

    /* live view geometry — the Phase D rule: no constants */
    int vpL, vpT, vw, vh;
    tagpu_vpwide_true_rect(ta, &vpL, &vpT, &vw, &vh);   /* TRUE, not the field */
    int eyeX = *(int*)(ta + OFF_EYEX), eyeY = *(int*)(ta + OFF_EYEY);
    int mapW = *(int*)(ta + OFF_MAP_W16), mapH = *(int*)(ta + OFF_MAP_H16);
    int nCols = *(int*)(ta + OFF_SWEEP_C), nRows = *(int*)(ta + OFF_SWEEP_R);
    const char* fmap = *(const char* const*)(ta + OFF_FEATMAP);
    const char* fdef = *(const char* const*)(ta + OFF_FEATDEF);
    if ((f->frame_counter % 300) == 0) {              /* gate trace */
        char b[192]; _snprintf(b, sizeof b,
            "scaffold GATES: vp=(%d,%d) view=%dx%d map16=%dx%d sweep=%dx%d fmap=%p fdef=%p eye=(%d,%d)",
            vpL, vpT, vw, vh, mapW, mapH, nCols, nRows,
            (const void*)fmap, (const void*)fdef, eyeX, eyeY);
        slog(b);
    }
    if (vw < 64 || vh < 64 || vw > 4096 || vh > 4096) return;
    if (mapW <= 0 || mapH <= 0 || mapW > 4096 || mapH > 4096) return;
    if (nCols <= 0 || nRows <= 0 || nCols > 512 || nRows > 512) return;
    if (!ptr_ok(fmap) || !ptr_ok(fdef)) return;

    if (vw != s_bw || vh != s_bh) {
        free(s_buf);
        s_buf = (unsigned char*)malloc((size_t)vw * vh);
        s_bw = vw; s_bh = vh;
        { char b[96]; _snprintf(b, sizeof b,
            "scaffold: buffer %dx%d vp=(%d,%d) sweep=%dx%d", vw, vh, vpL, vpT, nCols, nRows);
          slog(b); }
    }
    if (!s_buf) return;
    if (!s_census) { s_census = 1; census(fmap, fdef, mapW, mapH); }
    memset(s_buf, 0, (size_t)vw * vh);

    /* ---- walk the engine's own sweep rect and stamp tall features ---- */
    int r0 = (eyeY >> 4) - 16;
    s_lastR0 = r0; s_lastRows = nRows; s_lastFrame = f->frame_counter;
    int c0 = (eyeX >> 4) - 10;
    int tall = 0, flat = 0, gafFail = 0;
    for (int r = 0; r < nRows; r++) {
        int row = r0 + r;
        if (row < 0 || row >= mapH) continue;
        for (int c = 0; c < nCols; c++) {
            int col = c0 + c;
            if (col < 0 || col >= mapW) continue;
            const char* tile = fmap + ((size_t)row * mapW + col) * FEAT_STRIDE;
            unsigned idx = *(const unsigned short*)(tile + FT_DEFIDX);
            if (idx >= 0xFFFB) continue;                  /* no anchor here */
            const char* def = fdef + (size_t)idx * FD_STRIDE;
            if (*(const unsigned char*)(def + FD_HEIGHT) < 10) { flat++; continue; }
            tall++;

            unsigned char depth = (unsigned char)(r * 4 + 3 > 251 ? 251 : r * 4 + 3);

            /* engine projection (terrain-depth §3.4): anchor + footprint centre,
               height-corrected by the 2x2 corner-tile average */
            int fx = *(const short*)(def + FD_FOOTX);
            int fz = *(const short*)(def + FD_FOOTZ);
            if (fx < 0 || fx > 16) fx = 1;   /* garbage guard (defs 14+ read wild; */
            if (fz < 0 || fz > 16) fz = 1;   /* raw bytes logged by census)        */
            int h00 = *(const unsigned char*)(tile + FT_HEIGHT), h01 = h00, h10 = h00, h11 = h00;
            if (col + 1 < mapW) h01 = *(const unsigned char*)(tile + FEAT_STRIDE + FT_HEIGHT);
            if (row + 1 < mapH) {
                const char* t2 = fmap + ((size_t)(row + 1) * mapW + col) * FEAT_STRIDE;
                h10 = *(const unsigned char*)(t2 + FT_HEIGHT);
                if (col + 1 < mapW) h11 = *(const unsigned char*)(t2 + FEAT_STRIDE + FT_HEIGHT);
            }
            int sx = col * 16 + fx * 8 - eyeX;            /* buffer coords (no vpL) */
            int sy = row * 16 + fz * 8 - eyeY - (h00 + h01 + h10 + h11) / 8;

            const unsigned char* g = feat_frame0(def);
            int stamped = 0;
            if (g && !IsBadReadPtr(g, 0x18)) {
                int hx = *(const short*)(g + GF_HOTX);
                int hy = *(const short*)(g + GF_HOTY);
                stamped = stamp_gaf(g, sx - hx, sy - hy, depth);
            }
            if (!stamped) {                                /* footprint fallback */
                gafFail++;
                int px0 = col * 16 - eyeX, py1 = row * 16 - eyeY;
                int hgt = *(const unsigned char*)(def + FD_HEIGHT);
                int ya = py1 - hgt / 2 - fz * 16, yb = py1 + fz * 16;
                int xa = px0, xb = px0 + fx * 16;
                if (ya < 0) ya = 0; if (yb > s_bh) yb = s_bh;   /* clip BEFORE looping */
                if (xa < 0) xa = 0; if (xb > s_bw) xb = s_bw;
                for (int y = ya; y < yb; y++)
                    for (int x = xa; x < xb; x++)
                        stamp_px(x, y, depth);
            }
        }
    }

    /* ---- per-unit occlusion prediction (logged; the G12a exit check) ---- */
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    int logNow = (f->frame_counter % 300) == 0;
    if (ptr_ok(beg) && ptr_ok(end) && end > beg &&
        (size_t)(end - beg) <= (size_t)UNIT_STRIDE * 20000) {
        int uidx = 0;
        for (char* u = beg + UNIT_STRIDE; u < end; u += UNIT_STRIDE) {
            unsigned st = *(unsigned*)(u + U_STATE);
            if (!(st & 0x10000000u) || (st & 0x4000u)) continue;
            uidx++;
            if ((st & 3) != 1) continue;                  /* airborne: never occluded */
            char* o3 = *(char**)(u + U_OBJ3DO);
            if (!ptr_ok(o3)) continue;
            const unsigned char* cf = *(const unsigned char* const*)(o3 + O3_COMPOSITE);
            if (!ptr_ok(cf) || IsBadReadPtr(cf, 0x18)) continue;
            int cw = *(const unsigned short*)(cf + GF_WIDTH);
            int chh = *(const unsigned short*)(cf + GF_HEIGHT);
            int hx = *(const short*)(cf + GF_HOTX), hy = *(const short*)(cf + GF_HOTY);
            if (cw <= 0 || chh <= 0 || cw > 1280 || chh > 1280) continue;
            short wx = *(short*)(u + U_XPOS), wz = *(short*)(u + U_ZPOS), wy = *(short*)(u + U_YPOS);
            int relU = (wy >> 4) - r0;
            int Bu = relU * 4 + 1;                        /* units before features in-row */
            int bx0 = wx - eyeX - hx, by0 = wy - wz / 2 - eyeY - hy;
            int occ = 0, tot = 0;
            for (int y = by0; y < by0 + chh; y += 2) {
                if (y < 0 || y >= s_bh) continue;
                for (int x = bx0; x < bx0 + cw; x += 2) {
                    if (x < 0 || x >= s_bw) continue;
                    tot++;
                    if (s_buf[(size_t)y * s_bw + x] > Bu) occ++;
                }
            }
            if (logNow && tot > 0 && occ > 0) {
                char* def2 = *(char**)(u + U_TYPE);
                const char* nm = ptr_ok(def2) ? def2 + 0x20 : "?";
                char b[160]; _snprintf(b, sizeof b,
                    "scaffold: u%03d %-12.12s row=%d occl=%d%% (%d/%d px)",
                    uidx, nm, wy >> 4, occ * 100 / tot, occ, tot);
                slog(b);
            }
        }
    }

    if (logNow) {
        char b[128]; _snprintf(b, sizeof b,
            "scaffold: swept %dx%d tall=%d flat=%d gafFallback=%d eye=(%d,%d)",
            nCols, nRows, tall, flat, gafFail, eyeX, eyeY);
        slog(b);
    }

    /* ---- upload + draw the debug overlay quad over the viewport ---- */
    x_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (vw != s_texW || vh != s_texH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, vw, vh, 0, GL_RED, GL_UNSIGNED_BYTE, s_buf);
        s_texW = vw; s_texH = vh;
    } else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vw, vh, GL_RED, GL_UNSIGNED_BYTE, s_buf);
    serr("s-upload");

    int gw = f->game_width  > 0 ? f->game_width  : vpL + vw;
    int gh = f->game_height > 0 ? f->game_height : vpT + vh;
    float x0 = (float)vpL        / gw * 2.f - 1.f;
    float x1 = (float)(vpL + vw) / gw * 2.f - 1.f;
    float y0 = 1.f - (float)vpT        / gh * 2.f;   /* NDC top    */
    float y1 = 1.f - (float)(vpT + vh) / gh * 2.f;   /* NDC bottom */

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glEnable(GL_BLEND);
    x_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* quad vertex p: p.y=0 -> uv row 0 = buffer top -> screen top (y0) */
    x_glUniform4f(s_uRect, x0, y0, x1, y1);
    x_glUniform1f(s_uRows, (float)nRows);
    x_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    x_glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    serr("s-quad");
}

/* exports for the native pass: this frame's scaffold texture + row encoding */
GLuint tagpu_scaffold_texref(void) { return s_tex; }
int tagpu_scaffold_frameinfo(unsigned frame_counter, int* r0, int* nrows)
{
    if (s_state != 1 || !s_armed) return 0;
    if (frame_counter - s_lastFrame > 2) return 0;   /* stale (not armed/in-game) */
    *r0 = s_lastR0; *nrows = s_lastRows;
    return 1;
}

static void serr(const char* tag)
{
    typedef unsigned (WINAPI* PFNGE)(void);
    static PFNGE pge;
    if (GetFileAttributesA("tagpu_gldbg.on") == INVALID_FILE_ATTRIBUTES) return;
    if (!pge) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) pge = (PFNGE)GetProcAddress(gl, "glGetError");
    }
    if (!pge) return;
    unsigned e = pge();
    if (e) { FILE* fp = fopen("tagpu.log", "a");
             if (fp) { fprintf(fp, "serr %s=%x\n", tag, e); fclose(fp); } }
}
void tagpu_scaffold_glreset(void) { s_state = 0; s_texW = s_texH = 0; }
