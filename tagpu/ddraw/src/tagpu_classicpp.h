#ifndef TAGPU_CLASSICPP_H
#define TAGPU_CLASSICPP_H
/* The Classic++ switch and its knobs. The menu's rows -- Renderer, Undithered
   assets, Dynamic lighting, Shadows, Shadow quality -- live in the settings
   store (tagpu_settings.h, renderers.md 2.10b); the two files below are the
   LEVERS over it: a file, or a key inside the cfg, beats the store for what it
   names, and the menu greys the row it holds.

     tagpu_classicpp.on    the renderer switch, and the master arm: present =
     tagpu_classicpp.off   on, `.off` = off, neither = the store's Renderer
                           row (tagpu_opt.c). Off is Classic, exactly the
                           1997 pixels; on is the restored atlases, the
                           lighting below, the RGB fog rule (2.6), and the
                           shadows of 2.12.
     tagpu_classicpp.cfg   numeric knobs, `key=value` tokens separated by
                           whitespace or newlines, keyed like the lab's URL
                           parameters (tascene-design.md). Two of them
                           subdivide the switch -- they only ever narrow
                           it, so both at 1 with the .on file present is
                           exactly the switch alone:
                             aniso=N        restored-twin anisotropy, default 4
                                            (1 = off, which is what the
                                            Vulkan A/B is taken at);
                             assets=0|1     the restored atlases, default 1;
                                            0 draws Classic++ from the 8bpp
                                            indices, and pauses the restore
                                            jobs where they stand
                             light=0|1      the lambert below, default 1;
                                            0 draws Classic++ flat, and keeps
                                            the shadows
                             sun=AZ,EL      the terrain's sun, degrees
                                            (azimuth, elevation); `sun=off`
                                            is another spelling of `light=0`
                                            and does exactly that
                             unitsun=AZ,EL  the units' sun
                             amb=A          the ambient floor, 0..1
                           and the shadows' (renderers.md 2.12; tagpu_shadow.c):
                             shadows=0|1|2      0 none, 1 SOFT (the map-anchored
                                                depth map of 2.12, which HAS NO
                                                PRODUCER on this lane and draws
                                                nothing -- see tagpu_classicpp.c),
                                                2 HARD (Classic's own silhouette
                                                and slant, drawn under the
                                                switch), THE DEFAULT. Any of
                                                them also needs the engine's own
                                                Shadow option bit (+0x37F06
                                                bit2), and the silhouette needs
                                                bit3 as well; the keys below
                                                describe the soft map only
                             shadowsun=AZ,EL    the shadows' light, 225,40
                             penumbra=K         kernel radius per world unit of
                                                blocker distance, 0.05; 0 = hard
                             shadowlen=A,B|off  a caster's length a + b*height,
                                                14,0.25; off = physical
                             shade=S            the direct light a shadow
                                                removes, 1
                             terrainshadow=0|1  the hills cast too, DEFAULT 0
                                                -- it self-shadows the ground
                                                (renderers.md 2.7b); 1 is the
                                                fixture for fixing it
                             shadowres=N        the map's edge at zoom >= 1,
                                                2048 (256..4096)
                             airshadow=len|physical|drop   an airborne caster
                                                (2.2), len
                           A key the cfg does not name takes the store's
                           value, and without the store (tagpu_defaults.off)
                           the lab's default.

   Both are polled at most twice a second; the cfg is re-read when its write
   time or size changes, so `tacli arm <i> 'classicpp.cfg=sun=off'` takes
   effect within the next poll, live. */

/* The master arm: the .on file alone. Everything Classic++ owns is under it,
   and the two keys below only subdivide what it already allows. The shadow
   dimension reads THIS -- `shadows=` is its own key already, and the Classic
   sub-passes it replaces (tagpu_native.c, renderers.md 2.12) are suppressed by
   being in the Classic++ branch at all, not by which half of it is on. */
int tagpu_classicpp_on(void);

/* The menu keys the cfg itself names, TAGPU_HELD_*: those rows are held by the
   lever and greyed. `sun=off` holds the lighting row, being its old spelling. */
#define TAGPU_HELD_ASSETS    1u
#define TAGPU_HELD_LIGHT     2u
#define TAGPU_HELD_SHADOWS   4u
#define TAGPU_HELD_SHADOWRES 8u
unsigned tagpu_classicpp_held(void);

