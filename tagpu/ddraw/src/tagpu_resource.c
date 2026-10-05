#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "tagpu_resource.h"
#include "tagpu_profiles.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_detour.h"
#include "tagpu_replay.h"
#include "tagpu_puppet.h"
#include "tagpu_log.h"
#include "tagpu_patches.h"
#include "tagpu_vpwide.h"
#include "winapi_hooks.h"

/* DISASSEMBLED: 0x419670 uses these order roots and the def's CANBUILD list.
   The input and placement contracts are recorded in resource-snapping.md
   and exe-reverse-engineering.md. All engine access belongs to the input/
   publish thread, between simulation iterations. No pointer is retained
   across engine calls that can tear a level down. */
#define D_STRIDE 0x249u
#define D_BASE 0x1439B
#define U_FLAGS 0x110
#define U_ORDERS 0x5C
#define D_FLAGS 0x241
#define M_POINT 0x2CAA
#define M_MODE 0x2CC3
#define M_TYPE 0x2CC4
#define M_REGION 0x2CC6
#define PATCH_R 64
#define PATCH_SIDE (PATCH_R * 2 + 1)
#define PATCH_CAP (PATCH_SIDE * PATCH_SIDE)

typedef int (__stdcall* GridTest)(const void*, unsigned, unsigned, const void*);
typedef unsigned (__stdcall* SiteHeight)(void);
typedef const unsigned char* (__stdcall* Resolve)(int*, unsigned, void*, void*, const void*);
typedef void (__stdcall* Order)(int, int, void*, void*, const void*, int, int);
typedef unsigned char* (__attribute__((thiscall))* Action)(void*, const char*);

typedef struct {
    char *ta, *defs, *units, *grid;
    unsigned ndefs, nslots, first, last;
    int w, h, player, threshold, shift;
} World;
typedef struct {
    unsigned type, target;
    int kind, x, z, height, queued, anchor_x, anchor_z;
    float strength;
} Choice;
typedef struct {
    int action, pos[3], reclaim, reclaim_pos[3];
    unsigned type, target;
} Cancel;

/* UI state, game-thread owned: the engine simulation never reads it. */
static int s_armed, s_raw[3], s_raw_valid;
static unsigned char s_patch_seen[PATCH_CAP];
static unsigned short s_patch_queue[PATCH_CAP];

