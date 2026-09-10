/* tagpu_fps.c -- the on-screen frame-rate readout. Contract: tagpu_fps.h.

   THE ATLAS IS A CACHE AND THAT DECIDES THE DESIGN. `tagpu_text_place` keys on
   the WHOLE STRING and packs it onto a shelf that is never freed until the font
   changes -- so a readout that placed "FPS 101", then "FPS 102", would burn one
   permanent slot per distinct number, exhaust MAXSTR within seconds, and then
   start dropping. Worse, it shares that atlas with tagpu_mark's group digits
   and ShowRanges labels, so it would starve them too. This places the eleven
   FIXED strings "FPS" and "0".."9" and emits one quad per character: eleven
   entries, once, for the life of the font. The same reason tagpu_mark caches
   single digits for squads.

   SCREEN SPACE, NOT WORLD SPACE. The marker pass's text is world-anchored --
   its quads carry (wx, wz) for the fog lookup and take the zoom transform -- so
   a readout drawn through it would fade into fog and scale with the camera.
   This has its own two-triangle program in game-frame pixels and nothing else.

   THREADS. Render thread only, from the overlay's present. It reads the font
   the game thread published (tagpu_text_snapshot at hook 8) through
   tagpu_text_frame's per-frame latch, exactly as the marker gather does. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "opengl_utils.h"
#include "tagpu_fps.h"
#include "tagpu_text.h"

#define TRIGGER   "tagpu_fps.on"
#define POLL      30                  /* frames between trigger polls          */
#define WINDOW_MS 500u                /* averaging window                      */
#define MAXCH     16
#define QUADV     6
#define VST       4                   /* x, y, u, v                            */

typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_UNIFORM3F)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_DISABLE)(GLenum);
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_ACTIVETEX  x_glActiveTexture;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_UNIFORM3F  x_glUniform3f;
static PFN_DISABLE    x_glDisable;

/* The fork declares only the entry points its own passes use, so the core-1.1
   ones this needs are resolved by hand -- wglGetProcAddress first, then
   opengl32 itself, which is where a 1.1 symbol actually lives. Same helper as
   tagpu_feat.c's. */
static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

static int    s_state;                /* 0 unbuilt, 1 ready, 2 refused         */
static int    s_on = -1;
static GLuint s_prog, s_vao, s_vbo;
static GLint  s_uFrame, s_uInk;
static float  s_v[MAXCH * QUADV * VST];

/* the averaging window */
static DWORD    s_t0;
static unsigned s_frames;
static int      s_fps = -1;

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"      /* game-frame pixels, y down */
    "layout(location=1) in vec2 aUV;\n"
    "uniform vec2 uFrame;\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "  vUV = aUV;\n"
    "  vec2 n = vec2(aPos.x / uFrame.x * 2.0 - 1.0,\n"
    "                1.0 - aPos.y / uFrame.y * 2.0);\n"
    "  gl_Position = vec4(n, 0.0, 1.0);\n"
    "}\n";

/* The atlas is a 1-bit COVERAGE MASK (tagpu_text.c writes fg=255 over bg=0
   with transparent=0), so the sample is an alpha and never a colour. */
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uAtlas;\n"
    "uniform vec3 uInk;\n"
    "out vec4 oCol;\n"
    "void main(){\n"
    "  float a = texture(uAtlas, vUV).r;\n"
    "  if (a < 0.5) discard;\n"
    "  oCol = vec4(uInk, 1.0);\n"
    "}\n";

static GLuint mksh(GLenum t, const char* src)
{
    GLuint sh = glCreateShader(t);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL); glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; glGetShaderInfoLog(sh, sizeof lg, NULL, lg);
               flog("fps: shader FAILED:"); flog(lg); s_state = 2; }
    return sh;
}

