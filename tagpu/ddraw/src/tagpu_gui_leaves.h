/* tagpu_gui_leaves.h — the observed leaves. Private to tagpu_gui_hook.c.

   One entry per engine function that writes pixels into an 8bpp surface in
   the UI path. Each observer reads the engine's own arguments off the stack
   frame it was handed and records (surface, box, kind); the engine then runs
   the function unchanged. Conventions and boxes: gui-renderer.md appendix and
   the engine map; the byte strings are the prologues stolen, byte-matched at
   install, chosen to end on an instruction boundary with no rel32 inside.

   Stack frame handed to a `before`: e[0] = return address, e[1..] = the
   stack arguments in order (every one here is stdcall or cdecl). */

#define ARG(e, n) (((unsigned*)(e))[n])
#define SARG(e, n) (((int*)(e))[n])

/* The graphics globals block `*(0x51FBD0)` (RE 2026-09-07, gui-renderer.md
   appendix): +0xBC = the system-memory back buffer every flip presents and
   every NULL-context blit draws into, valid while +0xDC != 0. DrawGameScreen
   makes main+0x37E1B that buffer at 0x468D30; the shell has its own. */
#define GFX_GLOBALS_PP 0x0051FBD0u
#define GFX_BACKBUF    0xBC
#define GFX_BACKBUF_ON 0xDC
static const int* back_buffer(void)
{
    const char* g = *(const char* const*)GFX_GLOBALS_PP;
    if (!ptr_ok(g) || *(const int*)(g + GFX_BACKBUF_ON) == 0) return NULL;
    return *(const int* const*)(g + GFX_BACKBUF);
}
/* a blit's destination: its context, or the back buffer when it passed NULL
   (0x4C5E70 arm 1) */
static const int* ctx_or_back(unsigned ctxArg)
{
    return ctxArg ? (const int*)(size_t)ctxArg : back_buffer();
}

/* GAF frame header (file-formats.md; tagpu_gaf.h TAGPU_GF_*) */
#define GF_W(f)   (*(const unsigned short*)((f) + 0x00))
#define GF_H(f)   (*(const unsigned short*)((f) + 0x02))
#define GF_HX(f)  (*(const short*)((f) + 0x04))
#define GF_HY(f)  (*(const short*)((f) + 0x06))

/* ---- 0x4B7F90 CopyGafToContext(ctx, frame, x, y) stdcall ret 0x10 ------
   lands the frame at (x - HotX, y - HotY), clipped to ctx's clip rect
   (composite-buffer.md §3). 0x4B8500 is the shaded twin with the same shape. */
/* Blits that are not UI although they land in the back buffer: the unit
   composite blit inside 0x459200 (the engine draws its all-key composites
   while owndraw skips the rasterisers), and the cursor's draw paths (exe map
   "the two ways the cursor gets drawn"). Anything recorded while the flip is
   running is the cursor too (s_inFlip). Matched on the return address; the
   ranges are the functions' extents read from the disassembly (2026-09-07):
   0x459200's seven leaf calls sit at 0x459319..0x4597D3 (each return address
   five bytes on) and it ends at 0x4597DF; the cursor code's fifteen
   (0x4C23C9..0x4C297E) end at 0x4C2989;
   the flip 0x4C63A0 has three epilogues (0x4C6669, 0x4C668F, 0x4C67BA) and
   0x4C67C0 — its two callers are 0x4C641B and 0x4C6544, both inside the
   flip — ends at 0x4C6884. */
static int excluded_caller(unsigned ret)
{
    if (s_inFlip) return 1;
    if (ret >= 0x00459200u && ret < 0x00459800u) return 1;   /* the unit blit    */
    if (ret >= 0x004C2380u && ret < 0x004C2A00u) return 1;   /* cursor draws     */
    if (ret >= 0x004C6300u && ret < 0x004C6890u) return 1;   /* flip + 0x4C67C0  */
    return 0;
}

