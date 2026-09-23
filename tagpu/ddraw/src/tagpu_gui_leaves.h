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
/* `TA_GFX_PP` comes from inc/tagpu_engine.h, which tagpu_gui_hook.c -- this
   file's only includer -- includes above it. There is no local spelling of
   0x51FBD0 any more. [FROM REVIEW, landing 8d.] */
#define GFX_BACKBUF    0xBC
#define GFX_BACKBUF_ON 0xDC
static const int* back_buffer(void)
{
    const char* g = *(const char* const*)TA_GFX_PP;
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

/* THE BLIT'S TAIL, WITHOUT THE STACK FRAME -- everything `gaf_box` does once it
   knows the destination and the frame. It is factored out because the HUD
   chrome's re-emit (`chrome_emit`, tagpu_gui_hook.c) has to produce ops that are
   indistinguishable from an observed blit's: same box arithmetic, same identity
   hash, same first-sight plane capture. A second copy of this would be a second
   place for the sprite/pixels decision to drift.

   `ctx` may be NULL, which is the re-emit's case: there is no engine clip rect
   to honour because no engine call is in flight, and `op_add` already clamps the
   box to the surface. Every other caller passes the one it was handed. */
static void gaf_record(const int* ctx, SURF* s, const unsigned char* fr,
                       int x, int y, int kind)
{
    int l, t, r, b;
    if (!ptr_ok(fr)) { op_add(kind, NULL, 0, 0, 0, 0); return; }
    l = x - GF_HX(fr); t = y - GF_HY(fr);
    r = l + GF_W(fr) - 1; b = t + GF_H(fr) - 1;
    if (s && ctx) clip_ctx(ctx, &l, &t, &r, &b);
    op_add(kind, s, l, t, r, b);
    if (s_lastOp) {
        /* a plain keyed blit of an uncompressed-or-RLE frame with no sub-frames
           is the sprite the twin replays by identity; anything else (sub-frame
           stacks, the blended variants) is published as pixels */
        s_lastOp->frame = fr; s_lastOp->pix = *(const void* const*)(fr + 0x10);
        s_lastOp->fw = GF_W(fr); s_lastOp->fh = GF_H(fr); s_lastOp->ck = fr[0x08];
        s_lastOp->dx = (short)(x - GF_HX(fr)); s_lastOp->dy = (short)(y - GF_HY(fr));
        if (kind == OP_GAF && fr[0x0A] != 0) s_lastOp->kind = OP_GAFA;   /* sub-frames: pixels */
        /* THE IDENTITY AND THE PLANE, TAKEN HERE (G19f-7). We are inside the
           engine's own blit of this frame, which is the only moment the art is
           alive by the engine's ordering rather than by our hope; `publish`
           runs up to CENSUS_MS later, after a screen pop may have freed it.
           Only the sprite kinds resolve a frame at publish, so only they pay
           it. See `gaf_capture`.
           THE RE-EMIT HAS ITS OWN ORDERING for this same read and it is not
           this one -- it is not inside any engine call. See `chrome_emit`. */
        s_lastOp->fcomp = fr[0x09]; s_lastOp->fsub = fr[0x0A]; s_lastOp->fsubn = fr[0x0B];
        /* ONLY WHEN SOMETHING WILL CONSUME IT. `publish` returns at once when
           `!g_gui_draw`, and a census-less, draw-less window is thrown away
           unread (`s_nops = 0`) -- so without this test the decode ran inside
           the engine's blit for every new frame, every window, for a queue
           nobody would read, and with the seen table never filling every blit
           was a first sight. `after_alloc` below already gates on exactly this
           pair, which is the precedent.
           [FOUND 2026-09-16 by BOTH landing reviewers, independently.] */
        if ((s_census || g_gui_draw) &&
            s_lastOp->kind == OP_GAF && s_lastOp->fw && s_lastOp->fh &&
            s_lastOp->fw <= TAGPU_GAF_DECMAX && s_lastOp->fh <= TAGPU_GAF_DECMAX)
            gaf_capture(s_lastOp, fr);       /* publish's own test, so the
                                                scratch is never spent on a
                                                frame it will refuse anyway */
    }
}

static void gaf_box(void* e, int kind)
{
    const int* ctx = ctx_or_back(ARG(e, 1));
    const unsigned char* fr = (const unsigned char*)(size_t)ARG(e, 2);
    int x = SARG(e, 3), y = SARG(e, 4);
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
    gaf_record(ctx, s, fr, x, y, kind);
}
static int __cdecl before_gaf(void* e)  { if (on_game_thread()) gaf_box(e, OP_GAF);  return 0; }
static int __cdecl before_gafa(void* e) { if (on_game_thread()) gaf_box(e, OP_GAFA); return 0; }
/* 0x4B8310: the blit DrawText 0x4A50E0 takes when globals+0xF0 bit 7 is set.
   NOT the same shape as 0x4B7F90: stdcall with FIVE args (ret 0x14), it draws
   nothing unless that bit is set, and it BLENDS -- a raw frame goes to
   0x4CBF2C, which writes dst = [globals+0xC8][src * 256 + dst] for every src
   pixel other than arg 5. The first four args are 0x4B7F90's, which is all
   `gaf_box` reads; publish sends the op as box bytes, which the drain drops.
   [VERIFIED 2026-09-23 by disassembly; the engine map's leaf table.] */
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
        o->frame = font;                         /* the font's ADDRESS, as an
                                                    identity: `op_same` and the
                                                    probe use it, and since
                                                    G19f-8 nothing follows it  */
        o->dx = (short)x; o->dy = (short)y;      /* what the blitter was GIVEN */
        o->fg = (unsigned char)ARG(e, 7);
        o->bg = (unsigned char)ARG(e, 8);
        o->tr = (unsigned char)ARG(e, 9);
        s_strUsed += (unsigned)n + 1;
        /* AND THE FONT, HERE, WHERE THE ENGINE IS ABOUT TO READ IT (G19f-8).
           We are at the head of `0x4CCF60` with its own arguments, one
           instruction before it walks this string through this font; `publish`
           runs up to CENSUS_MS later over memory nothing here can say is still
           mapped. See `text_capture`.

           ONLY WHEN SOMETHING WILL CONSUME IT: `publish` returns at once when
           `!g_gui_draw`, and a census-less, draw-less window is thrown away
           unread, so without this test a font would be walked for every text
           draw of every such window for a queue nobody reads. `gaf_capture`
           above gates on exactly this pair and `after_alloc` below it does
           too. `nostring` is the lever that turns the string op off outright,
           and under it the capture would be scratch spent on an op that
           publishes its box anyway. */
        if ((s_census || g_gui_draw) && !s_nostring)
            text_capture(o, font, s_strBuf + o->soff, n);   /* our copy, just
                                                    taken: same bytes, one
                                                    fewer read of the engine's */
    } else if (s_lastOp && n > 0) s_strLost++;
    return 0;
}

