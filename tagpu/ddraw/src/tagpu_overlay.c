/* tagpu_overlay.c — G1 GPU overlay, compiled INTO our cnc-ddraw fork.
   Reuses the fork's already-loaded GL entry points; self-loads the few it lacks.
   Draws translucent GL 3.3-core geometry over the live game frame, toggled by a
   sentinel file (tagpu_overlay.off). No separate module => no runtime LoadLibrary,
   which is what destabilised TA under wine. */

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "opengl_utils.h"   /* the fork's extern GL function pointers   */
#include "tagpu_overlay.h"
#include "tagpu_tracer.h"
#include "tagpu_suppress.h"
#include "tagpu_render3do.h"
#include "tagpu_owndraw.h"
#include "tagpu_scaffold.h"
#include "tagpu_input.h"
#include "tagpu_native.h"
#include "tagpu_peek.h"
#include "tagpu_ui.h"
#include "tagpu_cat.h"

/* GL entry points the fork does not already expose — load once ourselves. */
typedef void (APIENTRY *PFN_UNIFORM4F)(GLint,GLfloat,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_UNIFORM1F)(GLint,GLfloat);
typedef void (APIENTRY *PFN_GETPROGINFO)(GLuint,GLsizei,GLsizei*,GLchar*);
typedef void (APIENTRY *PFN_DISABLE)(GLenum);
typedef void (APIENTRY *PFN_BLENDFUNC)(GLenum,GLenum);
typedef void (APIENTRY *PFN_READPIXELS)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);

static PFN_UNIFORM4F   x_glUniform4f;
static PFN_UNIFORM2F   x_glUniform2f;
static PFN_UNIFORM1F   x_glUniform1f;
static PFN_GETPROGINFO x_glGetProgramInfoLog;
static PFN_DISABLE     x_glDisable;
static PFN_BLENDFUNC   x_glBlendFunc;
static PFN_READPIXELS  x_glReadPixels;

static int   s_state = 0;   /* 0=unloaded 1=ready 2=failed */
static GLuint s_prog, s_vao, s_vbo, s_ebo;
static GLint  s_uAngle, s_uOffset, s_uScale, s_uColor;

static void olog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) {  /* GL 1.1 funcs may not come from wglGetProcAddress */
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) p = (void*)GetProcAddress(gl, n);
    }
    return p;
}

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "uniform float uAngle; uniform vec2 uOffset; uniform vec2 uScale;\n"
    "void main(){ float c=cos(uAngle), s=sin(uAngle);\n"
    "  vec2 r=vec2(pos.x*c-pos.y*s, pos.x*s+pos.y*c);\n"
    "  gl_Position=vec4(r*uScale+uOffset,0.0,1.0); }\n";
static const char* FS =
    "#version 330 core\n"
    "out vec4 frag; uniform vec4 uColor; void main(){ frag=uColor; }\n";

static GLuint mkshader(GLenum t, const char* src)
{
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok=0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok){ char log[512]; glGetShaderInfoLog(s,sizeof log,NULL,log);
              olog("tagpu: shader compile FAILED:"); olog(log); s_state=2; }
    return s;
}

