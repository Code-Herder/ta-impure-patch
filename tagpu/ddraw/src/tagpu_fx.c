/* tagpu_fx.c — the effects pass: weapon fire, explosions, debris.

   The engine draws these in two colour-only passes between the ground unit
   sweep and the airborne sweep (DrawGameScreen call sites 0x469B22 and
   0x469B2C; research/notes/effects.md has the decompiled rules):

     0x49BE60  projectiles  — walks the ProjectileStruct array (count
               main+0x141F3, base *(main+0x141F7), stride 0x6B). Per record,
               LOS-gated at the anchor tile, then by WeaponStruct.RenderType
               (+0x10C):
                 0 laser        DrawLine start->head, colour main+0xDCB[color],
                                two lines when color2 (+0x10E) != 0
                 1 model        shadow blob + 3DO root (+0x74) rotated by
                                (t0, t1-0x8000, t2-0x8000); the root's child
                                (thrust flame) while tick < +0x46, spinning by
                                +0x64 when WeaponTypeMask bit 21
                 2 ball         background-refraction sprite (main+0x1AB9B)
                                — NOT reproduced (counted only)
                 3 model        shadow blob + root, no rotation
                 4 sprite       shadow blob + anim set main+0x147BB[color]
                                (color 0..4), frame (tick-spawn) % n, opaque
                 5 flare        main+0x147F3, frame by remaining life, alpha
                 6 model        shadow blob + root rotated by the raw triple
                 7 lightning    two jagged polylines start->head, +-5 jitter
     0x420B00  explosions   — flying debris pieces (the particle slots,
               tagpu_limits_psys_begin..end -> +0x2C piece {node, turn@0x12,
               pos@0x16}) then, over the ExplosionStruct array (count then
               records, tagpu_limits_expl_pool(), stride 0x54; anchor must be
               inside the viewport rect): the LHT "flash" (anim state +0x10, table
               TAProgram+0xC8) for all, then per record the debris node (+0)
               rotated by +0x4C and the opaque sprite (anim state +0x04).

   Here: models are handed to the native pass (same face/atlas/palette path,
   unshaded like the engine's GAF_DrawTransformed); lines and sprites are
   gathered here and drawn by tagpu_vk_fx.c right after the unit bodies,
   at a depth band above every ground row and below the airborne band. GAF
   frames (raw or TA-RLE, sub-frame lists) are decoded into a private atlas.
   ALP sprites take the engine's table against the pixel underneath where the
   device has the ordered pixel interlock (TAGPU_FX_ALP_FN), a 50 % mix in RGB where it
   cannot; the LHT flash = additive, per-level colour derived
   from the live table. Armed by tagpu_fx.on; tokens: log, nolines, nomodels,
   nosprites, noexpl, nodebris, passive (gather + log, nothing drawn).
   The particle sfx pass (tagpu_sfx.c, armed by tagpu_sfx.on) rides the same
   buckets and shaders: layers 0..6 are emitted before the projectiles so the
   engine's order (smoke under weapon sprites) survives, layers 7..9 after
   the explosions; each sprite carries its own depth key. Read-only over sim. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tagpu_opt.h"
#include "tagpu_addr.h"   /* tagpu_user_top: the range filters' upper end */
#include "tagpu_fx.h"
#include "tagpu_pal.h"
#include "tagpu_sfx.h"
#include "tagpu_glsl.h"
#include "tagpu_line.h"
#include "tagpu_gaf.h"
#include "tagpu_classicpp.h"
#include "tagpu_packet.h"
#include "tagpu_native.h"   /* tagpu_native_scissor_on, for the hand-over */
#include "tagpu_vk.h"       /* tagpu_vk_armed(): whether to pay for the mirror */
#include "tagpu_fxmodel.h" /* a model is rasterised where it is emitted */
#include "tagpu_log.h"



#define MODE_FLAT   TAGPU_FXMODE_FLAT
#define MODE_OPAQUE TAGPU_FXMODE_OPAQUE
#define MODE_ALPHA  TAGPU_FXMODE_ALPHA
#define MODE_FLASH  TAGPU_FXMODE_FLASH

/* THE STRIDE IS tagpu_fx.h's, so the vertices written here and the Vulkan
   attribute array cannot drift apart. */
#define FXST     TAGPU_FX_VST   /* x,y,enc, u,v, c,mode, wx,wz               */
/* every model the packet's tables can name: a projectile's body and its
   flame, a debris piece, an explosion's body -- so the list itself never
   refuses one, and `tagpu_fx_model_bound` can never exceed it */
#define MAXMODEL (2 * (int)TAGPU_PK_MAX_PROJ + (int)TAGPU_PK_MAX_DEBRIS + \
                  (int)TAGPU_PK_MAX_EXPL)
#define ATLAS_DIM 2048
#define ATLAS_MAX 2048

static void flog(const char* s)
{
    tagpu_log(s);
}

/* the shared shelf atlas (tagpu_gaf.c). Its entries are keyed on the frame
   header AND its pixel pointer/dims: effect sequences (the flash) are freed
   when their explosion ends and the address is reused for other frames */
static TAGPU_GAFENT   s_atlasEnts[ATLAS_MAX];
static TAGPU_GAFATLAS s_atlas;

/* ---- arming ---- */
/* THE A/B LEVER. It re-arms when the file goes away and comes
   back, which is why `touch` on one that is already there does nothing. */
#define ABFILE   "tagpu_fx.ab"
static int  s_ab, s_abDone, s_abFrame;
static int  s_pubHave;                 /* this frame's hand-over is waiting   */
static TAGPU_FXHAND s_pub;
static int  s_mirrorAsked;             /* the atlas mirror has been asked for */
static int  s_rlistAsked;              /* ...or the published restore list    */
static int  s_armed = -1;
static int  s_log = 0, s_lines = 1, s_models = 1, s_sprites = 1, s_expl = 1, s_debris = 1;
static int  s_passive = 0;             /* gather + log only; engine keeps drawing */
static unsigned s_armCheck = 0;

/* THE ATLAS MIRROR AND THE RESTORE LIST, asked for together: on the arm beat,
   and in the gather the moment `atlas_setup` has given the atlas its
   dimensions, before the first lookup paints into it -- tagpu_feat.c's
   `consumer_ask`, for the same reason: the Vulkan edition draws only from the
   mirror, and nothing is published until it exists (`fx_publish`).

   THE MIRROR. Asking marks every painted entry reserved, and
   tagpu_gaf_atlas_get PAINTS a reserved entry before it returns it, so by the
   time a quad carries a UV those texels are in the mirror as well as in the
   texture. 8 MB with its key plane, so it is asked OF THE CONSUMER, NOT OF
   THE LEVER: these latches are one-way, so once asked the memory is held for
   the process's life; `tagpu_vk_armed()` is true whenever `tagpu_vk.on`
   exists, including under a renderer where the lever arms nothing, and the
   mirror would be paid for with no consumer at all.
   `tagpu_vk_owns_present()` is exactly "a Vulkan pass will run in this
   process", which is the question, and one load to answer. `s_mirrorAsked`
   is set only on SUCCESS, so a request made before the atlas has its
   dimensions is retried.

   THE RESTORE LIST. Under Classic++ `assets=1` the Vulkan lane restores the
   atlas itself, from this list; it is the only route by which a restored twin
   exists at all. Asked until it takes, exactly as the mirror is, because the
   knob is allowed to move mid-session. */
static void consumer_ask(void)
{
    if (!s_mirrorAsked && s_atlas.dim > 0 && tagpu_vk_owns_present())
        s_mirrorAsked = tagpu_gaf_atlas_mirror(&s_atlas);
    if (s_mirrorAsked && !s_rlistAsked && tagpu_classicpp_assets())
        s_rlistAsked = tagpu_gaf_atlas_restore_vk(&s_atlas);
}

static void read_arm(unsigned frame_counter)
{
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return;
    s_armCheck = frame_counter;
    int was = s_armed;
    s_armed = 0;
    char buf[128];
    int n = tagpu_opt_read("tagpu_fx.on", buf, sizeof buf);
    s_log = 0; s_lines = s_models = s_sprites = s_expl = s_debris = 1; s_passive = 0;
    if (n < 0) {
        if (was > 0) flog("fx: disarmed");
        return;
    }
    if (n > 0) {
        buf[n] = 0;
        char* p = buf;
        while (*p) {
            while (*p && *p <= ' ') p++;
            char* q = p;
            while (*q && *q > ' ') q++;
            int last = (*q == 0);
            *q = 0;
            if (!lstrcmpiA(p, "log")) s_log = 1;
            else if (!lstrcmpiA(p, "nolines")) s_lines = 0;
            else if (!lstrcmpiA(p, "nomodels")) s_models = 0;
            else if (!lstrcmpiA(p, "nosprites")) s_sprites = 0;
            else if (!lstrcmpiA(p, "noexpl")) s_expl = 0;
            else if (!lstrcmpiA(p, "nodebris")) s_debris = 0;
            else if (!lstrcmpiA(p, "passive")) s_passive = 1;
            if (last) break;
            p = q + 1;
        }
    }
    s_armed = 1;
    /* the A/B lever, on the same beat as the arm (tagpu_feat.c's shape) */
    s_ab = GetFileAttributesA(ABFILE) != INVALID_FILE_ATTRIBUTES;
    if (!s_ab) s_abDone = 0;
    consumer_ask();
    if (was != 1) {
        char b[128];
        _snprintf(b, sizeof b, "fx: ARMED (lines=%d models=%d sprites=%d expl=%d debris=%d log=%d passive=%d)",
                  s_lines, s_models, s_sprites, s_expl, s_debris, s_log, s_passive);
        flog(b);
    }
}

int tagpu_fx_armed(unsigned frame_counter)
{
    read_arm(frame_counter);
    return s_armed > 0;
}

/* three buckets, drawn in this order: lines, flashes (additive), sprites (weapon sprites,
   explosions) -- the particle events of layers 0..6 before them and of 7..9 after them
   (tagpu_vk_fx.c) -- so lasers and flashes sit over trail smoke and explosion sprites over
   their flash like the engine; no per-run segment table, so nothing is ever dropped for
   alternating too often */
/* the bucket names are tagpu_fx.h's too, for the same reason: the Vulkan lane
   concatenates the three arrays in this order and draws them with three
   different pipelines, so the order is part of the contract */
#define B_LINES   TAGPU_FXB_LINES
#define B_FLASH   TAGPU_FXB_FLASH
#define B_SPRITES TAGPU_FXB_SPRITES
#define NBUCKET   TAGPU_FXB_N
/* one array a bucket, each at its own cap (tagpu_fx.h) */
static float  s_vLines[TAGPU_FX_MAXV * FXST], s_vFlash[TAGPU_FX_MAXV * FXST],
              s_vSprites[TAGPU_FX_MAXV_SPRITES * FXST];
static float* const s_verts[NBUCKET] = {
    [B_LINES] = s_vLines, [B_FLASH] = s_vFlash, [B_SPRITES] = s_vSprites
};
static int    s_nv[NBUCKET];
static float  s_encCur;                /* depth key of what is being emitted  */
static int    s_mute = 0;              /* passive: count, emit nothing        */
static TAGPU_FXMODEL s_models_[MAXMODEL];
static int    s_nm = 0;
/* this frame's band (TAGPU_FXVIEW `encFx`, `encStep`, `modelCap`,
   `modelsOn`), latched by the gather */
static float  s_encFx, s_encStep;
static int    s_modelCap, s_modelsOn;
/* models asked for and not drawn this frame, and why: the posed pass or the
   unit pass cannot take them (`modelsOn`), the band is full, or the
   rasteriser refused it
   (tagpu_fxmodel.h: not carried, a texel the unit atlas has not painted, no
   room) -- the last reason is kept for the heartbeat */
static int    s_cModelLost, s_modelWhy;
/* the frame the models are rasterised into, from the view (tagpu_fxmodel.h) */
static TAGPU_FXRVIEW s_rv;

static int s_lhtInit = 0;
/* THE FLASH LIGHT TABLE'S OWN BYTES, at file scope because the hand-over
   carries them to the Vulkan pass: this array is the table. 32 x 1 RGB, and
   `s_lhtInit` is what says whether it has ever been built. */
static unsigned char s_lhtRGB[32 * 3];
/* WHAT IT WAS BUILT FROM, and the key that rebuilds it: the engine palette
   serial and the LHT bytes. A move of either rebuilds the table on the frame it
   is seen, so the flash colours are never the old table's beside a base atlas
   re-sent in the new one. */
static unsigned s_lhtPal;
static unsigned char s_lhtSrc[TAGPU_PK_LHT_BYTES];

