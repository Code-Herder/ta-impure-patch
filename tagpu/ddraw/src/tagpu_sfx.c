/* tagpu_sfx.c — the particle sfx pass: smoke, fire, wakes, nanolathe spray.

   The engine keeps ten "layer" vectors at *(main+0x38D77) (0x10 apart:
   {u8 flag, void** begin @4, void** end @8, void** cap @0xC}, 400 objects
   max per layer). Every emitter (EmitSfx_GraySmoke 0x472810, _BlackSmoke
   0x4728F0, _Unk5 0x472AB0 = fire, _NanoParticles 0x4720D0, _Bubbles
   0x472530, ...) takes the LAYER as an argument, stores it in the object's
   +0xC byte and appends the object to that layer. DrawGameScreen calls
   0x471F90(ctx, n) for n = 0..9 at ten fixed points of the frame
   (terrain-depth.md §3): 0/1/2 before the flat-feature pre-pass, 3/4 before
   the row sweep, 5/6 after it (before projectiles), 7 after explosions,
   8 after aircraft, 9 before the fog overlay — so the layer number IS the
   draw depth. 0x471F90 walks the layer calling vtbl+8 (draw) on each
   object; the sim tick walks them calling vtbl+4 (update: move, animate,
   cull) — the draw is pure.

   Object: {vtbl @0, endTick @4, tick @8, u8 layer @0xC, sub-particle
   vector {begin @0x10, end @0x14, cap @0x18}, class data...} (76 bytes,
   pool 0x51E610). The draw walks the sub-vector with a per-class stride and
   blits each sub-particle at sx = hi(x) - eyeX + vpL,
   sy = hi(y) - hi(alt)/2 - eyeY + vpT (positions are 16.16):

     vtbl      class            stride  draw
     0x4FD638  Smoke1 (grey)    0x20    AlphaCompsteBuf2OFFScreen seq@0[frame@0x14]
     0x4FD618  Smoke2 (dark)    0x20    same, LOS-gated
     0x4FD5D8  fire             0x3C    alpha seq@0[frame@0x2C], LOS-gated
     0x4FD588  flare sprite     0x34    alpha seq@0[frame@0x2C], LOS-gated
     0x4FD5F8  wake / bubbles   0x44    DrawBar 2x2 dot, colour byte @0x30, LOS-gated
     0x4FD5B8  nanolathe spray  0x30    DrawBar 2x2 dot, colour byte @0x28, LOS-gated
                                        (positions @0/4/8 here, @4/8/C elsewhere)
     0x4FD5A8  base (destroyed) -       nothing

   LOS gate = the local player's LOS counter at tile (hi(x)>>5, (hi(y) -
   hi(alt)/2)>>5) with true LOS, else the MAPPED bit. Armed by tagpu_sfx.on
   (tokens log, passive, nosmoke, nofire, nowake, nonano). Read-only over sim. */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "tagpu_sfx.h"
#include "tagpu_fxown.h"

#define OFF_LAYERS    0x38D77
#define NLAYER        10
#define LAYER_CAP     400

#define VT_SMOKE1  0x004FD638u
#define VT_SMOKE2  0x004FD618u
#define VT_FIRE    0x004FD5D8u
#define VT_FLARE   0x004FD588u
#define VT_WAKE    0x004FD5F8u
#define VT_NANO    0x004FD5B8u
#define VT_BASE    0x004FD5A8u

#define O_END      0x04
#define O_TICK     0x08
#define O_LAYER    0x0C
#define O_SUB0     0x10
#define O_SUB1     0x14

#define SQ_NAME    0x08

enum { K_SMOKE1 = 0, K_SMOKE2, K_FIRE, K_FLARE, K_WAKE, K_NANO, K_BASE, K_OTHER, NKIND };
static const char* KNAME[NKIND] = { "smoke1", "smoke2", "fire", "flare", "wake", "nano", "base", "other" };

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
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
    HANDLE h = CreateFileA("tagpu_sfx.on", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        tagpu_fxown_set_skip_sfx(0);
        if (was > 0) flog("sfx: disarmed");
        return 0;
    }
    char buf[128]; DWORD n = 0;
    s_log = 0; s_passive = 0; s_smoke = s_fire = s_wake = s_nano = 1;
    if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
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
    CloseHandle(h);
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

