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
#include "tagpu_classicpp.h"
#include "tagpu_native.h"
#include "tagpu_packet.h"   /* the frame packet: the view, the tables */
#include "tagpu_vk.h"       /* tagpu_vk_armed(): whether to pay for the mirror */
#include "tagpu_log.h"

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
#define WR_COUNT     TAGPU_LIM_WRECKS /* the pool 0x421F20 allocates a level   */
#define WR_BODYANIM  0x04      /* anim state: GAF wreck body                  */
#define WR_SHADANIM  0x10      /* anim state: GAF wreck shadow                */
#define WR_FLAGS     0x2F      /* u8; bit2 = this wreck casts a shadow        */

#define MODE_OPAQUE TAGPU_FXMODE_OPAQUE
#define MODE_ALPHA  TAGPU_FXMODE_ALPHA
/* a constant spliced into a shader string, as the preprocessor spells it */
#define FEAT_S(x)   FEAT_S2(x)
#define FEAT_S2(x)  #x

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
    tagpu_log(s);
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
   `tagpu_feat.ab` claims ONE Vulkan capture of this pass; `s_abFrame` travels
   to the Vulkan lane with the geometry rather than being polled on a second
   cadence, so the capture is of the frame the claim was made on. See
   tagpu_feat.h and tagpu_vk_feat.c. */
#define ABFILE   "tagpu_feat.ab"
static int s_ab, s_abDone, s_abFrame;
static int s_pubHave;                  /* this frame's hand-over is waiting   */
static TAGPU_FEATHAND s_pub;
static int s_mirrorAsked;              /* the atlas mirror has been asked for */
/* the map edge's mirror, as the publisher reads it (tagpu_feat.h
   `tagpu_feat_mapfeat_want`/`_holds`): stored here, read on the game thread.
   `s_mfHaveLevel` is written before `s_mfHaveOk` is first raised, and neither
   carries a reserved value, so no level_gen can read as "none". */
static volatile LONG s_mfWant, s_mfHaveOk, s_mfHaveLevel;
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
        s_mfWant = 0;                  /* nothing will take the map's table */
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
       the time a quad carries a UV those texels are in the mirror. Entries
       nothing draws may stay stale in it; nothing samples them.
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
    /* AND THE RESTORE LIST. Under Classic++ `assets=1` the Vulkan restorer
       paints the restored twin from this list, which is the only way the
       restored twin exists at all. Polled
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

/* the map the atlas's entries belong to (see tagpu_feat_gather) */
static const char* s_mapGrid;
static int         s_mapW, s_mapH;

/* the map's own features, then the map edge's mirror of them (B_MSHADOW,
   B_MBODY): drawn in this order, so the mirror's two are the last */
enum { B_SHADOW = 0, B_BODY = 1, B_MSHADOW = 2, B_MBODY = 3, NBUCKET = 4 };
/* grown on demand by feat_room(), never shrunk: a zoom-out reallocs once and
   every frame after it costs nothing */
static float* s_verts[NBUCKET];
static int    s_vcap[NBUCKET];         /* vertices each can hold             */
static int    s_nv[NBUCKET];           /* what this frame put in them        */
static const int s_vcap0[NBUCKET] = { BV_SHAD_0, BV_BODY_0, BV_SHAD_0, BV_BODY_0 };

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

/* THE SHADER PAIR IS A BUILD INPUT, NOT DEAD CODE, and no C in this file
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
    "layout(location=2) in vec2 aCM;\n"       /* colour key /255, mode: the
                                                   fragment stage reads the mode
                                                   and takes the hole from the
                                                   base atlas's alpha, which
                                                   tagpu_gaf.c keys against this
                                                   same key */
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
    "uniform sampler2D uAtlasRGB;\n"   /* Classic++: the atlas's restored twin */
    "uniform int uRestored;\n"         /* 1 = sample it where its alpha says so */
    TAGPU_GLSL_FOG_UNIFORMS
    /* the base atlas: the same texels expanded through the engine's table,
       alpha 0 at the frame's key (tagpu_vk_feat.c) */
    "uniform sampler2D uBase;\n"
    /* declared LAST, so the block offsets every other uniform already has are
       the ones the generated header printed before it */
    TAGPU_GLSL_EDGE_UNIFORMS
    "uniform int uClip;\n"            /* 1 while the mirror draws (below) */
    "uniform float uNudgeW;\n"        /* TAGPU_EDGE_NUDGE / zoom, world px  */
    TAGPU_GLSL_FOG_FN
    TAGPU_GLSL_EDGE_FN
    "void main(){\n"
    "  int m = int(vCM.y + 0.5);\n"
    /* THE FOLDS ARE THE GROUND'S, AND SO IS EVERY SAMPLE ON ONE. The terrain
       puts every fold -- the map's four edges and each copy's own x = q W --
       at the fold less e on screen, e = TAGPU_EDGE_NUDGE / zoom world px (the
       shared corner keeps the map's nudge, tagpu_terr.c), so a sample belongs
       to the side its point moved by e lies on. Every clip below tests that
       point, `s`, never vWorld itself: at the 0.25x floor every sample centre
       is a whole world px, so with the eye's parity a whole column or row of
       samples lies ON a fold, where vWorld alone would sit on the line and
       take whichever side the interpolation's rounding gave it. THE MARGIN IS
       THE INVARIANT: a sample on a fold is e from the line -- 1/8 world px at
       the floor, 128 ulps of a world coordinate under 16 384, and at every
       zoom 8 ss times the rasteriser's snap of the ground's own edge (1/256 of
       a framebuffer px, subPixelPrecisionBits 8, as the reference setup's GPU
       and llvmpipe report). What the margin cannot decide is a sample that lies
       within that snap of the line without lying on a fold, which only a zoom
       whose samples fall off the world's px lattice can produce: the line is
       the ground's, its rounding is not. */
    "  vec2 s = vWorld + vec2(uNudgeW);\n"
    /* THE MAP EDGE'S MIRROR (TAGPU_FEAT_MIRROR): the same texels in the edge's
       tone and nothing else -- no fog of war and no light, as for the mirrored
       ground under it (tagpu_terr.c). EACH COPY OF THE MAP DRAWS ONLY IN ITS
       OWN COLUMNS, as the ground does: a turned copy is its source reflected,
       and a quad that reached across a fold would lay one copy's feature over
       the next copy's reflection of it. And a copy of rows past the top or
       bottom never draws on the map's own rows: there it would cover the map
       -- a mirrored tree just south of the edge stands in FRONT of the last
       rows by the painter's order -- or, in a corner, the side copy that is
       the reflection of that map. vWorld is the sample's own point on the tile
       grid (the gather moves the quad, not the point), and `s` is it moved to
       the ground's side of a fold (above). The hole is the base atlas's, as
       below. */
    "  if ((m & " FEAT_S(TAGPU_FEAT_MIRROR) ") != 0) {\n"
    "    float x0 = float((m >> " FEAT_S(TAGPU_FEAT_COPY_SHIFT) ") - " FEAT_S(TAGPU_FEAT_COPY_BIAS) ") * uMapPx.x;\n"
    "    if (s.x < x0 || s.x >= x0 + uMapPx.x) discard;\n"
    "    if ((m & " FEAT_S(TAGPU_FEAT_OFFROW) ") != 0 && s.y >= 0.0 && s.y < uMapPx.y) discard;\n"
    "    vec4 mb = texture(uBase, vUV);\n"
    "    if (mb.a < 0.5) discard;\n"
    "    vec4 mt = uRestored == 1 ? texture(uAtlasRGB, vUV) : vec4(0.0);\n"
    "    float ma = ((m & 3) == 2) ? 0.5 : 1.0;\n"
    "    frag = vec4(taEdge(mt.a > 0.5 ? mt.rgb : mb.rgb, vWorld) * ma, ma);\n"
    "    return;\n"
    "  }\n"
    /* THE MAP'S OWN, IN THE MAP'S OWN COLUMNS while the mirror draws: past a
       side edge the picture is the map reflected, and a tree's overhang there
       would cover the reflection of the tree itself. Past the top and bottom
       the copies stand upright, not reflected, so the painter's order stands
       there and the overhang is kept. Under a black edge nothing is clipped. */
    "  if (uClip == 1 && (s.x < 0.0 || s.x >= uMapPx.x)) discard;\n"
    /* features are terrain furniture: the engine draws them under the fog
       overlay, so they stay visible in grey */
    TAGPU_GLSL_FOG_DISCARD
    "  vec4 b = texture(uBase, vUV);\n"
    /* colour-keyed: the key texel is a hole, and discarding keeps it out of
       the depth buffer too -- a tree occludes only where it has pixels. The
       base's alpha is 0 exactly where the frame's index is its key (NEAREST,
       the texel the index was) */
    "  if (b.a < 0.5) discard;\n"
    "  float a = ((m & 3) == 2) ? 0.5 : 1.0;\n"
    /* the twin's colour where the lazy restore has painted it (alpha 1 --
       tagpu_gaf.h), the base atlas's otherwise -- always, under Classic --
       times the GROUND's lambert at the anchor (a billboard has no normal of
       its own; the lab's lambertAt -- a tree on a shaded slope sits in the
       shade rather than on top of it; 1.0 under `light=0`), then the grey band
       as the RGB rule (renderers.md 2.6). Premultiplied. */
    "  vec4 t = uRestored == 1 ? texture(uAtlasRGB, vUV) : vec4(0.0);\n"
    "  vec3 c = t.a > 0.5 ? t.rgb : b.rgb;\n"
    "  c *= vLam;\n"
    TAGPU_GLSL_FOG_GREY_RGB("c")
    "  frag = vec4(c * a, a);\n"
    "}\n";
