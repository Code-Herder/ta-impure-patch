/* tagpu_render3do.c — Phase B: real GPU geometry into TA's compositor.

   For one unit per frame we take the ENGINE-POSED vertex buffers
   (PrimitiveStruct+0x22, refreshed by TA's lazy repose because we do NOT
   suppress DrawUnit), triangulate the Model3DONode face lists, render them
   flat-coloured into an offscreen FBO with TA's own dimetric projection
   (sx = +x, sy = -z - y/2, hotspot-anchored), read the pixels back and write
   them straight into the GAFFrame colour plane at Object3do+0x10 that TA's
   blit (0x459200 -> CopyGafToContext) stamps onto the frame.

   Palette trick: faces carry TA palette indices, so the FBO renders the INDEX
   itself (in the red channel, flat-interpolated) — readback needs no
   nearest-match pass and is exact. Background clears to index 1 = ColorKey.
   The depth PLANE of the composite is left as the engine wrote it: it encodes
   elevation for the cargo z-merge only (ground blit never reads it) and our
   silhouette matches the engine's because we render the same posed verts with
   the same projection. GL self-occlusion uses the true dimetric view depth
   (2y - z), resolved per-pixel by the FBO depth test. */

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_model3do.h"   /* TAGPU_PBMAXPIECE: the piece-count bound */
#include "tagpu_render3do.h"
#include "tagpu_pal.h"
#include "tagpu_gaf.h"
#include "tagpu_vk.h"         /* tagpu_vk_owns_present: is there a GL lane at all? */
#include "tagpu_classicpp.h"  /* tagpu_classicpp_assets: the restored twin is only worth mirroring while it is what the twin samples */
#include "tagpu_r3dcache.h"

/* NO ENGINE LAYOUT HERE ANY MORE. This file carried nine offsets — the
   Object3do piece array, PrimitiveStruct and Model3DONode — for the write-back
   the frame packet's landing 3 deleted. Every one of them became unused with
   it, which is the check that the deletion was complete rather than partial.
   They live on in research/notes/exe-reverse-engineering.md, and the pieces
   this file still needs arrive in the packet's PK_PIECE table. */
#define N_FACES       0x28     /* Model3DONode.pFaceArray                  */
#define FACE_STRIDE   0x20     /* Model3DOFace                             */
#define F_COLORTAB    0x00     /* PaletteEntry resolved to a table pointer */
#define F_VCOUNT      0x04     /* vertex indices in this face              */
#define F_TEXNAME     0x08     /* char* GAF frame name, 0 = flat colour    */
#define F_INDICES     0x0C     /* u16* vertex indices                      */
#define GF_WIDTH      0x00     /* GAFFrame u16                             */
#define GF_HEIGHT     0x02
#define GF_HOTX       0x04     /* s16 model-origin pixel inside the sprite */
#define GF_HOTY       0x06
#define GF_PTRCOLOR   0x10     /* u8* colour plane, top-down, stride=W     */

#define FBO_DIM   640          /* stock composite is AABB-capped 600x600   */
#define MAXVERTS  24576        /* triangulated vertices per unit per frame */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void rlog(const char* s)
{
    FILE* fl = fopen("tagpu.log", "a");
    if (fl) { fprintf(fl, "%s\n", s); fclose(fl); }
}


static int    s_state = 0;         /* 0=unloaded 1=ready 2=failed */
void tagpu_r3d_glreset(void);
static int    s_lutBuilt = 0;
/* 1 when the LUT that is up came from the ENGINE's own PALETTE.SHD rather than
   our computed ramp. Without it a single frame that arrived with no table —
   the first fill of a slot truncates until the slot has grown, so this is
   reachable at startup — would latch the fallback for the whole session and
   every unit would be shaded by the wrong ramp, quietly. */
static int    s_lutFromShd = 0;

/* ---- 8bpp texture atlas: the unit textures' GAF frames as palette indices
   in an R8 texture, NEAREST-sampled => the FBO stays index-exact. Since G14g
   it is a TAGPU_GAFATLAS (tagpu_gaf.c), the same shelf atlas the feature and
   effects passes use, with the unit layout renderers.md 2.5 decided: every
   frame in a cell with a 4-texel replicated border, 4-aligned, so the
   Classic++ twin can be mipmapped to level 2 without one frame bleeding into
   the next (tagpu_gaf.h `pad`/`align`/`mip`). The entry's u0..v1 are still
   the frame's OWN texels, edge-mapped (TA maps quad corners to texture
   edges; a centre inset shifts every interior sample half a texel and flips
   ~50% of NEAREST lookups on noisy textures), so Classic's samples never
   reach the border and its pixels do not move. Recycled when full, at the
   start of a frame (tagpu_r3d_atlas_frame) or of a blit-path render, never
   between an emit and its draw. 2048^2 holds ~1,400 median cells (32x64
   frames become 40x72); the old 1024^2 with a 1-texel gap held 256 entries
   and drew a 257th flat. ---- */