/* THE ALP BLEND TABLE AS THE VULKAN PASS READS IT (TAGPU_FX_ALPT_W x TAGPU_FX_ALPT_H words,
   tagpu_fx.h): rows 0..255 the engine's table, row = source index, column = destination
   index; row 256 the palette sorted as 0x4BA920 sorts it, each entry `R+G+B << 8 | index`,
   which the fragment stage's nearest-entry search 0x4BA9D0 walks. Rebuilt when the packet's
   table or the engine palette moves, as the light table is; `s_alpInit` 0 until the first. */
static uint32_t      s_alpT[TAGPU_FX_ALPT_W * TAGPU_FX_ALPT_H];
static unsigned char s_alpSrc[TAGPU_PK_ALP_BYTES];
static unsigned      s_alpPal, s_alpSerial;
static int           s_alpInit;

/* 0x4BA920: each entry's R+G+B and its index, exchange-sorted by the sum with a strict `>` --
   not a stable order, so the loop is the engine's as it stands: entries of equal sum keep the
   order this loop leaves them in, and the search's tie rule (the first strict minimum in this
   order) depends on it (effects.md §7.6: the flame's own colours tie). */
static void alp_build(const unsigned char* alp, const unsigned char* pal)
{
    int sum[256], idx[256], i, j;
    for (i = 0; i < 256 * 256; i++) s_alpT[i] = alp[i];
    for (i = 0; i < 256; i++) { sum[i] = pal[4 * i] + pal[4 * i + 1] + pal[4 * i + 2]; idx[i] = i; }
    for (i = 0; i < 256; i++)
        for (j = i + 1; j < 256; j++)
            if (sum[i] > sum[j]) {
                int t = sum[i]; sum[i] = sum[j]; sum[j] = t;
                t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }
    for (i = 0; i < 256; i++) s_alpT[256 * 256 + i] = ((uint32_t)sum[i] << 8) | (uint32_t)idx[i];
}

/* THE SHADER PAIR IS A BUILD INPUT, NOT DEAD CODE, and no C in this file
   references it -- `tools/spirv-gen.py` reads both strings
   out of the PREPROCESSED translation unit and generates the SPIR-V that
   `tagpu_vk_fx.c` draws with, so deleting them fails the build with "the
   manifest names tagpu_fx::VS and the source does not have it". The pragma
   below is paired; prove its `pop` with a planted probe rather than by
   reading -- one inside a comment is text and not a directive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec2 aCM;\n"       /* colour-or-ck /255, mode      */
    "layout(location=3) in vec2 aWorld;\n"
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"                  /* same view transform as units */
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"            /* same depth encoding as units */
    "out vec2 vUV; flat out vec2 vCM; out vec2 vWorld; out float vEnc;\n"
    "void main(){\n"
    "  vec2 p = (aPos.xy - uZoomC) * uZoom + uZoomC;\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - aPos.z/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vUV = aUV; vCM = aCM; vWorld = aWorld; vEnc = aPos.z;\n"
    "}\n";
/* THE ENGINE'S ALP RULE, FOR THE ORDERED EDITIONS FSO, NFSO AND LFSO (tagpu_vk_fx.c, *The ALP
   blend*). `0x4B8500` writes `T[src << 8 | dst]`, a palette entry chosen by the source's and the
   destination's indices (effects.md §3, §7.6). The ordered editions draw in the world's ordered
   pass, which has no colour attachment: each fragment reads and writes the world target's pixel
   itself (`uDst`, a storage image) inside the pixel interlock, which runs the critical sections
   of every fragment on one pixel in primitive order -- so a stack of sprites applies the table
   one after another, as the engine's blits do -- and the depth test is early, so a fragment the
   depth hides never reaches its write. The source's index is the index atlas's texel (`uIdx`,
   the base atlas's texels unexpanded); the destination's index is the palette entry the
   engine's own search 0x4BA9D0 picks for the pixel -- the entries whose R+G+B lies within 40 of
   the pixel's, walked in 0x4BA920's sorted order (row 256 of `uAlpT`), the first strict minimum
   of the squared RGB distance, else the entry where the walk stopped. The result is the pixel
   moved by what the table does to its index, `dst + pal[T[src][i]] - pal[i]`: the engine's
   pixel exactly where the destination is a palette colour, and the destination's own detail
   kept where it is not (Classic++). The target is premultiplied; an ALP pixel is opaque, as the
   engine's is. `taStore` is every other mode's blend as the pipelines' fixed function does it
   (`ONE, ONE_MINUS_SRC_ALPHA`; a flash, alpha 0, adds), so the edition draws every bucket. */
#define TAGPU_FX_ALP_EXT \
    "#extension GL_ARB_fragment_shader_interlock : require\n" \
    "#extension GL_EXT_shader_image_load_formatted : require\n" \
    "layout(pixel_interlock_ordered) in;\n" \
    "layout(early_fragment_tests) in;\n"
#define TAGPU_FX_ALP_DECL \
    "uniform usampler2D uIdx;\n" \
    "uniform usampler2D uAlpT;\n" \
    "uniform image2D uDst;\n"
#define TAGPU_FX_ALP_FN \
    "vec3 taAlp(vec4 d, uint s){\n" \
    "  ivec3 c = ivec3(round(clamp(d.rgb, 0.0, 1.0) * 255.0));\n" \
    "  int want = c.r + c.g + c.b;\n" \
    "  int lo = 0, hi = 256;\n" \
    "  while (lo < hi) {\n" \
    "    int m = (lo + hi) >> 1;\n" \
    "    if (int(texelFetch(uAlpT, ivec2(m, 256), 0).r >> 8) < want - 40) lo = m + 1; else hi = m;\n" \
    "  }\n" \
    "  int best = 1000000000, hit = -1, k = lo;\n" \
    "  for (; k < 256; k++) {\n" \
    "    uint e = texelFetch(uAlpT, ivec2(k, 256), 0).r;\n" \
    "    if (int(e >> 8) > want + 40) break;\n" \
    "    ivec3 q = ivec3(round(texelFetch(uPal, ivec2(int(e & 255u), 0), 0).rgb * 255.0)) - c;\n" \
    "    int dd = q.x * q.x + q.y * q.y + q.z * q.z;\n" \
    "    if (dd < best) { best = dd; hit = k; }\n" \
    "  }\n" \
    "  int di = int(texelFetch(uAlpT, ivec2(hit >= 0 ? hit : (k & 255), 256), 0).r & 255u);\n" \
    "  int o = int(texelFetch(uAlpT, ivec2(di, int(s)), 0).r);\n" \
    "  vec3 po = texelFetch(uPal, ivec2(o, 0), 0).rgb;\n" \
    "  vec3 pd = texelFetch(uPal, ivec2(di, 0), 0).rgb;\n" \
    "  return clamp(d.rgb + (po - pd), 0.0, 1.0);\n" \
    "}\n"
/* the critical section: `alp` 1 for an ALP texel, else `src` premultiplied over the pixel
   (`add` 1: a flash's light added). At the top level of main, after every discard. */
#define TAGPU_FX_ALP_STORE(alp, add, src) \
    "  ivec2 px = ivec2(gl_FragCoord.xy);\n" \
    "  beginInvocationInterlockARB();\n" \
    "  vec4 d = imageLoad(uDst, px);\n" \
    "  vec4 o = " src ";\n" \
    "  if (" alp ") o = vec4(taAlp(d, sIdx), 1.0);\n" \
    "  else if (" add ") o = vec4(d.rgb + o.rgb, d.a);\n" \
    "  else o = o + d * (1.0 - o.a);\n" \
    "  imageStore(uDst, px, clamp(o, 0.0, 1.0));\n" \
    "  endInvocationInterlockARB();\n"

/* THE SPRITE PROGRAM'S FRAGMENT STAGE, IN TWO EDITIONS: FS blends an ALP sprite (mode 2) at
   50 % in RGB, FSO computes the engine's ALP rule against the pixel underneath (TAGPU_FX_ALP_FN).
   Every other line is one text, the macros below. */
#define FX_FS_DECL \
    "in vec2 vUV; flat in vec2 vCM; in vec2 vWorld; in float vEnc;\n" \
    "out vec4 frag;\n" \
    "uniform sampler2D uPal;\n"              /* the engine's table: mode 0's colour */ \
    "uniform sampler2D uLht;\n"              /* 32x1 RGB additive per level  */ \
    "uniform sampler2D uAtlasRGB;\n"         /* Classic++: the atlas's restored twin */ \
    "uniform int uRestored;\n"               /* 1 = sample it where its alpha says so */ \
    TAGPU_GLSL_FOG_UNIFORMS \
    TAGPU_GLSL_SCAF_UNIFORMS \
    /* the base atlas: the same texels expanded through the engine's table, \
       alpha 0 at the frame's key and 255 - the flash level elsewhere \
       (tagpu_fx.h TAGPU_FX_FLASH_ALPHA, tagpu_vk_fx.c) */ \
    "uniform sampler2D uBase;\n" \
    TAGPU_GLSL_FOG_FN
#define FX_FS_MAIN \
    "void main(){\n" \
    "  int mode = int(vCM.y + 0.5);\n" \
    "  vec3 rgb; float a = 1.0;\n" \
    /* scaffold occlusion, the unit shader's rule; no draw of the Vulkan pass turns uScafOn on \
       (tagpu_vk_fx.c) */ \
    TAGPU_GLSL_SCAF_TEST \
    /* effects are transient: the engine's own passes are LOS-gated, so they \
       vanish in grey rather than darkening (uFog bit1 is set for this pass) */ \
    TAGPU_GLSL_FOG_DISCARD \
    /* mode 0 is a flat colour, and the vertex carries its palette index */ \
    "  if (mode == 0) {\n" \
    "    rgb = texelFetch(uPal, ivec2(int(vCM.x*255.0+0.5), 0), 0).rgb;\n" \
    "  } else {\n" \
    /* colour-keyed: the base's alpha is 0 exactly where the frame's index is \
       its key (NEAREST, the texel the index was) */ \
    "    vec4 b = texture(uBase, vUV);\n" \
    "    if (b.a < 0.5) discard;\n" \
    "    if (mode == 3) {\n" \
    "      int lv = 255 - int(b.a * 255.0 + 0.5);\n" \
    "      rgb = texelFetch(uLht, ivec2(lv, 0), 0).rgb;\n" \
    "    } else {\n" \
    /* the twin's colour where the lazy restore has painted it (alpha 1 -- \
       tagpu_gaf.h), the base atlas's otherwise; the flash mode above keeps \
       its level-driven light table. Effects hide in grey, so no RGB fog rule \
       is needed here */ \
    "      vec4 t = uRestored == 1 ? texture(uAtlasRGB, vUV) : vec4(0.0);\n" \
    "      rgb = t.a > 0.5 ? t.rgb : b.rgb;\n"
#define FX_FS_CLOSE \
    "    }\n" \
    "  }\n"
#define FX_FS_TAIL \
    FX_FS_CLOSE \
    /* premultiplied target: flashes are pure additive light (alpha 0) */ \
    "  if (mode == 3) frag = vec4(rgb, 0.0); else frag = vec4(rgb * a, a);\n" \
    "}\n"
static const char* FS =
    "#version 330 core\n"
    FX_FS_DECL
    FX_FS_MAIN
    "      if (mode == 2) a = 0.5;\n"
    FX_FS_TAIL;
static const char* FSO =
    "#version 330 core\n"
    TAGPU_FX_ALP_EXT
    FX_FS_DECL
    TAGPU_FX_ALP_DECL
    TAGPU_FX_ALP_FN
    FX_FS_MAIN
    FX_FS_CLOSE
    "  uint sIdx = texture(uIdx, vUV).r;\n"
    TAGPU_FX_ALP_STORE("mode == 2", "mode == 3", "vec4(rgb * a, a)")
    "}\n";
/* THE LINES' PROGRAM: lasers and lightning by tagpu_line.h's one rule --
   `0x4CC7AB`'s walk on the line grid, thickened to ss pixels; tagpu_mark.c's
   LVS and LFS say how, and tagpu_glsl.h holds the band and the walk. One
   instance a line record (`emit_line`): `aPos.xy` and `aUV` are its two ends,
   their line-grid pixels' centres in game units after the zoom, and `aPos.z`
   its depth key. LVS declares VS's uniforms, and LFS FS's samplers and
   uniforms, in their order, so the two programs share this pass's one layout;
   `uGrid` (tagpu_line.h `tagpu_line_grid`) follows FS's block at offset 64.
   LFS is FS's mode-0 path -- the fog and the flat palette colour -- behind
   the line test; FS's scaffold test is left out because `uScafOn` is 0 on
   every draw of this pass (tagpu_vk_fx.c, WHAT IT DOES NOT DO). */