/* THE MINIMAP PICTURE'S OBSERVER IS GONE (frame packet exchange, landing 4c).
   It sat at `BuildMinimapSurface 0x466780`'s entry and decoded `main+0x1426B`
   into a buffer of ours, on the LOADER thread — the one publisher the plan's
   rule forbids outright. It was there because this file said the picture was
   alive only inside that call. **That was wrong** [CORRECTED 2026-09-12 by
   objdump of the pristine build]: LoadMap stores the picture at `0x483900` and
   the ONLY thing that frees it is `0x483DFE`, inside `0x483DD0`, whose only
   caller is `0x491BB3` — the level TEARDOWN cascade. `0x466780` reads it at
   `0x46684F` and does not null it. So the picture is alive for the whole level,
   the packet's publisher decodes it itself on the level's first in-play draw,
   and nothing of ours runs on the loader thread any more. */

/* ---- 0x4BE950 DrawLine(ctx, x0, y0, x1, y1, colour) stdcall ----------- */
static int __cdecl before_line(void* e)
{
    if (s_inFlip) return 0;
    const int* ctx = ctx_or_back(ARG(e, 1));
    int x0 = SARG(e, 2), y0 = SARG(e, 3), x1 = SARG(e, 4), y1 = SARG(e, 5);
    int l = x0 < x1 ? x0 : x1, r = x0 < x1 ? x1 : x0;
    int t = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
    int ol, ot, orr, ob;
    /* AXIS-ALIGNED OR DIAGONAL, DECIDED HERE AND KEPT AS TWO OP KINDS
       [landing 8c]. For an axis-aligned line the bounding box IS the line --
       one pixel thick -- so it is a solid fill and ports exactly as `OP_BAR`
       does. For a diagonal the box is emphatically NOT the line: it is the
       square the line crosses, and replaying it would paint the whole square.
       That is the fault gui-renderer.md 20 traced to cyan squares, and this
       split is what stops landing 8c committing it again.
       Two kinds rather than a flag, because the CENSUS then counts them apart
       and the next session reads the ratio off `GUI kinds:` instead of
       guessing it -- the same move that made landing 8b honest about `focus`. */
    int diag = (x0 != x1 && y0 != y1);
    SURF* s;
    if (!on_game_thread()) return 0;
    ol = l; ot = t; orr = r; ob = b;
    s = surf_of_ctx(ctx);
    if (s) clip_ctx(ctx, &l, &t, &r, &b);
    op_add(diag ? OP_DIAG : OP_LINE, s, l, t, r, b);
    if (s_lastOp) {
        /* THE COLOUR IS THE SIXTH ARGUMENT and one byte of it reaches the
           surface: `0x4BE950` writes through `0x4CC7AB`, whose colour is
           `[ebp+0x1C]` stored `stos BYTE al` (exe-reverse-engineering.md). */
        s_lastOp->col = (unsigned char)ARG(e, 6);
        s_lastOp->clipped = (unsigned char)(l != ol || t != ot || r != orr || b != ob);
    }
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
    int ol, ot, orr, ob;
    SURF* s;
    if (!ptr_ok(rc)) { op_add(kind, NULL, 0, 0, 0, 0); return; }
    l = rc[0]; t = rc[1]; r = rc[2]; b = rc[3];
    if (l > r) { int q = l; l = r; r = q; }
    if (t > b) { int q = t; t = b; b = q; }
    ol = l; ot = t; orr = r; ob = b;        /* before the clamp, for `clipped` */
    s = surf_of_ctx(ctx);
    if (s) clip_ctx(ctx, &l, &t, &r, &b);
    op_add(kind, s, l, t, r, b);
    /* AND THE THIRD ARGUMENT, on the op `op_add` just recorded -- the same
       decoration `gaf_box` and `before_copy` make. Every early return above
       leaves `s_lastOp` NULL, so a clipped-away or surface-less op cannot write
       this into the PREVIOUS one.
       IT IS A PALETTE INDEX ONLY FOR `OP_BAR` (`0x4BF6F0`, whose writer
       `0x4CCDEA` takes the low byte and nothing wider). For `OP_FRAME` it is a
       SIGNED SHADE LEVEL and for the two `OP_RECT` leaves it reaches a
       different writer again -- `OP::col` in tagpu_gui_hook.c has all four.
       Recorded for all four anyway because the raw argument is what the
       observer saw; `publish` reads it for `OP_BAR` alone, and 8b/8c/8d each
       have to decide what their own means before reading it.
       [The vulkan-only plan, landing 8a; the other three CORRECTED by its
       review.] */
    if (s_lastOp) {
        s_lastOp->col = (unsigned char)ARG(e, 3);
        /* AND WHETHER THE CLAMP ABOVE MOVED AN EDGE -- `OP::clipped` says why
           `OP_RECT` must not be described as geometry when it did. */
        s_lastOp->clipped = (unsigned char)(l != ol || t != ot || r != orr || b != ob);
    }
}
static int __cdecl before_bar(void* e)  { if (on_game_thread()) rect_box(e, OP_BAR);  return 0; }
/* 0x4BF4D0: NOT a fill — a SHADE of what is already in the box, (ctx, RECT*, level)
   ret 0xC; one clip through 0x4BF620, then every pixel remapped through a 256-byte
   row of globals+0xC4 (darken) or +0xC8 (lighten). What the F4 popup 0x4948E0 draws
   its border with (×3 — three calls, not three fills). [CORRECTED 2026-09-18.] */