static int s_gafDbg = 0;          /* trace: the first blits after a build */
static void gaf_box(void* e, int kind)
{
    const int* ctx = ctx_or_back(ARG(e, 1));
    const unsigned char* fr = (const unsigned char*)(size_t)ARG(e, 2);
    int x = SARG(e, 3), y = SARG(e, 4);
    int l, t, r, b;
    SURF* s;
    if (excluded_caller(ARG(e, 0))) return;
    s = surf_of_ctx(ctx);
    if (s_trace && s_gafDbg > 0 && ptr_ok(ctx)) {
        const char* ta = *(const char* const*)TA_MAINPP;
        const char* top = ptr_ok(ta) ? *(const char* const*)(ta + OFF_GUI_TOP) : NULL;
        const char* ct = ptr_ok(top) ? *(const char* const*)(top + GM_CTRLS) : NULL;
        char b[300];
        s_gafDbg--;
        _snprintf(b, sizeof b, "gui trace: blit ret=%08X ctxArg=%08X hdr=(%d,%d,%d,%08X) clip=(%d,%d,%d,%d) at (%d,%d) panel+0xBC=%08X +0xB8=%08X",
                  ARG(e, 0), ARG(e, 1), ctx[0], ctx[1], ctx[2], (unsigned)ctx[3], ctx[7], ctx[8], ctx[9], ctx[10], x, y,
                  ptr_ok(ct) ? *(const unsigned*)(ct + P_SURFACE) : 0u, ptr_ok(ct) ? *(const unsigned*)(ct + 0xB8) : 0u);
        glog(b);
    }
    if (!ptr_ok(fr)) { op_add(kind, NULL, 0, 0, 0, 0); return; }
    l = x - GF_HX(fr); t = y - GF_HY(fr);
    r = l + GF_W(fr) - 1; b = t + GF_H(fr) - 1;
    if (s) clip_ctx(ctx, &l, &t, &r, &b);
    op_add(kind, s, l, t, r, b);
    if (s_lastOp) {
        /* a plain keyed blit of an uncompressed-or-RLE frame with no sub-frames
           is the sprite the twin replays by identity; anything else (sub-frame
           stacks, the blended variants) is published as pixels */
        s_lastOp->frame = fr; s_lastOp->pix = *(const void* const*)(fr + 0x10);
        s_lastOp->fw = GF_W(fr); s_lastOp->fh = GF_H(fr); s_lastOp->ck = fr[0x08];
        s_lastOp->dx = (short)(x - GF_HX(fr)); s_lastOp->dy = (short)(y - GF_HY(fr));
        if (kind == OP_GAF && fr[0x0A] != 0) s_lastOp->kind = OP_GAFA;   /* sub-frames: pixels */
    }
}
static int __cdecl before_gaf(void* e)  { if (on_game_thread()) gaf_box(e, OP_GAF);  return 0; }
static int __cdecl before_gafa(void* e) { if (on_game_thread()) gaf_box(e, OP_GAFA); return 0; }
/* 0x4B8310: the blit DrawText 0x4A50E0 takes when globals+0xF0 bit 7 is set,
   same (ctx, frame, x, y) shape [INFERRED from the call site 0x4A5191] */
static int __cdecl before_gafb(void* e) { if (on_game_thread()) gaf_box(e, OP_GAFB); return 0; }

/* ---- 0x4CCF60 glyph blitter, cdecl 9 args -----------------------------
   (base, pitch, font, str, x, y, fg, bg, transparent): writes rows=font[0]
   rows starting at y - (s8)font[2], one glyph after another, width = the
   glyph's own byte (exe-reverse-engineering.md "The in-game bitmap font"). */
