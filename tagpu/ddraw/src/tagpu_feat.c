/* tagpu_feat.c — the feature pass: trees, rocks, metal patches, splats and
   GAF wreckage, rendered natively with real depth.

   The engine keeps one 0xD-byte record per 16-px map tile (FeatureMap,
   *(main+0x14287)) and one 0x100-byte definition per feature type
   (*(main+0x1426F)). A tile whose FeatureDefIndex (+8) is below 0xFFFB is the
   ANCHOR of a feature; the other tiles of its footprint hold 0xFFFE and are
   never drawn. DrawGameScreen walks the sweep rect twice
   (research/notes/terrain-depth.md §3, decompiled):

     flat pre-pass   every tile, before any unit: clears tile->flags bit2,
                     and for def Height (+0xFA) < 10 draws at once; taller
                     defs get flags |= 4 and are deferred
     row sweep       per 16-px row, after that row's units, every deferred
                     tile left to right

   and both call the one leaf

     0x46A610(ctx, tile, tileX, tileY)   stdcall, ret 0x10

   whose three bodies are reproduced here. Its projection (the +128/+32 are
   baked immediates, not a viewport read):

     sx = (tileX + 8)*16 + FootprintX*16/2 − eyeX
     sy = (tileY + 2)*16 + FootprintZ*16/2 − eyeY
          − (h00 + h01 + h10 + h11) >> 3        (2x2 anchor-corner heights)

   Bodies:
     1  tile flags bit0 set, def mask (+0xFE) bit0 CLEAR — 3D wreckage: the
        engine copies the wreck record into the scratch feature-unit
        (*(main+0x1420F)) and calls DrawUnit. Counted here, drawn by the
        native pass's wreck gather (native.on="… wrecks").
     2  flags bit0 set, mask bit0 set — animated GAF wreckage: shadow from
        the anim state at rec+0x10 (only when rec+0x2F bit2 and shadows are
        on), then the body from rec+4, both plain colour-keyed.
     3  otherwise a normal feature: shadow sequence *(def+0xB0) then body
        *(def+0xAC), frame 0 when static, or the anim states at def+0xD8 /
        def+0xCC when the def animates (mask bit1). Alpha blit instead of a
        plain copy per mask bit2 (body) / bit3 (shadow); the shadow is drawn
        only when the FShadow option (main+0x37F06 bit4) is set.

   The LOS gate (0x4658E0, two projected footprint corners) applies only to
   defs with +0xFF bit3, and only when the tile's "seen" nibble (flags >> 3)
   is not the local player's id — exactly the engine's condition.

   Everything above is READ-ONLY: bodies 2 and 3 of the leaf touch no engine
   state at all, and body 1's writes are into the draw-side scratch unit, so
   owning the leaf (tagpu_featown.c) changes nothing the sim reads. The
   animation frames are advanced by the tick, not by the draw
   (GAFGetCurrentFramePtrAddr is a pure read — see tagpu_gaf.c), so features
   keep animating while we own the draw.

   Depth: the whole point of this pass. Bodies write depth at the row key the
   painter's sweep implies — tall features at 3 + rel*4 (above the same row's
   units at 1 + rel*4, below the next row's), flat ones just under the
   particle layers the engine draws around the pre-pass — so units are
   occluded by trees through the depth buffer. Shadows draw at a slightly
   lower key without depth writes: they are ground decals and must not
   occlude anything. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "tagpu_opt.h"
#include "tagpu_feat.h"
#include "tagpu_pal.h"
#include "tagpu_glsl.h"
#include "tagpu_featown.h"
#include "tagpu_gaf.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"
#include "tagpu_native.h"
#include "tagpu_packet.h"   /* the frame packet: the view, the tables */
#include "tagpu_vk.h"       /* tagpu_vk_armed(): whether to pay for the mirror */

/* ---- engine layout (terrain-depth.md appendix, byte-confirmed) ----
   THE FEATURE GRID IS NOT READ IN THIS FILE (frame packet exchange).
   Its cells are per-TICK sim state — the def index, the flags nibble and the
   wreck index all change as features are built, burned and scarred — so the
   game thread copies the anchors of the widest zoom rect into the packet, with
   the six height bytes the two height rules need. What stays is the FeatureDef
   record and the wreck record: per-MAP allocations the teardown cascade frees
   (0x483DD0 -> 0x422170), so their lifetime is tagpu_reclaim's fence, the same
   standing tagpu_terr.c has for the tile set. THEIR BASES ARE READ LIVE, not
   taken from the packet: the cascade frees each array and then NULLS its slot
   (0x4221F8 then 0x422214; 0x42227D then 0x42228B), and that null is the only
   invalidation this pass has — a base copied into a packet and held for a
   frame reads straight past it. */
#define TA_MAINPP    0x00511DE8u
#define OFF_FEATDEF  0x1426F   /* FeatureDef array, stride 0x100               */
#define OFF_FEATCOUNT 0x14253  /* i32 NumFeatureDefs: read LIVE beside the base  */
#define OFF_WRECKS   0x1420B   /* wreck records, stride 0x30                   */
#define FD_STRIDE    0x100
#define FD_NAME      0x00      /* char Name[0x20] — INLINE, not a pointer      */
#define FD_FOOTX     0x94      /* i16 footprint in 16-px tiles                */
#define FD_FOOTZ     0x96
#define FD_BODYSEQ   0xAC      /* GAF sequence: static body                   */
#define FD_SHADSEQ   0xB0      /* GAF sequence: static shadow                 */
#define FD_BODYANIM  0xCC      /* anim state: animating body                  */
#define FD_SHADANIM  0xD8      /* anim state: animating shadow                */
#define FD_HEIGHT    0xFA      /* u8; >= 10 = tall (defers to the row sweep)  */
#define FD_MASK      0xFE      /* u8; b0 GAF wreck, b1 animates, b2 body alpha,*/
                               /*     b3 shadow alpha                          */
#define FD_MASKHI    0xFF      /* u8; bit3 = LOS-gated                         */
#define WR_STRIDE    0x30
#define WR_COUNT     2048      /* the pool 0x421F29 allocates: 0x18000/0x30   */
#define WR_BODYANIM  0x04      /* anim state: GAF wreck body                  */
#define WR_SHADANIM  0x10      /* anim state: GAF wreck shadow                */
#define WR_FLAGS     0x2F      /* u8; bit2 = this wreck casts a shadow        */

#define MODE_OPAQUE TAGPU_FXMODE_OPAQUE
#define MODE_ALPHA  TAGPU_FXMODE_ALPHA

/* THE BUCKETS GROW; THESE ARE ONLY WHERE THEY START. One anchor is one quad
   (more with sub-frames), and how many anchors are on screen is the map's
   density times the zoomed-out view — neither of which is a constant. As
   fixed caps they fail: a 3840x2160 view at the 0.25x zoom floor over Town
   & Country offers 6206 anchors against a body bucket of 5461 and a shadow
   bucket of 2730 (MEASURED 2026-09-09 from a live session,
   `feat: ... DROPPED(full=3872)`), and because the gather walks rows from the
   top the drop is the BOTTOM of the screen losing its trees and wrecks.
   They are start sizes (sized so ordinary play never reallocs) and
   feat_room() grows past them. */
