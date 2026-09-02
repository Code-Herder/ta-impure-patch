/* tagpu_overlay.c — per-present entry point of the GPU pass, compiled INTO our
   cnc-ddraw fork (no separate module => no runtime LoadLibrary, which is what
   destabilised TA under wine). Called from render_ogl.c just before SwapBuffers.
   Runs the file-triggered services (input, peek, ui, catalogues, scenario),
   detects GL context changes, flushes the engine detours, logs the live roster
   tacli reads, then dispatches the GL passes (scaffold, native, write-back).
   tagpu_overlay.off is the kill switch for everything we draw in GL.
   The G1/G2/Phase-A proof markers (corner spinner, mouse dot, per-unit and
   per-piece triangles) were retired 2026-09-02; the log lines they shared stay. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "opengl_utils.h"   /* the fork's extern GL function pointers   */
#include "tagpu_overlay.h"
#include "tagpu_tracer.h"
#include "tagpu_suppress.h"
#include "tagpu_render3do.h"
#include "tagpu_owndraw.h"
#include "tagpu_fxown.h"
#include "tagpu_scaffold.h"
#include "tagpu_input.h"
#include "tagpu_native.h"
#include "tagpu_peek.h"
#include "tagpu_ui.h"
#include "tagpu_cat.h"
#include "tagpu_scenario.h"
#include "tagpu_weapons.h"

/* GL entry point the fork does not already expose — load once ourselves. */
typedef void (APIENTRY *PFN_READPIXELS)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);

static PFN_READPIXELS  x_glReadPixels;

static int   s_state = 0;   /* 0=unloaded 1=ready (reset on GL context change) */

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

static void init_overlay(void)
{
    x_glReadPixels = (PFN_READPIXELS)getgl("glReadPixels");
    if (!x_glReadPixels) olog("tagpu: glReadPixels missing - GL capture disabled");
    s_state = 1;
    olog("tagpu: overlay ready (built into fork)");
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

/* ---- G2: read live engine state (unit array, eye, local player, mouse) ----
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


/* Log TA's own mouse position (game-space, read from memory). No longer drawn,
   but still the way to read the engine's cursor without touching the user's
   pointer (input-firewall.md). */
static void log_mouse(const TAGPU_FRAME* f, char* ta)
{
    int mx = *(int*)(ta + OFF_MOUSE);
    int my = *(int*)(ta + OFF_MOUSE + 4);
    if (mx < -50 || mx > 4000 || my < -50 || my > 4000) return;
    static unsigned last = 0;
    if (f->frame_counter - last >= 15) { last = f->frame_counter;
        char b[96]; _snprintf(b, sizeof b, "mouse: game=(%d,%d)", mx, my); olog(b); }
}

/* Walk the live unit array and log what tacli reads: the `units:` line every
   30 frames (`scenario load` waits on alive>0, `roster` takes eye= from it) and
   the full roster block every 300 frames (`tacli roster`). Screen coords use the
   engine's rule sx=wx-eyeX+vpL, sy=wy-alt/2-eyeY+vpT at the 640x480 viewport. */
static void log_units(const TAGPU_FRAME* f)
{
    char* ta = *(char**)TA_MAINPP;
    if ((size_t)ta < 0x600000u) return;             /* no game struct yet (menu) */
    log_mouse(f, ta);
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    if ((size_t)beg < 0x600000u || (size_t)end < 0x600000u || end <= beg) return;
    if ((size_t)(end - beg) > (size_t)UNIT_STRIDE * 20000) return; /* sanity */

    int eyeX = *(int*)(ta + OFF_EYEX);
    int eyeY = *(int*)(ta + OFF_EYEY);
    unsigned char me = *(unsigned char*)(ta + OFF_LOCALPID);
    int gw = f->game_width  > 0 ? f->game_width  : 640;
    int gh = f->game_height > 0 ? f->game_height : 480;

    int alive = 0, onscreen = 0;
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
            /* idx = UnitInGameIndex (+0xA8), the engine's own slot. It is
               RECYCLED on death, so it is never a public identity — but it is
               what `tacli scenario` reports per spawned entity, so the roster
               has to speak the same number for the two to be comparable. */
            char db[192]; _snprintf(db, sizeof db,
                "  u%03d %-12.12s own=%d idx=%d world=(%d,%d,%d) screen=(%d,%d) nano=%.2f",
                alive, nm, (int)*(unsigned char*)(u + U_OWNER),
                (int)*(short*)(u + 0xA8), wx, wy, wz, sx, sy,
                *(float*)(u + 0x104));   /* build fraction REMAINING (build-state.md) */
            olog(db); }
        if (sx >= -gw / 40 && sx <= gw + gw / 40 && sy >= -gh / 40 && sy <= gh + gh / 40)
            onscreen++;
    }
    static unsigned last = 0;
    if (f->frame_counter - last >= 30) { last = f->frame_counter;
        char b[160]; _snprintf(b, sizeof b, "units: alive=%d onscreen=%d eye=(%d,%d) me=%d",
                               alive, onscreen, eyeX, eyeY, (int)me); olog(b); }
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
    /* on-demand weapon-slot dump (tagpu_weapons.trigger) — the A/B oracle for
       the extra-weapons module; read-only, runs armed or not. */
    tagpu_weapons_frame(f->frame_counter);
    /* on-demand GUI snapshot (tagpu_ui.trigger) — the read half of `tacli ui`.
       The menus are exactly where it earns its keep, so like peek it must run
       before any game exists. */
    tagpu_ui_frame(f);
    /* on-demand unit/feature catalogues (tagpu_units.trigger,
       tagpu_features.trigger) — validation layer 2 for `tacli scenario`. */
    tagpu_cat_frame(f);
    /* on-demand situation applier (tagpu_scenario.trigger) and the engine
       switches (tagpu_switches.trigger). Detection and reporting live here; the
       creation pass runs from this module's own Game_MainLoopTick detour, never
       mid-render. Switches must reach the menus too, like peek. */
    tagpu_scenario_frame(f);
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
    /* effects own-the-draw flush: no-op unless armed at launch. */
    tagpu_fxown_flush(f->frame_counter);
    if (GetFileAttributesA("tagpu_overlay.off")!=INVALID_FILE_ATTRIBUTES) { writeback_paint(f); return; }
    if (s_state==0) init_overlay();
    if (s_state!=1) { writeback_paint(f); return; }

    /* live-state logs tacli depends on (roster, units:, mouse:) + the 3DO probe */
    log_units(f);
    if ((f->frame_counter % 300) == 61) probe_unit_model();

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