static const char* LVS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec2 aCM;\n"
    "layout(location=3) in vec2 aWorld;\n"
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"
    "out vec2 vWorld; flat out vec2 vCM; flat out vec4 vLine;\n"
    TAGPU_GLSL_BAND_FN
    "void main(){\n"
    "  float t;\n"
    "  vec2 p = taBand(aPos.xy, aUV, gl_VertexID, t);\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - aPos.z/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vWorld = aWorld; vCM = aCM; vLine = vec4(aPos.xy, aUV);\n"
    "}\n";
static const char* LFS =
    "#version 330 core\n"
    "in vec2 vWorld; flat in vec2 vCM; flat in vec4 vLine;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uPal;\n"
    "uniform sampler2D uLht;\n"
    "uniform sampler2D uAtlasRGB;\n"
    "uniform int uRestored;\n"
    TAGPU_GLSL_FOG_UNIFORMS
    TAGPU_GLSL_SCAF_UNIFORMS
    "uniform sampler2D uBase;\n"
    "uniform ivec4 uGrid;\n"
    TAGPU_GLSL_FOG_FN
    TAGPU_GLSL_LINE_FN
    "void main(){\n"
    "  if (!taLineKeeps(gl_FragCoord.xy, vLine, uGrid))\n"
    "    discard;\n"
    TAGPU_GLSL_FOG_DISCARD
    "  frag = vec4(texelFetch(uPal, ivec2(int(vCM.x*255.0+0.5), 0), 0).rgb, 1.0);\n"
    "}\n";
/* LFS's ordered edition: the same test, the colour stored opaque inside the interlock, for the
   ordered pass that has no colour attachment (TAGPU_FX_ALP_FN) */
static const char* LFSO =
    "#version 330 core\n"
    TAGPU_FX_ALP_EXT
    "in vec2 vWorld; flat in vec2 vCM; flat in vec4 vLine;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uPal;\n"
    "uniform sampler2D uLht;\n"
    "uniform sampler2D uAtlasRGB;\n"
    "uniform int uRestored;\n"
    TAGPU_GLSL_FOG_UNIFORMS
    TAGPU_GLSL_SCAF_UNIFORMS
    "uniform sampler2D uBase;\n"
    "uniform ivec4 uGrid;\n"
    TAGPU_FX_ALP_DECL
    TAGPU_GLSL_FOG_FN
    TAGPU_GLSL_LINE_FN
    TAGPU_FX_ALP_FN
    "void main(){\n"
    "  if (!taLineKeeps(gl_FragCoord.xy, vLine, uGrid))\n"
    "    discard;\n"
    TAGPU_GLSL_FOG_DISCARD
    "  uint sIdx = 0u;\n"
    TAGPU_FX_ALP_STORE("false", "false",
        "vec4(texelFetch(uPal, ivec2(int(vCM.x*255.0+0.5), 0), 0).rgb, 1.0)")
    "}\n";
/* THE PARTICLES FROM THEIR EMITTER EVENTS (inc/tagpu_pevt.h; gpu-particles-plan.md, *The draw*).
   No vertex attribute: the storage block `pe` holds the sprites' frame table (TAGPU_PEVT_NSEQ *
   TAGPU_PEVT_NFRAME rows of 8 words: ox, oy, w, h, u0, v0, u1, v1), the wind's rows (at word 1536:
   base, n, two words free, then TAGPU_PEVT_WIND rows of x, rise, y, 0) and then the events as 20
   uints each (at word 5636); vertex v is corner v % 6 of particle (v / 6) % 10 of event v / 60
   (a smoke event's puff pbase + that), and the shader computes the particle at GameTime `uTick`
   -- tagpu_pevt.h's tagpu_pevt_nano_at, tagpu_pevt_sprite_at, tagpu_pevt_wake_at and the puff's
   tagpu_pevt_puff_u / _frame / _after, statement for statement, in 32-bit integer arithmetic,
   which wraps in GLSL as it does in C (a puff's frame walk is bounded by the frame table, 64): the keyed
   draws (tagpu_prand.h), a nano particle's start and end out of the cut boxes (64-bit product,
   toward zero: umulExtended) and its step (toward zero), the updates a particle has had, its
   position and its colour or frame. A particle not in the engine's object then is a degenerate
   quad; a wake dot is in it from its birth to its death walk, which the event carries. A nano or
   wake dot is DrawBar's 2 x 2 game pixels at the engine's projection (sx =
   hi(x) - eyeX + vpL, sy = hi(y) - hi(alt) / 2 - eyeY + vpT); a sprite is its frame's quad at
   that point less the hotspot, the quad emit_sprite makes, with the frame's atlas rect; both at
   the layer's key. EDIT tagpu_pevt.h WITH IT. NVS declares VS's uniforms first, in their order,
   so the two share the pass's vertex block; NFS declares FS's samplers and uniforms in theirs. */
typedef char nvs_frame_rows[(TAGPU_PEVT_NFRAME == 64u) ? 1 : -1];   /* NVS's 64 and 63 */
typedef char nvs_wind_at[(TAGPU_PEVT_WIND_AT == 1536u) ? 1 : -1];   /* NVS's 1536, 1540 */
typedef char nvs_evt_at[(TAGPU_PEVT_EVT_AT == 5636u) ? 1 : -1];     /* NVS's 5636      */
static const char* NVS =
    "#version 330 core\n"
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"
    "uniform ivec4 uEye;\n"                  /* eyeX, eyeY, vpL, vpT          */
    "uniform int uTick;\n"                   /* the packet's GameTime         */
    "uniform vec4 uEncA;\n"                  /* layer keys 0..3, 4..7, 8..9   */
    "uniform vec4 uEncB;\n"
    "uniform vec2 uEncC;\n"
    "layout(std430) readonly buffer PEvt { uint pe[]; };\n"
    "flat out vec2 vCM; flat out ivec2 vTile; out vec2 vUV;\n"
    "uint taMix(uint x){\n"
    "  x ^= x >> 16u; x *= 0x7FEB352Du; x ^= x >> 15u; x *= 0x846CA68Bu; x ^= x >> 16u;\n"
    "  return x;\n"
    "}\n"
    "int taRand(uint seed, uint p, uint d){ return int(taMix(taMix(seed ^ p) ^ d) >> 17u); }\n"
    "int taCoord(int base, int ext, int r){\n"
    "  uint m = ext < 0 ? 0u - uint(ext) : uint(ext);\n"
    "  uint hi, lo;\n"
    "  umulExtended(uint(r), m, hi, lo);\n"
    "  uint q = (hi << 17u) | (lo >> 15u);\n"
    "  return int(uint(base) + (ext < 0 ? 0u - q : q));\n"
    "}\n"
    "int taDivZ(int a, int b){\n"
    "  uint ua = a < 0 ? 0u - uint(a) : uint(a);\n"
    "  uint ub = b < 0 ? 0u - uint(b) : uint(b);\n"
    "  uint q = ua / ub;\n"
    "  return int((a < 0) != (b < 0) ? 0u - q : q);\n"
    "}\n"
    "void main(){\n"
    "  uint vid = uint(gl_VertexID);\n"
    "  uint k = (vid / 6u) % 10u, c = vid % 6u, b = 5636u + (vid / 60u) * 20u;\n"
    "  uint seed = pe[b], t0 = pe[b + 1u], fl = pe[b + 2u], cls = (fl >> 8u) & 15u;\n"
    "  uint P = uint(uTick);\n"
    "  vec2 cxy = vec2((c == 1u || c == 3u || c == 4u) ? 1.0 : 0.0,\n"
    "                  (c == 2u || c == 4u || c == 5u) ? 1.0 : 0.0);\n"
    "  vec2 off = vec2(0.0), sz = vec2(2.0);\n"
    "  bool on;\n"
    "  ivec3 pos;\n"
    "  vUV = vec2(0.0);\n"
    "  if (cls == 0u) {\n"
    "    uint sw = pe[b + 15u + k / 2u];\n"
    "    int s = (k & 1u) == 0u ? int(sw << 16u) >> 16 : int(sw) >> 16;\n"
    "    uint born = k < 5u ? t0 : t0 + 1u;\n"
    "    uint first = k < 5u ? ((fl & 16u) != 0u ? t0 + 1u : t0) : t0 + 2u;\n"
    "    on = s != 0 && (k < 5u || (fl & 32u) != 0u) && int(P - born) >= 0 &&\n"
    "         !(int(P - first) >= 0 && int(P - (born + uint(s))) > 0);\n"
    "    int u = max(int(P - first) + 1, 0);\n"
    "    for (int a = 0; a < 3; a++) {\n"
    "      int st = taCoord(int(pe[b + 3u + uint(a)]), int(pe[b + 6u + uint(a)]), taRand(seed, k, uint(a)));\n"
    "      int en = taCoord(int(pe[b + 9u + uint(a)]), int(pe[b + 12u + uint(a)]), taRand(seed, k, uint(a) + 3u));\n"
    "      int stp = taDivZ(int(uint(en) - uint(st)), s != 0 ? s : 1);\n"
    "      pos[a] = int(uint(st) + uint(u) * uint(stp));\n"
    "    }\n"
    "    uint col = 0xA0u | ((k % 5u + uint(u)) % 7u + 1u);\n"
    "    vCM = vec2(float(col) / 255.0, 0.0);\n"
    "  } else if (cls == 3u) {\n"
    "    int per = int(pe[b + 9u]), nsub = int(pe[b + 10u]);\n"
    "    uint gone = k < 2u ? pe[b + 14u + k] : 0u;\n"
    "    uint born = t0 + k;\n"
    "    uint first = k != 0u ? t0 + 2u : ((fl & 16u) != 0u ? t0 + 1u : t0);\n"
    "    on = k < 2u && int(k) < nsub && per >= 1 && int(P - born) >= 0 && int(P - gone) < 0;\n"
    "    int u = max(int(P - first) + 1, 0);\n"
    "    pos = ivec3(0);\n"
    "    if (on)\n"                         /* a third..tenth particle: no dot, no draws */
    "      for (int a = 0; a < 3; a++) {\n"
    "        int j = ((taRand(seed, k, uint(a)) * 7) >> 15) - 3;\n"
    "        pos[a] = int(pe[b + 3u + uint(a)] + (uint(j) << 16u) + uint(u) * pe[b + 6u + uint(a)]);\n"
    "      }\n"
    "    int cc = (u / max(per, 1)) % 7;\n"
    "    int col = pe[b + 13u] != 0u ? 0x61 + cc : 0x67 - cc;\n"
    "    vCM = vec2(float(col) / 255.0, 0.0);\n"
    "  } else if (cls == 4u || cls == 5u) {\n"
    "    int ival = int(pe[b + 6u]), npuff = int(pe[b + 9u]), rst = int(pe[b + 10u]);\n"
    "    int ncap = int(pe[b + 11u]), sq = int(pe[b + 12u]);\n"
    "    uint again = pe[b + 8u], j = pe[b + 14u] + k;\n"
    "    int dd = int(P - again);\n"
    "    uint np = 1u;\n"
    "    if (npuff > 1 && dd >= 0 && ival >= 1) np += uint(dd / ival) + 1u;\n"
    "    np = min(np, uint(max(npuff, 1)));\n"
    "    uint F = j == 0u ? pe[b + 7u] : again + (j - 1u) * uint(ival) + 1u;\n"
    "    int u = max(int(P - F) + 1, 0);\n"
    "    int nf = taCoord(2, ncap - 2, taRand(seed, j, 0u));\n"
    "    int h = rst / 2, f = 0, st = 0, len = rst;\n"
    "    bool live = ival >= 1 && rst >= 2 && j < np && !(u > 0 && nf <= 0);\n"
    "    for (int i = 0; i < 64 && live && u >= st + len; i++) {\n"
    "      f++;\n"
    "      if (f >= nf) live = false;\n"
    "      else { st += len; len = taCoord(h, h, taRand(seed, j, uint(f))); }\n"
    "    }\n"
    "    live = live && u < st + len;\n"
    "    ivec3 d = ivec3(0);\n"
    "    if (live && u > 0) {\n"            /* the wind's sums: row F + u - 1 less row F - 1 */
    "      uint wb = pe[1536u], wn = pe[1537u];\n"
    "      uint i0 = F - 1u - wb, i1 = F + uint(u) - 1u - wb;\n"
    "      if (i0 < wn && i1 < wn) {\n"
    "        uint a0 = 1540u + i0 * 4u, a1 = 1540u + i1 * 4u;\n"
    "        d = ivec3(int(pe[a1] - pe[a0]), int(pe[a1 + 1u] - pe[a0 + 1u]),\n"
    "                  int(pe[a1 + 2u] - pe[a0 + 2u]));\n"
    "      } else live = false;\n"
    "    }\n"
    "    uint rs = cls == 5u ? 4u : 2u, am = (1u << rs) - 1u;\n"
    "    pos.x = int(((pe[b + 3u] & ~7u) | (j & 7u)) + (uint(d.x) << 3u));\n"
    "    pos.y = int(((pe[b + 4u] & ~am) | ((j >> 6u) & am)) + (uint(d.y) << rs));\n"
    "    pos.z = int(((pe[b + 5u] & ~7u) | ((j >> 3u) & 7u)) + (uint(d.z) << 3u));\n"
    "    uint fb = (uint(1 + clamp(sq, 0, 1)) * 64u + uint(clamp(f, 0, 63))) * 8u;\n"
    "    int fw = int(pe[fb + 2u]), fh = int(pe[fb + 3u]);\n"
    "    on = live && fw > 0 && fh > 0;\n"
    "    off = vec2(float(int(pe[fb])), float(int(pe[fb + 1u])));\n"
    "    sz = vec2(float(fw), float(fh));\n"
    "    vUV = mix(vec2(uintBitsToFloat(pe[fb + 4u]), uintBitsToFloat(pe[fb + 5u])),\n"
    "              vec2(uintBitsToFloat(pe[fb + 6u]), uintBitsToFloat(pe[fb + 7u])), cxy);\n"
    "    vCM = vec2(0.0, cls == 5u ? 3.0 : 2.0);\n"   /* Smoke1's leaf 0x475700 tests no sight */
    "  } else {\n"
    "    bool fire = cls == 1u;\n"
    "    int per = int(pe[b + 9u]), nsub = int(pe[b + 10u]), m = int(pe[b + 11u]);\n"
    "    uint len = pe[b + 12u];\n"
    "    uint born = t0 + k * (fire ? 1u : 10u);\n"
    "    uint endT = fire ? t0 + len : born + len;\n"
    "    uint first = k != 0u ? born + 1u : ((fl & 16u) != 0u ? t0 + 1u : t0);\n"
    "    int u = max(int(P - first) + 1, 0);\n"
    "    for (int a = 0; a < 3; a++)\n"
    "      pos[a] = int(pe[b + 3u + uint(a)] + uint(u) * pe[b + 6u + uint(a)]);\n"
    "    int mm = max(m, 1);\n"
    "    int f = fire ? (u / max(per, 1)) % mm\n"
    "                 : (int((uint(taRand(seed, k, 0u)) * uint(mm)) / 32768u) + u) % mm;\n"
    "    uint fb = uint(clamp(f, 0, 63)) * 8u;\n"
    "    int fw = int(pe[fb + 2u]), fh = int(pe[fb + 3u]);\n"
    "    on = int(k) < nsub && int(P - born) >= 0 && int(P - endT) <= 0 && m >= 1 &&\n"
    "         (!fire || per >= 1) && f >= 0 && f < 64 && fw > 0 && fh > 0;\n"
    "    off = vec2(float(int(pe[fb])), float(int(pe[fb + 1u])));\n"
    "    sz = vec2(float(fw), float(fh));\n"
    "    vUV = mix(vec2(uintBitsToFloat(pe[fb + 4u]), uintBitsToFloat(pe[fb + 5u])),\n"
    "              vec2(uintBitsToFloat(pe[fb + 6u]), uintBitsToFloat(pe[fb + 7u])), cxy);\n"
    "    vCM = vec2(0.0, 2.0);\n"
    "  }\n"
    "  int hx = pos.x >> 16, ha = pos.y >> 16, hy = pos.z >> 16;\n"
    "  int zp = hy - (ha >> 1);\n"
    "  int L = int(fl & 15u);\n"
    "  float enc = L < 4 ? uEncA[L] : L < 8 ? uEncB[L - 4] : uEncC[L - 8];\n"
    "  vec2 q = vec2(float(hx - uEye.x + uEye.z), float(zp - uEye.y + uEye.w)) + off + sz * cxy;\n"
    "  vec2 p = (q - uZoomC) * uZoom + uZoomC;\n"
    "  gl_Position = on ? vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                          clamp(1.0 - enc/uDepthScale, 0.0, 1.0), 1.0)\n"
    "                   : vec4(-2.0, -2.0, 0.0, 1.0);\n"
    "  vTile = ivec2(hx >> 5, zp >> 5);\n"
    "}\n";