static int __cdecl before_text(void* e)
{
    if (s_inFlip) return 0;
    const unsigned char* base = (const unsigned char*)(size_t)ARG(e, 1);
    const unsigned char* font = (const unsigned char*)(size_t)ARG(e, 3);
    const unsigned char* str  = (const unsigned char*)(size_t)ARG(e, 4);
    int x = SARG(e, 5), y = SARG(e, 6);
    int rows, top, w = 0, i, n;
    SURF* s = NULL;
    if (!on_game_thread()) return 0;
    if (!ptr_ok(font) || !ptr_ok(str)) { op_add(OP_TEXT, NULL, 0, 0, 0, 0); return 0; }
    rows = font[0];
    top  = y - (signed char)font[2];
    for (i = 0; i < 256 && str[i] && str[i] != '\n'; i++) {   /* 0x4CCFA0: '\n' ends the draw */
        int c = (int)str[i] - (int)font[3];
        unsigned off;
        if (c < 0) continue;
        off = *(const unsigned short*)(font + 4 + 2 * c);
        if (off) w += font[off];
    }
    n = i;                                       /* the bytes the blitter reads */
    for (i = 0; i < s_nsurf; i++) if (s_surf[i].base == (unsigned)(size_t)base) { s = &s_surf[i]; break; }
    op_add(OP_TEXT, s, x, top, x + w - 1, top + rows - 1);
    /* G17d: the STRING, not the box's bytes. Copied here, on the game thread,
       because the argument is routinely a caller's stack temp and publish runs
       at the flip — the same reason a sprite's pixels are copied (3.5). With no
       room in the scratch the op stays what it was, a box of captured pixels,
       so a full scratch costs the arena and never the picture. */
    if (s_lastOp && n > 0 && (unsigned)n < STR_SCRATCH - s_strUsed) {
        OP* o = s_lastOp;
        memcpy(s_strBuf + s_strUsed, str, (size_t)n);
        s_strBuf[s_strUsed + n] = 0;
        o->soff = s_strUsed;
        o->slen = (unsigned short)n;
        o->frame = font;                         /* the font object            */
        o->dx = (short)x; o->dy = (short)y;      /* what the blitter was GIVEN */
        o->fg = (unsigned char)ARG(e, 7);
        o->bg = (unsigned char)ARG(e, 8);
        o->tr = (unsigned char)ARG(e, 9);
        s_strUsed += (unsigned)n + 1;
    } else if (s_lastOp && n > 0) s_strLost++;
    return 0;
}

/* ---- 0x466780 BuildMinimapSurface: the TNT picture, while it is alive ----
   G17e. The engine fits `main+0x1426B` into a 126-px box and throws half of
   what it has away; the picture itself is 252x252 (or 252x256) and is a GAF
   frame, built at 0x483900..0x483936 and freed at 0x483DF3/0x483E0B. Decoded
   here, on the game thread, into a buffer the render thread reads — the same
   copy-it-now discipline every other op follows, and for a stronger reason:
   the pointer is nulled before the first frame of the map is ever presented.

   Published by bumping a generation AFTER the bytes are in place, so a render
   thread that sees the new generation sees the whole picture. One map load is
   one write; nothing else touches the buffer. */
static unsigned char s_mmPic[TAGPU_GAF_DECMAX * TAGPU_GAF_DECMAX];
static volatile int      s_mmW, s_mmH;
static volatile unsigned s_mmGen;
static unsigned          s_mmFails;

static void mm_fail(const char* why)
{
    char b[160];
    if (s_mmFails++ >= 4) return;
    _snprintf(b, sizeof b, "gui: minimap picture NOT snapshotted — %s (#%u)", why, s_mmFails);
    b[sizeof b - 1] = '\0';
    glog(b);
}

