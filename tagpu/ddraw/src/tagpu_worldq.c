/* tagpu_worldq.c -- worldq.h over the running engine (tagpu_worldq.h): every engine read and order the
   resource, placement and sweep rules make, each bounded as stated beside it. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "tagpu_worldq.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_packet.h"
#include "tagpu_puppet.h"
#include "tagpu_simreg.h"

SIM_FEATURE(worldq, SIM_PUPPET_OWNER, "resource-mex resource-upgrade resource-queued-upgrade resource-mixed resource-plain resource-uniform resource-locked resource-inherit resource-classic resource-geo-upgrade resource-nearby resource-priority resource-refusals resource-frontend place-basic sweep-basic");

#define D_STRIDE  0x249u
#define D_BASE    0x1439B
#define D_FLAGS   0x241
#define D_TAGS    0x245
#define U_FLAGS   0x110
#define U_ORDERS  0x5C
#define N_NEXT    0x4A
#define N_OWNER   0x0E
#define N_TARGET  0x16
#define N_POS     0x22
#define N_TYPE    0x36
#define N_FLAGS   0x42
#define N_OWNED   0x52
#define PL_CONTROL 0x73
#define VA_SITE_CALL 0x004198C6u

typedef unsigned (__stdcall* SiteHeight)(void);
typedef unsigned (__stdcall* BlockedHeight)(const void*, unsigned);
typedef const unsigned char* (__stdcall* Resolve)(int*, unsigned, void*, void*, const void*);
typedef void (__stdcall* Order)(int, int, void*, void*, const void*, int, int);
typedef void (__stdcall* RemoveOrder)(void*, const void*);
typedef unsigned char* (__attribute__((thiscall))* Action)(void*, const char*);
typedef int (__stdcall* GroundHeight)(const int*);
typedef int (__stdcall* Mapped)(const void*, const int*);

/* the build cursor's site test, decoded once at attach */
static WqSiteFn s_cursor_site;
SIM_VAR(s_cursor_site, SIM_INSTALL);

/* The action names 0x438760 is given, the engine's own strings in .rdata, in WQ_ACT_* order: the build
   cursor's (0x419670) and the resolver's (0x440187, 0x43F694, 0x44004C). */
static const uintptr_t ACT_NAME[WQ_ACTS] = { 0x00502654u, 0x00502640u, 0x005016C4u, 0x00501B68u, 0x005052F0u };

static WQ_ENGINE* ctx_of(const WORLDQ* q) { return (WQ_ENGINE*)q->self; }

/* THE CALL AT 0x4198C6, decoded. tagpu_patches.c's queued-site fix rewrites it before this runs (DllMain's
   order), and nothing writes it after attach, so the target found here is the one the build cursor calls for
   the rest of the process. */
void tagpu_worldq_init(void)
{
    const unsigned char* call = (const unsigned char*)(uintptr_t)VA_SITE_CALL;
    s_cursor_site = call[0] == 0xE8
        ? (WqSiteFn)(uintptr_t)(VA_SITE_CALL + 5 + *(const unsigned*)(call + 1)) : NULL;
}

WqSiteFn tagpu_worldq_cursor_site(void) { return s_cursor_site; }

static int footprint(const char* d, int* x, int* z)
{
    *x = *(short*)(d + 0x14A); *z = *(short*)(d + 0x14C);
    return *x > 0 && *z > 0 && *x <= WQ_FOOT_MAX && *z <= WQ_FOOT_MAX;
}

static const char* def_at(const WQ_ENGINE* c, int type)
{
    return type > 0 && (unsigned)type < c->ndefs ? c->defs + (unsigned)type * D_STRIDE : NULL;
}

/* An alive unit that is not excluded at `slot`, of any owner, or NULL. */
static char* live_unit(const WQ_ENGINE* c, unsigned slot)
{
    char* u;
    unsigned flags;
    if (!slot || slot >= c->nslots) return NULL;
    u = c->units + slot * UNIT_STRIDE;
    flags = *(unsigned*)(u + U_FLAGS);
    return (flags & 0x10000000u) && !(flags & 0x4000u) ? u : NULL;
}

