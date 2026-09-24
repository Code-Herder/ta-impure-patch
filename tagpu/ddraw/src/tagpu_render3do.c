/* tagpu_render3do.c — the material layer the native and posed passes share:
   the unit texture atlas, the face-shade calibration and its multipliers, the
   face material helpers, and the build-state staging formulas. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tagpu_model3do.h"   /* F_COLORTAB: the Model3DOFace layout */
#include "tagpu_render3do.h"
#include "tagpu_pal.h"
#include "tagpu_gaf.h"
#include "tagpu_vk.h"         /* tagpu_vk_owns_present: will a Vulkan pass run at all? */
#include "tagpu_classicpp.h"  /* tagpu_classicpp_assets: the restored twin is only worth mirroring while it is what the twin samples */
#include "tagpu_log.h"

/* The Object3do piece array, PrimitiveStruct and Model3DONode layouts are in
   research/notes/exe-reverse-engineering.md; the pieces this file needs
   arrive in the packet's PK_PIECE table, and the face offsets it reads are
   tagpu_model3do.h's. */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void rlog(const char* s)
{
    tagpu_log(s);
}


static int    s_state = 0;         /* 0=unloaded 1=ready 2=failed */
static int    s_shadeBuilt = 0;

/* ---- the unit texture atlas: the unit textures' GAF frames as palette
   indices, which the Vulkan unit pass expands into its RGBA8 base atlas and
   samples NEAREST. It is a TAGPU_GAFATLAS (tagpu_gaf.c), the same shelf atlas
   the feature and effects passes use, with the unit layout renderers.md 2.5
   decided: every frame in a cell with a 4-texel replicated border, 4-aligned,
   so the Classic++ twin can be mipmapped to level 2 without one frame bleeding
   into the next (tagpu_gaf.h `pad`/`align`/`mip`). The entry's u0..v1 are
   still the frame's OWN texels, edge-mapped (TA maps quad corners to texture
   edges; a centre inset shifts every interior sample half a texel and flips
   ~50% of NEAREST lookups on noisy textures), so Classic's samples never
   reach the border. Recycled when full, at the start of a frame
   (tagpu_r3d_atlas_frame), never between an emit and its draw. 2048^2 holds
   ~1,400 median cells (32x64 frames become 40x72). ---- */
#define ATLAS_DIM  2048
#define ATLAS_MAX  2048
#define ATLAS_PAD  4                      /* tools/tascene UNIT_PAD          */
#define ATLAS_MIP  2                      /* renderers.md 2.9: covers 0.25   */
static TAGPU_GAFENT  s_atlasEnts[ATLAS_MAX];
static TAGPU_GAFATLAS s_atlas;

/* The atlas entry for a unit texture frame, uploading it on first sight.
   NULL for an unreadable frame, a compressed one (the engine's 3DO textures
   are raw planes; a compressed frame draws flat so Classic does not move --
   whether the engine would texture it is not established) or a full atlas
   (recycled next frame). */
static const TAGPU_GAFENT* atlas_get(const char* g)
{
    const unsigned char* f = tagpu_gaf_frame_sane(g);
    if (!f || f[TAGPU_GF_COMP] != 0) return NULL;
    return tagpu_gaf_atlas_get(&s_atlas, f);
}

/* ---- per-face directional shading: the engine's 32 shade rows ----
   The vertex stage chooses a row per face, `neutral + dir * round(12 * N.SH_L)`
   clamped to 0..31 (tagpu_posedraw.c), the way the engine's rasteriser picks a
   PALETTE.SHD row; `s_shNeutral` and `s_shDir` are that table's calibration.
   Without the engine's table the rows are our own ramp, factor 0.60 +
   0.025*row, so row 16 is EXACT 1.0. */
#define SH_ROWS    32
#define SH_NEUTRAL 16
/* WHAT THE CALIBRATION IS BUILT FROM, AND THE KEY THAT REBUILDS IT: the
   engine's table (`s_shadePal`, its serial when the calibration was taken)
   and the engine's shade table (`s_shd`, the last copy a packet carried). A
   move of either rebuilds k[], so the unit pass never draws a calibration
   taken from one table over a base atlas expanded from another.
   THE COPY IS KEPT because a frame may arrive with no shade table -- the first
   fill of a slot truncates until the slot has grown, which is reachable at
   startup -- and a rebuild on such a frame must still calibrate from the
   engine's own table rather than fall back to the computed ramp for the rest of
   the session. */
