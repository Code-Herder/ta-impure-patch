/* tagpu_scaffold.c — the scene-depth scaffold (a native-res pass).

   The engine has no screen depth plane (terrain-depth.md §4): scene order is a
   painter's sweep keyed on the 16-px map-tile row (§3.3). This module rebuilds
   that key per frame, from live engine data only, as a viewport-sized byte
   buffer ("the scaffold"):

       0                 = free (terrain / flat features / nothing) = FAR
       3 + relRow*4      = a TALL feature's silhouette pixel (def Height >= 10),
                           stamped at its anchor-row key; larger = nearer

   A ground unit's compare value is 1 + relRow*4 (units draw before features
   within a row, so a feature of the SAME row occludes the unit; a unit one row
   lower occludes that feature). relRow is relative to the sweep's first row
   (eyeY>>4 - 16), exactly the engine's own bucket origin.

   Debug outputs:
     - colour overlay: scaffold pixels tinted far(blue)->near(red), 55% alpha,
       drawn over the live frame (capture it with a window grab);
     - per-unit occlusion PREDICTION in tagpu.log ("scaffold: uNNN ... occl=P%"):
       fraction of the unit's composite rect covered by scaffold pixels nearer
       than the unit's row key. The engine frame must agree.

   RESOLUTION RULE (Phase D constraint): nothing here is 640x480 — the viewport
   rect (main+0x37E27..), view dims (main+0x37E37/3B) and sweep dims
   (main+0x1424B/4F) are read live every frame. Armed by tagpu_scaffold.on. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "tagpu_scaffold.h"
#include "tagpu_zoom.h"      /* the predicted eye every pass draws from */
#include "tagpu_packet.h"    /* the true viewport, from this frame's packet */
#include "tagpu_vk.h"        /* tagpu_vk_ab_arm: the A/B claim */

/* ---- engine layout (terrain-depth.md, binary-verified) ----
   This file reads no per-frame engine field of its own. The view, the map
   dimensions, the engine's sweep rect, the feature anchors in it and every
   unit it stamps for come out of the packet; what is left below is the
   FeatureDef record's own layout and the one LIVE read of its base. Those
   records are the per-MAP asset the teardown cascade frees, so their lifetime
   is tagpu_reclaim's fence — the same standing tagpu_terr.c and
   tagpu_r3dcache.c have, and not the per-frame sim state the packet exists to
   copy. The base is READ LIVE rather than taken from the packet because the
   cascade frees the array at 0x42227D and then NULLS main+0x1426F at
   0x42228B, and that null is this pass's only refusal afterwards; a base
   copied into a packet and held for a frame reads past it. */
#define TA_MAINPP    0x00511DE8u
#define OFF_FEATDEF  0x1426F   /* FeatureDef array, stride 0x100                */
#define OFF_FEATCOUNT 0x14253  /* i32 NumFeatureDefs: read LIVE beside the base  */
#define FEAT_STRIDE  0x0D
#define FT_HEIGHT    0x04      /* u8 tile height                                */
#define FT_DEFIDX    0x08      /* u16; <0xFFFB = live feature anchor            */
#define FD_STRIDE    0x100
#define FD_FOOTX     0x94      /* u16 footprint (16-px tiles)                   */
#define FD_FOOTZ     0x96
#define FD_BODYSEQ   0xAC      /* GAF sequence (static body frames)             */
#define FD_HEIGHT    0xFA      /* u8; >=10 = tall (defers to the row sweep)     */
#define GF_WIDTH     0x00      /* GAFFrame header                               */
#define GF_HEIGHT    0x02
#define GF_HOTX      0x04
#define GF_HOTY      0x06
#define GF_CKEY      0x08
#define GF_COMPRESSED 0x09
#define GF_PTRCOLOR  0x10
#define SEQ_NFRAMES  0x00      /* GAF anim entry: u16 frame count               */
#define SEQ_FRAMES   0x28      /* -> inline GAFFrame[] table, stride 0x18       */

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void slog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int    s_armed = -1;        /* re-checked every 30 frames          */
static unsigned char* s_buf = 0;   /* viewport-sized scaffold, malloc'd   */
static int    s_bw = 0, s_bh = 0;  /* current buffer dims                 */
static int    s_lastR0 = 0, s_lastRows = 0;
static unsigned s_lastFrame = 0;