#pragma GCC diagnostic pop

/* THE ATLAS IS THE PASS, NOT THE BACKEND, so its layout is set up here rather
   than in a backend bring-up: with `dim` at 0, `tagpu_gaf_atlas_create`
   refuses, `tagpu_gaf_atlas_mirror` is never asked for, every sprite lookup
   returns NULL, and the pass gathers into `atlas=0` and hands over nothing.
   The atlas is a CPU layout: `atlas_create` keys its existence on `made`,
   and no image exists on this side (tagpu_gaf.h). */
static void atlas_setup(void)
{
    tagpu_gaf_atlas_lost(&s_atlas);          /* laid out again on the create below */
    s_atlas.dim = ATLAS_DIM; s_atlas.max = ATLAS_MAX;
    s_atlas.ents = s_atlasEnts; s_atlas.tag = "feat";
    /* a world atlas: its base atlas is RGBA, so the key has to travel as a
       plane beside the indices (tagpu_gaf.h `keyPlane`) */
    s_atlas.keyPlane = 1;
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
    /* and the repack's moves are published, because tagpu_vk_feat.c applies
       them to its own copy: a repack then sends nothing but the paints it
       lacked, where the whole page is a wait on the render thread */
    s_atlas.moveList = 1;
    /* and its paints are bounded by what one frame's upload holds
       (tagpu_gaf.h `budget`): tagpu_vk_feat.c sends them in the frame's own
       command buffer, and a paint past the allowance waits a frame instead */
    s_atlas.budget = TAGPU_GAF_BUDGET;
    tagpu_gaf_atlas_create(&s_atlas);
}

/* ---- emission ---- */
static float s_encCur = 0.0f;
static int   s_bucketCur = B_BODY;
static int   s_mute = 0;                /* passive: count, emit nothing        */
static int   s_cBody, s_cShadow, s_cAtlasFail, s_cOverflow;
static int   s_cMBody, s_cMShadow;     /* the mirror's frames, apart          */
static int   s_ownable = 0;            /* the wreck pass is up: we may own it  */
/* this gather draws the mirror, so its map quads keep to the map's columns;
   set by the gather before the one publish that reads it */
static int   s_clip = 0;

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
   per-cell overlay rather than all at once. For the mirror that same sum is
   the point the corner is DRAWN at on the tile grid (x - 128 + eyeX), which is
   what the edge's tone and its map test read.
   `flip` mirrors the frame about its anchor: the hotspot is taken from the
   frame's other side and the two u swap, so a pixel centre i + 0.5 from the
   left samples texel w - 1 - i. */
