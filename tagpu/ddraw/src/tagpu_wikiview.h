#ifndef TAGPU_WIKIVIEW_H
#define TAGPU_WIKIVIEW_H
/* tagpu_wikiview -- the tech tree's unit view, the render thread's half: the picked unit's model
   (tagpu_wikitech3d_get, its body's arena) baked by the wiki's instance of the bake, posed by the
   pose its script's host left in the record (TAGPU_PK_WIKI tv_*) -- or its rest pose -- and laid
   out as one vertex stream of triangles in the model's own space, for tagpu_vk_wikiview.c to draw
   under the orbit camera. The mockup is tools/wiki/mockup/unitview.js; the game thread's half is
   tagpu_wiki.c, THE TECH TREE'S UNIT VIEW.

   THE SPACE. The arena holds a model as the engine's loader does, X and Z negated (tagpu_wikiasset.h),
   so the bake's vertices and the composed pieces are in those held axes, faced by the identity --
   pose.js `model` with the yaw [1, 0], which the mockup's posed view draws. The held axes are the
   file's turned half a turn about Y: the winding and the up axis are the file's.

   Render thread only. The vertices and the fit are this module's and hold until its next frame. */
#include "tagpu_gaf.h"                  /* TAGPU_GAFVIEW */

struct TAGPU_FRAME;

/* floats per vertex of the stream:
     0-2   position          the posed point, held axes
     3-4   uv                the atlas corner (normalised), (-1, -1) for a face of one colour
     5-7   colour            that face's colour, the engine's table's RGB in [0, 1]
     8-9   quad point        tagpu_posebake.h's material 5-6
     10-11 quad shape (a, b) its 7-8
     12-15 quad rect         its 9-12, (-1, ...) for a face that keeps its uv */
#define TAGPU_WV_STRIDE 16

/* The most vertices the stream holds: the bake's bound on a whole model (tagpu_posebake.c
   PB_MAXVERT), so every body range fits. */
#define TAGPU_WV_MAXVERT 49152

/* This frame's view, from the frame's packet: run before tagpu_wikistate_present, whose layout
   draws the note this decides. Nothing when the wiki is closed or no unit is picked. */
void tagpu_wikiview_frame(const struct TAGPU_FRAME* f);

/* The view's note for `frame` (TAGPU_TV_NOTE_*): the host's half from the record, EMPTY when the
   pose left nothing to draw, LOADING while there is no picture. LOADING for any other frame. */
int  tagpu_wikiview_note(unsigned frame);

typedef struct {
    const float*         vert;          /* nvert x TAGPU_WV_STRIDE, triangles                    */
    int                  nvert;
    float                c[3], dist;    /* the fit: the rest pose's bounds' centre, and the
                                           camera's distance at zoom 1 (unitview.js)            */
    float                yaw, pitch, zoom;  /* the record's camera, bounded                     */
    TAGPU_GAFVIEW        atlas;         /* the wiki's unit atlas the uvs address                */
    const unsigned char* pal;           /* the engine's table it expands through, and its serial */
    unsigned             palSerial;
} TAGPU_WIKIVIEWHAND;

/* Frame `frame`'s picture: 1 with what to draw, 0 when the frame has none. */
int  tagpu_wikiview_hand(unsigned frame, TAGPU_WIKIVIEWHAND* h);

/* The view's copy of the atlas is at `serial` (tagpu_gaf_atlas_ack; `keep` 0 when it holds none).
   The wiki's stage's unit pass acknowledges the same atlas on the frames it draws; the two never
   draw in one frame, and each takes the whole page when the tiles due since its own copy pass what
   one frame sends (tagpu_vk_wikiview.c, tagpu_vk_unit.c base_upload). */
void tagpu_wikiview_atlas_ack(unsigned serial, int keep);

#endif