static void init_overlay(void)
{
    { char b[320]; _snprintf(b, sizeof b,
        "init entry: xwgl=%p glCreateShader=%p glShaderSource=%p glUseProgram=%p "
        "glGenVertexArrays=%p glBindVertexArray=%p glGenBuffers=%p glDrawElements=%p "
        "glGetUniformLocation=%p glEnable=%p",
        (void*)xwglGetProcAddress,(void*)glCreateShader,(void*)glShaderSource,
        (void*)glUseProgram,(void*)glGenVertexArrays,(void*)glBindVertexArray,
        (void*)glGenBuffers,(void*)glDrawElements,(void*)glGetUniformLocation,
        (void*)glEnable);
      olog(b); }

    x_glUniform4f        = (PFN_UNIFORM4F)  getgl("glUniform4f");
    x_glUniform2f        = (PFN_UNIFORM2F)  getgl("glUniform2f");
    x_glUniform1f        = (PFN_UNIFORM1F)  getgl("glUniform1f");
    x_glGetProgramInfoLog= (PFN_GETPROGINFO)getgl("glGetProgramInfoLog");
    x_glDisable          = (PFN_DISABLE)    getgl("glDisable");
    x_glBlendFunc        = (PFN_BLENDFUNC)  getgl("glBlendFunc");
    x_glReadPixels       = (PFN_READPIXELS) getgl("glReadPixels");
    if (!x_glUniform4f||!x_glUniform2f||!x_glUniform1f||!x_glDisable||!x_glBlendFunc){
        olog("tagpu: missing a GL uniform/blend proc"); s_state=2; return; }

    GLuint vs=mkshader(GL_VERTEX_SHADER,VS), fs=mkshader(GL_FRAGMENT_SHADER,FS);
    if (s_state==2) return;
    s_prog=glCreateProgram(); glAttachShader(s_prog,vs); glAttachShader(s_prog,fs);
    glLinkProgram(s_prog);
    GLint ok=0; glGetProgramiv(s_prog,GL_LINK_STATUS,&ok);
    if(!ok){ char log[512]; if(x_glGetProgramInfoLog) x_glGetProgramInfoLog(s_prog,sizeof log,NULL,log);
             olog("tagpu: link FAILED:"); olog(log); s_state=2; return; }
    glDeleteShader(vs); glDeleteShader(fs);
    s_uAngle =glGetUniformLocation(s_prog,"uAngle");
    s_uOffset=glGetUniformLocation(s_prog,"uOffset");
    s_uScale =glGetUniformLocation(s_prog,"uScale");
    s_uColor =glGetUniformLocation(s_prog,"uColor");

    const float verts[]={0.0f,0.16f, -0.14f,-0.10f, 0.14f,-0.10f};
    const unsigned short idx[]={0,1,2};
    glGenVertexArrays(1,&s_vao); glBindVertexArray(s_vao);
    glGenBuffers(1,&s_vbo); glBindBuffer(GL_ARRAY_BUFFER,s_vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof verts,verts,GL_STATIC_DRAW);
    glGenBuffers(1,&s_ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,s_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof idx,idx,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,2*sizeof(float),(void*)0);
    glBindVertexArray(0);
    s_state=1;
    olog("tagpu: overlay ready (built into fork, GL 3.3 core)");
}

/* Capture the composited GL framebuffer (game + our overlay) to a PPM next to the exe.
   This is the review path for anything drawn in GL — the cnc-ddraw surface screenshot
   only sees the 8bpp game surface, which predates our overlay. */
void tagpu_overlay_capture(const TAGPU_FRAME* f)
{
    if (s_state != 1 || !x_glReadPixels || !f) return;
    int w = f->win_width, h = f->win_height;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    /* mode changes can report an off-by-one drawable (e.g. 2161) — an
       out-of-bounds read makes glReadPixels fail wholesale (all black) */
    w &= ~3; h &= ~7;
    unsigned char* buf = (unsigned char*)malloc((size_t)w * h * 3);
    if (!buf) return;
    x_glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);
    FILE* fp = fopen("tagpu_gl.ppm", "wb");
    if (fp) {
        fprintf(fp, "P6\n%d %d\n255\n", w, h);
        /* glReadPixels is bottom-up; write rows top-down for a normal image */
        for (int y = h - 1; y >= 0; --y)
            fwrite(buf + (size_t)y * w * 3, 1, (size_t)w * 3, fp);
        fclose(fp);
        olog("tagpu: GL framebuffer captured to tagpu_gl.ppm");
    }
    free(buf);
}

/* ---- G2: read live engine state and draw a marker on every unit ----
   Addresses binary-confirmed against stock TA 3.1 (see wiki: field-notes / roadmap).
   All reads are read-only; guards keep us safe at the menu (no game struct yet). */
