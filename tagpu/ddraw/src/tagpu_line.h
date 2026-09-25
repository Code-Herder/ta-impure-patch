#ifndef TAGPU_LINE_H
#define TAGPU_LINE_H
/* tagpu_line.h -- where a line's two endpoints land on the game-pixel grid.

   EVERY LINE THE VULKAN LANE DRAWS FOLLOWS ONE RULE: DrawLine `0x4CC7AB`'s
   Bresenham on the GAME-PIXEL grid, each lit game pixel covered whole (`ss` x
   `ss` target pixels). That covers the markers' order lines and selection
   rects (tagpu_mark.c), the effects' lasers and lightning (tagpu_fx.c) and the
   nanoframe wire (tagpu_vk_unit.c). The walk itself is decided per game pixel
   by the fragment stage, in integers (tagpu_glsl.h `TAGPU_GLSL_LINE_FN`), so
   it cannot differ between two GPUs. What reaches that test is two INTEGER
   endpoints, and they are decided here, on the CPU, by one rule:

     an endpoint's game pixel is floor(Z(p)), where p is its position in the
     1x frame and Z the wheel zoom about the view centre,
     Z(p) = (p - c) * zoom + c.

   `p` carries the engine's own rounding wherever the engine draws that line
   from integers -- a laser's two pixels, a queued build site's corners, a
   selection rect's corners -- and is then that engine pixel's CENTRE, k + 0.5,
   so at 1x the pixel is the engine's own and at any other zoom it is the pixel
   the engine's pixel centre lands in (the selection rect's notion of the grid
   since it was first drawn this way). For our own geometry -- the sub-pixel
   anchor of a walking unit, a range arc, a waypoint crosshair, a posed
   nanoframe vertex -- `p` is the fractional position itself, and at 1x the
   rule is `floor(p)`, which is the engine's `>> 16` truncation of a 16.16
   position.

   THE ENGINE'S CLIP, AFTER THE ZOOM (tagpu_line_clip below). Every line the
   engine draws that this lane redraws as a line -- the selection rect
   (`0x467A50`), the build site (`0x438C00`), the target circle and the range
   circles' chords, the lasers -- goes through DrawLine `0x4BE950`, which
   clips both ends to the context's clip rect before it walks. A line that
   crosses the viewport edge is walked from where that clip MOVED the end, not
   from the end, and the two walks disagree along the whole visible part. So
   the markers and the effects put their integer ends through the same two
   clips before a record is built. The nanoframe wire does not: the engine's
   wire is the polygon edge walk `0x4C0820`, which clips scanlines rather than
   ends, so its visible pixels stay where the unclipped line puts them.

   THE BOUNDS, TWO OF THEM. The fragment test multiplies two coordinate
   differences in unsigned 32-bit arithmetic, `2 * minor * i`, which holds for
   any line whose ends both lie in [-TAGPU_LINE_MAXC, TAGPU_LINE_MAXC] (both
   differences are then at most 2 * MAXC = 32766, and 2 * 32766^2 + 32766 <
   2^32). A CLIPPED line meets that by construction: tagpu_line_clip returns
   its ends inside the surface, and it refuses a surface wider or taller than
   MAXC + 1. What bounds a clipped line BEFORE the clip is TAGPU_LINE_FAR, the
   box the clip's 64-bit products are exact in; it is 2^29 game pixels, which
   no world position reaches at any zoom the wheel allows, and an end past it
   is counted by the caller and not drawn. The wire is not clipped, so its
   ends are held to the MAXC box itself; a nanoframe's edge is a few hundred
   world pixels long and its unit is culled to the view. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define TAGPU_LINE_MAXC 16383
#define TAGPU_LINE_FAR  (1 << 29)

/* 1 and the endpoint's game pixel, or 0 when it lies outside [-bound, bound]:
   TAGPU_LINE_FAR for a line tagpu_line_clip will clip, TAGPU_LINE_MAXC for
   one drawn as it stands. */
static __inline int tagpu_line_px(double x, double y, double zoom, double zcx,
                                  double zcy, int bound, int* gx, int* gy)
{
    double fx = floor((x - zcx) * zoom + zcx);
    double fy = floor((y - zcy) * zoom + zcy);
    if (!(fx >= -bound && fx <= bound && fy >= -bound && fy <= bound)) return 0;
    *gx = (int)fx;
    *gy = (int)fy;
    return 1;
}