static int __cdecl before_frame(void* e) { if (on_game_thread()) rect_box(e, OP_FRAME); return 0; }
static int __cdecl before_rect(void* e) { if (on_game_thread()) rect_box(e, OP_RECT); return 0; }
/* 0x4BF7B0: the focus rectangle GUI_StageUpdateDraw draws last, (ctx, RECT*, level).
   ITS OWN KIND SINCE LANDING 8b, not OP_RECT: four edges of one box through
   0x4BEC70, whose writer 0x4CC8DF reads the destination and remaps it through
   globals+0xC8. A tint, not a colour -- see the OP_FOCUS comment in
   tagpu_gui_hook.c. [The "eight edges" this said at first were two mutually
   exclusive arms on ctx == NULL; corrected by 8b's review.]

   AND SINCE LANDING 8d IT RECORDS THE FOUR EDGES AS FOUR OPS, in the engine's
   own order, rather than one op for the box. Three things fall out of that and
   each of them was a reason the box could not be published:

     - THE CLIP. `0x4BF7B0` hands each edge to `0x4BEC70` separately and
       `0x4BEA20` clips each on its own, so a rectangle crossing the clip rect
       is an OPEN figure -- exactly `OP_RECT::clipped`'s problem, which that
       kind answers by falling back to the box's bytes. A tint has no such
       fallback (there is no colour to publish), so the decomposition IS the
       answer: `op_add` already drops an edge that clips to nothing and clamps
       the rest, which is what `0x4BEA20` does, one edge at a time.
     - THE CORNERS. Top covers (l,t)..(r,t) inclusive at both ends and left
       covers (l,t)..(l,b), so (l,t) is remapped TWICE -- `LUT[LUT[x]]` -- and
       so are the other three corners. Four sequential ops reproduce that; one
       box op would have had to carry the rule.
     - THE AREA. `s_kindArea[OP_FOCUS]` now counts the pixels the engine writes
       instead of the bounding box, so `gui area:`'s `focus` figure is the
       traffic and not an over-estimate of it. The numbers in gui-renderer.md
       from before this landing are the old measure.

   Disassembled 2026-09-21 for this landing: both arms draw (l,t,r,t),
   (r,t,r,b), (l,b,r,b), (l,t,l,b) with the level passed straight through as
   `0x4BEC70`'s sixth argument and `0x4CC8DF`'s sixth. The RECT fields are
   [esi]=l, [esi+4]=t, [esi+8]=r, [esi+0xC]=b. */