#define TA_MAINPP     0x00511DE8u  /* TAdynmemStruct** */
#define OFF_BEGIN     0x14357      /* UnitStruct* BeginUnitsArray_p */
#define OFF_END       0x1435B      /* UnitStruct* EndOfUnitsArray_p */
#define OFF_EYEX      0x1431F      /* int scroll origin X */
#define OFF_EYEY      0x14323      /* int scroll origin Y */
#define OFF_LOCALPID  0x2A42       /* char local player slot */
#define OFF_MOUSE     0x2C76       /* POINT CurtMousePostion (game-space x,y) */
#define UNIT_STRIDE   0x118
#define U_STATE       0x110        /* uint UnitStateMask: alive 0x10000000, skip 0x4000 */
#define U_XPOS        0x6C         /* short world X */
#define U_ZPOS        0x70         /* short altitude */
#define U_YPOS        0x74         /* short world Y (map depth) */
#define U_OWNER       0xFF         /* byte owner slot */


/* ---- G6 probe: dump the posed 3DO piece tree of the first alive unit ----
   Chain (binary-verified 2026-08-31, see wiki unit-3do-bridge): unit+0x9E ->
   Object3doStruct; pieces inline at obj3do+0x22, stride 0x36, index == COB
   piece number. Read-only; logs one unit per call. */
#define U_OBJ3DO      0x9E     /* Object3doStruct* */
#define O3_NUMPARTS   0x00     /* u16 piece count            */
#define O3_THISUNIT   0x0C     /* UnitStruct* back-pointer   */
#define O3_BODYTURN   0x18     /* u16[3] cached unit turn    */
#define O3_BASEOBJ    0x1E     /* PrimitiveStruct* (== obj3do+0x22) */
#define O3_PRIM0      0x22     /* inline PrimitiveStruct[]   */
#define PRIM_STRIDE   0x36
#define P_NODE        0x00     /* Model3DONode*              */
#define P_POS         0x04     /* i32[3] 16.16: x, y(up), z  */
#define P_TURN        0x10     /* u16[3]: x-pitch,y-yaw,z-roll (65536=360deg) */
#define P_ORIGIN      0x16     /* i32[3] 16.16 posed origin, model space */
#define P_VBUF        0x22     /* i32* posed verts (VertexCount*3)       */
#define P_FLAGS       0x28     /* bit0 = visible             */
#define N_VCOUNT      0x04     /* Model3DONode.VertexCount   */
#define N_NAME        0x1C     /* Model3DONode.pNameStr      */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void probe_unit_model(void)
{
    char* ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) return;
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    if (!ptr_ok(beg) || !ptr_ok(end) || end <= beg) return;

    for (char* u = beg + UNIT_STRIDE; u < end; u += UNIT_STRIDE) {
        unsigned st = *(unsigned*)(u + U_STATE);
        if (!(st & 0x10000000u) || (st & 0x4000u)) continue;

        char* o3 = *(char**)(u + U_OBJ3DO);
        if (!ptr_ok(o3)) { olog("probe: unit has no Object3do"); return; }
        if (*(char**)(o3 + O3_THISUNIT) != u) { olog("probe: ThisUnit mismatch!"); return; }

        int nparts = *(unsigned short*)(o3 + O3_NUMPARTS);
        if (nparts <= 0 || nparts > 64) { olog("probe: bad NumParts"); return; }

        {
        char b[256]; int i;
        _snprintf(b, sizeof b, "probe: unit=%p obj3do=%p base=%p parts=%d bodyTurn=(%u,%u,%u)",
                  u, o3, *(void**)(o3 + O3_BASEOBJ), nparts,
                  *(unsigned short*)(o3 + O3_BODYTURN),
                  *(unsigned short*)(o3 + O3_BODYTURN + 2),
                  *(unsigned short*)(o3 + O3_BODYTURN + 4));
        olog(b);

        for (i = 0; i < nparts; i++) {
            char* pr = o3 + O3_PRIM0 + i * PRIM_STRIDE;
            char* nd = *(char**)(pr + P_NODE);
            const char* nm = "?";
            int vc = -1;
            if (ptr_ok(nd)) {
                char* ns = *(char**)(nd + N_NAME);
                if (ptr_ok(ns) && ns[0] >= 0x20 && ns[0] < 0x7F) nm = ns;
                vc = *(int*)(nd + N_VCOUNT);
            }
            _snprintf(b, sizeof b,
                "  piece %2d '%.12s' vis=%d verts=%d pos=(%d,%d,%d) turn=(%u,%u,%u) org=(%d,%d,%d) vbuf=%p",
                i, nm, *(unsigned char*)(pr + P_FLAGS) & 1, vc,
                *(int*)(pr + P_POS)      >> 16, *(int*)(pr + P_POS + 4)  >> 16, *(int*)(pr + P_POS + 8) >> 16,
                *(unsigned short*)(pr + P_TURN), *(unsigned short*)(pr + P_TURN + 2), *(unsigned short*)(pr + P_TURN + 4),
                *(int*)(pr + P_ORIGIN)   >> 16, *(int*)(pr + P_ORIGIN + 4) >> 16, *(int*)(pr + P_ORIGIN + 8) >> 16,
                *(void**)(pr + P_VBUF));
            olog(b);
        }
        }
        return;   /* one unit per call is enough */
    }
}