static unsigned char s_shd[SH_ROWS * 256];
static int           s_haveShd;
static unsigned      s_shadePal;
static int s_shNeutral = SH_NEUTRAL;   /* the row that leaves a colour as it is */
static int s_shDir     = 1;            /* +1 = higher row is brighter           */

/* THE FACE-SHADE MULTIPLIER (bar-camera-port.md 2.2): a shade row in full
   colour. The fragment multiplies its RGB by k[row] and clamps, in both
   presets; the engine remaps the index through the row instead, and the
   multiplier is the full-colour fit of that remap. For each row, k is the
   least-squares slope through the origin of the shaded colour against the
   unshaded one, over every palette
   entry and channel the row does not clip, normalised so the neutral row is
   exactly 1.0.

   "CLIPS" IS THE SHADED CHANNEL AT THE PALETTE'S TOP, >= SH_CLIP. The table
   saturates on the palette's 251 and 252 entries as well as on 255 (the stock
   PALETTE.SHD's rows 16..31 land 263 channels there against 2 509 on 255), and
   a test for 255 alone lets those saturated brights drag the bright rows'
   slope down: MEASURED on the stock table, k[22] and k[27] are 1.391 and 1.603
   with >= 250 and 1.365 and 1.543 with == 255.

   Built from the ENGINE's table, not the presented one: Gamma scales the
   presented palette and clips it again at 255, and the multiplier is a fact
   about the shade table. Without the engine's SHD it is the computed ramp's
   own factor, 0.60 + 0.025 * row (row 16 exactly 1.0). */
#define SH_CLIP 250
static float s_shadeK[SH_ROWS];
static int   s_kBuilt;

static void shade_k_build(const unsigned char* shd, const unsigned char* pal)
{
    char b[400];
    double k[SH_ROWS], n;
    int r, i, c, o;
    if (shd && pal) {
        for (r = 0; r < SH_ROWS; r++) {
            double sxy = 0.0, sxx = 0.0;
            for (i = 0; i < 256; i++) {
                const unsigned char* x = pal + (size_t)i * 4;
                const unsigned char* y = pal + (size_t)shd[r * 256 + i] * 4;
                for (c = 0; c < 3; c++) {
                    if (y[c] >= SH_CLIP) continue;
                    sxy += (double)x[c] * y[c];
                    sxx += (double)x[c] * x[c];
                }
            }
            k[r] = sxx > 0.0 ? sxy / sxx : 1.0;
        }
        n = k[s_shNeutral] > 0.0 ? k[s_shNeutral] : 1.0;
        for (r = 0; r < SH_ROWS; r++) s_shadeK[r] = (float)(k[r] / n);
    } else {
        for (r = 0; r < SH_ROWS; r++) s_shadeK[r] = 0.60f + 0.025f * (float)r;
    }
    s_kBuilt = 1;
    o = _snprintf(b, sizeof b, "r3d shade: face-shade k[] from the %s:",
                  shd ? "engine SHD" : "computed ramp");
    for (r = 0; r < SH_ROWS && o > 0 && o < (int)sizeof b - 8; r++)
        o += _snprintf(b + o, sizeof b - o, " %.3f", (double)s_shadeK[r]);
    b[sizeof b - 1] = 0;
    rlog(b);
}

