/* tagpu_terr.c — the terrain pass: the 32x32 map tiles, native.

   The engine's pass 0x483FA0 is a grid blit and nothing else
   (research/notes/terrain-depth.md 2, re-read from the bytes for G13b):

     tileX0 = eyeX/32 (toward zero);  fracX = eyeX - tileX0*32
     cols   = ceil((viewW + fracX)/32)            same for rows/viewH/fracY
     idx    = TILE_MAP[(tileY0+row)*rowStride + tileX0+col]   u16, *(main+0x1428B)
     gfx    = *(TILE_SET+4) + idx*0x400           8bpp, 32 wide, *(main+0x14283)
     screen = (vpL + col*32 - fracX, vpT + row*32 - fracY)

   rowStride = FeatureMapSizeX/2 (the map's width in 32-px cells). Colour plane
   only: no colour key (tiles are opaque), no shading, no depth, and NEITHER
   height NOR LOS enters it — cliffs, water and shadows are painted into the
   tile art by the map compiler. Three things follow, and this module leans on
   all three:

   * Terrain is the frame's implicit far plane, so it draws at a depth key
     BELOW every other band — under the flat-feature band (0.40), not merely
     under the row sweep.
   * Water does not animate, so a still atlas loses nothing. MEASURED
     2026-09-05, not assumed: camera pinned over open water on Anteer Strait
     and over Ring Atoll's lagoon, NOT ONE PIXEL of the viewport changed over
     10 s, nor over 30 s at +10 game speed, and NOT ONE BYTE of the 256-entry
     live palette changed across 24 samples. The minimap kept changing in the
     same frames, which is what says the capture was live. Earlier comments
     here claimed the engine cycles the palette for water; it does not.
   * Fog is a straight import of the shared rule (tagpu_glsl.h) — with one
     change that owning the bottom layer forces. Terrain must PAINT the fog's
     solid black in unexplored cells rather than discard: nothing is behind it
     any more except tagpu_terrown.c's key fill (TAGPU_GLSL_FOG_TERRAIN).

   The tile set is built by LoadMap and never changes after, so the atlas is
   built ONCE per map — a single R8 texture of 32x32 cells on a 34-texel pitch,
   64 per row (a GL_TEXTURE_2D_ARRAY is not viable: 5062 tiles on Two Continents
   against the usual 2048-layer cap). The spare texel on each side is a
   replicated edge guard, not padding — see CELL_PITCH. A map change is the TILE_SET pointer or its
   count moving. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_opt.h"
#include "tagpu_terr.h"
#include "tagpu_pal.h"
#include "tagpu_restoreglsl.h"
#include "tagpu_classicpp.h"
#include "tagpu_glsl.h"
#include "tagpu_terrown.h"
#include "tagpu_native.h"
#include "tagpu_shadow.h"
#include "tagpu_zoom.h"
#include "tagpu_vk.h"       /* tagpu_vk_armed(): whether to pay for the mirrors */

/* ---- engine layout (terrain-depth.md 1, byte-confirmed) ---- */
#define OFF_TILEMAP  0x1428B   /* u16 per 32-px cell, stride mapW16/2         */
#define OFF_TILESET  0x14283   /* -> {u32 count; u8* pixels}                  */
#define OFF_MAPW16   0x14233   /* map W/H in 16-px tiles                      */
#define OFF_MAPH16   0x14237
#define OFF_FEATMAP  0x14287   /* FeatureStruct grid, stride 0xD, one per     */
#define FT_STRIDE    0x0D      /* 16-px cell; the height byte at +4 is what   */
#define FT_HEIGHT    0x04      /* Classic++ lights the terrain from           */

#define TILE_PX      32
#define TILE_BYTES   0x400
#define ATLAS_COLS   64
/* Cells are laid out one texel apart, and that texel is a COPY of the cell's
   last row/column — it is not padding, it is a guard rail. A fragment centre
   that lands exactly on a quad's far edge interpolates u (or v) to exactly u1,
   and GL_NEAREST resolves that to floor(u1*W) = the FIRST texel of the next
   cell: at zoom 0.25 with ss=2 a tile is 16 FBO px, so a tile boundary lands
   on a pixel centre whenever the cell's game-space top edge is odd, and every
   such row sampled the unrelated tile 64 cells later in the atlas — the blue
   hairlines along tile edges. Duplicating the edge texel makes that sample the
   right colour instead. Nothing else changes: the quad still spans 32 texels,
   so sampling at 1:1 is bit-identical to the un-padded atlas. */
#define CELL_BORDER  1
#define CELL_PITCH   (TILE_PX + 2 * CELL_BORDER) /* 34                         */
#define ATLAS_W      (ATLAS_COLS * CELL_PITCH)   /* 2176                       */
#define MAX_TILES    65536                      /* the index is a u16          */
#define ICOMP        4                          /* col,row, atlas col,row      */
/* THE STAGING CEILING IS MEMORY, NOT A RESOLUTION. What one frame may need is
   reserved from the live viewport at the zoom floor (tagpu_terr_clamp_span),
   so it tracks the screen. MEASURED, from the `terr: staging` line: 1024x768
   reserves 10672 cells (83 KB), 2560x1440 54208 (423 KB), 3840x2160 124488
   (972 KB), 5120x2880 223568 (1746 KB); 7680x4320 works out at 507592
   (3966 KB). This is only the point past which a viewport is NOT BELIEVED —
   `vw`/`vh` are read out of engine memory, and a garbage pair must not be
   allowed to ask for an arbitrary allocation. 24 MB is a viewport of about
   14000 x 14000, which is past any screen and well short of a wild value.
   A cell costs FOUR SHORTS (s_inst); as six vertices of six floats, the same
   ceiling would be 432 MB. */
#define INST_MAX_BYTES  (24u * 1024u * 1024u)
#define INST_MAX_CELLS  ((int)(INST_MAX_BYTES / (ICOMP * sizeof(short))))
#define TERR_ENC     0.10f                      /* under every other band      */
#define DEFAULT_KEY  254                        /* see tagpu_terrown.c         */

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* ---- arming ---- */
static int s_armed = -1;
static int s_log = 0, s_passive = 0, s_over = 0, s_key = DEFAULT_KEY;
static unsigned s_armCheck = 0;

/* ---- the hand-over to the Vulkan edition, and the A/B lever (Phase G/G19e)
   `tagpu_terr.ab` makes this pass draw over a black frame with a cleared depth
   buffer and read it back ONCE; `s_abFrame` travels to the Vulkan lane with the
   instances rather than being polled twice on two cadences, so both lanes
   capture the same frame. See tagpu_terr.h and tagpu_vk_terr.c. */
#define ABFILE   "tagpu_terr.ab"
static int s_ab, s_abDone, s_abFrame;
static int s_pubHave;                  /* this frame's hand-over is waiting   */
static TAGPU_TERRHAND s_pub;
/* THE CPU MIRRORS ARE ASKED FOR, ONCE, AND THEN KEPT. Unlike tagpu_gaf.c's
   incremental atlas (§2.29), both of this pass's big textures are built WHOLE
   in one call out of a buffer that is freed three lines later -- so the whole
   of the mechanism here is "do not free it", and a mirror is correct from the
   instant it exists because it IS the buffer glTexImage2D was handed. What the
   flag has to do instead is force ONE rebuild when the Vulkan lane arms after
   the texture was built: `ensure_atlas` and `ensure_height` both early-return
   on an identity test, and the extra term below is what makes them fall
   through exactly once. */
static int s_mirrorWant;               /* the Vulkan lane asked for mirrors   */
static unsigned char* s_atlasMirror;   /* ATLAS_W x s_atlasH, or NULL         */
static unsigned s_atlasMirrorSerial;
/* THE RESTORED TWIN'S MIRROR WAS THE OTHER WAY OF FEEDING THE SAME LANE, and
   it went in landing 11-5c with the GL half it read back from. It could only
   ever be a read-back: the indexed atlas above is the buffer an upload was
   handed, but the restored one was painted on the GPU by tagpu_restoreglsl.c,
   so a CPU copy meant pulling it back down every published frame. What is left
   is the other route, below -- hand the CONSUMER the work instead of the
   pixels -- and tagpu_terr.h's `atlasRgb`/`atlasRgbRows` are now always NULL/0.
   [The vulkan-only plan, landing 11-5c.] */

/* ---- THE RESTORE REQUEST, for a lane that restores on its own -----------
   The other half of gate 2's mirror, and its replacement: instead of reading
   the GL twin back so a second lane can upload it, hand that lane the WORK.
   Latched on the arm beat exactly as `s_mirrorWant` is, and for the same
   reason -- the answer is a file-attribute query and it does not change
   mid-map in any way worth paying for every frame.

   The list is the one `glsl_begin` built for the GL job, RETAINED instead of
   freed: see tagpu_terr.h, `restoreFrames`. Retaining it costs
   `s_atlasN * sizeof(TAGPU_RGLSL_FRAME)` -- 44 bytes a tile, so under 90 KB
   for a full atlas -- and it is the only copy of two facts a `_vk` file cannot
   re-derive, the tileability flags and the centre-out order. */
static int                s_rvkWant;   /* tagpu_restorevk.on, latched         */
static TAGPU_RGLSL_FRAME* s_rFrames;   /* s_rFrameN entries, restore order    */
static int                s_rFrameN;
static unsigned           s_rSerial;   /* bumped on every change, drop included */
static int                s_rRepaint;  /* the list is a repaint over a restore */

/* Forget the list. Every caller is a point where the ATLAS stopped being the
   one the list describes, so the serial moves even though nothing replaces it:
   a consumer keyed on the serial then drops its job instead of painting the
   new map's atlas with the old map's rectangles. */
static void rlist_drop(void)
{
    if (s_rFrames) { free(s_rFrames); s_rFrames = NULL; }
    if (s_rFrameN || s_rRepaint) { s_rFrameN = 0; s_rRepaint = 0; }
    s_rSerial++;
}
static unsigned char* s_hMirror;       /* s_hW x s_hH, or NULL                */
static unsigned s_hMirrorSerial;

int tagpu_terr_key(void) { return s_key; }