static char* own_unit(const WQ_ENGINE* c, unsigned slot)
{
    char* u = live_unit(c, slot);
    return u && *(unsigned char*)(u + U_OWNER) == (unsigned)c->seat ? u : NULL;
}

static unsigned def_id(const WQ_ENGINE* c, const char* u)
{
    uintptr_t d = (uintptr_t)*(char* const*)(u + U_TYPE), base = (uintptr_t)c->defs, offset;
    if (d < base) return 0;
    offset = d - base;
    if (offset % D_STRIDE || offset / D_STRIDE >= c->ndefs) return 0;
    return (unsigned)(offset / D_STRIDE);
}

/* The resource building a definition is: 0 none, 1 an extractor (ExtractsMetal finite and above 0), 2 a
   geothermal plant (a 0x8F yard byte). The loader allocates exactly FootX * FootZ yard bytes (0x42CF30), so
   the whole bounded yard is inspected, unlike TADR's 64-byte heuristic. */
static int kind(const char* d)
{
    const unsigned char* yard;
    float metal = *(float*)(d + 0x1CE);
    int fx, fz;
    if (d[0x22F] || !footprint(d, &fx, &fz) || !*(const unsigned char* const*)(d + 0x14E)) return 0;
    if (isfinite(metal) && metal > 0) return WQ_KIND_MEX;
    yard = *(const unsigned char* const*)(d + 0x14E);
    return yard && memchr(yard, 0x8F, (size_t)fx * fz) ? WQ_KIND_GEO : 0;
}

int tagpu_worldq_def_kind(const void* def) { return kind((const char*)def); }

/* ExtractsMetal for an extractor, EnergyMake otherwise; 0 when not finite or not above 0 */
static float strength(const char* d, int k)
{
    float v = *(float*)(d + (k == WQ_KIND_MEX ? 0x1CE : 0x1C2));
    return isfinite(v) && v > 0 ? v : 0;
}

/* The explored-map grid: LoadMap allocates W*H/4 u16 entries, the dimensions even (the open's bound), and
   the stamp 0x481930 writes the player's map bit +0x146 -- not its seat number. */
static int discovered(const WQ_ENGINE* c, int x, int z)
{
    const unsigned short* mapped;
    unsigned bit;
    if (x < 0 || z < 0 || x >= c->w || z >= c->h) return 0;
    mapped = *(const unsigned short* const*)(c->ta + 0x14273);
    bit = *(unsigned char*)(c->rec + 0x146);
    if (!mapped || bit >= 16) return 0;
    return (mapped[(z / 2) * (c->w / 2) + x / 2] & (1u << bit)) != 0;
}

static int metal(const WQ_ENGINE* c, int x, int z)
{
    return discovered(c, x, z) ? (unsigned char)c->grid[((size_t)z * c->w + x) * FT_STRIDE + 7] : -1;
}

/* The cell word the site tests take: x in the low half, z in the high, each a signed 16-bit value the test
   bounds itself (0x47D302..0x47D352: x, z >= 1, x + fx < W, z + fz < H), as 0x47D820 does
   (0x47D834..0x47D871). */
static unsigned cell_word(int cx, int cz)
{
    return (unsigned)(uint16_t)cx | (unsigned)(uint16_t)cz << 16;
}

/* The loader's order descriptor array is fixed for the level, stride 0x19; capability bit 0 marks a placed
   build (the one 0x438C00 draws a rectangle for), not a factory's count node. */
static int build_node(const WQ_ENGINE* c, const char* n)
{
    const char* first = *(const char* const*)0x00512344u;
    const char* last = *(const char* const*)0x00512348u;
    uintptr_t bytes;
    unsigned action = (unsigned char)n[4], type = *(unsigned short*)(n + N_TYPE);
    if (!first || (uintptr_t)last < (uintptr_t)first) return 0;
    bytes = (uintptr_t)last - (uintptr_t)first;
    return bytes % 0x19u == 0 && action < bytes / 0x19u &&
        (*(unsigned*)(first + action * 0x19u + 0x0C) & 1u) &&
        type && type < c->ndefs && *(int*)(n + N_POS) > 0 && *(int*)(n + N_POS + 8) > 0;
}

