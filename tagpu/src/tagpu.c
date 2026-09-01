/* tagpu.dll — GPU overlay/renderer companion for our cnc-ddraw fork.
   G1: prove we can draw our own OpenGL 3.3-core content over the live game frame,
   inside the fork's render thread/context, translucently and toggleably.

   The fork calls TagpuPresent(&frame) once per frame, just before SwapBuffers, with the
   GL context current and the viewport already set to the letterboxed game image. We draw,
   then leave GL state as cnc-ddraw expects (blend disabled, our VAO/program unbound). */

#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <math.h>
#include <stdio.h>
#include "tagpu.h"

/* ---- GL 2.0+ entry points, loaded via wglGetProcAddress ---- */
static PFNGLCREATESHADERPROC        p_glCreateShader;
static PFNGLSHADERSOURCEPROC        p_glShaderSource;
static PFNGLCOMPILESHADERPROC       p_glCompileShader;
static PFNGLGETSHADERIVPROC         p_glGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC    p_glGetShaderInfoLog;
static PFNGLDELETESHADERPROC        p_glDeleteShader;
static PFNGLCREATEPROGRAMPROC       p_glCreateProgram;
static PFNGLATTACHSHADERPROC        p_glAttachShader;
static PFNGLLINKPROGRAMPROC         p_glLinkProgram;
static PFNGLGETPROGRAMIVPROC        p_glGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC   p_glGetProgramInfoLog;
static PFNGLUSEPROGRAMPROC          p_glUseProgram;
static PFNGLGENVERTEXARRAYSPROC     p_glGenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC     p_glBindVertexArray;
static PFNGLGENBUFFERSPROC          p_glGenBuffers;
static PFNGLBINDBUFFERPROC          p_glBindBuffer;
static PFNGLBUFFERDATAPROC          p_glBufferData;
static PFNGLVERTEXATTRIBPOINTERPROC p_glVertexAttribPointer;
static PFNGLENABLEVERTEXATTRIBARRAYPROC p_glEnableVertexAttribArray;
static PFNGLGETUNIFORMLOCATIONPROC  p_glGetUniformLocation;
static PFNGLUNIFORM4FPROC           p_glUniform4f;
static PFNGLUNIFORM2FPROC           p_glUniform2f;
static PFNGLUNIFORM1FPROC           p_glUniform1f;

static int   g_loaded = 0;
static int   g_ready  = 0;   /* shader+VAO built                */
static int   g_failed = 0;   /* init failed, don't retry        */
static GLuint g_prog, g_vao, g_vbo;
static GLint  g_uAngle, g_uOffset, g_uScale, g_uColor;

static void logline(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static void* load1(const char* n)
{
    void* p = (void*)wglGetProcAddress(n);
    if (!p) { char b[128]; _snprintf(b, sizeof b, "tagpu: missing GL proc %s", n); logline(b); }
    return p;
}

#define LOAD(v, t, n) do { p_##v = (t)load1(n); if (!p_##v) g_failed = 1; } while (0)

static void load_gl(void)
{
    LOAD(glCreateShader, PFNGLCREATESHADERPROC, "glCreateShader");
    LOAD(glShaderSource, PFNGLSHADERSOURCEPROC, "glShaderSource");
    LOAD(glCompileShader, PFNGLCOMPILESHADERPROC, "glCompileShader");
    LOAD(glGetShaderiv, PFNGLGETSHADERIVPROC, "glGetShaderiv");
    LOAD(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC, "glGetShaderInfoLog");
    LOAD(glDeleteShader, PFNGLDELETESHADERPROC, "glDeleteShader");
    LOAD(glCreateProgram, PFNGLCREATEPROGRAMPROC, "glCreateProgram");
    LOAD(glAttachShader, PFNGLATTACHSHADERPROC, "glAttachShader");
    LOAD(glLinkProgram, PFNGLLINKPROGRAMPROC, "glLinkProgram");
    LOAD(glGetProgramiv, PFNGLGETPROGRAMIVPROC, "glGetProgramiv");
    LOAD(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC, "glGetProgramInfoLog");
    LOAD(glUseProgram, PFNGLUSEPROGRAMPROC, "glUseProgram");
    LOAD(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC, "glGenVertexArrays");
    LOAD(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC, "glBindVertexArray");
    LOAD(glGenBuffers, PFNGLGENBUFFERSPROC, "glGenBuffers");
    LOAD(glBindBuffer, PFNGLBINDBUFFERPROC, "glBindBuffer");
    LOAD(glBufferData, PFNGLBUFFERDATAPROC, "glBufferData");
    LOAD(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC, "glVertexAttribPointer");
    LOAD(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC, "glEnableVertexAttribArray");
    LOAD(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC, "glGetUniformLocation");
    LOAD(glUniform4f, PFNGLUNIFORM4FPROC, "glUniform4f");
    LOAD(glUniform2f, PFNGLUNIFORM2FPROC, "glUniform2f");
    LOAD(glUniform1f, PFNGLUNIFORM1FPROC, "glUniform1f");
    g_loaded = 1;
}

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "uniform float uAngle;\n"
    "uniform vec2  uOffset;\n"
    "uniform vec2  uScale;\n"
    "void main(){\n"
    "  float c=cos(uAngle), s=sin(uAngle);\n"
    "  vec2 r = vec2(pos.x*c - pos.y*s, pos.x*s + pos.y*c);\n"
    "  gl_Position = vec4(r*uScale + uOffset, 0.0, 1.0);\n"
    "}\n";

static const char* FS =
    "#version 330 core\n"
    "out vec4 frag;\n"
    "uniform vec4 uColor;\n"
    "void main(){ frag = uColor; }\n";

static GLuint compile(GLenum type, const char* src)
{
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; p_glGetShaderInfoLog(s, sizeof log, NULL, log);
               logline("tagpu: shader compile FAILED:"); logline(log); g_failed = 1; }
    return s;
}