static int __cdecl before_focus(void* e)
{
    const int* ctx;
    const int* rc;
    int box[4], k, lvl;
    SURF* s;
    /* the four edges, in the engine's order: top, right, bottom, left */
    static const unsigned char EDGE[4][4] = {
        { 0, 1, 2, 1 },   /* (l, t) - (r, t) */
        { 2, 1, 2, 3 },   /* (r, t) - (r, b) */
        { 0, 3, 2, 3 },   /* (l, b) - (r, b) */
        { 0, 1, 0, 3 }    /* (l, t) - (l, b) */
    };
    if (!on_game_thread()) return 0;
    if (s_inFlip) return 0;
    ctx = ctx_or_back(ARG(e, 1));
    rc  = (const int*)(size_t)ARG(e, 2);
    if (!ptr_ok(rc)) { op_add(OP_FOCUS, NULL, 0, 0, 0, 0); return 0; }
    /* THE ROW, BOUNDED HERE BECAUSE THE ENGINE DOES NOT BOUND IT ANYWHERE.
       `0x4BF7B0` passes this int through `0x4BEC70` unmasked and `0x4CC8DF`
       does `shl eax,0x8` on it, so a level outside 0..31 reads OFF the 32-row
       table -- the engine's own bug, and one whose output is whatever memory
       follows the table. We cannot reproduce that and will not guess at it, so
       such an op is refused and counted; the counter is what says it never
       happens. It never can from the one caller: `0x4A16F0` starts at 0x1F and
       walks 31, 28, 24, 19, 13, 6 over six expanding rectangles. */
    lvl = SARG(e, 3);
    if (lvl < 0 || lvl >= (int)TAGPU_GUI_SHADE_ROWS) { s_focusRowBad++; return 0; }
    box[0] = rc[0]; box[1] = rc[1]; box[2] = rc[2]; box[3] = rc[3];
    /* NORMALISING IS EXACT HERE where it would not be for a filled box: each
       edge is a SEGMENT and `0x4CC8DF` swaps its own endpoints (`0x4CC8FA`,
       `0x4CC94E`), so (l,t)-(r,t) and (r,t)-(l,t) write the same pixels. */
    if (box[0] > box[2]) { int q = box[0]; box[0] = box[2]; box[2] = q; }
    if (box[1] > box[3]) { int q = box[1]; box[1] = box[3]; box[3] = q; }
    s = surf_of_ctx(ctx);
    for (k = 0; k < 4; k++) {
        int l = box[EDGE[k][0]], t = box[EDGE[k][1]];
        int r = box[EDGE[k][2]], b = box[EDGE[k][3]];
        if (s) clip_ctx(ctx, &l, &t, &r, &b);
        op_add(OP_FOCUS, s, l, t, r, b);
        /* `col` IS THE ROW, not a palette index -- `OP::col` in
           tagpu_gui_hook.c has what the argument means for each of the four
           `rect_box` leaves, and this is the one that means a shade level. */
        /* `edge` IS PART OF THE IDENTITY, not decoration: see `op_same`.
           A one-pixel-tall rect makes edges 0 and 2 the same box. */
        if (s_lastOp) { s_lastOp->col = (unsigned char)lvl;
                        s_lastOp->edge = (unsigned char)k; }
    }
    return 0;
}

/* ---- 0x4C6B70 surface->surface blit stdcall(dst ctx, src surface, x, y)
        ret 0x10 (the GUI panel reaching the frame; gui-renderer.md §2) -- */
/* THE BODY OF THE COPY LEAF, REACHABLE WITHOUT A DETOUR FRAME. Factored out
   for the same reason `gaf_record` was: the panel re-emit in `tagpu_gui_hook.c`
   has to produce an op the publisher cannot tell apart from an observed one, and
   the way to guarantee that is to run the same code rather than a second copy of
   the arithmetic. `dst` may be NULL -- the re-emit reaches the destination as a
   SURF and has no context to clip against, and the engine's own panel blit is
   clipped by the surface bounds alone, which `op_add` already applies. */