/* ---- the ops ---- */

static int wq_map(const WORLDQ* q, WQ_MAP* out)
{
    const WQ_ENGINE* c = ctx_of(q);
    out->w = c->w; out->h = c->h; out->threshold = c->threshold;
    out->sea = *(const unsigned char*)(c->ta + 0x1427F);
    out->lostype = *(const unsigned char*)(c->ta + 0x14281);
    /* no bound of the engine's own [INFERRED: the placement and the sweep cap their appends
       themselves, below the recorder's 4096 nodes a list] */
    out->list_cap = 0;
    return 1;
}

static int wq_explored(const WORLDQ* q, int cx, int cz) { return discovered(ctx_of(q), cx, cz); }
static int wq_metal(const WORLDQ* q, int cx, int cz) { return metal(ctx_of(q), cx, cz); }

static int wq_coverage(const WORLDQ* q, int cx, int cz, int fx, int fz)
{
    const WQ_ENGINE* c = ctx_of(q);
    int dx, dz, count = 0;
    for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++)
        if (metal(c, cx + dx, cz + dz) > c->threshold) count++;
    return count;
}

/* 0x408090(player, pos): the mapped test the resolver's reclaim case makes inline, patched by the same fix
   (tagpu_patches.c, LOS_MAPPED). DISASSEMBLED: it reads the bit 1 << main[0x2A43], the local player's
   (0x4080E0..0x4080F4), whatever record it is passed, so it answers for the local seat only and is called
   for no other. */
static int wq_mapped_point(const WORLDQ* q, const int32_t pos[3])
{
    const WQ_ENGINE* c = ctx_of(q);
    if (c->seat != *(const unsigned char*)(c->ta + OFF_LOCALPLAYER)) return 0;
    return ((Mapped)0x00408090u)(c->rec, (const int*)pos) != 0;
}

static int wq_ground(const WORLDQ* q, const int32_t pos[3])
{
    (void)q;
    return ((GroundHeight)0x00485070u)((const int*)pos);
}

static int wq_ndefs(const WORLDQ* q) { return (int)ctx_of(q)->ndefs; }

static int wq_def(const WORLDQ* q, int type, WQ_DEF* out)
{
    const WQ_ENGINE* c = ctx_of(q);
    const char* d = def_at(c, type);
    unsigned flags, tags;
    memset(out, 0, sizeof *out);
    if (!d) return 0;
    flags = *(const unsigned*)(d + D_FLAGS);
    tags = *(const unsigned*)(d + D_TAGS);
    out->type = (uint16_t)type;
    out->fx = *(const short*)(d + 0x14A);
    out->fz = *(const short*)(d + 0x14C);
    out->kind = (uint8_t)kind(d);
    out->yard = *(const unsigned char* const*)(d + 0x14E) != NULL;
    out->workertime = *(const unsigned short*)(d + 0x1FE);
    out->reach = *(const unsigned short*)(d + 0x212);
    out->speed = *(const int*)(d + 0x192);
    out->strength = out->kind ? strength(d, out->kind) : 0;
    if (flags & 0x40u) out->flags |= WQ_DF_BUILDER;
    if ((flags & 0x40u) && (tags & 0x200u) && out->workertime) out->flags |= WQ_DF_ASSIST;
    if (flags & 0x800u) out->flags |= WQ_DF_VTOL;
    if (tags & 0x400u) out->flags |= WQ_DF_RECLAIMER;
    if (d[0x22F]) out->flags |= WQ_DF_MOVER;
    memcpy(out->name, d + UD_NAME, WQ_NAME - 1);
    return 1;
}