#define ATLAS_DIM  2048
#define ATLAS_MAX  2048
#define ATLAS_PAD  4                      /* tools/tascene UNIT_PAD          */
#define ATLAS_MIP  2                      /* renderers.md 2.9: covers 0.25   */
static TAGPU_GAFENT  s_atlasEnts[ATLAS_MAX];
static TAGPU_GAFATLAS s_atlas;

/* The atlas entry for a unit texture frame, uploading it on first sight.
   NULL for an unreadable frame, a compressed one (the engine's 3DO textures
   are raw planes; a compressed frame drew flat before G14g too, kept so
   Classic does not move -- whether the engine would texture it is not
   established) or a full atlas (recycled next frame). */
static const TAGPU_GAFENT* atlas_get(const char* g)
{
    const unsigned char* f = tagpu_gaf_frame_sane(g);
    if (!f || f[TAGPU_GF_COMP] != 0) return NULL;
    return tagpu_gaf_atlas_get(&s_atlas, f);
}

/* ---- per-face directional shading (G10): palette-aware shade LUT ----
   The composite stays 8bpp, so "lighting" = remapping each palette index to
   the palette's nearest entry to rgb*factor. 32 brightness rows, factor
   0.60 + 0.025*row => row 16 is EXACT 1.0 and forced to identity (shade off
   and shade-neutral are bit-identical to the unshaded renderer). Candidate
   indices 2..254 only — never emit reserved 0/1(ColorKey)/255. Built once
   from the live in-game palette.
   [`tagpu_shade.off` USED TO DISABLE THE REMAP PER FRAME and this line still
   said so on 2026-09-15, when the Vulkan unit pass went looking for it as an
   A/B lever: nothing anywhere in the tree reads that file. The lever is gone;
   only the sentence survived it.] */
#define SH_ROWS    32
#define SH_NEUTRAL 16
static int s_shNeutral = SH_NEUTRAL;   /* LUT row that is identity/neutral      */
static int s_shDir     = 1;            /* +1 = higher row is brighter           */
/* THE LUT'S CPU MIRROR (Phase G / G19e). 8 KB, built once per context and
   again when the engine's own table first arrives, so it is simply kept rather
   than put behind a latch -- and it is the buffer this very call hands GL, in
   the same call, so the two cannot differ. */
static unsigned char s_lutMirror[SH_ROWS * 256];
static unsigned      s_lutSerial;