static int world(World* w)
{
    char *state, *end, *player;
    uintptr_t first, last;
    uintptr_t distance;
    memset(w, 0, sizeof *w);
    w->ta = *(char**)TA_MAIN_PP;
    if (!w->ta) return 0;
    w->player = *(unsigned char*)(w->ta + OFF_WATCHED);
    if (w->player >= 10 || tagpu_puppet_seat(w->player)) return 0;
    w->w = *(int*)(w->ta + OFF_MAP_W16); w->h = *(int*)(w->ta + OFF_MAP_H16);
    /* LoadMap's row product is the allocation; bound it before any index or
       byte product. The input path cannot run while the loader owns it. */
    if (w->w < 2 || w->h < 2 || (w->w & 1) || (w->h & 1) || w->w > 0x7FFF || w->h > 0x7FFF ||
        (unsigned)w->w > 0x7FFFFFFFu / FT_STRIDE / (unsigned)w->h) return 0;
    w->ndefs = *(unsigned*)(w->ta + OFF_UDEFCOUNT);
    if (!w->ndefs || w->ndefs > TAGPU_LIM_TYPES) return 0;
    w->defs = *(char**)(w->ta + D_BASE);
    w->units = *(char**)(w->ta + OFF_UNIT_BEGIN);
    end = *(char**)(w->ta + OFF_UNIT_END);
    w->grid = *(char**)(w->ta + OFF_FEATMAP);
    state = *(char**)(w->ta + 0x391E9);
    if (!w->defs || !w->units || !w->grid || !state || (uintptr_t)end < (uintptr_t)w->units) return 0;
    distance = (uintptr_t)end - (uintptr_t)w->units;
    if (distance % UNIT_STRIDE || distance / UNIT_STRIDE >= TAGPU_PK_DESIGN_SLOTS) return 0;
    w->nslots = *(unsigned short*)(w->ta + 0x14351);
    if (w->nslots != (unsigned)(distance / UNIT_STRIDE) + 1) return 0;
    player = w->ta + 0x1B63 + w->player * 0x14B;
    first = (uintptr_t)*(char**)(player + 0x67); last = (uintptr_t)*(char**)(player + 0x6B);
    if (first < (uintptr_t)w->units || last < first || last > (uintptr_t)end ||
        (first - (uintptr_t)w->units) % UNIT_STRIDE ||
        (last - (uintptr_t)w->units) % UNIT_STRIDE) return 0;
    w->first = (unsigned)((first - (uintptr_t)w->units) / UNIT_STRIDE);
    w->last = (unsigned)((last - (uintptr_t)w->units) / UNIT_STRIDE);
    w->threshold = *(unsigned*)(state + 0xD30) > 255u ? 255 : *(unsigned*)(state + 0xD30);
    w->shift = (fake_GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    return 1;
}

static char* unit(const World* w, unsigned slot)
{
    char* u;
    unsigned flags;
    if (!slot || slot >= w->nslots) return NULL;
    u = w->units + slot * UNIT_STRIDE;
    flags = *(unsigned*)(u + U_FLAGS);
    return (flags & 0x10000000u) && !(flags & 0x4000u) ? u : NULL;
}

static unsigned def_id(const World* w, const char* u)
{
    uintptr_t d = (uintptr_t)*(char* const*)(u + U_TYPE), base = (uintptr_t)w->defs;
    uintptr_t offset;
    if (d < base) return 0;
    offset = d - base;
    if (offset % D_STRIDE || offset / D_STRIDE >= w->ndefs) return 0;
    return (unsigned)(offset / D_STRIDE);
}

static int footprint(const char* d, int* x, int* z)
{
    *x = *(short*)(d + 0x14A); *z = *(short*)(d + 0x14C);
    return *x > 0 && *z > 0 && *x <= 64 && *z <= 64;
}

static int kind(const char* d)
{
    const unsigned char* yard;
    float metal = *(float*)(d + 0x1CE);
    int fx, fz;
    if (d[0x22F] || !footprint(d, &fx, &fz) ||
        !*(const unsigned char* const*)(d + 0x14E)) return 0;
    if (isfinite(metal) && metal > 0) return 1;
    /* The loader allocates exactly FootX*FootZ yard bytes (0x42CF30).
       Unlike TADR's 64-byte heuristic, inspect the complete bounded yard. */
    yard = *(const unsigned char* const*)(d + 0x14E);
    return yard && memchr(yard, 0x8F, (size_t)fx * fz) ? 2 : 0;
}

static float strength(const char* d, int k)
{
    float v = *(float*)(d + (k == 1 ? 0x1CE : 0x1C2));
    return isfinite(v) && v > 0 ? v : 0;
}

static int selected(const World* w, const char* u)
{
    return *(unsigned char*)(u + U_OWNER) == w->player && (*(unsigned*)(u + U_FLAGS) & 0x10u);
}

static int can_build(const World* w, const char* u, unsigned type)
{
    unsigned id = def_id(w, u), count, i;
    const char* d;
    const unsigned short* list;
    if (!id) return 0;
    d = w->defs + id * D_STRIDE;
    if (!(*(unsigned*)(d + D_FLAGS) & 0x40u)) return 0;
    count = *(unsigned*)(d + 0x152); list = *(const unsigned short* const*)(d + 0x156);
    /* CANBUILD's writers allocate bl_room(count) entries; all writers and
       readers run on the game thread at this point. Bound the count and
       every type taken from it; definitions live until level teardown. */
    if (!list || count > TAGPU_LIM_TYPES) return 0;
    for (i = 0; i < count; i++) if (list[i] == type && type < w->ndefs) return 1;
    return 0;
}

static int can_assist(const World* w, const char* u)
{
    unsigned id = def_id(w, u);
    const char* d;
    if (!id) return 0;
    d = w->defs + id * D_STRIDE;
    return (*(unsigned*)(d + D_FLAGS) & 0x40u) &&
           (*(unsigned*)(d + 0x245) & 0x200u) && *(unsigned short*)(d + 0x1FE);
}

static int discovered(const World* w, int x, int z)
{
    const unsigned short* mapped;
    unsigned bit;
    if (x < 0 || z < 0 || x >= w->w || z >= w->h) return 0;
    mapped = *(const unsigned short* const*)(w->ta + 0x14273);
    bit = *(unsigned char*)(w->ta + 0x1B63 + 0x14B * w->player + 0x146);
    if (!mapped || bit >= 16) return 0;
    /* LoadMap allocates W*H/4 u16 entries; dimensions are even; the setter
       0x481930 uses the player's map bit, not its seat number. */
    return (mapped[(z / 2) * (w->w / 2) + x / 2] & (1u << bit)) != 0;
}

static int metal(const World* w, int x, int z)
{
    return discovered(w, x, z) ? (unsigned char)w->grid[((size_t)z * w->w + x) * FT_STRIDE + 7] : -1;
}

static int coverage(const World* w, int x, int z, int fx, int fz)
{
    int dx, dz, count = 0;
    for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++)
        if (metal(w, x + dx, z + dz) > w->threshold) count++;
    return count;
}

