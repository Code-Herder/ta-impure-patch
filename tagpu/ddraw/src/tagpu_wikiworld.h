#ifndef TAGPU_WIKIWORLD_H
#define TAGPU_WIKIWORLD_H
/* tagpu_wikiworld -- the producer of the in-game wiki's 3D stage: what the wiki's instances of the
   world passes (tagpu_scene.h) draw their hand-overs from, and the inert answers that stand in
   for the modules only the game's world has (research/notes/ingame-wiki-3d-stage-plan.md,
   sections 2, 3, 6, 7 and 8; landings 6g and 6h).

   WHAT IT PRODUCES, once a frame, on the render thread: from the stage this frame's wiki draws
   (tagpu_wikistate_scene) and the published 3D arena (tagpu_wiki3d_get), the stage's ground as the
   game draws a map -- the terrain hand-over (the description's tiles in tagpu_terr.c's atlas
   layout, one cell a 32-px tile), the feature hand-over (the deposits' and vents' frames in an
   atlas of its own, their quads in tagpu_feat.c's form) and the engine's fog grid -- and the world
   target's geometry: the map at zoom 1, one world unit a game pixel, at the Supersampling
   setting's `ss`, composited into the image's rect (tagpu_wikistage_image), the fit the stage's
   ops are built with. The cells, quads and fog are tagpu_wikiscene.c's, which is paired with the
   mockup.

   AND THE UNITS AS THEIR MODELS: tagpu_wikiscene.c's model records (every unit and every ghost
   whose type the arena resolved, and every 3D feature of a ground in 3D), each baked by the wiki's instance of the geometry bake from the
   arena's model and posed at rest, turned onto its facing, by the wiki's posed program -- the
   posed-draw hand-over the wiki's unit pass draws from, in the game's order (the units, then the
   ghosts' window). The bake, the material layer and the posed program take their `_frame` calls
   in tagpu_native.c's order, under one constant level generation (the arena is never freed).
   THE WIKI'S BAKE READS THE ARENA AND NOTHING ELSE because the loader wrote every pointer in it,
   each inside it, and left no word of a file that a reader follows (tagpu_wikiasset.h). Each type's
   piece run is the readers' walk of its model, and every node of it is re-checked against the
   arena's used extent before the run is kept -- a bound on the pointer's value, not a probe -- so
   a type whose walk does not stay inside is a box, and a 3D feature whose walk does not falls
   its stage's ground back whole (`wrecks_build`).

   A stage whose ground did not resolve still draws its units: the image then carries no ground
   (`ground` 0), and the whitebox's ground is drawn under it (tagpu_wikistate_image).

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
   terrain whenever the ground resolved, the features whenever it has any, the units whenever a
   record was made) is fixed here before the seam prepares the passes; the seam composites the
   image only when every expected pass's prepare answered 1 on this frame, and the wiki layer draws
   R0, the image and R2 then, R0 and R1 otherwise (tagpu_wikistate_image). A record the bake
   refuses this frame (its texels not yet painted: the unit atlas's allowance) withholds the
   units' hand-over whole, so the unit pass does not prepare and the frame is the whitebox -- never
   a stage with a unit missing. The lever `tagpu_wiki3d.on` takes `off` (no 3D stage), `nomap`,
   `nofeat` and `nounits` (that hand-over withheld while still expected, so the frame shows R1
   whole: the check of that rule) and `nounit=TYPE` (that type left unresolved: its box over the
   image, every other type in 3D).

   Render thread only, every entry point. Nothing here reads engine memory. */

#include "tagpu_wikistage.h"            /* WIKISTAGE_UNITS, WIKISTAGE_SITES */

/* THE 3D FEATURE MODELS ONE STAGE CAN HOLD: a feature whose section names a model (a wreck such
   as `armmex_dead`) is drawn through the unit pass, so each distinct section takes a bake entry
   beside the units. The stage description's reader refuses a description with more; the
   placements are bounded by the stage's features (WIKISCENE_WRECKS). */
#define WIKIWORLD_FEAT3D   64u

/* THE WIKI INSTANCES' TAGPU_PB_FRAMEMAX (tagpu_scene.h): the entries one frame of the stage may ask
   the bake for, which sizes its caches and the unit pass's vertex-buffer table. The bake keys an
   entry by model (and ghost) and a material by (model, owner), never by record, so a frame asks for
   at most one of each per type the entry names (WIKI_MAX_DEFS) and 3D feature, a material once per
   owner of the eight: within this bound on every stage description written so far, not on every
   one the data could hold. An entry this frame asked for is never evicted (tagpu_posebake.c
   `geom_slot`); a frame that needs one more is refused by the bake, which hands nothing over, so
   that frame is the whitebox whole. */
#define WIKIWORLD_FRAMEMAX ((unsigned)WIKISTAGE_UNITS + (unsigned)WIKISTAGE_SITES + 1u + \
                            WIKIWORLD_FEAT3D)

/* the file a `tagpu_wiki.shot` capture is written to: the wiki's target, before the Gamma curve */
#define WIKIWORLD_SHOT "tagpu_wiki3d_shot.ppm"

struct TAGPU_FRAME;

/* Once a frame, from tagpu_overlay_draw after tagpu_wikistate_present: this frame's hand-overs, or
   none (no stage drawn, the arena not loaded, the ground fallen back, the lever off). */
void tagpu_wikiworld_frame(const struct TAGPU_FRAME* f);

/* what frame `frame`'s image needs, for the seam */
typedef struct {
    int             terr, feat, unit;   /* the image needs that pass prepared                   */
    int             unitWhole;          /* every record was handed over: else no image this frame */
    int             ground;             /* the image carries the ground (tagpu_wikistate_image) */
    const uint32_t* resolved;           /* the view's types drawn in 3D, WIKI_MAX_DEFS bits;
                                           this module's, valid until its next frame             */
} WIKIWORLDWANT;

/* Render-thread frame `frame`'s 3D stage: 1 when the seam is to prepare the wiki's passes, with
   what the image needs (`*w`); 0 when there is no 3D stage this frame and the wiki layer draws the
   whitebox. */
int  tagpu_wikiworld_wants(unsigned frame, WIKIWORLDWANT* w);

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