static int __cdecl before_minimap(void* e)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    const unsigned char* fr;
    int w, h;
    (void)e;
    /* every refusal says WHICH one, capped: a silent miss here is a minimap
       that quietly stays the engine's, which looks like nothing at all */
    /* THIS ONE DOES NOT RUN ON THE GAME THREAD, and that is measured, not
       assumed [MEASURED 2026-09-09]: with the usual `on_game_thread()` guard in
       place this observer fired and refused on every map load, naming the
       thread. Every other site in this file is called from the game loop; the
       minimap build is not. So the guard is deliberately absent here, which is
       safe for exactly this handler and would not be for any other:
       `tagpu_gaf_decode` is pure (it reads the frame and writes only the buffer
       it is given — no statics), `s_mmPic` has one writer and one write per map
       load, the generation is published behind a barrier, and nothing engine-
       side is touched. It records nothing into the op ring, which is what makes
       the SPSC contract irrelevant to it. */
    if (!ptr_ok(ta)) { mm_fail("no TAdynmemStruct"); return 0; }
    fr = tagpu_gaf_frame_sane(*(const void* const*)(ta + 0x1426B));
    if (!fr) { mm_fail("main+0x1426B is not a readable GAF frame"); return 0; }
    w = *(const unsigned short*)(fr + TAGPU_GF_W);
    h = *(const unsigned short*)(fr + TAGPU_GF_H);
    if (w <= 0 || h <= 0 || w > TAGPU_GAF_DECMAX || h > TAGPU_GAF_DECMAX) { mm_fail("picture size out of range"); return 0; }
    if (!tagpu_gaf_decode(fr, w, h, s_mmPic)) { mm_fail("decode failed"); return 0; }
    s_mmW = w; s_mmH = h;
    MemoryBarrier();
    s_mmGen++;
    {
        char b[190];
        _snprintf(b, sizeof b, "gui: minimap picture %dx%d snapshotted (gen %u, engine box %dx%d, thread %u%s)",
                  w, h, s_mmGen,
                  (int)*(const short*)(ta + 0x142EB), (int)*(const short*)(ta + 0x142ED),
                  (unsigned)GetCurrentThreadId(),
                  on_game_thread() ? "" : " — NOT the game thread");
        b[sizeof b - 1] = '\0';
        glog(b);
    }
    return 0;
}

/* ---- 0x4BE950 DrawLine(ctx, x0, y0, x1, y1, colour) stdcall ----------- */
static int __cdecl before_line(void* e)
{
    if (s_inFlip) return 0;
    const int* ctx = ctx_or_back(ARG(e, 1));
    int x0 = SARG(e, 2), y0 = SARG(e, 3), x1 = SARG(e, 4), y1 = SARG(e, 5);
    int l = x0 < x1 ? x0 : x1, r = x0 < x1 ? x1 : x0;
    int t = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
    SURF* s;
    if (!on_game_thread()) return 0;
    s = surf_of_ctx(ctx);
    if (s) clip_ctx(ctx, &l, &t, &r, &b);
    op_add(OP_LINE, s, l, t, r, b);
    return 0;
}

/* ---- 0x4BF6F0 DrawBar(ctx, RECT*, colour) and 0x4BF8C0
        DrawTranspRectangle(ctx, RECT*, colour), stdcall; RECT = 4 ints, edges
        inclusive (ui-markers.md §4) -------------------------------------- */
static void rect_box(void* e, int kind)
{
    const int* ctx = ctx_or_back(ARG(e, 1));
    if (s_inFlip) return;
    const int* rc  = (const int*)(size_t)ARG(e, 2);
    int l, t, r, b;
    SURF* s;
    if (!ptr_ok(rc)) { op_add(kind, NULL, 0, 0, 0, 0); return; }
    l = rc[0]; t = rc[1]; r = rc[2]; b = rc[3];
    if (l > r) { int q = l; l = r; r = q; }
    if (t > b) { int q = t; t = b; b = q; }
    s = surf_of_ctx(ctx);
    if (s) clip_ctx(ctx, &l, &t, &r, &b);
    op_add(kind, s, l, t, r, b);
}
static int __cdecl before_bar(void* e)  { if (on_game_thread()) rect_box(e, OP_BAR);  return 0; }
/* 0x4BF4D0: a framed box (three clipped fills), (ctx, RECT*, colour) ret 0xC — what the
   F4 popup 0x4948E0 draws its border with */
static int __cdecl before_frame(void* e) { if (on_game_thread()) rect_box(e, OP_FRAME); return 0; }
static int __cdecl before_rect(void* e) { if (on_game_thread()) rect_box(e, OP_RECT); return 0; }
/* 0x4BF7B0: the focus rectangle GUI_StageUpdateDraw draws last, (ctx, RECT*, colour) */
static int __cdecl before_focus(void* e) { if (on_game_thread()) rect_box(e, OP_RECT); return 0; }

/* ---- 0x4C6B70 surface->surface blit stdcall(dst ctx, src surface, x, y)
        ret 0x10 (the GUI panel reaching the frame; gui-renderer.md §2) -- */