static int fits(const World* w, unsigned type, int x, int z, unsigned ignore, int* height)
{
    char* d = w->defs + type * D_STRIDE;
    int fx, fz, dx, dz, ok;
    unsigned char copy[D_STRIDE], yard[64 * 64];
    if (!footprint(d, &fx, &fz) || x < 1 || z < 1 || x + fx >= w->w || z + fz >= w->h ||
        x * 16 + fx * 8 > 32767 || z * 16 + fz * 8 > 32767) return 0;
    for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++)
        if (!discovered(w, x + dx, z + dz)) return 0;
    if (ignore) {
        const char* old = unit(w, ignore);
        unsigned id;
        int ox, oz, ofx, ofz;
        const unsigned char *oldyard, *newyard;
        if (!old || !(id = def_id(w, old))) return 0;
        if (*(unsigned*)(old + U_FLAGS) & 0x20000000u) {
            /* DISASSEMBLED: 0x47D17C clears the yard bit for every old yard
               byte with bit 0. The candidate's bit 0 is read ONLY by
               0x47D530's yard-bit test. Project that post-clear test into a
               bounded private yard; the engine grid and definitions stay
               untouched. Contested footprints can re-stamp another unit
               during clear, so they cannot establish this projection. */
            if (*(unsigned*)(old + U_FLAGS) & 0x0C000000u) return 0;
            ox = *(short*)(old + 0x76); oz = *(short*)(old + 0x78);
            if (!footprint(w->defs + id * D_STRIDE, &ofx, &ofz) ||
                ofx != *(short*)(old + 0x7E) || ofz != *(short*)(old + 0x80) ||
                ox < 0 || oz < 0 || ox + ofx > w->w || oz + ofz > w->h) return 0;
            oldyard = *(const unsigned char* const*)(w->defs + id * D_STRIDE + 0x14E);
            newyard = *(const unsigned char* const*)(d + 0x14E);
            if (!oldyard || !newyard) return 0;
            memcpy(copy, d, sizeof copy); memcpy(yard, newyard, (size_t)fx * fz);
            for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++) {
                int rx = x + dx - ox, rz = z + dz - oz;
                if (rx >= 0 && rz >= 0 && rx < ofx && rz < ofz &&
                    (oldyard[rz * ofx + rx] & 1)) yard[dz * fx + dx] &= ~1u;
            }
            *(unsigned char**)(copy + 0x14E) = yard;
            d = (char*)copy;
        }
    }
    /* 0x47D554 exempts exactly the old unit number from occupancy checks. */
    ok = ((GridTest)0x0047D2E0u)(d, (unsigned)x | (unsigned)z << 16, ignore,
                                w->ta + 0x1B63 + w->player * 0x14B) & 1;
    if (ok && height) *height = ((SiteHeight)0x0047C780u)() & 255;
    return ok;
}

static int distinct(const World* w, Choice* c, int fx, int fz);
static int queued(const World* w, const Choice* c);
static int plans_block(const World* w, const Choice* c);

static int site(const World* w, unsigned type, const int seed[3], const int raw[3], int radius,
                int legal, unsigned ignore, Choice* out)
{
    int fx, fz, dx, dz, base_x = seed[0] >> 20, base_z = seed[2] >> 20;
    int best = 0, bx = 0, bz = 0, height = 0, k = kind(w->defs + type * D_STRIDE);
    double distance = 1e100;
    if (!k || !footprint(w->defs + type * D_STRIDE, &fx, &fz)) return 0;
    for (dz = -radius; dz <= radius; dz++) for (dx = -radius; dx <= radius; dx++) {
        int cx = base_x + dx, cz = base_z + dz, x = cx - fx / 2, z = cz - fz / 2;
        int h = 0, count;
        double tx = cx - raw[0] / 1048576.0 + 0.5;
        double tz = cz - raw[2] / 1048576.0 + 0.5;
        double dist = tx * tx + tz * tz;
        if (x < 0 || z < 0 || x + fx > w->w || z + fz > w->h ||
            x * 16 + fx * 8 > 32767 || z * 16 + fz * 8 > 32767) continue;
        count = k == 1 ? coverage(w, x, z, fx, fz) : 1;
        if (!count || count < best || (count == best && dist >= distance)) continue;
        if ((legal || k == 2) && !fits(w, type, x, z, ignore, &h)) continue;
        if (legal) {
            Choice candidate = {0};
            candidate.type = type; candidate.kind = k; candidate.x = x; candidate.z = z;
            candidate.height = h; candidate.target = ignore;
            if (plans_block(w, &candidate)) {
                if (k == 1 && !ignore && !distinct(w, &candidate, fx, fz)) continue;
                if (!queued(w, &candidate)) continue;
            }
        }
        best = count; distance = dist; bx = x; bz = z; height = h;
    }
    if (!best) return 0;
    out->type = type; out->kind = k; out->x = bx; out->z = bz; out->height = height;
    return 1;
}

/* A closed, explored metal component is a patch. A component reaching the
   finite window is broad metal ground; unexplored neighbors cannot prove
   closure. No unseen metal byte is consulted. Eight neighbors keep diagonal
   deposit cells in one patch. The arena bound is independent of map size. */