static void shade_build(const unsigned char* shd)
{
    /* the ENGINE'S table (tagpu_pal.h), the one every world colour is built
       from: a snapshot we own, so this reads no engine memory at all */
    const unsigned char* pal = tagpu_pal_engine();
    int r, i;
    if (!pal) return;

    /* Prefer the ENGINE's own 32x256 shade table (PALETTE.SHD, built at init —
       the table its Gouraud rasteriser 0x459C70 uses; shadows-cloak.md /
       build-state.md). It arrives in the frame packet, copied by the game
       thread, so the render thread never dereferences the graphics globals
       for it. Calibrate rather than assume: neutral = the row with
       the most identity entries, direction from end-row luminance. Fall back
       to our computed ramp when the packet carried none. */
    if (shd) {
        int bestr = -1, bestn = -1;
        long lum0 = 0, lum31 = 0;
        for (r = 0; r < SH_ROWS; r++) {
            int n = 0;
            for (i = 0; i < 256; i++) n += (shd[r*256 + i] == i);
            if (n > bestn) { bestn = n; bestr = r; }
        }
        for (i = 0; i < 256; i++) {
            const unsigned char* c0 = pal + (size_t)shd[0*256 + i]  * 4;
            const unsigned char* c1 = pal + (size_t)shd[31*256 + i] * 4;
            lum0  += c0[0] + c0[1] + c0[2];
            lum31 += c1[0] + c1[1] + c1[2];
        }
        s_shNeutral = bestr;
        s_shDir     = (lum31 >= lum0) ? 1 : -1;
        s_shadeBuilt = 1;
        s_shadePal = tagpu_pal_engine_serial();
        shade_k_build(shd, pal);
        { char b[96]; _snprintf(b, sizeof b,
            "r3d shade: engine SHD table (neutral=%d id=%d/256 dir=%d)",
            s_shNeutral, bestn, s_shDir); rlog(b); }
        return;
    }
    s_shNeutral = SH_NEUTRAL; s_shDir = 1;
    s_shadeBuilt = 1;
    s_shadePal = tagpu_pal_engine_serial();
    shade_k_build(NULL, NULL);
    rlog("r3d shade: computed ramp (32 rows, row 16 exactly 1.0) — no shade table in "
         "the packet yet; it is rebuilt from the engine's own the first frame one arrives");
}

/* Model-space toward-camera axis: depth is 2y-z (larger = nearer), so the
   nearness gradient (0,2,-1)/sqrt5 points at the viewer. Faces that survive
   the depth test face the camera, so flipping each normal into the +V
   hemisphere yields the OUTWARD normal without trusting 3DO winding. */
/* SH_V = (0, 0.8944, -0.4472) */
/* sun: high, from screen upper-left, slightly toward camera:
   SH_L = (-0.35, 0.80, -0.49). Both vectors are kept here as the CALIBRATION
   OF RECORD — tagpu_native.c and tagpu_classicpp.c cite this file by name for
   them — and not as live constants: each pass carries its own copy of the
   numbers in the form its shader wants. */

/* ---- build-state (nanoframe) staging — engine formulas from build-state.md.
   p = Nanoframe*255 runs 255->0 over the build; two triangle-wave "blues"
   bounce over palette ramp 0xA0..0xAF; 5 stages pick a height threshold t and
   the (above, band, below) colours: -2 erase-to-ColorKey, -1 keep, else index. */
static int nano_tri(int ph)
{
    return (ph & 0x10) ? 0xAF - (ph & 0xF) : 0xA0 + (ph & 0xF);
}
static void nano_stage(int p, float b1, float b2, float* t, float c[3])
{
    if (p >= 236)      { *t = (float)((p-235)*255/20);              c[0]=-2; c[1]=b1; c[2]=-2; }
    else if (p >= 201) { *t = (float)((p-200)*255/35);              c[0]=-2; c[1]=b1; c[2]=-2; }
    else if (p >= 116) { *t = (float)(((115-p)*255/85 - 1) & 0xFF); c[0]=-2; c[1]=b2; c[2]=b1; }
    else if (p >= 31)  { *t = (float)(((30-p)*255/85 - 1) & 0xFF);  c[0]=b1; c[1]=b2; c[2]=-1; }
    else               { *t = (float)(p*255/30);                    c[0]=-1; c[1]=b1; c[2]=-1; }
}

/* The whole build-state decision for one unit, in the engine's own terms, in
   one place, so whatever stages the scaffold (the native pass, tagpu_native.c)
   cannot drift from the engine's formulas.

   Fills `t` (the height threshold, in composite depth-plane bytes), `c`
   (cAbove, cBand, cBelow: -2 erase, -1 keep the texture, else a palette index
   over 255) and `wire` (the second blue, the wireframe's colour). Returns 0 —
   touching nothing — when the unit is not a nanoframe, which is every unit
   whose `Nanoframe` (+0x104, the fraction of the build REMAINING) is 0.

   Deliberately does NOT test tagpu_nano.off: each caller reads that flag at
   its own cadence (the native pass once per arm poll; the composite path only
   after this function has said the unit is a nanoframe at all, so the stat
   costs nothing on the units that are not). build-state.md 0x458DD0 /
   0x458D30. */