static void shade_upload(const unsigned char* lut)
{
    /* THE GL UPLOAD STOOD HERE; THE LUT IS THE PASS [landing 11-3]. The texture
       it filled had one consumer, the GL unit shader, which went with the draw
       halves. `s_lutMirror` below is what the Vulkan twin samples and is the
       whole of what this function does now. */
    memcpy(s_lutMirror, lut, sizeof s_lutMirror);
    s_lutSerial++;
    s_lutBuilt = 1;
}
static void shade_build_lut(const unsigned char* shd)
{
    /* the palette the screen is SHOWN with (tagpu_pal.h): a snapshot we own,
       so this reads no engine memory at all -- which also retires the
       IsBadReadPtr that used to stand in for a bound here */
    const unsigned char* pal = tagpu_pal_live();
    static unsigned char lut[SH_ROWS * 256];
    int r, i, c;
    if (!pal) return;

    /* Prefer the ENGINE's own 32x256 shade table (PALETTE.SHD, built at init —
       the table its Gouraud rasteriser 0x459C70 uses; shadows-cloak.md /
       build-state.md). It arrives in the frame packet, copied by the game
       thread (landing 3): this file used to dereference the graphics globals
       for it on the render thread behind an IsBadReadPtr, and a probe is not a
       lifetime argument. Calibrate rather than assume: neutral = the row with
       the most identity entries, direction from end-row luminance. Fall back
       to our computed LUT when the packet carried none. */
    {
        if (shd) {
            int bestr = -1, bestn = -1;
            for (r = 0; r < SH_ROWS; r++) {
                int n = 0;
                for (i = 0; i < 256; i++) n += (shd[r*256 + i] == i);
                if (n > bestn) { bestn = n; bestr = r; }
            }
            long lum0 = 0, lum31 = 0;
            for (i = 0; i < 256; i++) {
                const unsigned char* c0 = pal + (size_t)shd[0*256 + i]  * 4;
                const unsigned char* c1 = pal + (size_t)shd[31*256 + i] * 4;
                lum0  += c0[0] + c0[1] + c0[2];
                lum31 += c1[0] + c1[1] + c1[2];
            }
            s_shNeutral = bestr;
            s_shDir     = (lum31 >= lum0) ? 1 : -1;
            shade_upload(shd);
            s_lutFromShd = 1;
            { char b[96]; _snprintf(b, sizeof b,
                "r3d shade: engine SHD table (neutral=%d id=%d/256 dir=%d)",
                s_shNeutral, bestn, s_shDir); rlog(b); }
            return;
        }
    }
    for (r = 0; r < SH_ROWS; r++) {
        float f = 0.60f + 0.025f * r;
        for (i = 0; i < 256; i++) {
            if (r == SH_NEUTRAL || i <= 1 || i == 255) {
                lut[r * 256 + i] = (unsigned char)i;
                continue;
            }
            int tr = (int)(pal[i*4+0] * f + 0.5f); if (tr > 255) tr = 255;
            int tg = (int)(pal[i*4+1] * f + 0.5f); if (tg > 255) tg = 255;
            int tb = (int)(pal[i*4+2] * f + 0.5f); if (tb > 255) tb = 255;
            int best = i, bestd = 0x7FFFFFFF;
            for (c = 2; c <= 254; c++) {
                int dr = pal[c*4+0] - tr, dg = pal[c*4+1] - tg, db = pal[c*4+2] - tb;
                int d = dr*dr + dg*dg + db*db;
                if (d < bestd) { bestd = d; best = c; }
            }
            lut[r * 256 + i] = (unsigned char)best;
        }
    }
    s_shNeutral = SH_NEUTRAL; s_shDir = 1;
    s_lutFromShd = 0;
    shade_upload(lut);
    /* only reachable with shd == NULL: the SHD branch above returns */
    rlog("r3d shade: computed palette LUT (32 rows, row 16 identity) — no shade table in "
         "the packet yet; it is rebuilt from the engine's own the first frame one arrives");
}

/* Model-space toward-camera axis: depth is 2y-z (larger = nearer), so the
   nearness gradient (0,2,-1)/sqrt5 points at the viewer. Faces that survive
   the depth test face the camera, so flipping each normal into the +V
   hemisphere yields the OUTWARD normal without trusting 3DO winding. */
/* SH_V = (0, 0.8944, -0.4472) */
/* sun: high, from screen upper-left, slightly toward camera:
   SH_L = (-0.35, 0.80, -0.49). Both vectors are kept here as the CALIBRATION
   OF RECORD — tagpu_native.c, tagpu_hires_draw.c and tagpu_classicpp.c all
   cite this file by name for them — and no longer as live constants: the
   write-back that shaded with them is gone and each pass carries its own copy
   of the numbers in the form its shader wants. */

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

/* The whole build-state decision for one unit, in the engine's own terms, so
   the two renderers that stage the scaffold (this one into a composite plane,
   the native pass into the GL frame) cannot drift apart on the formulas.

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

/* The two GL objects the surviving passes share: the unit texture atlas and
   the shade LUT. Everything else this function used to build — the program,
   the FBO, its three attachments and the vertex array — belonged to the
   write-back and went with it (landing 3). */