static int distinct(const World* w, Choice* c, int fx, int fz)
{
    int x, z, sx = -1, sz = -1, left, top;
    unsigned head = 0, tail = 0;
    int boundary = 0, edges = 0;
    for (z = c->z; z < c->z + fz && sx < 0; z++) for (x = c->x; x < c->x + fx; x++)
        if (metal(w, x, z) > w->threshold) { sx = x; sz = z; break; }
    if (sx < 0) return 0;
    left = sx - PATCH_R; top = sz - PATCH_R;
    memset(s_patch_seen, 0, sizeof s_patch_seen);
    s_patch_queue[tail++] = PATCH_R * PATCH_SIDE + PATCH_R;
    s_patch_seen[PATCH_R * PATCH_SIDE + PATCH_R] = 1;
    c->anchor_x = sx; c->anchor_z = sz;
    while (head < tail) {
        unsigned idx = s_patch_queue[head++];
        int rx = idx % PATCH_SIDE, rz = idx / PATCH_SIDE, dx, dz;
        if (!rx || !rz || rx == PATCH_SIDE - 1 || rz == PATCH_SIDE - 1) return 0;
        if (left + rx == 0) edges |= 1;
        if (left + rx == w->w - 1) edges |= 2;
        if (top + rz == 0) edges |= 4;
        if (top + rz == w->h - 1) edges |= 8;
        for (dz = -1; dz <= 1; dz++) for (dx = -1; dx <= 1; dx++) {
            unsigned next = (unsigned)(rz + dz) * PATCH_SIDE + rx + dx;
            int wx = left + rx + dx, wz = top + rz + dz;
            if (s_patch_seen[next] || wx < 0 || wz < 0 || wx >= w->w || wz >= w->h) continue;
            s_patch_seen[next] = 1;
            if (!discovered(w, wx, wz)) return 0;
            if (metal(w, wx, wz) <= w->threshold) { boundary = 1; continue; }
            if (tail >= PATCH_CAP) return 0;
            s_patch_queue[tail++] = (unsigned short)next;
            if (wz < c->anchor_z || (wz == c->anchor_z && wx < c->anchor_x))
                c->anchor_x = wx, c->anchor_z = wz;
        }
    }
    /* The edge of the map alone cannot distinguish a small uniform metal
       map from a deposit. Require a discovered background cell as well. */
    return boundary && (edges & 3) != 3 && (edges & 12) != 12;
}

static int resolve(const char* u, int command, const char* target, const int pos[3])
{
    int index = -1;
    const unsigned char* action = ((Resolve)0x0043F0E0u)(&index, command,
                                (void*)u, (void*)target, pos);
    /* 0x4401E3 returns the output pointer with byte zero on refusal;
       the native caller 0x48D0AF accepts only a nonzero action. */
    return action && *action ? *action : -1;
}

static void position(const World* w, const Choice* c, int pos[3])
{
    int fx, fz;
    footprint(w->defs + c->type * D_STRIDE, &fx, &fz);
    pos[0] = (c->x * 16 + fx * 8) * 65536;
    pos[1] = c->height * 65536;
    pos[2] = (c->z * 16 + fz * 8) * 65536;
}

static int snapped_site(const World* w, unsigned type, const int raw[3], int radius,
                        int legal, Choice* out)
{
    Choice c = {0};
    if (!site(w, type, raw, raw, radius, legal, 0, &c)) return 0;
    if (c.kind == 1) {
        Choice stable = {0};
        int fx, fz, next[3];
        footprint(w->defs + type * D_STRIDE, &fx, &fz);
        position(w, &c, next);
        if (c.x + fx / 2 != (raw[0] >> 20) || c.z + fz / 2 != (raw[2] >> 20)) {
            /* A search boundary can expose only part of a deposit. Both
               modes require the chosen coverage target to remain best in
               the footprint-sized neighborhood, using the raw mouse tie.
               Occupancy cannot turn a partial target into a complete one. */
            if (!site(w, type, next, raw, fx > fz ? fx : fz, 0, 0, &stable) ||
                stable.x != c.x || stable.z != c.z) return 0;
        }
    }
    *out = c;
    return 1;
}

static int build_node(const World* w, const char* n)
{
    const char* first = *(const char* const*)0x00512344u;
    const char* last = *(const char* const*)0x00512348u;
    uintptr_t bytes;
    unsigned action = (unsigned char)n[4], type = *(unsigned short*)(n + 0x36);
    if (!first || (uintptr_t)last < (uintptr_t)first) return 0;
    bytes = (uintptr_t)last - (uintptr_t)first;
    /* The loader's descriptor array is fixed for the level, stride 0x19.
       Capability bit 0 identifies placed build orders, not factory counts. */
    return bytes % 0x19u == 0 && action < bytes / 0x19u &&
        (*(unsigned*)(first + action * 0x19u + 0x0C) & 1u) &&
        type && type < w->ndefs && *(int*)(n + 0x22) > 0 && *(int*)(n + 0x2A) > 0;
}

