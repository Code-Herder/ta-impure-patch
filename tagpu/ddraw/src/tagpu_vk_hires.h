#ifndef TAGPU_VK_HIRES_H
#define TAGPU_VK_HIRES_H
/* The replacement meshes' CASTERS, drawn by Vulkan. Contract only;
   tagpu_vk_hires.c is the pass.

   READ THE BLOCK AT THE FOOT OF THIS FILE FIRST: NOTHING PUBLISHES A
   HAND-OVER, so this pass draws nothing. What follows is the contract it is
   built to, kept because it is the shape a revival needs.

   IT DRAWS INTO THE CAST-SHADOW MAP AND NOWHERE ELSE. A replacement mesh's
   body is not this pass's; only its casters are.

   WHAT IT IS FED (TAGPU_HIHAND, at the foot): the triangles as CPU bytes, the
   per-unit anchor, yaw/enc and cast triple, and the pose rows. No GL name
   crosses.

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

   IT RETURNS 0 (see the foot of this file). A revival has to re-establish the
   invariant the count rests on: it counts the casters that reached the
   hand-over complete and whose device resources exist, and a producer must
   refuse to publish a record short of the map, so the count is 0 or all of
   them and never a part. */
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

   These four types are the shape this pass consumes, kept so that the next
   producer does not have to invent them again.

   THERE IS NO PRODUCER.

   glTF REPLACEMENT MODELS ARE DISABLED AND THEIR IMPLEMENTATION IS TODO AND
   OUT OF SCOPE. Reviving them needs a LOADER first: the glTF parser, the piece
   table, the material grouping and the COB-driven pose, which are in git
   history as `tagpu_hires.c`.
   What this pass owns is the vertex buffers, the descriptors, the pose
   uniform block and the caster draw.
   [gpu-status 2.80.] */

/* The cap every count in the hand-over is checked against. */
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