static int wq_yard(const WORLDQ* q, int type, uint8_t* out, int cap)
{
    const char* d = def_at(ctx_of(q), type);
    const unsigned char* yard;
    int fx, fz;
    if (!d || !footprint(d, &fx, &fz) || fx * fz > cap ||
        !(yard = *(const unsigned char* const*)(d + 0x14E))) return 0;
    memcpy(out, yard, (size_t)fx * fz);
    return fx * fz;
}

static int wq_build_options(const WORLDQ* q, int builder_type, uint16_t* out, int cap)
{
    const WQ_ENGINE* c = ctx_of(q);
    if (!def_at(c, builder_type) || !c->menu) return 0;
    return c->menu((unsigned)builder_type, out, cap);
}

/* THE SITE TEST, the instance's own (WQ_SITE_*), with the unit `ignore` taken as gone -- Quick Build's
   upgrade, where 0x47D554 exempts exactly that unit number from the occupancy checks.
   THE SCRATCH. DISASSEMBLED: 0x47D2E0 zeroes 0x51E684 and 0x51E688 (0x47D2EE, 0x47D2F4) and fills them with
   the site's altitude, which 0x47C780 reads back; the AI's 0x47C770 and the build cursor read them too. Both
   words are put back as they were after both calls, so a view taken in the mouse handler or the packet fill
   leaves the engine's own reading of them as it was.
   THE PLAYER. With a player 0x47D2E0 refuses a site whose centre the LOCAL player has not mapped
   (main[0x2A43], 0x47D3D1..0x47D3FA), and under LosType bit 1 reads the passed record's line-of-sight
   counters +0x7C (0x47D40C..0x47D42C) -- DISASSEMBLED; a unit or a yard occupant refuses only while that flag
   is set (0x47D53B, 0x47D55B). The record passed is the seat's. */
static int wq_site_ok(const WORLDQ* q, int type, int cx, int cz, int ignore, int* alt)
{
    const WQ_ENGINE* c = ctx_of(q);
    char* d = (char*)def_at(c, type);
    unsigned char copy[D_STRIDE], yard[WQ_FOOT_MAX * WQ_FOOT_MAX];
    unsigned scratch0, scratch1;
    int fx, fz, dx, dz, ok;
    if (!d || !footprint(d, &fx, &fz) || ignore < 0 || (unsigned)ignore >= c->nslots) return 0;
    if (ignore) {
        const char* old = live_unit(c, (unsigned)ignore);
        unsigned id;
        int ox, oz, ofx, ofz;
        const unsigned char *oldyard, *newyard;
        if (!old || !(id = def_id(c, old))) return 0;
        if (*(unsigned*)(old + U_FLAGS) & 0x20000000u) {
            /* DISASSEMBLED: 0x47D17C clears the yard bit for every old yard byte with bit 0. The
               candidate's bit 0 is read ONLY by 0x47D530's yard-bit test. Project that post-clear test
               into a bounded private yard; the engine grid and definitions stay untouched. Contested
               footprints can re-stamp another unit during clear, so they cannot establish this
               projection. */
            if (*(unsigned*)(old + U_FLAGS) & 0x0C000000u) return 0;
            ox = *(short*)(old + 0x76); oz = *(short*)(old + 0x78);
            if (!footprint(c->defs + id * D_STRIDE, &ofx, &ofz) ||
                ofx != *(short*)(old + 0x7E) || ofz != *(short*)(old + 0x80) ||
                ox < 0 || oz < 0 || ox + ofx > c->w || oz + ofz > c->h) return 0;
            oldyard = *(const unsigned char* const*)(c->defs + id * D_STRIDE + 0x14E);
            newyard = *(const unsigned char* const*)(d + 0x14E);
            if (!oldyard || !newyard) return 0;
            memcpy(copy, d, sizeof copy); memcpy(yard, newyard, (size_t)fx * fz);
            for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++) {
                int rx = cx + dx - ox, rz = cz + dz - oz;
                if (rx >= 0 && rz >= 0 && rx < ofx && rz < ofz &&
                    (oldyard[rz * ofx + rx] & 1)) yard[dz * fx + dx] &= ~1u;
            }
            *(unsigned char**)(copy + 0x14E) = yard;
            d = (char*)copy;
        }
    }
    scratch0 = *(unsigned*)0x0051E684u; scratch1 = *(unsigned*)0x0051E688u;
    ok = c->site(d, cell_word(cx, cz), (unsigned)ignore, c->rec) & 1;
    if (ok && alt) *alt = (int)(((SiteHeight)0x0047C780u)() & 255);
    *(unsigned*)0x0051E684u = scratch0; *(unsigned*)0x0051E688u = scratch1;
    return ok;
}