int tagpu_terr_armed(unsigned frame_counter)
{
    int was;
    char buf[128];
    int n;
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return s_armed > 0;
    s_armCheck = frame_counter;
    was = s_armed;
    s_armed = 0;
    n = tagpu_opt_read("tagpu_terr.on", buf, sizeof buf);
    if (n < 0) {
        tagpu_terrown_set_skip(0);
        if (was > 0) flog("terr: disarmed");
        return 0;
    }
    {
        int wasKey = s_key;
        s_log = 0; s_passive = 0; s_over = 0; s_key = DEFAULT_KEY;
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
                else if (!lstrcmpiA(p, "over")) s_over = 1;
                else if (!strncmp(p, "key=", 4)) {
                    int k = atoi(p + 4);
                    /* index 0 is the fog's own solid black — never a free key */
                    if (k > 0 && k < 256) s_key = k;
                }
                if (last) break;
                p = q + 1;
            }
        }
        /* a live key change would leave one frame filled with the old index
           while the composite inverts against the new one — hand the draw back
           for a frame instead */
        if (s_key != wasKey) tagpu_terrown_set_skip(0);
    }
    s_armed = 1;
    /* the A/B lever, on the same beat. It re-arms when the file goes away and
       comes back, which is why `touch` on one that is already there does
       nothing. */
    s_ab = GetFileAttributesA(ABFILE) != INVALID_FILE_ATTRIBUTES;
    if (!s_ab) s_abDone = 0;
    /* THE CPU MIRRORS (Phase G / G19e), on this beat and not per frame --
       tagpu_vk_armed() is two file-attribute queries and a pass that asked
       every frame would make them on every frame of ordinary play, where the
       answer is no and stays no.
       ASKED FOR HERE, AND THIS RUNS BEFORE THE GATHER, so the rebuild the flag
       forces happens in the same frame's ensure_atlas / ensure_height and the
       first hand-over that carries a mirror carries a COMPLETE one. Once asked
       it stays asked for the process's life: the flag is what keeps the buffers
       off an ordinary play session, and un-asking it mid-session would only buy
       back memory a re-arm would immediately spend again. */
    /* ASKED OF THE CONSUMER, NOT OF THE LEVER. [FROM THE 4d-1 LANDING REVIEW.]
    This used to test `tagpu_vk_armed()`, which is true whenever `tagpu_vk.on`
    exists -- and these latches are one-way, so once asked the memory is held
    for the process's life. Until 4d-1 that was right: `tagpu_vk.on` under
    `renderer=openglcore` brought up route D, which consumed the mirror. Route
    D is gone, so on that path the lever now arms nothing and the mirror would
    be paid for with no consumer at all. `tagpu_vk_owns_present()` is exactly
    "a Vulkan pass will run in this process", which is the question. */
    if (!s_mirrorWant && tagpu_vk_owns_present()) s_mirrorWant = 1;
    /* AND THE OTHER WAY OF FEEDING THAT LANE: hand it the frame list and let
       it restore, rather than reading our own restore back for it. Latched on
       the same beat and read only when there is a lane to feed. */
    if (!s_rvkWant && s_mirrorWant &&
        GetFileAttributesA("tagpu_restorevk.on") != INVALID_FILE_ATTRIBUTES) {
        s_rvkWant = 1;
        flog("terr: restorevk -- the restored atlas is the other lane's to paint, "
             "so no read-back and the frame list is published instead");
        /* AND THE MIRROR GOES WITH THE LATCH, ROWS FIRST -- which the first
           version of this did not do, and the claim in tagpu_terr.h that the
           two hand-over fields are mutually exclusive was false because of it.
           [FROM THE LANDING-7c REVIEW; independently found the same hour.]
           This lever is POLLED until it latches, so it can be created
           mid-session -- and then the read-back's early return stopped
           updating the mirror while leaving `rows > 0` standing, so the
           publisher below ran BOTH blocks. A consumer then resized its restored
           image twice in one call and uploaded a frozen mirror into the very
           image the other lane renders into; where the mirror's rows and the
           atlas's height disagreed it refused every frame instead and the
           terrain stopped drawing altogether.
           Freeing it was the same argument the read-back's own failure path
           made: a buffer nothing will read again is up to 23 MB held for the
           process, and leaving ROWS standing is worse than the memory because
           the publish was gated on `rows > 0`. Both halves went with the
           read-back in landing 11-5c and there is nothing left to free. */
    }
    if (s_passive || s_over) tagpu_terrown_set_skip(0);
    if (was != 1) {
        char b[160];
        _snprintf(b, sizeof b, "terr: ARMED (log=%d passive=%d over=%d key=%d)",
                  s_log, s_passive, s_over, s_key);
        flog(b);
    }
    return 1;
}

int tagpu_terr_on(void) { return s_armed > 0; }

/* ---- the atlas -----------------------------------------------------------
   THE ATLAS IS BUILT WHEN THIS SAYS SO. `s_atlasBuilt` used to share the job
   with a GL texture name, and keying `ensure_atlas`'s already-built test on
   that name rebuilt the whole atlas EVERY FRAME on a lane with no GL -- a
   5.9 MB calloc and free, the per-tile copy loop, an `IsBadReadPtr` over the
   tile set, a log line a frame, and an `s_atlasMirrorSerial++` that made the
   Vulkan twin re-upload the entire atlas image every frame. The name went in
   landing 11-5c and the flag is the whole answer. [FROM THE 4b-2 LANDING
   REVIEW, 2026-09-18 -- the twelfth instance of that landing's own shape, in
   a function gpu-status.md 2.50 listed as fixed when only its mirror was.] */
static int    s_atlasBuilt;
static int    s_atlasH, s_atlasN;      /* atlas rows*CELL_PITCH, tiles held   */
static const void* s_setPtr;           /* the TILE_SET we built from          */
static int    s_setCount;
static int    s_maxTex;
/* Classic++ (tagpu_classicpp.on): the RESTORED copy of the atlas -- the same
   cells on the same pitch, true colour from the unditherer's model run as
   fragment passes by tagpu_restoreglsl.c straight into this texture -- so the
   one set of UVs serves both looks. Built once per map, a slice per frame,
   and the cells SHOW AS THEY LAND (renderers.md 4c Q6): the restorer clears
   the texture to alpha 0 when the job starts and its out pass writes alpha 1
   over every cell it paints, guard ring included, so the shader's alpha test
   is the per-cell flag -- no second texture, no upload, and a cell's samples
   are all-or-nothing because one quad paints its interior and its ring.
   Draws issued in the same frame are in order, so a cell whose out pass was
   issued by this frame's slice is restored in this frame's terrain draw. */
static int    s_rgbState = 0;      /* 0 none, 1 request published, -1 failed  */
static unsigned s_rgbPalSerial;    /* tagpu_pal serial the request was built through */
static const unsigned char* s_setPix;   /* the current set's tile pixels     */
/* the last gathered rect, in 32-px cells: the GLSL job restores the tiles
   under it first (renderers.md 4c Q6), so it starts one frame after the atlas */
static int    s_rectTx0, s_rectTy0, s_rectCols, s_rectRows, s_rectValid;
/* Classic++ lighting: the engine's height grid as one R8 texel per 16-px
   cell, built with the atlas (once per map, from FeatureStruct+4 -- the byte
   tagpu_feat.c reads per anchor) so the fragment shader can take the lab's
   normal at any point of the map without a vertex stream growing: the
   heightfield normal is per fragment here, from the grid, where the lab
   computes it per vertex on 16-px sub-quads -- 4x the terrain vertices,
   29 MB a frame at the zoom floor, for the same field. Unit 5. */
static int    s_hW, s_hH;              /* 0 while there is no usable grid    */
static const void* s_hGrid;            /* the inputs the texture was built  */
static const void* s_hSet;             /* from, or last attempted from      */
static unsigned s_hFrame;              /* the frame of that attempt          */
static int    s_hMeshW, s_hMeshH;      /* the grid it was built from: a failed
                                          rebuild leaves the old mesh, and this
                                          is what keeps it undrawn (review) */
/* THE CASTER MESH'S CPU MIRROR (Phase G / G19e, the shadow pass). The same
   answer as the atlas's and the height grid's: the very buffers the
   glBufferData calls below were handed, kept instead of freed, so the Vulkan
   shadow pass draws the SAME vertices and the SAME index order rather than a
   second evaluation of build_hills' arithmetic. 19.3 MB on Two Continents
   (6.4 vertices + 12.9 indices) and paid for only while the Vulkan lane is
   armed -- `s_mirrorWant`, which is set from tagpu_vk_armed() on the arm beat.
   The serial says when they last changed, so the Vulkan lane uploads on a map
   change and not per frame. */
static float*    s_hMeshV;             /* s_hMeshVN * 3 floats, or NULL       */
static unsigned* s_hMeshI;             /* s_hMeshIN indices, or NULL          */
static size_t    s_hMeshVN, s_hMeshIN;
static unsigned  s_hMeshSerial;
/* 1 when build_hills ran under `s_mirrorWant` and produced no mirror anyway --
   a grid too small to mesh, or a malloc that failed. It is what stops
   ensure_height's mirror term from asking for the same rebuild every frame for
   the life of the map; cleared wherever the mesh itself is. */
static int       s_hMeshNoMirror;

/* THIS FRAME'S CELLS, one record each: the cell's column and row in the
   gather's own grid, then its tile's column and row in the atlas. Everything
   the six vertices used to carry is rebuilt from these four shorts by the
   vertex shader below, which is why a whole 4K view fits in a megabyte.
   GROWN FROM THE VIEWPORT (terr_reserve, through tagpu_terr_clamp_span) and
   never shrunk: a resolution change reserves once and every frame after it
   costs nothing. */
static short* s_inst;
static int    s_instCells;             /* what s_inst can hold                */
static int    s_ncell;                 /* what this frame put in it           */
/* what the shader needs to rebuild them: the screen point grid cell (0,0)'s
   top-left corner sits on, and the atlas's texel size. The map cell it is,
   the other half of the rebuild, is s_rectTx0/s_rectTy0. */
static float s_origX, s_origY, s_iw, s_ih;
/* The shader spells CELL_PITCH, CELL_BORDER and TILE_PX as literals — GLSL
   cannot see a C macro — so a change to any of the three must not silently
   leave the UVs a texel out. This is that change refusing to compile. */
typedef char terr_atlas_consts_unchanged[
    (CELL_PITCH == 34 && CELL_BORDER == 1 && TILE_PX == 32) ? 1 : -1];