static void emit_frame(const TAGPU_FXVIEW* v, const unsigned char* g, int sx, int sy,
                       int wax, int waz, int mode, float lam, int flip, int depth)
{
    int b = s_bucketCur, w, h;
    float x0, y0, x1, y1, c, ua, ub;
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
                if ((mode & 3) == MODE_OPAQUE && sg[TAGPU_GF_SUBALP]) m = MODE_ALPHA | (mode & ~3);
                emit_frame(v, sg, sx, sy, wax, waz, m, lam, flip, depth + 1);
            }
            return;
        }
    }
    switch (b) {
    case B_BODY:    s_cBody++;    break;
    case B_SHADOW:  s_cShadow++;  break;
    case B_MBODY:   s_cMBody++;   break;
    default:        s_cMShadow++; break;
    }
    if (s_mute) return;
    w = *(const unsigned short*)(g + TAGPU_GF_W);
    h = *(const unsigned short*)(g + TAGPU_GF_H);
    {
        int hx = *(const short*)(g + TAGPU_GF_HOTX);
        x0 = (float)(sx - (flip ? w - hx : hx));
    }
    y0 = (float)(sy - *(const short*)(g + TAGPU_GF_HOTY));
    x1 = x0 + (float)w; y1 = y0 + (float)h;
    if (x1 < (float)v->evpL || x0 > (float)(v->evpL + v->evw) ||
        y1 < (float)v->evpT || y0 > (float)(v->evpT + v->evh)) return;
    if (!feat_room(b, 6)) { s_cOverflow++; return; }
    {
        const unsigned dN = s_atlas.deferN;
        e = tagpu_gaf_atlas_get(&s_atlas, g);
        /* a deferral is not a failure: the anchor's rollback counts it */
        if (!e) { if (s_atlas.deferN == dN) s_cAtlasFail++; return; }
    }
    c = (float)e->ck / 255.0f;
    ua = flip ? e->u1 : e->u0;
    ub = flip ? e->u0 : e->u1;
    /* ONE TIE RULE FOR EVERY SPRITE, BY ITS OWN TEXEL GRID. The corners lie on
       whole game px, so a sample centre can fall exactly on a texel boundary --
       at the 0.25x floor every sample centre is a whole world px -- and there
       the texel NEAREST reads, and whether the quad covers the sample at all,
       would be the rounding's: it follows where the frame sits in the atlas,
       and a turned quad rounds the other way from an upright one. So every
       quad moves by TAGPU_EDGE_NUDGE after the zoom, as the terrain's do
       (tagpu_glsl.h): an upright one up and left, a TURNED one right, because
       its texels run the other way -- its sample at a' then reads the source's
       texel under r(a') + e, the one the map's own sprite reads at r(a'), so a
       mirrored sprite is its source's texels reflected.
       IT IS A 1/32-PX SHIFT AT EVERY ZOOM, the ground's own: every sample reads
       the texel under its point plus e = NUDGE / zoom world px. On the dyadic
       zooms from 0.5x to 8x no sample lies within e before a texel boundary (at
       1x a sample is 1/4 world px inside its texel), so none changes; at the
       floor every tie takes the texel after it; and at the wheel's other
       resting levels (its 1.14 steps, tagpu_zoom.c) every sample within e
       before a boundary reads the next texel -- which is where the ground
       under the sprite is read too. Which zoom has the ties is the zoom
       centre's parity: that is for an even viewport width, whose centre is a
       whole px; with an odd one it is a half px, which takes the 0.25x
       samples off the whole world px and puts the 0.5x ones on them. The
       world corners below are taken from the moved quad, so vWorld
       stays the sample's own point for the fog lookup and the edge's clips. */
    {
        const float nu = TAGPU_EDGE_NUDGE_PX / (v->zoom > 0.0f ? v->zoom : 1.0f);
        const float dx = flip ? nu : -nu;
        x0 += dx; x1 += dx; y0 -= nu; y1 -= nu;
    }
    {   /* world position of each corner: the anchor plus the corner's offset
           from the projected anchor (screen px and world px are 1:1 here) */
        float wx0 = (float)wax + (x0 - (float)sx), wx1 = (float)wax + (x1 - (float)sx);
        float wz0 = (float)waz + (y0 - (float)sy), wz1 = (float)waz + (y1 - (float)sy);
        put_vert(b, x0, y0, ua, e->v0, c, mode, wx0, wz0, lam);
        put_vert(b, x1, y0, ub, e->v0, c, mode, wx1, wz0, lam);
        put_vert(b, x0, y1, ua, e->v1, c, mode, wx0, wz1, lam);
        put_vert(b, x1, y0, ub, e->v0, c, mode, wx1, wz0, lam);
        put_vert(b, x1, y1, ub, e->v1, c, mode, wx1, wz1, lam);
        put_vert(b, x0, y1, ua, e->v1, c, mode, wx0, wz1, lam);
    }
}

/* ---- the LOS gate, 0x4658E0: two projected footprint corners. The per-tile
   test is the same one the effects pass runs per projectile, so it is shared
   rather than copied (both are the engine's LosType rule). Its points reach
   past the gathers' slab: the sweep's first row lies up to 15 px above it, a
   cell's height lifts a point up to 127 px more, and a far corner lies up to
   MAXFOOT cells past its row — and a sprite can reach below its footprint, so
   a corner past the slab is not a point nothing draws. The shared test
   answers each point from the grid as it is, moving only a point in the
   grid's unwritten last column or row onto that band's written edge
   (tagpu_fx_tile_visible). ---- */
static int feat_visible(const TAGPU_FXVIEW* v, int col, int row, int fx, int fz, int th)
{
    int hh = th >> 1;
    if (tagpu_fx_tile_visible(v, col * 16, row * 16 - hh)) return 1;
    return tagpu_fx_tile_visible(v, col * 16 + fx * 16, row * 16 + fz * 16 - hh);
}

/* ---- per-frame counters ---- */
typedef struct {
    int anchors, flat, tall, gafwreck, wreck3d, losSkip, junk, animated, shadows, outside, deferred;
    int mirrored;                /* the map's anchors drawn past its edge */
} FEATC;
static FEATC s_c;
static unsigned s_cAnchTrunc;   /* frames whose anchor table was truncated */
static int s_logged;
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
           The pool is FIXED for a level: 0x421F20 allocates WR_COUNT records at
           stride 0x30 and threads a free list through all of them, and the
           engine's allocators hand back WR_COUNT itself for "no record"
           (tagpu_engine.h). The engine's own draw at 0x46A6C4 does not bound this
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
                emit_frame(v, g, sx, sy, wax, waz, MODE_OPAQUE, lam, 0, 0);
                s_c.shadows++;
            }
        }
        g = tagpu_gaf_state_frame(rec + WR_BODYANIM);
        if (g) {
            s_bucketCur = B_BODY; s_encCur = encBody;
            emit_frame(v, g, sx, sy, wax, waz, MODE_OPAQUE, lam, 0, 0);
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
                emit_frame(v, g, sx, sy, wax, waz, (mask & 8) ? MODE_ALPHA : MODE_OPAQUE, lam, 0, 0);
                s_c.shadows++;
            }
        }
        /* the engine gates BOTH body paths on the static sequence pointer */
        if (!bodySeq) return;
        g = animating ? tagpu_gaf_state_frame(def + FD_BODYANIM)
                      : tagpu_gaf_seq_frame(bodySeq, 0);
        if (!g) return;
        s_bucketCur = B_BODY; s_encCur = encBody;
        emit_frame(v, g, sx, sy, wax, waz, (mask & 4) ? MODE_ALPHA : MODE_OPAQUE, lam, 0, 0);
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

/* ---- the map edge's mirror -------------------------------------------------
   Past the map, the view shows the MAP'S OWN features reflected with the
   ground under them (tascene-view.html, buildFeatures): the map as its TNT
   lays it out (tagpu_packet.h TAGPU_PK_MAPFEAT), never the live anchors -- a
   tree burnt, reclaimed or lost before a save, a wreck a scenario placed, or
   a feature the .ota schema placed, is the game's, not the map file's. The
   table arrives once a level; this pass keeps its own copy, bucketed by row
   and ordered by column inside each row, keyed on the level it came from. */