static void init_gl(void)
{
    GLuint vs, fs;
    GLint ok = 0;
    x_glDrawArrays    = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glActiveTexture = (PFN_ACTIVETEX) getgl("glActiveTexture");
    x_glUniform2f     = (PFN_UNIFORM2F) getgl("glUniform2f");
    x_glUniform3f     = (PFN_UNIFORM3F) getgl("glUniform3f");
    x_glDisable       = (PFN_DISABLE)   getgl("glDisable");
    if (!x_glDrawArrays || !x_glActiveTexture ||
        !x_glUniform2f || !x_glUniform3f || !x_glDisable) {
        flog("fps: missing GL proc"); s_state = 2; return;
    }
    /* Both are created before either is tested so the cleanup below is one
       path -- mksh returns the shader even when it failed, and a leak here is
       permanent: s_state 2 is never retried inside one GL context. */
    vs = mksh(GL_VERTEX_SHADER, VS); fs = mksh(GL_FRAGMENT_SHADER, FS);
    if (s_state == 2) { glDeleteShader(vs); glDeleteShader(fs); return; }
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs); glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!ok) { flog("fps: link FAILED"); glDeleteProgram(s_prog); s_prog = 0;
               s_state = 2; return; }
    s_uFrame = glGetUniformLocation(s_prog, "uFrame");
    s_uInk   = glGetUniformLocation(s_prog, "uInk");
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "uAtlas"), 0);
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof s_v, NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VST * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, VST * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    s_state = 1;
    flog("fps: armed - the frame-rate readout (tagpu_fps.on, or the Render menu)");
}

/* one cached string -> one quad at (x, y), advancing x. 0 = the atlas refused */
static int emit(const char* s, float* x, float y, int* nv)
{
    int ax, ay, w, h, yoff, aw = 1, ah = 1, i = *nv;
    float u0, v0, u1, v1, x0, x1, y0, y1;
    if (i + QUADV > MAXCH * QUADV) return 0;
    if (!tagpu_text_place(s, &ax, &ay, &w, &h, &yoff)) return 0;
    tagpu_text_dims(&aw, &ah);
    u0 = (float)ax / aw;        v0 = (float)ay / ah;
    u1 = (float)(ax + w) / aw;  v1 = (float)(ay + h) / ah;
    x0 = *x; x1 = x0 + (float)w;
    y0 = y;  y1 = y0 + (float)h;
#define PUT(n, px, py, pu, pv) \
    do { float* p = s_v + (size_t)(i + (n)) * VST; \
         p[0] = (px); p[1] = (py); p[2] = (pu); p[3] = (pv); } while (0)
    PUT(0, x0, y0, u0, v0); PUT(1, x1, y0, u1, v0); PUT(2, x0, y1, u0, v1);
    PUT(3, x1, y0, u1, v0); PUT(4, x1, y1, u1, v1); PUT(5, x0, y1, u0, v1);
#undef PUT
    *nv = i + QUADV;
    *x = x1 + 1.0f;
    return 1;
}

void tagpu_fps_present(const TAGPU_FRAME* f)
{
    static unsigned poll;
    DWORD now;
    char num[12];
    float x = 6.0f, y = 6.0f;
    int nv = 0, i;

    if (!f || s_state == 2) return;
    if (s_on < 0 || (poll++ % POLL) == 0)
        s_on = GetFileAttributesA(TRIGGER) != INVALID_FILE_ATTRIBUTES;
    if (!s_on) { s_fps = -1; s_frames = 0; s_t0 = 0; return; }

    /* the window: count every present, republish twice a second */
    now = timeGetTime();
    s_frames++;
    if (!s_t0) { s_t0 = now; s_frames = 0; }
    else if (now - s_t0 >= WINDOW_MS) {
        s_fps = (int)((s_frames * 1000u) / (now - s_t0));
        s_frames = 0; s_t0 = now;
    }
    if (s_fps < 0) return;                  /* nothing to say for the first window */

    if (!s_state) init_gl();
    if (s_state != 1) return;

    /* The font the game thread published, latched for this frame exactly as the
       marker gather latches it -- so the atlas cannot repack under our quads. */
    tagpu_text_frame();

    if (!emit("FPS", &x, y, &nv)) return;   /* no font yet: draw nothing */
    _snprintf(num, sizeof num, "%d", s_fps > 9999 ? 9999 : s_fps);
    num[sizeof num - 1] = 0;
    for (i = 0; num[i]; i++) {
        char d[2]; d[0] = num[i]; d[1] = 0;
        if (!emit(d, &x, y, &nv)) break;
    }
    if (!nv) return;

    glUseProgram(s_prog);
    x_glUniform2f(s_uFrame, (float)f->game_width, (float)f->game_height);
    x_glUniform3f(s_uInk, 1.0f, 1.0f, 1.0f);
    x_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tagpu_text_tex());
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)nv * VST * sizeof(float), s_v);
    x_glDisable(GL_DEPTH_TEST);
    x_glDrawArrays(GL_TRIANGLES, 0, nv);
    glBindVertexArray(0);
}

void tagpu_fps_glreset(void)
{
    s_state = 0; s_prog = 0; s_vao = 0; s_vbo = 0;
    s_fps = -1; s_frames = 0; s_t0 = 0;
}