/* ONE QUAD PER VISIBLE CELL, AND THE CELL IS AN INSTANCE. `aCorner` is the
   unit quad's six corners in the engine's own vertex order — one static buffer
   uploaded at init and never touched again — and `aCell` is this frame's four
   shorts for the cell, per instance.

   The three values the per-vertex stream used to carry are rebuilt here, and
   rebuilt EXACTLY: every term is an integer far below 2^24 — a grid column is
   at most 2052 (the 16384-px viewport tagpu_native.c will believe, at the
   0.25x zoom floor, over 32), a map cell at most 2047 (`mapW16 <= 4096`, and
   the tile map's stride is half that), an atlas column 63 and an atlas row
   1023 — so each product and sum is exact in float and aPos, aUV and aWorld
   are bit for bit the floats the CPU used to write. Those same bounds are
   what puts every field of aCell inside a signed short. The atlas texel size arrives as the same
   `uTexel` float the CPU used to multiply by, so the UVs are the same product
   of the same two operands. What changes is only the cost — four shorts a
   cell against six vertices of six floats — and that is what lets one frame's
   budget cover a 3840x2160 view at the zoom floor. */
/* THIS PASS'S TWO SHADERS, AND NEITHER HAS A C REFERENCE LEFT. They are a
   BUILD INPUT, not dead GL code: `tools/spirv-gen.py` reads them out of the
   PREPROCESSED translation unit under the manifest names tagpu_terr::VS and
   tagpu_terr::FS, and generates the SPIR-V `tagpu_vk_terr.c` draws the terrain
   with -- so deleting either fails the build, and editing one edits the
   terrain the player sees. `tools/spirv-check.sh` re-extracts them through the
   preprocessor on every link and compares the hashes.
   The pragma below is paired and its `pop` is PROVED with a planted probe
   rather than read -- landing 11-4b put one at column 0 inside a comment,
   where it is text and not a directive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aCorner;\n"   /* per vertex: 0/1 x 0/1        */
    "layout(location=1) in vec4 aCell;\n"     /* per instance: col,row,cx,cy  */
    "uniform vec2 uGame;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"
    "uniform float uEnc;\n"
    "uniform vec2 uOrigin;\n"   /* screen px of grid cell (0,0)'s corner      */
    "uniform vec2 uTile0;\n"    /* the map cell grid cell (0,0) IS            */
    "uniform vec2 uTexel;\n"    /* 1/ATLAS_W, 1/atlas height                  */
    "out vec2 vUV; out vec2 vWorld;\n"
    "void main(){\n"
    /* CELL_PITCH 34, CELL_BORDER 1, TILE_PX 32 — held to those values by
       terr_atlas_consts_unchanged in tagpu_terr.c */
    "  vec2 g = aCell.xy + aCorner;\n"
    "  vec2 aPos = uOrigin + g * 32.0;\n"
    "  vec2 aWorld = (uTile0 + g) * 32.0;\n"
    "  vec2 aUV = (aCell.zw * 34.0 + 1.0 + aCorner * 32.0) * uTexel;\n"
    "  vec2 p = (aPos - uZoomC) * uZoom + uZoomC - vec2(" TAGPU_EDGE_NUDGE ");\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - uEnc/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vUV = aUV; vWorld = aWorld;\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; in vec2 vWorld;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAtlas;\n"
    "uniform sampler2D uPal;\n"
    "uniform sampler2D uAtlasRGB;\n"   /* Classic++: the restored atlas    */
    "uniform int uRestored;\n"         /* 1 = a restore is running or done:
                                          sample it where its alpha says so */
    "uniform sampler2D uHeight;\n"     /* Classic++: R8 height per 16-px cell */
    "uniform vec2 uHDim;\n"            /* its size: mapW16, mapH16; 0 = none */
    TAGPU_GLSL_FOG_UNIFORMS
    TAGPU_GLSL_FOG_FN
    TAGPU_GLSL_LIGHT_UNIFORMS
    TAGPU_GLSL_SHADOW_UNIFORMS
    TAGPU_GLSL_LIGHT_FN
    /* the lab's normalAt (tascene-view.html): at a grid point, central
       differences of the height over 32 world units, coordinates clamped to
       the map; elevation and x/z are the same world units. The height is the
       engine's byte, so `* 255.0` gives it back exactly. */
    "float taH(ivec2 p){\n"
    "  p = clamp(p, ivec2(0), ivec2(uHDim) - 1);\n"
    "  return texelFetch(uHeight, p, 0).r * 255.0;\n"
    "}\n"
    "vec3 taGridN(ivec2 p){\n"
    "  float dx = (taH(p + ivec2(1, 0)) - taH(p - ivec2(1, 0))) * (1.0/32.0);\n"
    "  float dz = (taH(p + ivec2(0, 1)) - taH(p - ivec2(0, 1))) * (1.0/32.0);\n"
    "  return normalize(vec3(-dx, 1.0, -dz));\n"
    "}\n"
    /* the lab's terrain normal AT THIS FRAGMENT: the four grid-point normals
       of the 16-px cell it is in, interpolated exactly as the lab's two
       triangles per cell interpolate them (the diagonal runs (1,0)-(0,1)),
       so the field is the one the owner looked at, not a bilinear cousin */
    "vec3 taTerrN(vec2 w){\n"
    "  vec2 g = w * (1.0/16.0);\n"
    "  vec2 f = floor(g), t = g - f;\n"
    "  ivec2 c = ivec2(f);\n"
    "  vec3 n00 = taGridN(c), n10 = taGridN(c + ivec2(1, 0));\n"
    "  vec3 n01 = taGridN(c + ivec2(0, 1)), n11 = taGridN(c + ivec2(1, 1));\n"
    "  return t.x + t.y <= 1.0\n"
    "    ? n00 + t.x * (n10 - n00) + t.y * (n01 - n00)\n"
    "    : n11 + (1.0 - t.x) * (n01 - n11) + (1.0 - t.y) * (n10 - n11);\n"
    "}\n"
    /* the world point THIS FRAGMENT depicts -- (x, h, z + h/2), the lab's
       terrain vertex (tascene-view.html buildTerrainLab), h the height at
       the fragment over the same two triangles -- and its screen derivatives
       for the shadow's receiver-plane bias. The derivatives are ANALYTIC:
       the height is reconstructed here from the grid, so a dFdx() of it
       would jump at every cell edge (the lab interpolates the point as a
       varying, whose derivative is exact per triangle); the gradient of the
       triangle the fragment is in, times the derivative of vWorld, which IS
       a varying, is the same clean number. */
    "vec3 taTerrW(vec2 w, out vec3 dWdx, out vec3 dWdy){\n"
    "  vec2 g = w * (1.0/16.0);\n"
    "  vec2 f = floor(g), t = g - f;\n"
    "  ivec2 c = ivec2(f);\n"
    "  float h00 = taH(c), h10 = taH(c + ivec2(1, 0));\n"
    "  float h01 = taH(c + ivec2(0, 1)), h11 = taH(c + ivec2(1, 1));\n"
    "  float h, gx, gz;\n"
    "  if (t.x + t.y <= 1.0) {\n"
    "    h = h00 + t.x * (h10 - h00) + t.y * (h01 - h00);\n"
    "    gx = (h10 - h00) * (1.0/16.0); gz = (h01 - h00) * (1.0/16.0);\n"
    "  } else {\n"
    "    h = h11 + (1.0 - t.x) * (h01 - h11) + (1.0 - t.y) * (h10 - h11);\n"
    "    gx = (h11 - h01) * (1.0/16.0); gz = (h11 - h10) * (1.0/16.0);\n"
    "  }\n"
    "  vec2 sx = dFdx(w), sy = dFdy(w);\n"
    "  float hx = gx * sx.x + gz * sx.y, hy = gx * sy.x + gz * sy.y;\n"
    "  dWdx = vec3(sx.x, hx, sx.y + 0.5 * hx);\n"
    "  dWdy = vec3(sy.x, hy, sy.y + 0.5 * hy);\n"
    "  return vec3(w.x, h, w.y + 0.5 * h);\n"
    "}\n"
    "void main(){\n"
    /* the depicted world point and its derivatives FIRST, while the flow is
       still uniform (tagpu_glsl.h, the shadow half) */
    "  vec3 taWx = vec3(0.0), taWy = vec3(0.0);\n"
    "  vec3 taW = uHDim.x > 0.5 ? taTerrW(vWorld, taWx, taWy) : vec3(vWorld.x, 0.0, vWorld.y);\n"
    /* terrain is the bottom layer: it paints the fog's black instead of
       discarding, and it darkens (never hides) in grey — the engine's rule */
    TAGPU_GLSL_FOG_TERRAIN
    /* Classic++ (uLit): the restored colour where the reveal has painted it
       -- alpha is the restorer's own "painted" mark (see s_rgbTex), a cell it
       has not reached yet is alpha 0 -- and the palette's colour elsewhere,
       so the reveal goes lit-indexed to lit-restored; lit by the lab's rule
       from the heightfield normal; then the grey band as the RGB rule
       (renderers.md 2.6) rather than the index LUT. Nothing below this
       branch runs under Classic++, nothing in it runs under Classic. */
    "  if (uLit == 1) {\n"
    "    vec4 t = uRestored == 1 ? texture(uAtlasRGB, vUV) : vec4(0.0);\n"
    "    vec3 c = t.a > 0.5 ? t.rgb\n"
    "           : texelFetch(uPal, ivec2(int(texture(uAtlas, vUV).r * 255.0 + 0.5), 0), 0).rgb;\n"
    "    if (uHDim.x > 0.5) c *= taLambert(uLambert == 1 ? taTerrN(vWorld) : vec3(0.0, 1.0, 0.0),\n"
    "                                     taW, taWx, taWy);\n"
    TAGPU_GLSL_FOG_GREY_RGB("c")
    "    frag = vec4(c, 1.0); return;\n"
    "  }\n"
    /* NEAREST on an R8 atlas: the texel IS the palette index, and the quad
       spans exactly the tile's 32 texels, so no colour key and no filtering to
       get wrong. A fragment landing exactly on the far edge reads the cell's
       replicated guard texel rather than the next cell (CELL_PITCH). */
    "  int pi = int(texture(uAtlas, vUV).r * 255.0 + 0.5);\n"
    TAGPU_GLSL_FOG_SHADE("pi")
    "  frag = vec4(texelFetch(uPal, ivec2(pi, 0), 0).rgb, 1.0);\n"
    "}\n";
#pragma GCC diagnostic pop

