#ifndef TAGPU_VK_HIRES_H
#define TAGPU_VK_HIRES_H
/* The replacement meshes' CASTERS, drawn by Vulkan (the Vulkan-only plan's
   gate 3b). Contract only; tagpu_vk_hires.c is the pass.

   READ THE BLOCK AT THE FOOT OF THIS FILE FIRST: since landing 11 D3 NOTHING
   PUBLISHES A HAND-OVER, so this pass draws nothing. What follows is the
   contract it was built to, kept because it is the shape a revival needs, and
   written here in the PAST tense so it cannot be read as a description of a
   running system.

   IT DREW INTO THE CAST-SHADOW MAP AND NOWHERE ELSE. The bodies belonged to
   `tagpu_hires_draw.c`'s GL program -- a glTF unit shaded per pixel with normal
   maps and metallic/roughness was never what this gate was about. What blocked
   the Vulkan lane was narrower and entirely mechanical: `tagpu_shadow.c`
   counted every caster the GL map held that this side had no copy of, and a
   single Peewee with `hires/armpw.glb` active made that count 1 -- which stood
   the shadow map down, and the terrain and unit passes with it. So the whole of
   gate 3b was: put those casters in the map, and stop the census refusing.
   BOTH of those files are gone -- `tagpu_shadow.c` to landing 11 D2 and
   `tagpu_hires_draw.c` to D3 -- so there is no census to satisfy and no GL map
   to be smaller than.

   WHAT IT WAS FED. `tagpu_hires_handover` (`tagpu_hires_draw.h`, deleted),
   recorded by the GL depth pass AS IT DREW: the triangles as CPU bytes, the
   per-unit anchor, yaw/enc and cast triple exactly as the GL uniforms had them,
   and the pose rows. No GL name crossed, which was the whole-phase rule.

   THE ALBEDO DOES NOT CROSS, AND `cutoutSeen` IS WHY THAT IS HONEST. The depth
   path samples the albedo for one purpose -- the alpha cutout -- and every
   material of the shipped replacement is alphaMode OPAQUE, so the sample is
   discarded for every group. A frame that DOES carry a cutout group is one
   this pass cannot reproduce, and it draws nothing rather than a map missing
   its holes. */

#include "tagpu_vk_pass.h"

/* Take this frame's hand-over and put its vertices, poses and uniform blocks
   on the device. Called from the seam BESIDE `tagpu_vk_unit_upload` and before
   `tagpu_vk_shadow_prepare`, for the same reason that one is: the map's
   casters are this pass's geometry, so it all has to exist before the map is
   drawn. Puts no pixel anywhere. */
void tagpu_vk_hires_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* How many replacement-mesh casters this pass is READY to put in the map this
   frame. Valid after `upload` and before `cast`, which is where the shadow
   pass's census needs it.

   IT RETURNS 0, AND HAS SINCE BEFORE D3 (see the foot of this file). The
   paragraph this replaced explained why it was comparable to `otherCasters` and
   only ever smaller: `tagpu_shadow.c` counted what the GL pass drew, this
   counted the subset that reached the hand-over complete and whose device
   resources existed, and the hand-over refused to publish a record short of the
   GL map, so it was 0 or all of them and never a part. Neither counter exists
   now; the invariant is recorded because a revival has to re-establish it. */
int  tagpu_vk_hires_casters(void);

/* Draw them, inside the caller's render pass, with the caller's viewport and
   scissor already set. `rp` is that render pass, needed once to build the
   pipeline against it; a second call with a different one rebuilds.

   RETURNS THE NUMBER DRAWN, or -1 when this pass has casters it could not draw.
   The caller adds the count to the unit pass's and treats -1 as "the map is
   incomplete", which is the same refusal. 0 is an ordinary answer: a frame
   with no replacement mesh on screen. */
int  tagpu_vk_hires_cast(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         VkRenderPass rp);

/* The seam's teardown, in the shape every other pass uses: `down` releases
   everything this pass owns, and is only safe after the seam's
   vkDeviceWaitIdle. */
void tagpu_vk_hires_down(const TAGPU_VKPASS* d);
int  tagpu_vk_hires_down_owed(void);
void tagpu_vk_hires_down_paid(const TAGPU_VKPASS* d);