/* DISASSEMBLED 0x47D820, stdcall (def, x | z << 16), `ret 8`: the altitude the build cursor gives a blocked
   site (0x4198FE; 0x47C780 at 0x4198F5 gives a free one's, from the site test's scratch). It reads the def's
   footprint and yardmap and the map's heights and sea level, writes nothing outside its own frame, and
   answers 0 for a footprint outside [1, w) x [1, h). */
static int wq_blocked_alt(const WORLDQ* q, int type, int cx, int cz)
{
    const char* d = def_at(ctx_of(q), type);
    if (!d) return 0;
    return (int)(((BlockedHeight)0x0047D820u)(d, cell_word(cx, cz)) & 255);
}

/* The feature of a cell by its anchor, as 0x43E6D5..0x43E754 finds it: a word below 0xFFFB is an anchor,
   0xFFFE a footprint tile whose anchor is `cell - (byte[+0xA] * W + byte[+0xB])` cells back
   (0x43E71D..0x43E736), and the word found there must be an anchor's (0x43E73C). The engine bounds a direct
   anchor's def by the count (0x43E6F5) but not the hop's, nor tests that the hop stays on the grid; this does
   all three. The grid is bounded as the feature pass bounds it: the cells and defs present, W and H 1..4096,
   the def count 1..0xFFFA. The footprint is held to 1..16 cells. */
static int wq_feature(const WORLDQ* q, int cx, int cz, WQ_FEAT* out)
{
    const WQ_ENGINE* c = ctx_of(q);
    const unsigned char* defs = *(const unsigned char* const*)(c->ta + OFF_FEATDEF);
    int ndefs = *(const int*)(c->ta + OFF_FEATCOUNT), cell, fx, fz;
    const unsigned char *cl, *d;
    unsigned word;
    memset(out, 0, sizeof *out);
    if (!defs || c->w > 4096 || c->h > 4096 || ndefs <= 0 || ndefs >= 0xFFFB ||
        cx < 0 || cz < 0 || cx >= c->w || cz >= c->h) return 0;
    cell = cz * c->w + cx;
    cl = (const unsigned char*)c->grid + (size_t)cell * FT_STRIDE;
    word = *(const uint16_t*)(cl + FT_DEFIDX);
    if (word == 0xFFFEu) {
        cell -= cl[FT_WIDX] * c->w + cl[FT_WIDX + 1];
        if (cell < 0) return 0;
        word = *(const uint16_t*)(c->grid + (size_t)cell * FT_STRIDE + FT_DEFIDX);
    }
    if (word >= 0xFFFBu || (int)word >= ndefs) return 0;
    d = defs + (size_t)word * FD_STRIDE;
    fx = *(const int16_t*)(d + FD_FOOTX); fz = *(const int16_t*)(d + FD_FOOTZ);
    out->ax = cell % c->w; out->az = cell / c->w;
    out->def = (uint16_t)word;
    out->fx = (int16_t)(fx < 1 ? 1 : fx > 16 ? 16 : fx);
    out->fz = (int16_t)(fz < 1 ? 1 : fz > 16 ? 16 : fz);
    out->flags = d[FD_MASK];
    out->metal = *(const float*)(d + 0xF0);
    out->energy = *(const float*)(d + 0xEC);
    return 1;
}

static int wq_nslots(const WORLDQ* q) { return (int)ctx_of(q)->nslots; }

static int wq_seat_units(const WORLDQ* q, int* first, int* last)
{
    *first = (int)ctx_of(q)->first; *last = (int)ctx_of(q)->last;
    return 1;
}