int tagpu_r3d_nano_state(float nano, unsigned id, unsigned tick,
                         float* t, float c[3], float* wire)
{
    if (!(nano > 0.0f && nano <= 1.0f)) return 0;
    {
        unsigned slot = id;
        int b1 = nano_tri((int)((slot ^ 5) + tick * 0x21 / 0x1E));
        int b2 = nano_tri((int)((slot ^ 9) + tick * 0x39 / 0x1E));
        int p  = (int)(nano * 255.0f);
        if (p > 255) p = 255; else if (p < 0) p = 0;
        *wire = (float)b2 / 255.0f;
        nano_stage(p, (float)b1 / 255.0f, *wire, t, c);
    }
    return 1;
}

/* The two things the passes share: the unit texture atlas and the shade
   calibration. */
static void r3d_init(void)
{
    /* the index atlas, laid out now, before the unit pass's first lookup */
    tagpu_gaf_atlas_lost(&s_atlas);
    s_atlas.dim = ATLAS_DIM; s_atlas.max = ATLAS_MAX;
    s_atlas.ents = s_atlasEnts; s_atlas.tag = "unit";
    /* a world atlas: its base atlas is RGBA, so the key has to travel as a
       plane beside the indices (tagpu_gaf.h `keyPlane`) */
    s_atlas.keyPlane = 1;
    s_atlas.pad = ATLAS_PAD; s_atlas.align = ATLAS_PAD; s_atlas.mip = ATLAS_MIP;
    if (!tagpu_gaf_atlas_create(&s_atlas)) { rlog("render3do: atlas texture FAILED"); s_state = 2; return; }

    /* the shade calibration is built lazily from the packet's table on first
       use (tagpu_r3d_shade_want). The atlas above is laid out whether or not a
       Vulkan pass will consume it (its existence is `made`, tagpu_gaf.h) and
       `s_state = 1` means "the atlas is ready and the shade calibration may be
       built", which is what `tagpu_r3d_ensure` answers for the unit pass. */
    s_state = 1;
    /* THE MIRROR IS ASKED FOR HERE, BEFORE THE FIRST PAINT, and that ordering is
       the whole of it. A mirror allocated after the atlas has filled only marks
       the frames already painted to re-decode "on their next use" -- and the
       bake is cached, so nothing asks the atlas again, that next use never
       arrives and the Vulkan unit pass stands down on a mirror that never
       converges. Measured: 25 s of settled play with the census still
       reading `unit=0`. Asked at arm time, every paint from the first one lands
       in the mirror and nothing needs re-decoding at all. `s_atlas.dim` is set
       a few lines above, which is the precondition `_want` tests, and the call
       is idempotent inside tagpu_gaf.c. */
    /* ASKED OF THE CONSUMER, NOT OF THE LEVER. These latches are one-way, so
    once asked the memory is held for the process's life, and
    `tagpu_vk_armed()` is true whenever `tagpu_vk.on` exists -- including under
    `renderer=gdi`, where the lever arms nothing and the mirror would be
    paid for with no consumer at all. `tagpu_vk_owns_present()` is exactly
    "a Vulkan pass will run in this process", which is the question. */
    if (tagpu_vk_owns_present()) tagpu_r3d_atlas_mirror_want();
    rlog("render3do: ready (unit atlas + shade calibration)");
}

/* Validate a candidate GAFFrame* (same 0x18-byte header as the composite) and
   sample a representative palette index from its 8bpp colour plane: median-ish
   probe of 5 texels, skipping the frame's ColorKey. Returns -1 if the
   candidate is not a sane uncompressed GAFFrame. A fully-transparent texture
   (e.g. the 'ground' footprint quad) returns its ColorKey so the face renders
   transparent, exactly as the engine's textured rasteriser leaves it. */
static int gaf_sample(unsigned p)
{
    if (!ptr_ok((void*)(size_t)p) || IsBadReadPtr((void*)(size_t)p, 0x18)) return -1;
    const char* g = (const char*)(size_t)p;
    int w = *(const unsigned short*)(g + 0x00);
    int h = *(const unsigned short*)(g + 0x02);
    unsigned char ck   = *(const unsigned char*)(g + 0x08);
    unsigned char comp = *(const unsigned char*)(g + 0x09);
    const unsigned char* px = *(const unsigned char* const*)(g + 0x10);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || comp != 0) return -1;
    if (!ptr_ok(px) || IsBadReadPtr(px, (SIZE_T)w * h)) return -1;
    int sx[5] = { w/2, w/4, 3*w/4, w/2,   w/2 };
    int sy[5] = { h/2, h/2, h/2,   h/4, 3*h/4 };
    int i;
    for (i = 0; i < 5; i++) {
        unsigned char c = px[sy[i] * w + sx[i]];
        if (c != ck) return c;
    }
    return ck;   /* fully transparent texture */
}