/* THE ENGINE'S OWN SIGHT TEST, at the particle's exact tile (0x473A00, 0x474170, 0x473590:
   `(hi(x) >> 5, (hi(y) - hi(alt) / 2) >> 5)`, off the map hidden; in true-LOS mode the viewed
   player's LOS counter, else its mapped bit), against the slot's fog source, whose two bits are
   exactly those (inc/tagpu_fogsrc.h: bit 1 out of LOS, set only in true-LOS mode; bit 0 never
   explored). The smoothed fog the other effects take is not applied: a particle is all there or
   not at all, as the engine draws it. A dot is its palette colour; a sprite (mode 2) is FS's
   alpha path: the base atlas's key test, then NFS's 50 % mix in RGB, or NFSO's engine table
   against the pixel underneath (TAGPU_FX_ALP_FN). A Smoke1 puff (mode 3) is a sprite with no
   sight test, as its leaf 0x475700 has none. */
#define FX_NFS_DECL \
    "flat in vec2 vCM; flat in ivec2 vTile; in vec2 vUV;\n" \
    "out vec4 frag;\n" \
    "uniform sampler2D uPal;\n" \
    "uniform sampler2D uLht;\n" \
    "uniform sampler2D uAtlasRGB;\n" \
    "uniform int uRestored;\n" \
    TAGPU_GLSL_FOG_UNIFORMS \
    TAGPU_GLSL_SCAF_UNIFORMS \
    "uniform sampler2D uBase;\n" \
    TAGPU_GLSL_FOG_FN
#define FX_NFS_FOG \
    "void main(){\n" \
    "  int mode = int(vCM.y + 0.5);\n" \
    "  if ((uFog & 1) == 1 && mode != 3) {\n" \
    "    if (vTile.x < 0 || vTile.y < 0 || vTile.x >= uFogSize.x || vTile.y >= uFogSize.y)\n" \
    "      discard;\n" \
    "    int c = taFogCell(vTile.x, vTile.y);\n" \
    "    if ((uFog & 4) == 4 ? (c & 2) != 0 : (c & 1) != 0) discard;\n" \
    "  }\n"
#define FX_NFS_MAIN \
    FX_NFS_FOG \
    "  if (mode >= 2) {\n" \
    "    vec4 b = texture(uBase, vUV);\n" \
    "    if (b.a < 0.5) discard;\n" \
    "    vec4 t = uRestored == 1 ? texture(uAtlasRGB, vUV) : vec4(0.0);\n" \
    "    vec3 rgb = t.a > 0.5 ? t.rgb : b.rgb;\n"
#define FX_NFS_TAIL \
    "  } else {\n" \
    "    frag = vec4(texelFetch(uPal, ivec2(int(vCM.x*255.0+0.5), 0), 0).rgb, 1.0);\n" \
    "  }\n" \
    "}\n"
/* NFSO's main after the fog test: the key test for a sprite, then the store; a dot is its
   palette colour, opaque */
#define FX_NFSO_TAIL \
    "  if (mode >= 2 && texture(uBase, vUV).a < 0.5) discard;\n" \
    "  uint sIdx = texture(uIdx, vUV).r;\n" \
    TAGPU_FX_ALP_STORE("mode >= 2", "false", \
        "vec4(texelFetch(uPal, ivec2(int(vCM.x*255.0+0.5), 0), 0).rgb, 1.0)") \
    "}\n"
static const char* NFS =
    "#version 330 core\n"
    FX_NFS_DECL
    FX_NFS_MAIN
    "    frag = vec4(rgb * 0.5, 0.5);\n"
    FX_NFS_TAIL;
static const char* NFSO =
    "#version 330 core\n"
    TAGPU_FX_ALP_EXT
    FX_NFS_DECL
    TAGPU_FX_ALP_DECL
    TAGPU_FX_ALP_FN
    FX_NFS_FOG
    FX_NFSO_TAIL;
#pragma GCC diagnostic pop

/* THE ATLAS IS THE PASS, NOT THE BACKEND -- the feature pass's shape and the
   same reason (tagpu_feat.c). `atlas_create` makes no texture: it lays out the
   shelf and keys the atlas's existence on `made` (tagpu_gaf.h). A path that
   skipped it would leave `dim` at 0, and every sprite lookup would return
   NULL. */
static void atlas_setup(void)
{
    tagpu_gaf_atlas_lost(&s_atlas);      /* the struct describes nothing yet */
    s_atlas.dim = ATLAS_DIM; s_atlas.max = ATLAS_MAX;
    s_atlas.ents = s_atlasEnts; s_atlas.tag = "fx";
    /* a world atlas: its base atlas is RGBA, so the key has to travel as a
       plane beside the indices (tagpu_gaf.h `keyPlane`) */
    s_atlas.keyPlane = 1;
    /* its paints are bounded by what one frame's upload holds (tagpu_gaf.h
       `budget`): tagpu_vk_fx.c sends them in the frame's own command buffer */
    s_atlas.budget = TAGPU_GAF_BUDGET;
    tagpu_gaf_atlas_create(&s_atlas);   /* laid out now, not on first use */
}

/* ---- emission ---- */
static void put_vert(int b, float x, float y, float u, float v, float c, int mode, float wx, float wz)
{
    float* o = s_verts[b] + (size_t)s_nv[b] * FXST;
    o[0] = x; o[1] = y; o[2] = s_encCur; o[3] = u; o[4] = v;
    o[5] = c; o[6] = (float)mode; o[7] = wx; o[8] = wz;
    s_nv[b]++;
}

static int s_cLines = 0, s_cSprites = 0, s_cFlash = 0, s_cAtlasFail = 0;
static int s_cOverflow = 0, s_cQuads = 0, s_cLineFar = 0;
/* this frame's wheel zoom, latched by the gather: a line's ends are
   quantised through it where the line is emitted (tagpu_line.h) */
static double s_lzoom = 1.0, s_lzcx, s_lzcy;
/* and DrawLine's clip rect -- the viewport, inclusive -- and the surface, in
   line-grid pixels (tagpu_line_clip) */
static int s_lclipL, s_lclipT, s_lclipR, s_lclipB, s_lclipW, s_lclipH;
static int s_lss = 1;                   /* the line grid's ss, latched with the zoom */
static TAGPU_LINE_AB s_lab;             /* an A/B frame's lines before the clip */

/* A PART ASKED FOR AND NOT WRITTEN. Every emitter return that leaves a
   sprite's quad or a line out of the buckets for want of this frame's room
   counts here: the atlas deferring a paint past the allowance, the atlas
   full, a frame whose pixels would not decode (all three are
   `tagpu_gaf_atlas_get` answering NULL), a bucket full. A sprite and an effect
   snapshot it and take themselves back out when it moved (`emit_sprite`,
   `effect_begin`), so the rollback is decided by the one count every such
   path feeds, not by the paths' own tallies.
   NOT COUNTED, because no part the frame asks for is lost: the `nosprites`
   and `nolines` levers and the mute, which ask for none, and a frame the GAF
   reader refuses (`tagpu_gaf_frame_geom`: zero-sized, or larger than the
   decoder takes -- the publisher resolved every frame through the same test)
   or one nested past four levels. That is the frame's own shape and the same
   on every frame, so it cannot draw an effect half on one frame and whole on
   the next. */
static unsigned s_partsLost;

static void put_quad(int b, float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1, float c, int mode,
                     float wx, float wz)
{
    if (s_nv[b] + 6 > TAGPU_FX_MAXV_OF(b)) { s_cOverflow++; s_partsLost++; return; }
    put_vert(b, x0, y0, u0, v0, c, mode, wx, wz);
    put_vert(b, x1, y0, u1, v0, c, mode, wx, wz);
    put_vert(b, x0, y1, u0, v1, c, mode, wx, wz);
    put_vert(b, x1, y0, u1, v0, c, mode, wx, wz);
    put_vert(b, x1, y1, u1, v1, c, mode, wx, wz);
    put_vert(b, x0, y1, u0, v1, c, mode, wx, wz);
    s_cQuads++;
}

/* ONE LINE RECORD, the instance LVS draws: the two ends after the wheel zoom
   in (x, y) and (u, v) -- their line-grid pixels' centres -- the depth key,
   the colour on the flat path and the fog point. The engine draws these lines
   from integer pixels through DrawLine `0x4BE950`, so each end is that pixel's
   CENTRE put through tagpu_line.h's rule -- at 1x and ss = 1 the engine's own
   pixel -- and then through DrawLine's clip to the viewport on the line grid
   (tagpu_line_clip). A line past TAGPU_LINE_FAR is counted (`s_cLineFar`) and
   not drawn; one the clip leaves nothing of is not drawn either. */
