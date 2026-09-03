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
    unsigned int palTex, lutTex, fogTex, fogLutTex;
    int   shNeutral, shDir;     /* the engine's SHD rows, for the ramp anchor */
} TAGPU_HVIEW;

typedef struct {
    const void* mesh;           /* from tagpu_hires_mesh()                    */
    float ax, ay;               /* frame-px anchor                            */
    float wx0, wz0;             /* world x and projected world z at the anchor*/
    float enc;                  /* depth key base                             */
    float shadowDy;             /* body -> ground shift for the shadow pass   */
    unsigned yaw;
    float alpha;                /* 0.5 while cloaked                          */
    int   fog;                  /* uFog bits, as the native shader takes them */
    float waterT, digT;
    int   waterMode;
    int   shadow;               /* this unit owes a silhouette shadow         */
} TAGPU_HUNIT;

int  tagpu_hires_draw_ready(void);
/* pass 1 = the silhouette shadow pass, 0 = bodies. Leaves program, VAO and the
   active texture unit dirty: the caller restores what it still needs. */
void tagpu_hires_draw(const TAGPU_HVIEW* v, const TAGPU_HUNIT* u, int n,
                      int shadowPass, unsigned frame_counter);
void tagpu_hires_draw_glreset(void);
#endif