static void copy_record(SURF* s, const int* dst, const int* src, int x, int y)
{
    int l, t, r, b;
    /* 0x4CBBE0 lands the whole source at (x - originX, y - originY) */
    x -= *(const short*)((const char*)src + 0x18);
    y -= *(const short*)((const char*)src + 0x1A);
    l = x; t = y; r = x + src[CTX_W] - 1; b = y + src[CTX_H] - 1;
    if (s && dst) clip_ctx(dst, &l, &t, &r, &b);
    op_add(OP_COPY, s, l, t, r, b);
    if (s_lastOp) {
        s_lastOp->src = (unsigned)src[CTX_BASE];
        s_lastOp->sl = (short)(l - x); s_lastOp->st = (short)(t - y);   /* source top-left of the box */
    }
}

static int __cdecl before_copy(void* e)
{
    const int* dst = ctx_or_back(ARG(e, 1));
    const int* src = ctx_or_back(ARG(e, 2));      /* src NULL = the screen too */
    SURF* s;
    if (!on_game_thread() || excluded_caller(ARG(e, 0))) return 0;
    if (ptr_ok(src)) surf_of_ctx(src);            /* the source is a surface too */
    s = surf_of_ctx(dst);
    if (!ptr_ok(src)) { op_add(OP_COPY, NULL, 0, 0, 0, 0); return 0; }
    copy_record(s, dst, src, SARG(e, 3), SARG(e, 4));
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

/* ---- 0x4C7580 GAF_DrawTransformed(ctx, src, int xy[8], int uv[8]) stdcall
        ret 0x10 — a TEXTURED QUAD: FOUR screen vertices and their texture
        coordinates. [CORRECTED 2026-09-21 by the landing review, which
        disassembled the loop: `0x4C763D..0x4C7679` walks arg3 with `add
        edx,0x8` and `cmp ecx,0x4; jl`, so it is four vertices at stride 8, not
        three. The earlier "three vertices (x0,y0,x1,y1,x2,y2)" reading came
        from the 2026-09-07 sighting of the option screens' backdrop, where
        only the first three were looked at.]

        When `uv` is NULL the engine synthesises its own quad
        `(0,0) (w-1,0) (w-1,h-1) (0,h-1)` at `[esp+0x4C..0x68]` — note `w-1`,
        not `w`, which is why the whole-frame test below cannot be relaxed to
        cover those callers without changing what it compares.

        The box is the vertices' bounding box over ALL FOUR, clipped. ------ */
static int __cdecl before_scale(void* e)
{
    const int* ctx = ctx_or_back(ARG(e, 1));
    const unsigned char* fr = (const unsigned char*)(size_t)ARG(e, 2);
    const int* xy = (const int*)(size_t)ARG(e, 3);
    const int* uv = (const int*)(size_t)ARG(e, 4);
    int l, t, r, b, i, dw = 0, dh = 0, plain = 0;
    int winU = 0, winV = 0, winW = 0, winH = 0;   /* the source window, frame texels */
    SURF* s;
    if (!on_game_thread() || s_inFlip) return 0;
    if (!ptr_ok(xy)) { op_add(OP_SCALE, NULL, 0, 0, 0, 0); return 0; }
    l = r = xy[0]; t = b = xy[1];
    /* FOUR, not three: the engine's own loop reads four vertices, so a box over
       three of them can be too small. [The landing review's.] */
    for (i = 1; i < 4; i++) {
        if (xy[2 * i] < l) l = xy[2 * i];
        if (xy[2 * i] > r) r = xy[2 * i];
        if (xy[2 * i + 1] < t) t = xy[2 * i + 1];
        if (xy[2 * i + 1] > b) b = xy[2 * i + 1];
    }
    /* AN AXIS-ALIGNED, UNCLIPPED, WHOLE-FRAME STAMP IS A SPRITE -- and NOTHING
       ELSE IS CLAIMED HERE. The three vertices are origin, +u and +u+v, so the
       stamp is axis-aligned exactly when v0 and v1 share a y and v1 and v2 share
       an x, and it covers the whole frame exactly when the uv triangle runs
       (0,0) (w,0) (w,h). The in-game player badge is that case, measured
       2026-09-21 as `xy=(132,5)(152,5)(152,25) uv=(0,0)(32,0)(32,32)`. A rotated
       or sheared stamp, a partial uv window, or one the context clipped keeps
       the old behaviour and publishes its box: the bound is the test, not a
       belief about what the engine draws.

       AND THE EXTENT IS HALF-OPEN, which the vertices' bounding box is not. The
       far vertex is the edge the span stops BEFORE, not a pixel: golden draws
       that badge over x 132..151, twenty pixels, where the bbox says 132..152.
       Taking the bbox made it twenty-one wide and put every interior line a
       texel out. So the destination is `xy[2]-xy[0]` by `xy[5]-xy[1]`, and that
       same span is the resample denominator. [The bars were one pixel too wide
       for exactly this reason in b0b867a; this is the same mistake in a second
       place, found by measuring the extent rather than trusting the box.]

       `fr[0x0A]` is the sub-frame count, refused for the same reason
       `gaf_record` turns such a frame into OP_GAFA: a stack is not one plane. */
    /* ALL FOUR VERTICES, or the shape is not the rectangle this claims it is.
       `xy[6]`/`xy[7]` (v3) went uninspected until the landing review: the test
       pinned v0's row, v1's column and the u/v extents, which a quad whose
       fourth corner sits anywhere at all still satisfies -- and such a quad
       would have been published as an axis-aligned sprite. Every caller
       disassembled so far builds a true rectangle, so this corrects a test that
       was too weak rather than a picture that was wrong. */
    if (ptr_ok(fr) && ptr_ok(uv) && fr[0x0A] == 0 &&
        xy[1] == xy[3] && xy[2] == xy[4] &&
        xy[6] == xy[0] && xy[7] == xy[5] &&
        uv[1] == uv[3] && uv[2] == uv[4] &&
        uv[6] == uv[0] && uv[7] == uv[5]) {
        /* THE SOURCE IS A WINDOW, NOT NECESSARILY THE WHOLE FRAME [landing 8e].
           The quad's u/v corners are axis-aligned by the four tests above, so
           the source is the rectangle (uv[0], uv[1]) with the same half-open
           extents the destination uses. SKIRMISH.GUI's four player swatches are
           the case that forced this: measured 2026-09-21 as
           `xy=(214,94)(233,94)(233,113)(214,113) uv=(1,1)(31,1)(31,31)(1,31)`
           against a 32x32 frame -- a 30x30 window inset one texel, resampled to
           19x19. They were the ENTIRE remaining residual of the shell (1 444 px,
           `raw=62400 pct=0.23`), and the old test refused them on `uv[0] == 0`
           alone. Nothing else about the shape changed: this widened what counts
           as a source, not what counts as an axis-aligned stamp. */
        int su = uv[0], sv = uv[1];
        int sww = uv[2] - uv[0], swh = uv[5] - uv[1];
        int fw = (int)GF_W(fr), fh = (int)GF_H(fr);
        dw = xy[2] - xy[0]; dh = xy[5] - xy[1];
        /* THE WINDOW IS BOUNDED BY THE FRAME, and the frame's own header is
           engine DATA -- so this is the bound that makes `scale_capture`'s
           reads facts rather than arithmetic it hopes about. */
        if (dw > 0 && dh > 0 && dw <= TAGPU_GAF_DECMAX && dh <= TAGPU_GAF_DECMAX &&
            su >= 0 && sv >= 0 && sww > 0 && swh > 0 &&
            fw > 0 && fh > 0 && su + sww <= fw && sv + swh <= fh &&
            l == xy[0] && t == xy[1]) {
            /* AND IT MUST BE KEYABLE. The consumer tells two windows of one
               frame apart by this packed value alone, so a window that will
               not fit it keeps the old path rather than claiming an identity
               it does not have. 0 is reserved for the whole frame, which is
               what every 1:1 sprite and the in-game badge publish, so their
               atlas keys are bit-for-bit what they were. */
            if (su == 0 && sv == 0 && sww == fw && swh == fh) {
                r = l + dw - 1; b = t + dh - 1; plain = 1;
            } else if (su <= 255 && sv <= 255 && sww <= 255 && swh <= 255) {
                r = l + dw - 1; b = t + dh - 1; plain = 2;
            }
            if (plain) { winU = su; winV = sv; winW = sww; winH = swh; }
        }
    }
    s = surf_of_ctx(ctx);
    if (s) {
        int cl = l, ct = t, cr = r, cb = b;
        clip_ctx(ctx, &cl, &ct, &cr, &cb);
        if (cl != l || ct != t || cr != r || cb != b) plain = 0;   /* clipped: box it */
        l = cl; t = ct; r = cr; b = cb;
    }
    op_add(OP_SCALE, s, l, t, r, b);
    if (plain && s_lastOp) {
        OP* o = s_lastOp;
        o->frame = fr; o->pix = *(const void* const*)(fr + 0x10); o->ck = fr[0x08];
        o->fw = (unsigned short)dw; o->fh = (unsigned short)dh;
        o->dx = (short)l; o->dy = (short)t;
        o->fcomp = fr[0x09]; o->fsub = fr[0x0A]; o->fsubn = fr[0x0B];
        o->su  = (unsigned short)winU;  o->sv  = (unsigned short)winV;
        o->sww = (unsigned short)winW; o->swh = (unsigned short)winH;
        /* 0 = the whole frame; `plain == 2` is the windowed case, and the pack
           cannot produce 0 there because both extents are > 0 */
        o->swin = (plain == 2)
                ? ((unsigned)winU | ((unsigned)winV << 8) |
                   ((unsigned)winW << 16) | ((unsigned)winH << 24))
                : 0u;
        /* the same gate `gaf_record` uses: never decode for a window nobody reads */
        if (s_census || g_gui_draw) scale_capture(o, fr, dw, dh);
        /* AFTER the capture, and on its result: `glen` is what `publish` tests
           before it makes a `PK_SPRITE`, so counting the same thing keeps the
           census and the queue telling one story.

           FROM THE OP'S OWN BOX, not from the locals. `op_add` clamps to the
           SURFACE before it accumulates `s_kindArea`, and these locals were
           only clipped to the CONTEXT -- so taking them here would let the
           semantic half exceed the total it is subtracted from whenever the
           two rectangles differ. Reading `o->l..o->b` back is the same box
           `s_kindArea` just took, by construction rather than by coincidence. */
        if (o->glen)
            s_scaleSem += (unsigned)(o->r - o->l + 1) * (unsigned)(o->b - o->t + 1);
    }
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
    /* AND THE COLOUR, which this leaf recorded nowhere until 2026-09-21. Without
       it the op could only ever publish as its box's BYTES, and since the clean
       cut those are dropped -- so the engine's own clear of the offscreen
       (0x4C6890(offscreen, 0) at the head of 0x467D70) reached the twin as
       nothing at all, and every region the engine leaves at index 0 presented as
       the lane's magenta instead of black. Same shape as `rect_box`'s third
       argument, taken while the engine is inside the call. */
    if (s_lastOp) s_lastOp->col = (unsigned char)ARG(e, 2);
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

/* ---- 0x4D85A0 MEM_Free(block) cdecl: THE SURFACE'S DESTRUCTOR -----------
   `pub_surface_bytes` reads a surface's pixels straight out of engine memory
   at the flip. What makes that safe is not the range test on the pointer —
   that answers a question about the value, not about the memory — but this:
   the table entry cannot outlive the block it names.

   Every destination this file records is a surface object from
   SurfaceCreateNamed 0x4C69F0, which asks MEM_Alloc 0x4D83B0 for `w*h+0x30`
   bytes (0x4C6A01..0x4C6A04) and points the object's base field at
   `block+0x30` (0x4C6A0E/0x4C6A14): ONE allocation, header and pixels. The
   block can only be released through this function — 0x4D85A0 is the only
   caller of the allocator's own free 0x4D85B0, and all 363 sites that free
   anything in the engine call it, SurfaceFree 0x4C6AC0 included (0x4C6ACF).
   So an observer at its entry is the surface's destructor, and on the game
   thread — the thread the flip and therefore the publisher run on — nothing
   of ours can run between the drop here and the release, by ordering rather
   than by luck.

   Before this (G18-8) the table was retired by the two paths we had NAMED:
   SurfaceFree (before_free) and the main offscreen's re-create
   (surf_drop_offscreens, at the next 0x4C69F0("OFFSCREEN")). A surface freed
   any other way left an entry pointing into the heap's free list, which reads
   back as whatever the block became — and faults outright once the heap hands
   the segment back, which is exactly what a level teardown makes likely.

   The engine's allocator is genuinely multi-threaded (`0x4D85B0` takes a
   critical section at `0x4D85C2`), and a free on another thread MUST NOT WALK
   THE TABLE: `surf_drop` swap-removes and `surf_get` memsets the tail, so a
   scan racing those reads a slot mid-move — it can mark a slot that is about
   to become a different, live surface and leave the freed one standing, which
   is the very fault this observer exists to remove. So an off-thread free
   pushes the block pointer into `surf_free_offthread`'s ring, unfiltered and
   without reading the table at all, and the game thread retires the entry at
   the top of the next flip, before the census or the publisher read a base.
   Filtering there would not be an optimisation but a hole: MEM_Free fires once
   per block, so a filter that misses queues nothing and nobody ever re-checks.
   The residual window — one thread freeing a surface another is drawing into —
   is the engine's own and was never ours to close.

   The observer sits at the ENTRY of 0x4D85A0, before the allocator's own
   critical section, so the CRT `free()` that surf_drop calls inverts no lock. */
static int __cdecl before_memfree(void* e)
{
    unsigned p = ARG(e, 1);
    int i;
    if (!p) return 0;
    if (!on_game_thread()) { surf_free_offthread(p); return 0; }
    for (i = 0; i < s_nsurf; i++) {
        if (s_surf[i].owner != p) continue;   /* owner == base - 0x30 */
        if (s_trace) {
            char b[220];
            _snprintf(b, sizeof b, "gui trace: MEM_Free surface %08X %dx%d from %08X",
                      s_surf[i].base, s_surf[i].w, s_surf[i].h, ARG(e, 0));
            glog(b);
        }
        surf_drop(i);
        return 0;
    }
    return 0;
}

/* ---- 0x4A81E0 GUI_StageUpdateDraw(gi, flags) stdcall ret 8: the one place
        a screen's surface is drawn into. Traced, not boxed: it tells the
        census which screen was built or redrawn between two flips -------- */
static int __cdecl before_build(void* e)
{
    if (!on_game_thread()) return 0;
    /* OUR OWN FORCED REPAINT IS NOT AN ENGINE BUILD. `builds=` is the one
       diagnostic that could separate what the engine asked for from what we
       did, so counting our synthetic call here would poison it. It is worth
       keeping clean even though it did NOT settle the question it was reached
       for -- three boots per arm gave overlapping means, gpu-status 2.61 --
       because whatever settles that will be built on this counter or beside
       it. [FOUND by landing 9's review.] */
    if (s_repainting) return 0;
    s_builds++;
    s_buildFlags |= ARG(e, 2);
    if (s_trace && (ARG(e, 2) & 1)) s_gafDbg = 6;      /* a build: trace its first blits */
    return 0;
}

/* A LOADER-DECODED PCX'S TAG. The string is MEASURED rather than assumed:
   `SurfaceCreateNamed 0x4C69F0` is handed `"bitmaps\FrontendX.PCX"` for the
   main menu's 640x480 backdrop, and a five-screen walk (2026-09-21) also names
   `"bitmaps\singlebg.PCX"` and `"bitmaps\Skirmsetup4x.PCX"`; the other tags a
   shell session creates are "OFFSCREEN", the screen's own name ("MAINMENU.GUI",
   "SINGLE.GUI", ...), "SAVE UNDER" and "SAVEMOUSE 1..3".

   WHY THE PREFIX IS SAFE, and it is a fact about the BINARY: all 18 call sites
   of `0x4C69F0` were enumerated and every literal tag resolved, and NONE of the
   twelve begins with `bitmaps\` -- so this cannot alias one of the engine's
   composed or scratch surfaces, which is the case that would be a correctness
   bug. (exe-reverse-engineering.md has the table.)

   THE NAME OF THIS FUNCTION IS NARROWER THAN WHAT IT CLAIMS, deliberately left
   rather than silently widened: `0x4CAF30` is reached through a generic
   `bitmaps\<name>.PCX` loader (`0x429290`, four callers, one of them working
   out of "bitmaps\glamour"), so this matches that whole class and not only the
   three shell backdrops. That is the RIGHT class: what the tag establishes is
   "the PCX loader filled this surface", never "this is a menu background". Only
   the directory is tested because the file name is not what makes a surface an
   asset -- what makes it one is that the loader fills it and no observed draw
   covers a pixel of it, which `op_add` is what actually enforces, and a claimed
   surface that is never a copy source costs nothing either way. Bounded and
   case-insensitive; a tag that is not a readable string simply is not one of
   these. [Reach found by the landing review of 381465c.] */
static int tag_is_shell_bg(const char* tag)
{
    static const char pre[] = "bitmaps\\";
    int i;
    if (!ptr_ok(tag)) return 0;
    for (i = 0; i < 8; i++) {
        char c = tag[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != pre[i]) return 0;
    }
    return 1;
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
    /* CLAIMED HERE, EARNED BY SURVIVING `op_add`. The tag only says where the
       surface came from; the claim that nothing composes into it is kept by
       `op_add` clearing this the first time an op names it. The bytes are NOT
       read now -- the surface is blank at this point (see the seed below) and
       the loader has not run -- they cross at the first copy that reads it. */
    if (s) { s->isAsset = tag_is_shell_bg(tag); s->assetSent = 0;
             s->assetTries = 0; s->assetTok = 0;     /* all four, as everywhere else */
             snap_free(s); }   /* a new allocation: the old bytes are not this one's */
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
    /* NOT a pixel-writing leaf: the allocator's free, the one place a recorded
       surface's memory can go away. It rides the same table so it takes the
       same all-or-nothing byte match — G18-8, before_memfree. */
    { 0x004D85A0u, "memfree", { 0x8B, 0x44, 0x24, 0x04, 0x50 }, 5, before_memfree, NULL },
    { 0x004A81E0u, "build", { 0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0xC0, 0x03, 0x00, 0x00 }, 10, before_build, NULL },
    { 0x004BF4D0u, "frame", { 0x83, 0xEC, 0x40, 0x53, 0x55, 0x56, 0x57 }, 7, before_frame, NULL },
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
