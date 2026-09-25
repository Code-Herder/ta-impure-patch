#ifndef TAGPU_FXMODEL_H
#define TAGPU_FXMODEL_H
/* tagpu_fxmodel.h — THE EFFECTS MODELS' RASTERISER, the engine's own.

   Every 3DO model an effect draws -- a weapon's body and its thrust flame, a
   debris piece, an explosion's body -- arrives in the frame packet already
   posed by the engine's rotation (tagpu_packet.h, "THE EFFECTS MODELS"). What
   is left is what the engine does with it next: project each vertex to whole
   screen pixels and hand every face to one of two integer rasterisers,

     0x4C7580 + 0x4C7310  a four-vertex face, textured: two edge chains
                          walked in 16.16, a span stepped in 16.16, one
                          texel per pixel by the frame width's addressing
     0x4C0330             any other face with flag bit 0: a flat fill

   and this module does exactly that, integer for integer, on the render
   thread. Its output is not a picture but RUNS: a row's pixels that take the
   same texel, as [x0, x1) on one row with that texel's centre in the unit
   atlas -- or with a palette index, where the texel is the frame's colour
   key (the span copies it; the atlas marks it a hole) or the face is flat.
   The unit pass draws the runs as rectangles through the unit program's own
   fragment stage (tagpu_posedraw.c `FXVS`), so a model takes the unit
   atlas's texels, the face-shade multiplier, fog and Classic++'s lighting
   exactly as a unit does.

   WHY THE RASTERISER AND NOT GEOMETRY. A triangle with interpolated UVs is a
   different picture: the engine walks each edge in 16.16 from a vertex biased
   by 0xFFFF, steps its UV per row and per pixel by TRUNCATED divisions, and
   picks the texel by the frame width's own addressing -- at 1x a rocket is a
   few dozen pixels, and every one of them is decided by that arithmetic.
   MEASURED for the geometry alternative (the node posed on the GPU and drawn
   as two triangles a face, paused at 1x against the engine's own draw): 3 of
   20 model pixels differed on a frame of rockets, 10 of 62 on a mixed one and
   309 of 831 on a large battle. The runs match the engine's pixels exactly
   wherever nothing the engine draws later covers them.

   RENDER THREAD ONLY. It reads the packet it is handed and, for each
   textured face, the unit atlas through `tagpu_r3d_atlas_texels`
   (tagpu_render3do.h): the mirror's texels where the frame is painted, and
   where it is not, the engine's GAF frame at the address the packet carries,
   decoded on this thread under the fence that orders every engine read of
   the pose bake (thread-split.allow, tagpu_render3do.c). Nothing else. */

#include <stdint.h>

struct TAGPU_PACKET;

/* THE RUN BUDGET: the most runs one frame's models take, 4 MB at 32 bytes a
   run. The runs are held 2 + nimg times over -- this module's arena, the
   posed pass's hand-over (tagpu_posedraw.c), and the unit pass's storage
   buffer, host-visible, one per swapchain image (tagpu_vk_unit.c, up to
   TAGPU_VK_SLOTS) -- in a 32-bit process: 20 MB at the full budget with
   three images. So the budget is what those copies may cost together and
   not what a frame could ask for. All of them bound the count by this one
   constant: a model that would take this module's arena past it is not
   drawn, and its record is taken back whole (tagpu_fx.c), so the others
   never see more. A run is one row's pixels of one texel, so the budget is
   131 072 such pieces of model on one frame; the effects gather's numbers
   against it are in gpu-status.md 2.89. */
#define TAGPU_FXM_RUN_MAX (1u << 17)

/* The frame the models are drawn into, from the effects pass's view. The
   engine projects `(v >> 16) - eye + 0x80` across and `... + 0x20` down: its
   screen has the viewport at (0x80, 0x20), and `ox`/`oy` carry that onto the
   frame, which has it at (vpL, vpT). The clip rect is the engine's own for
   these draws, `vp_addr` (L, T, R, B), in the engine's space: the rasterisers
   take L and T as the first column and row drawn and R and B as the first
   ones NOT drawn. */
typedef struct TAGPU_FXRVIEW {
    int eyeX, eyeY;             /* the eye the effects pass projects with     */
    int ox, oy;                 /* engine screen px -> frame px               */
    int clipL, clipT, clipR, clipB;
} TAGPU_FXRVIEW;

/* ONE RUN, 32 bytes: two vec4 in the unit pass's storage buffer, laid out as
   tagpu_posedraw.c's `FXVS` reads them. `u`/`v` are the texel's CENTRE in
   the unit atlas, or -1 for a run that takes `fc` (index / 255) from the
   palette instead. Frame px. */
typedef struct TAGPU_FXRUN {
    float x0, x1, y, pad0;
    float u, v, fc, pad1;
} TAGPU_FXRUN;

/* the arena: emptied once per frame by the effects gather, and taken back to
   a mark by its bracket when an effect is lost */
void     tagpu_fxmodel_frame(void);
unsigned tagpu_fxmodel_mark(void);
void     tagpu_fxmodel_rewind(unsigned mark);
const TAGPU_FXRUN* tagpu_fxmodel_runs(unsigned* n);

/* Rasterise model `model` of packet `pk`, appending its runs. 1 when it is
   drawn as the engine draws it -- which may be no run at all, off screen or
   facing away -- and 0 when it cannot be: a model the packet did not carry, a
   texel the unit atlas has not painted yet (its allowance), a row count past
   the arena. On 0 the runs it appended are already taken back. */
int tagpu_fxmodel_raster(const struct TAGPU_PACKET* pk, unsigned model,
                         const TAGPU_FXRVIEW* v);

/* the reason the last 0 was returned, for the effects heartbeat */
enum { TAGPU_FXM_OK = 0, TAGPU_FXM_NOTCARRIED, TAGPU_FXM_TEXEL, TAGPU_FXM_ROOM };
int tagpu_fxmodel_why(void);
#endif