/* ---- WHAT A HIRES HAND-OVER IS, AND WHO IS SUPPOSED TO PUBLISH ONE --------

   These four types lived in `inc/tagpu_hires_draw.h` until landing 11 D3
   deleted that header with the GL lane. They are kept, unchanged, because they
   are the shape this pass consumes and the next producer should not have to
   invent them again.

   THERE IS NO PRODUCER, AND THERE WAS NONE BEFORE THE DELETION EITHER.
   `s_hiHave` was assigned in five places across three functions, but the only
   assignment of **1** was at `tagpu_hires_draw.c:611`, inside
   `tagpu_hires_depth` -- and that function had ZERO call sites, so the flag was
   never set and the hand-over returned 0 on every frame of every session. (The
   other four assignments all wrote 0.) Independently of that,
   `tagpu_hires_draw_ready()` answered 0 because `opengl32.dll` is never in the
   process, so `tagpu_native.c` nulled every replacement-mesh pointer and there
   was nothing to publish in any case. Measured on live runs: D2's four
   `crowd-static` arms each log `hires draw: missing GL proc` and then the
   fallback to the engine's own 3DO. D3's `one-unit` arms do NOT -- that fixture
   is an ARMCOM with no replacement mesh, so the draw path was never reached at
   all, and its logs show only the loader declining to find one.

   glTF REPLACEMENT MODELS ARE DISABLED AND THEIR IMPLEMENTATION IS TODO AND
   OUT OF SCOPE -- the owner's ruling, 2026-09-19. Reviving them needs a LOADER
   first: the glTF parser, the piece table, the material grouping and the
   COB-driven pose went with `tagpu_hires.c` and are in git at D3's parent.
   What this pass still owns is everything it always did -- the vertex buffers,
   the descriptors, the pose uniform block and the caster draw.
   [gpu-status 2.80.] */

/* The cap every count in the hand-over is checked against, moved here with
   the types it bounds (landing 11 D3). */
#define TAGPU_HI_MAXHAND 256

typedef struct TAGPU_HIGREC {       /* one glTF material's draw */
    float base[4];                  /* baseColorFactor, linear  */
    float cutoff;                   /* < 0 OPAQUE, else the alpha cutoff */
    int   first, count;             /* VERTICES, as TAGPU_HGROUP gives them */
} TAGPU_HIGREC;

typedef struct TAGPU_HIMESH {
    /* the triangles as bytes. `gen` moves on every reload of the file, so a
       consumer caching a device buffer keyed on it is told when the triangles
       under it changed -- the pointer alone is not an identity, because the
       mesh table recycles its slots. */
    const float* v;
    int      ntri, stride;
    unsigned gen;
    int      npiece;
    int      grpOff, ngroup;        /* this mesh's run in `groups` */
} TAGPU_HIMESH;

typedef struct TAGPU_HIUREC {
    int   mesh;                     /* index into `meshes`      */
    int   npose;                    /* pieces `rows` carries    */
    unsigned rowOff;                /* first of npose*3 vec4 in `rows` */
    float anchor[4];                /* ax, ay, world x, projected world z */
    float yawEnc[3];                /* cos(yaw), sin(yaw), enc -- AS THE GL
                                       pass computes them, so the port does no
                                       trigonometry of its own to disagree in */
    float cast[3];                  /* altitude, ground + throw, length scale */
} TAGPU_HIUREC;

typedef struct TAGPU_HIHAND {
    unsigned frame;
    /* 1 = the GL depth pass ran this frame. 0 means the map has no replacement
       mesh in it, which is NOT the same as "no unit had one": the pass returns
       early when it is not ready, and a consumer that read the unit records
       alone would draw casters the oracle did not. */
    int   depthOn;
    float shadowMat[16];
    const TAGPU_HIUREC* units;  int nunit;
    const TAGPU_HIMESH* meshes; int nmesh;
    const TAGPU_HIGREC* groups; int ngroup;
    const float* rows; unsigned nrow;   /* vec4s, 3 per piece per unit */
    /* THE ALBEDO IS NOT CARRIED, AND THIS IS THE FLAG THAT MAKES THAT HONEST.
       The depth path samples the albedo for one thing only -- the alpha cutout
       (`if (uCutoff >= 0.0 && tex.a * uBase.a < uCutoff) discard;`) -- and
       every material of the shipped replacement is alphaMode OPAQUE, so
       `cutoff` is negative for every group and the sampled value is discarded.
       1 here means a group with a real cutoff was drawn into the GL map, and
       the Vulkan pass must then draw NOTHING rather than a map with the holes
       missing. Deferring the textures is only honest while this is checked. */
    int   cutoutSeen;
} TAGPU_HIHAND;

#endif