/* ---- G6 write-back proof: repaint TA's own per-unit composite buffer with our
   content, in place, so the engine's blit (0x459200) stamps it onto the frame.
   Composite buffer (binary-verified, wiki composite-buffer): GAFFrame at
   Object3do+0x10 = 0x18 header + colour plane (w*h, 8bpp, top-down, stride=W,
   index 1 = ColorKey/transparent) + depth plane (w*h, larger=nearer).
   We do NOT suppress: we let the engine build the buffer, then overwrite the
   colour plane every frame; the next frame's blit shows our pattern. Read the
   authoritative sim; write only into the engine-owned scratch composite. */
#define U_TYPE        0x92     /* UnitDefStruct* */
#define UDEF_NAME     0x00     /* char Name[0x20] */
#define O3_COMPOSITE  0x10     /* GAFFrame* (persistent per-unit composite) */
#define GF_WIDTH      0x00     /* u16 */
#define GF_HEIGHT     0x02     /* u16 */
#define GF_COLORKEY   0x08     /* u8, ==1 */
#define GF_COMPRESSED 0x09     /* u8, ==0 */
#define GF_SUBFRAMES  0x0A     /* u8, ==0 */
#define GF_PTRCOLOR   0x10     /* u8* colour plane */
#define GF_PTRDEPTH   0x14     /* u8* depth plane */

static int   s_wb_state = 0;              /* 0=unchecked 1=armed 2=off */
static char  s_wb_type[32] = "armcom";    /* target unit-type name */

static void wb_init(void)
{
    HANDLE h = CreateFileA("tagpu_writeback.on", GENERIC_READ, FILE_SHARE_READ,
                           0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { s_wb_state = 2; return; }
    char buf[64]; DWORD n = 0;
    if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
        buf[n] = 0;
        int i = 0; while (buf[i] && buf[i] > ' ') i++;   /* first token */
        if (i > 0 && i < (int)sizeof s_wb_type) { buf[i] = 0; lstrcpyA(s_wb_type, buf); }
    }
    CloseHandle(h);
    s_wb_state = 1;
    { char b[96]; _snprintf(b, sizeof b, "writeback: ARMED target=\"%s\"", s_wb_type); olog(b); }
}

