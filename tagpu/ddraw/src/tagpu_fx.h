#ifndef TAGPU_FX_H
#define TAGPU_FX_H
/* Effects pass (weapon fire, explosions, debris) — gathered from the live
   engine arrays the two engine passes read (projectiles 0x49BE60, explosions
   0x420B00), rendered natively into the native pass's FBO. Armed by
   tagpu_fx.on. See research/notes/effects.md. */
#include "tagpu.h"

/* everything the effects gather/render needs from the native pass's frame */
typedef struct TAGPU_FXVIEW {
    const char* ta;              /* TAdynmem                                   */
    int eyeX, eyeY, vpL, vpT;
    int gw, gh, ss;
    float zoom, zoomCx, zoomCy;  /* the native pass's view zoom (G12d demo)    */
    float encSprite, depthScale; /* this frame's sprite depth key and VS scale */
    int fogMode;                 /* LosType & 3                                */
    const unsigned char* los;    /* per-32px-tile 255/0 (watched player)       */
    const unsigned char* mapd;
    int losW, losH;
    unsigned int frame_counter;
} TAGPU_FXVIEW;

/* one 3DO node to emit through the native geometry path */
typedef struct TAGPU_FXMODEL {
    const char* node;            /* Model3DONode*                              */
    float ax, ay;                /* anchor in frame px (engine projection)     */
    float wx, wz;                /* world x, projected world z (fog lookup)    */
    short turn[3];               /* engine rotation triple (65536 = 360 deg)   */
    int   owner;
} TAGPU_FXMODEL;

int  tagpu_fx_armed(unsigned frame_counter);    /* tagpu_fx.on present (30f) */
int  tagpu_fx_gather(const TAGPU_FXVIEW* v);    /* returns total drawables    */
int  tagpu_fx_nmodels(void);
const TAGPU_FXMODEL* tagpu_fx_model(int i);
/* lines + sprites into the currently bound FBO (depth test on, mask off);
   uses its own program/VAO; leaves program/VAO/texture bindings dirty */
void tagpu_fx_render(const TAGPU_FXVIEW* v, unsigned int palTex,
                     unsigned int losTex, unsigned int mapTex);
void tagpu_fx_glreset(void);
#endif
