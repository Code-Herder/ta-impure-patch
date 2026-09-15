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
   the game thread published as bytes in the frame packet (tagpu_packet_pub.c,
   at hook 8) through tagpu_text_frame's per-frame latch, exactly as the marker
   gather does.

   THE PACKET LINE. `tagpu_packet.show` (polled with the trigger) adds a second
   row under the frame rate: `PK<seq> T<tick> E<eyeX>,<eyeY> A<ack> D<dx>,<dy>`
   from the packet the driver acquired this frame — the frame packet exchange's
   "visibly alive" readout (landing 1), with the command acknowledgement since
   landing 2 (the last record applied, and the cumulative anchor delta applied
   by then). Same eleven strings plus "PK", "T", "E", "A", "D", "," and "-",
   so it costs the atlas nothing per frame either.

   TWO BACKENDS DRAW THIS PASS SINCE G19d, AND FROM ONE SET OF QUADS. The
   Vulkan lane's edition of it (tagpu_vk_fps.c) does not rebuild the geometry;
   it takes the vertices this file just built, through `tagpu_fps_quads`, and
   samples the same atlas bytes. That is what makes "0 px apart" a statement
   about the two APIs rather than about two pieces of arithmetic that happen to
   agree -- the quads, the atlas, the frame size and the ink are literally the
   same values, and only the rasteriser differs.

   `tagpu_fps_quads` CONSUMES what it returns, and here is exactly what that
   buys. The Vulkan lane presents from `ogl_render` while this runs inside the
   overlay, and the overlay does not run on every path that reaches the swap.
   Consuming makes it impossible for one frame's vertices to be drawn TWICE --
   which is the case that matters, because the second draw would be of a frame
   whose readout has already been superseded. It does not make it impossible for
   the lane to draw the newest vertices on a frame this file was skipped on:
   that frame shows a readout one frame stale, which is a digit and not a fault.
   [The header used to claim the stronger property. A review disproved it
   2026-09-15; making it true would need a frame stamp the two files share, and
   nothing yet needs one.]

   THE A/B (`tagpu_fps.ab`) IS AN ORACLE, NOT INSTRUMENTATION. With the file
   present this pass clears the frame to black before it draws, reads the result
   back to `tagpu_fps_gl.ppm` and latches -- one frame, once, until the lever is
   removed. Black because the Vulkan lane clears its own image to whatever
   `color=` says and `color=0,0,0` makes the two backgrounds the same, which is
   what turns two captures into a pixel comparison. It clears the WHOLE frame on
   purpose: a readout drawn over the game cannot be compared against anything,
   because the game is under it. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opengl_utils.h"
#include "tagpu_fps.h"
#include "tagpu_text.h"

#define TRIGGER   "tagpu_fps.on"
#define PKSHOW    "tagpu_packet.show"
#define ABFILE    "tagpu_fps.ab"      /* the G19d A/B: capture one frame       */
#define ABOUT     "tagpu_fps_gl.ppm"
#define POLL      30                  /* frames between trigger polls          */
#define WINDOW_MS 500u                /* averaging window                      */
#define MAXCH     48
#define QUADV     6
/* the header's bound is this one; a build where they disagree does not link */
typedef char tagpu_fps_maxv_agrees[(MAXCH * QUADV == TAGPU_FPS_MAXV) ? 1 : -1];
#define VST       4                   /* x, y, u, v                            */

typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_UNIFORM2F)(GLint, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_UNIFORM3F)(GLint, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_DISABLE)(GLenum);
typedef void (APIENTRY *PFN_CLEARCOLOR)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_CLEAR)(GLbitfield);
typedef void (APIENTRY *PFN_READPIXELS)(GLint, GLint, GLsizei, GLsizei,
                                        GLenum, GLenum, void*);