void tagpu_terr_glreset(void)
{
    /* and the hand-over goes with the context: its texel pointers name textures
       that no longer exist and its cells a frame that will not be drawn. The
       feature pass has carried this line since it was written; this one did
       not, and the asymmetry was in the G19e diff. */
    s_pubHave = 0; s_abFrame = 0;
    s_atlasBuilt = 0;                   /* rebuilt on the next gather          */
    s_hMeshW = s_hMeshH = 0;
    /* the mirror goes with the mesh it mirrors: keeping it would hand the
       Vulkan lane vertices for a grid this pass is about to rebuild */
    free(s_hMeshV); s_hMeshV = NULL;
    free(s_hMeshI); s_hMeshI = NULL;
    s_hMeshVN = s_hMeshIN = 0;
    s_hMeshNoMirror = 0;
    s_hW = s_hH = 0; s_hGrid = NULL; s_hFrame = 0;
    s_rectValid = 0;
    /* The set identity (s_setPtr/s_setCount/s_setPix) is LEFT ALONE: zeroing
       s_atlasBuilt is what forces the atlas rebuild, and the identity's job is
       to tell a NEW MAP from the same set (ensure_atlas drops a standing
       restore request on a new map). The request does not survive the reset
       either -- the image it was painted into was the consumer's and went with
       the device -- so it is published again and the consumer restores from
       scratch rather than being handed a serial it has already seen. */
    s_rgbState = 0;
    rlist_drop();
}

/* ---- the height grid: one R8 texel per 16-px cell, once per map ----
   Keyed on ITS OWN inputs -- the grid pointer, the dims, and the tile set the
   atlas was built from (LoadMap could hand a same-sized map the same
   allocation, and the set's identity is what this module already trusts to
   say "new map") -- and re-checked every frame by ensure_height, because the
   set can be ready a frame before the grid is. A build that fails leaves
   s_hW 0, which is what the render gates the lambert on (never s_hTex: a
   previous map's texture is still a live id), and is retried every 60 frames
   until it succeeds or the inputs change. Without a grid Classic++ terrain
   draws UNLIT -- the restored colour and the grey rule stay, only the
   lambert is skipped (uHDim 0 in the shader). */
static void build_hills(const unsigned char* buf, int w, int h);
static void build_height(const char* ta, unsigned frame)
{
    const char* grid = *(const char* const*)(ta + OFF_FEATMAP);
    int w = *(const int*)(ta + OFF_MAPW16), h = *(const int*)(ta + OFF_MAPH16);
    unsigned char* buf;
    int r, c;
    char b[160];
    s_hW = s_hH = 0;
    /* and the mirror goes with the dimensions, so that "s_hMirror is non-NULL"
       and "it is s_hW x s_hH bytes" are one fact rather than two: every exit
       below this point either fails, leaving no mirror, or installs one at the
       size it also sets. */
    free(s_hMirror); s_hMirror = NULL;
    s_hGrid = grid; s_hSet = s_setPtr; s_hFrame = frame;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || w > s_maxTex || h > s_maxTex) {
        flog("terr: height grid dims out of range -- Classic++ terrain draws unlit");
        return;
    }
    if (!ptr_ok(grid) || IsBadReadPtr(grid, (SIZE_T)w * (SIZE_T)h * FT_STRIDE)) {
        flog("terr: height grid unreadable -- Classic++ terrain draws unlit (retried)");
        return;
    }
    buf = (unsigned char*)malloc((size_t)w * (size_t)h);
    if (!buf) return;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            buf[(size_t)r * w + c] =
                *(const unsigned char*)(grid + ((size_t)r * w + c) * FT_STRIDE + FT_HEIGHT);
    build_hills(buf, w, h);          /* TODO: unconditional -- 19 MB even when
                                        terrainshadow=0, which is the default.
                                        See build_hills' header. */
    /* THE MIRROR IS THE BUFFER (Phase G / G19e), exactly as the atlas's: the
       memory the loop above filled, kept instead of freed, so the Vulkan lane's
       uHeight is those texels rather than a second read of the engine's grid.
       0.5 MB at 512x512 cells, 16 MB at the 4096 ceiling, and paid for only
       while the Vulkan lane is armed. `s_hW`/`s_hH` reach the consumer through
       the hand-over's `hDimW`/`hDimH`, and build_height keeps those in step
       with the mirror -- see the free at the top of this function. */
    if (s_mirrorWant) {
        free(s_hMirror);
        s_hMirror = buf;
        s_hMirrorSerial++;
    } else {
        free(s_hMirror);
        s_hMirror = NULL;
        free(buf);
    }
    s_hW = w; s_hH = h;
    _snprintf(b, sizeof b, "terr: height grid %dx%d uploaded from %p (Classic++ lighting)",
              w, h, (const void*)grid);
    flog(b);
}

/* ---- the heightfield as a caster (G14i, renderers.md 2.8, 2.12) ----
   One vertex per grid point at the world point the lab's terrain vertex
   depicts -- (c*16, h, r*16 + h/2) -- and two triangles per cell on the
   diagonal taTerrN interpolates across ((1,0)-(0,1)), indices ordered by
   cell row so the rows under the light window are one contiguous range.
   Static: the map's heights never change. Two Continents: 537,600 vertices
   (6.4 MB), 3.2 M indices (12.9 MB), once per map.

   ==== TODO (IMPORTANT), 2026-09-09: 19 MB of VRAM per map for a pass that is
   OFF BY DEFAULT and normally never draws. ====
   `terrainshadow` now defaults to 0 (tagpu_classicpp.c shadow_defaults --
   the ground self-shadowed itself, renderers.md 2.7b), and the only caller of
   tagpu_terr_hills_draw is gated on it (tagpu_shadow.c:398). This function is
   NOT gated: build_height calls it unconditionally, so every map pays 6.4 MB
   of vertices and 12.9 MB of indices that nothing reads.

   Do NOT fix it by gating the build on the flag. The flag is live -- the cfg
   is re-read while the game runs (read_cfg, and the render-options screen
   triggers it) -- so `terrainshadow=1` mid-session must still produce a mesh,
   and that is the fixture the eventual shadow fix gets measured in.

   Build it LAZILY instead, on the first tagpu_terr_hills_draw after the grid
   changed. The obstacle is that `buf` is freed at the end of build_height, so
   the lazy path needs the bytes: either keep that w*h byte buffer alive (0.5 MB
   on Two Continents, 3 % of what it replaces) or re-read the engine grid at
   OFF_FEATMAP with the same ptr_ok/IsBadReadPtr guard build_height uses. Keep
   the existing s_hMeshW/s_hMeshH == s_hW/s_hH check in the draw so a failed
   rebuild still refuses rather than indexing past the old mesh. */
static void build_hills(const unsigned char* buf, int w, int h)
{
    size_t nv = (size_t)w * (size_t)h, ni = (size_t)(w - 1) * (size_t)(h - 1) * 6, k = 0;
    float* vb; unsigned* ib;
    int r, c;
    char b[160];
    /* THE TWO EXITS THAT LEAVE NO MESH LEAVE NO MIRROR EITHER, and say so, or
       ensure_height's mirror term would ask for this rebuild on every frame of
       the map. */
    if (w < 2 || h < 2) { s_hMeshNoMirror = 1; return; }
    vb = (float*)malloc(nv * 3 * sizeof(float));
    ib = (unsigned*)malloc(ni * sizeof(unsigned));
    if (!vb || !ib) { free(vb); free(ib); flog("terr: hills: out of memory");
                      s_hMeshNoMirror = 1; return; }
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            float hh = (float)buf[(size_t)r * w + c];
            float* o = vb + ((size_t)r * w + c) * 3;
            o[0] = (float)(c * 16); o[1] = hh; o[2] = (float)(r * 16) + hh * 0.5f;
        }
    for (r = 0; r < h - 1; r++)
        for (c = 0; c < w - 1; c++) {
            unsigned i00 = (unsigned)(r * w + c), i10 = i00 + 1u;
            unsigned i01 = i00 + (unsigned)w, i11 = i01 + 1u;
            ib[k++] = i00; ib[k++] = i10; ib[k++] = i01;
            ib[k++] = i11; ib[k++] = i01; ib[k++] = i10;
        }
    /* THE MESH IS THE PASS, exactly as the atlas and the height grid are: `vb`
       and `ib` are what the consumer draws, and `s_hMeshSerial` is what it
       checks, so they are kept rather than freed and only while the Vulkan lane
       is armed. Every exit above this point has already returned, so "the
       pointers are non-NULL" and "they are s_hMeshVN/s_hMeshIN long" are one
       fact rather than two. */
    free(s_hMeshV); free(s_hMeshI);
    if (s_mirrorWant) {
        s_hMeshV = vb; s_hMeshI = ib;
        s_hMeshVN = nv; s_hMeshIN = ni;
        s_hMeshSerial++;
        s_hMeshNoMirror = 0;
    } else {
        s_hMeshV = NULL; s_hMeshI = NULL;
        s_hMeshVN = s_hMeshIN = 0;
        free(vb); free(ib);
    }
    s_hMeshW = w; s_hMeshH = h;
    _snprintf(b, sizeof b, "terr: hills mesh %dx%d grid points, %u cells (Classic++ shadows)",
              w, h, (unsigned)((w - 1) * (h - 1)));
    flog(b);
}

int tagpu_terr_hills_draw(int r0, int r1, TAGPU_TERRHILLS* out)
{
    int cells = s_hMeshW - 1, rows = s_hMeshH - 1;
    size_t first, count;
    if (out) memset(out, 0, sizeof *out);
    /* ONLY THE MESH BUILT FROM THIS GRID: after a map change whose rebuild
       failed (too small, out of memory) the old mesh is still what the arrays
       hold, and the new grid's size would index past it. Both terms are the
       MESH's own.
       There used to be a third, `!s_hVao` -- a GL vertex-array name, created
       only on a lane that had a GL context. It sat above the publication of
       the very mirror this function exists to hand over, so on a lane with no
       GL this returned 0 before reaching it and the Vulkan shadow pass was
       handed nothing, for a reason that had nothing to do with the mesh.
       [The vulkan-only plan, landing 11-5c.] */
    if (s_hMeshW < 2 || s_hMeshH < 2) return 0;
    if (s_hMeshW != s_hW || s_hMeshH != s_hH) return 0;
    if (r0 < 0) r0 = 0;
    if (r1 > rows - 1) r1 = rows - 1;
    if (r1 < r0) return 0;
    first = (size_t)r0 * (size_t)cells * 6u;
    count = (size_t)(r1 - r0 + 1) * (size_t)cells * 6u;
    /* THE RANGE, computed once, here, by the code that owns the mesh: the
       Vulkan shadow pass draws this and nothing else, so the clamp is not
       evaluated a second time at the far end. Only published with the mirror the range
       indexes into, and only when the range is inside it -- `first + count`
       is at most `rows * cells * 6` by the clamp above, and the bound is
       re-checked rather than argued because the two are separate mallocs. */
    if (out && s_hMeshV && s_hMeshI && first + count <= s_hMeshIN) {
        out->v = s_hMeshV;   out->nv = s_hMeshVN;
        out->idx = s_hMeshI; out->ni = s_hMeshIN;
        out->serial = s_hMeshSerial;
        out->firstIndex = (unsigned)first;
        out->indexCount = (unsigned)count;
    }
    return 1;
}