/* The two halves of the switch (renderers.md 2.10's `Undithered assets`
   and `Dynamic lighting` rows). Each is the master arm AND its own cfg key, so
   a caller asks one question rather than two, and neither can be on while the
   .on file is absent. */
int tagpu_classicpp_assets(void);   /* the restored atlases: uRestored, and the restore jobs */
int tagpu_classicpp_lit(void);      /* the lambert: uLambert, and the baked one in tagpu_feat.c */

/* NOT the branch. `uLit` -- the Classic++ colour path in the terrain, unit and
   feature shaders, which also carries the RGB fog rule (renderers.md 2.6) --
   follows tagpu_classicpp_on(), because turning one half off must not drop the
   frame back to Classic. `light=0` is the level normal handed to taLambert
   (tagpu_glsl.h) and 1.0 baked into a feature's anchor; `assets=0` is
   `uRestored` 0, which the branch reads. */

/* The lighting the knobs describe, in the lab's terms (tascene-view.html
   readLook / LAB_LIGHT): a sun as the unit vector TOWARD the light in map
   space (x east, y up, z south), the ambient floor, and what LEVEL ground
   receives from that sun -- `level = amb + (1-amb)*max(sun.y, 0)` -- which
   every lambert is divided by so that level is exactly 1.0 (the art is
   already lit; the sun may only modulate by the tilt from level). The
   shaders take 1/level as uNorm (tagpu_glsl.h TAGPU_GLSL_LIGHT_FN).
   `sun=off` is `light=0`: the level normal, so the rule is exactly 1.0 and
   the shadow term inside it survives. It does not move `amb`. */
/* shadows=: which shadow the Classic++ frame draws (renderers.md 2.10's
   Off | Hard | Soft row). HARD is Classic's pair -- the 5-px silhouette
   and the cached slant -- emitted under the switch instead of the depth map;
   the two are never both on. Classic itself is not governed by this key: the
   engine's option bits rule there, and this file is the Classic++ knob. */
#define TAGPU_SHADOWS_OFF   0
#define TAGPU_SHADOWS_SOFT  1
#define TAGPU_SHADOWS_HARD  2
#define TAGPU_AIRSHADOW_LEN      0
#define TAGPU_AIRSHADOW_PHYSICAL 1
#define TAGPU_AIRSHADOW_DROP     2
typedef struct {
    float sun[3];
    float unitSun[3];
    float amb;
    float level, unitLevel;
    /* the shadows (renderers.md 2.12): the lab's knobs at the lab's defaults */
    int   shadows;          /* shadows=: TAGPU_SHADOWS_*                  */
    float shadowSun[3];     /* toward the light the shadows fall from     */
    float penumbra;
    int   shadowlenOn;      /* 0 = shadowlen=off, the physical length     */
    float shadowlen[2];     /* a + b * model height                       */
    float shade;
    int   terrainshadow;
    int   shadowres;
    int   airshadow;        /* TAGPU_AIRSHADOW_*                          */
    /* THE ANISOTROPY THE RESTORED TWINS ARE FILTERED WITH, `aniso=` (default 4,
       1 = off). It is a knob rather than a constant because ANISOTROPIC SAMPLE
       PLACEMENT IS IMPLEMENTATION-DEFINED and GL and Vulkan do it differently
       on the same hardware: MEASURED 2026-09-16, restored unit art is 0 px
       apart between the two lanes at `aniso=1` and up to 9 levels apart on 27 %
       of unit pixels at 4x. PLAY keeps 4x on both lanes and the A/B is
       taken at `aniso=1` as a stated substitution -- so the oracle still
       catches every porting mistake and excludes only the one thing the specs
       leave free. That is what this knob is for; it is not a quality setting
       to reach for. [gpu-status 2.37.] */
    float aniso;
} TAGPU_LIGHT;
const TAGPU_LIGHT* tagpu_classicpp_light(void);

/* The terrain rule evaluated on the CPU for a normal `n` (unit length, map
   space): the lab's lambertAt, for the passes that light a billboard by the
   ground under it. Exactly 1.0 for a level normal -- the same expression
   computes `level`, so the quotient is x/x.

   This is the RAW rule and does not read `light=` itself: its one caller
   (tagpu_feat.c) reads the flag once per frame and bakes 1.0 into the vertex
   attribute instead of calling this, so the key is applied in one place and a
   poll landing mid-gather cannot make one frame's anchors disagree. */
float tagpu_classicpp_ground(const float n[3]);
#endif
