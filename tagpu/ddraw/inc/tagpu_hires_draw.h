#ifndef TAGPU_HIRES_DRAW_H
#define TAGPU_HIRES_DRAW_H
/* The replacement-mesh RENDER PASS: its own GL program and vertex format, so a
   glTF unit is shaded like a modern asset (per-pixel light, normal maps,
   metallic/roughness, mipmapped true colour) instead of being squeezed through
   the engine's palette-index shader. See tagpu_hires_draw.c. */

/* everything the pass shares with tagpu_native.c's frame — the contract that
   makes our units composite with engine units, terrain and effects */
typedef struct {
    float game[2];              /* game_width, game_height                    */
    float zoom,  zoomC[2];      /* view zoom and its centre, game px          */
    float depthScale;           /* > every depth key in use this frame        */
    float ss;                   /* supersample factor (1 or 2)                */
    int   scafOn;
    unsigned int scafTex;
    float scafP[4];             /* vpL, vpT, vw, vh                           */
    float fogOrg[2], fogDim[2];
    /* the shared textures the pass samples: it binds these itself, on the
       units its shader names (1 LUT, 2 palette, 3 scaffold, 4 fog grid,
       5 fog LUT), so it does not depend on being called while the native
       pass's own binds happen to still be live */
    unsigned int palTex, lutTex, fogTex, fogLutTex;
    int   shNeutral, shDir;     /* the engine's SHD rows, for the ramp anchor */
} TAGPU_HVIEW;

typedef struct {
    const void* mesh;           /* from tagpu_hires_mesh()                    */
    /* The engine's COB pose, one 4x3 row-major matrix (3 vec4) per piece, in
       the order tagpu_hires_piece() names them: it carries a REST vertex to
       where the script is holding that piece this frame. NULL = rest pose.
       A hidden piece (COB HIDE) arrives as an all-zero matrix, which collapses
       its triangles to a point and so draws nothing. */
    const float* pose;
    int   npose;                /* pieces covered by `pose`                   */
    float ax, ay;               /* frame-px anchor                            */
    float wx0, wz0;             /* world x and projected world z at the anchor*/
    float enc;                  /* depth key base                             */
    float shadowDy;             /* body -> ground shift for the shadow pass   */
    unsigned yaw;
    float alpha;                /* 0.5 while cloaked                          */
    int   fog;                  /* uFog bits, as the native shader takes them */
    float waterT, digT;
    int   waterMode;
    int   shadow;               /* this unit owes a shadow                    */
    int   slant;                /* ...the structure kind: the engine's ground
                                   projection x+y/4, -z-y/4 instead of the
                                   body's silhouette (tagpu_native.c)         */
    /* Classic++ shadows (G14i): the caster's three numbers for the depth
       pass -- altitude, ground + throw, the length rule's scale -- and
       whether this unit stays out of it (a nanoframe, an aircraft under
       airshadow=drop); `air` for the silhouette that `drop` keeps */
    float cast[3];
    int   castSkip;
    int   air;
} TAGPU_HUNIT;

/* 0 when the pass cannot draw, so the caller can leave those units to the
   native pass instead of to nobody. Builds the program on the first call —
   CALL ONLY WITH A CURRENT GL CONTEXT. */
int  tagpu_hires_draw_ready(void);
/* pass 1 = the silhouette shadow pass, 0 = bodies. Leaves program, VAO and the
   active texture unit dirty: the caller restores what it still needs. */
void tagpu_hires_draw(const TAGPU_HVIEW* v, const TAGPU_HUNIT* u, int n,
                      int shadowPass, unsigned frame_counter);
void tagpu_hires_draw_glreset(void);
/* Classic++ shadows: the replacement meshes into the shadow map along
   `shadowMat` (tagpu_shadow.c's), posed as the body would be, the material's
   alpha cutout kept; the caller has the shadow FBO bound and takes the
   program back afterwards */
void tagpu_hires_depth(const TAGPU_HVIEW* v, const TAGPU_HUNIT* u, int n,
                       const float* shadowMat);

/* ---- the hand-over to the Vulkan lane (the Vulkan-only plan's gate 3b) ----

   WHAT CROSSES IS WHAT `tagpu_hires_depth` ABOVE DREW, recorded by that
   function as it draws rather than re-derived afterwards -- the same discipline
   as tagpu_posedraw.c's depth twin, and for the same reason: `castSkip`, a
   missing VAO and a zero-count group each drop a unit from the GL map, and a
   second pass that re-evaluates those tests can disagree with the first while
   both look right.

   NO GL NAME IS IN HERE. The triangles are `tagpu_hires_verts`' CPU copy, the
   textures are not carried at all (below), and every pointer is valid for the
   frame it was published for, exactly as TAGPU_PDHAND's arrays are. */
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

/* Exactly once per frame, and only for the frame it was published for --
   `now` is the fork's render-thread counter, COMPARED against the frame the
   record was made on. 0 = nothing to draw. */
int  tagpu_hires_handover(TAGPU_HIHAND* out, unsigned now);

/* This frame's counter, and the previous frame's record dropped with it. Called
   unconditionally once per frame from `tagpu_native_frame`, beside
   `tagpu_posedraw_frame`: `tagpu_hires_depth` runs only when there IS a
   replacement mesh, so without this a record outlives the frame it describes. */
void tagpu_hires_frame(unsigned frame_counter);
#endif
