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
   "visibly alive" readout, with the command acknowledgement (the last record
   applied, and the cumulative anchor delta applied by then). Same eleven strings plus "PK", "T", "E", "A", "D", "," and "-",
   so it costs the atlas nothing per frame either.

   THE VULKAN PASS DRAWS THIS READOUT FROM THE QUADS BUILT HERE. It
   (tagpu_vk_fps.c) does not rebuild the geometry; it takes the vertices this
   file just built, through `tagpu_fps_quads`, and samples the same atlas bytes.

   `tagpu_fps_quads` CONSUMES what it returns, and here is exactly what that
   buys. The Vulkan pass records in render_vk.c's `tagpu_vk_frame`, after the
   overlay, and the overlay does not reach this file on every frame that is
   presented (`tagpu_overlay.off`, a level teardown).
   Consuming makes it impossible for one frame's vertices to be drawn TWICE --
   which is the case that matters, because the second draw would be of a frame
   whose readout has already been superseded. It does not make it impossible for
   the lane to draw the newest vertices on a frame this file was skipped on:
   that frame shows a readout one frame stale, which is a digit and not a fault.
   The stronger property would need a frame stamp the two files share, and
   nothing yet needs one.

   THE A/B (`tagpu_fps.ab`) IS AN ORACLE, NOT INSTRUMENTATION, and what there
   is of it here is the two lines that say WHEN. The lever latches
   a claim for one frame, once, until the file is removed; the seam then captures
   THAT frame from the Vulkan image and `tagpu_vk_ab_arm` has already unlinked
   the target so the file on the disk is this arming's. Diff it against a capture
   from another BUILD. `tagpu_vk.on=color=0,0,0` matters for it, because a
   capture is only comparable when the pass sits alone over a known background. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_fps.h"
#include "tagpu_text.h"
#include "tagpu_vk.h"      /* tagpu_vk_ab_arm: the A/B claim */

#define TRIGGER   "tagpu_fps.on"
#define PKSHOW    "tagpu_packet.show"
#define ABFILE    "tagpu_fps.ab"      /* the A/B: capture one frame            */
#define POLL      30                  /* frames between trigger polls          */
#define WINDOW_MS 500u                /* averaging window                      */
#define MAXCH     48
#define QUADV     6
/* the header's bound is this one; a build where they disagree does not link */
typedef char tagpu_fps_maxv_agrees[(MAXCH * QUADV == TAGPU_FPS_MAXV) ? 1 : -1];
#define VST       4                   /* x, y, u, v                            */

static int    s_on = -1, s_pk, s_ab, s_abDone, s_abFrame;
static int    s_nv;                   /* this frame's vertices, handed over once */
static int    s_fw, s_fh;             /* ...and the frame size they are in     */
static float  s_v[MAXCH * QUADV * VST];

/* the averaging window */
static DWORD    s_t0;
static unsigned s_frames;
static int      s_fps = -1;

/* THE SHADER PAIR IS A BUILD INPUT, NOT CODE THIS FILE RUNS. Nothing here
   references them -- tools/spirv-gen.py reads them out of the PREPROCESSED
   translation unit and generates the SPIR-V tagpu_vk_fps.c draws with, so deleting
   them fails the build with "the manifest names <pass>::VS and the source does not
   have it". The pragma below is paired and its `pop` was PROVED with a planted
   probe rather than read: a `pop` inside a comment is text and not a directive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
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
#pragma GCC diagnostic pop

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

/* The vertices this frame built, for the Vulkan edition of the pass, and HANDED
   OVER EXACTLY ONCE -- see the file header. 0 means there is nothing to draw,
   which is also what a frame the overlay did not run on returns.

   `ab` RIDES WITH THEM, AND THAT IS WHAT MAKES THE COMPARISON HONEST. Both
   lanes could poll `tagpu_fps.ab` for themselves, but they poll on different cadences (this one every 30 frames, the lane every
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

       THE A/B FLAG GOES WITH IT, AND ITS LIFE IS ONE FRAME: set at the end of
       this function, read by the lane before the next present, gone here.
       Latched until the Vulkan lane consumed it, the flag would survive a frame
       where the lane did not get as far as `tagpu_vk_fps_prepare` (not yet
       `ST_READY`, a swapchain rebuild, the Vulkan lever off) and ride with a
       LATER frame's vertices -- a capture of the wrong frame, which is the one
       thing the design exists to prevent. A capture the lane never collected is
       simply not written, and `tools/vk-ab.py` says so. */
    /* THIS PASS BUILDS QUADS; THE VULKAN PASS DRAWS THEM. The poll, the
       frame-rate window, the font latch and the vertex array are the pass, and
       they run unconditionally. */

    s_nv = 0; s_abFrame = 0;

    if (!f) return;
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
        /* ...and the command acknowledgement: the last record
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

    /* OUTSIDE ANY DRAW, because the A/B is armed without one -- the
       scaffold's shape (tagpu_scaffold.c) and the same reason. Read once so the two arms below cannot disagree about whether
       this is the capture frame. */
    int taking = s_ab && !s_abDone;

    if (taking) {
        /* THE A/B CLAIM. The lever claims the VULKAN capture: `tagpu_vk_ab_arm`
           unlinks the target `_vk.ppm` at the instant the claim latches, which is what
           makes the file on the disk this arming's rather than an earlier run's. Diff
           it against a capture taken from another BUILD. */
        s_abDone = 1;
        s_abFrame = tagpu_vk_ab_arm("fps");
    }

    /* PUBLISHED AFTER THE DRAW WHERE THERE IS ONE, and after the build in
       either case: `s_v` holds what this frame put together, and the Vulkan
       lane is about to draw the same array. */
    s_nv = nv; s_fw = f->game_width; s_fh = f->game_height;
}