/* ---- per-frame counters ---- */
static int s_nObj[NLAYER], s_nKind[NKIND], s_nSub, s_nSprites, s_nDots, s_nFogged, s_nBad;
static int s_layerKind[NLAYER][NKIND];
static int s_logged;

static int kind_of(unsigned vt)
{
    switch (vt) {
    case VT_SMOKE1: return K_SMOKE1;
    case VT_SMOKE2: return K_SMOKE2;
    case VT_FIRE:   return K_FIRE;
    case VT_FLARE:  return K_FLARE;
    case VT_WAKE:   return K_WAKE;
    case VT_NANO:   return K_NANO;
    case VT_BASE:   return K_BASE;
    default:        return K_OTHER;
    }
}

static const char* seq_name(const char* seq)
{
    if (!ptr_ok(seq) || IsBadReadPtr(seq, 0x2C)) return "?";
    return seq + SQ_NAME;
}

void tagpu_sfx_gather(const TAGPU_FXVIEW* v, int from, int to)
{
    if (s_armed != 1) return;
    const char* ta = v->ta;
    const char* layers = *(const char* const*)(ta + OFF_LAYERS);
    if (!ptr_ok(layers) || IsBadReadPtr(layers, NLAYER * 0x10)) return;
    int alphaOn = (tagpu_fx_caps() & 0x20) != 0;   /* the alpha blit is gated */
    int eyeX = v->eyeX, eyeY = v->eyeY, vpL = v->vpL, vpT = v->vpT;
    int L;
    char lb[220];
    int doLog = s_log && (v->frame_counter % 60) == 0;
    tagpu_fx_set_mute(s_passive);          /* passive: count + log, emit nothing */
    if (doLog && from == 0) tagpu_fx_trace(3);   /* the first three of OUR emissions */

    for (L = from; L <= to && L < NLAYER; L++) {
        const char* lay = layers + L * 0x10;
        const char* const* b = *(const char* const* const*)(lay + 4);
        const char* const* e = *(const char* const* const*)(lay + 8);
        if (!b || !e || e <= b) continue;
        size_t n = (size_t)(e - b);
        if (n > LAYER_CAP + 1 || !ptr_ok(b) || IsBadReadPtr(b, n * 4)) { s_nBad++; continue; }
        float enc = v->encLayer[L];
        int under = (L <= 6);              /* the engine draws it before its projectile pass */
        size_t i;
        for (i = 0; i < n; i++) {
            const char* o = b[i];
            if (!ptr_ok(o) || IsBadReadPtr(o, 0x4C)) { s_nBad++; continue; }
            unsigned vt = *(const unsigned*)o;
            int k = kind_of(vt);
            s_nObj[L]++; s_nKind[k]++; s_layerKind[L][k]++;
            if (k == K_BASE || k == K_OTHER) continue;
            int stride, posOff, mode;      /* mode: 0 sprite, 1 dot */
            int frameOff = 0, colOff = 0, losGate = 1, on = 1;
            switch (k) {
            case K_SMOKE1: stride = 0x20; posOff = 4; mode = 0; frameOff = 0x14; losGate = 0; on = s_smoke; break;
            case K_SMOKE2: stride = 0x20; posOff = 4; mode = 0; frameOff = 0x14; on = s_smoke; break;
            case K_FIRE:   stride = 0x3C; posOff = 4; mode = 0; frameOff = 0x2C; on = s_fire; break;
            case K_FLARE:  stride = 0x34; posOff = 4; mode = 0; frameOff = 0x2C; on = s_fire; break;
            case K_WAKE:   stride = 0x44; posOff = 4; mode = 1; colOff = 0x30; on = s_wake; break;
            default:       stride = 0x30; posOff = 0; mode = 1; colOff = 0x28; on = s_nano; break;
            }
            const char* sb = *(const char* const*)(o + O_SUB0);
            const char* se = *(const char* const*)(o + O_SUB1);
            if (!sb || !se || se <= sb) continue;
            size_t ns = (size_t)(se - sb) / (size_t)stride;
            if (ns > 4096 || !ptr_ok(sb) || IsBadReadPtr(sb, ns * (size_t)stride)) { s_nBad++; continue; }
            s_nSub += (int)ns;
            if (doLog && s_logged < 12) {
                s_logged++;
                const char* p0 = sb;
                int X = *(const int*)(p0 + posOff), A = *(const int*)(p0 + posOff + 4), Y = *(const int*)(p0 + posOff + 8);
                _snprintf(lb, sizeof lb,
                    "sfx: L%d obj=%p vt=%08x %s type=%u end=%d tick=%d n=%u p0 pos=(%d,%d,%d) %s=%d seq=\"%.16s\"",
                    L, (const void*)o, vt, KNAME[k], (unsigned)*(const unsigned char*)(o + O_LAYER),
                    *(const int*)(o + O_END), *(const int*)(o + O_TICK), (unsigned)ns,
                    X >> 16, A >> 16, Y >> 16,
                    mode ? "col" : "frame",
                    mode ? (int)*(const unsigned char*)(p0 + colOff) : *(const int*)(p0 + frameOff),
                    mode ? "-" : seq_name(*(const char* const*)p0));
                flog(lb);
            }
            if (!on) continue;
            if (mode == 0 && !alphaOn) continue;
            size_t j;
            for (j = 0; j < ns; j++) {
                const char* p = sb + j * (size_t)stride;
                int hx = *(const int*)(p + posOff) >> 16;
                int halt = *(const int*)(p + posOff + 4) >> 16;
                int hy = *(const int*)(p + posOff + 8) >> 16;
                int hzp = hy - (halt >> 1);
                if (losGate && !tagpu_fx_tile_visible(v, hx, hzp)) { s_nFogged++; continue; }
                int sx = hx - eyeX + vpL, sy = hzp - eyeY + vpT;
                if (mode == 0) {
                    const char* seq = *(const char* const*)p;
                    int frame = *(const int*)(p + frameOff);
                    if (tagpu_fx_emit_seq_frame(seq, frame, sx, sy, TAGPU_FXMODE_ALPHA,
                                                (float)hx, (float)hzp, enc, under))
                        s_nSprites++;
                } else {
                    int col = *(const unsigned char*)(p + colOff);
                    if (tagpu_fx_emit_dot(sx, sy, col, (float)hx, (float)hzp, enc, under))
                        s_nDots++;
                }
            }
        }
    }
    tagpu_fx_trace(0);
    tagpu_fx_set_mute(0);
}

