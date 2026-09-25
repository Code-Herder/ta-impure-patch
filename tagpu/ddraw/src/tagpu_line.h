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

   THE BOUND. The fragment test multiplies two coordinate differences in
   unsigned 32-bit arithmetic, `2 * minor * i`, which holds for any line whose
   endpoints both lie in [-TAGPU_LINE_MAXC, TAGPU_LINE_MAXC] (both differences
   are then at most 2 * MAXC = 32766, and 2 * 32766^2 + 32766 < 2^32). A line
   with an endpoint outside that box is not drawn and the caller counts it:
   that is a line more than 16 000 game pixels off the frame, where every
   caller has already culled its lines to the view.

   THE ENGINE'S CLIP IS NOT APPLIED. `0x4CC7AB` first hands its ends to
   `0x4CC650`, which moves an end lying off the surface onto its edge with a
   truncating integer intersection. A line wholly on the surface is untouched
   by it; one that crosses the edge is walked by the engine from the moved
   end, so along the part it draws its pixels can sit one off ours. The lane
   does not clip its lines to the 1x surface at all -- zoomed out they carry
   on past it -- so the walk is always the unclipped one. */

#include <math.h>
#include <stdio.h>

#define TAGPU_LINE_MAXC 16383

/* 1 and the endpoint's game pixel, or 0 when it lies outside the bound. */
static __inline int tagpu_line_px(double x, double y, double zoom,
                                  double zcx, double zcy, int* gx, int* gy)
{
    double fx = floor((x - zcx) * zoom + zcx);
    double fy = floor((y - zcy) * zoom + zcy);
    if (!(fx >= -TAGPU_LINE_MAXC && fx <= TAGPU_LINE_MAXC &&
          fy >= -TAGPU_LINE_MAXC && fy <= TAGPU_LINE_MAXC)) return 0;
    *gx = (int)fx;
    *gy = (int)fy;
    return 1;
}

/* THE LINE LIST OF AN A/B FRAME, the other half of the lines' gate. A pass
   whose A/B claims a frame writes every line it hands the GPU that frame to
   `tagpu_<pass>_lines.txt` beside the capture, in draw order: a header
   `# <pass> <game w> <game h>`, then `<ax> <ay> <bx> <by> <palette index>`
   a line, the ends as tagpu_line_px decided them. tools/line-oracle.py walks
   each line with its own reading of `0x4CC7AB` and compares the pixels with
   the capture, so the endpoints are the one input the model and the shader
   share. NULL when the file will not open; the capture goes on without it. */
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

#endif