/* once per frame after the atlas is known: (re)build when the inputs moved,
   or when the last attempt failed and 60 frames have passed */
static void ensure_height(const char* ta, unsigned frame)
{
    const char* grid = *(const char* const*)(ta + OFF_FEATMAP);
    int w = *(const int*)(ta + OFF_MAPW16), h = *(const int*)(ta + OFF_MAPH16);
    int same = s_hGrid == (const void*)grid && s_hSet == s_setPtr;
    /* the mirror term is `ensure_atlas`'s, for the same reason: the only way to
       obtain one for a grid that is already uploaded is to build it again, once */
    if (s_hW > 0 && same && s_hW == w && s_hH == h &&
        (!s_mirrorWant || (s_hMirror && (s_hMeshV || s_hMeshNoMirror)))) return;
    if (s_hW == 0 && same && s_hFrame != 0 && frame - s_hFrame < 60) return;
    build_height(ta, frame);
}

/* ---- the atlas: built once per map, never per frame ---- */
static int ensure_atlas(const char* ta)
{
    /* Per map: LoadMap 0x483610 builds the set and stores its pointer at
       0x483B68, and the map-free routine 0x483DD0 nulls it (0x483ECA) inside
       the teardown cascade tagpu_reclaim fences. That lifetime is the argument
       for the two probes below; they are sanity nets, and the identity test
       is what keeps the megabyte one off the per-frame path (cross-thread-
       engine-reads.md §4). */
    const int* set = *(const int* const*)(ta + OFF_TILESET);
    const unsigned char* pix;
    unsigned char* buf;
    int count, rows, h, i;
    char b[192];

    if (!ptr_ok(set) || IsBadReadPtr((void*)set, 8)) return 0;
    count = set[0];
    pix = (const unsigned char*)(size_t)(unsigned)set[1];
    if (count <= 0 || count > MAX_TILES) return 0;
    /* the identity test FIRST: the probe below walks megabytes of tile art and
       this runs every frame.
       THE MIRROR TERM IS WHAT LETS THE VULKAN LANE ARM LATE (Phase G / G19e).
       The mirror is the buffer this function is about to build and upload from,
       so the only way to obtain one for an atlas that is already built is to
       build it again -- once, on the first frame after the lane arms. Every
       later frame takes the early return as before. */
    if (s_atlasBuilt && s_setPtr == (const void*)set && s_setCount == count &&
        (!s_mirrorWant || s_atlasMirror)) return 1;
    if (!ptr_ok(pix) || IsBadReadPtr((void*)pix, (SIZE_T)count * TILE_BYTES)) return 0;

    rows = (count + ATLAS_COLS - 1) / ATLAS_COLS;
    h = rows * CELL_PITCH;
    if (h > s_maxTex) {                 /* keep what fits; the rest draw black */
        rows = s_maxTex / CELL_PITCH;
        h = rows * CELL_PITCH;
        _snprintf(b, sizeof b, "terr: tile set %d exceeds the atlas (%dx%d max) — %d kept",
                  count, ATLAS_W, s_maxTex, rows * ATLAS_COLS);
        flog(b);
    }
    buf = (unsigned char*)calloc((size_t)ATLAS_W * (size_t)h, 1);
    if (!buf) { flog("terr: atlas alloc failed"); return 0; }
    for (i = 0; i < count && i < rows * ATLAS_COLS; i++) {
        const unsigned char* src = pix + (size_t)i * TILE_BYTES;
        unsigned char* cell = buf + (size_t)(i / ATLAS_COLS) * CELL_PITCH * ATLAS_W
                                  + (size_t)(i % ATLAS_COLS) * CELL_PITCH;
        unsigned char* dst = cell + (size_t)CELL_BORDER * ATLAS_W + CELL_BORDER;
        int r;
        for (r = 0; r < TILE_PX; r++) {
            unsigned char* row = dst + (size_t)r * ATLAS_W;
            memcpy(row, src + (size_t)r * TILE_PX, TILE_PX);
            row[-1] = row[0];                       /* left  border column */
            row[TILE_PX] = row[TILE_PX - 1];        /* right border column */
        }
        memcpy(cell, dst - CELL_BORDER, CELL_PITCH);                    /* top    */
        memcpy(cell + (size_t)(CELL_PITCH - 1) * ATLAS_W,               /* bottom */
               dst + (size_t)(TILE_PX - 1) * ATLAS_W - CELL_BORDER, CELL_PITCH);
    }
    /* THE MIRROR IS THE BUFFER, NOT A COPY OF IT. Keeping `buf` here rather
       than freeing it is the whole of this pass's answer to "how does the
       consumer get these texels" (tagpu_terr.h): it is correct from the instant
       it exists, because it is the very memory the loop above filled, and
       nothing writes it again -- the tile set is built by LoadMap and never
       changes.
       ~5.9 MB on Two Continents, paid for only while the Vulkan lane is armed.
       The serial is bumped whether or not the bytes differ from the last
       build's: a rebuild means a new allocation, and the Vulkan lane's upload
       test is about the buffer it last read, not about its contents. */
    if (s_mirrorWant) {
        free(s_atlasMirror);
        s_atlasMirror = buf;
        s_atlasMirrorSerial++;
    } else {
        free(s_atlasMirror);
        s_atlasMirror = NULL;
        free(buf);
    }

    s_atlasH = h;
    s_atlasN = count < rows * ATLAS_COLS ? count : rows * ATLAS_COLS;
    /* a different set is a new map: drop a restore still running on the old
       one and start over. (A GL reset reaches here with the SAME set, after
       glreset has already reset the restore, so both calls are no-ops then.) */
    if (s_setPtr != (const void*)set || s_setCount != count || s_setPix != pix) {
        s_rgbState = 0;
        rlist_drop();
    }
    s_setPtr = (const void*)set; s_setCount = count; s_setPix = pix;
    s_atlasBuilt = 1;
    _snprintf(b, sizeof b, "terr: atlas built %dx%d for %d tiles (set=%p pix=%p, %d KB)",
              ATLAS_W, h, count, (void*)set, (void*)pix, (count * TILE_BYTES) >> 10);
    flog(b);
    return 1;
}

/* ---- Classic++: the GLSL restorer, straight into s_rgbTex ----
   Every tile of the set as a frame, visible ones first, and since the cells
   show as they land (s_rgbTex) the order is what the player watches: a tile's
   rank is the Chebyshev distance in cells from the CENTRE of the last gathered
   rect to the nearest map cell that uses it, so the reveal radiates from the
   middle of the screen, reaches the viewport's edge at rank ~half its span and
   carries on outward across the map at the same pace. (Ranking the whole rect
   0, as the one-flip version did, restored the visible cells in tile-index
   order -- a scatter.) The set holds every tile the map references and nothing
   else, so no tile is left unranked, but an unreferenced one would simply go
   last. */
static int* restore_order(const char* ta, int count)
{
    const unsigned short* tmap = *(const unsigned short* const*)(ta + OFF_TILEMAP);
    int mapW16 = *(const int*)(ta + OFF_MAPW16), mapH16 = *(const int*)(ta + OFF_MAPH16);
    int stride = mapW16 / 2, mrows = mapH16 / 2, my, mx, i, maxRank = 0, *rank, *order, *bucket, *next;
    int cx = s_rectTx0 + s_rectCols / 2, cy = s_rectTy0 + s_rectRows / 2;
    if (!ptr_ok(tmap) || stride <= 0 || mrows <= 0 || stride > 2048 || mrows > 2048) return NULL;
    rank = (int*)malloc((size_t)count * sizeof *rank);
    order = (int*)malloc((size_t)count * sizeof *order);
    if (!rank || !order) { free(rank); free(order); return NULL; }
    for (i = 0; i < count; i++) rank[i] = 8192;
    for (my = 0; my < mrows; my++) {
        int dy = my < cy ? cy - my : my - cy;
        for (mx = 0; mx < stride; mx++) {
            int dx = mx < cx ? cx - mx : mx - cx;
            int d = dx > dy ? dx : dy, idx = tmap[(size_t)my * stride + mx];
            if (idx < count && d < rank[idx]) rank[idx] = d;
        }
    }
    for (i = 0; i < count; i++) if (rank[i] > maxRank) maxRank = rank[i];
    /* counting sort by rank, stable in tile order */
    bucket = (int*)calloc((size_t)maxRank + 2, sizeof *bucket);
    next = (int*)calloc((size_t)maxRank + 2, sizeof *next);
    if (!bucket || !next) { free(bucket); free(next); free(rank); free(order); return NULL; }
    for (i = 0; i < count; i++) bucket[rank[i] + 1]++;
    for (i = 1; i <= maxRank + 1; i++) bucket[i] += bucket[i - 1];
    for (i = 0; i < count; i++) order[bucket[rank[i]] + next[rank[i]]++] = i;
    free(bucket); free(next); free(rank);
    return order;
}

/* `repaint`: the atlas is already restored and only the palette moved, so the
   destination keeps what it holds and every tile is queued again over it --
   the terrain recolours centre-out instead of blanking for the seconds a
   whole restore takes. Which of the two it is is the consumer's to act on;
   this side only says which. */