static int plans_block(const World* w, const Choice* c)
{
    const char* d = w->defs + c->type * D_STRIDE;
    const char *primary = NULL, *target = c->target ? unit(w, c->target) : NULL;
    const unsigned char* yard = *(const unsigned char* const*)(d + 0x14E);
    int fx, fz, shift = w->shift;
    unsigned i;
    int pos[3];
    if (!footprint(d, &fx, &fz) || !yard) return 1;
    position(w, c, pos);
    if (!shift) for (i = w->first; i <= w->last; i++) {
        const char* u = unit(w, i);
        if (u && selected(w, u) && can_build(w, u, c->type) &&
            (!c->target || (target && resolve(u, 12, target, pos) >= 0))) { primary = u; break; }
    }
    for (i = w->first; i <= w->last; i++) {
        const char *u = unit(w, i), *n;
        unsigned steps = 0;
        int ordered;
        if (!u || *(unsigned char*)(u + U_OWNER) != w->player) continue;
        ordered = primary && selected(w, u) &&
            ((can_build(w, u, c->type) && (!c->target || resolve(u, 12, target, pos) >= 0)) ||
             (can_assist(w, u) && resolve(u, 7, primary, pos) >= 0));
        for (n = *(const char* const*)(u + U_ORDERS); n && steps++ < 16384;
             n = *(const char* const*)(n + 0x4A)) {
            const char* od;
            const unsigned char* oy;
            int ofx, ofz, ox, oz, x, z;
            if (*(const char* const*)(n + 0x0E) != u) break;
            if (!build_node(w, n)) continue;
            if (ordered &&
                !(*(unsigned*)(n + 0x42) & 4u)) continue;
            od = w->defs + *(unsigned short*)(n + 0x36) * D_STRIDE;
            if (od[0x22F] || !footprint(od, &ofx, &ofz) ||
                !(oy = *(const unsigned char* const*)(od + 0x14E))) continue;
            ox = (int)(((int64_t)*(int*)(n + 0x22) - ofx * 524288 + 524288) >> 20);
            oz = (int)(((int64_t)*(int*)(n + 0x2A) - ofz * 524288 + 524288) >> 20);
            for (z = c->z > oz ? c->z : oz; z < c->z + fz && z < oz + ofz; z++)
                for (x = c->x > ox ? c->x : ox; x < c->x + fx && x < ox + ofx; x++) {
                    unsigned a = yard[(z - c->z) * fx + x - c->x];
                    unsigned b = oy[(z - oz) * ofx + x - ox];
                    if (((a & 4u) && (b & 6u)) || ((b & 4u) && (a & 6u))) return 1;
                }
        }
    }
    return 0;
}

static int same_plan(const World* w, const Choice* c, const char* node)
{
    unsigned t = *(unsigned short*)(node + 0x36);
    int x, z, fx, fz;
    if (!build_node(w, node) || kind(w->defs + t * D_STRIDE) != c->kind ||
        !footprint(w->defs + t * D_STRIDE, &fx, &fz)) return 0;
    x = ((*(int*)(node + 0x22) >> 16) - fx * 8) / 16;
    z = ((*(int*)(node + 0x2A) >> 16) - fz * 8) / 16;
    if (c->kind == 2 || c->target) {
        int pos[3];
        position(w, c, pos);
        return llabs((int64_t)*(int*)(node + 0x22) - pos[0]) <= 1048576 &&
               llabs((int64_t)*(int*)(node + 0x2A) - pos[2]) <= 1048576;
    } else {
        Choice other = {0};
        other.x = x; other.z = z;
        return x >= 0 && z >= 0 && x + fx <= w->w && z + fz <= w->h &&
            distinct(w, &other, fx, fz) && other.anchor_x == c->anchor_x &&
            other.anchor_z == c->anchor_z;
    }
}

static const char* native_match(const char* u, int action, const char* target, const int* pos)
{
    const char* n;
    unsigned steps = 0;
    /* DISASSEMBLED 0x43AFF2..0x43B032: the Shift root takes the FIRST
       action/optional-target/position match, ignoring the build type.
       Mirror its unsigned +/-16-world-unit arithmetic, including wrap. */
    for (n = *(const char* const*)(u + U_ORDERS); n && steps++ < 16384;
         n = *(const char* const*)(n + 0x4A)) {
        if (*(const char* const*)(n + 0x0E) != u) return NULL;
        if ((unsigned char)n[4] == action && (!target || *(const char* const*)(n + 0x16) == target) &&
            (unsigned)pos[0] - *(const unsigned*)(n + 0x22) + 0x100000u <= 0x200000u &&
            (unsigned)pos[2] - *(const unsigned*)(n + 0x2A) + 0x100000u <= 0x200000u) return n;
    }
    return NULL;
}

static int find_cancel(const World* w, const char* u, const Choice* c, Cancel* out)
{
    const char *node, *previous = NULL, *target = c->target ? unit(w, c->target) : NULL;
    unsigned steps = 0;
    for (node = *(const char* const*)(u + U_ORDERS); node && steps++ < 16384;
         previous = node, node = *(const char* const*)(node + 0x4A)) {
        int pos[3], action;
        if (*(const char* const*)(node + 0x0E) != u) break;
        if (!same_plan(w, c, node) || (*(unsigned*)(node + 0x42) & 0x40000u)) continue;
        memcpy(pos, node + 0x22, sizeof pos);
        action = (unsigned char)node[4];
        /* Finding this exact first match on the main chain proves the
           engine root will unlink it rather than append or remove a neighbor.
           No simulation iteration or other mutator runs before the call. */
        if (native_match(u, action, NULL, pos) != node) continue;
        memset(out, 0, sizeof *out);
        out->action = action; out->type = *(unsigned short*)(node + 0x36);
        memcpy(out->pos, pos, sizeof pos);
        if (c->target) {
            if (!target || !previous || (*(unsigned*)(previous + 0x42) & 0x40000u) ||
                *(const char* const*)(previous + 0x16) != target ||
                resolve(u, 12, target, pos) != (unsigned char)previous[4] ||
                memcmp(previous + 0x22, pos, sizeof pos)) continue;
            out->reclaim = (unsigned char)previous[4]; out->target = c->target;
            memcpy(out->reclaim_pos, previous + 0x22, sizeof out->reclaim_pos);
            if (native_match(u, out->reclaim, target, out->reclaim_pos) != previous) continue;
        }
        return 1;
    }
    return 0;
}