/* ---- the A/B, and what this frame hands the Vulkan lane ----
   `tagpu_scaffold.ab` claims one Vulkan capture of this pass, and it is of the
   frame the claim latched on because `s_abFrame` travels with the scaffold
   below rather than being polled a second time. See
   inc/tagpu_scaffold.h. */
#define ABFILE  "tagpu_scaffold.ab"
static int s_ab, s_abDone, s_abFrame;

/* Published AFTER the gather, taken exactly once, and every field of it is
   what the gather built. `s_pubBuf` is `s_buf`, which this file
   owns and rebuilds only on the render thread. */
static const unsigned char* s_pubBuf;
static int   s_pubW, s_pubH;
static float s_pubRect[4], s_pubRows;

/* THE SHADER PAIR IS A BUILD INPUT, NOT CODE THIS FILE RUNS. Nothing here
   references them -- tools/spirv-gen.py
   reads them out of the PREPROCESSED translation unit and generates the SPIR-V
   tagpu_vk_scaffold.c draws with, so deleting them fails the build with "the manifest names
   <pass>::VS and the source does not have it". The pragma below is paired and its
   `pop` is PROVED with a planted probe rather than read: a `pop` inside a
   comment is text, not a directive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 p;\n"           /* unit quad 0..1 */
    "uniform vec4 uRect;\n"                     /* NDC x0,y0,x1,y1 */
    "out vec2 uv;\n"
    "void main(){ uv=p;\n"
    "  gl_Position=vec4(mix(uRect.x,uRect.z,p.x), mix(uRect.y,uRect.w,p.y),0.,1.); }\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 uv; out vec4 frag;\n"
    "uniform sampler2D uScaf; uniform float uRows;\n"
    "void main(){ float v = texture(uScaf, uv).r * 255.0;\n"
    "  if (v < 2.5) discard;\n"                 /* 0 = far/free */
    "  float rel = (v - 3.0) / 4.0;\n"
    "  float t = clamp(rel / max(uRows - 1.0, 1.0), 0.0, 1.0);\n"
    "  vec3 c = mix(vec3(0.10,0.35,1.00), vec3(1.00,0.15,0.10), t);\n"
    "  frag = vec4(c, 0.55); }\n";
#pragma GCC diagnostic pop

/* Stamp one opaque-mask pixel run helper */
static void stamp_px(int bx, int by, unsigned char depth)
{
    if (bx < 0 || by < 0 || bx >= s_bw || by >= s_bh) return;
    s_buf[(size_t)by * s_bw + bx] = depth;
}

/* Stamp a GAF frame's opaque silhouette at (x0,y0) = top-left in buffer coords.
   Handles raw (Compressed=0) and TA-RLE (Compressed=1) colour planes. Returns
   0 if the plane was unreadable (caller falls back to a footprint stamp). */
static int stamp_gaf(const unsigned char* g, int x0, int y0, unsigned char depth)
{
    int w  = *(const unsigned short*)(g + GF_WIDTH);
    int h  = *(const unsigned short*)(g + GF_HEIGHT);
    unsigned char ck   = *(const unsigned char*)(g + GF_CKEY);
    unsigned char comp = *(const unsigned char*)(g + GF_COMPRESSED);
    const unsigned char* px = *(const unsigned char* const*)(g + GF_PTRCOLOR);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    if (!ptr_ok(px)) return 0;

    if (comp == 0) {
        if (IsBadReadPtr(px, (SIZE_T)w * h)) return 0;
        for (int y = 0; y < h; y++) {
            const unsigned char* row = px + (size_t)y * w;
            for (int x = 0; x < w; x++)
                if (row[x] != ck) stamp_px(x0 + x, y0 + y, depth);
        }
        return 1;
    }
    /* TA GAF RLE: per row u16 byte count, then codes:
       b&1 -> skip (b>>1) transparent px; b&2 -> next byte repeated (b>>2)+1;
       else -> (b>>2)+1 literal bytes follow. */
    const unsigned char* p = px;
    for (int y = 0; y < h; y++) {
        if (IsBadReadPtr(p, 2)) return 0;
        int rowlen = *(const unsigned short*)p;  p += 2;
        if (rowlen < 0 || rowlen > 4096 || IsBadReadPtr(p, rowlen)) return 0;
        const unsigned char* q = p; int x = 0;
        while (q < p + rowlen && x < w) {
            unsigned char b = *q++;
            if (b & 1) x += b >> 1;
            else if (b & 2) {
                int n = (b >> 2) + 1;
                if (q >= p + rowlen) break;
                unsigned char v = *q++;
                if (v != ck) for (int i = 0; i < n && x + i < w; i++)
                                 stamp_px(x0 + x + i, y0 + y, depth);
                x += n;
            } else {
                int n = (b >> 2) + 1;
                for (int i = 0; i < n && q < p + rowlen; i++) {
                    unsigned char v = *q++;
                    if (v != ck && x < w) stamp_px(x0 + x, y0 + y, depth);
                    x++;
                }
            }
        }
        p += rowlen;
    }
    return 1;
}