/* Returns the face's palette index, or -1 for "draw nothing": a face with no
   flat colour AND no resolved texture (e.g. the 'ground' footprint quad) is
   skipped by the engine's rasteriser too — painting it draws a green slab. */
static int face_colour(const char* fa)
{
    /* pColorTable is NOT always a live pointer: live armcom faces carry the
       deterministic non-pointer value 0x01E11F00 there, which passes a naive
       range check and faults on deref. Only
       trust a small value as a literal palette index; never dereference it.
       Textured faces (pColorTable==0): the loader leaves resolved texture
       pointers in the TexState words — live dumps show +0x10 for plain
       textures (ruparm) and +0x18 for the team-colour path (torso) — and the
       target is the ubiquitous GAFFrame header, so validate + sample both. */
    unsigned ct = *(const unsigned*)(fa + F_COLORTAB);
    int s = (ct > 1 && ct < 255) ? (int)ct : -1;
    if (s < 0) s = gaf_sample(*(const unsigned*)(fa + 0x10));
    if (s < 0) s = gaf_sample(*(const unsigned*)(fa + 0x18));
    if (s < 0) {
        unsigned ap = *(const unsigned*)(fa + 0x18);
        if (ptr_ok((void*)(size_t)ap) && !IsBadReadPtr((void*)(size_t)ap, 0x2C)) {
            const char* an = (const char*)(size_t)ap;
            int nfr = *(const unsigned short*)(an + 0x00);
            const unsigned* tab = *(const unsigned* const*)(an + 0x28);
            /* the table is an INLINE GAFFrame[] (stride 0x18), not ptrs */
            if (nfr > 0 && nfr <= 64)
                s = gaf_sample((unsigned)(size_t)tab);
        }
    }
    return s;
}

/* Resolve a face's texture GAFFrame for rendering: plain textures live at
   +0x10 (inline GAFFrame[]); the team-colour path at +0x18 is a GAF anim
   entry whose +0x28 is an inline frame table — pick frame[owner] there
   (clamped), pending confirmation of the exact player->frame mapping.
   Returns NULL for untextured faces. */
static const char* face_texframe(const char* fa, int owner)
{
    unsigned tp = *(const unsigned*)(fa + 0x10);
    if (ptr_ok((void*)(size_t)tp) && !IsBadReadPtr((void*)(size_t)tp, 0x18)) {
        const char* g = (const char*)(size_t)tp;
        int w = *(const unsigned short*)(g + 0x00);
        int h = *(const unsigned short*)(g + 0x02);
        if (w > 0 && h > 0 && w <= 512 && h <= 512 &&
            *(const unsigned char*)(g + 0x09) == 0) return g;
    }
    unsigned ap = *(const unsigned*)(fa + 0x18);
    if (ptr_ok((void*)(size_t)ap) && !IsBadReadPtr((void*)(size_t)ap, 0x2C)) {
        const char* an = (const char*)(size_t)ap;
        int nfr = *(const unsigned short*)(an + 0x00);
        const char* tab = *(const char* const*)(an + 0x28);
        if (nfr > 0 && nfr <= 64 && ptr_ok(tab)) {
            int k = owner < 0 ? 0 : (owner >= nfr ? nfr - 1 : owner);
            return tab + k * 0x18;                /* inline GAFFrame[] */
        }
    }
    return NULL;
}

/* ---- exports for the native pass (tagpu_native.c): share the atlas and
   the shade calibration so both paths draw identical materials ---- */
/* IS THERE A RESTORE ROUTE AT ALL. The route is the published list, and this
   is what says it exists. Latched by the arm, so it does not flicker. */