static int wq_unit(const WORLDQ* q, int slot, WQ_UNIT* out)
{
    const WQ_ENGINE* c = ctx_of(q);
    const char* u = slot > 0 ? own_unit(c, (unsigned)slot) : NULL;
    memset(out, 0, sizeof *out);
    if (!u) return 0;
    out->slot = (uint16_t)slot;
    out->type = (uint16_t)def_id(c, u);
    out->index = *(const unsigned short*)(u + U_INDEX);
    out->owner = *(const unsigned char*)(u + U_OWNER);
    out->finished = *(const float*)(u + 0x104) == 0;
    memcpy(out->pos, u + U_XFIX, sizeof out->pos);
    out->state = *(const unsigned*)(u + U_FLAGS);
    return 1;
}

/* The unit's list, walked on the game thread, the one thread that links, unlinks and frees order nodes:
   bounded by `cap` and WQ_LIST_MAX, it stops at a node another unit owns. A target pointer becomes a slot by
   exact division against the unit array. */
static int wq_orders(const WORLDQ* q, int slot, WQ_ORDER* out, int cap)
{
    const WQ_ENGINE* c = ctx_of(q);
    const char *u = slot > 0 ? own_unit(c, (unsigned)slot) : NULL, *n;
    int k;
    if (!u) return -1;
    if (cap > WQ_LIST_MAX) cap = WQ_LIST_MAX;
    for (n = *(const char* const*)(u + U_ORDERS), k = 0; n && k < cap;
         n = *(const char* const*)(n + N_NEXT), k++) {
        uintptr_t t = (uintptr_t)*(const char* const*)(n + N_TARGET), base = (uintptr_t)c->units;
        WQ_ORDER* o = &out[k];
        if (*(const char* const*)(n + N_OWNER) != u) break;
        o->place = (uint16_t)k;
        o->action = (unsigned char)n[4];
        o->started = ((unsigned char)n[6] & 2u) != 0;
        o->build = (uint8_t)build_node(c, n);
        o->owned = *(const char* const*)(n + N_OWNED) != NULL;
        o->type = *(const unsigned short*)(n + N_TYPE);
        o->target = t >= base && (t - base) % UNIT_STRIDE == 0 && (t - base) / UNIT_STRIDE < c->nslots
            ? (uint16_t)((t - base) / UNIT_STRIDE) : (uint16_t)WQ_NONE;
        memcpy(o->pos, n + N_POS, sizeof o->pos);
        o->flags = *(const unsigned*)(n + N_FLAGS);
    }
    return k;
}

/* 0x43F0E0, stdcall (&index, mode, unit, target, pos), answers through its first argument a pointer to the
   action byte, 0 on refusal: 0x4401E3 returns the output pointer with byte zero, and the native caller
   0x48D0AF accepts only a nonzero action. */
static int wq_can(const WORLDQ* q, int slot, int mode, int target_slot, const int32_t pos[3])
{
    const WQ_ENGINE* c = ctx_of(q);
    char *u = slot > 0 ? own_unit(c, (unsigned)slot) : NULL, *target = NULL;
    int index = -1;
    const unsigned char* a;
    if (!u) return -1;
    if (target_slot != (int)WQ_NONE && !(target = target_slot > 0 ? live_unit(c, (unsigned)target_slot) : NULL))
        return -1;
    a = ((Resolve)0x0043F0E0u)(&index, (unsigned)mode, u, target, pos);
    return a && *a ? *a : -1;
}

/* 0x438760 writes the byte of an order name, 0 for a name it does not find (0x43880F, 0x438819); looked up
   once a view, on first use: the descriptor array it searches is fixed for a level. */
static int wq_action(const WORLDQ* q, int act)
{
    WQ_ENGINE* c = ctx_of(q);
    if (act < 0 || act >= WQ_ACTS) return -1;
    if (!c->got[act]) {
        c->act[act] = 0;
        ((Action)0x00438760u)(&c->act[act], (const char*)ACT_NAME[act]);
        c->got[act] = 1;
    }
    return c->act[act] ? c->act[act] : -1;
}