static int name_ieq(const char* a, const char* b)
{
    int i;
    for (i = 0; i < 31 && a[i] && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return a[i] == 0 || a[i] <= ' ';   /* def name is NUL-padded; match its full length */
}

static void writeback_paint(const TAGPU_FRAME* f)
{
    if (s_wb_state == 0) wb_init();
    if (s_wb_state != 1) return;

    char* ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) return;
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    if (!ptr_ok(beg) || !ptr_ok(end) || end <= beg) return;
    if ((size_t)(end - beg) > (size_t)UNIT_STRIDE * 20000) return;

    int painted = 0;
    for (char* u = beg + UNIT_STRIDE; u < end; u += UNIT_STRIDE) {
        unsigned st = *(unsigned*)(u + U_STATE);
        if (!(st & 0x10000000) || (st & 0x4000)) continue;

        char* def = *(char**)(u + U_TYPE);
        char* o3  = *(char**)(u + U_OBJ3DO);
        if (!ptr_ok(def) || !ptr_ok(o3)) continue;

        /* One-shot diagnostic: dump the first alive unit's identity + composite state
           so a match/format failure is debuggable from the log. */
        static int diag = 0;
        if (!diag) { diag = 1;
            char* fr = *(char**)(o3 + O3_COMPOSITE);
            char b[224];
            _snprintf(b, sizeof b,
                "writeback DIAG: name@00=\"%.16s\" unitname@20=\"%.16s\" objname@80=\"%.16s\" comp=%p%s",
                def + 0x00, def + 0x20, def + 0x80, (void*)fr,
                ptr_ok(fr) ? "" : " (invalid)");
            olog(b);
            if (ptr_ok(fr)) { char b2[160]; _snprintf(b2, sizeof b2,
                "writeback DIAG: W=%d H=%d colorkey=%u comp=%u sub=%u pcol=%p pdep=%p",
                *(unsigned short*)(fr + GF_WIDTH), *(unsigned short*)(fr + GF_HEIGHT),
                *(unsigned char*)(fr + GF_COLORKEY), *(unsigned char*)(fr + GF_COMPRESSED),
                *(unsigned char*)(fr + GF_SUBFRAMES),
                *(void**)(fr + GF_PTRCOLOR), *(void**)(fr + GF_PTRDEPTH)); olog(b2); }
        }

        /* "armcom" may live in Name@0x00, UnitName@0x20, or ObjectName@0x80;
           token "all" renders every unit the engine has a composite for. */
        if (!name_ieq("all", s_wb_type) &&
            !name_ieq(def + 0x00, s_wb_type) &&
            !name_ieq(def + 0x20, s_wb_type) &&
            !name_ieq(def + 0x80, s_wb_type)) continue;

        char* frame = *(char**)(o3 + O3_COMPOSITE);
        if (!ptr_ok(frame)) continue;
        /* G12b: natively-rendered units leave the composite path — keep the
           plane ColorKey-empty so the engine blit shows nothing */
        if (tagpu_native_owns_unit(u)) {
            extern void tagpu_r3dcache_wipe(unsigned int frame);
            tagpu_r3dcache_wipe((unsigned int)(size_t)frame);
            continue;
        }
        if (*(unsigned char*)(frame + GF_COMPRESSED) != 0) continue;
        if (*(unsigned char*)(frame + GF_SUBFRAMES)  != 0) continue;

        int W = *(unsigned short*)(frame + GF_WIDTH);
        int H = *(unsigned short*)(frame + GF_HEIGHT);
        if (W <= 0 || H <= 0 || W > 1280 || H > 1280) continue;

        unsigned char* col = *(unsigned char**)(frame + GF_PTRCOLOR);
        if (!ptr_ok(col)) continue;

        /* Phase B: real GPU render of the posed 3DO into the colour plane.
           Falls back to the G6 proof gradient if the GL path is unavailable,
           so a render failure is visually obvious (gradient) in the log. */
        if (!tagpu_render3do(f, u, o3, frame)) {
            int shift = (int)(f->frame_counter / 2);
            int x, y;
            for (y = 0; y < H; y++) {
                unsigned char* row = col + (size_t)y * W;   /* stride = W, top-down */
                for (x = 0; x < W; x++)
                    row[x] = (unsigned char)(2 + ((x + y + shift) % 252));  /* 2..253 */
            }
        }
        painted++;
    }

    static unsigned last = 0;
    if (f->frame_counter - last >= 60) { last = f->frame_counter;
        char b[96]; _snprintf(b, sizeof b, "writeback: painted=%d units (type=%s)", painted, s_wb_type);
        olog(b); }
}