int tagpu_r3d_atlas_restore_armed(void) { return s_atlas.rlistWant ? 1 : 0; }
unsigned tagpu_r3d_atlas_gen(void)  { return s_atlas.gen; }
/* ---- THE LEVEL BOUNDARY ------------------------------------------------
   THIS ATLAS KEYS ON AN ADDRESS AND THE ADDRESSES ARE RECYCLED.
   `tagpu_gaf_atlas_find(a, g, pix, w, h, win)` matches on the frame header's
   address and the pixel plane's, so an entry is only right for as long as
   that address means that art. The engine's per-level teardown frees the
   model textures and the next level's loader is free to hand a new frame the
   address an old one had -- at which point this atlas serves the PREVIOUS
   level's texels for it, and nothing anywhere would detect it: the entry is
   valid, the UV is in range, the picture is simply wrong.

   `tagpu_fx.c` and `tagpu_feat.c` drop theirs at this boundary for exactly
   this reason. The atlas's other reset -- when FULL -- is not a level
   boundary.

   It is called from `tagpu_native_frame` beside `cache_gen_check` and
   `tagpu_posebake_frame`, the other two level-keyed caches, rather than from
   `tagpu_r3d_atlas_frame` lower down this file: `tagpu_posebake_frame` LATCHES
   `tagpu_r3d_atlas_gen()` for the frame, so a drop after it would leave this
   frame's bakes stamped with the generation before the drop and cost a second,
   pointless drop on the next frame. Taking it here means every consumer sees
   one generation for the whole frame.

   The cost of being right is one re-decode of whatever is on screen at a level
   change -- the same cost `tagpu_fx.c` pays -- and the first frame of a
   session drops nothing, because `s_atlasGen` starts at 0 and no level's
   generation encodes to that. */
void tagpu_r3d_atlas_level(unsigned level_gen)
{
    static unsigned s_atlasGen;          /* 0 = no level seen yet */
    unsigned g = level_gen + 1u;         /* so that 0 stays "nothing seen" */
    if (g == s_atlasGen) return;
    if (s_atlasGen && s_state == 1) tagpu_gaf_atlas_forget(&s_atlas);
    s_atlasGen = g;
}

void tagpu_r3d_atlas_frame(void)
{
    if (s_state != 1) return;
    if (s_atlas.full) tagpu_gaf_atlas_reset(&s_atlas);
    /* No palette here: this atlas's restore is the Vulkan restorer's --
       `tagpu_gaf_atlas_restore_vk` publishes the frame list and
       `tagpu_vk_restore.c` paints it, each reading the palette for itself. */
}
/* `shd` is the packet's copy of PALETTE.SHD, or NULL: the calibration and
   the multipliers are built out of the last one seen, or the computed ramp
   before any has been, and rebuilt when the shade table's bytes or the
   engine's palette serial move (`s_shadePal`). Called every frame from
   tagpu_native.c; it is what keeps `s_shadeK` current for the Vulkan unit
   pass. */
void tagpu_r3d_shade_want(const unsigned char* shd)
{
    if (s_state != 1) return;
    if (shd && (!s_haveShd || memcmp(shd, s_shd, sizeof s_shd) != 0)) {
        memcpy(s_shd, shd, sizeof s_shd);
        s_haveShd = 1;
        s_shadeBuilt = 0;
    }
    if (!s_shadeBuilt || s_shadePal != tagpu_pal_engine_serial())
        shade_build(s_haveShd ? s_shd : NULL);
}

/* ---- the Vulkan lane's texels; tagpu_render3do.h has the contract ------- */
void tagpu_r3d_atlas_mirror_want(void)
{
    /* Idempotent inside tagpu_gaf.c, and asked only once the atlas has its
       dimensions -- before that there is nothing to size the mirror to, which
       is the first frames of a session. */
    if (s_state == 1 && !s_atlas.mirror && s_atlas.dim > 0)
        tagpu_gaf_atlas_mirror(&s_atlas);
}

const unsigned char* tagpu_r3d_atlas_mirror(int* dim, int* rows, unsigned* serial)
{
    int r;
    if (dim) *dim = 0;
    if (rows) *rows = 0;
    if (serial) *serial = 0;
    if (!s_atlas.mirror || s_atlas.dim <= 0) return NULL;
    /* the shelf cursor bounds every painted cell, so only the rows the packer
       has used need uploading -- the feature pass's own bound, and for the same
       reason: the rest of a 2048-square atlas has never been written */
    r = s_atlas.shelfY + s_atlas.shelfH;
    if (r < 0) r = 0;
    if (r > s_atlas.dim) r = s_atlas.dim;
    if (dim) *dim = s_atlas.dim;
    if (rows) *rows = r;
    if (serial) *serial = s_atlas.mirrorSerial;
    return s_atlas.mirror;
}

