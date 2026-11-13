/* tasim_dmg.c -- the engine's damage arithmetic (tasim_dmg.h). Each function's engine reading is at its
   declaration. */
#include <math.h>
#include "tasim_dmg.h"
#include "tasim_fixed.h"

int ts_stricmp(const char* a, const char* b)
{
    for (;; ++a, ++b) {
        unsigned ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca == cb) {
            if (!ca) return 0;
            continue;
        }
        if (ca - 'A' < 26u) ca += 0x20;
        if (cb - 'A' < 26u) cb += 0x20;
        if (ca != cb) return ca < cb ? -1 : 1;
    }
}

int32_t ts_dmg_base(uint16_t dflt, const TS_DMGROW* rows, int32_t n, const char* type)
{
    int32_t lo = 0, hi = n;
    if (!rows || n <= 0) return (int32_t)dflt;
    while (lo != hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        if (ts_stricmp(rows[mid].name, type) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo != n && !(ts_stricmp(type, rows[lo].name) < 0)) return rows[lo].damage;
    return (int32_t)dflt;
}

int32_t ts_dmg_scale(int32_t base, float factor)
{
    return ts_ftol((double)base * (double)factor);
}

unsigned ts_vet_level(uint16_t kills)
{
    const unsigned l = kills / 5u;
    return l > 5u ? 5u : l;
}

int32_t ts_dmg_shooter(int32_t amount, unsigned level)
{
    return (int32_t)((6u * level + 100u) * (uint32_t)amount) / 100;
}

int32_t ts_dmg_cheats(int32_t amount, uint16_t cheats)
{
    if (cheats & 0x80u) amount = (int32_t)((uint32_t)amount + (uint32_t)amount);
    if (cheats & 0x100u) amount /= 2;
    return amount;
}

int ts_dmg_kind(uint32_t wflags)
{
    return (wflags & TS_WF_PARALYZER) ? 2 : 1;
}

uint16_t ts_dmg_angle(int32_t px, int32_t pz, int32_t ux, int32_t uz, uint16_t heading)
{
    const int32_t a = ts_angle((int32_t)((uint32_t)px - (uint32_t)ux), (int32_t)((uint32_t)pz - (uint32_t)uz));
    return (uint16_t)((uint32_t)a - heading);
}

uint16_t ts_hit_word(int32_t amount, int kind, int armoured, int32_t armour, unsigned level)
{
    int32_t a = amount;
    if (kind != 10) {
        if (armoured && amount < TS_DMG_OUTRIGHT) a = ts_mul_shr(armour, amount, 16);
        if (amount < TS_DMG_OUTRIGHT) a = (int32_t)((uint32_t)(25 - (int32_t)level) * (uint32_t)a * 4u) / 100;
    }
    if (kind == 2 || kind == 10) return (uint16_t)(a < 0 ? 0 : a > 0xFFFF ? 0xFFFF : a);
    return (uint16_t)(int16_t)(a < -0x8000 ? -0x8000 : a > 0x7FFF ? 0x7FFF : a);
}

/* one axis of the box gap (0x49A2B8..0x49A2E0): below the box, above it, or inside */
static int32_t gap(int32_t at, int32_t u, int32_t lo, int32_t hi)
{
    const int32_t a = (int32_t)((uint32_t)lo + (uint32_t)u), b = (int32_t)((uint32_t)hi + (uint32_t)u);
    if (at < a) return (int32_t)((uint32_t)a - (uint32_t)at);
    if (at > b) return (int32_t)((uint32_t)at - (uint32_t)b);
    return 0;
}

int32_t ts_area_distance(const int32_t at[3], const int32_t u[3], const int32_t bmin[3], const int32_t bmax[3])
{
    const double gx = (double)gap(at[0], u[0], bmin[0], bmax[0]);
    const double gy = (double)gap(at[1], u[1], bmin[1], bmax[1]);
    const double gz = (double)gap(at[2], u[2], bmin[2], bmax[2]);
    return ts_ftol(sqrt((gx * gx + gy * gy) + gz * gz));
}

float ts_area_falloff(int32_t dpx, int32_t R, float edge)
{
    double q, f;
    if (dpx == 0) return 1.0f;
    q = (double)dpx / (double)R - 1.0;
    f = q * q * (1.0 - (double)edge) + (double)edge;
    return (float)f;
}