/* Draw a marker at a game-space (sx,sy) using the current program/VAO/blend state. */
static void marker(const TAGPU_FRAME* f, int sx, int sy, float r, float g, float b, float a, float scl)
{
    int gw = f->game_width  > 0 ? f->game_width  : 640;
    int gh = f->game_height > 0 ? f->game_height : 480;
    float nx = (float)sx / (float)gw * 2.0f - 1.0f;
    float ny = 1.0f - (float)sy / (float)gh * 2.0f;
    if (nx < -1.05f || nx > 1.05f || ny < -1.05f || ny > 1.05f) return;
    x_glUniform4f(s_uColor, r, g, b, a);
    x_glUniform1f(s_uAngle, 0.0f);
    x_glUniform2f(s_uScale, scl * 0.68f, scl);   /* 0.68 ~ 480/640 aspect for a square marker */
    x_glUniform2f(s_uOffset, nx, ny);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0);
}

/* Read TA's own mouse position from memory and draw a marker on it. Proves the
   read->project->draw pipeline with live engine state even at the menu (no game needed).
   Also logs it, which lets us calibrate the OS-pointer -> game-coord mapping exactly. */

/* Yellow micro-marker on each posed piece origin of a unit: visual proof of the
   unit->Object3do->PrimitiveStruct chain (wiki: unit-3do-bridge). Projection is
   the engine's own per-vertex rule: sx=+x, sy=-z-y/2 (Render_DrawSpriteGroupUnit). */
static void draw_piece_markers(const TAGPU_FRAME* f, char* u, int sx, int sy)
{
    char* o3 = *(char**)(u + U_OBJ3DO);
    if (!ptr_ok(o3)) return;
    int nparts = *(unsigned short*)(o3 + O3_NUMPARTS);
    if (nparts <= 0 || nparts > 64) return;
    for (int i = 0; i < nparts; i++) {
        char* pr = o3 + O3_PRIM0 + i * PRIM_STRIDE;
        if (!(*(unsigned char*)(pr + P_FLAGS) & 1)) continue;   /* COB-hidden piece */
        int ox = *(int*)(pr + P_ORIGIN)     >> 16;
        int oy = *(int*)(pr + P_ORIGIN + 4) >> 16;              /* model y = up   */
        int oz = *(int*)(pr + P_ORIGIN + 8) >> 16;              /* model z = fwd  */
        marker(f, sx + ox, sy - oz - oy / 2, 1.0f, 0.9f, 0.1f, 0.95f, 0.05f);
    }
}

static void draw_mouse(const TAGPU_FRAME* f, char* ta)
{
    int mx = *(int*)(ta + OFF_MOUSE);
    int my = *(int*)(ta + OFF_MOUSE + 4);
    if (mx < -50 || mx > 4000 || my < -50 || my > 4000) return;
    marker(f, mx, my, 1.0f, 1.0f, 0.1f, 0.95f, 0.10f);   /* yellow dot at TA's cursor */
    static unsigned last = 0;
    if (f->frame_counter - last >= 15) { last = f->frame_counter;
        char b[96]; _snprintf(b, sizeof b, "mouse: game=(%d,%d)", mx, my); olog(b); }
}