/* THE DIMENSION BOUND, the one this pass already refuses a map past
   (`mapW > 4096` in the gather): every array below is sized by it, so an index
   under the map's own dimension is under the array's. */
#define MF_DIM 4096
static TAGPU_PK_MAPFEAT s_mf[TAGPU_PK_MAX_MAPFEAT];   /* the copy, by row     */
static int              s_mfRow[MF_DIM + 1];          /* row starts into s_mf */
static int              s_mfCur[MF_DIM];              /* the bucketing's cursors */
static int              s_mfLast[MF_DIM];             /* per row, the last column
                                                         kept + 1 (the order test) */
static int              s_mfN, s_mfW, s_mfH, s_mfTrunc;
static int              s_mfHeld;      /* the copy is level s_mfLevel's         */
static unsigned         s_mfLevel;

int tagpu_feat_mapfeat_want(void) { return (int)s_mfWant; }
int tagpu_feat_mapfeat_holds(unsigned level_gen)
{
    return s_mfHaveOk && (unsigned)s_mfHaveLevel == level_gen;
}

/* what this pass will emit this frame: armed, not the passive A/B lever, and
   with the wreck half that lets it own the feature leaf (the gather's
   `s_mute` is the same test) */
static int feat_emits(void)
{
    return s_armed == 1 && !s_passive && tagpu_native_wrecks_armed();
}

void tagpu_feat_mapfeat_ask(int mirror)
{
    s_mfWant = (mirror && feat_emits()) ? 1 : 0;
}

/* ONE ENTRY OF THE TABLE, KEPT OR NOT: inside the map it claims, and to the
   right of the last entry kept on its row. The second test is what makes each
   row's bucket STRICTLY ordered by column -- the order mirror_gather walks it
   in -- by this side's own test and not by the publisher's. The publisher
   emits the list row-major, one anchor a cell, so it drops nothing there.
   Called in the same order by both passes of the bucketing, each from a
   cleared `s_mfLast`, so the two passes keep the same entries. */
static int mf_keep(const TAGPU_PK_MAPFEAT* e, int w, int h)
{
    if (e->row >= h || e->col >= w || e->col + 1 <= s_mfLast[e->row]) return 0;
    s_mfLast[e->row] = e->col + 1;
    return 1;
}

/* THE LEVEL'S TABLE, INTO THIS PASS'S COPY, from the packet that carries it
   whole (`mapfeat_ok`): an empty one is a map with no features, and is held
   like any other. Bucketed by row with a counting sort, every entry BOUNDED
   on the way in (`mf_keep`), so the lookups below index `s_mfRow` by a row
   under mapH and every kept column is under mapW. */
static void mapfeat_take(const TAGPU_PACKET* pk)
{
    const TAGPU_PK_MAPFEAT* src = tagpu_pk_mapfeat(pk);
    int w = pk->map_w16, h = pk->map_h16, n = (int)pk->n_mapfeat, i, kept = 0;
    if (!pk->mapfeat_ok || (s_mfHeld && s_mfLevel == pk->level_gen)) return;
    if (w <= 0 || h <= 0 || w > MF_DIM || h > MF_DIM || n < 0 || n > (int)TAGPU_PK_MAX_MAPFEAT)
        return;
    if (n > 0 && !src) return;
    memset(s_mfRow, 0, (size_t)(h + 1) * sizeof *s_mfRow);
    memset(s_mfLast, 0, (size_t)h * sizeof *s_mfLast);
    for (i = 0; i < n; i++)
        if (mf_keep(&src[i], w, h)) s_mfRow[src[i].row + 1]++;
    for (i = 0; i < h; i++) { s_mfRow[i + 1] += s_mfRow[i]; s_mfCur[i] = s_mfRow[i]; }
    memset(s_mfLast, 0, (size_t)h * sizeof *s_mfLast);
    for (i = 0; i < n; i++)
        if (mf_keep(&src[i], w, h)) { s_mf[s_mfCur[src[i].row]++] = src[i]; kept++; }
    s_mfN = kept; s_mfW = w; s_mfH = h;
    s_mfTrunc = (pk->truncated & TAGPU_PK_TRUNC_MAPFEAT) != 0;
    s_mfLevel = pk->level_gen; s_mfHeld = 1;
    InterlockedExchange(&s_mfHaveLevel, (LONG)s_mfLevel);
    InterlockedExchange(&s_mfHaveOk, 1);
    {
        char b[160];
        _snprintf(b, sizeof b, "feat: mirror: the map's own features taken for level %u: "
                               "%d of %d over %dx%d%s",
                  pk->level_gen, kept, n, w, h, s_mfTrunc ? " (the snapshot was TRUNCATED)" : "");
        b[sizeof b - 1] = 0;
        flog(b);
    }
}

int tagpu_feat_mapfeat_sync(const TAGPU_PACKET* pk, int mirror)
{
    if (!mirror || !feat_emits()) return 1;
    if (!pk || !pk->in_game) return 0;
    mapfeat_take(pk);
    return s_mfHeld && s_mfLevel == pk->level_gen && s_mfW == pk->map_w16 && s_mfH == pk->map_h16;
}

/* THE DEPTH KEYS' TWO RECTS. The map's own features take their keys over the
   engine's sweep rect CLAMPED to the map (r0/rows/c0/cols), with the mirror on
   or off: an on-map key never moves with the edge setting, so the map sorts
   against units and 3D wrecks exactly as it does without the mirror. A
   mirrored copy keys as its source (mirror_key); the flats of the mirror's
   rows above and below the map take their band over the UNCLAMPED sweep
   (ur0/urows/uc0/ucols). */
typedef struct {
    int r0, rows, c0, cols; float span;      /* clamped: the map's own keys */
    int ur0, urows, uc0, ucols;              /* unclamped: the whole sweep  */
    int tuc0;                                /* a turned copy's columns:
                                                [tuc0, tuc0 + ucols), the sweep
                                                mirrored about the view
                                                (mirror_gather)             */
} KEYRECT;

/* DrawGameScreen starts its feature sweep this many columns left of the
   eye's (`0x469748..0x4697B3`, exe-reverse-engineering.md) and sweeps the
   view's columns + 12 (OFF_SWEEP_C): 10 columns left of the view, 2 right. */
#define SWEEP_COL_LEAD 10

