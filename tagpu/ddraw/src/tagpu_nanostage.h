#ifndef TAGPU_NANOSTAGE_H
#define TAGPU_NANOSTAGE_H
/* tagpu_nanostage.h -- the engine's nanoframe staging (build-state.md, 0x458DD0 / 0x458D30) as one
   pure function, so every caller stages a unit under construction with the same arithmetic: the
   game's world through tagpu_render3do.c `tagpu_r3d_nano_state`, and the in-game wiki's stage
   through tagpu_wikiscene.c, which tools/wiki/scenecheck.mjs compiles natively and pairs with the
   mockup's port (tools/wiki/mockup/scene.js `nanoState`). No global, no engine header, no Win32.

   p = Nanoframe*255 runs 255->0 over the build; two triangle-wave "blues" bounce over palette ramp
   0xA0..0xAF; 5 stages pick a height threshold t and the (above, band, below) colours: -2
   erase-to-ColorKey, -1 keep, else index/255. */

static __inline int tagpu_nanostage_tri(int ph)
{
    return (ph & 0x10) ? 0xAF - (ph & 0xF) : 0xA0 + (ph & 0xF);
}

static __inline void tagpu_nanostage_stage(int p, float b1, float b2, float* t, float c[3])
{
    if (p >= 236)      { *t = (float)((p-235)*255/20);              c[0]=-2; c[1]=b1; c[2]=-2; }
    else if (p >= 201) { *t = (float)((p-200)*255/35);              c[0]=-2; c[1]=b1; c[2]=-2; }
    else if (p >= 116) { *t = (float)(((115-p)*255/85 - 1) & 0xFF); c[0]=-2; c[1]=b2; c[2]=b1; }
    else if (p >= 31)  { *t = (float)(((30-p)*255/85 - 1) & 0xFF);  c[0]=b1; c[1]=b2; c[2]=-1; }
    else               { *t = (float)(p*255/30);                    c[0]=-1; c[1]=b1; c[2]=-1; }
}

/* `nano` is the fraction of the build REMAINING (unit +0x104), `id` the unit's slot and `tick` the
   simulation's. Fills `t` (the height threshold, in composite depth-plane bytes), `c` (cAbove,
   cBand, cBelow) and `wire` (the second blue, the wireframe's colour), and returns 1; returns 0 --
   touching nothing -- for a unit that is not a nanoframe, whose fraction is 0 (or not in (0, 1]). */
static __inline int tagpu_nanostage(float nano, unsigned id, unsigned tick, float* t, float c[3],
                                    float* wire)
{
    if (!(nano > 0.0f && nano <= 1.0f)) return 0;
    {
        unsigned slot = id;
        int b1 = tagpu_nanostage_tri((int)((slot ^ 5) + tick * 0x21 / 0x1E));
        int b2 = tagpu_nanostage_tri((int)((slot ^ 9) + tick * 0x39 / 0x1E));
        int p  = (int)(nano * 255.0f);
        if (p > 255) p = 255; else if (p < 0) p = 0;
        *wire = (float)b2 / 255.0f;
        tagpu_nanostage_stage(p, (float)b1 / 255.0f, *wire, t, c);
    }
    return 1;
}

#endif