static int restore_publish(const char* ta, int repaint)
{
    /* the ART's palette for the tileability test; the SCREEN's goes out
       separately as `s_pub.pal` and is the restore's own input */
    const unsigned char* art = tagpu_pal_engine();
    int n = s_atlasN, i;
    int* order;
    TAGPU_RGLSL_FRAME* frames;
    order = restore_order(ta, n);
    frames = (TAGPU_RGLSL_FRAME*)malloc((size_t)n * sizeof *frames);
    if (!order || !frames) { free(order); free(frames); flog("terr: restore order alloc failed"); return 0; }
    for (i = 0; i < n; i++) {
        int t = order[i];
        TAGPU_RGLSL_FRAME* f = &frames[i];
        f->ax = f->dx = (t % ATLAS_COLS) * CELL_PITCH + CELL_BORDER;
        f->ay = f->dy = (t / ATLAS_COLS) * CELL_PITCH + CELL_BORDER;
        f->w = f->h = TILE_PX; f->border = CELL_BORDER; f->key = -1;   /* tiles are opaque */
        f->padR = f->padB = 0;                                          /* no alignment slack */
        f->wrap = art ? tagpu_rglsl_tileable(s_setPix + (size_t)t * TILE_BYTES, TILE_PX, TILE_PX, art, -1) : 0;
    }
    free(order);
    /* THE LIST IS THE HAND-OVER, so it is retained rather than freed and the
       serial is bumped by the drop -- a DROP IS NEWS, it is how a consumer
       learns the atlas it was painting is not this map's. Ownership stays
       ours; the consumer copies what it needs. */
    rlist_drop();                      /* whatever was there is the old list */
    s_rFrames = frames;
    s_rFrameN = n;
    s_rRepaint = repaint;
    s_rgbPalSerial = tagpu_pal_serial();
    if (s_log) {
        char b[160];
        _snprintf(b, sizeof b, "terr: restore request published -- %d frames, "
                  "%dx%d atlas, serial %u%s",
                  n, ATLAS_W, s_atlasH, s_rSerial, repaint ? ", repaint" : "");
        flog(b);
    }
    return 1;
}

/* Once per frame after the atlas is known: publish the restore request when
   Classic++ is on and none stands for this tile set, then publish it again
   when the palette moves under it.

   THE PAINTING IS THE CONSUMER'S; THE ORDER IS OURS. This side owns
   `restore_order` above -- the centre-out reveal the player watches -- and the
   frame list; the Vulkan pass owns the image, the job and the progress
   (tagpu_vk_terr.c). Until landing 11-5c this whole route was UNREACHABLE and
   had been since 4b-2: the list was published from inside the GL bring-up's
   `glsl_begin`, and the driver in front of it returned at `!s_atlasTex` -- a
   GL texture name that is only ever created on a lane with a GL context. So
   `tagpu_restorevk.on` armed a consumer that was never sent anything, and
   `restored` below -- keyed on the GL restore's own state machine -- could
   only ever publish 0. One guard on a GL name, and a Vulkan-lane feature that
   read as "off". [The vulkan-only plan, landing 11-5c.] */
static void restore_step(const char* ta)
{
    /* the request is only worth building when something asked for it: without
       `tagpu_restorevk.on` there is no painter on this lane and the terrain
       draws indexed, which is the default and stays it */
    if (!s_rvkWant) return;
    if (!tagpu_classicpp_assets() || !s_atlasBuilt || !s_setPix) return;
    if (s_rgbState == 0) {
        if (!s_rectValid) return;          /* the order wants a viewport: next frame */
        if (!tagpu_pal_live()) return;     /* ...and a palette: next frame  */
        if (!restore_publish(ta, 0)) {
            s_rgbState = -1;
            flog("terr: the restore request could not be built; Classic++ terrain stays indexed");
            return;
        }
        s_rgbState = 1;
        return;
    }
    /* Published, and then the palette moved under it (the Gamma option, or
       `+gamma N`): the tiles hold the brightness the old palette gave them
       while the engine's own pixels beside them moved. Queue them all again
       over the image that is there. */
    if (s_rgbState == 1 && s_rgbPalSerial != tagpu_pal_serial()) {
        char b[128];
        if (!s_rectValid) return;          /* restore_order wants a viewport: next frame */
        if (!restore_publish(ta, 1)) {
            s_rgbPalSerial = tagpu_pal_serial();   /* do not retry every frame */
            flog("terr: palette changed but the repaint request could not be built; the atlas keeps the old colours");
            return;
        }
        _snprintf(b, sizeof b, "terr: palette changed (serial=%u): %d tiles queued for repaint",
                  s_rgbPalSerial, s_atlasN);
        flog(b);
    }
}

/* The cells a rect of `w` x `h` game px can cost the gather. The +2 is the
   gather's own worst case: ceil32 of the size plus a fractional eye offset
   costs one extra column and one extra row. */
static int span_cells(int w, int h)
{
    long cols = (long)w / 32 + 2, rows = (long)h / 32 + 2;
    long n = cols * rows;                    /* < 2^31 for any believed w,h */
    return n > 0 ? (int)n : 0;
}

/* Grow the staging to hold `cells`, never shrink. Render thread, and in
   practice once per resolution: the reservation is made for the widest rect
   the VIEWPORT can produce, not for this frame's rect, so zooming never
   allocates. A refusal — the memory guard, or a failed realloc — leaves the
   old buffer intact and is not an error here: the caller trims the rect to
   what is held, which costs a black margin and not a dropped frame. */
static void terr_reserve(int cells)
{
    short* p;
    size_t bytes;
    if (cells <= s_instCells) return;
    if (cells > INST_MAX_CELLS) cells = INST_MAX_CELLS;
    if (cells <= s_instCells) return;
    bytes = (size_t)cells * ICOMP * sizeof(short);
    p = (short*)realloc(s_inst, bytes);
    if (!p) {
        char b[128];
        _snprintf(b, sizeof b, "terr: could not reserve %d cells (%u KB) —"
                               " the view is trimmed to the %d held",
                  cells, (unsigned)(bytes / 1024), s_instCells);
        flog(b);
        return;
    }
    {
        char b[128];
        _snprintf(b, sizeof b, "terr: staging %d -> %d cells (%u KB)",
                  s_instCells, cells, (unsigned)(bytes / 1024));
        flog(b);
    }
    s_inst = p; s_instCells = cells;
}

/* The one budget every pass has to agree on — see tagpu_terr.h for the
   contract. `tagpu_terr_gather` bails when the rect it is asked for needs more
   cells than the staging holds, and a bail HANDS THE DRAW BACK: one frame of
   the engine's own terrain under an inverted composite, which reads as a
   flash. Bailing is the one behaviour that looks like a bug, so the rect is
   trimmed to what can actually be drawn BEFORE any pass sizes itself from it
   (tagpu_native.c), leaving terrain, units, wrecks and features one rect.

   The reservation is made from the VIEWPORT at the zoom floor, so on any
   screen whose viewport is believed the trim below finds nothing to do and no
   view is ever drawn short. It runs anyway, because the reservation can be
   refused and the gather's bail must never be what a player sees.

   Shrink is proportional and iterated rather than solved: the rect keeps its
   aspect, so the margin is even on all four sides, and no square root is needed
   for a loop that converges in three passes at any sane viewport. */
void tagpu_terr_clamp_span(int vw, int vh, int* w, int* h)
{
    int guard = 64;
    if (*w < 32) *w = 32;
    if (*h < 32) *h = 32;
    /* the widest rect this viewport can ever ask for: itself at the zoom floor.
       vw/vh are the true 1x viewport, already range-checked by the caller. */
    if (vw > 0 && vh > 0)
        terr_reserve(span_cells((int)((float)vw / TAGPU_ZOOM_MIN) + 64,
                                (int)((float)vh / TAGPU_ZOOM_MIN) + 64));
    while (guard-- > 0) {
        if (span_cells(*w, *h) <= s_instCells) return;
        *w -= *w / 32 + 1;
        *h -= *h / 32 + 1;
    }
}

/* the engine's own arithmetic: `cdq; and edx,0x1f; add; sar 5` is a division
   toward zero, not a floor — reproduce it, do not "fix" it */
static int div32_trunc(int v) { return (v + (v < 0 ? 31 : 0)) >> 5; }
static int ceil32(int v)      { int q = div32_trunc(v); return (v - q * 32) ? q + 1 : q; }

/* one visible cell: where it is in this frame's grid, and where its tile is in
   the atlas. The shader turns the pair into the six vertices. */
static void put_cell(int col, int row, int cx, int cy)
{
    short* o = s_inst + (size_t)s_ncell * ICOMP;
    o[0] = (short)col; o[1] = (short)row; o[2] = (short)cx; o[3] = (short)cy;
    s_ncell++;
}

/* Every exit that draws nothing must hand the draw back: the skip byte is
   latched from the previous frame, so returning early with it set would leave
   the viewport flat key-colour until the 90-frame watchdog notices. */
static int terr_bail(void)
{
    /* A FRAME THAT GATHERS NOTHING HANDS NOTHING OVER. `tagpu_terr_render` is
       where this used to be done, and tagpu_native.c calls that only when the
       gather returned cells -- so every bail below left the PREVIOUS frame's
       hand-over standing, pointing at a tile atlas `ensure_atlas` may have
       freed on the way past. The frame stamp bounds it either way; this makes
       the flag tell the truth as well, which is what tagpu_feat_glreset and
       tagpu_scaffold_frame already did. [G19e RE-REVIEW, 2026-09-15.] */
    s_pubHave = 0; s_abFrame = 0;
    tagpu_terrown_set_skip(0);
    return 0;
}

#define TA_MAINPP 0x00511DE8u   /* this file's own read (fenced: the tile set and map) */