typedef void (APIENTRY *PFN_GETINTEGERV)(GLenum, GLint*);
typedef void (APIENTRY *PFN_PIXELSTOREI)(GLenum, GLint);
typedef void (APIENTRY *PFN_GETFLOATV)(GLenum, GLfloat*);
typedef GLboolean (APIENTRY *PFN_ISENABLED)(GLenum);
typedef void (APIENTRY *PFN_ENABLE)(GLenum);
static PFN_DRAWARRAYS x_glDrawArrays;
static PFN_ACTIVETEX  x_glActiveTexture;
static PFN_UNIFORM2F  x_glUniform2f;
static PFN_UNIFORM3F  x_glUniform3f;
static PFN_DISABLE    x_glDisable;
/* the A/B's five, resolved with the rest and never called without the lever */
static PFN_CLEARCOLOR  x_glClearColor;
static PFN_CLEAR       x_glClear;
static PFN_READPIXELS  x_glReadPixels;
static PFN_GETINTEGERV x_glGetIntegerv;
static PFN_PIXELSTOREI x_glPixelStorei;
static PFN_GETFLOATV   x_glGetFloatv;
static PFN_ISENABLED   x_glIsEnabled;
static PFN_ENABLE      x_glEnable;

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
static int    s_on = -1, s_pk, s_ab, s_abDone, s_abFrame;
static int    s_nv;                   /* this frame's vertices, handed over once */
static int    s_fw, s_fh;             /* ...and the frame size they are in     */
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
    x_glClearColor    = (PFN_CLEARCOLOR) getgl("glClearColor");
    x_glClear         = (PFN_CLEAR)      getgl("glClear");
    x_glReadPixels    = (PFN_READPIXELS) getgl("glReadPixels");
    x_glGetIntegerv   = (PFN_GETINTEGERV)getgl("glGetIntegerv");
    x_glPixelStorei   = (PFN_PIXELSTOREI)getgl("glPixelStorei");
    x_glGetFloatv     = (PFN_GETFLOATV)  getgl("glGetFloatv");
    x_glIsEnabled     = (PFN_ISENABLED)  getgl("glIsEnabled");
    x_glEnable        = (PFN_ENABLE)     getgl("glEnable");
    if (!x_glDrawArrays || !x_glActiveTexture ||
        !x_glUniform2f || !x_glUniform3f || !x_glDisable) {
        flog("fps: missing GL proc"); s_state = 2; return;
    }
    /* The A/B's five are NOT in that test: the readout is a play feature and
       the oracle is not, so a driver that somehow lacked one of them would
       lose the A/B and keep the readout. `ab_capture` checks them itself. */
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

/* ---- the G19d A/B oracle ------------------------------------------------
   One frame of the GL lane's readout over a black field, as a binary PPM, so
   that it can be compared texel for texel against the Vulkan lane's capture of
   the same frame (tools/vk-ab.py). Not a screenshot of the game: the game is
   deliberately not in it.

   THE RECT IS THE VIEWPORT'S, asked for rather than assumed. The pass draws in
   GAME-FRAME pixels through `uFrame` and the viewport is what maps those onto
   the target, so the viewport is the rect the comparison is about -- and the
   Vulkan lane's swapchain covers the game window's CLIENT area, which is the
   same rect only when the fork is not letterboxing. The compare tool refuses
   two captures of different sizes rather than scaling one, because a scaled
   comparison cannot be 0 px by construction. */
static void ab_write_ppm(const char* path, int w, int h, const unsigned char* rgba)
{
    FILE* fp;
    int y, x;
    unsigned char* row;
    unsigned char* line;
    /* The row buffer before the file, for the reason tagpu_vk_shot.c gives: a
       header-only PPM reads as a broken writer rather than as a failed malloc. */
    line = (unsigned char*)malloc((size_t)w * 3);
    if (!line) { flog("fps: no memory for a row of the A/B capture"); return; }
    if (!(fp = fopen(path, "wb"))) {
        flog("fps: the A/B could not open its file"); free(line); return;
    }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    /* glReadPixels hands back the BOTTOM row first and a PPM's first row is the
       TOP one, so the rows are written backwards. Getting this wrong produces a
       capture that differs from the Vulkan one in every text pixel and in
       nothing else, which reads as a Y-flip bug in the port rather than in the
       oracle -- hence the note. */
    /* ONE fwrite A ROW, for the reason tagpu_vk_shot.c gives: this is the render
       thread and a stdio call per pixel is a million of them at 1080p. */
    for (y = h - 1; y >= 0; y--) {
        row = (unsigned char*)rgba + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            line[x * 3 + 0] = row[(size_t)x * 4 + 0];
            line[x * 3 + 1] = row[(size_t)x * 4 + 1];
            line[x * 3 + 2] = row[(size_t)x * 4 + 2];
        }
        fwrite(line, 1, (size_t)w * 3, fp);
    }
    free(line);
    fclose(fp);
}