void tagpu_sfx_frame_done(const TAGPU_FXVIEW* v)
{
    if (s_armed != 1) return;
    /* we gathered this frame: the engine's layer draw may be skipped */
    if (!s_passive) tagpu_fxown_set_skip_sfx(1);
    tagpu_fxown_beat_sfx(v->frame_counter);
    static unsigned last = 0;
    if (v->frame_counter - last >= 60) {
        last = v->frame_counter;
        char b[512]; int L, k, p = 0;
        sappend(b, sizeof b, &p, "sfx: layers");
        for (L = 0; L < NLAYER; L++) {
            if (!s_nObj[L]) continue;
            sappend(b, sizeof b, &p, " L%d=%d(", L, s_nObj[L]);
            for (k = 0; k < NKIND; k++)
                if (s_layerKind[L][k]) sappend(b, sizeof b, &p, "%s:%d ", KNAME[k], s_layerKind[L][k]);
            if (p > 0 && b[p-1] == ' ') b[--p] = 0;
            sappend(b, sizeof b, &p, ")");
        }
        sappend(b, sizeof b, &p, " sub=%d fogged=%d bad=%d -> sprites=%d dots=%d%s",
                s_nSub, s_nFogged, s_nBad, s_nSprites, s_nDots,
                s_passive ? " (passive: nothing emitted)" : "");
        flog(b);
    }
    memset(s_nObj, 0, sizeof s_nObj); memset(s_nKind, 0, sizeof s_nKind);
    memset(s_layerKind, 0, sizeof s_layerKind);
    s_nSub = s_nSprites = s_nDots = s_nFogged = s_nBad = 0; s_logged = 0;
}