static void emit_line(int x0, int y0, int x1, int y1, int colidx, float wx, float wz)
{
    int ax, ay, bx, by;
    if (!s_lines) return;
    s_cLines++;
    if (s_mute) return;
    if (s_nv[B_LINES] + 1 > TAGPU_FX_MAXV) { s_cOverflow++; s_partsLost++; return; }
    if (!tagpu_line_px(x0 + 0.5, y0 + 0.5, s_lzoom, s_lzcx, s_lzcy, s_lss,
                       TAGPU_LINE_FAR, &ax, &ay) ||
        !tagpu_line_px(x1 + 0.5, y1 + 0.5, s_lzoom, s_lzcx, s_lzcy, s_lss,
                       TAGPU_LINE_FAR, &bx, &by)) {
        s_cLineFar++;
        return;
    }
    tagpu_line_ab_add(&s_lab, ax, ay, bx, by, colidx);
    if (!tagpu_line_clip(&ax, &ay, &bx, &by, s_lclipL, s_lclipT, s_lclipR, s_lclipB,
                         s_lclipW, s_lclipH))
        return;
    put_vert(B_LINES, tagpu_line_centre(ax, s_lss), tagpu_line_centre(ay, s_lss),
             tagpu_line_centre(bx, s_lss), tagpu_line_centre(by, s_lss),
             (float)colidx / 255.0f, MODE_FLAT, wx, wz);
}

/* the fx pass's own `nosprites` token lives at ITS call sites (fx_sprite),
   not here: the particle pass emits through this path too */
static void emit_sprite_r(const unsigned char* g, int sx, int sy, int mode, float wx, float wz, int depth)
{
    TAGPU_GAFGEOM gm;
    if (depth > 4) return;
    /* THE FRAME IS AN OPAQUE HANDLE HERE. It came out of the
       packet, resolved by the game thread against the sequence the engine's
       own record named; this file reads not one byte of it. Everything below
       goes through tagpu_gaf.c, which is the module whose class (session
       reader) carries the lifetime argument for GAF storage. */
    if (!tagpu_gaf_frame_geom(g, &gm)) return;
    if (gm.subn) {
        int k;
        for (k = 0; k < gm.subn; k++) {
            TAGPU_GAFGEOM sm;
            const unsigned char* sg = tagpu_gaf_subframe(g, k);
            if (!sg || !tagpu_gaf_frame_geom(sg, &sm)) continue;
            emit_sprite_r(sg, sx, sy,
                          (mode == MODE_OPAQUE && sm.subalp) ? MODE_ALPHA : mode,
                          wx, wz, depth + 1);
        }
        return;
    }
    int b = (mode == MODE_FLASH) ? B_FLASH : B_SPRITES;
    if (mode == MODE_FLASH) s_cFlash++; else s_cSprites++;
    if (s_mute) return;
    const unsigned dN = s_atlas.deferN;
    const TAGPU_GAFENT* e = tagpu_gaf_atlas_get(&s_atlas, g);
    /* a deferral is the allowance's and the atlas counts it (`deferN`), so it
       is not an atlas failure; either way the part is lost */
    if (!e) { if (s_atlas.deferN == dN) s_cAtlasFail++; s_partsLost++; return; }
    int w = gm.w, h = gm.h;
    float x0 = (float)(sx - gm.hotx);
    float y0 = (float)(sy - gm.hoty);
    float x1 = x0 + (float)w, y1 = y0 + (float)h;
    float c = (float)e->ck / 255.0f;
    put_quad(b, x0, y0, x1, y1, e->u0, e->v0, e->u1, e->v1, c, mode, wx, wz);
}

/* A SPRITE IS DRAWN WHOLE OR NOT AT ALL. A compound frame is several atlas
   entries; any one of them lost (`s_partsLost`) takes the sprite's other
   quads back out, so no frame shows part of an explosion. What did paint
   stays painted and draws on a later frame. */
static void emit_sprite(const unsigned char* g, int sx, int sy, int mode, float wx, float wz, int depth)
{
    int nv[NBUCKET], b, quads = s_cQuads;
    const unsigned lost = s_partsLost;
    for (b = 0; b < NBUCKET; b++) nv[b] = s_nv[b];
    emit_sprite_r(g, sx, sy, mode, wx, wz, depth);
    if (s_partsLost != lost) {
        for (b = 0; b < NBUCKET; b++) s_nv[b] = nv[b];
        s_cQuads = quads;
    }
}

/* the fx pass's sprites honour its own nosprites token */
static void fx_sprite(const unsigned char* g, int sx, int sy, int mode, float wx, float wz)
{
    if (s_sprites) emit_sprite(g, sx, sy, mode, wx, wz, 0);
}

/* THE PARTICLE EVENTS OF THIS FRAME, copied out of the packet while the gather holds it: the
   Vulkan pass prepares after tagpu_packet_frame_end, past which no packet pointer may be read,
   so the hand-over carries this copy, as it carries s_verts and the LHT. Sized to the packet's
   own bound on the table (TAGPU_PK_MAX_PEVT, which the acquire has checked n against). `classes`
   says which to draw: bit 0 nano, bit 1 fire and the flare, bit 2 the wake, bit 3 smoke; the rest
   are left out.
   THE SPRITES' FRAMES: frame i of each sprite sequence (the flare sequence, "smoke 1", "smoke 2"),
   as the packet resolved it, put through the atlas here, the same lookup emit_sprite makes for a
   sprite the native pass draws: its size, its hotspot and its atlas rect, for the GPU to index by
   the frame it computes. A sequence no copied event draws is not looked up. A frame not drawn
   this frame -- no handle, a compound frame (said once and left out), not painted yet (the
   atlas's allowance) -- has w 0, and so has every frame while the engine's alpha blit is not built
   (TAProgram+0xF0 bit 5, the gate the native pass takes).
   THE WIND'S ROWS (smoke): copied as the packet carries them, after a header {base, n}; the GPU
   reads row t - base for t in base .. base + n - 1 and draws no puff that needs another. */
static TAGPU_PEVT s_evtBuf[TAGPU_PK_MAX_PEVT];
static const TAGPU_PEVT* s_evt;
static int      s_nEvt;
static unsigned s_evtTick;
static TAGPU_PEVT_FRAME s_evtFrame[TAGPU_PEVT_NSEQ * TAGPU_PEVT_NFRAME];
static uint32_t s_evtWind[4u + 4u * TAGPU_PEVT_WIND];
static int      s_saidCompound;
void tagpu_fx_set_events(const TAGPU_PEVT* e, int n, unsigned tick, const unsigned* frames,
                         const unsigned* nframe, const int32_t* wind, int nwind, unsigned wbase,
                         unsigned classes)
{
    int i, w = 0;
    unsigned seqs = 0, s;
    if (!e || n < 1) n = 0;
    if (n > (int)TAGPU_PK_MAX_PEVT) n = (int)TAGPU_PK_MAX_PEVT;
    for (i = 0; i < n; i++) {
        const unsigned c = TAGPU_PEVT_CLASS(e[i].flags);
        const unsigned bit = c == TAGPU_PEVT_C_NANO ? 1u : c == TAGPU_PEVT_C_WAKE ? 4u :
                             TAGPU_PEVT_SMOKE(c) ? 8u : 2u;
        if (!(classes & bit)) continue;
        s_evtBuf[w++] = e[i];
        if (c == TAGPU_PEVT_C_FIRE || c == TAGPU_PEVT_C_FLARE) seqs |= 1u;
        else if (TAGPU_PEVT_SMOKE(c)) seqs |= e[i].seqsel ? 4u : 2u;
    }
    s_evt = w ? s_evtBuf : NULL;
    s_nEvt = w;
    s_evtTick = tick;
    if (!wind || nwind < 0) nwind = 0;
    if (nwind > (int)TAGPU_PEVT_WIND) nwind = (int)TAGPU_PEVT_WIND;
    s_evtWind[0] = wbase;
    s_evtWind[1] = (uint32_t)nwind;
    if (nwind) memcpy(s_evtWind + 4, wind, (size_t)nwind * 16u);
    memset(s_evtFrame, 0, sizeof s_evtFrame);
    if (!frames || !nframe || !(tagpu_fx_caps() & 0x20)) return;
    for (s = 0; s < TAGPU_PEVT_NSEQ; s++) {
        int nf = (int)nframe[s];
        if (!(seqs & 1u << s)) continue;
        if (nf > (int)TAGPU_PEVT_NFRAME) nf = (int)TAGPU_PEVT_NFRAME;
        for (i = 0; i < nf; i++) {
            const unsigned r = s * TAGPU_PEVT_NFRAME + (unsigned)i;
            const unsigned char* g = (const unsigned char*)(size_t)frames[r];
            const TAGPU_GAFENT* a;
            TAGPU_GAFGEOM gm;
            if (!g || !tagpu_gaf_frame_geom(g, &gm)) continue;
            if (gm.subn) {
                if (!(s_saidCompound & 1 << s)) {
                    s_saidCompound |= 1 << s;
                    flog(s == 0 ? "fx: the flare sequence has a compound frame; the particle "
                                  "events draw nothing for it" :
                         s == 1 ? "fx: \"smoke 1\" has a compound frame; the particle events "
                                  "draw nothing for it" :
                                  "fx: \"smoke 2\" has a compound frame; the particle events "
                                  "draw nothing for it");
                }
                continue;
            }
            if (!(a = tagpu_gaf_atlas_get(&s_atlas, g))) continue;
            s_evtFrame[r].ox = -gm.hotx; s_evtFrame[r].oy = -gm.hoty;
            s_evtFrame[r].w = gm.w;      s_evtFrame[r].h = gm.h;
            s_evtFrame[r].u0 = a->u0; s_evtFrame[r].v0 = a->v0;
            s_evtFrame[r].u1 = a->u1; s_evtFrame[r].v1 = a->v1;
        }
    }
}

/* TAProgram capability bits (+0xF0): bit5 ALP alpha table built, bit7 LHT.
   Read by the publisher; this is the frame's copy, latched at gather. */
static unsigned s_caps;
unsigned tagpu_fx_caps(void) { return s_caps; }

/* ---- THE EFFECTS BAND -------------------------------------------------------
   The engine paints its effects in one order and with no depth at all: every
   projectile record in turn (its shadow blob, then its lines, its model or
   its sprite), then the debris pieces, then every explosion's flash, then
   each explosion's model followed by its sprite (effects.md §1, §2). The
   models are drawn by the posed pass with depth written and the sprites by
   the effects pass after it with depth tested, so that order is carried as
   KEYS: the band above the ground rows (tagpu_native.c) is cut into a
   sequence, and

     model k of the frame               encFx + (2k + 1) * encStep
     a sprite or a line after k models  encFx + 2k * encStep

   so a sprite lands over every model emitted before it and under every model
   after it, which is exactly where the engine's painter puts it. A flash is
   the exception: the engine draws all of them before any explosion's model,
   so every flash takes the key the explosion walk starts at.

   THE KEYS REWIND WITH THE BRACKET. A record taken back out restores `s_nm`,
   and the next key is computed from `s_nm`, so the next record takes the
   keys the lost one would have had and the sequence never has a hole a
   later sprite could fall into. */
static float key_after(int nmodels)
{
    return (float)((double)s_encFx + 2.0 * (double)nmodels * (double)s_encStep);
}

/* ONE MODEL OF A RECORD, RASTERISED WHERE IT IS EMITTED: tagpu_fxmodel.c
   projects the packet's posed model with this pass's eye and runs the
   engine's two rasterisers over it, appending its runs; this records which
   runs are the model's, its key in the band and its fog anchor. The unit pass
   draws the runs (tagpu_native.c hands them to tagpu_posedraw_fx).

   A MODEL IS A PART OF ITS RECORD, and one that cannot be drawn this frame is
   a part lost (`s_partsLost`): the posed pass cannot record it, the band is
   full, or the rasteriser refused it -- a model the packet did not carry, a
   texel the unit atlas's allowance has not painted yet, no room. The bracket
   then takes the whole record back. A model that rasterises to no run at all
   -- off the clip rect, or every face turned away -- draws nothing in the
   engine either, so it is not a loss and takes no key; nor is a model the
   `nomodels` lever does not ask for. */