int tagpu_terr_gather(const TAGPU_FXVIEW* v)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    const unsigned short* tmap;
    int mapW16, mapH16, stride, mrows;
    int tx0, ty0, fx, fy, cols, rows, r, c;
    int skipped = 0, junk = 0, own, wasFilled, emit;
    int eyeX, eyeY, vpL, vpT, evw, evh;
    float iw, ih;

    if (s_armed != 1) return terr_bail();
    if (!ptr_ok(ta)) return terr_bail();
    /* THE ATLAS BOUND IS A DEVICE LIMIT, so it is asked of the device that will
       sample it. A GL bring-up stood in front of this and had to be gated: it
       built this pass's program on first sight and resolved entry points, so on
       the vulkan-only lane it logged `terr: missing GL proc`, latched a failed
       state and killed the pass for the process, GATHER INCLUDED -- measured
       2026-09-18 by running it, the hand-over never fired and the capture's
       target was not even unlinked. It went in landing 11-5c and what is left
       is the bound. `ensure_atlas` was never the gated part: it builds the tile
       mirror the hand-over carries and gated only its own upload.
       [The vulkan-only plan, landings 4b-2 and 11-5c.] */
    if (s_maxTex <= 0 || s_maxTex != tagpu_vk_max_image_dim()) {
        /* The bring-up read GL_MAX_TEXTURE_SIZE into `s_maxTex`; with no GL
           context that never happened and `s_maxTex` stayed 0, so
           `ensure_atlas` kept `s_maxTex / CELL_PITCH` = 0 rows and cached a
           zero-row atlas for the life of the process.
           REFUSED RATHER THAN GUESSED while the lane is still coming up: 0 from
           `tagpu_vk_max_image_dim` means "no device yet", the gather hands the
           draw back for those few frames exactly as it does for any other
           missing input, and the atlas is built once the real limit is known.
           Measured 2026-09-18: `terr: atlas built 2176x0 ... 0 kept`. */
        int m = tagpu_vk_max_image_dim();
        if (m <= 0) return terr_bail();
        /* AND A CHANGED BOUND INVALIDATES THE ATLAS. The accessor re-asks when
           the GPU picker re-picks a physical device; if the new one is smaller,
           an atlas laid out against the old bound is one the twin's image
           creation will refuse, and `s_atlasBuilt` would otherwise keep it for
           the session. Rebuilding is what the GL lane does on a context change
           and costs the same. [FROM THE 4b-2 LANDING REVIEW.] */
        if (s_maxTex > 0 && m != s_maxTex) s_atlasBuilt = 0;
        s_maxTex = m;
    }
    if (!ensure_atlas(ta)) return terr_bail();
    ensure_height(ta, v->frame_counter);
    restore_step(ta);

    s_ncell = 0;
    /* Read BEFORE touching the skip: it says whether the engine frame we are
       about to composite over is the key fill rather than a terrain blit. */
    wasFilled = tagpu_terrown_filled();
    /* Emitting without owning the draw is only ever right as a deliberate A/B
       (`over`) or for the one frame after we hand the draw back — our terrain
       is opaque and covers the whole viewport, so any other time it would hide
       the health bars, wireframes, build cursor and chat that the composite's
       key test exists to let through. Without the patch installed there IS no
       key, so the pass counts and says so rather than blanking the overlays. */
    own = !s_passive && !s_over && tagpu_terrown_installed();
    emit = own || s_over || wasFilled;

    tmap = *(const unsigned short* const*)(ta + OFF_TILEMAP);
    mapW16 = *(const int*)(ta + OFF_MAPW16);
    mapH16 = *(const int*)(ta + OFF_MAPH16);
    if (!ptr_ok(tmap)) return terr_bail();
    if (mapW16 <= 0 || mapH16 <= 0 || mapW16 > 4096 || mapH16 > 4096) return terr_bail();
    stride = mapW16 / 2;                       /* TILE_MAP width in 32-px cells */
    mrows  = mapH16 / 2;
    if (stride <= 0 || mrows <= 0) return terr_bail();

    /* Run the engine's own algorithm over the ZOOM's viewport, not the
       engine's. Screen and world differ by a pure translation, so widening the
       rect is the same as moving the eye to its top-left corner and asking for
       more columns — the quads still land at unzoomed game coordinates, which is
       what the vertex shader's scale-about-the-centre expects. At zoom >= 1
       these are the engine's own numbers and the arithmetic is untouched. */
    vpL = v->evpL; vpT = v->evpT; evw = v->evw; evh = v->evh;
    eyeX = v->eyeX + (vpL - v->vpL);
    eyeY = v->eyeY + (vpT - v->vpT);
    tx0 = div32_trunc(eyeX); fx = eyeX - tx0 * 32;
    ty0 = div32_trunc(eyeY); fy = eyeY - ty0 * 32;
    cols = ceil32(evw + fx);
    rows = ceil32(evh + fy);
    if (cols <= 0 || rows <= 0) return terr_bail();
    /* the staging holds what tagpu_terr_clamp_span reserved for this viewport,
       and it trimmed the rect to fit — so this is the guard, not the policy */
    if (!s_inst || (long)cols * rows > s_instCells) return terr_bail();
    s_rectTx0 = tx0; s_rectTy0 = ty0; s_rectCols = cols; s_rectRows = rows; s_rectValid = 1;

    iw = 1.0f / (float)ATLAS_W;
    ih = 1.0f / (float)s_atlasH;
    /* what the vertex shader rebuilds the quads from: the screen point grid
       cell (0,0) starts at, and the atlas texel size. `vpL - fx` is the same
       integer `vpL + c * 32 - fx` was built on, so the positions are the same
       floats. The map cell is s_rectTx0/s_rectTy0, set just above. */
    s_origX = (float)(vpL - fx); s_origY = (float)(vpT - fy);
    s_iw = iw; s_ih = ih;
    for (r = 0; r < rows && emit; r++) {
        int my = ty0 + r;
        if (my < 0 || my >= mrows) { skipped += cols; continue; }
        for (c = 0; c < cols; c++) {
            int mx = tx0 + c;
            int idx;
            if (mx < 0 || mx >= stride) { skipped++; continue; }
            idx = tmap[(size_t)my * stride + mx];
            if (idx >= s_atlasN) { junk++; continue; }
            /* the quad still spans exactly TILE_PX texels, and its far edge
               lands ON the guard column, which is a copy of the last real one.
               Screen and world differ by a pure translation here, so the cell's
               world rect is exactly (mx*32, my*32)..+32 — the space the
               engine's fog grid is built in — and the shader reaches it as
               uTile0 + (col,row), which is (tx0+c, ty0+r) = (mx, my). */
            put_cell(c, r, idx % ATLAS_COLS, idx / ATLAS_COLS);
        }
    }

    /* we drew this frame: the engine's terrain pass may be skipped. Handing it
       back here clears `filled`, so the composite stops inverting on the SAME
       frame the quads above were emitted for — which is why the hand-back never
       shows a frame of bare key fill. */
    tagpu_terrown_set_skip(own && s_ncell > 0);
    tagpu_terrown_beat(v->frame_counter);

    {
        static unsigned last = 0;
        if (v->frame_counter - last >= 60) {
            char b[240];
            last = v->frame_counter;
            _snprintf(b, sizeof b,
                "terr: grid=%dx%d tile0=(%d,%d) frac=(%d,%d) map=%dx%d cells=%d"
                " zoomvp=%dx%d off-map=%d junk=%d atlas=%dx%d/%d%s%s",
                cols, rows, tx0, ty0, fx, fy, stride, mrows, s_ncell, evw, evh,
                skipped, junk, ATLAS_W, s_atlasH, s_setCount,
                s_over ? " (over: engine still drawing)"
                       : (s_passive ? " (passive: engine still drawing)" : ""),
                tagpu_terrown_installed() ? ""
                    : " (NOTHING EMITTED: terrown.on must exist at DLL attach —"
                      " arm it before launch, not after)");
            flog(b);
        }
    }
    if (s_log && s_ncell) {
        static unsigned lastl = 0;
        if (v->frame_counter - lastl >= 120) {
            char b[200];
            int lx = tx0 < 0 ? 0 : (tx0 >= stride ? stride - 1 : tx0);
            int ly = ty0 < 0 ? 0 : (ty0 >= mrows ? mrows - 1 : ty0);
            lastl = v->frame_counter;
            _snprintf(b, sizeof b,
                "terr: cell(0,0) idx=%u at=(%d,%d) world=(%d,%d) vp=(%d,%d %dx%d) key=%d",
                (unsigned)tmap[(size_t)ly * stride + lx],
                v->vpL - fx, v->vpT - fy, tx0 * 32, ty0 * 32,
                v->vpL, v->vpT, v->vw, v->vh, s_key);
            flog(b);
        }
    }
    return s_ncell;
}

/* Everything the draw below was made of, for the Vulkan edition of this pass
   (tagpu_terr.h). Nothing is computed here that the draw did not already use:
   each field is the value that went into a uniform, a pointer into the array
   the upload took, or the buffer a texture was uploaded from. */
/* THE FOG GRID IS COPIED, NOT ALIASED. `v->fogGrid` points INSIDE a frame-packet
   slot, and tagpu_packet.h gives both packet pointers a lifetime that ends at
   tagpu_packet_frame_end() -- which render_ogl.c calls BEFORE it runs the
   Vulkan lane, so a pointer handed on from here is read past its contract. It
   held only because the give-back happens at the next acquire, which is also
   where the `poison` lever fills the slot: the one stale read that lever cannot
   see, and outside frame_end's tail==head check as well. [FOUND BY THE G19e
   LANDING REVIEW, 2026-09-15.] The atlas and the height grid were already
   handed over as buffers this module owns; this makes the fog grid the same,
   and makes the file header's "a pass reads no engine state" true of the whole
   hand-over rather than of most of it.

   `cells * 2` is the size the GL lane's own glTexImage2D was given for this
   grid, so the read is bounded by the bound the GL upload already trusts; the
   1024-a-side cap bounds the ALLOCATION, and the pass re-checks it (FOG_MAXDIM)
   because a bound in one file is a bound only while both are read together. */
#define FOG_COPY_MAXDIM 1024
static unsigned short* s_fogCopy;
static int             s_fogCopyCells;

/* ONE STEP OF THE RESTORED READ-BACK, on the render thread with the context
   current. A no-op unless the Vulkan lane asked for mirrors, and again once the
   restorer has stopped painting, so a settled map pays one integer compare. */