/* A readable GAFFrame header with sane dims, or NULL. */
static const unsigned char* frame_sane(const unsigned char* g)
{
    if (!ptr_ok(g) || IsBadReadPtr(g, 0x18)) return 0;
    int w = *(const unsigned short*)(g + GF_WIDTH);
    int h = *(const unsigned short*)(g + GF_HEIGHT);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    return g;
}

/* Resolve a feature def's static body GAF frame 0 (engine:
   GAF_SequenceIndex2Frame(*(def+0xAC), 0)). Layout per Phase B RE: anim entry
   +0x00 u16 nframes, +0x28 -> frame table (inline GAFFrame[] stride 0x18; we
   also tolerate a pointer table). NULL if unreadable. */
static const unsigned char* feat_frame0(const char* def)
{
    const char* seq = *(const char* const*)(def + FD_BODYSEQ);
    if (!ptr_ok(seq) || IsBadReadPtr(seq, 0x2C)) return 0;
    int nf = *(const unsigned short*)(seq + SEQ_NFRAMES);
    if (nf <= 0 || nf > 512) return 0;
    const unsigned char* tab = *(const unsigned char* const*)(seq + SEQ_FRAMES);
    const unsigned char* g = frame_sane(tab);            /* inline frame 0 */
    if (!g && tab && !IsBadReadPtr(tab, 4))              /* or a pointer table */
        g = frame_sane(*(const unsigned char* const*)tab);
    return g;
}

/* NO WHOLE-MAP WALK. The packet carries the anchors of the gather rect, which
   is what this pass draws from, and a whole-map walk is not something a
   render-thread pass should do at all. Which defs a map uses, and where the
   tall ones are, is `tagpu_features.trigger`'s question (tagpu_cat.c, a
   tooling reader with its documented caveat). */

