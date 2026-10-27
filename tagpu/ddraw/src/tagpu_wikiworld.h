#ifndef TAGPU_WIKIWORLD_H
#define TAGPU_WIKIWORLD_H
/* tagpu_wikiworld -- the producer of the in-game wiki's 3D stage: what the wiki's instances of the
   world passes (tagpu_scene.h) draw their hand-overs from, and the inert answers that stand in
   for the modules only the game's world has (research/notes/ingame-wiki-3d-stage-plan.md,
   sections 2, 3, 7 and 8; landing 6g).

   WHAT IT PRODUCES, once a frame, on the render thread: from the stage this frame's wiki draws
   (tagpu_wikistate_scene) and the published 3D arena (tagpu_wiki3d_get), the stage's ground as the
   game draws a map -- the terrain hand-over (the description's tiles in tagpu_terr.c's atlas
   layout, one cell a 32-px tile), the feature hand-over (the deposits' and vents' frames in an
   atlas of its own, their quads in tagpu_feat.c's form) and the engine's fog grid -- and the world
   target's geometry: the map at zoom 1, one world unit a game pixel, at the Supersampling
   setting's `ss`, composited into the image's rect (tagpu_wikistage_image), the fit the stage's
   ops are built with. The cells, quads and fog are tagpu_wikiscene.c's, which is paired with the
   mockup. No unit record is produced: the units are the whitebox's boxes, over the image.

   THE HAND-OVERS LIVE FOR ONE ITERATION OF THE RENDER LOOP. tagpu_wikiworld_frame runs in
   tagpu_overlay_draw after tagpu_wikistate_present, and the wiki's passes take what it published in
   tagpu_vk_frame later in the same iteration, on the same thread (render_vk.c). Each hand-over
   carries the frame it was published on and is refused on any other, so a hand-over a frame left
   standing -- the window closed, the seam returned early -- is never drawn later. Every pointer in
   one names a buffer of this module (rebuilt only here, only when the stage's ground changes, and
   so never between the publish and the take) or the arena (immutable, never freed).

   THE FEATURE ATLAS IS PAINTED ON THE RENDER THREAD ONLY: tagpu_gaf.c decodes through static
   buffers it shares with the game's atlases, all of which are painted here too.

   THE IMAGE IS DRAWN WHOLE OR NOT AT ALL. What a frame expects (`tagpu_wikiworld_wants`: the
   terrain whenever the ground resolved, the features whenever it has any) is fixed here before the
   seam prepares the passes; the seam composites the image only when every expected pass's prepare
   answered 1 on this frame, and the wiki layer draws R0, the image and R2 then, R0 and R1
   otherwise (tagpu_wikistate_image). The lever `tagpu_wiki3d.on` takes `off` (no 3D stage),
   `nomap` and `nofeat` (that hand-over withheld while still expected, so the frame shows R1
   whole: the check of that rule).

   Render thread only, every entry point. Nothing here reads engine memory. */

#include "tagpu_wikistage.h"            /* WIKISTAGE_UNITS, WIKISTAGE_SITES */

/* THE 3D FEATURES ONE STAGE CAN HOLD: a feature whose section names a model (a wreck such as
   `armmex_dead`) is drawn through the unit pass, so each takes a bake entry beside the units. The
   stage description's reader refuses a description with more. */
#define WIKIWORLD_FEAT3D   64u

/* THE MOST POSED OBJECTS ONE FRAME OF THE STAGE DRAWS: every unit, the ghost of every gesture
   site, the placement ghost and every 3D feature. It is the wiki instances' TAGPU_PB_FRAMEMAX
   (tagpu_scene.h), which sizes the bake's caches and the unit pass's vertex-buffer table, so a
   frame of the stage can never evict an entry it has asked for (tagpu_posebake.h). */
#define WIKIWORLD_FRAMEMAX ((unsigned)WIKISTAGE_UNITS + (unsigned)WIKISTAGE_SITES + 1u + \
                            WIKIWORLD_FEAT3D)

/* the file a `tagpu_wiki.shot` capture is written to: the wiki's target, before the Gamma curve */
#define WIKIWORLD_SHOT "tagpu_wiki3d_shot.ppm"

struct TAGPU_FRAME;

/* Once a frame, from tagpu_overlay_draw after tagpu_wikistate_present: this frame's hand-overs, or
   none (no stage drawn, the arena not loaded, the ground fallen back, the lever off). */
void tagpu_wikiworld_frame(const struct TAGPU_FRAME* f);

/* Render-thread frame `frame`'s 3D stage: 1 when the seam is to prepare the wiki's passes, with
   whether the image needs the terrain pass (`*terr`) and the feature pass (`*feat`); 0 when there
   is no 3D stage this frame and the wiki layer draws the whitebox. */
int  tagpu_wikiworld_wants(unsigned frame, int* terr, int* feat);

/* The seam's answer for `frame`: whether the image was drawn whole, and the render-thread time the
   wiki's passes took in the seam (prepares, the target's pass and the composite), ms. Logged with
   the producer's own time every 300 frames while the stage is shown. */
void tagpu_wikiworld_drawn(unsigned frame, int image, double ms);

/* THE SHOT (tagpu_wiki.c's trigger tagpu_wiki.shot): 1 when `frame` is the first frame since the
   record asked for one whose image is drawn -- the seam then records the capture of the wiki's
   target into WIKIWORLD_SHOT, which this call has unlinked so no earlier file stands for it -- and
   `_armed` says it was recorded. */
int  tagpu_wikiworld_shot_due(unsigned frame);
void tagpu_wikiworld_shot_armed(unsigned frame, unsigned w, unsigned h);

#endif