static int queued(const World* w, const Choice* c)
{
    unsigned i;
    int found = 0;
    for (i = w->first; i <= w->last; i++) {
        const char *u = unit(w, i), *node;
        unsigned steps = 0;
        if (!u || *(unsigned char*)(u + U_OWNER) != w->player) continue;
        /* The live unit owns this heap chain; its sole mutator/destructor is
           this thread. The walk is bounded even for a malformed cycle. */
        for (node = *(const char* const*)(u + U_ORDERS); node && steps++ < 16384;
             node = *(const char* const*)(node + 0x4A)) {
            if (*(const char* const*)(node + 0x0E) != u) break;
            if (same_plan(w, c, node)) {
                Cancel cancel;
                found |= TAGPU_RESOURCE_QUEUED;
                if (selected(w, u) && find_cancel(w, u, c, &cancel))
                    return found | TAGPU_RESOURCE_CANCEL;
                break;
            }
        }
    }
    return found;
}

static unsigned unqueue(const World* w, const Choice* c)
{
    unsigned i, count = 0;
    for (i = w->first; i <= w->last; i++) {
        const char* u = unit(w, i);
        Cancel cancel;
        if (!u || !selected(w, u) || !find_cancel(w, u, c, &cancel)) continue;
        /* Copy values, never retain a heap node across destruction. The
           bounded unit arena remains live throughout these input roots. */
        ((Order)0x0043AFC0u)(cancel.action, 1, (void*)u, NULL, cancel.pos, cancel.type, 0);
        if (cancel.target) {
            const char* target = unit(w, cancel.target);
            const char* reclaim = target ? native_match(u, cancel.reclaim, target, cancel.reclaim_pos) : NULL;
            /* Destructors may re-enter handlers. Re-query the remaining
               pair by copied values; never append a reclaim if it vanished. */
            if (reclaim && !(*(unsigned*)(reclaim + 0x42) & 0x40000u) &&
                !memcmp(reclaim + 0x22, cancel.reclaim_pos, sizeof cancel.reclaim_pos))
                ((Order)0x0043AFC0u)(cancel.reclaim, 1, (void*)u, (void*)target, cancel.reclaim_pos, 0, 0);
        }
        count++;
    }
    return count;
}

static int quick_choice(const World* w, Choice* chosen)
{
    unsigned i, t, target_id = *(unsigned short*)(w->ta + 0x2CBA);
    unsigned char available[TAGPU_LIM_TYPES] = {0};
    const char* target = unit(w, target_id);
    int desired = 0;
    float old = 0;
    Choice best[3] = {{0}};
    const TagpuResourcePolicy* p = tagpu_profiles_policy();
    if (!s_raw_valid || *(unsigned char*)(w->ta + M_MODE) != 1 ||
        !(*(unsigned char*)(w->ta + M_REGION) & 2) || !tagpu_profiles_quick() ||
        (fake_GetAsyncKeyState(VK_MENU) & 0x8000)) return 0;
    if (target) {
        unsigned id = def_id(w, target);
        if (!id || *(unsigned char*)(target + U_OWNER) != w->player ||
            *(float*)(target + 0x104) != 0 || !(desired = kind(w->defs + id * D_STRIDE))) return 0;
        old = strength(w->defs + id * D_STRIDE, desired);
    } else {
        unsigned feature = *(unsigned short*)(w->ta + 0x2CBC);
        unsigned count = *(unsigned*)(w->ta + OFF_FEATCOUNT);
        const char* defs = *(const char* const*)(w->ta + OFF_FEATDEF);
        if (count <= 0xFFFBu && feature < count && defs && (*(unsigned short*)(defs + feature * FD_STRIDE + 0xFE) & 0x80u)) return 0;
    }
    for (i = w->first; i <= w->last; i++) {
        const char* u = unit(w, i);
        unsigned id, n, j;
        const char* d;
        const unsigned short* list;
        if (!u || !selected(w, u) || !(id = def_id(w, u))) continue;
        d = w->defs + id * D_STRIDE;
        if (!(*(unsigned*)(d + D_FLAGS) & 0x40u)) continue;
        n = *(unsigned*)(d + 0x152); list = *(const unsigned short* const*)(d + 0x156);
        if (!list || n > TAGPU_LIM_TYPES) continue;
        if (target && resolve(u, 12, target, s_raw) < 0) continue;
        for (j = 0; j < n; j++) if (list[j] && list[j] < w->ndefs) available[list[j]] = 1;
    }
    memset(chosen, 0, sizeof *chosen);
    for (t = 1; t < w->ndefs; t++) if (available[t]) {
        Choice c = {0};
        char* d = w->defs + t * D_STRIDE;
        int k = kind(d), fx, fz;
        float value;
        if (!k || (desired && k != desired) || (value = strength(d, k)) <= old) continue;
        if (best[k].type && value <= best[k].strength) continue;
        if (!footprint(d, &fx, &fz)) continue;
        if (target) {
            int pos[3];
            memcpy(pos, target + U_XFIX, sizeof pos);
            c.x = ((pos[0] >> 16) - fx * 8 + 8) / 16;
            c.z = ((pos[2] >> 16) - fz * 8 + 8) / 16;
            if (!fits(w, t, c.x, c.z, *(unsigned short*)(target + U_INDEX), &c.height)) continue;
            c.type = t; c.kind = k;
        } else {
            if (!snapped_site(w, t, s_raw, p->radius, 1, &c)) continue;
            if (k == 1 && !distinct(w, &c, fx, fz)) continue;
        }
        c.strength = value; c.target = target ? target_id : 0;
        c.queued = queued(w, &c);
        if (!c.queued && plans_block(w, &c)) continue;
        best[k] = c;
    }
    *chosen = best[1].type ? best[1] : best[2];
    if (best[1].type && best[2].type) {
        int pos[3], oldpos[3];
        int64_t dx, dz, d1, d2;
        position(w, &best[2], pos); position(w, &best[1], oldpos);
        dx = (pos[0] - (int64_t)s_raw[0]) >> 16; dz = (pos[2] - (int64_t)s_raw[2]) >> 16;
        d1 = dx * dx + dz * dz;
        dx = (oldpos[0] - (int64_t)s_raw[0]) >> 16; dz = (oldpos[2] - (int64_t)s_raw[2]) >> 16;
        d2 = dx * dx + dz * dz;
        if (d1 < d2) *chosen = best[2];
    }
    if (!chosen->type) return 0;
    chosen->queued = queued(w, chosen);
    return 1;
}