/* THE CLIP DrawLine `0x4BE950` APPLIES, both halves, in its order, on ends
   already on the game-pixel grid. 1 with the ends moved in place, and then
   inside [0, w) x [0, h); 0 when the engine would draw nothing.

   First `0x4BEA20` [DISASSEMBLED 2026-09-25], against the context's clip rect
   [L, R] x [T, B], inclusive on every side (`0x4C6AE0` copies it out of
   `ctx+0x1C..+0x28`). That rect is the viewport: `DrawGameScreen` stores
   `main+0x37E27` = {128, 32, W - 1, H - 33} into its context through
   `0x4C6B10` (`0x468D85`), and at zoom < 1 `vpwide` clamps the widened rect
   back to it. The lane passes the same screen rect in the ZOOMED grid,
   because that is where the viewport's edge is once the world is scaled
   about its centre. One pass, end 0 then end 1, each tested x < L, y < T,
   x > R, y > B in that order and moved along the line with the ORIGINAL
   deltas and a truncating division; an end off a side the line runs away
   from rejects it. A single pass can leave an end just past a corner, and
   that is what the second half is for.

   Then `0x4CC650`, the walk's own clip, against the surface [0, w) x [0, h):
   while an end is off it, reject a line wholly off one side, else move each
   end onto the edge (0 or w - 1, 0 or h - 1) with the CURRENT deltas, and
   test again. The rect is clamped to the surface first, as `vpwide` clamps
   it, so the ends reaching this half are within a few pixels of it.
   THE LOOP'S BOUND: four passes, then the line is not drawn --
   tools/line-band-check.py finds none that takes more than one.

   THE ARITHMETIC IS 64-BIT. The engine multiplies in 32 bits, and on its own
   1x coordinates no product comes near 2^31, so the two agree there; in the
   zoomed grid the ends may lie up to TAGPU_LINE_FAR out, where only 64 bits
   hold `(edge - end) * delta` (under 2^31 * 2^31). The divisions truncate
   toward zero in both. */
static __inline int tagpu_line_clip(int* px0, int* py0, int* px1, int* py1,
                                    int L, int T, int R, int B, int w, int h)
{
    long long x0 = *px0, y0 = *py0, x1 = *px1, y1 = *py1, dx, dy;
    int xle, yle, pass;
    if (w < 1 || h < 1 || w > TAGPU_LINE_MAXC + 1 || h > TAGPU_LINE_MAXC + 1) return 0;
    if (L < 0) L = 0;
    if (T < 0) T = 0;
    if (R > w - 1) R = w - 1;
    if (B > h - 1) B = h - 1;
    if (L > R || T > B) return 0;
    /* 0x4BEA20 */
    xle = x0 <= x1; yle = y0 <= y1;
    dx = x1 - x0;   dy = y1 - y0;
    if (x0 < L) { if (!xle || !dx) return 0; y0 += (L - x0) * dy / dx; x0 = L; }
    if (y0 < T) { if (!yle || !dy) return 0; x0 += (T - y0) * dx / dy; y0 = T; }
    if (x0 > R) { if (xle || !dx) return 0;  y0 += (R - x0) * dy / dx; x0 = R; }
    if (y0 > B) { if (yle || !dy) return 0;  x0 += (B - y0) * dx / dy; y0 = B; }
    if (x1 < L) { if (xle || !dx) return 0;  y1 += (L - x1) * dy / dx; x1 = L; }
    if (y1 < T) { if (yle || !dy) return 0;  x1 += (T - y1) * dx / dy; y1 = T; }
    if (x1 > R) { if (!xle || !dx) return 0; y1 += (R - x1) * dy / dx; x1 = R; }
    if (y1 > B) { if (!yle || !dy) return 0; x1 += (B - y1) * dx / dy; y1 = B; }
    /* 0x4CC650. No division below is by zero: a zero delta on an axis means
       both ends share that coordinate, so an end off that axis has the other
       off the same side, and the line was rejected by the test above it. */
    for (pass = 0; pass < 4; pass++) {
        if (x0 >= 0 && x0 < w && x1 >= 0 && x1 < w &&
            y0 >= 0 && y0 < h && y1 >= 0 && y1 < h) {
            *px0 = (int)x0; *py0 = (int)y0; *px1 = (int)x1; *py1 = (int)y1;
            return 1;
        }
        dx = x1 - x0; dy = y1 - y0;
        if (dx >= 0 ? (x0 >= w || x1 < 0) : (x0 < 0 || x1 >= w)) return 0;
        if (dy >= 0 ? (y0 >= h || y1 < 0) : (y0 < 0 || y1 >= h)) return 0;
        if (x0 < 0)       { y0 += -x0 * dy / dx;          x0 = 0; }
        else if (x0 >= w) { y0 += (w - 1 - x0) * dy / dx; x0 = w - 1; }
        if (y0 < 0)       { x0 += -y0 * dx / dy;          y0 = 0; }
        else if (y0 >= h) { x0 += (h - 1 - y0) * dx / dy; y0 = h - 1; }
        if (x1 < 0)       { y1 += -x1 * dy / dx;          x1 = 0; }
        else if (x1 >= w) { y1 += (w - 1 - x1) * dy / dx; x1 = w - 1; }
        if (y1 < 0)       { x1 += -y1 * dx / dy;          y1 = 0; }
        else if (y1 >= h) { x1 += (h - 1 - y1) * dx / dy; y1 = h - 1; }
    }
    return 0;
}