static void ab_capture(void)
{
    GLint vp[4] = { 0, 0, 0, 0 };
    GLint oldpack = 4;
    unsigned char* buf;
    char msg[160];
    if (!x_glReadPixels || !x_glGetIntegerv || !x_glPixelStorei) {
        flog("fps: the A/B needs glReadPixels and this context has none"); s_abDone = 1; return;
    }
    x_glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0 || vp[2] > 8192 || vp[3] > 8192) {
        flog("fps: the A/B found no sane viewport"); s_abDone = 1; return;
    }
    buf = (unsigned char*)malloc((size_t)vp[2] * vp[3] * 4);
    if (!buf) { s_abDone = 1; return; }
    /* GL_PACK_ALIGNMENT goes back too: 4 is the initial value every other
       reader in the fork assumes, and leaving it at 1 is the same kind of leak
       from a lever into play state as the clear colour was. */
    x_glGetIntegerv(GL_PACK_ALIGNMENT, &oldpack);
    x_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    x_glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, buf);
    x_glPixelStorei(GL_PACK_ALIGNMENT, oldpack);
    ab_write_ppm(ABOUT, vp[2], vp[3], buf);
    free(buf);
    _snprintf(msg, sizeof msg,
              "fps: A/B wrote %s, %dx%d - the Vulkan lane writes tagpu_fps_vk.ppm",
              ABOUT, (int)vp[2], (int)vp[3]);
    msg[sizeof msg - 1] = 0;
    flog(msg);
    s_abDone = 1;
}

/* The vertices this frame built, for the Vulkan edition of the pass, and HANDED
   OVER EXACTLY ONCE -- see the file header. 0 means there is nothing to draw,
   which is also what a frame the overlay did not run on returns.

   `ab` RIDES WITH THEM, AND THAT IS WHAT MAKES THE COMPARISON HONEST. Both
   lanes could poll `tagpu_fps.ab` for themselves -- and the first version did --
   but they poll on different cadences (this one every 30 frames, the lane every
   250 ms), so the two captures could land several hundred frames apart, and the
   readout changes its number twice a second. The captures would then differ in
   the digits and agree about nothing else, which reads exactly like a broken
   port. Handing the flag over with the vertices it belongs to makes "the same
   frame" a fact about one variable rather than a hope about two clocks. */
int tagpu_fps_quads(const float** v, int* nv, int* fw, int* fh, int* ab)
{
    if (!s_nv) return 0;
    *v = s_v; *nv = s_nv; *fw = s_fw; *fh = s_fh;
    *ab = s_abFrame;
    s_nv = 0; s_abFrame = 0;
    return 1;
}