static void r3d_init(void)
{
    /* 8bpp index atlas (R8, NEAREST both ways: sampled texel == palette
       index), created now so the unit program never samples texture 0 */
    tagpu_gaf_atlas_lost(&s_atlas);
    s_atlas.dim = ATLAS_DIM; s_atlas.max = ATLAS_MAX;
    s_atlas.ents = s_atlasEnts; s_atlas.tag = "unit";
    s_atlas.pad = ATLAS_PAD; s_atlas.align = ATLAS_PAD; s_atlas.mip = ATLAS_MIP;
    s_atlas.prio = 3;                 /* restored after terrain, features, effects */
    if (!tagpu_gaf_atlas_create(&s_atlas)) { rlog("render3do: atlas texture FAILED"); s_state = 2; return; }

    /* shade LUT texture (built lazily from the packet's table on first use).
       THE TEXTURE IS GL; EVERYTHING ELSE IN THIS FUNCTION IS THE PASS -- the
       atlas above is laid out on either lane (its existence is `made`, not a GL
       name, since tagpu_gaf.h's change) and `s_state = 1` means "the atlas and
       the shade LUT are ready", which is what `tagpu_r3d_ensure` answers for
       the unit pass. Seventh instance in this landing of GL object creation
       entangled with CPU setup a pass needs. [The vulkan-only plan, 4b-2.] */
    /* the LUT texture's creation stood here and went with its only consumer
       [landing 11-3]; the CPU half below is what `tagpu_r3d_ensure` answers. */
    s_state = 1;
    /* THE MIRROR IS ASKED FOR HERE, BEFORE THE FIRST PAINT, and that ordering is
       the whole of it. It used to be asked for in `pd_view_publish`, which is
       right on the GL lane: the atlas fills during the BAKE, the mirror is
       allocated after it, and the frames already painted are marked to
       re-decode "on their next use" -- which comes, because the GL lane keeps
       drawing and re-asking. On the vulkan-only lane the bake is cached and
       nothing asks the atlas again, so that next use never arrives and the
       twin stands down on a mirror that never converges. Measured: 25 s of
       settled play with the census still reading `unit=0`.
       Asked at arm time instead, every paint from the first one lands in the
       mirror and nothing needs re-decoding at all. `s_atlas.dim` is set a few
       lines above, which is the precondition `_want` tests, and the call is
       idempotent inside tagpu_gaf.c. The GL lane is unchanged in kind -- it
       allocates the same mirror under the same `tagpu_vk_armed()` condition,
       only sooner, which is strictly more of what the mirror is for.
       [The vulkan-only plan, landing 4b-2.] */
    /* ASKED OF THE CONSUMER, NOT OF THE LEVER. [FROM THE 4d-1 LANDING REVIEW.]
    This used to test `tagpu_vk_armed()`, which is true whenever `tagpu_vk.on`
    exists -- and these latches are one-way, so once asked the memory is held
    for the process's life. Until 4d-1 that was right: `tagpu_vk.on` under
    `renderer=openglcore` brought up route D, which consumed the mirror. Route
    D is gone, so on that path the lever now arms nothing and the mirror would
    be paid for with no consumer at all. `tagpu_vk_owns_present()` is exactly
    "a Vulkan pass will run in this process", which is the question. */
    if (tagpu_vk_owns_present()) tagpu_r3d_atlas_mirror_want();
    rlog("render3do: ready (unit atlas + shade LUT; the write-back path is gone)");
}

/* Resolve a face's flat colour to a TA palette index. pColorTable is the
   on-disk PaletteEntry "resolved to a palette/color-table pointer" (tamem.h);
   the r3d_diag dump below tells us the real shape — until then: a small value
   IS the index, a valid pointer's first byte is our best candidate, else a
   neutral grey. Textured faces get the same treatment for now (flat-shaded
   milestone; GAF sampling is the next step). */
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
   skipped by the engine's rasteriser too — painting it was the green-slab bug. */