void tagpu_resource_level_end(void) { s_raw_valid = 0; }

void tagpu_resource_mouse(void)
{
    char* ta = *(char**)TA_MAIN_PP;
    if (!s_armed || !ta) { s_raw_valid = 0; return; }
    tagpu_profiles_begin_match();
    memcpy(s_raw, ta + M_POINT, sizeof s_raw);
    s_raw_valid = 1;
}

static int __cdecl manual_cursor(void* frame)
{
    World w;
    Choice c = {0};
    unsigned type;
    int radius, pos[3];
    (void)frame;
    if (!s_raw_valid || !world(&w) || *(unsigned char*)(w.ta + M_MODE) != 14) return 0;
    /* Restore the raw point on every evaluation, including a stationary Alt
       change. The capture is made after the viewport's mouse repair. */
    memcpy(w.ta + M_POINT, s_raw, sizeof s_raw);
    type = *(unsigned short*)(w.ta + M_TYPE);
    radius = tagpu_profiles_radius();
    if (!type || type >= w.ndefs || !radius || (fake_GetAsyncKeyState(VK_MENU) & 0x8000)) return 0;
    if (snapped_site(&w, type, s_raw, radius, 0, &c)) {
        position(&w, &c, pos);
        pos[1] = s_raw[1];
        memcpy(w.ta + M_POINT, pos, sizeof pos);
    }
    return 0;
}

static int existing_guard(const char* u, const char* target, int action)
{
    const char* n;
    unsigned steps = 0;
    for (n = *(const char* const*)(u + U_ORDERS); n && steps++ < 16384;
         n = *(const char* const*)(n + 0x4A))
        if (*(unsigned char*)(n + 4) == action && *(const char* const*)(n + 0x16) == target) return 1;
    return 0;
}