static int draw_units(const TAGPU_FRAME* f)
{
    char* ta = *(char**)TA_MAINPP;
    if ((size_t)ta < 0x600000u) return -1;          /* no game struct yet (menu) */
    draw_mouse(f, ta);                               /* always: proves live-state read */
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    if ((size_t)beg < 0x600000u || (size_t)end < 0x600000u || end <= beg) return -1;
    if ((size_t)(end - beg) > (size_t)UNIT_STRIDE * 20000) return -1; /* sanity */

    int eyeX = *(int*)(ta + OFF_EYEX);
    int eyeY = *(int*)(ta + OFF_EYEY);
    unsigned char me = *(unsigned char*)(ta + OFF_LOCALPID);
    int gw = f->game_width  > 0 ? f->game_width  : 640;
    int gh = f->game_height > 0 ? f->game_height : 480;

    int alive = 0, drawn = 0;
    for (char* u = beg + UNIT_STRIDE; u < end; u += UNIT_STRIDE) {
        unsigned st = *(unsigned*)(u + U_STATE);
        if (!(st & 0x10000000) || (st & 0x4000)) continue;
        alive++;
        short wx = *(short*)(u + U_XPOS), wz = *(short*)(u + U_ZPOS), wy = *(short*)(u + U_YPOS);
        int sx = wx - eyeX + 128;
        int sy = wy - (wz / 2) - eyeY + 32;
        /* full roster dump every ~10s: index, type, owner, position — makes
           headless camera steering to any specific unit possible */
        if ((f->frame_counter % 300) == 0) {
            char* def = *(char**)(u + 0x92);
            const char* nm = "?";
            if ((size_t)def > 0x600000u && (size_t)def < 0x7FFF0000u) nm = def + 0x20;
            char db[160]; _snprintf(db, sizeof db,
                "  u%03d %-12.12s own=%d world=(%d,%d,%d) screen=(%d,%d) nano=%.2f",
                alive, nm, (int)*(unsigned char*)(u + U_OWNER), wx, wy, wz, sx, sy,
                *(float*)(u + 0x104));   /* build fraction REMAINING (build-state.md) */
            olog(db); }
        float nx = (float)sx / (float)gw * 2.0f - 1.0f;
        float ny = 1.0f - (float)sy / (float)gh * 2.0f;
        if (nx < -1.05f || nx > 1.05f || ny < -1.05f || ny > 1.05f) continue; /* off-screen */
        unsigned char owner = *(unsigned char*)(u + U_OWNER);
        if (owner == me) x_glUniform4f(s_uColor, 0.20f, 1.00f, 0.35f, 0.95f);  /* mine: green */
        else             x_glUniform4f(s_uColor, 1.00f, 0.25f, 0.20f, 0.95f);  /* other: red  */
        x_glUniform1f(s_uAngle, 0.0f);
        x_glUniform2f(s_uScale, 0.13f, 0.19f);      /* ~12px square marker in 640x480 */
        x_glUniform2f(s_uOffset, nx, ny);
        glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0);
        drawn++;
        draw_piece_markers(f, u, sx, sy);
    }
    static unsigned last = 0;
    if (f->frame_counter - last >= 30) { last = f->frame_counter;
        char b[160]; _snprintf(b, sizeof b, "units: alive=%d drawn=%d eye=(%d,%d) me=%d",
                               alive, drawn, eyeX, eyeY, (int)me); olog(b); }
    return drawn;
}

static void oerr(const char* tag)
{
    typedef GLenum (WINAPI* PFNGE)(void);
    static PFNGE pge;
    if (GetFileAttributesA("tagpu_gldbg.on") == INVALID_FILE_ATTRIBUTES) return;
    if (!pge) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (gl) pge = (PFNGE)GetProcAddress(gl, "glGetError");
    }
    if (!pge) return;
    GLenum e = pge();
    if (e != GL_NO_ERROR) {
        FILE* fp = fopen("tagpu.log", "a");
        if (fp) { fprintf(fp, "oerr %s=%x\n", tag, e); fclose(fp); }
    }
}