/* ORDERS OUT. Every command names units by slot, and before each root call the slot is re-derived: below
   nslots, alive and not excluded, owned by the seat, whose record's controller is a local human (1) or a
   local AI (2) and which is not a puppet seat (tagpu_puppet_seat, refused at the open as well) -- so on a
   puppet nothing is ordered, the recorder being the only writer of a puppet's state. A target slot is
   re-derived the same way without the owner test. A REMOVE walks the list again, bounded at WQ_LIST_MAX, to
   the node's place, and acts only on the node whose action, x, z and flags equal what `orders` copied. So a
   recycled slot or a list another call changed costs a refused command and never a call on memory the
   engine has freed: a bound, and a lifetime -- the game thread owns the unit array and the lists until the
   teardown, and every caller of this runs on that thread.
   NO NODE POINTER OUTLIVES 0x439F80: its destructor 0x43A1F0 runs the action's handler with event 2 when the
   node's +6 bit 2 is set, starts the StopBuilding script (0x505104) when +0x42 bit 0x400000 is set, and for
   the head calls 0x489800(unit, 3), which resets the unit's three slots and queues a COB start of
   TargetCleared (0x508D58, through 0x4B0A70); the roots 0x43ADC0 and 0x43AFC0 allocate and free nodes. */
static int wq_order(const WORLDQ* q, const WQ_CMD* cmd)
{
    const WQ_ENGINE* c = ctx_of(q);
    char *u = cmd->unit ? own_unit(c, cmd->unit) : NULL, *target = NULL;
    unsigned controller = *(const unsigned char*)(c->rec + PL_CONTROL);
    if (!u || (controller != 1 && controller != 2) || tagpu_puppet_seat((unsigned)c->seat)) return WQ_REFUSED;
    if (cmd->target != WQ_NONE && !(target = live_unit(c, cmd->target))) return WQ_REFUSED;
    if (cmd->verb == WQ_CMD_NEW) {
        ((Order)0x0043ADC0u)(cmd->action, cmd->shift, u, target, cmd->pos, cmd->type, 0);
        return WQ_OK;
    }
    if (cmd->verb == WQ_CMD_CLICK) {
        ((Order)0x0043AFC0u)(cmd->action, cmd->shift, u, target, cmd->pos, cmd->type, 0);
        return WQ_OK;
    }
    if (cmd->verb == WQ_CMD_REMOVE) {
        const WQ_ORDER* e = &cmd->expect;
        const char* node;
        unsigned place;
        for (node = *(const char* const*)(u + U_ORDERS), place = 0;
             node && place < e->place && place < WQ_LIST_MAX && *(const char* const*)(node + N_OWNER) == u;
             node = *(const char* const*)(node + N_NEXT), place++) {}
        if (!node || place != e->place || *(const char* const*)(node + N_OWNER) != u ||
            (unsigned char)node[4] != e->action || *(const int*)(node + N_POS) != e->pos[0] ||
            *(const int*)(node + N_POS + 8) != e->pos[2] || *(const unsigned*)(node + N_FLAGS) != e->flags)
            return WQ_REFUSED;
        ((RemoveOrder)0x00439F80u)(u, node);
        return WQ_OK;
    }
    return WQ_REFUSED;
}

static int wq_economy(const WORLDQ* q, WQ_ECON* out)
{
    (void)q;
    memset(out, 0, sizeof *out);
    return WQ_UNSUPPORTED;
}

static int wq_gametime(const WORLDQ* q) { return *(const int*)(ctx_of(q)->ta + OFF_GAMETIME); }

static const WORLDQ_OPS OPS = {
    wq_map, wq_explored, wq_metal, wq_coverage, wq_mapped_point, wq_ground, wq_ndefs, wq_def, wq_yard,
    wq_build_options, wq_site_ok, wq_blocked_alt, wq_feature, wq_nslots, wq_seat_units, wq_unit, wq_orders,
    wq_can, wq_action, wq_order, wq_economy, wq_gametime,
};