const unsigned char* tagpu_r3d_atlas_key(const TAGPU_GAFBAND** bands)
{
    if (bands) *bands = s_atlas.mirror ? s_atlas.band : NULL;
    return s_atlas.mirror ? s_atlas.keym : NULL;
}

/* THE LIST, THE ONLY ANSWER TO THE QUESTION (the shape features and effects
   use). Under Classic++ `assets=1` the Vulkan restorer (tagpu_vk_restore.c)
   paints the twin from it. Polled on every beat until it takes, because the lever may appear
   mid-session. */
static int s_rlistAsked;

void tagpu_r3d_atlas_restore_want(void)
{
    if (s_state != 1 || !s_atlas.mirror || s_atlas.dim <= 0 ||
        !tagpu_classicpp_assets())
        return;
    if (!s_rlistAsked) s_rlistAsked = tagpu_gaf_atlas_restore_vk(&s_atlas);
    /* AND THE RATIO THE TWIN IS TO BE FILTERED AT. This is `rgbAniso`'s only
       writer, and the consumer stands a frame down when it disagrees with the
       ratio its sampler was built at.

       IT IS THE KNOB, NOT A CONSTANT, AND NOT THE CLAMP. Publishing a fixed
       default ratio would stand the pass down on any device without
       anisotropic filtering, because there `s_twinAniso` is 0.0f. Publishing
       what the consumer actually applies would make the test compare a value
       against itself. The knob is the one thing both ends read independently
       (`tagpu_classicpp_light()->aniso`, the same field the Vulkan sampler is
       built from), so comparing them still catches the case the sampler's own
       comment names -- a knob edited mid-session, when the sampler cannot be
       rebuilt because every slot's submit still names it -- and catches
       nothing else. Written every beat rather than at the arm, because that
       case is exactly a value that changes after the arm has latched. */
    s_atlas.rgbAniso = tagpu_classicpp_light()->aniso;
}

/* THE LIST, AND THE TWIN'S SHAPE WITH IT. There is no read-back to carry the
   shape, so `dim` and `mips` come from the atlas -- and so does `aniso`: it
   is the ratio the twin was filtered with, and a consumer that cannot apply
   the same one draws different art wherever a unit is minified at an angle.
   What it publishes is the KNOB (written by tagpu_r3d_atlas_restore_want
   above), not the constant in `tagpu_gaf.h` and not the value the device
   allowed -- see `rgbAniso` there for why those are three different numbers.
   This is the ONLY reporter of it. NULL until the list has been armed AND has
   entries; a consumer that gets NULL falls back to whatever it did before,
   which is the indexed atlas. */
const TAGPU_RGLSL_FRAME* tagpu_r3d_atlas_restore_list(int* dim, int* n, unsigned* gen,
                                                      int* mips, float* aniso)
{
    if (dim) *dim = 0;
    if (n) *n = 0;
    if (gen) *gen = 0;
    if (mips) *mips = 0;
    if (aniso) *aniso = 0.0f;
    if (!s_atlas.rlistWant || !s_atlas.rlist || s_atlas.dim <= 0) return NULL;
    if (dim) *dim = s_atlas.dim;
    if (n) *n = s_atlas.rlistN;
    if (gen) *gen = s_atlas.rlistGen;
    if (mips) *mips = s_atlas.mip;
    if (aniso) *aniso = s_atlas.rgbAniso;
    return s_atlas.rlist;
}

const float* tagpu_r3d_shade_k(void)
{
    return s_kBuilt ? s_shadeK : NULL;
}

int tagpu_r3d_shade_neutral(void) { return s_shNeutral; }
int tagpu_r3d_shade_dir(void)     { return s_shDir; }
int tagpu_r3d_atlas_uv(const char* g, float uv[4], float* ck)
{
    if (s_state != 1) return 0;
    const TAGPU_GAFENT* e = atlas_get(g);
    if (!e) return 0;
    uv[0] = e->u0; uv[1] = e->v0; uv[2] = e->u1; uv[3] = e->v1;
    *ck = (float)e->ck / 255.0f;
    return 1;
}
int tagpu_r3d_ready(void) { return s_state == 1; }
int tagpu_r3d_ensure(void)             /* init on demand */
{
    if (s_state == 0) r3d_init();
    return s_state == 1;
}
const char* tagpu_r3d_face_texframe(const char* fa, int owner) { return face_texframe(fa, owner); }
int tagpu_r3d_face_colour(const char* fa) { return face_colour(fa); }