/* THE MAP'S OWN KEY for an anchor at (row, col) of the clamped rect, and the
   one function that computes it: the mirror takes the same key for a copy
   (mirror_key), so the two cannot round apart.
   - FLAT, the backdrop band: 0.40 + 0.10 f, f the row-major fraction over
     the clamped rect, in [0, 1) -- above the particle layers the engine draws
     before the pre-pass (0..2) and below those after it (3..4).
   - TALL, the row key the painter's sweep implies: above this row's units
     (1 + rel*4), below the next row's, with the column fraction
     1.5 (col - c0) / cols for left to right inside the row. The engine's
     edge clamps keep rel inside [0, sweepRows), the same range the unit
     gather uses; clamping to the band the frame's keys were sized for (rows
     + the gather's row slack) means no arithmetic here can ever push a
     feature into the effects band above it.
   TWO FLATS OF ONE ROW CAN TIE IN THE DEPTH BUFFER. Neighbouring columns are
   0.1/span apart in the key; the 24-bit buffer resolves about 1.2e-7 of the
   frame's depth scale (tagpu_native.c `depthScale`, some 360 at 1x and 890
   at 0.25x at 1024x768), so a key step under ~4e-5 at 1x or ~1e-4 at 0.25x
   is not a step. Where two such flats overlap, VK_COMPARE_OP_LESS keeps the
   one drawn first -- the left one, the anchors going left to right. */
static float map_key(const TAGPU_FXVIEW* v, const KEYRECT* k, int row, int col, int flat)
{
    if (flat) {
        float f = ((float)(row - k->r0) * (float)k->cols + (float)(col - k->c0)) / k->span;
        return 0.40f + 0.10f * f;
    } else {
        int rel = row - v->r0;
        if (rel < 0) rel = 0;
        if (rel > v->rows + 8) rel = v->rows + 8;
        return 3.0f + (float)rel * 4.0f + 1.5f * ((float)(col - k->c0) / (float)k->cols);
    }
}

/* A MIRRORED FEATURE'S KEY IS ITS SOURCE'S, AS COPY 0 OF ITS ROW TAKES IT.
   `col` is the source's own column, whatever copy the feature stands in, so
   every copy of a row stacks its features exactly as copy 0 does: the same
   keys, the same ties in the depth buffer (map_key), and the same order of
   emission (mirror_gather), whichever way round the copy is.
   - ON THE MAP'S OWN ROWS copy 0 is the map, and the key is map_key: a side
     copy is the map's picture reflected, overlaps and ties included.
   - ABOVE AND BELOW the clamped rect copy 0 is the upright band of the
     mirror, and a TALL key is map_key's rule on its row. A FLAT one takes
     f in [-0.6, -0.1) above and [1.1, 1.6) below, row-major over the whole
     sweep; the map's flats are in [0, 1), so the three bands never overlap
     and the map's own features, which reach into the band above, stay in
     front of it as the painter's order puts them. Every flat stays inside
     the particles' bracket, 0.30 below and 0.60 above, shadows (0.03 under)
     included.
   The source column is inside the clamped columns for every copy the view
   can show: the camera keeps the view's centre on the map (tagpu_zoom.c),
   so the reflection of what the view shows past an edge is on the map in
   view, and its sweep holds the sources. A column outside them keys as the
   nearest inside -- a copy only the sweep's margins reach. Different copies
   of one source share a key and never a sample: the fragment stage keeps
   each copy inside its own columns. */
static float mirror_key(const TAGPU_FXVIEW* v, const KEYRECT* k, int row, int col, int flat)
{
    const int T = k->r0, B = k->r0 + (k->rows > 0 ? k->rows : 0);
    if (k->cols > 0 && k->span > 0.0f) {
        const int L = k->c0, R = k->c0 + k->cols;
        col = col < L ? L : col >= R ? R - 1 : col;
        if (!flat || (row >= T && row < B)) return map_key(v, k, row, col, flat);
    } else {
        /* no column of the map in the sweep: the camera never leaves one,
           and the keys run over the whole sweep instead */
        const int L = k->uc0, R = k->uc0 + (k->ucols > 1 ? k->ucols : 1);
        col = col < L ? L : col >= R ? R - 1 : col;
        if (!flat) {
            int rel = row - v->r0;
            if (rel < 0) rel = 0;
            if (rel > v->rows + 8) rel = v->rows + 8;
            return 3.0f + (float)rel * 4.0f + 1.5f * ((float)(col - L) / (float)(R - L));
        }
    }
    {
        /* a flat above or below the clamped rect -- or beside a rect with no
           columns at all -- row-major over this band of the sweep */
        const int b0 = row < T ? k->ur0 : row >= B ? B : T;
        const int b1 = row < T ? T : row >= B ? k->ur0 + k->urows : B;
        const float u = ((float)(row - b0) * (float)k->ucols + (float)(col - k->uc0)) /
                        ((float)(b1 - b0 > 1 ? b1 - b0 : 1) * (float)(k->ucols > 1 ? k->ucols : 1));
        const float f = row < T ? -0.6f + 0.5f * u : row >= B ? 1.1f + 0.5f * u : u;
        return 0.40f + 0.10f * f;
    }
}

/* one of the map's anchors, mirrored to sweep cell (row, col); `copy` is
   its mode bits past the plain mode (TAGPU_FEAT_MIRROR and the copy it
   belongs to, tagpu_feat.h) */
static void mirror_feature(const TAGPU_FXVIEW* v, const TAGPU_PK_MAPFEAT* m,
                           const char* fdefs, int nDefs, int shadowsOn,
                           const KEYRECT* k, int row, int col,
                           int flipX, int flipY, int copy)
{
    const char* def;
    const char* shadSeq;
    const char* bodySeq;
    const unsigned char* g;
    unsigned mask;
    int fx, fz, hx, hz, wax, waz, sx, sy, flat, animating;
    float enc;
    if (!nDefs || (int)m->def >= nDefs) { s_c.junk++; return; }
    def = fdefs + (size_t)m->def * FD_STRIDE;
    flat = *(const unsigned char*)(def + FD_HEIGHT) < 10;
    if (flat ? !s_flat : !s_tall) return;
    fx = *(const short*)(def + FD_FOOTX);
    fz = *(const short*)(def + FD_FOOTZ);
    if (fx < 0 || fx > MAXFOOT) { fx = 1; s_c.junk++; }
    if (fz < 0 || fz > MAXFOOT) { fz = 1; s_c.junk++; }
    mask = *(const unsigned char*)(def + FD_MASK);
    s_c.mirrored++;
    /* THE PROJECTION, REFLECTED (the lab's buildFeatures). A reflection
       reverses the cells a footprint spans, so a mirrored feature ENDS on the
       cell its source begins on. The ground past the edge is the tile ART
       reflected, and the art has the height baked in, so across a top or
       bottom edge the feature moves by the height the other way: the half
       height is ADDED where the map's own takes it off. Upright either way --
       only a side edge turns the sprite over. */
    hx = (fx * 16) / 2; hz = (fz * 16) / 2;
    wax = flipX ? (col + 1) * 16 - hx : col * 16 + hx;
    waz = flipY ? (row + 1) * 16 - hz + m->lift : row * 16 + hz - m->lift;
    sx = wax + 128 - v->eyeX;
    sy = waz + 32 - v->eyeY;
    enc = mirror_key(v, k, row, m->col, flat);
    shadSeq = *(const char* const*)(def + FD_SHADSEQ);
    bodySeq = *(const char* const*)(def + FD_BODYSEQ);
    animating = (mask & 2) != 0;
    {
        /* WHOLE OR NOT AT ALL, as for the map's own (the gather's loop) */
        const int nSh = s_nv[B_MSHADOW], nBo = s_nv[B_MBODY];
        const unsigned dN = s_atlas.deferN;
        if (shadSeq && shadowsOn && s_shadow) {
            g = animating ? tagpu_gaf_state_frame(def + FD_SHADANIM)
                          : tagpu_gaf_seq_frame(shadSeq, 0);
            if (g) {
                s_bucketCur = B_MSHADOW;
                s_encCur = enc - (flat ? 0.03f : 0.3f);
                emit_frame(v, g, sx, sy, wax, waz,
                           ((mask & 8) ? MODE_ALPHA : MODE_OPAQUE) | copy,
                           1.0f, flipX, 0);
            }
        }
        /* the engine gates both body paths on the static sequence pointer */
        if (bodySeq) {
            g = animating ? tagpu_gaf_state_frame(def + FD_BODYANIM)
                          : tagpu_gaf_seq_frame(bodySeq, 0);
            if (g) {
                s_bucketCur = B_MBODY; s_encCur = enc;
                emit_frame(v, g, sx, sy, wax, waz,
                           ((mask & 4) ? MODE_ALPHA : MODE_OPAQUE) | copy,
                           1.0f, flipX, 0);
            }
        }
        if (s_atlas.deferN != dN) {
            s_nv[B_MSHADOW] = nSh; s_nv[B_MBODY] = nBo;
            s_c.deferred++;
        }
    }
}