void tagpu_scaffold_frame(const TAGPU_FRAME* f)
{
    /* NOTHING IS HANDED OVER UNTIL THIS FRAME HAS DRAWN IT, and the A/B flag
       goes with it. Every return below leaves both clear, which is what stops
       the flag LATCHING: set once and cleared only on consumption, it would
       survive a frame the Vulkan lane never collected and ride a LATER frame's
       scaffold, so the capture would be of a different frame than the claim --
       the one thing the design exists to prevent. */
    /* THIS PASS GATHERS; tagpu_vk_scaffold.c DRAWS, out of the hand-over at
       the bottom of this function. The gather is unconditional: it is the pass, and there is no
       second way through here. */

    s_pubBuf = NULL; s_abFrame = 0;

    if (s_armed < 0 || (f->frame_counter % 30) == 0) {
        s_armed = GetFileAttributesA("tagpu_scaffold.on") != INVALID_FILE_ATTRIBUTES;
        /* the A/B re-arms when the lever is taken away and put back, so a
           second capture needs no relaunch */
        s_ab = GetFileAttributesA(ABFILE) != INVALID_FILE_ATTRIBUTES;
        if (!s_ab) s_abDone = 0;
    }
    if (!s_armed) return;
    /* live view geometry — the Phase D rule: no constants. EVERYTHING comes
       from the FRAME PACKET: the true 1x rect and the map and sweep dimensions
       the game thread published, the same predicted eye the native pass draws
       from (so the two never disagree by a frame), the feature anchors of the
       gather rect, and the units this pass predicts occlusion for. The only
       engine memory left is the FeatureDef record, whose base the packet also
       carries and whose lifetime is the level. No in-game packet, no
       scaffold. */
    const TAGPU_PACKET* pk = f->packet;
    int vpL, vpT, vw, vh, eyeX, eyeY;
    if (!pk || !pk->in_game) return;
    vpL = pk->vp[0]; vpT = pk->vp[1]; vw = pk->vp[2]; vh = pk->vp[3];
    if (!tagpu_zoom_predicted_eye(&eyeX, &eyeY)) return;
    int mapW = pk->map_w16, mapH = pk->map_h16;
    int nCols = pk->sweep_cols, nRows = pk->sweep_rows;
    const char* taNow = *(const char* const*)TA_MAINPP;
    const char* fdef = ptr_ok(taNow) ? *(const char* const*)(taNow + OFF_FEATDEF) : NULL;
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
    int liveDefs = ptr_ok(taNow) ? *(const int*)(taNow + OFF_FEATCOUNT) : 0;
    int nDefs = pk->feat_defcount;
    if (nDefs < 0 || nDefs > 4096) nDefs = 0;
    if (liveDefs < 0 || liveDefs > 4096) liveDefs = 0;
    if (!nDefs || (liveDefs && liveDefs < nDefs)) nDefs = liveDefs;
    if ((f->frame_counter % 300) == 0) {              /* gate trace */
        char b[192]; _snprintf(b, sizeof b,
            "scaffold GATES: vp=(%d,%d) view=%dx%d map16=%dx%d sweep=%dx%d fdef=%p anchors=%u eye=(%d,%d)",
            vpL, vpT, vw, vh, mapW, mapH, nCols, nRows,
            (const void*)fdef, pk->n_anchors, eyeX, eyeY);
        slog(b);
    }
    if (vw < 64 || vh < 64 || vw > 4096 || vh > 4096) return;
    if (mapW <= 0 || mapH <= 0 || mapW > 4096 || mapH > 4096) return;
    if (nCols <= 0 || nRows <= 0 || nCols > 512 || nRows > 512) return;
    if (!ptr_ok(fdef)) return;

    if (vw != s_bw || vh != s_bh) {
        free(s_buf);
        s_buf = (unsigned char*)malloc((size_t)vw * vh);
        s_bw = vw; s_bh = vh;
        { char b[96]; _snprintf(b, sizeof b,
            "scaffold: buffer %dx%d vp=(%d,%d) sweep=%dx%d", vw, vh, vpL, vpT, nCols, nRows);
          slog(b); }
    }
    if (!s_buf) return;
    memset(s_buf, 0, (size_t)vw * vh);

    /* ---- walk the packet's anchors over the engine's own sweep rect ----
       The packet's anchor table covers the WIDEST zoom rect, so this rect is a
       sub-rect of it, and each entry already carries the four corner heights
       the engine's projection averages. */
    int r0 = (eyeY >> 4) - 16;
    s_lastR0 = r0; s_lastRows = nRows; s_lastFrame = f->frame_counter;
    int c0 = (eyeX >> 4) - 10;
    int tall = 0, flat = 0, gafFail = 0, junk = 0;
    const TAGPU_PK_ANCHOR* anch = tagpu_pk_anchors(pk);
    for (unsigned ai = 0; ai < pk->n_anchors; ai++) {
        const TAGPU_PK_ANCHOR* a = &anch[ai];
        int row = a->row, col = a->col;
        int r = row - r0, c = col - c0;
        {
            if (r < 0 || r >= nRows || c < 0 || c >= nCols) continue;
            if (row < 0 || row >= mapH || col < 0 || col >= mapW) continue;
            unsigned idx = a->def;
            const char* def;
            /* THE DEF BOUND. A tile can name a def past the map's own count,
               whose 0x100-byte record holds garbage (the terrain-depth note's
               "Corrections"); the feature pass refuses those and so does this
               one. Counted as `junk`, as there — and applied BEFORE the address
               is formed, not after. */
            if (!nDefs || (int)idx >= nDefs) { junk++; continue; }
            def = fdef + (size_t)idx * FD_STRIDE;
            if (*(const unsigned char*)(def + FD_HEIGHT) < 10) { flat++; continue; }
            tall++;

            unsigned char depth = (unsigned char)(r * 4 + 3 > 251 ? 251 : r * 4 + 3);

            /* engine projection (terrain-depth §3.4): anchor + footprint centre,
               height-corrected by the 2x2 corner-tile average */
            int fx = *(const short*)(def + FD_FOOTX);
            int fz = *(const short*)(def + FD_FOOTZ);
            if (fx < 0 || fx > 16) fx = 1;   /* garbage guard: defs past the map's */
            if (fz < 0 || fz > 16) fz = 1;   /* own count hold wild footprints     */
            int h00 = a->h, h01 = a->hr, h10 = a->hd, h11 = a->hrd;
            int sx = col * 16 + fx * 8 - eyeX;            /* buffer coords (no vpL) */
            int sy = row * 16 + fz * 8 - eyeY - (h00 + h01 + h10 + h11) / 8;

            const unsigned char* g = feat_frame0(def);
            int stamped = 0;
            if (g && !IsBadReadPtr(g, 0x18)) {
                int hx = *(const short*)(g + GF_HOTX);
                int hy = *(const short*)(g + GF_HOTY);
                stamped = stamp_gaf(g, sx - hx, sy - hy, depth);
            }
            if (!stamped) {                                /* footprint fallback */
                gafFail++;
                int px0 = col * 16 - eyeX, py1 = row * 16 - eyeY;
                int hgt = *(const unsigned char*)(def + FD_HEIGHT);
                int ya = py1 - hgt / 2 - fz * 16, yb = py1 + fz * 16;
                int xa = px0, xb = px0 + fx * 16;
                /* clip BEFORE looping. One clamp per line: two to a line is
                   correct C that -Wall reads as a misleading indentation. */
                if (ya < 0) ya = 0;
                if (yb > s_bh) yb = s_bh;
                if (xa < 0) xa = 0;
                if (xb > s_bw) xb = s_bw;
                for (int y = ya; y < yb; y++)
                    for (int x = xa; x < xb; x++)
                        stamp_px(x, y, depth);
            }
        }
    }

    /* ---- per-unit occlusion prediction (logged) ----
       Out of the packet's units table: the state bits, the 16.16 anchor and
       the unit composite's rect and hotspot, all copied by the game thread.
       No engine array is walked here and nothing is probed with IsBadReadPtr —
       a check whose answer can go stale between the check and the read, and
       never the argument (cross-thread-engine-reads.md §5). */
    int logNow = (f->frame_counter % 300) == 0;
    {
        const TAGPU_PK_UNIT* uu = tagpu_pk_units(pk);
        unsigned ui;
        for (ui = 0; ui < pk->n_units; ui++) {
            const TAGPU_PK_UNIT* u = &uu[ui];
            int cw = u->comp_w, chh = u->comp_h, hx = u->comp_hx, hy = u->comp_hy;
            int wx, wz, wy, relU, Bu, bx0, by0, occ = 0, tot = 0;
            if ((u->state & 3) != 1) continue;            /* airborne: never occluded */
            if (cw <= 0 || chh <= 0 || cw > 1280 || chh > 1280) continue;
            wx = (int)(short)(u->pos[0] >> 16);
            wz = (int)(short)(u->pos[1] >> 16);
            wy = (int)(short)(u->pos[2] >> 16);
            relU = (wy >> 4) - r0;
            Bu = relU * 4 + 1;                            /* units before features in-row */
            bx0 = wx - eyeX - hx; by0 = wy - wz / 2 - eyeY - hy;
            for (int y = by0; y < by0 + chh; y += 2) {
                if (y < 0 || y >= s_bh) continue;
                for (int x = bx0; x < bx0 + cw; x += 2) {
                    if (x < 0 || x >= s_bw) continue;
                    tot++;
                    if (s_buf[(size_t)y * s_bw + x] > Bu) occ++;
                }
            }
            if (logNow && tot > 0 && occ > 0) {
                char b[160]; _snprintf(b, sizeof b,
                    "scaffold: u%03d %-12.12s row=%d occl=%d%% (%d/%d px)",
                    ui + 1, u->name[0] ? u->name : "?", wy >> 4, occ * 100 / tot, occ, tot);
                slog(b);
            }
        }
    }

    if (logNow) {
        char b[160]; _snprintf(b, sizeof b,
            "scaffold: swept %dx%d tall=%d flat=%d gafFallback=%d junk=%d eye=(%d,%d)",
            nCols, nRows, tall, flat, gafFail, junk, eyeX, eyeY);
        slog(b);
    }

    /* ---- the debug overlay quad's rect, for the Vulkan pass ----
       Everything above is the GATHER: the packet's anchors walked over the
       engine's own sweep rect into `s_buf`, and the per-unit occlusion
       prediction read back out of it. Below are the four NDC numbers
       tagpu_vk_scaffold.c draws the quad at. */
    int gw = f->game_width  > 0 ? f->game_width  : vpL + vw;
    int gh = f->game_height > 0 ? f->game_height : vpT + vh;
    float x0 = (float)vpL        / gw * 2.f - 1.f;
    float x1 = (float)(vpL + vw) / gw * 2.f - 1.f;
    float y0 = 1.f - (float)vpT        / gh * 2.f;   /* NDC top    */
    float y1 = 1.f - (float)(vpT + vh) / gh * 2.f;   /* NDC bottom */

    /* Read once per frame, so nothing below can disagree about whether this is
       the capture frame. */
    int taking = s_ab && !s_abDone;

    if (taking) {
        /* THE A/B CLAIM, of the VULKAN capture: `tagpu_vk_ab_arm` unlinks the
           target `_vk.ppm` at the instant the claim latches, which is what
           makes the file on the disk this arming's rather than an earlier
           run's. Diff it against a capture taken from another BUILD. */
        s_abDone = 1;
        s_abFrame = tagpu_vk_ab_arm("scaffold");
    }

    /* PUBLISHED AFTER THE GATHER: these are the bytes and the numbers the frame
       was built from, and the Vulkan lane is about to draw the same ones. */
    s_pubBuf = s_buf; s_pubW = vw; s_pubH = vh;
    s_pubRect[0] = x0; s_pubRect[1] = y0; s_pubRect[2] = x1; s_pubRect[3] = y1;
    s_pubRows = (float)nRows;
}