static int __cdecl before_copy(void* e)
{
    const int* dst = ctx_or_back(ARG(e, 1));
    const int* src = ctx_or_back(ARG(e, 2));      /* src NULL = the screen too */
    int x = SARG(e, 3), y = SARG(e, 4);
    int l, t, r, b;
    SURF* s;
    if (!on_game_thread() || excluded_caller(ARG(e, 0))) return 0;
    if (ptr_ok(src)) surf_of_ctx(src);            /* the source is a surface too */
    s = surf_of_ctx(dst);
    if (!ptr_ok(src)) { op_add(OP_COPY, NULL, 0, 0, 0, 0); return 0; }
    /* 0x4CBBE0 lands the whole source at (x - originX, y - originY) */
    x -= *(const short*)((const char*)src + 0x18);
    y -= *(const short*)((const char*)src + 0x1A);
    l = x; t = y; r = x + src[CTX_W] - 1; b = y + src[CTX_H] - 1;
    if (s) clip_ctx(dst, &l, &t, &r, &b);
    op_add(OP_COPY, s, l, t, r, b);
    if (s_lastOp) {
        s_lastOp->src = (unsigned)src[CTX_BASE];
        s_lastOp->sl = (short)(l - x); s_lastOp->st = (short)(t - y);   /* source top-left of the box */
    }
    return 0;
}

/* the allocation's tag rides beside the shared return stack (s_retStack) */
static const char* s_allocTag[32];

/* ---- 0x4C6D20(ctx, desc, RECT* src, RECT* dst) stdcall ret 0x10 — the
        descriptor blit the listbox and textfield handlers use; the box is the
        destination rect (edges inclusive, like every RECT here) ------------ */
static int __cdecl before_gafd(void* e)
{
    if (s_inFlip) return 0;
    const int* ctx = ctx_or_back(ARG(e, 1));
    const int* rc  = (const int*)(size_t)ARG(e, 4);
    int l, t, r, b;
    SURF* s;
    if (!on_game_thread()) return 0;
    if (!ptr_ok(rc)) { op_add(OP_GAFD, NULL, 0, 0, 0, 0); return 0; }
    l = rc[0]; t = rc[1]; r = rc[2]; b = rc[3];
    s = surf_of_ctx(ctx);
    if (s) clip_ctx(ctx, &l, &t, &r, &b);
    op_add(OP_GAFD, s, l, t, r, b);
    return 0;
}

/* ---- 0x4C7580 GAF_DrawTransformed(ctx, src, int xy[6], int uv[6]) stdcall
        ret 0x10 — a TEXTURED TRIANGLE: three screen vertices (x0,y0,x1,y1,x2,y2)
        and their texture coordinates [MEASURED 2026-09-07: the in-game option
        screens' wide backdrop right of the 128-px panel is drawn as these,
        e.g. (214,94)(233,94)(233,113) with uv (1,1)(31,1)(31,31)]. The box is
        the vertices' bounding box, clipped. ------------------------------- */
static int __cdecl before_scale(void* e)
{
    const int* ctx = ctx_or_back(ARG(e, 1));
    const int* xy = (const int*)(size_t)ARG(e, 3);
    int l, t, r, b, i;
    SURF* s;
    if (!on_game_thread() || s_inFlip) return 0;
    if (!ptr_ok(xy)) { op_add(OP_SCALE, NULL, 0, 0, 0, 0); return 0; }
    l = r = xy[0]; t = b = xy[1];
    for (i = 1; i < 3; i++) {
        if (xy[2 * i] < l) l = xy[2 * i];
        if (xy[2 * i] > r) r = xy[2 * i];
        if (xy[2 * i + 1] < t) t = xy[2 * i + 1];
        if (xy[2 * i + 1] > b) b = xy[2 * i + 1];
    }
    s = surf_of_ctx(ctx);
    if (s) clip_ctx(ctx, &l, &t, &r, &b);
    op_add(OP_SCALE, s, l, t, r, b);
    return 0;
}

/* ---- 0x4C6890 SurfaceFill(surface, colour) stdcall ret 8: the whole
        surface (NULL = the back buffer) ----------------------------------- */