static int face_colour(const char* fa)
{
    /* pColorTable is NOT always a live pointer: live armcom faces carry the
       deterministic non-pointer value 0x01E11F00 there, which passes a naive
       range check and faults on deref (crashed the first Phase B run). Only
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

/* THE WRITE-BACK IS GONE (frame packet exchange, landing 3). `tagpu_render3do`
   rendered one unit's ENGINE-POSED vertex buffers into an FBO, read the pixels
   back and wrote them into the GAFFrame colour plane at Object3do+0x10 — the
   Phase B proof that our geometry could reach the screen at all. Two things
   retired it. The native pass has drawn units straight into the frame since
   Phase C, so nothing has used this since; and it walked the Object3do's
   PrimitiveStructs on the RENDER thread and then STORED into engine memory
   from there, which is the one shape the exchange exists to remove. Landing 2
   could claim "no render-thread store into engine memory remains" only because
   this path was off by default (`tagpu_writeback.on`), and an opt-in exception
   is still an exception.

   What the file keeps is what the native and posed passes share: the unit
   texture atlas, the shade LUT and its calibration, the face material helpers,
   and the build-state staging formulas. Its FBO, program, vertex scratch and
   the four readback planes went with the write-back, so `r3d_init` now builds
   the atlas and the LUT and nothing else. */


/* ---- exports for the native pass (G12b, tagpu_native.c): share the atlas,
   shade LUT and calibration so both paths draw identical materials ---- */
GLuint tagpu_r3d_atlas_rgbref(void) { return s_atlas.rgb; }
unsigned tagpu_r3d_atlas_gen(void)  { return s_atlas.gen; }
/* ---- THE LEVEL BOUNDARY (G19f-7) ---------------------------------------
   THIS ATLAS KEYS ON AN ADDRESS AND THE ADDRESSES ARE RECYCLED.
   `tagpu_gaf_atlas_find(a, g, pix, w, h)` matches on the frame header's
   address and the pixel plane's, so an entry is only right for as long as
   that address means that art. The engine's per-level teardown frees the
   model textures and the next level's loader is free to hand a new frame the
   address an old one had -- at which point this atlas serves the PREVIOUS
   level's texels for it, and nothing anywhere would detect it: the entry is
   valid, the UV is in range, the picture is simply wrong.

   `tagpu_fx.c` and `tagpu_feat.c` both already drop theirs at this boundary
   for exactly this reason; the unit atlas was the one that did not, and reset
   only when FULL or on a GL context loss. Neither is a level boundary.

   It is called from `tagpu_native_frame` beside `cache_gen_check` and
   `tagpu_posebake_frame`, the other two level-keyed caches, rather than from
   `tagpu_r3d_atlas_frame` lower down this file: `tagpu_posebake_frame` LATCHES
   `tagpu_r3d_atlas_gen()` for the frame, so a drop after it would leave this
   frame's bakes stamped with the generation before the drop and cost a second,
   pointless drop on the next frame. Taking it here means every consumer sees
   one generation for the whole frame.

   The cost of being right is one re-decode of whatever is on screen at a level
   change -- the same cost `tagpu_fx.c` accepted -- and the first frame of a
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
    /* AND THE PALETTE ARGUMENT WENT WITH THE RESTORE IT FED. This took
       `const unsigned char* pal` and handed it to `tagpu_gaf_atlas_restore`,
       which 11-5e-2 deleted with the GL restorer; the recycle above is all
       that is left and it reads no palette. This atlas's restore is the other
       lane's: `tagpu_gaf_atlas_restore_vk` publishes the frame list and
       `tagpu_vk_restore.c` paints it, each reading the palette for itself. */
}
/* `shd` is the packet's copy of PALETTE.SHD, or NULL: the LUT is built once
   per GL context out of whichever the caller has. */
/* BUILD THE LUT, WITHOUT ANYONE ASKING FOR ITS GL NAME. Until this split the
   construction was a side effect of `_texref`, which only the GL composite
   calls -- so on the vulkan-only lane `s_lutBuilt` stayed 0, `s_lutMirror` was
   empty, and the unit twin stood down on a mirror that nothing was ever going
   to fill. Measured: `mirrors atlas=1 dim=2048 lut=0 pal=1 fogLut=1`.
   ELEVENTH INSTANCE in this landing of GL and the pass being entangled -- here
   not a handle used as a test, but a CONSTRUCTION reachable only through one.
   [The vulkan-only plan, landing 4b-2.] */
void tagpu_r3d_lut_want(const unsigned char* shd)
{
    /* build once — and REBUILD the first time the engine's own table arrives
       after a frame that had none */
    if (s_state == 1 && (!s_lutBuilt || (shd && !s_lutFromShd))) shade_build_lut(shd);
}

/* `tagpu_r3d_lut_texref` stood here: it returned the GL shade-LUT texture to
   whoever was about to bind it. Its consumers were the GL unit and terrain
   shaders, so it went with them [landing 11-3]. `tagpu_r3d_lut_want` -- the
   CPU half it wrapped -- is still called every frame from tagpu_native.c and
   is what keeps `s_lutMirror` current for the Vulkan twin. */
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