static int click(void* frame, int right)
{
    World w;
    Choice c;
    const int* record = ((const int* const*)frame)[1];
    const char* primary[TAGPU_LIM_UNITS];
    unsigned char actions[TAGPU_LIM_UNITS];
    int reclaims[TAGPU_LIM_UNITS];
    unsigned np = 0, i, helper = 0;
    int shift, pos[3];
    if (!s_armed || !world(&w)) return 0;
    if (!right && *(unsigned char*)(w.ta + M_MODE) == 14) {
        manual_cursor(NULL);
        ((void (__stdcall*)(void))0x004197D0u)();
        return 0;
    }
    if ((*(int*)(w.ta + 0x37EFA) != 0) != right) return 0;
    /* The queued mouse record owns its modifier state. A later key release
       must not change which existing plans this click will preserve. */
    w.shift = (record[2] & 4) != 0;
    {
        unsigned scratch0 = *(unsigned*)0x0051E684u, scratch1 = *(unsigned*)0x0051E688u;
        int have = quick_choice(&w, &c);
        *(unsigned*)0x0051E684u = scratch0; *(unsigned*)0x0051E688u = scratch1;
        if (!have) return 0;
    }
    if (c.queued) {
        if (w.shift && (c.queued & TAGPU_RESOURCE_CANCEL)) {
            unsigned removed = unqueue(&w, &c);
            tagpu_logf("resource: unqueued type=%u site=%d,%d builders=%u upgrade=%u",
                       c.type, c.x, c.z, removed, c.target);
            return 1;
        }
        tagpu_logf("resource: already queued type=%u site=%d,%d", c.type, c.x, c.z);
        return 1;
    }
    position(&w, &c, pos);
    shift = w.shift;
    for (i = w.first; i <= w.last; i++) {
        const char* u = unit(&w, i);
        if (u && selected(&w, u) && can_build(&w, u, c.type) &&
            (!c.target || resolve(u, 12, unit(&w, c.target), pos) >= 0)) {
            if (np >= TAGPU_LIM_UNITS) return 0;
            const char* d = w.defs + def_id(&w, u) * D_STRIDE;
            unsigned char action = 0;
            ((Action)0x00438760u)(&action, (*(unsigned*)(d + D_FLAGS) & 0x800u) ?
                "VTOL_MOBILEBUILD" : "MOBILEBUILD");
            if (!action) return 0;
            primary[np] = u; actions[np] = action;
            reclaims[np] = c.target ? resolve(u, 12, unit(&w, c.target), pos) : -1;
            np++;
        }
    }
    if (!np) return 0;
    for (i = 0; i < np; i++) {
        const char* u = primary[i];
        if (c.target) {
            const char* target = unit(&w, c.target);
            ((Order)0x0043AFC0u)(reclaims[i], shift, (void*)u, (void*)target, pos, 0, 0);
        }
        ((Order)0x0043AFC0u)(actions[i], shift || c.target, (void*)u, NULL, pos, c.type, 0);
    }
    for (i = w.first; i <= w.last; i++) {
        const char *u = unit(&w, i), *main;
        unsigned j;
        int guard;
        if (!u || !selected(&w, u) || !can_assist(&w, u)) continue;
        for (j = 0; j < np && u != primary[j]; j++) {}
        if (j < np) continue;
        main = primary[helper++ % np];
        guard = resolve(u, 7, main, pos);
        if (guard < 0) continue;
        if (shift) {
            for (j = 0; j < np && !existing_guard(u, primary[j], guard); j++) {}
            if (j < np) continue;
        }
        ((Order)0x0043AFC0u)(guard, shift, (void*)u, (void*)main, pos, 0, 0);
    }
    tagpu_logf("resource: quick type=%u site=%d,%d builders=%u upgrade=%u shift=%d",
               c.type, c.x, c.z, np, c.target, shift);
    return 1;
}
static int __cdecl left_click(void* frame) { return click(frame, 0); }
static int __cdecl right_click(void* frame) { return click(frame, 1); }

void tagpu_resource_packet(TAGPU_PACKET* p)
{
    World w;
    Choice c;
    int fx, fz, have;
    unsigned scratch0, scratch1;
    if (!s_armed || !p->in_game || !world(&w)) return;
    scratch0 = *(unsigned*)0x0051E684u; scratch1 = *(unsigned*)0x0051E688u;
    have = quick_choice(&w, &c);
    *(unsigned*)0x0051E684u = scratch0; *(unsigned*)0x0051E688u = scratch1;
    if (!have) return;
    footprint(w.defs + c.type * D_STRIDE, &fx, &fz);
    p->cursor_mode = 14; p->build_unit_id = (unsigned short)c.type;
    p->region_flags = (p->region_flags & ~0x48u) | 0x40u;
    p->resource_queued = c.queued;
    p->build_rect[0] = c.x * 16; p->build_rect[1] = c.height; p->build_rect[2] = c.z * 16;
    p->build_rect[3] = (c.x + fx) * 16; p->build_rect[4] = c.height; p->build_rect[5] = (c.z + fz) * 16;
}

int tagpu_resource_live(void) { return s_armed; }

void tagpu_resource_init(void)
{
    static const unsigned char build[] = {0x83,0xEC,0x14,0x8B,0x15,0xE8,0x1D,0x51,0x00};
    static const unsigned char left[] = {0x51,0xA1,0xE8,0x1D,0x51,0x00};
    static const unsigned char right[] = {0x51,0x8B,0x0D,0xE8,0x1D,0x51,0x00};
    if (!tagpu_vpwide_mouse_world_live()) {
        tagpu_logf("resource: raw mouse coordinate producer unavailable; not armed"); return;
    }
    if (!tagpu_build_lists_live()) {
        tagpu_logf("resource: CANBUILD allocation bound unavailable; not armed"); return;
    }
    if (!tagpu_detour_bytes_ok(0x004197D0u, build, sizeof build) ||
        !tagpu_detour_bytes_ok(0x00498F70u, left, sizeof left) ||
        !tagpu_detour_bytes_ok(0x00499100u, right, sizeof right)) {
        tagpu_logf("resource: incompatible input/placement sites; not armed"); return;
    }
    if (tagpu_detour_observe(0x004197D0u, build, sizeof build, manual_cursor, NULL) &&
        tagpu_detour_gate(0x00498F70u, left, sizeof left, left_click, 4) &&
        tagpu_detour_gate(0x00499100u, right, sizeof right, right_click, 4)) {
        s_armed = 1;
        tagpu_logf("resource: input and placement hooks armed");
    } else tagpu_logf("resource: hook installation failed; not armed");
}
