/* tagpu_sfx.c — the particle sfx pass: smoke, fire, wakes, nanolathe spray.

   WHAT THE ENGINE DOES. It keeps ten "layer" vectors at *(main+0x38D77) (0x10
   apart: {u8 flag, void** begin @4, end @8, cap @0xC}, 400 objects max per
   layer, every emitter refusing past that). Each emitter (EmitSfx_GraySmoke
   0x472810, _BlackSmoke 0x4728F0, _Unk5 0x472AB0 = fire, _NanoParticles
   0x4720D0, _Bubbles 0x472530, ...) takes the LAYER as an argument and
   appends the object to it. DrawGameScreen calls 0x471F90(ctx, n) for
   n = 0..9 at ten fixed points of the frame (terrain-depth.md §3): 0/1/2
   before the flat-feature pre-pass, 3/4 before the row sweep, 5/6 after it
   (before projectiles), 7 after explosions, 8 after aircraft, 9 before the
   fog overlay — so THE LAYER NUMBER IS THE DRAW DEPTH. 0x471F90 walks the
   layer calling vtbl+8 (draw) on each object; the sim tick walks them calling
   vtbl+4 (update: move, animate, cull) — the draw is pure.

   WHAT THIS FILE DOES. It reads the packet's particle table and nothing else.
   The walk above — ten engine vectors, and inside each object a second one at
   its class's stride — is the publisher's (tagpu_packet_pub.c, game thread,
   once per sim tick), and so is every sprite's GAF frame lookup. Those
   sub-vectors are std::vectors the game thread GROWS mid-play, freeing the old
   array (0x4732E0), so from another thread the {begin,end} pair could be read
   skewed and a consistent pair could name memory just freed — a client
   tagpu_reclaim's per-LEVEL fence does not cover (cross-thread-engine-reads.md
   §5). A probe would only make that rarer.

   What arrives per drawable sub-particle is 16 bytes: the projected world
   point (hi(x), hi(y) - hi(alt)/2), the resolved GAF frame or a colour byte,
   the layer and the class. The classes, with the engine's own draw rule:

     vtbl      class            stride  draw
     0x4FD638  Smoke1 (grey)    0x20    alpha seq@0[frame@0x14], NOT LOS-gated
     0x4FD618  Smoke2 (dark)    0x20    same, LOS-gated
     0x4FD5D8  fire             0x3C    alpha seq@0[frame@0x2C], LOS-gated
     0x4FD588  flare sprite     0x34    alpha seq@0[frame@0x2C], LOS-gated
     0x4FD5F8  wake / bubbles   0x44    DrawBar 2x2 dot, colour byte @0x30
     0x4FD5B8  nanolathe spray  0x30    DrawBar 2x2 dot, colour byte @0x28
                                        (positions @0/4/8 here, @4/8/C elsewhere)
     0x4FD5A8  base (destroyed) -       nothing

   LOS gate = the local player's LOS counter at the tile, true LOS or the
   MAPPED bit — applied HERE, against the fog grid the render thread holds,
   because that grid is the render thread's own.
   Armed by tagpu_sfx.on (tokens log, passive, nosmoke, nofire, nowake,
   nonano). Read-only over sim. */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "tagpu_sfx.h"
#include "tagpu_packet.h"
#include "tagpu_opt.h"
#include "tagpu_fxown.h"
#include "tagpu_log.h"

#define NLAYER        10

enum { K_SMOKE1 = 0, K_SMOKE2, K_FIRE, K_FLARE, K_WAKE, K_NANO, NKIND };
static const char* KNAME[NKIND] = { "smoke1", "smoke2", "fire", "flare", "wake", "nano" };

static void flog(const char* s)
{
    tagpu_log(s);
}

/* bounded append: MSVCRT's _vsnprintf returns -1 and leaves no NUL when the
   text does not fit, so the cursor is clamped and the buffer re-terminated */
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
static int  s_armed = -1;
static int  s_log = 0, s_passive = 0;
static int  s_smoke = 1, s_fire = 1, s_wake = 1, s_nano = 1;
static unsigned s_armCheck = 0;

int tagpu_sfx_armed(unsigned frame_counter)
{
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return s_armed > 0;
    s_armCheck = frame_counter;
    int was = s_armed;
    s_armed = 0;
    char buf[128];
    int n = tagpu_opt_read("tagpu_sfx.on", buf, sizeof buf);
    if (n < 0) {
        tagpu_fxown_set_skip_sfx(0);
        if (was > 0) flog("sfx: disarmed");
        return 0;
    }
    s_log = 0; s_passive = 0; s_smoke = s_fire = s_wake = s_nano = 1;
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
            else if (!lstrcmpiA(p, "passive")) s_passive = 1;
            else if (!lstrcmpiA(p, "nosmoke")) s_smoke = 0;
            else if (!lstrcmpiA(p, "nofire")) s_fire = 0;
            else if (!lstrcmpiA(p, "nowake")) s_wake = 0;
            else if (!lstrcmpiA(p, "nonano")) s_nano = 0;
            if (last) break;
            p = q + 1;
        }
    }
    s_armed = 1;
    if (s_passive) tagpu_fxown_set_skip_sfx(0);
    if (was != 1) {
        char b[128];
        _snprintf(b, sizeof b, "sfx: ARMED (smoke=%d fire=%d wake=%d nano=%d log=%d passive=%d)",
                  s_smoke, s_fire, s_wake, s_nano, s_log, s_passive);
        flog(b);
    }
    return 1;
}

int tagpu_sfx_on(void) { return s_armed > 0; }