/* floor(a / b) for b > 0, whatever a's sign */
static int floor_div(int a, int b)
{
    int q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

/* THE SWEEP PAST THE MAP: every cell past the map of the rect the engine
   would sweep -- the unclamped rect for a copy the right way round, the rect
   mirrored about the view for a turned one (below) -- takes the map's anchor
   at the cell it folds to, and its source's key (mirror_key). The engine's
   sweep never reaches the map's last row or column (the clamps in the
   gather), so an anchor there is not drawn on the map and is not mirrored
   either.
   WALKED BY ANCHOR, NOT BY CELL: a sweep row splits at the folds into runs of
   at most one map width, each a copy of the map's row either way round
   (`tagpu_edge_reflect`: copy q = floor(col / mapW), turned over when q is
   odd), so each run is one pass over the row's bucket (ordered by `mf_keep`)
   and a cell with no anchor costs nothing -- at a 3840 x 2160 corner at 0.25x
   the sweep is 944 x 560 cells and a map row has a handful of anchors. Every
   index is bounded: a row by the fold into [0, mapH), a bucket by `s_mfRow`,
   and every column from the bucket is under mapW.
   EVERY RUN IS EMITTED IN ITS SOURCE ROW'S ORDER, a turned one right to left
   on the screen: the runs follow the rows down, and each run takes the
   bucket in the order the map's own anchors come (row-major, the engine's
   sweep). With the keys (mirror_key) the order is the whole of how a copy
   stacks: the shadows write no depth, so where two overlap the later one
   blends over the earlier, and where two bodies' keys tie in the depth
   buffer the one drawn first stays. A copy laid down in its source's order
   over its source's keys stacks as its source does.
   A TURNED RUN IS SWEPT OVER THE RECT MIRRORED ABOUT THE VIEW (KEYRECT
   `tuc0`). The engine's rect reaches 10 columns left of the view and 2 right
   (SWEEP_COL_LEAD), so the map's own art that overhangs to the right of its
   anchor is drawn from anchors up to 10 columns off-screen; a turned copy
   overhangs to the left, and takes its cells from the rect the engine would
   sweep over the view reflected. A copy is drawn exactly when its source
   would be, so the strip past a side edge is the map's own picture
   reflected, up to the edge of the view. */
static void mirror_gather(const TAGPU_FXVIEW* v, const char* fdefs, int nDefs,
                          int shadowsOn, const KEYRECT* k)
{
    const int mapW = s_mfW, mapH = s_mfH;
    const int c0 = k->uc0, c1 = k->uc0 + k->ucols;
    const int t0 = k->tuc0, t1 = k->tuc0 + k->ucols;
    const int s0 = c0 < t0 ? c0 : t0, s1 = c1 > t1 ? c1 : t1;
    /* the view, the gather's rect in world px */
    const int vx0 = v->eyeX + (v->evpL - v->vpL), vx1 = vx0 + v->evw;
    const int vy0 = v->eyeY + (v->evpT - v->vpT), vy1 = vy0 + v->evh;
    int row;
    if (k->ucols <= 0 || k->ucols > MF_DIM || k->urows <= 0 || mapW <= 0 || mapH <= 0) return;
    for (row = k->ur0; row < k->ur0 + k->urows; row++) {
        int offRow = row < 0 || row >= mapH, flipY = 0, srow = row, a0, a1, q;
        /* A COPY THE VIEW CANNOT SHOW IS NOT EMITTED: the fragment stage keeps
           every copy inside its own columns, and a row past the top or bottom
           inside its band, so a copy whose region misses the view draws
           nothing. The copies left are the ones whose sources the view holds
           (mirror_key). */
        if (row < 0 ? vy0 >= 0 : row >= mapH && vy1 <= mapH * 16) continue;
        if (offRow) srow = tagpu_edge_reflect(row, mapH, &flipY);
        if (srow >= mapH - 1) continue;
        a0 = s_mfRow[srow]; a1 = s_mfRow[srow + 1];
        if (a0 == a1) continue;
        for (q = floor_div(s0, mapW); q * mapW < s1; q++) {
            const int base = q * mapW, turned = q & 1;
            const int w0 = turned ? t0 : c0, w1 = turned ? t1 : c1;
            const int lo = base > w0 ? base : w0, hi = base + mapW < w1 ? base + mapW : w1;
            int copy, j;
            /* the map's own cells are the anchor loop's */
            if (!offRow && q == 0) continue;
            if (lo >= hi || base * 16 >= vx1 || (base + mapW) * 16 <= vx0) continue;
            /* THE MODE'S COPY FIELD IS BOUNDED BEFORE IT IS BUILT (tagpu_feat.h):
               q + COPY_BIAS is in [0, 2 COPY_BIAS), so the mode stays under 2^15 */
            if (q < -TAGPU_FEAT_COPY_BIAS || q >= TAGPU_FEAT_COPY_BIAS) continue;
            copy = TAGPU_FEAT_MIRROR | (offRow ? TAGPU_FEAT_OFFROW : 0) |
                   ((q + TAGPU_FEAT_COPY_BIAS) << TAGPU_FEAT_COPY_SHIFT);
            if (!turned) {
                for (j = a0; j < a1; j++) {
                    const int scol = s_mf[j].col, col = base + scol;
                    if (scol >= mapW - 1 || col >= hi) break;
                    if (col < lo) continue;
                    mirror_feature(v, &s_mf[j], fdefs, nDefs, shadowsOn, k, row, col,
                                   0, flipY, copy);
                }
            } else {
                for (j = a0; j < a1; j++) {
                    const int scol = s_mf[j].col, col = base + mapW - 1 - scol;
                    if (scol >= mapW - 1 || col < lo) break;
                    if (col >= hi) continue;
                    mirror_feature(v, &s_mf[j], fdefs, nDefs, shadowsOn, k, row, col,
                                   1, flipY, copy);
                }
            }
        }
    }
}

int tagpu_feat_gather(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    const TAGPU_PK_ANCHOR* anch;
    const char* fdefs;
    int mapW, mapH, nCols, nRows, r0, c0;
    int localPl, shadowsOn, nDefs, liveDefs, outside = 0; /* a flag: see the loop below */
    int onMap, mirror;
    unsigned ai;
    KEYRECT key;
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
    /* the allowance's frame, after the reset and before the first lookup */
    tagpu_gaf_atlas_frame(&s_atlas);
    /* This atlas's restore is the Vulkan restorer's: `tagpu_gaf_atlas_restore_vk`
       above publishes the frame list and `tagpu_vk_restore.c` paints it. */

    memset(s_nv, 0, sizeof s_nv);
    memset(&s_c, 0, sizeof s_c);
    s_cBody = s_cShadow = s_cAtlasFail = s_cOverflow = 0;
    s_cMBody = s_cMShadow = 0;
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
    key.ur0 = r0; key.urows = nRows;             /* before the map's clamps */
    if (r0 < 0) { nRows += r0; r0 = 0; }
    if (r0 + nRows > mapH - 1) nRows = mapH - r0 - 1;
    c0 = ((v->eyeX + (v->evpL - v->vpL)) >> 4) - SWEEP_COL_LEAD;
    nCols += (v->evw - v->vw) >> 4;
    if (nCols > 4096) nCols = 4096;
    key.uc0 = c0; key.ucols = nCols;
    /* the same rule applied to the view reflected: its right edge, rounded
       up, is the reflected view's left edge rounded down (mirror_gather) */
    key.tuc0 = -floor_div(-(v->eyeX + (v->evpL - v->vpL) + v->evw), 16) + SWEEP_COL_LEAD - nCols;
    if (c0 < 0) { nCols += c0; c0 = 0; }
    if (c0 + nCols > mapW - 1) nCols = mapW - c0 - 1;
    /* `v->mirror` is 1 only while this level's copy is held
       (tagpu_feat_mapfeat_sync); its dimensions are the ones every index into
       it was bounded against, so they must be this map's. */
    mirror = v->mirror && s_mfHeld && s_mfLevel == pk->level_gen && s_mfW == mapW && s_mfH == mapH &&
             key.urows > 0 && key.ucols > 0;
    s_clip = mirror;
    /* A VIEW WHOLLY PAST THE MAP has no cell of its own to sweep, and still
       has the mirror to draw. */
    onMap = nRows > 0 && nCols > 0;
    if (!onMap && !mirror) return feat_bail();
    /* the map's own keys: the engine's clamped rect, whatever the edge (KEYRECT) */
    key.r0 = r0; key.rows = nRows; key.c0 = c0; key.cols = nCols;
    key.span = (float)nRows * (float)nCols;

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

    /* THE ANCHORS COME OUT OF THE PACKET, in the same row-major order the grid
       walk produced them, over a rect the publisher sized for the WIDEST zoom —
       so this loop's own rect is a sub-rect of it and the filter below selects
       it. `outside=1` says THIS FRAME'S rect asked
       for cells outside the ones the packet carried — it is a flag, not a
       count, because what fell outside cannot be counted from here — and it is
       0 at every reachable zoom. It is the number to look at if features ever
       stop short of the frame edge. */
    anch = tagpu_pk_anchors(pk);
    /* a table the publisher could not fit: it keeps the rows nearest the
       view's centre and drops the rest whole (tagpu_packet_pub.c), so a count
       here that is not 0 is the number to look at if features ever vanish
       from the top or bottom of the frame */
    if (pk->truncated & TAGPU_PK_TRUNC_ANCHORS) s_cAnchTrunc++;
    if (onMap && (r0 < pk->anch_r0 || c0 < pk->anch_c0 ||
        r0 + nRows > pk->anch_r0 + pk->anch_rows ||
        c0 + nCols > pk->anch_c0 + pk->anch_cols)) outside++;
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
            enc = map_key(v, &key, row, col, flat);
            {
                /* A FEATURE IS DRAWN WHOLE OR NOT AT ALL: its shadow and body
                   -- and every sub-frame of either -- are several atlas
                   entries, and one of them deferred past the allowance takes
                   the rest of this anchor's quads back out, so no frame shows
                   a body without its shadow. What did paint stays painted and
                   draws on the next frame, with the rest. */
                const int nSh = s_nv[B_SHADOW], nBo = s_nv[B_BODY];
                const unsigned dN = s_atlas.deferN;
                draw_feature(v, pk, a, def, flat, enc, shadowsOn);
                if (s_atlas.deferN != dN) {
                    s_nv[B_SHADOW] = nSh; s_nv[B_BODY] = nBo;
                    s_c.deferred++;
                }
            }
        }
    }
    s_c.outside = outside;
    /* the mirror's, after the map's own: its buckets are drawn last */
    if (mirror) mirror_gather(v, fdefs, nDefs, shadowsOn, &key);

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
            sappend(b, sizeof b, &p, " anim=%d los-skip=%d junk=%d outside=%d trunc=%u -> body=%d shadow=%d atlas=%d",
                    s_c.animated, s_c.losSkip, s_c.junk, s_c.outside, s_cAnchTrunc, s_cBody, s_cShadow, s_atlas.n);
            /* repacks should settle at a small number and stop; `wall` means
               the map wants more than one 2048 page holds (tagpu_gaf.h) */
            sappend(b, sizeof b, &p, " repack=%u%s", s_atlas.repacks,
                    s_atlas.repackWall ? " WALL" : "");
            if (s_cOverflow || s_cAtlasFail)
                sappend(b, sizeof b, &p, " DROPPED(full=%d atlas-fail=%d)",
                        s_cOverflow, s_cAtlasFail);
            if (s_c.deferred)
                sappend(b, sizeof b, &p, " deferred=%d", s_c.deferred);
            if (v->mirror)
                sappend(b, sizeof b, &p, " | mirror: map=%d%s anchors=%d body=%d shadow=%d",
                        (s_mfHeld && s_mfLevel == pk->level_gen) ? s_mfN : -1, s_mfTrunc ? "(trunc)" : "",
                        s_c.mirrored, s_cMBody, s_cMShadow);
            if (s_passive) sappend(b, sizeof b, &p, " (passive: nothing emitted)");
            else if (!s_ownable)
                sappend(b, sizeof b, &p,
                        " (nothing emitted: native.on needs \"wrecks\" before we can own the leaf)");
            flog(b);
        }
    }
    return s_nv[B_SHADOW] + s_nv[B_BODY] + s_nv[B_MSHADOW] + s_nv[B_MBODY];
}