/* The scaffold this frame drew, for the Vulkan edition of the pass, HANDED
   OVER EXACTLY ONCE -- see inc/tagpu_scaffold.h. */
int tagpu_scaffold_overlay(const unsigned char** buf, int* w, int* h,
                           float rect[4], float* rows, int* ab)
{
    if (!s_pubBuf) return 0;
    *buf = s_pubBuf; *w = s_pubW; *h = s_pubH;
    rect[0] = s_pubRect[0]; rect[1] = s_pubRect[1];
    rect[2] = s_pubRect[2]; rect[3] = s_pubRect[3];
    *rows = s_pubRows;
    *ab = s_abFrame;
    s_pubBuf = NULL; s_abFrame = 0;
    return 1;
}

/* exports for the native pass: this frame's scaffold texture + row encoding */
int tagpu_scaffold_frameinfo(unsigned frame_counter, int* r0, int* nrows)
{
    /* NOT GATED ON ANY BACKEND'S READINESS, because these two numbers are the
       GATHER's and the gather runs whatever draws. A readiness test would hand
       every caller "no rows", silently, on a lane whose backend never brings a
       program up -- and the caller it feeds is the unit shader's occlusion
       input. The freshness test below is the real one and it is
       API-independent: `s_lastFrame` is stamped by the gather. */
    if (!s_armed) return 0;
    if (frame_counter - s_lastFrame > 2) return 0;   /* stale (not armed/in-game) */
    *r0 = s_lastR0; *nrows = s_lastRows;
    return 1;
}