/* THE OPEN'S BOUNDS, each read value made data before it is an index: the map's W and H even and 2..0x7FFF,
   and W x H x 13 below 2^31 -- LoadMap's row product is the grid's allocation, so it is bounded before any
   index or byte product; the def count 1..TAGPU_LIM_TYPES; the unit array's span a whole number of 0x118
   records below TAGPU_PK_DESIGN_SLOTS and the slot count main+0x14351 one more than it; the seat's block
   (record +0x67 .. +0x6B) inside the array on record boundaries. The input path cannot run while the loader
   owns any of it. */
int tagpu_worldq_open(WQ_ENGINE* ctx, WORLDQ* out, int seat, int site, WqMenuFn menu)
{
    char *state, *end;
    uintptr_t first, last, distance;
    memset(ctx, 0, sizeof *ctx);
    memset(out, 0, sizeof *out);
    ctx->ta = *(char**)TA_MAIN_PP;
    if (!ctx->ta || seat < 0 || seat >= WQ_SEATS || tagpu_puppet_seat((unsigned)seat)) return 0;
    ctx->seat = seat;
    ctx->w = *(int*)(ctx->ta + OFF_MAP_W16); ctx->h = *(int*)(ctx->ta + OFF_MAP_H16);
    if (ctx->w < 2 || ctx->h < 2 || (ctx->w & 1) || (ctx->h & 1) || ctx->w > 0x7FFF || ctx->h > 0x7FFF ||
        (unsigned)ctx->w > 0x7FFFFFFFu / FT_STRIDE / (unsigned)ctx->h) return 0;
    ctx->ndefs = *(unsigned*)(ctx->ta + OFF_UDEFCOUNT);
    if (!ctx->ndefs || ctx->ndefs > TAGPU_LIM_TYPES) return 0;
    ctx->defs = *(char**)(ctx->ta + D_BASE);
    ctx->units = *(char**)(ctx->ta + OFF_UNIT_BEGIN);
    end = *(char**)(ctx->ta + OFF_UNIT_END);
    ctx->grid = *(char**)(ctx->ta + OFF_FEATMAP);
    state = *(char**)(ctx->ta + 0x391E9);
    if (!ctx->defs || !ctx->units || !ctx->grid || !state || (uintptr_t)end < (uintptr_t)ctx->units) return 0;
    distance = (uintptr_t)end - (uintptr_t)ctx->units;
    if (distance % UNIT_STRIDE || distance / UNIT_STRIDE >= TAGPU_PK_DESIGN_SLOTS) return 0;
    ctx->nslots = *(unsigned short*)(ctx->ta + OFF_UNITSLOTS);
    if (ctx->nslots != (unsigned)(distance / UNIT_STRIDE) + 1) return 0;
    ctx->rec = ctx->ta + OFF_PLAYERS + seat * PLAYER_STRIDE;
    first = (uintptr_t)*(char**)(ctx->rec + 0x67); last = (uintptr_t)*(char**)(ctx->rec + 0x6B);
    if (first < (uintptr_t)ctx->units || last < first || last > (uintptr_t)end ||
        (first - (uintptr_t)ctx->units) % UNIT_STRIDE || (last - (uintptr_t)ctx->units) % UNIT_STRIDE) return 0;
    ctx->first = (unsigned)((first - (uintptr_t)ctx->units) / UNIT_STRIDE);
    ctx->last = (unsigned)((last - (uintptr_t)ctx->units) / UNIT_STRIDE);
    ctx->threshold = *(unsigned*)(state + 0xD30) > 255u ? 255 : (int)*(unsigned*)(state + 0xD30);
    ctx->site = site == WQ_SITE_CURSOR ? s_cursor_site : (WqSiteFn)0x0047D2E0u;
    if (!ctx->site) return 0;
    ctx->menu = menu;
    out->ops = &OPS;
    out->self = ctx;
    out->seat = seat;
    return 1;
}