void tagpu_fps_present(const TAGPU_FRAME* f)
{
    static unsigned poll;
    DWORD now;
    char num[12];
    float x = 6.0f, y = 6.0f;
    int nv = 0, i;

    /* NOTHING TO HAND OVER UNTIL THIS FRAME HAS BUILT IT. Every return below
       leaves both at 0.

       THE A/B FLAG GOES WITH IT, AND LEAVING IT LATCHED WAS A BUG. [FROM REVIEW
       2026-09-15.] It was set once and cleared only when the Vulkan lane
       consumed it -- so on a frame where the lane did not get as far as
       `tagpu_vk_fps_prepare` (not yet `ST_READY`, a swapchain rebuild, the
       Vulkan lever off) the flag survived and rode with a LATER frame's
       vertices. The two captures would then have been of different frames,
       which is the one thing the design exists to prevent. Its life is now one
       frame: set at the end of this function, read by the lane before the next
       present, gone here. A capture the lane never collected is simply not
       written, and `tools/vk-ab.py` says so. */
    s_nv = 0; s_abFrame = 0;

    if (!f || s_state == 2) return;
    if (s_on < 0 || (poll++ % POLL) == 0) {
        s_on = GetFileAttributesA(TRIGGER) != INVALID_FILE_ATTRIBUTES;
        s_pk = GetFileAttributesA(PKSHOW) != INVALID_FILE_ATTRIBUTES;
        /* the A/B re-arms when the lever is taken away and put back, so a
           second capture needs no relaunch */
        s_ab = GetFileAttributesA(ABFILE) != INVALID_FILE_ATTRIBUTES;
        if (!s_ab) s_abDone = 0;
    }
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
    tagpu_text_frame(f->packet);

    if (!emit("FPS", &x, y, &nv)) return;   /* no font yet: draw nothing */
    _snprintf(num, sizeof num, "%d", s_fps > 9999 ? 9999 : s_fps);
    num[sizeof num - 1] = 0;
    for (i = 0; num[i]; i++) {
        char d[2]; d[0] = num[i]; d[1] = 0;
        if (!emit(d, &x, y, &nv)) break;
    }
    if (s_pk && f->packet) {
        /* the second row: one cached string per character, like the first,
           one row (the font's height) below it */
        const TAGPU_PACKET* p = f->packet;
        char line[64];
        int ax, ay, w, h = 0, yoff;
        /* ...and since landing 2 the command acknowledgement: the last record
           the game thread applied before this draw, and the cumulative anchor
           delta it had applied by then */
        _snprintf(line, sizeof line, "PK%u T%u E%d,%d A%u D%d,%d", p->head_seq, p->tick, p->eye[0], p->eye[1],
                  p->cmd_ack_seq, p->cmd_ack_dx, p->cmd_ack_dy);
        line[sizeof line - 1] = 0;
        if (tagpu_text_place("FPS", &ax, &ay, &w, &h, &yoff)) {
            x = 6.0f;
            for (i = 0; line[i]; i++) {
                char d[2]; d[0] = line[i]; d[1] = 0;
                if (line[i] == ' ') { x += 4.0f; continue; }
                if (!emit(d, &x, y + (float)h + 2.0f, &nv)) break;
            }
        }
    }
    if (!nv) return;

    /* THE FRAME GOES BLACK FIRST WHEN THE A/B IS ARMED, and only then. The
       comparison is of this pass's pixels, so everything that is not this pass
       has to leave the frame -- scissor off, because a scissor left on from the
       UI layer would clear a rectangle rather than the frame. One frame.

       AND EVERY PIECE OF STATE IT TOUCHES GOES BACK. [FROM REVIEW 2026-09-15.]
       The clear colour and the scissor enable were left where the capture put
       them, for the rest of the session, by a lever that exists to measure the
       renderer and must therefore not change it. They are small leaks -- the
       other clear sites set their own colour and the scissor rests disabled --
       and they are exactly the kind of "harmless today" that an A/B taken in
       six months would be reading. */
    {
        int can = x_glClear && x_glClearColor && x_glDisable && x_glGetFloatv &&
                  x_glIsEnabled && x_glEnable;
        int shot = s_ab && !s_abDone && can;
        /* AND AN A/B THAT CANNOT RUN SAYS SO, ONCE. [FROM THE REVIEW'S SECOND
           PASS 2026-09-15.] The state save and restore added three more entry
           points to this list, and a context missing any of them used to make
           the lever do nothing at all -- no clear, no capture, no line in the
           log, and `s_abDone` left clear so it tried again every poll for the
           session. `ab_capture` logs for its own three; these had nothing. */
        if (s_ab && !s_abDone && !can) {
            flog("fps: the A/B needs glClearColor/glClear/glGetFloatv/glIsEnabled/"
                 "glEnable and this context is missing one - nothing captured");
            s_abDone = 1;
        }
        GLfloat oldcol[4] = { 0, 0, 0, 0 };
        GLboolean oldsc = GL_FALSE;
        if (shot) {
            x_glGetFloatv(GL_COLOR_CLEAR_VALUE, oldcol);
            oldsc = x_glIsEnabled(GL_SCISSOR_TEST);
            x_glDisable(GL_SCISSOR_TEST);
            x_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            x_glClear(GL_COLOR_BUFFER_BIT);
        }

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

    /* PUBLISHED AFTER THE GL DRAW, not before: `s_v` is what was just drawn,
       and the Vulkan lane is about to draw the same array. */
    s_nv = nv; s_fw = f->game_width; s_fh = f->game_height;

        if (shot) {
            ab_capture();
            s_abFrame = 1;
            x_glClearColor(oldcol[0], oldcol[1], oldcol[2], oldcol[3]);
            if (oldsc) x_glEnable(GL_SCISSOR_TEST);
        }
    }
}

void tagpu_fps_glreset(void)
{
    s_state = 0; s_prog = 0; s_vao = 0; s_vbo = 0;
    s_fps = -1; s_frames = 0; s_t0 = 0;
    s_nv = 0; s_abFrame = 0;
}