/* the local class numbers ARE the packet's: the log's names index this enum
   and the table's `kind` byte indexes the same rows */
typedef char sfx_kind_agrees[(K_SMOKE1 == (int)TAGPU_PK_PK_SMOKE1 &&
                              K_SMOKE2 == (int)TAGPU_PK_PK_SMOKE2 &&
                              K_FIRE   == (int)TAGPU_PK_PK_FIRE &&
                              K_FLARE  == (int)TAGPU_PK_PK_FLARE &&
                              K_WAKE   == (int)TAGPU_PK_PK_WAKE &&
                              K_NANO   == (int)TAGPU_PK_PK_NANO &&
                              NKIND    == (int)TAGPU_PK_NPARTKIND &&
                              NLAYER   == (int)TAGPU_PK_NLAYER) ? 1 : -1];

/* ---- per-frame counters ---- */
static int s_nSub, s_nSprites, s_nDots, s_nFogged;
static int s_nObj[NLAYER], s_nKind[NKIND];

/* THE LAYERS COME OUT OF THE PACKET. The publisher walks the engine's vectors
   on the thread that writes them, once per sim tick, and resolves every
   sprite's GAF frame there (the header says why); what arrives is a flat table
   in LAYER ORDER with the projection already done.

   `from`/`to` select a layer range, because the layer IS the draw depth
   and the engine interleaves them with its own effects passes: 0..6 before
   the projectiles, 7..9 after the explosions. */
void tagpu_sfx_gather(const TAGPU_FXVIEW* v, int from, int to)
{
    const TAGPU_PACKET* pk = v->packet;
    const TAGPU_PK_PART* tab = tagpu_pk_part(pk);
    unsigned base = 0;
    int L;
    if (s_armed != 1 || !tab) return;
    tagpu_fx_set_mute(s_passive);          /* passive: count + log, emit nothing */
    for (L = 0; L < NLAYER; L++) {
        unsigned n = pk->part_n[L], i;
        float enc = v->encLayer[L];
        int under = (L <= 6);              /* the engine draws it before its projectile pass */
        if (L < from || L > to) { base += n; continue; }
        s_nObj[L] += (int)pk->part_obj[L];
        for (i = 0; i < n; i++) {
            const TAGPU_PK_PART* q = &tab[base + i];
            int on, losGate;
            s_nSub++;
            s_nKind[q->kind]++;
            switch (q->kind) {
            case TAGPU_PK_PK_SMOKE1: on = s_smoke; losGate = 0; break;
            case TAGPU_PK_PK_SMOKE2: on = s_smoke; losGate = 1; break;
            case TAGPU_PK_PK_FIRE:
            case TAGPU_PK_PK_FLARE:  on = s_fire;  losGate = 1; break;
            case TAGPU_PK_PK_WAKE:   on = s_wake;  losGate = 1; break;
            default:                 on = s_nano;  losGate = 1; break;
            }
            if (losGate && !tagpu_fx_tile_visible(v, q->x, q->zp)) { s_nFogged++; continue; }
            if (!on) continue;
            {
                int sx = q->x - v->eyeX + v->vpL, sy = q->zp - v->eyeY + v->vpT;
                if (q->frame) {
                    if (!(tagpu_fx_caps() & 0x20)) continue;     /* the alpha blit is gated */
                    if (tagpu_fx_emit_frame((const unsigned char*)(size_t)q->frame, sx, sy,
                                            TAGPU_FXMODE_ALPHA, (float)q->x, (float)q->zp, enc, under))
                        s_nSprites++;
                } else {
                    if (tagpu_fx_emit_dot(sx, sy, q->col, (float)q->x, (float)q->zp, enc, under))
                        s_nDots++;
                }
            }
        }
        base += n;
    }
    tagpu_fx_set_mute(0);
}

void tagpu_sfx_frame_done(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    if (s_armed != 1) return;
    /* we gathered this frame: the engine's layer draw may be skipped — but only
       once the packet says the publisher was filling the particle table for us,
       or the first armed frames would suppress the engine's ten layer draws
       against an empty table */
    if (!s_passive && (pk->fx_want & TAGPU_PK_FXWANT_SFX)) tagpu_fxown_set_skip_sfx(1);
    tagpu_fxown_beat_sfx(v->frame_counter);
    static unsigned last = 0;
    if (s_log && v->frame_counter - last >= 60) {
        last = v->frame_counter;
        char b[512]; int L, k, p = 0;
        sappend(b, sizeof b, &p, "sfx: layers");
        for (L = 0; L < NLAYER; L++) {
            if (!pk->part_obj[L] && !pk->part_n[L]) continue;
            sappend(b, sizeof b, &p, " L%d=%u(%u)", L, pk->part_obj[L], pk->part_n[L]);
        }
        sappend(b, sizeof b, &p, " kinds");
        for (k = 0; k < NKIND; k++)
            if (s_nKind[k]) sappend(b, sizeof b, &p, " %s:%d", KNAME[k], s_nKind[k]);
        sappend(b, sizeof b, &p, " sub=%d fogged=%d -> sprites=%d dots=%d%s%s",
                s_nSub, s_nFogged, s_nSprites, s_nDots,
                (pk->truncated & TAGPU_PK_TRUNC_PART) ? " TRUNCATED" : "",
                s_passive ? " (passive: nothing emitted)" : "");
        flog(b);
    }
    memset(s_nObj, 0, sizeof s_nObj); memset(s_nKind, 0, sizeof s_nKind);
    s_nSub = s_nSprites = s_nDots = s_nFogged = 0;
}