static int __cdecl before_fill(void* e)
{
    if (s_inFlip) return 0;
    const int* ctx = ctx_or_back(ARG(e, 1));
    SURF* s;
    if (!on_game_thread()) return 0;
    s = surf_of_ctx(ctx);
    op_add(OP_FILL, s, 0, 0, s ? s->w - 1 : 0, s ? s->h - 1 : 0);
    return 0;
}

/* ---- 0x4C6AC0 SurfaceFree(surface) stdcall ret 4: forget it ------------- */
static int __cdecl before_free(void* e)
{
    const int* obj = (const int*)(size_t)ARG(e, 1);
    int i;
    if (!on_game_thread() || !ptr_ok(obj)) return 0;
    for (i = 0; i < s_nsurf; i++)
        if (s_surf[i].base == (unsigned)obj[CTX_BASE]) { surf_drop(i); break; }
    return 0;
}

/* ---- 0x4A81E0 GUI_StageUpdateDraw(gi, flags) stdcall ret 8: the one place
        a screen's surface is drawn into. Traced, not boxed: it tells the
        census which screen was built or redrawn between two flips -------- */
static int __cdecl before_build(void* e)
{
    if (!on_game_thread()) return 0;
    s_builds++;
    s_buildFlags |= ARG(e, 2);
    if (s_trace && (ARG(e, 2) & 1)) s_gafDbg = 6;      /* a build: trace its first blits */
    return 0;
}

/* ---- 0x4C69F0 SurfaceCreateNamed(tag, w, h) stdcall ret 0xC: register the
        new surface from its returned object (resolution.md §3.3) --------- */
static int __cdecl before_alloc(void* e)
{
    (void)e;
    return on_game_thread() || !s_gameTid;   /* hijack the return: we want eax */
}
static void* __cdecl after_alloc(unsigned int* regs)
{
    const int* obj = (const int*)(size_t)regs[7];        /* eax */
    void* ret = s_retDepth > 0 ? s_retStack[--s_retDepth] : NULL;
    const char* tag = s_allocTag[s_retDepth];
    SURF* s = ptr_ok(obj) ? surf_of_ctx(obj) : NULL;
    if (s && tag == (const char*)(size_t)TAG_OFFSCREEN) { s->isOffscreen = 1; surf_drop_offscreens(s->base); }
    if (s && s_trace) {
        char b[200];
        _snprintf(b, sizeof b, "gui trace: alloc \"%.32s\" %dx%d base %08X (screen %s)",
                  ptr_ok(tag) ? tag : "?", s->w, s->h, s->base, top_screen_name());
        glog(b);
    }
    if (s && (s_census || g_gui_draw)) {
        s->seeded = 0;                       /* a fresh surface: the twin must be re-seeded */
        /* seed NOW, while the surface is blank, so the first census after a
           screen's build diffs the build itself rather than adopting it */
        const unsigned char* cur = (const unsigned char*)(size_t)s->base;
        int y;
        if (!s->copy) s->copy = (unsigned char*)malloc((size_t)s->w * s->h);
        if (!s->mask) s->mask = (unsigned char*)malloc((size_t)s->w * s->h);
        if (s->copy && s->mask && ptr_ok(cur)) {
            for (y = 0; y < s->h; y++) memcpy(s->copy + (size_t)y * s->w, cur + (size_t)y * s->pitch, (size_t)s->w);
            memset(s->mask, 0, (size_t)s->w * s->h);
            s->copyValid = 1;
        }
    }
    return ret;
}

static int __cdecl before_alloc_push(void* e)
{
    if (!before_alloc(e) || s_retDepth >= 32) return 0;
    s_allocTag[s_retDepth] = (const char*)(size_t)ARG(e, 1);
    s_retStack[s_retDepth++] = (void*)(size_t)ARG(e, 0);
    return 1;
}

/* ---- the flip's source: the OFFSCREEN the engine flips is the global
        one at main+0x37E1B [to be replaced by the flip's own chain once the
        RE names it] ------------------------------------------------------- */
static const int* flip_source(void* entry_esp)
{
    (void)entry_esp;
    return back_buffer();          /* the flip copies *(globals+0xBC) to the primary */
}