#define BV_BODY_0    32768     /* vertices per bucket to start (6 = one quad) */
#define BV_SHAD_0    16384
/* THE CEILING IS MEMORY, NOT A VIEW. Per bucket: 16 MB is 400k vertices, 66k
   quads — an order of magnitude past the anchors any stock map has in total,
   let alone on screen — so it is the point past which something is wrong, not
   a budget the view is expected to live inside. Hitting it still logs
   DROPPED(full=), which is then a real report and not a resolution. */
#define BV_MAX_BYTES (16u * 1024u * 1024u)
/* THE VERTEX LAYOUT IS THE HEADER'S, so that the Vulkan edition of this pass
   builds its vertex input from the same table the VAO below is built from. */
#define FVST         TAGPU_FEAT_VST
#define ATLAS_DIM    2048
#define ATLAS_MAX    4096      /* a map's feature frames: a body and a shadow */
                               /* per def, plus every frame of the animating  */
                               /* ones — a 200v200 battle scars the ground    */
                               /* with enough smudge/scar defs to pass 1024   */
#define MAXFOOT      16        /* junk-def guard: footprints beyond this are  */
                               /* garbage (terrain-depth "Corrections")       */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* bounded append (MSVCRT's _vsnprintf returns -1 on truncation) */
static void sappend(char* b, int cap, int* p, const char* fmt, ...)
{
    va_list ap; int n;
    if (*p >= cap - 1) return;
    va_start(ap, fmt);
    n = _vsnprintf(b + *p, (size_t)(cap - 1 - *p), fmt, ap);
    va_end(ap);
    if (n < 0 || n > cap - 1 - *p) *p = cap - 1; else *p += n;
    b[*p] = 0;
}

/* ---- arming ---- */
static int s_armed = -1;
static int s_log = 0, s_passive = 0;
static int s_flat = 1, s_tall = 1, s_shadow = 1, s_wreck = 1;
static unsigned s_armCheck = 0;

/* The GAF atlas this pass fills once per map. Declared here because the arm
   poll below asks it for a CPU mirror. */
static TAGPU_GAFENT   s_atlasEnts[ATLAS_MAX];
static TAGPU_GAFATLAS s_atlas;

/* ---- the hand-over to the Vulkan edition, and the A/B lever ----
   `tagpu_feat.ab` makes this pass draw over a black frame with a cleared depth
   buffer and read it back ONCE; `s_abFrame` travels to the Vulkan lane with the
   geometry rather than being polled twice on two cadences, so both lanes
   capture the same frame. See tagpu_feat.h and tagpu_vk_feat.c. */
#define ABFILE   "tagpu_feat.ab"
static int s_ab, s_abDone, s_abFrame;
static int s_pubHave;                  /* this frame's hand-over is waiting   */
static TAGPU_FEATHAND s_pub;
static int s_mirrorAsked;              /* the atlas mirror has been asked for */
static int s_rlistAsked;               /* ...or the published restore list    */

int tagpu_feat_armed(unsigned frame_counter)
{
    int was;
    char buf[128];
    int n;
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return s_armed > 0;
    s_armCheck = frame_counter;
    was = s_armed;
    s_armed = 0;
    n = tagpu_opt_read("tagpu_feat.on", buf, sizeof buf);
    if (n < 0) {
        tagpu_featown_set_skip(0);
        if (was > 0) flog("feat: disarmed");
        return 0;
    }
    {
        s_log = 0; s_passive = 0;
        s_flat = s_tall = s_shadow = s_wreck = 1;
        if (n > 0) {
            char* p = buf;
            buf[n] = 0;
            while (*p) {
                char* q;
                int last;
                while (*p && *p <= ' ') p++;
                q = p;
                while (*q && *q > ' ') q++;
                last = (*q == 0);
                *q = 0;
                if (!lstrcmpiA(p, "log")) s_log = 1;
                else if (!lstrcmpiA(p, "passive")) s_passive = 1;
                else if (!lstrcmpiA(p, "noflat")) s_flat = 0;
                else if (!lstrcmpiA(p, "notall")) s_tall = 0;
                else if (!lstrcmpiA(p, "noshadow")) s_shadow = 0;
                else if (!lstrcmpiA(p, "nowreck")) s_wreck = 0;
                if (last) break;
                p = q + 1;
            }
        }
    }
    s_armed = 1;
    /* the A/B lever, on the same beat. It re-arms when the file goes away and
       comes back, which is why `touch` on one that is already there does
       nothing. */
    s_ab = GetFileAttributesA(ABFILE) != INVALID_FILE_ATTRIBUTES;
    if (!s_ab) s_abDone = 0;
    /* THE ATLAS MIRROR, on this beat and not per frame --
       tagpu_vk_armed() is two file-attribute queries and a pass that asked
       every frame would make them on every frame of ordinary play, where the
       answer is no and stays no.

       IT IS ASKED FOR HERE, AND THIS RUNS BEFORE THE GATHER, which is what
       makes the hand-over correct on the first frame that has one rather than
       a few frames later: asking marks every painted entry reserved, and
       tagpu_gaf_atlas_get PAINTS a reserved entry before it returns it, so by
       the time a quad carries a UV those texels are in the mirror as well as in
       the texture. Entries nothing draws may stay stale in it; nothing samples
       them, in either lane.
       4 MB, so it is paid for only while the Vulkan lane is armed -- and
       `s_mirrorAsked` is set only on SUCCESS, so a request made before the
       atlas has its dimensions (the first frames of a session) is retried. */
    /* ASKED OF THE CONSUMER, NOT OF THE LEVER. `tagpu_vk_armed()` is true
    whenever `tagpu_vk.on` exists, whether or not anything will consume the
    mirror -- and these latches are one-way, so once asked the memory is held
    for the process's life. `tagpu_vk_owns_present()` is exactly "a Vulkan
    pass will run in this process", which is the question. */
    if (!s_mirrorAsked && s_atlas.dim > 0 && tagpu_vk_owns_present())
        s_mirrorAsked = tagpu_gaf_atlas_mirror(&s_atlas);
    /* AND THE RESTORE LIST. Under Classic++ `assets=1` the other lane
       restores for itself, which is the only way the restored twin reaches it
       at all. Polled
       on every beat until it takes, exactly as the mirror is, because the
       knob is allowed to move mid-session. */
    if (s_mirrorAsked && !s_rlistAsked && tagpu_classicpp_assets())
        s_rlistAsked = tagpu_gaf_atlas_restore_vk(&s_atlas);
    if (s_passive) tagpu_featown_set_skip(0);
    if (was != 1) {
        char b[160];
        _snprintf(b, sizeof b,
            "feat: ARMED (flat=%d tall=%d shadow=%d wreck=%d log=%d passive=%d)",
            s_flat, s_tall, s_shadow, s_wreck, s_log, s_passive);
        flog(b);
    }
    return 1;
}

int tagpu_feat_on(void) { return s_armed > 0; }

/* the map the atlas's entries belong to (see tagpu_feat_gather) */
static const char* s_mapGrid;
static int         s_mapW, s_mapH;

enum { B_SHADOW = 0, B_BODY = 1, NBUCKET = 2 };
/* grown on demand by feat_room(), never shrunk: a zoom-out reallocs once and
   every frame after it costs nothing */
static float* s_verts[NBUCKET];
static int    s_vcap[NBUCKET];         /* vertices each can hold             */
static int    s_nv[NBUCKET];           /* what this frame put in them        */
static const int s_vcap0[NBUCKET] = { BV_SHAD_0, BV_BODY_0 };

/* Room for `need` more vertices in bucket `b`, growing it if there is not.
   Doubling from the start size, so the growth is amortised and a view that
   settles reallocs once. A refusal — the ceiling above, or a failed realloc —
   leaves the bucket intact and the caller counts the drop, which is the only
   way to see one. Render thread, and put_vert
   re-reads s_verts[b] every call, so a realloc between quads is safe: the
   check is made once per quad, before its six writes. */
static int feat_room(int b, int need)
{
    int cap = s_vcap[b];
    int want = s_nv[b] + need;
    float* p;
    const int max = (int)(BV_MAX_BYTES / (FVST * sizeof(float)));
    if (want <= cap) return 1;
    if (cap <= 0) cap = s_vcap0[b];
    while (cap < want && cap < max) cap *= 2;
    if (cap > max) cap = max;
    if (want > cap) return 0;                    /* the ceiling, not a bug   */
    p = (float*)realloc(s_verts[b], (size_t)cap * FVST * sizeof(float));
    if (!p) return 0;                            /* keep what we have        */
    {
        char m[128];
        _snprintf(m, sizeof m, "feat: bucket %d grew %d -> %d verts (%u KB)",
                  b, s_vcap[b], cap,
                  (unsigned)((size_t)cap * FVST * sizeof(float) / 1024));
        flog(m);
    }
    s_verts[b] = p; s_vcap[b] = cap;
    return 1;
}

/* THE SHADER PAIR IS A BUILD INPUT, NOT DEAD GL CODE, and no C in this file
   references it -- `tools/spirv-gen.py` reads both strings
   out of the PREPROCESSED translation unit and generates the SPIR-V that
   `tagpu_vk_feat.c` draws with, so deleting them fails the build with "the
   manifest names tagpu_feat::VS and the source does not have it". The pragma
   below is paired, and its `pop` is PROVED with a planted probe rather than
   read: a `pop` inside a comment is text and not a directive, and silently
   leaves the warning disabled for the rest of the file. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec2 aCM;\n"       /* colour key /255, mode        */
    "layout(location=3) in vec2 aWorld;\n"
    "layout(location=4) in float aLam;\n"   /* Classic++: the ground's lambert */
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"
    "out vec2 vUV; flat out vec2 vCM; out vec2 vWorld; flat out float vLam;\n"
    "void main(){\n"
    "  vec2 p = (aPos.xy - uZoomC) * uZoom + uZoomC;\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - aPos.z/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vUV = aUV; vCM = aCM; vWorld = aWorld; vLam = aLam;\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; flat in vec2 vCM; in vec2 vWorld; flat in float vLam;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAtlas;\n"
    "uniform sampler2D uPal;\n"
    "uniform sampler2D uAtlasRGB;\n"   /* Classic++: the atlas's restored twin */
    "uniform int uRestored;\n"         /* 1 = sample it where its alpha says so */
    "uniform int uLit;\n"              /* 1 = Classic++: lit, RGB fog rule     */
    TAGPU_GLSL_FOG_UNIFORMS
    TAGPU_GLSL_FOG_FN
    "void main(){\n"
    /* features are terrain furniture: the engine draws them under the fog
       overlay, so they stay visible in grey and are merely shade-remapped */
    TAGPU_GLSL_FOG_DISCARD
    "  float idx = texture(uAtlas, vUV).r;\n"
    /* colour-keyed: the key texel is a hole, and discarding keeps it out of
       the depth buffer too — a tree occludes only where it has pixels */
    "  if (abs(idx - vCM.x) < 0.5/255.0) discard;\n"
    "  float a = (int(vCM.y + 0.5) == 2) ? 0.5 : 1.0;\n"
    /* Classic++ (uLit): the twin's colour where the lazy restore has painted
       it (alpha 1 -- tagpu_gaf.h), the palette's for a frame not yet
       restored, times the GROUND's lambert at the anchor (a billboard has no
       normal of its own; the lab's lambertAt -- a tree on a shaded slope sits
       in the shade rather than on top of it), then the grey band as the RGB
       rule (renderers.md 2.6). The hole stays the index test above. */
    "  if (uLit == 1) {\n"
    "    vec4 t = uRestored == 1 ? texture(uAtlasRGB, vUV) : vec4(0.0);\n"
    "    vec3 c = t.a > 0.5 ? t.rgb : texelFetch(uPal, ivec2(int(idx*255.0+0.5), 0), 0).rgb;\n"
    "    c *= vLam;\n"
    TAGPU_GLSL_FOG_GREY_RGB("c")
    "    frag = vec4(c * a, a); return;\n"
    "  }\n"
    "  int pi = int(idx*255.0+0.5);\n"
    TAGPU_GLSL_FOG_SHADE("pi")
    "  vec3 rgb = texelFetch(uPal, ivec2(pi, 0), 0).rgb;\n"
    "  frag = vec4(rgb * a, a);\n"           /* premultiplied, like the FBO */
    "}\n";
#pragma GCC diagnostic pop

/* THE ATLAS IS THE PASS, NOT THE BACKEND, so its layout is set up here rather
   than in a backend bring-up: with `dim` at 0, `tagpu_gaf_atlas_create`
   refuses, `tagpu_gaf_atlas_mirror` is never asked for, every sprite lookup
   returns NULL, and the pass gathers into `atlas=0` and hands over nothing.
   Nothing in here is GL: `atlas_create` gates its own texture and keys the
   atlas's existence on `made` rather than on a GL name (tagpu_gaf.h). */
static void atlas_setup(void)
{
    tagpu_gaf_atlas_lost(&s_atlas);          /* its texture is made on first use */
    s_atlas.dim = ATLAS_DIM; s_atlas.max = ATLAS_MAX;
    s_atlas.ents = s_atlasEnts; s_atlas.tag = "feat";
    s_atlas.prio = 1;                        /* restored after the terrain, before effects */
    /* Every frame in here is a feature standing on the map, so nothing in it
       ever stops being wanted: when it fills, re-lay it tallest-first and
       keep it rather than drop it (tagpu_gaf.h `repack`). Without it the
       atlas hits `full` at 48% occupancy and is rebuilt from nothing on the
       next frame -- and on the frame after that, for as long as the view
       stays wide enough to want more frames than arrival order can pack:
       37,140 rebuilds measured in one 4K session, each of them re-decoding
       ~200 GAF frames and clearing the Classic++ restore queue before it
       could land. */
    s_atlas.repack = 1;
    tagpu_gaf_atlas_create(&s_atlas);   /* never bind texture 0 to uAtlas */
}

/* ---- emission ---- */
static float s_encCur = 0.0f;
static int   s_bucketCur = B_BODY;
static int   s_mute = 0;                /* passive: count, emit nothing        */
static int   s_cBody, s_cShadow, s_cAtlasFail, s_cOverflow;
static int   s_ownable = 0;            /* the wreck pass is up: we may own it  */

static void put_vert(int b, float x, float y, float u, float v, float c, int mode,
                     float wx, float wz, float lam)
{
    float* o = s_verts[b] + (size_t)s_nv[b] * FVST;
    o[0] = x; o[1] = y; o[2] = s_encCur; o[3] = u; o[4] = v;
    o[5] = c; o[6] = (float)mode; o[7] = wx; o[8] = wz; o[9] = lam;
    s_nv[b]++;
}

/* One GAF frame at its hotspot, clipped away when fully off the viewport.
   (wax, waz) is the feature's WORLD anchor — the fog rule is a per-fragment
   lookup in the explored/LOS maps, so every vertex carries the world position
   of its own corner (anchor + its offset from the projected anchor), which
   also makes a tree straddling the fog edge fade across it like the engine's
   per-cell overlay rather than all at once. */
static void emit_frame(const TAGPU_FXVIEW* v, const unsigned char* g, int sx, int sy,
                       int wax, int waz, int mode, float lam, int depth)
{
    int b = s_bucketCur, w, h;
    float x0, y0, x1, y1, c;
    const TAGPU_GAFENT* e;
    if (depth > 4) return;                       /* sub-frame recursion guard */
    g = tagpu_gaf_frame_sane(g);
    if (!g) return;
    {
        int sub = g[TAGPU_GF_SUBN];
        if (sub) {                               /* a sub-frame list, like sprites */
            const unsigned char* const* arr =
                *(const unsigned char* const* const*)(g + TAGPU_GF_PIX);
            int k;
            if (!ptr_ok(arr) || IsBadReadPtr(arr, (SIZE_T)sub * 4)) return;
            for (k = 0; k < sub; k++) {
                const unsigned char* sg = tagpu_gaf_frame_sane(arr[k]);
                int m = mode;
                if (!sg) continue;
                if (mode == MODE_OPAQUE && sg[TAGPU_GF_SUBALP]) m = MODE_ALPHA;
                emit_frame(v, sg, sx, sy, wax, waz, m, lam, depth + 1);
            }
            return;
        }
    }
    if (b == B_BODY) s_cBody++; else s_cShadow++;
    if (s_mute) return;
    w = *(const unsigned short*)(g + TAGPU_GF_W);
    h = *(const unsigned short*)(g + TAGPU_GF_H);
    x0 = (float)(sx - *(const short*)(g + TAGPU_GF_HOTX));
    y0 = (float)(sy - *(const short*)(g + TAGPU_GF_HOTY));
    x1 = x0 + (float)w; y1 = y0 + (float)h;
    if (x1 < (float)v->evpL || x0 > (float)(v->evpL + v->evw) ||
        y1 < (float)v->evpT || y0 > (float)(v->evpT + v->evh)) return;
    if (!feat_room(b, 6)) { s_cOverflow++; return; }
    e = tagpu_gaf_atlas_get(&s_atlas, g);
    if (!e) { s_cAtlasFail++; return; }
    c = (float)e->ck / 255.0f;
    {   /* world position of each corner: the anchor plus the corner's offset
           from the projected anchor (screen px and world px are 1:1 here) */
        float wx0 = (float)wax + (x0 - (float)sx), wx1 = (float)wax + (x1 - (float)sx);
        float wz0 = (float)waz + (y0 - (float)sy), wz1 = (float)waz + (y1 - (float)sy);
        put_vert(b, x0, y0, e->u0, e->v0, c, mode, wx0, wz0, lam);
        put_vert(b, x1, y0, e->u1, e->v0, c, mode, wx1, wz0, lam);
        put_vert(b, x0, y1, e->u0, e->v1, c, mode, wx0, wz1, lam);
        put_vert(b, x1, y0, e->u1, e->v0, c, mode, wx1, wz0, lam);
        put_vert(b, x1, y1, e->u1, e->v1, c, mode, wx1, wz1, lam);
        put_vert(b, x0, y1, e->u0, e->v1, c, mode, wx0, wz1, lam);
    }
}

/* ---- the LOS gate, 0x4658E0: two projected footprint corners. The per-tile
   test is the same one the effects pass runs per projectile, so it is shared
   rather than copied (both are the engine's LosType rule). ---- */
static int feat_visible(const TAGPU_FXVIEW* v, int col, int row, int fx, int fz, int th)
{
    int hh = th >> 1;
    if (tagpu_fx_tile_visible(v, col * 16, row * 16 - hh)) return 1;
    return tagpu_fx_tile_visible(v, col * 16 + fx * 16, row * 16 + fz * 16 - hh);
}

/* ---- per-frame counters ---- */
typedef struct {
    int anchors, flat, tall, gafwreck, wreck3d, losSkip, junk, animated, shadows, outside;
} FEATC;
static FEATC s_c;
static int s_logged;
static int s_cpp;                       /* Classic++ this frame: the colour branch (uLit)        */
static int s_lit;                       /* light= this frame: anchors take the ground's light    */

/* Classic++: the ground's lambert at an anchor -- the lab's lambertAt(col,
   row): central differences of the height over 32 world units at the anchor
   cell, coordinates clamped to the map, normal (-dx, 1, -dz), then the one
   lighting rule (tagpu_classicpp.c). Every corner of every quad the anchor
   emits takes this one value, as the lab's do. */
/* The gradient over the anchor's four neighbours. The publisher clamps each
   at the map edge — (col-1 -> col at column 0, col+1
   -> col at the last column, and the same for rows) — so the six bytes in the
   anchor ARE the six values HAT() produced. */
static float ground_lambert(const TAGPU_PK_ANCHOR* a)
{
    float dx = (float)((int)a->hr - (int)a->hl) / 32.0f;
    float dz = (float)((int)a->hd - (int)a->hu) / 32.0f;
    float inv = 1.0f / sqrtf(dx * dx + 1.0f + dz * dz);
    float n[3];
    n[0] = -dx * inv; n[1] = inv; n[2] = -dz * inv;
    return tagpu_classicpp_ground(n);
}

/* FeatureDef.Name is an inline char[0x20] (tagpu_cat.c reads the same field
   for `tacli features`), so it needs no dereference — only a NUL somewhere */
static const char* def_name(const char* def)
{
    int i;
    for (i = 0; i < 0x20; i++)
        if (def[FD_NAME + i] == 0) return i ? def + FD_NAME : "?";
    return "?";
}

/* one anchor: the three bodies of 0x46A610 */
static void draw_feature(const TAGPU_FXVIEW* v, const TAGPU_PACKET* pk,
                         const TAGPU_PK_ANCHOR* a, const char* def, int flat,
                         float encBody, int shadowsOn)
{
    int fx = *(const short*)(def + FD_FOOTX);
    int fz = *(const short*)(def + FD_FOOTZ);
    int col = a->col, row = a->row;
    unsigned flags = a->flags;
    unsigned mask  = *(const unsigned char*)(def + FD_MASK);
    int h00, h01, h10, h11, sx, sy, wax, waz;
    float lam;
    const unsigned char* g;
    /* junk defs hold wild footprints; they only move our quad, but an
       unclamped one once hung the render thread (terrain-depth Corrections) */
    if (fx < 0 || fx > MAXFOOT) { fx = 1; s_c.junk++; }
    if (fz < 0 || fz > MAXFOOT) { fz = 1; s_c.junk++; }

    h00 = a->h; h01 = a->hr; h10 = a->hd; h11 = a->hrd;

    /* the engine's projection, +128/+32 baked in as (col+8)*16 / (row+2)*16 */
    wax = col * 16 + (fx * 16) / 2;
    waz = row * 16 + (fz * 16) / 2 - ((h00 + h01 + h10 + h11) >> 3);
    sx = wax + 128 - v->eyeX;
    sy = waz + 32 - v->eyeY;
    /* `light=` is applied HERE, by baking 1.0 -- a billboard has no normal, so
       there is no level normal to hand the rule the way the terrain and unit
       shaders do, and no shadow term in it to preserve either */
    lam = s_lit ? ground_lambert(a) : 1.0f;

    if (flags & 1) {                                  /* wreckage on this tile */
        /* LIVE, every anchor: see the header — the null the teardown leaves is
           what refuses this once the cascade has run */
        const char* taNow = *(const char* const*)TA_MAINPP;
        const char* recs = ptr_ok(taNow) ? *(const char* const*)(taNow + OFF_WRECKS) : NULL;
        const char* rec;
        if (!(mask & 1)) {                            /* body 1: 3D wreck      */
            s_c.wreck3d++;                            /* the native wreck pass */
            return;                                   /* draws it as a unit    */
        }
        s_c.gafwreck++;
        if (!s_wreck) return;
        /* THE INDEX IS BOUNDED, AND THERE IS NO PROBE. The tile's `wreck` word
           is engine DATA: unbounded it addresses up to 65535*0x30 ~ 3 MB past
           the pool, and a probe like IsBadReadPtr answers a question about the
           past — never the argument (CLAUDE.md).
           The pool is FIXED: 0x421F20 allocates 0x18000 bytes at stride 0x30
           once per level and threads a free list through all of them, so there
           are WR_COUNT = 2048 records, and 0x4232A0 returns 2048 itself for
           "no record". The engine's own draw at 0x46A6C4 does not bound this
           either, but it only forms the address for a cell it is drawing.
           The LIFETIME argument: the base is read live
           just above, the teardown frees the pool at 0x4221F8 and nulls
           main+0x1420B at 0x422214 inside the cascade tagpu_reclaim fences
           (0x491B60 -> 0x483DD0 -> 0x422170), and that null is the refusal. */
        if (!ptr_ok(recs) || a->wreck >= WR_COUNT) return;
        rec = recs + (size_t)a->wreck * WR_STRIDE;
        if (shadowsOn && s_shadow && (*(const unsigned char*)(rec + WR_FLAGS) & 4)) {
            g = tagpu_gaf_state_frame(rec + WR_SHADANIM);
            if (g) {
                s_bucketCur = B_SHADOW; s_encCur = encBody - 0.3f;
                emit_frame(v, g, sx, sy, wax, waz, MODE_OPAQUE, lam, 0);
                s_c.shadows++;
            }
        }
        g = tagpu_gaf_state_frame(rec + WR_BODYANIM);
        if (g) {
            s_bucketCur = B_BODY; s_encCur = encBody;
            emit_frame(v, g, sx, sy, wax, waz, MODE_OPAQUE, lam, 0);
        }
        return;
    }

    /* body 3: a normal feature — shadow first, then the body */
    {
        const char* shadSeq = *(const char* const*)(def + FD_SHADSEQ);
        const char* bodySeq = *(const char* const*)(def + FD_BODYSEQ);
        int animating = (mask & 2) != 0;
        if (animating) s_c.animated++;
        if (shadSeq && shadowsOn && s_shadow) {
            g = animating ? tagpu_gaf_state_frame(def + FD_SHADANIM)
                          : tagpu_gaf_seq_frame(shadSeq, 0);
            if (g) {
                s_bucketCur = B_SHADOW;
                s_encCur = encBody - (flat ? 0.03f : 0.3f);
                emit_frame(v, g, sx, sy, wax, waz, (mask & 8) ? MODE_ALPHA : MODE_OPAQUE, lam, 0);
                s_c.shadows++;
            }
        }
        /* the engine gates BOTH body paths on the static sequence pointer */
        if (!bodySeq) return;
        g = animating ? tagpu_gaf_state_frame(def + FD_BODYANIM)
                      : tagpu_gaf_seq_frame(bodySeq, 0);
        if (!g) return;
        s_bucketCur = B_BODY; s_encCur = encBody;
        emit_frame(v, g, sx, sy, wax, waz, (mask & 4) ? MODE_ALPHA : MODE_OPAQUE, lam, 0);
        if (s_log && s_logged < 8 &&
            sx >= v->vpL && sx < v->vpL + v->vw && sy >= v->vpT && sy < v->vpT + v->vh) {
            char b[256];
            s_logged++;
            _snprintf(b, sizeof b,
                "feat: def=%u \"%.24s\" h=%u foot=%dx%d mask=%02X/%02X %s frame=%ux%u hot=(%d,%d) tile=(%d,%d) at=(%d,%d) enc=%.2f",
                (unsigned)a->def, def_name(def),
                (unsigned)*(const unsigned char*)(def + FD_HEIGHT), fx, fz,
                mask, *(const unsigned char*)(def + FD_MASKHI),
                flat ? "flat" : "tall",
                (unsigned)*(const unsigned short*)(g + TAGPU_GF_W),
                (unsigned)*(const unsigned short*)(g + TAGPU_GF_H),
                *(const short*)(g + TAGPU_GF_HOTX), *(const short*)(g + TAGPU_GF_HOTY),
                col, row, sx, sy, encBody);
            flog(b);
        }
    }
}

/* Every exit from the gather that draws nothing must also hand the draw back:
   the skip byte is latched from the previous frame, so returning early with it
   set would leave the engine's leaf detoured and the map bare of features until
   the 90-frame watchdog in tagpu_featown_flush notices. */
static int feat_bail(void)
{
    tagpu_featown_set_skip(0);
    return 0;
}

int tagpu_feat_gather(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    const TAGPU_PK_ANCHOR* anch;
    const char* fdefs;
    int mapW, mapH, nCols, nRows, r0, c0;
    int localPl, shadowsOn, nDefs, liveDefs, outside = 0; /* a flag: see the loop below */
    unsigned ai;
    float flatSpan;
    if (s_armed != 1) return feat_bail();
    if (!pk || !pk->in_game) return feat_bail();
    /* THE ATLAS IS WHAT THIS PASS NEEDS BEFORE IT CAN GATHER, and it is the
       only thing it needs: set the layout up once, since `made` latches, and
       refuse only if that fails. */
    if (!s_atlas.made) {
        atlas_setup();
        if (!s_atlas.made) return feat_bail();
    }
    if (s_atlas.full) tagpu_gaf_atlas_reset(&s_atlas);
    /* This atlas's restore is the other lane's: `tagpu_gaf_atlas_restore_vk`
       above publishes the frame list and `tagpu_vk_restore.c` paints it. */

    memset(s_nv, 0, sizeof s_nv);
    memset(&s_c, 0, sizeof s_c);
    s_cBody = s_cShadow = s_cAtlasFail = s_cOverflow = 0;
    s_logged = 0;
    /* `passive` is the explicit A/B lever. The wrecks requirement is not an
       anti-double-draw test and must not be read as one -- the engine's
       features reach the reference surface and no screen (gpu-status 2.81).
       It is a COMPLETENESS test: this pass draws flats, talls and wrecks as
       one depth-sorted set, and `tagpu_native.on=wrecks` is what supplies the
       wreck half, so without it we would emit a feature scene with its 3D
       husks missing. `tagpu_native.on` ships as `all wrecks`, so the branch is
       taken in every shipped configuration; it is a refusal for a hand-built
       arm set, not a gate the defaults depend on. */
    s_ownable = tagpu_native_wrecks_armed();
    s_mute = s_passive || !s_ownable;
    s_cpp = tagpu_classicpp_on();
    s_lit = tagpu_classicpp_lit();

    {   /* LIVE, once per frame: see the header */
        const char* taNow = *(const char* const*)TA_MAINPP;
        fdefs = ptr_ok(taNow) ? *(const char* const*)(taNow + OFF_FEATDEF) : NULL;
        liveDefs = ptr_ok(taNow) ? *(const int*)(taNow + OFF_FEATCOUNT) : 0;
    }
    mapW = pk->map_w16;
    mapH = pk->map_h16;
    nCols = pk->sweep_cols;
    nRows = pk->sweep_rows;
    if (!ptr_ok(fdefs)) return feat_bail();
    if (mapW <= 0 || mapH <= 0 || mapW > 4096 || mapH > 4096) return feat_bail();
    if (nCols <= 0 || nRows <= 0 || nCols > 1024 || nRows > 1024) return feat_bail();

    /* THE MAP CHANGED — tell the atlas its contents are meaningless.
       `repack` makes the atlas keep what it holds, and the wall makes it keep
       that layout for good; both are right for one map and wrong across two.
       The engine allocates `FeatureMap` per map (main+0x14287, 0xD per 16-px
       tile) with its dimensions beside it, so the grid pointer moving, or the
       dimensions moving under it, is the load. The same identity test the
       terrain pass makes on its TILE_SET (tagpu_terr.c `s_setPtr`/`s_setCount`).
       Belt and braces: the repack also evicts every entry nothing has asked
       for since the last one, so even an undetected change (a new map handed
       the same allocation at the same size) cannot accumulate -- the previous
       map's frames go at the first repack that needs the room. Checked here,
       after the pointer is validated and before any atlas_get can run. */
    if (fdefs != s_mapGrid || mapW != s_mapW || mapH != s_mapH) {
        if (s_mapGrid) tagpu_gaf_atlas_forget(&s_atlas);
        s_mapGrid = fdefs; s_mapW = mapW; s_mapH = mapH;
    }

    /* The engine's own sweep rect and its edge clamps (DrawGameScreen), run
       over the ZOOM's viewport rather than the engine's (TAGPU_FXVIEW.evpL):
       screen and world differ by a pure translation, so reaching further is
       "move the eye to the wider rect's top-left and ask for more tiles". The
       quads still land at unzoomed game coordinates — the vertex shader does
       the scaling. At zoom >= 1 the deltas are zero and this IS the engine's
       rect, tile for tile. */
    r0 = ((v->eyeY + (v->evpT - v->vpT)) >> 4) - 16;
    nRows += (v->evh - v->vh) >> 4;
    /* the nCols/nRows <= 1024 sanity check above is on the ENGINE's numbers;
       re-state a ceiling on ours, since we just added to them. The effective
       rect is already trimmed to the terrain budget, and the map clamps below
       bound this further — this is the guard keeping its meaning, not a new
       policy. */
    if (nRows > 4096) nRows = 4096;
    if (r0 < 0) { nRows += r0; r0 = 0; }
    if (r0 + nRows > mapH - 1) nRows = mapH - r0 - 1;
    c0 = ((v->eyeX + (v->evpL - v->vpL)) >> 4) - 10;
    nCols += (v->evw - v->vw) >> 4;
    if (nCols > 4096) nCols = 4096;
    if (c0 < 0) { nCols += c0; c0 = 0; }
    if (c0 + nCols > mapW - 1) nCols = mapW - c0 - 1;
    if (nRows <= 0 || nCols <= 0) return feat_bail();

    /* THE FEATUREDEF BOUND IS THE SMALLER OF THE LIVE COUNT AND THE PACKET'S, and
       which one wins is an ORDERING FACT about the engine rather than a preference.
       The array at `main+0x1426F` is grown ONE RECORD AT A TIME as the map's
       features are read: `0x422543` reallocs it to `(count+1)·0x100`, `0x422558`
       stores the new base, the caller fills the new record, and only then does
       `0x422DAC` write `count + 1`. **The count is incremented last**, so it never
       describes more records than the allocation holds — a live count read after a
       live base is a conservative bound on that base, never an optimistic one.
       The packet's count alone is NOT: it belongs to the packet's level, and across
       a level boundary the teardown zeroes the count (`0x422299`) and nulls the
       base (`0x42228B`) while the new map's array starts at one record and grows,
       so a held packet from a map with 442 defs would authorise 442 records of a
       30-record array. Taking the smaller is safe under both, and costs one load of
       a field in a struct this pass is already dereferencing. */
    nDefs = pk->feat_defcount;
    if (nDefs < 0 || nDefs > 4096) nDefs = 0;         /* no guard we can trust */
    if (liveDefs < 0 || liveDefs > 4096) liveDefs = 0;
    if (!nDefs || (liveDefs && liveDefs < nDefs)) nDefs = liveDefs;
    localPl = pk->local_player;
    shadowsOn = (pk->gfx_opt & 0x10) != 0;
    flatSpan = (float)nRows * (float)nCols;

    /* THE ANCHORS COME OUT OF THE PACKET, in the same row-major order the grid
       walk produced them, over a rect the publisher sized for the WIDEST zoom —
       so this loop's own rect is a sub-rect of it and the filter below selects
       it. `outside=1` says THIS FRAME'S rect asked
       for cells outside the ones the packet carried — it is a flag, not a
       count, because what fell outside cannot be counted from here — and it is
       0 at every reachable zoom. It is the number to look at if features ever
       stop short of the frame edge. */
    anch = tagpu_pk_anchors(pk);
    if (r0 < pk->anch_r0 || c0 < pk->anch_c0 ||
        r0 + nRows > pk->anch_r0 + pk->anch_rows ||
        c0 + nCols > pk->anch_c0 + pk->anch_cols) outside++;
    for (ai = 0; ai < pk->n_anchors; ai++) {
        const TAGPU_PK_ANCHOR* a = &anch[ai];
        int row = a->row, col = a->col;
        {
            unsigned idx = a->def;
            const char* def;
            int flat;
            float enc;
            if (row < r0 || row >= r0 + nRows) continue;
            if (col < c0 || col >= c0 + nCols) continue;
            /* tiles can name defs past the map's real ones, whose 0x100-byte
               records hold garbage — wild footprints and invalid sequence
               pointers (terrain-depth.md "Corrections") */
            /* THE COUNT IS THE BOUND AND THERE IS NO PROBE BEHIND IT. `nDefs`
               is the smaller of the live count and the packet's, and the live
               one never over-describes the live base (see the block above the
               loop), so the address below is inside the allocation by
               construction. The refusal when the count is 0 -- which is what
               the teardown leaves -- is the same line. */
            if (!nDefs || (int)idx >= nDefs) { s_c.junk++; continue; }
            def = fdefs + (size_t)idx * FD_STRIDE;
            s_c.anchors++;
            flat = *(const unsigned char*)(def + FD_HEIGHT) < 10;
            if (flat) s_c.flat++; else s_c.tall++;
            if (flat ? !s_flat : !s_tall) continue;
            /* the engine's gate: only defs flagged +0xFF bit3 are LOS-tested,
               and a tile already seen by the local player skips even that */
            if ((*(const unsigned char*)(def + FD_MASKHI) & 8) &&
                ((a->flags >> 3) & 0xF) != (unsigned)localPl) {
                int fx = *(const short*)(def + FD_FOOTX);
                int fz = *(const short*)(def + FD_FOOTZ);
                if (fx < 0 || fx > MAXFOOT) fx = 1;
                if (fz < 0 || fz > MAXFOOT) fz = 1;
                if (!feat_visible(v, col, row, fx, fz, a->h)) {
                    s_c.losSkip++;
                    continue;
                }
            }
            if (flat) {
                /* the backdrop band: above the particle layers the engine
                   draws before the pre-pass (0..2) and below those after it
                   (3..4); the scan fraction keeps the engine's paint order
                   when two flat features overlap */
                float f = ((float)(row - r0) * (float)nCols + (float)(col - c0)) / flatSpan;
                enc = 0.40f + 0.10f * f;
            } else {
                /* the row key the painter's sweep implies: above this row's
                   units (1 + rel*4), below the next row's; the column
                   fraction keeps left-to-right order inside the row.
                   The engine's edge clamps keep rel inside [0, sweepRows),
                   the same range the unit gather uses; clamping to the band
                   the frame's keys were sized for (rows + the gather's row
                   slack) means no arithmetic here can ever push a feature
                   into the effects band above it. */
                int rel = row - v->r0;
                if (rel < 0) rel = 0;
                if (rel > v->rows + 8) rel = v->rows + 8;
                enc = 3.0f + (float)rel * 4.0f
                      + 1.5f * ((float)(col - c0) / (float)nCols);
            }
            draw_feature(v, pk, a, def, flat, enc, shadowsOn);
        }
    }
    s_c.outside = outside;

    /* we drew this frame: the engine's feature leaf may be skipped. Body 1
       (3D wreckage) goes through DrawUnit, which only the native pass's wreck
       gather replaces — without it, owning the leaf would lose every husk. */
    if (!s_passive && s_ownable) tagpu_featown_set_skip(1);
    else tagpu_featown_set_skip(0);
    tagpu_featown_beat(v->frame_counter);

    {
        static unsigned last = 0;
        if (v->frame_counter - last >= 60) {
            char b[420]; int p = 0;
            last = v->frame_counter;
            sappend(b, sizeof b,
                    &p, "feat: rect=%dx%d anchors=%d flat=%d tall=%d gafwreck=%d 3dwreck=%d",
                    nCols, nRows, s_c.anchors, s_c.flat, s_c.tall, s_c.gafwreck, s_c.wreck3d);
            sappend(b, sizeof b, &p, " defs=%d", nDefs);
            sappend(b, sizeof b, &p, " anim=%d los-skip=%d junk=%d outside=%d -> body=%d shadow=%d atlas=%d",
                    s_c.animated, s_c.losSkip, s_c.junk, s_c.outside, s_cBody, s_cShadow, s_atlas.n);
            /* repacks should settle at a small number and stop; `wall` means
               the map wants more than one 2048 page holds (tagpu_gaf.h) */
            sappend(b, sizeof b, &p, " repack=%u%s", s_atlas.repacks,
                    s_atlas.repackWall ? " WALL" : "");
            if (s_cOverflow || s_cAtlasFail)
                sappend(b, sizeof b, &p, " DROPPED(full=%d atlas-fail=%d)",
                        s_cOverflow, s_cAtlasFail);
            if (s_passive) sappend(b, sizeof b, &p, " (passive: nothing emitted)");
            else if (!s_ownable)
                sappend(b, sizeof b, &p,
                        " (nothing emitted: native.on needs \"wrecks\" before we can own the leaf)");
            flog(b);
        }
    }
    return s_nv[B_SHADOW] + s_nv[B_BODY];
}

/* Everything the draw above was made of, for the Vulkan edition of this pass
   (tagpu_feat.h). Nothing is computed here that the draw did not already use:
   each field is the value that went into a uniform, a pointer into the array
   the upload took, or the buffer a texture was uploaded from. */
/* THE FOG GRID IS COPIED, NOT ALIASED. `v->fogGrid` points INSIDE a frame-packet
   slot, and tagpu_packet.h gives both packet pointers a lifetime that ends at
   tagpu_packet_frame_end() -- which render_ogl.c calls BEFORE it runs the
   Vulkan lane, so a pointer handed on from here is read past its contract. An
   alias would hold only because the give-back happens at the next acquire,
   which is also where the `poison` lever fills the slot: the one stale read
   that lever cannot see, and outside frame_end's tail==head check as well.
   The atlas and the height grid are handed over as buffers this module owns;
   the copy makes the fog grid the same, and makes the file header's "a pass
   reads no engine state" true of the whole hand-over.

   `cells * 2` is the size the GL lane's own glTexImage2D was given for this
   grid, so the read is bounded by the bound the GL upload already trusts; the
   1024-a-side cap bounds the ALLOCATION, and the pass re-checks it (FOG_MAXDIM)
   because a bound in one file is a bound only while both are read together. */
#define FOG_COPY_MAXDIM 1024
static unsigned short* s_fogCopy;
static int             s_fogCopyCells;

static void feat_publish(const TAGPU_FXVIEW* v, int total)
{
    int fogBad = 0;                    /* fog wanted, no grid: publish nothing */
    /* NOTHING IS PUBLISHED ON A SHIPPED FRAME. `s_mirrorAsked` is the latch the
       arm beat sets when the Vulkan lane is up (see there); while it is 0 the
       lane is not armed, nothing will ever call the hand-over, and the memset
       and the forty stores below are pure cost on the path every player runs.
       `s_pubHave` is cleared with it so no earlier frame's hand-over can be
       taken later. */
    if (!s_mirrorAsked) { s_pubHave = 0; s_abFrame = 0; return; }
    memset(&s_pub, 0, sizeof s_pub);
    s_pub.shadow = s_verts[B_SHADOW]; s_pub.nShadow = s_nv[B_SHADOW];
    s_pub.body   = s_verts[B_BODY];   s_pub.nBody   = s_nv[B_BODY];
    s_pub.gw = (float)v->gw; s_pub.gh = (float)v->gh;
    s_pub.zoom = v->zoom > 0.0f ? v->zoom : 1.0f;
    s_pub.zoomCx = v->zoomCx; s_pub.zoomCy = v->zoomCy;
    s_pub.depthScale = v->depthScale > 1.0f ? v->depthScale : 512.0f;
    /* THE ROUTE IS THE PUBLISHED LIST, NOT A GL TEXTURE NAME. `s_atlas.rgb`
       is 0 for the life of the process (tagpu_gaf.h), so keyed on it this
       would publish 0 on every frame and the consumer would stand every
       restored frame down. `rlistWant` is what says a restore route exists;
       it is latched by the arm. */
    s_pub.restored = (s_atlas.rlistWant && tagpu_classicpp_assets()) ? 1 : 0;
    s_pub.lit = s_cpp ? 1 : 0;
    s_pub.fog = v->fogMode & 1;
    s_pub.fogOrgX = (float)v->fogOrgX; s_pub.fogOrgY = (float)v->fogOrgY;
    s_pub.fogCols = (float)v->fogCols; s_pub.fogRows = (float)v->fogRows;
    s_pub.atlas = s_atlas.mirror; s_pub.atlasDim = s_atlas.dim;
    {   /* the shelf cursor bounds every cell in the atlas (tagpu_feat.h) */
        int rows = s_atlas.shelfY + s_atlas.shelfH;
        if (rows < 0) rows = 0;
        if (rows > s_atlas.dim) rows = s_atlas.dim;
        s_pub.atlasRows = rows;
    }
    s_pub.atlasSerial = s_atlas.mirrorSerial;
    /* THE REQUEST, NOT THE PICTURE: the restored twin's texels are never
       read back (tagpu_gaf.h), so the list is the only restore route this
       hand-over carries. */
    if (s_atlas.rlistWant && s_atlas.rlist) {
        s_pub.restoreFrames  = s_atlas.rlist;
        s_pub.restoreN       = s_atlas.rlistN;
        s_pub.restoreGen     = s_atlas.rlistGen;
        s_pub.restoreRepaint = s_atlas.rlistRepaint;
        s_pub.restoreBlanks  = s_atlas.rlistBlanks;
    }
    s_pub.pal = tagpu_pal_live(); s_pub.palSerial = tagpu_pal_serial();
    /* the grid as the fragment shader will read it, and only when it will:
       `uFog` 0 means taFog is never called and uFogGrid never sampled, which
       is why the GL lane can leave its own (possibly stale) texture bound. */
    if (s_pub.fog && v->fogGrid && v->fogCols > 0 && v->fogRows > 0 &&
        v->fogCols <= FOG_COPY_MAXDIM && v->fogRows <= FOG_COPY_MAXDIM) {
        int cells = v->fogCols * v->fogRows;
        if (cells > s_fogCopyCells) {
            unsigned short* n = (unsigned short*)realloc(s_fogCopy, (size_t)cells * 2);
            if (n) { s_fogCopy = n; s_fogCopyCells = cells; }
        }
        if (s_fogCopy && cells <= s_fogCopyCells) {
            memcpy(s_fogCopy, v->fogGrid, (size_t)cells * 2);
            s_pub.fogGrid = s_fogCopy;
            s_pub.fogGridCols = v->fogCols; s_pub.fogGridRows = v->fogRows;
        }
    }
    /* fog on with no grid is not a frame this pass may draw, so it publishes
       NOTHING -- the copy can fail, and the port would sample a 1x1 image while
       uFogDim carried the real size. Refused the way the restored atlas already
       is; tagpu_terr.c has the argument in full. */
    fogBad = (s_pub.fog && !s_pub.fogGrid);
    s_pub.fogLut = tagpu_native_foglut();
    s_pub.vpL = v->vpL; s_pub.vpT = v->vpT; s_pub.vw = v->vw; s_pub.vh = v->vh;
    /* WHETHER THE CLIP IS ACTUALLY ON, not whether a rect exists. The native
       pass enables the scissor only when it resolved glScissor, and a Vulkan
       lane that clipped while the GL lane did not would differ in every feature
       the gather's margin reaches outside the viewport -- which on a forest map
       is a wide band down both edges. */
    s_pub.scissorOn = tagpu_native_scissor_on();
    s_pub.ss = v->ss;
    /* THE A/B FLAG LIVES EXACTLY ONE FRAME. The Vulkan lane takes the hand-over
       later in this same render-thread iteration, so a flag that was not taken
       was not taken because the lane is down -- and a claim left standing would
       pair a fresh Vulkan capture with a GL one from some earlier frame. */
    s_pub.ab = s_abFrame; s_abFrame = 0;
    /* THE STAMP (tagpu_terr.h has the argument in full). `s_pub.shadow`/`body`
       point into `s_verts`, which this file `realloc`s, and `s_pub.atlas` into
       the GAF mirror -- so the hand-over is this frame's or it is nothing. */
    s_pub.frame = v->frame_counter;
    s_pubHave = total > 0 && !fogBad;
}

/* Hand it over, ONCE (tagpu_feat.h). */
int tagpu_feat_handover(TAGPU_FEATHAND* out, unsigned now)
{
    if (!s_pubHave || !out) return 0;
    /* not this frame's, so not alive -- and cleared, so the next frame starts
       honest. See tagpu_terr.h. */
    if (s_pub.frame != now) { s_pubHave = 0; s_abFrame = 0; return 0; }
    *out = s_pub;
    s_pubHave = 0; s_abFrame = 0;
    return 1;
}

void tagpu_feat_render(const TAGPU_FXVIEW* v)
{
    /* THIS PASS DOES NOT DRAW; IT GATHERS AND HANDS OVER to the Vulkan twin,
       which draws. */
    int total = s_nv[B_SHADOW] + s_nv[B_BODY];
    int taking;
    /* A FRAME WITH NOTHING TO DRAW HANDS NOTHING OVER. Leaving the previous
       frame's hand-over standing would have the Vulkan lane draw last frame's
       features over this frame's -- and on the frame a level is torn down, over
       nothing at all. `total`, the gather's own count, is the whole
       refusal. */
    if (total == 0) { s_pubHave = 0; s_abFrame = 0; return; }

    /* Read once so the two arms cannot disagree about which frame is the
       capture frame. */
    taking = s_ab && !s_abDone;
    /* NO `ss` BOUND ON THIS A/B: the Vulkan lane captures its gw*ss by gh*ss
       world target, so this pass is measurable at every `ss`, including the
       shipped ss=2. The refusal is in the lane that can see the target: if
       there is none that frame, tagpu_vk.c says so by name and captures
       nothing (tagpu_vk_world.h). */
    if (taking) {
        /* THE A/B CLAIM: the lever claims the VULKAN capture. `tagpu_vk_ab_arm`
           unlinks the target `_vk.ppm` at the instant the claim latches, which
           is what makes the file on the disk this arming's and not an earlier
           run's. Diff it against a capture from another BUILD. */
        s_abDone = 1;
        s_abFrame = tagpu_vk_ab_arm("feat");
    }

    /* PUBLISHED AFTER THE GATHER: these are the vertices, the numbers and the
       texels this frame built, and the Vulkan lane is about to draw the same
       ones. */
    feat_publish(v, total);
}