/* THE RESTORED TWIN'S MIRROR (the Vulkan-only plan's gate 3), asked for
   SEPARATELY from the indexed one and behind `tagpu_classicpp_assets()`. It is
   a second 16 MB at this atlas's 2048 square and it is worth anything only
   while the restorer is what the GL twin samples, so a session that never turns
   Classic++ on never pays for it. Latched on SUCCESS only, so a request made
   before the atlas has its dimensions -- the first frames of a session -- is
   retried on the next publish rather than remembered as a failure.
   `tagpu_gaf_atlas_mirror_rgb` is itself idempotent and latches its own
   failures, so this flag is only about not re-asking on every frame. */
static int s_mirrorRgbAsked;
/* ...OR THE LIST, WHICH IS THE OTHER ANSWER TO THE SAME QUESTION and is asked
   FIRST (the Vulkan-only plan's landing 7e-2, the shape 7d gave features and
   effects). With `tagpu_restorevk.on` beside TotalA.exe the other lane restores
   for itself, so there is nothing to read back: the list is armed, the 16 MB
   read-back is never asked for, and if it was already armed on an earlier beat
   then arming the list frees it. Polled on every beat until it takes, exactly
   as the mirror is, because the lever may appear mid-session. */
static int s_rlistAsked;

void tagpu_r3d_atlas_mirror_rgb_want(void)
{
    if (s_state != 1 || !s_atlas.mirror || s_atlas.dim <= 0 ||
        !tagpu_classicpp_assets())
        return;
    if (!s_rlistAsked) s_rlistAsked = tagpu_gaf_atlas_restore_vk(&s_atlas);
    if (!s_rlistAsked && !s_mirrorRgbAsked)
        s_mirrorRgbAsked = tagpu_gaf_atlas_mirror_rgb(&s_atlas);
}

/* ONE READ-BACK STEP PER PUBLISHED FRAME. It is a glReadPixels off an FBO, so
   it belongs where the context is current and the caller is the render thread;
   it is a no-op until the mirror is armed and again once the restorer has
   stopped painting, so a settled scene pays one integer compare. */
void tagpu_r3d_atlas_mirror_rgb_step(void)
{
    /* the list's arm frees the mirror, so the step below would find nothing to
       do -- but it is gated here as well rather than left to that, because the
       arm is a poll that can land on any frame and "mutually exclusive by
       construction" was already wrong once on this plan for that reason */
    if (s_mirrorRgbAsked && !s_rlistAsked) tagpu_gaf_atlas_mirror_rgb_step(&s_atlas);
}

/* THE ROWS ARE THE READ-BACK'S, NOT THE SHELF'S, and that is the difference
   from the indexed accessor above. The indexed mirror is written by the same
   `atlas_paint` that writes GL, so the shelf cursor bounds what is in it; this
   one is filled by a read-back that runs at its own cadence, so what is in the
   buffer is what the last step covered. Reporting the shelf here would hand a
   consumer rows nothing had read -- and reporting rows a consumer's serial
   cannot distinguish is the fault the gate-2 review found on the other side of
   exactly this hand-over. */
const unsigned char* tagpu_r3d_atlas_mirror_rgb(int* dim, int* rows, int* mips,
                                                float* aniso, unsigned* serial)
{
    if (dim) *dim = 0;
    if (rows) *rows = 0;
    if (mips) *mips = 0;
    if (aniso) *aniso = 0.0f;
    if (serial) *serial = 0;
    if (!s_atlas.mirrorRgb || s_atlas.mirrorRgbRows <= 0 || s_atlas.dim <= 0)
        return NULL;
    /* A CHAIN THAT DID NOT COME BACK WHOLE IS NO MIRROR AT ALL, and the first
       draft of this accessor got that exactly backwards. It handed the levels
       that were actually read and told the consumer to "build the shallower
       image rather than one with an undefined level in it" -- which is a third
       option nobody has: GL still filters this twin to its own MAX_LEVEL, so a
       shallower Vulkan chain is A DIFFERENT PICTURE wherever a unit is
       minified, which is ordinary play, and it is the very thing the aniso
       check three lines from the consumer's refusal stands down for.
       AND THE DEPTH IS ALSO WHAT SIZES THE IMAGE. Reporting a depth that moves
       makes `atlas_rgb_build` rebuild, which is `kill_image` on an image the
       other slots' submitted command buffers still name, with no fence between
       -- gate 2's confirmed use-after-free, one atlas over. Refusing here is
       what makes that rebuild UNREACHABLE rather than rare: `mip` is this
       atlas's compile-time depth, so every publish carrying rows carries the
       same `*mips`, and the consumer's own dimension-and-depth test becomes
       the assertion it is written as. The consumer already draws nothing on a
       frame with no mirror, which is the right answer to a chain we cannot
       reproduce. [The gate-3a re-review's finding 4.]
       `mip` IS NOT LITERALLY IMMUTABLE, and the first draft of this argument
       said it was. `r3d_init` reassigns it on every GL context reset and
       `tagpu_gaf_atlas_restore` demotes it to 0 on a GL with no
       glGenerateMipmap. The argument survives on ORDERING rather than on the
       constant: both writes land strictly before any publish carrying rows,
       because the restore runs at the top of the frame and the twin cannot
       exist before the demote. Naming the real invariant matters -- the
       verification pass found the false one, and the same pair is what
       `tagpu_gaf.c` now bounds its mirror writes against. */
    if (s_atlas.mirrorRgbMips != s_atlas.mip)
        return NULL;                 /* every out-param was zeroed on entry */
    if (dim) *dim = s_atlas.dim;
    if (rows) *rows = s_atlas.mirrorRgbRows;
    if (mips) *mips = s_atlas.mirrorRgbMips;
    if (aniso) *aniso = s_atlas.rgbAniso;
    if (serial) *serial = s_atlas.mirrorRgbSerial;
    return s_atlas.mirrorRgb;
}