static void emit_model(const TAGPU_FXVIEW* v, unsigned model, float wx, float wz)
{
    TAGPU_FXMODEL* o;
    unsigned first, n;
    if (!s_models || s_mute || model == TAGPU_PK_NOMODEL) return;
    if (!s_modelsOn || s_nm >= s_modelCap || s_nm >= MAXMODEL) {
        s_cModelLost++;
        s_partsLost++;
        return;
    }
    first = tagpu_fxmodel_mark();
    if (!tagpu_fxmodel_raster(v->packet, model, &s_rv)) {
        s_modelWhy = tagpu_fxmodel_why();
        s_cModelLost++;
        s_partsLost++;
        return;
    }
    n = tagpu_fxmodel_mark() - first;
    if (n == 0) return;
    o = &s_models_[s_nm];
    o->run0 = first; o->nrun = n;
    o->wx = wx; o->wz = wz;
    o->enc = (float)((double)s_encFx + (2.0 * (double)s_nm + 1.0) * (double)s_encStep);
    s_nm++;
    s_encCur = key_after(s_nm);
}

/* ---- AN EFFECT IS DRAWN WHOLE OR NOT AT ALL ---------------------------------
   One record of the packet is one effect: a projectile with its ground
   shadow, its body and its flame, an explosion with its flash, its body and
   its sprite, a debris piece, a laser's lines. Its parts land in different
   places -- a shadow and a sprite in B_SPRITES, a flash in B_FLASH, a line in
   B_LINES, a model in the model list and its runs in tagpu_fxmodel.c's arena
   -- and any one of them can be lost for this frame's want of room
   (`s_partsLost`: an atlas paint deferred past the allowance, an atlas full,
   a decode that failed, a bucket full, a model `emit_model` could not
   rasterise). Taking back that part alone would draw the
   rest without it for a frame: a flash with no explosion, a shadow under no
   shell, a flame with no rocket. So every record is emitted inside a
   bracket, and ANY PART OF IT LOST takes the whole record back out at the
   bracket's end: every bucket, the quad count, the model list, the run
   arena, the sequence key and an A/B frame's line list (`s_lab`) return to
   where they stood at its start. Nothing else is
   emitted inside a bracket, so the rollback takes no other effect's parts
   with it, and what did paint stays painted and draws whole on a later
   frame.

   THE TWO HALVES ARE DRAWN BY TWO PASSES, and a pass can still refuse a whole
   frame after the gather: the posed pass (the models) and the effects pass
   (everything else) then agree per frame through the hand-over's `nmodels`
   (tagpu_fx.h), so neither draws a frame's effects without the other. */
static int      s_effNv[NBUCKET], s_effNm, s_effQuads;
static unsigned s_effLost, s_effRun, s_effLab;

static void effect_begin(void)
{
    int b;
    for (b = 0; b < NBUCKET; b++) s_effNv[b] = s_nv[b];
    s_effNm = s_nm;
    s_effRun = tagpu_fxmodel_mark();
    s_effQuads = s_cQuads;
    s_effLost = s_partsLost;
    s_effLab = s_lab.n;
    s_encCur = key_after(s_nm);
}

static void effect_end(void)
{
    int b;
    if (s_partsLost == s_effLost) return;
    for (b = 0; b < NBUCKET; b++) s_nv[b] = s_effNv[b];
    s_nm = s_effNm;
    /* the list only ever grows inside a bracket, so its count at the start
       is a prefix of what it holds now */
    s_lab.n = s_effLab;
    tagpu_fxmodel_rewind(s_effRun);
    s_cQuads = s_effQuads;
    s_encCur = key_after(s_nm);
}

/* the shaders' fog rule (tagpu_glsl.h) on the CPU — bilinear coverage over the
   entry's four corner bits, thresholded at 0.5 */
static float fog_cov(unsigned m, float fx, float fy)
{
    float tl = (float)( m       & 1u), tr = (float)((m >> 1) & 1u);
    float bl = (float)((m >> 2) & 1u), br = (float)((m >> 3) & 1u);
    float top = tl + (tr - tl) * fx, bot = bl + (br - bl) * fx;
    return top + (bot - top) * fy;
}

/* ---- the fog source's guard -------------------------------------------------

   `f` is the native pass's per-frame copy of the packet's fog source
   (tagpu_native.c `s_fogSrc`), taken by the acquire's bounds and re-checked by
   tagpu_fog_src_ok when it was copied, so its words are exactly `stride * h`
   long. The guard below re-asks the same questions of the values, because a
   bound in one file is a bound only while both are read together, and the
   pointer's range test is a sanity filter on a value, never the safety
   argument: the safety is that the copy sized its own buffer from the numbers
   it carries. A trip is a bug in that hand-over, so it is logged and shown,
   ONCE, from a thread of its own so the renderer never blocks on the dialog. */

static volatile LONG s_fogAlarmed;
/* 512: the formatted message is ~260 bytes, and _snprintf silently truncates
   mid-word, dropping exactly the half that says what to do. */
static char s_fogAlarm[512];