/* Everything the gather above produced, for the Vulkan edition of this pass
   (tagpu_feat.h): each field is a uniform's value, a pointer into the vertex
   arrays, or a buffer the consumer uploads an image from. */
/* THE FOG GRID IS COPIED, NOT ALIASED. `v->fogGrid` points INSIDE a frame-packet
   slot, and tagpu_packet.h gives both packet pointers a lifetime that ends at
   tagpu_packet_frame_end() -- which render_vk.c calls BEFORE `tagpu_vk_frame`
   runs the Vulkan lane, so a pointer handed on from here is read past its
   contract. An
   alias would hold only because the give-back happens at the next acquire,
   which is also where the `poison` lever fills the slot: the one stale read
   that lever cannot see, and outside frame_end's tail==head check as well.
   The atlas and the height grid are handed over as buffers this module owns;
   the copy makes the fog grid the same, and makes the file header's "a pass
   reads no engine state" true of the whole hand-over.

   `cells * 2` is the grid's own size, cols x rows 16-bit cells as the packet
   publishes them; the 1024-a-side cap bounds the ALLOCATION, and the pass
   re-checks it (FOG_MAXDIM) because a bound in one file is a bound only while
   both are read together. */
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
    s_pub.mshadow = s_verts[B_MSHADOW]; s_pub.nMShadow = s_nv[B_MSHADOW];
    s_pub.mbody   = s_verts[B_MBODY];   s_pub.nMBody   = s_nv[B_MBODY];
    /* the map on the TILE grid, as the terrain's uMapPx (tagpu_terr.c): the
       two passes test the one edge */
    s_pub.mapPxW = (float)((s_mapW / 2) * 32); s_pub.mapPxH = (float)((s_mapH / 2) * 32);
    s_pub.clip = s_clip;
    s_pub.gw = (float)v->gw; s_pub.gh = (float)v->gh;
    s_pub.zoom = v->zoom > 0.0f ? v->zoom : 1.0f;
    s_pub.nudgeW = TAGPU_EDGE_NUDGE_PX / s_pub.zoom;
    s_pub.zoomCx = v->zoomCx; s_pub.zoomCy = v->zoomCy;
    s_pub.depthScale = v->depthScale > 1.0f ? v->depthScale : 512.0f;
    /* THE ROUTE IS THE PUBLISHED LIST: `rlistWant` is what says a restore
       route exists; it is latched by the arm. */
    s_pub.restored = (s_atlas.rlistWant && tagpu_classicpp_assets()) ? 1 : 0;
    s_pub.fog = v->fogMode & 1;
    s_pub.fogOrgX = (float)v->fogOrgX; s_pub.fogOrgY = (float)v->fogOrgY;
    s_pub.fogCols = (float)v->fogCols; s_pub.fogRows = (float)v->fogRows;
    tagpu_feat_atlas_hand(&s_pub);
    /* THE REQUEST, NOT THE PICTURE: the restored twin's texels are never
       read back (tagpu_gaf.h), so the list is the only restore route this
       hand-over carries. */
    if (s_atlas.rlistWant && s_atlas.rlist) {
        s_pub.restoreFrames  = s_atlas.rlist;
        s_pub.restoreN       = s_atlas.rlistN;
        s_pub.restoreGen     = s_atlas.rlistGen;
    }
    /* the grid as the fragment shader will read it, and only when it will:
       `uFog` 0 means taFog is never called and uFogGrid never sampled. */
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
    s_pub.vpL = v->vpL; s_pub.vpT = v->vpT; s_pub.vw = v->vw; s_pub.vh = v->vh;
    /* WHETHER THE CLIP IS ACTUALLY ON, not whether a rect exists: the native
       pass's own decision (tagpu_native.c `s_scissorOn`). A pass that clipped
       when the rest of the world did not would differ in every feature the
       gather's margin reaches outside the viewport -- which on a forest map is
       a wide band down both edges. */
    s_pub.scissorOn = tagpu_native_scissor_on();
    s_pub.ss = v->ss;
    /* THE A/B FLAG LIVES EXACTLY ONE FRAME. The Vulkan lane takes the hand-over
       later in this same render-thread iteration, so a flag that was not taken
       was not taken because the lane is down -- and a claim left standing would
       capture some later frame than the one the lever was armed on. */
    s_pub.ab = s_abFrame; s_abFrame = 0;
    /* THE STAMP (tagpu_terr.h has the argument in full). `s_pub.shadow`/`body`
       point into `s_verts`, which this file `realloc`s, and `s_pub.atlas` into
       the GAF mirror -- so the hand-over is this frame's or it is nothing. */
    s_pub.frame = v->frame_counter;
    s_pubHave = total > 0 && !fogBad;
}