/* THE LIST, AND THE TWIN'S SHAPE WITH IT. On this path there is no read-back
   to carry the shape, so `dim` and `mips` come from the atlas -- and so does
   `aniso`, which the mirror accessor also reports but which was never the
   mirror's fact: it is the ratio GL applied to the TWIN, and a consumer that
   cannot apply the same one draws different art wherever a unit is minified at
   an angle. NULL until the list has been armed AND has entries; a consumer that
   gets NULL falls back to whatever it did before, which is the indexed atlas. */
const TAGPU_RGLSL_FRAME* tagpu_r3d_atlas_restore_list(int* dim, int* n, unsigned* gen,
                                                      int* repaint, unsigned* blanks,
                                                      int* mips, float* aniso)
{
    if (dim) *dim = 0;
    if (n) *n = 0;
    if (gen) *gen = 0;
    if (repaint) *repaint = 0;
    if (blanks) *blanks = 0;
    if (mips) *mips = 0;
    if (aniso) *aniso = 0.0f;
    if (!s_atlas.rlistWant || !s_atlas.rlist || s_atlas.dim <= 0) return NULL;
    if (dim) *dim = s_atlas.dim;
    if (n) *n = s_atlas.rlistN;
    if (gen) *gen = s_atlas.rlistGen;
    if (repaint) *repaint = s_atlas.rlistRepaint;
    if (blanks) *blanks = s_atlas.rlistBlanks;
    if (mips) *mips = s_atlas.mip;
    if (aniso) *aniso = s_atlas.rgbAniso;
    return s_atlas.rlist;
}

const unsigned char* tagpu_r3d_lut_mirror(int* w, int* h, unsigned* serial)
{
    if (w) *w = 256;
    if (h) *h = SH_ROWS;
    if (serial) *serial = s_lutSerial;
    return s_lutBuilt ? s_lutMirror : NULL;
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
int tagpu_r3d_ensure(void)             /* init on demand (GL context current) */
{
    if (s_state == 0) r3d_init();
    return s_state == 1;
}
const char* tagpu_r3d_face_texframe(const char* fa, int owner) { return face_texframe(fa, owner); }
int tagpu_r3d_face_colour(const char* fa) { return face_colour(fa); }

/* NOTHING CALLS THIS SINCE 11-5e-1 -- see tagpu_native.c's `*_glreset` banner
   for the whole cascade and why it is left standing. */
void tagpu_r3d_glreset(void)
{
    /* fresh GL context: the new atlas/LUT textures are EMPTY — the CPU-side
       caches must forget what was uploaded or everything samples black. The
       atlas's twin died with the context too. THERE IS NOTHING LEFT TO ORDER
       THIS AGAINST: this comment named `tagpu_rglsl_glreset`, which
       tagpu_native_glreset ran first so the restorer forgot its job before
       the atlas forgot the twin -- 11-5e-1 deleted the watch that drove the
       cascade and 11-5e-2 deleted the restorer itself, so the ordering
       constraint is gone rather than merely unenforced. */
    s_state = 0;
    s_lutBuilt = 0;
    s_lutFromShd = 0;
    tagpu_gaf_atlas_lost(&s_atlas);
}