static DWORD WINAPI fog_alarm_thread(LPVOID p)
{
    (void)p;
    MessageBoxA(NULL, s_fogAlarm, "tagpu: fog source guard tripped",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    return 0;
}

static void fog_alarm(const char* why, const TAGPU_FOGSRC* f, int wx, int wzp)
{
    /* The LOG fires every trip (throttled); only the DIALOG is one-shot, so a
       second, different trip still reaches the log. */
    static DWORD tick;
    DWORD now = GetTickCount();
    const void* words = f ? (const void*)f->words : NULL;
    int w = f ? f->w : 0, h = f ? f->h : 0, stride = f ? f->stride : 0;
    if (now - tick > 1000) {
        tick = now;
        tagpu_logf("FOGGUARD %s words=%p w=%d h=%d stride=%d world=(%d,%d)",
                   why, words, w, h, stride, wx, wzp);
    }
    if (InterlockedCompareExchange(&s_fogAlarmed, 1, 0) != 0) return;
    _snprintf(s_fogAlarm, sizeof s_fogAlarm,
              "The fog source guard caught a bad read and dropped it.\n\n"
              "%s\nwords=%p w=%d h=%d stride=%d world=(%d,%d)\n\n"
              "The game is still running. Please report it, and keep the log.",
              why, words, w, h, stride, wx, wzp);
    s_fogAlarm[sizeof s_fogAlarm - 1] = 0;
    /* never on the render thread: a modal dialog there stops the frame loop */
    {
        HANDLE hd = CreateThread(NULL, 0, fog_alarm_thread, NULL, 0, NULL);
        if (hd) CloseHandle(hd);
    }
}

int tagpu_fog_at(const TAGPU_FOGSRC* f, int wx, int wzp)
{
    float gx, gy, fx, fy;
    int cx, cy, r = 0;
    unsigned e;
    if (!f || !f->words) return 0;
    /* A plausible userland pointer, and nothing narrower: the copy is a heap
       allocation, and the process heap of a 0x400000 image can sit below
       0x600000. */
    if ((size_t)f->words <= 0x10000u || (size_t)f->words > tagpu_user_top()) {
        fog_alarm("the words are not a plausible allocation", f, wx, wzp);
        return 0;
    }
    /* THE BOUND: 1..TAGPU_FOG_MAXDIM a side and the stride the width implies,
       so tagpu_fog_entry's largest index, stride * h - 1, is inside the words */
    if (!tagpu_fog_src_ok(f)) {
        fog_alarm("the dimensions are not a source's", f, wx, wzp);
        return 0;
    }
    /* step 4 (inc/tagpu_fogsrc.h): the entry under the point and the point's
       place inside it, floor and fraction taken from the unclamped g */
    gx = (float)(wx  - 16) * (1.0f / 32.0f);
    gy = (float)(wzp - 16) * (1.0f / 32.0f);
    fx = floorf(gx); fy = floorf(gy);
    cx = (int)fx; cy = (int)fy;
    e = tagpu_fog_entry(f, cx, cy);
    if (fog_cov( e       & 0xFu, gx - fx, gy - fy) >= 0.5f) r |= 1;
    if (fog_cov((e >> 4) & 0xFu, gx - fx, gy - fy) >= 0.5f) r |= 2;
    return r;
}

/* engine LOS gate for a projectile anchor tile (0x49BE60 head); the particle
   leaves (tagpu_sfx.c) and the feature gate 0x4658E0 (tagpu_feat.c) run the
   same test. BOTH fog bands hide: the engine draws no effect it cannot
   currently see, and 0x4658E0 likewise tests the LOS counter when LosType&2
   and MAPPED otherwise — which is exactly the pairing the source encodes,
   since the out-of-LOS bit is set only in true-LOS mode. Every point is
   answered from the source, however far past the view or the map it lies. */
int tagpu_fx_tile_visible(const TAGPU_FXVIEW* v, int wx, int wzp)
{
    if (!(v->fogMode & 1) || !v->fog) return 1;
    return tagpu_fog_at(v->fog, wx, wzp) == 0;
}

/* the rect the engine's own explosion and debris passes cull against, out of
   the packet (`vp_addr`: L, T, R, B inclusive — the true rect, or the widened
   one at zoom < 1, exactly as the engine's field held it for this draw) */
static int in_vprect(const TAGPU_PACKET* pk, int sx, int sy)
{
    return sx >= pk->vp_addr[0] && sx <= pk->vp_addr[2] &&
           sy >= pk->vp_addr[1] && sy <= pk->vp_addr[3];
}

static unsigned s_rng = 0x12345u;
static int jitter5(void)            /* rand()*11/0x8000 - 5, visual only */
{
    s_rng = s_rng * 0x343FDu + 0x269EC3u;
    return (int)(((s_rng >> 16) & 0x7FFF) * 11 / 0x8000) - 5;
}

typedef struct { int hidden, fogged, laser, model, ball, sprite, flare, light, expl, debris, flash, other; } FXC;
static FXC s_c;

/* weapon fire, explosions, debris: the three tables the publisher gathered.
   NOTHING HERE READS ENGINE MEMORY. The projectile, explosion and
   debris arrays are walked on the game thread, inside DrawGameScreen, once per
   sim tick; every weapon field, every colour number and every GAF frame was
   resolved there. What is left in this file is the engine's DRAWING RULES —
   which rendertype makes which primitive, where the shadow blob goes, how the
   lightning bolt jitters — applied to values. */
static void gather_fx(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    const TAGPU_PK_PROJ* pj = tagpu_pk_proj(pk);
    const TAGPU_PK_EXPL* ex = tagpu_pk_expl(pk);
    const TAGPU_PK_DEBRIS* db = tagpu_pk_debris(pk);
    int alphaOn = (s_caps & 0x20) != 0, flashOn = (s_caps & 0x80) != 0;
    int eyeX = v->eyeX, eyeY = v->eyeY, vpL = v->vpL, vpT = v->vpT;
    unsigned i;
    char lb[288];

    s_mute = s_passive;                /* passive: count + log, emit nothing */

    /* ---- projectiles (the engine's 0x49BE60 rules) ---- */
    for (i = 0; i < pk->n_proj; i++) {
        const TAGPU_PK_PROJ* p = &pj[i];
        int X = p->pos[0], ALT = p->pos[1], Y = p->pos[2];
        int hx = X >> 16, halt = ALT >> 16, hy = Y >> 16;
        int hzp = hy - (halt >> 1);
        int sx, sy;
        float wx, wz;
        if (!tagpu_fx_tile_visible(v, hx, hzp)) { s_c.fogged++; continue; }
        sx = hx - eyeX + vpL; sy = (hy - eyeY) - (halt >> 1) + vpT;
        wx = (float)hx; wz = (float)hzp;
        if (s_log && i < 8 && (v->frame_counter % 60) == 0) {
            _snprintf(lb, sizeof lb,
                "fx: p%u rt=%d col=%d/%d pos=(%d,%d,%d) start=(%d,%d,%d) scr=(%d,%d) model=%d/%d frame=%08x",
                i, p->rt, p->col, p->col2, hx, halt, hy,
                p->start[0] >> 16, p->start[1] >> 16, p->start[2] >> 16,
                sx, sy, (int)p->model, (int)p->cmodel, p->frame);
            lb[sizeof lb - 1] = 0;
            flog(lb);
        }
        /* THE RECORD IS ONE EFFECT: its shadow, its body and its flame, whole
           or not at all (`effect_begin`) */
        effect_begin();
        /* ground shadow blob (rendertypes 1,3,4,6): alpha blit of the shadow
           sequence's frame 0 at the projectile's ground point */
        if ((p->flags & TAGPU_PK_FX_SHADOW) && alphaOn && pk->shadow_frame)
            fx_sprite((const unsigned char*)(size_t)pk->shadow_frame,
                      sx, (p->shadow_y - eyeY) + vpT, MODE_ALPHA, wx, wz);
        switch (p->rt) {
        case 0: {
            s_c.laser++;
            int x0 = sx, y0 = sy;
            int x1 = (p->start[0] >> 16) - eyeX + vpL;
            int y1 = ((p->start[2] >> 16) - ((p->start[1] >> 16) >> 1)) - eyeY + vpT;
            if (!(p->flags & TAGPU_PK_FX_COL2)) emit_line(x0, y0, x1, y1, p->col, wx, wz);
            else {
                /* engine: a second line one pixel beside the first, in
                   colour2, drawn first (0x49BE60 case 0) */
                int ax0 = x0, ay0 = y0, bx = x1, by = y1;
                int sx2, sy2, ex2, ey2;
                if (abs(y0 - y1) < abs(x0 - x1)) {
                    if (x1 < x0) { ax0 = x1; bx = x0; ay0 = y1; by = y0; }
                    sx2 = ax0; sy2 = ay0 - 1; ex2 = bx; ey2 = by - 1;
                } else {
                    if (y1 < y0) { ax0 = x1; bx = x0; ay0 = y1; by = y0; }
                    sx2 = ax0 - 1; sy2 = ay0; ex2 = bx + 1; ey2 = by;
                    /* the engine's variant shifts start x-1 / end x+1 */
                }
                emit_line(sx2, sy2, ex2, ey2, p->col2, wx, wz);
                emit_line(ax0, ay0, bx, by, p->col, wx, wz);
            }
            break;
        }
        case 1: case 3: case 6:
            s_c.model++;
            emit_model(v, p->model, wx, wz);
            emit_model(v, p->cmodel, wx, wz);
            break;
        case 2:
            s_c.ball++;              /* background refraction: engine-only */
            break;
        case 4:
            s_c.sprite++;
            if (p->frame)
                fx_sprite((const unsigned char*)(size_t)p->frame, sx, sy, MODE_OPAQUE, wx, wz);
            break;
        case 5:
            s_c.flare++;
            if (p->frame && alphaOn)
                fx_sprite((const unsigned char*)(size_t)p->frame, sx, sy, MODE_ALPHA, wx, wz);
            break;
        case 7: {
            s_c.light++;
            int dx = X - p->start[0];
            int dz = ALT - p->start[1];
            int dy = Y - p->start[2];
            double len = sqrt((double)dx * dx + (double)dz * dz + (double)dy * dy);
            long long len16 = (long long)len;                    /* __ftol */
            long long n16 = (len16 << 16) / 0x50000;              /* steps of 5 */
            int nseg = (int)(n16 >> 16);
            if (n16 != 0 && nseg > 0 && nseg < 512) {
                long long stx = ((long long)dx << 16) / n16;
                long long stz = ((long long)dz << 16) / n16;
                long long sty = ((long long)dy << 16) / n16;
                int pass;
                for (pass = 0; pass < 2; pass++) {
                    long long cx = p->start[0], cz = p->start[1], cy = p->start[2];
                    int px = (int)(cx >> 16), pz = (int)(cz >> 16), py = (int)(cy >> 16);
                    int k;
                    for (k = 0; k < nseg; k++) {
                        cx += stx; cz += stz; cy += sty;
                        int jx = (int)(cx >> 16) + jitter5();
                        int jz = (int)(cz >> 16) + jitter5();
                        int jy = (int)(cy >> 16) + jitter5();
                        emit_line(px - eyeX + vpL, (py - (pz >> 1)) - eyeY + vpT,
                                  jx - eyeX + vpL, (jy - (jz >> 1)) - eyeY + vpT, p->col, wx, wz);
                        px = jx; pz = jz; py = jy;
                    }
                }
            }
            break;
        }
        default:
            s_c.other++;
            break;
        }
        effect_end();
    }

    /* ---- flying debris pieces (drawn by 0x4211D0) ---- */
    if (s_debris) {
        for (i = 0; i < pk->n_debris; i++) {
            const TAGPU_PK_DEBRIS* d = &db[i];
            int X = d->pos[0], ALT = d->pos[1], Y = d->pos[2];
            int hx = X >> 16, halt = ALT >> 16, hy = Y >> 16;
            int sx = hx - eyeX + vpL, sy = (hy - eyeY) - (halt >> 1) + vpT;
            if (!in_vprect(pk, sx, sy)) continue;
            s_c.debris++;
            /* a piece is a record of its own, and its model its one part */
            effect_begin();
            emit_model(v, d->model, (float)hx, (float)(hy - (halt >> 1)));
            effect_end();
        }
    }

    /* ---- explosions (0x420B00): the flash of every record, then the bodies.
       THE ENGINE'S TWO PASSES ARE TWO LAYERS, AND THE SINKS AND THE KEYS KEEP
       THEM APART: a flash goes to B_FLASH, drawn before B_SPRITES, and at the
       key the walk starts at, which is under every explosion's model and over
       every model before it; a model takes the next key in the sequence and a
       sprite the one after. So one walk that emits each record whole draws
       every flash before every body, as the engine's two walks do, and lets a
       record be taken back out as one effect. ---- */
    if (s_expl && pk->n_expl) {
        const float flashKey = key_after(s_nm);
        s_c.expl = (int)pk->n_expl;
        for (i = 0; i < pk->n_expl; i++) {
            const TAGPU_PK_EXPL* e = &ex[i];
            int X = e->pos[0], ALT = e->pos[1], Y = e->pos[2];
            int hx = X >> 16, halt = ALT >> 16, hy = Y >> 16;
            int sx = hx - eyeX + vpL, sy = (hy - eyeY) - (halt >> 1) + vpT;
            float wx, wz;
            if (!in_vprect(pk, sx, sy)) continue;
            wx = (float)hx; wz = (float)(hy - (halt >> 1));
            effect_begin();
            if (flashOn && e->flash) {
                s_encCur = flashKey;
                fx_sprite((const unsigned char*)(size_t)e->flash, sx, sy, MODE_FLASH, wx, wz);
                s_encCur = key_after(s_nm);
                s_c.flash++;
            }
            emit_model(v, e->model, wx, wz);
            if (e->frame)
                fx_sprite((const unsigned char*)(size_t)e->frame, sx, sy, MODE_OPAQUE, wx, wz);
            effect_end();
        }
    }

    /* ---- LHT flash colours from the packet's table + the engine's palette
       (unscaled, like every world colour: the composite applies the Gamma) ---- */
    if (flashOn) {
        const unsigned char* lht = tagpu_pk_lht(pk);
        const unsigned char* pal = tagpu_pal_engine();
        unsigned char* rgb = s_lhtRGB;   /* file scope: the hand-over carries it */
        if (pal && lht && (!s_lhtInit || s_lhtPal != tagpu_pal_engine_serial() ||
                           memcmp(lht, s_lhtSrc, sizeof s_lhtSrc) != 0)) {
            int L;
            for (L = 0; L < 32; L++) {
                long sr = 0, sg = 0, sb = 0; int d;
                for (d = 0; d < 256; d++) {
                    int m = lht[L * 256 + d];
                    sr += (int)pal[m*4+0] - pal[d*4+0];
                    sg += (int)pal[m*4+1] - pal[d*4+1];
                    sb += (int)pal[m*4+2] - pal[d*4+2];
                }
                sr /= 256; sg /= 256; sb /= 256;
                rgb[L*3+0] = (unsigned char)(sr < 0 ? 0 : sr > 255 ? 255 : sr);
                rgb[L*3+1] = (unsigned char)(sg < 0 ? 0 : sg > 255 ? 255 : sg);
                rgb[L*3+2] = (unsigned char)(sb < 0 ? 0 : sb > 255 ? 255 : sb);
            }
            /* THE TABLE IS THE PASS. `s_pub.lht` hands the bytes to the
               Vulkan pass, which builds its own image from them, so the
               table reaching `rgb` IS the work. */
            memcpy(s_lhtSrc, lht, sizeof s_lhtSrc);
            s_lhtPal = tagpu_pal_engine_serial();
            s_lhtInit = 1;
        }
    }

    /* ---- the ALP blend table, when the engine has one (bit 5: without it 0x4B8500 draws
       nothing, and neither does this pass) ---- */
    if (alphaOn) {
        const unsigned char* alp = tagpu_pk_alp(pk);
        const unsigned char* pal = tagpu_pal_engine();
        if (pal && alp && (!s_alpInit || s_alpPal != tagpu_pal_engine_serial() ||
                           memcmp(alp, s_alpSrc, sizeof s_alpSrc) != 0)) {
            alp_build(alp, pal);
            memcpy(s_alpSrc, alp, sizeof s_alpSrc);
            s_alpPal = tagpu_pal_engine_serial();
            s_alpSerial++;
            s_alpInit = 1;
        }
    }

    static unsigned last = 0;
    if (v->frame_counter - last >= 60) {
        last = v->frame_counter;
        _snprintf(lb, sizeof lb,
            "fx: proj=%u (laser=%d model=%d sprite=%d flare=%d light=%d ball=%d fogged=%d) expl=%u flash=%d debris=%u -> lines=%d (far=%d) sprites=%d flashq=%d models=%d runs=%u (lost=%d why=%d of cap %d) atlas=%d%s",
            pk->n_proj, s_c.laser, s_c.model, s_c.sprite, s_c.flare, s_c.light, s_c.ball, s_c.fogged,
            pk->n_expl, s_c.flash, pk->n_debris, s_cLines, s_cLineFar, s_cSprites, s_cFlash, s_nm,
            tagpu_fxmodel_mark(), s_cModelLost, s_modelWhy, s_modelCap, s_atlas.n,
            s_passive ? " (passive)" : "");
        lb[sizeof lb - 1] = 0;      /* _snprintf leaves a full buffer unterminated */
        flog(lb);
    }
    s_mute = 0;
}

/* one frame: the particle layers the engine draws first, the two effects
   passes, then the particle layers it draws after them */
int tagpu_fx_gather(const TAGPU_FXVIEW* v)
{
    int fxOn = (s_armed == 1), sfxOn = tagpu_sfx_on();
    if (!fxOn && !sfxOn) return 0;
    /* THE ATLAS IS WHAT THIS PASS NEEDS BEFORE IT CAN GATHER -- set up once,
       since `made` latches. */
    if (!s_atlas.made) {
        atlas_setup();
        if (!s_atlas.made) return 0;
        /* only while the pass is armed, as the beat asks: particles alone
           gather here too */
        if (fxOn) consumer_ask();
    }
    /* the frame's capability bits, from the packet: bit5 the ALP alpha table
       is enabled, bit7 the LHT one. Read by the publisher, on the game thread. */
    s_caps = v->packet ? v->packet->fx_caps : 0u;
    /* THE ATLAS KEYS ON A GAF FRAME'S ADDRESS, AND THE FRAMES ARE PER-LEVEL.
       An explosion's two anim states come from `main+0x1AB8F`, a table
       `0x420620` builds from the level load (`0x4919D2`) and `0x420960` frees
       and NULLS from the teardown (`0x491B9F`) — not from the session "fx"
       bank the projectile sequences live in. So a second level's allocator can
       hand a new frame the address an old one had, and this atlas would serve
       the old level's pixels for it. Dropping it at the boundary costs one
       re-decode of what is on screen; not dropping it is a wrong picture that
       nothing detects. */
    {
        static unsigned s_atlasGen;
        unsigned g = v->packet ? v->packet->level_gen + 1u : 0u;
        if (g && g != s_atlasGen) { tagpu_gaf_atlas_forget(&s_atlas); s_atlasGen = g; }
    }
    if (s_atlas.full) tagpu_gaf_atlas_reset(&s_atlas);
    /* the allowance's frame, after the reset and before the first lookup */
    tagpu_gaf_atlas_frame(&s_atlas);
    /* `tagpu_gaf_atlas_restore_vk` above publishes this atlas's frame list for
       the Vulkan restorer. */
    memset(s_nv, 0, sizeof s_nv); s_nm = 0;
    tagpu_fxmodel_frame();
    s_cLines = s_cSprites = s_cFlash = s_cAtlasFail = s_cOverflow = s_cQuads = 0;
    s_cLineFar = 0;
    s_cModelLost = 0; s_modelWhy = 0;
    s_lzoom = v->zoom > 0.0f ? (double)v->zoom : 1.0;
    s_lzcx = (double)v->zoomCx; s_lzcy = (double)v->zoomCy;
    s_lss = v->ss > 0 ? v->ss : 1;
    tagpu_line_rect(v->vpL, v->vpT, v->vw, v->vh, s_lss,
                    &s_lclipL, &s_lclipT, &s_lclipR, &s_lclipB);
    s_lclipW = v->gw * s_lss; s_lclipH = v->gh * s_lss;
    /* the lines of a frame the A/B may claim, kept before the clip for the
       oracle; the claim in tagpu_fx_render writes them */
    tagpu_line_ab_begin(&s_lab, s_ab && !s_abDone);
    memset(&s_c, 0, sizeof s_c);
    /* the band, latched for the frame (tagpu_fx.h); the cap is also bounded
       by the list, which `tagpu_fx_model_bound` never exceeds */
    s_encFx = v->encFx; s_encStep = v->encStep;
    s_modelCap = v->modelCap < 0 ? 0 : v->modelCap > MAXMODEL ? MAXMODEL : v->modelCap;
    s_modelsOn = v->modelsOn;
    /* THE MODELS' FRAME. The engine's projection puts the viewport at (0x80,
       0x20) and this pass puts it at (vpL, vpT); the clip rect is the one the
       engine's context holds for these draws, `vp_addr`, in the engine's
       space -- the rasterisers clip before anything is offset. */
    s_rv.eyeX = v->eyeX; s_rv.eyeY = v->eyeY;
    s_rv.ox = v->vpL - 0x80; s_rv.oy = v->vpT - 0x20;
    if (v->packet) {
        s_rv.clipL = v->packet->vp_addr[0]; s_rv.clipT = v->packet->vp_addr[1];
        s_rv.clipR = v->packet->vp_addr[2]; s_rv.clipB = v->packet->vp_addr[3];
    }
    s_encCur = key_after(0); s_mute = 0;
    tagpu_fx_set_events(NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0);
    if (sfxOn) tagpu_sfx_gather(v);
    if (fxOn) gather_fx(v);
    if (sfxOn) tagpu_sfx_frame_done(v);
    if (s_cOverflow || s_cAtlasFail) {
        static unsigned last = 0;
        if (v->frame_counter - last >= 60) {
            char b[160];
            last = v->frame_counter;
            _snprintf(b, sizeof b, "fx: DROPPED this frame: bucket-full=%d atlas-fail=%d (lines=%d flash=%d sprites=%d verts)",
                      s_cOverflow, s_cAtlasFail, s_nv[B_LINES], s_nv[B_FLASH], s_nv[B_SPRITES]);
            flog(b);
        }
    }
    return s_nv[B_LINES] + s_nv[B_FLASH] + s_nv[B_SPRITES] + s_nm + s_nEvt;
}

/* EVERY MODEL THE GATHER COULD ASK FOR, before any of its gates: a
   projectile's body and its flame, a piece, an explosion's body. The gather
   emits at most one model per model named here, so the count bounds it
   whatever the fog, the viewport rect and the levers decide -- and the
   tables' own sizes bound the count by MAXMODEL. */
int tagpu_fx_model_bound(const TAGPU_PACKET* pk)
{
    const TAGPU_PK_PROJ* pj;
    const TAGPU_PK_EXPL* ex;
    const TAGPU_PK_DEBRIS* db;
    unsigned i;
    int n = 0;
    if (!pk) return 0;
    pj = tagpu_pk_proj(pk);
    ex = tagpu_pk_expl(pk);
    db = tagpu_pk_debris(pk);
    for (i = 0; pj && i < pk->n_proj && i < TAGPU_PK_MAX_PROJ; i++)
        n += (pj[i].model != TAGPU_PK_NOMODEL) + (pj[i].cmodel != TAGPU_PK_NOMODEL);
    for (i = 0; db && i < pk->n_debris && i < TAGPU_PK_MAX_DEBRIS; i++)
        n += db[i].model != TAGPU_PK_NOMODEL;
    for (i = 0; ex && i < pk->n_expl && i < TAGPU_PK_MAX_EXPL; i++)
        n += ex[i].model != TAGPU_PK_NOMODEL;
    return n > MAXMODEL ? MAXMODEL : n;
}

int tagpu_fx_nmodels(void) { return s_nm; }
const TAGPU_FXMODEL* tagpu_fx_model(int i) { return (i >= 0 && i < s_nm) ? &s_models_[i] : NULL; }

/* ---- the Vulkan hand-over (the fourth world pass) -------------------------
   tagpu_fx.h is the contract. Published AFTER the gather, from the very
   arrays, numbers and texels it built. The fog is not in it: the pass samples
   the slot's fog image (tagpu_vk_fog.h). */

static void fx_publish(const TAGPU_FXVIEW* v, int total)
{
    int b;
    /* NOTHING IS PUBLISHED UNTIL A VULKAN PASS WILL RUN. `s_mirrorAsked` is
       the latch `consumer_ask` sets the first time `tagpu_vk_owns_present()`
       says yes and the mirror is allocated; while it is 0 nothing will ever
       take the hand-over, and the stores below are pure cost that is not paid.
       IT IS A ONE-WAY LATCH: nothing clears it for the life of the process.
       That is deliberate on the cheap side of a trade -- the mirror it guards
       is already allocated by then, so re-testing per beat would buy back a
       memcpy and nothing else -- and `tagpu_feat.c` does the same, which is
       the reason not to make this one file differ. */
    if (!s_mirrorAsked) { s_pubHave = 0; s_abFrame = 0; return; }
    memset(&s_pub, 0, sizeof s_pub);
    for (b = 0; b < NBUCKET; b++) { s_pub.vert[b] = s_verts[b]; s_pub.n[b] = s_nv[b]; }
    s_pub.nmodels = s_nm;
    s_pub.gw = (float)v->gw; s_pub.gh = (float)v->gh;
    s_pub.zoom = v->zoom > 0.0f ? v->zoom : 1.0f;
    s_pub.zoomCx = v->zoomCx; s_pub.zoomCy = v->zoomCy;
    s_pub.depthScale = v->depthScale > 1.0f ? v->depthScale : 512.0f;
    /* THE ROUTE IS THE PUBLISHED LIST: `rlistWant` is what says a restore
       route exists; it is latched by the arm. */
    s_pub.restored = (s_atlas.rlistWant && tagpu_classicpp_assets()) ? 1 : 0;
    /* bit1 set as well: effects hide in grey rather than darkening; bit2 the true-LOS mode, which
       the nano events' tile test takes (NFS) */
    s_pub.fog = (v->fogMode & 1) | 2 | ((v->fogMode & 2) << 1);
    s_pub.pevt = s_evt; s_pub.npevt = s_nEvt; s_pub.pevtTick = s_evtTick;
    s_pub.pevtFrame = s_evtFrame;
    s_pub.pevtWind = s_evtWind;
    s_pub.eyeX = v->eyeX; s_pub.eyeY = v->eyeY;
    memcpy(s_pub.encLayer, v->encLayer, sizeof s_pub.encLayer);
    s_pub.scafP[0] = (float)v->vpL; s_pub.scafP[1] = (float)v->vpT;
    s_pub.scafP[2] = (float)v->vw;  s_pub.scafP[3] = (float)v->vh;
    s_pub.uss   = (float)(v->ss > 0 ? v->ss : 1);
    s_pub.zoomF = v->zoom > 0.0f ? v->zoom : 1.0f;
    s_pub.zoomCFx = v->zoomCx; s_pub.zoomCFy = v->zoomCy;
    tagpu_fx_atlas_hand(&s_pub);
    /* THE RESTORE REQUEST, WHICH IS THE ONLY RESTORE ROUTE PUBLISHED HERE. */
    if (s_atlas.rlistWant && s_atlas.rlist) {
        s_pub.restoreFrames  = s_atlas.rlist;
        s_pub.restoreN       = s_atlas.rlistN;
        s_pub.restoreGen     = s_atlas.rlistGen;
    }
    /* THE LIGHT TABLE ONLY WHEN IT HAS BEEN BUILT. A frame with flash vertices
       and no table has no colour for its flashes, so tagpu_vk_fx.c refuses it
       rather than guess one. */
    s_pub.lht = s_lhtInit ? s_lhtRGB : NULL;
    s_pub.alpT = s_alpInit ? s_alpT : NULL;
    s_pub.alpSerial = s_alpSerial;
    s_pub.vpL = v->vpL; s_pub.vpT = v->vpT; s_pub.vw = v->vw; s_pub.vh = v->vh;
    /* WHETHER THE CLIP IS ACTUALLY ON, not whether a rect exists: the native
       pass decides it (tagpu_native.c `s_scissorOn`). */
    s_pub.scissorOn = tagpu_native_scissor_on();
    s_pub.ss = v->ss;
    /* THE A/B FLAG LIVES EXACTLY ONE FRAME (tagpu_feat.c has the argument). */
    s_pub.ab = s_abFrame; s_abFrame = 0;
    /* THE STAMP, AND WHAT IT ACTUALLY BOUNDS (tagpu_fx.h has it in full): not a
       freed pointer -- `s_verts` and the GAF mirror are static and are never
       freed -- but a STALE READ. `tagpu_native.c` calls this pass only `if
       (nfx)`, so a frame that gathered nothing never publishes while the next
       gather overwrites `s_verts` underneath the standing hand-over. */
    s_pub.frame = v->frame_counter;
    s_pubHave = total > 0;
}

/* The atlas's half of the hand-over and the engine's table it is expanded
   through -- tagpu_feat.c's `tagpu_feat_atlas_hand`, for the same two
   readers. */
int tagpu_fx_atlas_hand(TAGPU_FXHAND* h)
{
    TAGPU_GAFVIEW av;
    int ok = tagpu_gaf_atlas_view(&s_atlas, &av);
    h->atlas = av.idx; h->atlasDim = ok ? av.dim : s_atlas.dim;
    h->atlasKey = av.key; h->atlasDirty = av.dirty;
    h->atlasRows = av.rows;
    h->atlasSerial = av.serial; h->atlasWhole = av.whole;
    h->pal = tagpu_pal_engine(); h->palSerial = tagpu_pal_engine_serial();
    return ok;
}

int  tagpu_fx_atlas_owed(void) { return tagpu_gaf_atlas_owed(&s_atlas); }
void tagpu_fx_atlas_ack(unsigned serial, int keep) { tagpu_gaf_atlas_ack(&s_atlas, serial, keep); }

/* Hand it over, ONCE (tagpu_fx.h). */
int tagpu_fx_handover(TAGPU_FXHAND* out, unsigned now)
{
    if (!s_pubHave || !out) return 0;
    /* not this frame's, so not alive -- and cleared, so the next frame starts
       honest. See tagpu_terr.h. */
    if (s_pub.frame != now) { s_pubHave = 0; s_abFrame = 0; return 0; }
    *out = s_pub;
    s_pubHave = 0; s_abFrame = 0;
    return 1;
}

void tagpu_fx_render(const TAGPU_FXVIEW* v)
{
    /* THIS PASS ONLY GATHERS AND HANDS OVER -- the feature pass's shape
       (tagpu_feat.c). */
    /* the models count: a frame whose effects are all models still hands
       over, because the posed pass draws them only on a frame this pass has
       taken (tagpu_fx.h `nmodels`) */
    int total = s_nv[B_LINES] + s_nv[B_FLASH] + s_nv[B_SPRITES] + s_nm + s_nEvt;
    int taking;
    /* A FRAME WITH NOTHING TO DRAW HANDS NOTHING OVER. Leaving the previous
       frame's hand-over standing would have the Vulkan lane draw last frame's
       effects over this frame's -- and on the frame a level is torn down, over
       nothing at all. `total` is the whole refusal. */
    if (total == 0) { s_pubHave = 0; s_abFrame = 0; return; }

    /* OUTSIDE ANY DRAW, because this pass issues none: the A/B is armed
       from the gather. Read once so the two arms cannot disagree about
       which frame is the capture frame. */
    taking = s_ab && !s_abDone;
    /* NO `ss` BOUND ON THIS A/B: the Vulkan lane draws the world into a gw*ss
       by gh*ss target and the capture reads that target, so this pass is
       measurable on the configuration it actually ships in. The refusal lives
       in the lane that can see the target: if there is none that frame,
       tagpu_vk.c says so by name and captures nothing. [tagpu_vk_world.h.] */

    /* Everything above is the GATHER; `fx_publish` below hands the vertices
       and the numbers to the Vulkan pass (tagpu_vk_fx.c), which draws them. */
    if (taking) {
        /* THE A/B CLAIM. The lever claims the VULKAN capture: `tagpu_vk_ab_arm`
           unlinks the target `_vk.ppm` at the instant the claim latches, which is what
           makes the file on the disk this arming's rather than an earlier run's. Diff
           it against a capture taken from another BUILD. */
        s_abDone = 1;
        s_abFrame = tagpu_vk_ab_arm("fx");
        /* and the frame's lines as emit_line had them before the clip, with
           the rect it clipped to (tagpu_line.h) */
        tagpu_line_ab_write(&s_lab, "fx", s_lclipW, s_lclipH, s_lss,
                            s_lclipL, s_lclipT, s_lclipR, s_lclipB);
    }

    /* PUBLISHED AFTER THE GATHER: these are the vertices, the numbers and the
       texels this frame built, and the Vulkan lane is about to draw the same
       ones. */
    fx_publish(v, total);
}