static void terr_publish(const TAGPU_FXVIEW* v, int restored, const TAGPU_LIGHT* L)
{
    int fogBad = 0;                    /* fog wanted, no grid: publish nothing */
    /* NOTHING IS PUBLISHED ON A SHIPPED FRAME. `s_mirrorWant` is the latch the
       arm beat sets when the Vulkan lane is up (see there); while it is 0 the
       lane is not armed, nothing will ever call the hand-over, and the memset
       and the forty stores below are pure cost on the path every player runs.
       `s_pubHave` is cleared with it so no earlier frame's hand-over can be
       taken later. [FROM THE G19e LANDING REVIEW, 2026-09-15.] */
    if (!s_mirrorWant) { s_pubHave = 0; s_abFrame = 0; return; }
    memset(&s_pub, 0, sizeof s_pub);
    s_pub.cells = s_inst; s_pub.ncell = s_ncell;
    s_pub.gw = (float)v->gw; s_pub.gh = (float)v->gh;
    s_pub.zoom = v->zoom > 0.0f ? v->zoom : 1.0f;
    s_pub.zoomCx = v->zoomCx; s_pub.zoomCy = v->zoomCy;
    s_pub.depthScale = v->depthScale > 1.0f ? v->depthScale : 512.0f;
    s_pub.enc = TERR_ENC;
    s_pub.origX = s_origX; s_pub.origY = s_origY;
    s_pub.tile0X = (float)s_rectTx0; s_pub.tile0Y = (float)s_rectTy0;
    s_pub.texelW = s_iw; s_pub.texelH = s_ih;
    s_pub.restored = restored;
    s_pub.lit = tagpu_classicpp_on() ? 1 : 0;
    s_pub.lambert = tagpu_classicpp_lit() ? 1 : 0;
    s_pub.fog = v->fogMode & 1;
    /* THE CAST-SHADOW BLOCK. Since G19e's shadow pass the map itself is drawn
       by tagpu_vk_shadow.c into an image of its own, so this is no longer a
       refusal: the Vulkan lane samples ITS map with THESE uniforms, which are
       the ones the GL draw below is about to be given.
       tagpu_shadow_apply leaves every other shadow uniform ALONE when the map
       is not live, which for a freshly linked program means zero -- so the rest
       of that block is published as zero, and the two lanes agree about it. */
    s_pub.shadowOn = tagpu_shadow_live() ? 1 : 0;
    if (s_pub.shadowOn) {
        memcpy(s_pub.shadowMat, tagpu_shadow_mat(), sizeof s_pub.shadowMat);
        s_pub.shadowSun[0] = L->shadowSun[0];
        s_pub.shadowSun[1] = L->shadowSun[1];
        s_pub.shadowSun[2] = L->shadowSun[2];
        tagpu_shadow_scale(s_pub.shScale);
        s_pub.penumbra = L->penumbra;
        s_pub.shade = L->shade;
    }
    s_pub.fogOrgX = (float)v->fogOrgX; s_pub.fogOrgY = (float)v->fogOrgY;
    s_pub.fogCols = (float)v->fogCols; s_pub.fogRows = (float)v->fogRows;
    s_pub.hDimW = (float)s_hW; s_pub.hDimH = (float)s_hH;
    s_pub.sun[0] = L->sun[0]; s_pub.sun[1] = L->sun[1]; s_pub.sun[2] = L->sun[2];
    s_pub.amb = L->amb;
    s_pub.norm = 1.0f / L->level;
    s_pub.atlas = s_atlasMirror;
    s_pub.atlasW = ATLAS_W; s_pub.atlasH = s_atlasH;
    s_pub.atlasSerial = s_atlasMirrorSerial;
    /* published only when the read-back has covered rows -- a non-NULL pointer
       with 0 rows would hand a consumer an image with nothing to upload */
    /* THE REQUEST, AND NOTHING ELSE. `atlasRgb`/`atlasRgbRows` were the other
       half of tagpu_terr.h's "mutually exclusive" pair -- the GL twin's
       restored atlas, read back so this lane could upload it -- and they went
       with the GL half in landing 11-5c, so they are now always NULL/0 and the
       pair is exclusive by construction rather than by a latch order.
       The serial goes out even with no list, because a DROP is news: it is how
       a consumer learns the atlas it was painting is not this map's. */
    if (s_rvkWant) {
        s_pub.restoreFrames  = s_rFrames;
        s_pub.restoreN       = s_rFrames ? s_rFrameN : 0;
        s_pub.restoreSerial  = s_rSerial;
        s_pub.restoreRepaint = s_rRepaint;
    }
    /* the height mirror only while it matches the dimensions the shader is
       being told about -- build_height keeps those two in step (see there) */
    if (s_hW > 0 && s_hH > 0) {
        s_pub.height = s_hMirror;
        s_pub.hW = s_hW; s_pub.hH = s_hH;
        s_pub.heightSerial = s_hMirrorSerial;
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
    /* FOG ON WITH NO GRID IS NOT A FRAME THIS PASS MAY DRAW, so it publishes
       NOTHING rather than a frame it cannot describe. The copy can fail -- a
       refused `realloc`, which is precisely the address-space pressure this
       phase exists to measure, or a grid past FOG_COPY_MAXDIM -- and publishing
       `fog` 1 with a NULL grid had the Vulkan lane sample a 1x1 image while
       `uFogDim` carried the real size: the GL twin draws correct fog and the
       port draws something else, silently.
       Clearing `fog` instead would be just as silent a difference the other
       way (a lit ring where the twin has none), so the answer is the one this
       file already gives for the restored atlas and the shadow map: stand down
       for the frame. The GL lane is untouched either way -- it draws from its
       own texture and never reads this struct.
       [G19e RE-REVIEW, 2026-09-15.] */
    fogBad = (s_pub.fog && !s_pub.fogGrid);
    s_pub.fogLut = tagpu_native_foglut();
    s_pub.vpL = v->vpL; s_pub.vpT = v->vpT; s_pub.vw = v->vw; s_pub.vh = v->vh;
    /* WHETHER THE CLIP IS ACTUALLY ON, not whether a rect exists -- the feature
       pass's reasoning (tagpu_feat.c), and terrain covers the whole viewport,
       so an unclipped Vulkan lane would differ in the entire margin the zoom's
       widened gather reaches past it. */
    s_pub.scissorOn = tagpu_native_scissor_on();
    s_pub.ss = v->ss;
    /* THE A/B FLAG LIVES EXACTLY ONE FRAME. The Vulkan lane takes the hand-over
       later in this same render-thread iteration, so a flag that was not taken
       was not taken because the lane is down -- and a claim left standing would
       pair a fresh Vulkan capture with a GL one from some earlier frame. */
    s_pub.ab = s_abFrame; s_abFrame = 0;
    /* THE STAMP IS WHAT MAKES THE POINTERS ABOVE SAFE (tagpu_terr.h). Every one
       of them aliases a buffer this file owns and rebuilds, so the hand-over is
       good for this frame and no other -- and `tagpu_terr_handover` is where
       that is enforced, because the flag alone cannot be: a frame whose gather
       bailed never reaches this function to clear it. */
    s_pub.frame = v->frame_counter;
    s_pubHave = s_ncell > 0 && !fogBad;
}

/* Hand it over, ONCE (tagpu_terr.h). */
int tagpu_terr_handover(TAGPU_TERRHAND* out, unsigned now)
{
    if (!s_pubHave || !out) return 0;
    /* NOT THIS FRAME'S, SO NOT ALIVE. Dropping it here rather than trusting the
       flag is the whole of the G19e re-review's first finding: `s_pub.atlas`
       and `s_pub.height` are buffers `ensure_atlas`/`build_height` free on a
       map change, and the flag survives any frame the gather bailed on. The
       stale hand-over is also CLEARED, so the next frame starts honest. */
    if (s_pub.frame != now) { s_pubHave = 0; s_abFrame = 0; return 0; }
    *out = s_pub;
    s_pubHave = 0; s_abFrame = 0;
    return 1;
}

void tagpu_terr_render(const TAGPU_FXVIEW* v, unsigned int palTex)
{
    /* THIS PASS NO LONGER DRAWS; IT GATHERS AND HANDS OVER. It used to do both,
       picking with `gl_draws = !tagpu_vk_owns_present()` [landing 4b-2]. The GL
       lane went in landing 11-2 and its draw half here went in 11-3, so the
       question has one answer and is not asked any more. The instances, the
       numbers and the texels below are the gather's; the Vulkan twin draws them.
       The scaffold (tagpu_scaffold.c) has the same shape. */
    int restored;
    /* A FRAME WITH NOTHING TO DRAW HANDS NOTHING OVER. Leaving the previous
       frame's hand-over standing would have the Vulkan lane draw last frame's
       terrain over this frame's -- and on the frame a level is torn down, over
       nothing at all.

       `s_state` WAS THE GL PROGRAM'S and was only asked where GL drew: it is
       permanently 0 on this lane, where `init_gl` is never called, so testing it
       would refuse every hand-over. With the draw gone the term is gone with it,
       and `s_ncell` -- the gather's own -- is the whole refusal, which is what it
       always really was. */
    if (s_ncell == 0) { s_pubHave = 0; s_abFrame = 0; return; }

    /* COMPUTED BEFORE THE GATE, because the hand-over carries it: the RGB
       mirror's state and `assets=` are both CPU-side, so this is the gather's
       answer rather than the draw's. */
    /* THE FIELD, NOT A STATE MACHINE'S WORD FOR IT: `restored` is what sets
       `uRestored` in the consumer's shader (tagpu_vk_terr.c), so it says
       exactly "there is a restore request standing for this atlas". It used to
       read the GL restorer's `s_rgbState`, which on a lane with no GL context
       never left 0 -- so the consumer painted the atlas it had been handed and
       was then told to sample the indexed one. [The vulkan-only plan, landing
       11-5c.] */
    restored = (s_rFrames && tagpu_classicpp_assets()) ? 1 : 0;
    {
        const TAGPU_LIGHT* L = tagpu_classicpp_light();
        /* HOISTED OUT OF THE DRAW, because on the vulkan-only lane the A/B is
           armed without one. Read once so the two arms cannot disagree about
           which frame is the capture frame. */
        int taking = s_ab && !s_abDone;
        /* NO `ss` BOUND ON THIS A/B ANY MORE. It used to refuse itself whenever
           `ss != 1`, because the GL capture is this FBO's viewport -- gw*ss by
           gh*ss -- and the Vulkan half was the window's client rect, so at the
           shipped ss=2 the two files differed by a factor of two and
           tools/vk-ab.py refused the pair. Landing 4c-2 gave the Vulkan lane a
           gw*ss by gh*ss world target and 4c-3 pointed the capture at it, so both
           halves are now the same size at every `ss` and this pass is measurable on
           the configuration it actually ships in. The refusal moved to the lane
           that can see the target: if there is none that frame, tagpu_vk.c says so
           by name and captures nothing. [tagpu_vk_world.h.] */
        /* THE GL DRAW STOOD HERE -- the program, its eighteen uniforms, the six
           texture units, the instance upload and one glDrawArraysInstanced.
           Deleted by the vulkan-only plan's landing 11-3. Everything above is
           the GATHER and still runs: the instances, the numbers and the texels
           are built the same way and `terr_publish` below hands them to the
           Vulkan twin, which is what draws them. */
        if (taking) {
            /* THE A/B CLAIM, which is all that is left of it. Until landing 4d-2 this
               pass also captured a GL half (`tagpu_abshot.c`) and claimed the Vulkan one
               only when that half had reached the disk -- route D gave the two lanes a
               window each, so one frame could be photographed from both sides and diffed.
               Route D went in 4d-1, the GL half had nothing left to pair with, and it went
               too. What the lever does now is claim the VULKAN capture: `tagpu_vk_ab_arm`
               unlinks the target `_vk.ppm` at the instant the claim latches, which is what
               makes the file on the disk this arming's rather than an earlier run's. Diff
               it against a capture taken from another BUILD. */
            s_abDone = 1;
            s_abFrame = tagpu_vk_ab_arm("terr");
        }

        /* PUBLISHED AFTER THE DRAW WHERE THERE IS ONE, and after the gather in
           either case: these are the instances, the numbers and the texels this
           frame built, and the Vulkan lane is about to draw the same ones. */
        terr_publish(v, restored, L);
    }
}