/* THE LINE LIST OF AN A/B FRAME, the other half of the lines' gate. A pass
   whose A/B claims a frame writes every line it built that frame to
   `tagpu_<pass>_lines.txt` beside the capture: a header, then
   `<ax> <ay> <bx> <by> <palette index>` a line, the ends as tagpu_line_px
   decided them. tools/line-oracle.py walks each line with its own reading of
   `0x4CC7AB` and compares the pixels with the capture, so the ends are the one
   input the model and the shader share.

   Two headers. `# <pass> <game w> <game h>` is a list of lines drawn as they
   stand (the wire). `# <pass> <game w> <game h> <L> <T> <R> <B>` is a list of
   lines BEFORE tagpu_line_clip, with the rect it was given: the oracle then
   clips each line with its own transcription of the two clips and drops what
   falls outside the rect, as the pass's viewport scissor does. NULL when the
   file will not open; the capture goes on without it. */
static __inline FILE* tagpu_line_list_open(const char* pass, int gw, int gh)
{
    char name[64];
    FILE* f;
    _snprintf(name, sizeof name, "tagpu_%s_lines.txt", pass);
    name[sizeof name - 1] = 0;
    f = fopen(name, "w");
    if (f) fprintf(f, "# %s %d %d\n", pass, gw, gh);
    return f;
}

static __inline void tagpu_line_list_add(FILE* f, int ax, int ay, int bx, int by,
                                         int col)
{
    if (f) fprintf(f, "%d %d %d %d %d\n", ax, ay, bx, by, col);
}

/* THE CLIPPED PASSES' LIST, collected where each line is built -- the one
   place its ends are known before the clip moves them -- and written when the
   A/B claims the frame. `on` is set by the gather of a frame the A/B may
   claim and is -1 once a row could not be kept, when nothing is written:
   a list short of the frame's lines would fail the oracle for a reason that
   is not the lines. Rows are in build order; the oracle compares lit pixels,
   not colours, so the order of overlapping lines does not enter it. */
typedef struct TAGPU_LINE_AB {
    int on;
    unsigned n, cap;
    int* v;
} TAGPU_LINE_AB;

static __inline void tagpu_line_ab_begin(TAGPU_LINE_AB* a, int on)
{
    a->on = on ? 1 : 0;
    a->n = 0;
}

static __inline void tagpu_line_ab_add(TAGPU_LINE_AB* a, int ax, int ay, int bx,
                                       int by, int col)
{
    int* r;
    if (a->on != 1) return;
    if (a->n == a->cap) {
        unsigned nc = a->cap ? a->cap * 2u : 256u;
        int* nv = (int*)realloc(a->v, (size_t)nc * 5u * sizeof(int));
        if (!nv) { a->on = -1; return; }
        a->v = nv;
        a->cap = nc;
    }
    r = a->v + (size_t)a->n * 5u;
    r[0] = ax; r[1] = ay; r[2] = bx; r[3] = by; r[4] = col;
    a->n++;
}

static __inline void tagpu_line_ab_write(TAGPU_LINE_AB* a, const char* pass,
                                         int gw, int gh, int L, int T, int R, int B)
{
    char name[64];
    FILE* f;
    unsigned i;
    if (a->on != 1) { a->on = 0; return; }
    a->on = 0;
    _snprintf(name, sizeof name, "tagpu_%s_lines.txt", pass);
    name[sizeof name - 1] = 0;
    f = fopen(name, "w");
    if (!f) return;
    fprintf(f, "# %s %d %d %d %d %d %d\n", pass, gw, gh, L, T, R, B);
    for (i = 0; i < a->n; i++) {
        const int* r = a->v + (size_t)i * 5u;
        fprintf(f, "%d %d %d %d %d\n", r[0], r[1], r[2], r[3], r[4]);
    }
    fclose(f);
}

#endif