/* ---- the table ---------------------------------------------------------- */
typedef struct LEAF {
    unsigned va;
    const char* name;
    unsigned char stolen[10];
    int nst;
    tagpu_detour_before_fn before;
    tagpu_detour_after_fn  after;
} LEAF;

static const LEAF LEAVES[] = {
    { 0x004B7F90u, "gaf",   { 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 }, 6, before_gaf,  NULL },
    { 0x004B8500u, "gafa",  { 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 }, 6, before_gafa, NULL },
    { 0x004CCF60u, "text",  { 0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF0 }, 6, before_text, NULL },
    { 0x004BE950u, "line",  { 0x83, 0xEC, 0x30, 0x56, 0x8B, 0x74, 0x24, 0x38 }, 8, before_line, NULL },
    { 0x004BF6F0u, "bar",   { 0x83, 0xEC, 0x40, 0x8B, 0x44, 0x24, 0x48 }, 7, before_bar,  NULL },
    { 0x004BF8C0u, "rect",  { 0x83, 0xEC, 0x68, 0x53, 0x56, 0x57 }, 6, before_rect, NULL },
    { 0x004C6B70u, "copy",  { 0x83, 0xEC, 0x30, 0x56, 0x8B, 0x74, 0x24, 0x38 }, 8, before_copy, NULL },
    { 0x004C69F0u, "alloc", { 0x53, 0x56, 0x8B, 0x74, 0x24, 0x10 }, 6, before_alloc_push, after_alloc },
    { 0x004B8310u, "gafb",  { 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 }, 6, before_gafb,  NULL },
    { 0x004C6D20u, "gafd",  { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x30 }, 7, before_gafd, NULL },
    { 0x004C7580u, "scale", { 0xB8, 0x8C, 0x7D, 0x00, 0x00 }, 5, before_scale, NULL },
    { 0x004BF7B0u, "focus", { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x57 }, 7, before_focus, NULL },
    { 0x004C6890u, "fill",  { 0x83, 0xEC, 0x64, 0x53, 0x55, 0x56, 0x57 }, 7, before_fill,  NULL },
    { 0x004C6AC0u, "free",  { 0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0 }, 6, before_free,  NULL },
    { 0x004A81E0u, "build", { 0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0xC0, 0x03, 0x00, 0x00 }, 10, before_build, NULL },
    { 0x004BF4D0u, "frame", { 0x83, 0xEC, 0x40, 0x53, 0x55, 0x56, 0x57 }, 7, before_frame, NULL },
    /* G17e: NOT a pixel-writing leaf. `BuildMinimapSurface 0x466780` is the one
       place the TNT's own minimap picture (`main+0x1426B`, TED_GENERATED_PIC) is
       alive and reachable: it consumes the frame at 0x46684F, its single caller
       is 0x4669B0 and the loader frees the picture at 0x483DF3/0x483E0B in a
       function that calls neither. An observer at its ENTRY therefore sees the
       picture by construction, and needs to know nothing about the loader.
       It rides the same table so it takes the same all-or-nothing byte match. */
    { 0x00466780u, "minimap", { 0x83, 0xEC, 0x40, 0x53, 0x8B, 0x1D, 0xE8, 0x1D, 0x51, 0x00 }, 10, before_minimap, NULL },
};
#define LEAF_COUNT ((int)(sizeof LEAVES / sizeof LEAVES[0]))

static int leaves_match(void)
{
    int i;
    for (i = 0; i < LEAF_COUNT; i++)
        if (!tagpu_detour_bytes_ok(LEAVES[i].va, LEAVES[i].stolen, LEAVES[i].nst)) {
            char b[120];
            _snprintf(b, sizeof b, "gui: bytes differ at %s 0x%08X", LEAVES[i].name, LEAVES[i].va);
            glog(b);
            return 0;
        }
    return 1;
}

static int leaves_install(void)
{
    int i, n = 0;
    for (i = 0; i < LEAF_COUNT; i++)
        n += tagpu_detour_observe(LEAVES[i].va, LEAVES[i].stolen, LEAVES[i].nst,
                                  LEAVES[i].before, LEAVES[i].after) ? 1 : 0;
    return n;
}