/* The atlas's half of the hand-over (tagpu_feat.h), and the palette its
   base atlas is expanded through: THE ENGINE'S TABLE, unscaled -- the world
   composite applies the Gamma factor once, to the finished image
   (tagpu_pal.h). One writer for both the draw's hand-over and the upload the
   consumer owes on a frame it does not draw. */
int tagpu_feat_atlas_hand(TAGPU_FEATHAND* h)
{
    TAGPU_GAFVIEW av;
    int ok = tagpu_gaf_atlas_view(&s_atlas, &av);
    h->atlas = av.idx; h->atlasDim = ok ? av.dim : s_atlas.dim;
    h->atlasKey = av.key; h->atlasDirty = av.dirty;
    h->atlasRows = av.rows;
    h->atlasSerial = av.serial; h->atlasWhole = av.whole;
    h->atlasMoves = av.moves; h->atlasMoveN = av.moveN;
    h->atlasMoveSerial = av.moveSerial; h->atlasMovePrev = av.movePrev;
    h->pal = tagpu_pal_engine(); h->palSerial = tagpu_pal_engine_serial();
    return ok;
}

int  tagpu_feat_atlas_owed(void) { return tagpu_gaf_atlas_owed(&s_atlas); }
void tagpu_feat_atlas_ack(unsigned serial, int keep) { tagpu_gaf_atlas_ack(&s_atlas, serial, keep); }

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
    /* THIS PASS DOES NOT DRAW; IT GATHERS AND HANDS OVER to tagpu_vk_feat.c,
       which draws. */
    int total = s_nv[B_SHADOW] + s_nv[B_BODY] + s_nv[B_MSHADOW] + s_nv[B_MBODY];
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