void tagpu_overlay_draw(const TAGPU_FRAME* f)
{
    if (!f || f->abi!=TAGPU_ABI) return;
    /* in-process input injection (tagpu_keys.txt / tagpu_eye.txt) — must run
       even at the menus and regardless of the overlay's enable state */
    tagpu_input_frame(f);
    /* on-demand memory reads (tagpu_peek.trigger) — no-op unless triggered, and
       must run at the menus too: switch effects land before the first game */
    tagpu_peek_frame(f->frame_counter);
    /* on-demand GUI snapshot (tagpu_ui.trigger) — the read half of `tacli ui`.
       The menus are exactly where it earns its keep, so like peek it must run
       before any game exists. */
    tagpu_ui_frame(f);
    /* on-demand unit/feature catalogues (tagpu_units.trigger,
       tagpu_features.trigger) — validation layer 2 for `tacli scenario`. */
    tagpu_cat_frame(f);
    /* DISPLAY-MODE CHANGES: the fork restarts its render thread with a NEW
       GL context — every GL object id we cached is dead. Detect the context
       change and re-init all GL-owning modules from scratch (without this a
       stale-FBO bind fails and our pass clears the real backbuffer black —
       found during the 1024x768 resolution test). */
    {
        extern void tagpu_native_glreset(void);
        extern void tagpu_scaffold_glreset(void);
        extern void tagpu_r3d_glreset(void);
        typedef HGLRC (WINAPI* PFNWGC)(void);
        static PFNWGC pwgc;
        static HGLRC s_ctx = 0;
        if (!pwgc) {
            HMODULE gl = GetModuleHandleA("opengl32.dll");
            if (gl) pwgc = (PFNWGC)GetProcAddress(gl, "wglGetCurrentContext");
        }
        HGLRC cur = pwgc ? pwgc() : 0;
        if (cur != s_ctx) {
            if (s_ctx) {
                s_state = 0;
                s_wb_state = 0;
                tagpu_native_glreset();
                tagpu_scaffold_glreset();
                tagpu_r3d_glreset();
                olog("tagpu: GL CONTEXT CHANGED - all modules reset");
            }
            s_ctx = cur;
        }
    }
    /* G4 tracer flush: no-op unless the tracer was armed. Runs independently of the
       overlay's own enable state (must precede the tagpu_overlay.off early-return). */
    tagpu_tracer_flush(f->frame_counter);
    /* G5 suppressor flush: no-op unless suppression was armed. */
    tagpu_suppress_flush(f->frame_counter);
    /* Phase B own-the-draw flush: no-op unless armed. */
    tagpu_owndraw_flush(f->frame_counter);
    if (GetFileAttributesA("tagpu_overlay.off")!=INVALID_FILE_ATTRIBUTES) { writeback_paint(f); return; }
    if (s_state==0) init_overlay();
    if (s_state!=1) { writeback_paint(f); return; }

    float t=(float)f->frame_counter;
    float pulse=0.5f+0.5f*(float)sin(t*0.05f);

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glEnable(GL_BLEND);
    x_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* live unit markers (the G2 proof) */
    int units = draw_units(f);
    if ((f->frame_counter % 300) == 61) probe_unit_model();

    /* small corner indicator so we can tell "overlay running" from "no units":
       cyan when we have a game+units, amber pulse when not in a game */
    x_glUniform1f(s_uAngle, t*0.03f);
    x_glUniform2f(s_uScale, 0.11f, 0.14f);
    x_glUniform2f(s_uOffset, -0.9f, 0.86f);
    if (units >= 0) x_glUniform4f(s_uColor, 0.15f,0.85f,1.0f, 0.9f);
    else            x_glUniform4f(s_uColor, 1.0f,0.7f,0.1f, 0.4f+0.4f*pulse);
    glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_SHORT,0);

    x_glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);

    /* G12a: scene-depth scaffold debug overlay (tagpu_scaffold.on). Own GL
       state block; leaves program/VAO at 0. */
    oerr("pre-scaffold");
    tagpu_scaffold_frame(f);
    oerr("scaffold");

    /* G12b: native unit pass (tagpu_native.on) — needs this frame's scaffold */
    tagpu_native_frame(f);
    oerr("native");

    /* G6/Phase B write-back LAST: its FBO pass clobbers the viewport and FBO
       binding (it restores binding 0), which is safe here — nothing after us
       uses GL this frame and the fork re-establishes viewport/program/VAO at
       the top of the next one. */
    writeback_paint(f);
    oerr("writeback");
}