static void build(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    if (g_failed) return;
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    p_glLinkProgram(g_prog);
    GLint ok = 0; p_glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; p_glGetProgramInfoLog(g_prog, sizeof log, NULL, log);
               logline("tagpu: program link FAILED:"); logline(log); g_failed = 1; return; }
    p_glDeleteShader(vs); p_glDeleteShader(fs);

    g_uAngle  = p_glGetUniformLocation(g_prog, "uAngle");
    g_uOffset = p_glGetUniformLocation(g_prog, "uOffset");
    g_uScale  = p_glGetUniformLocation(g_prog, "uScale");
    g_uColor  = p_glGetUniformLocation(g_prog, "uColor");

    /* an equilateral-ish triangle centred on its own origin */
    const float verts[] = { 0.0f, 0.16f,  -0.14f,-0.10f,  0.14f,-0.10f };
    p_glGenVertexArrays(1, &g_vao);
    p_glBindVertexArray(g_vao);
    p_glGenBuffers(1, &g_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    p_glBindVertexArray(0);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);

    g_ready = 1;
    logline("tagpu: overlay ready (GL 3.3 core program + VAO built)");
}

__declspec(dllexport) void __cdecl TagpuPresent(const TAGPU_FRAME* f)
{
    static int entered = 0;
    if (!entered) { entered = 1; logline("tagpu: TagpuPresent entered (first call)"); }

    if (g_failed || !f || f->abi != TAGPU_ABI) return;

    /* toggle: presence of tagpu_overlay.off suppresses drawing */
    if (GetFileAttributesA("tagpu_overlay.off") != INVALID_FILE_ATTRIBUTES) return;

    if (!g_loaded) { logline("tagpu: load_gl()..."); load_gl(); logline("tagpu: load_gl done"); }
    if (g_failed) return;
    if (!g_ready) { logline("tagpu: build()..."); build(); logline("tagpu: build returned");
                    if (g_failed || !g_ready) return; }
    { static int drew = 0; if (!drew) { drew = 1; logline("tagpu: first draw"); } }

    float t = (float)f->frame_counter;
    float angle = t * 0.035f;
    /* aspect: keep the triangle from stretching with the window */
    float aspect = (f->vp_h > 0 && f->vp_w > 0) ? (float)f->vp_w / (float)f->vp_h : 1.0f;
    float sx = 1.0f / (aspect > 1.0f ? aspect : 1.0f);
    float sy = (aspect < 1.0f ? aspect : 1.0f);
    /* pulse the colour so liveness is visible even in a still */
    float pulse = 0.5f + 0.5f * (float)sin(t * 0.05f);

    p_glUseProgram(g_prog);
    p_glBindVertexArray(g_vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    p_glUniform1f(g_uAngle, angle);
    p_glUniform2f(g_uScale, sx, sy);
    /* upper-left quadrant of the game image */
    p_glUniform2f(g_uOffset, -0.55f, 0.55f);
    p_glUniform4f(g_uColor, 0.15f, 0.85f, 1.0f, 0.55f * (0.6f + 0.4f * pulse));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* a second, fixed marker (opaque magenta) as an unmistakable anchor */
    p_glUniform1f(g_uAngle, 0.0f);
    p_glUniform2f(g_uScale, sx * 0.5f, sy * 0.5f);
    p_glUniform2f(g_uOffset, 0.55f, 0.55f);
    p_glUniform4f(g_uColor, 1.0f, 0.1f, 0.7f, 0.85f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* leave state as cnc-ddraw expects */
    glDisable(GL_BLEND);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
