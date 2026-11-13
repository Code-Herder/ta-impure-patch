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
#include "tagpu_addr.h"
#include "tagpu_weapons.h"
#include "tagpu_datakeys.h"

SIM_FEATURE(worldq, SIM_PUPPET_OWNER, "resource-mex resource-upgrade resource-queued-upgrade resource-mixed resource-plain resource-uniform resource-locked resource-inherit resource-classic resource-geo-upgrade resource-nearby resource-priority resource-refusals resource-frontend place-basic sweep-basic stock-sweep-basic");

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
#define N_COUNT   0x3A
#define N_FLAGS   0x42
#define N_OWNED   0x52
#define PL_CONTROL 0x73
#define VA_SITE_CALL 0x004198C6u
#define MC_BASE   0x00512358u       /* the 32 movement-class records (THE MOVEMENT CLASSES, below) */
#define MC_STRIDE 0x20u

typedef unsigned (__stdcall* SiteHeight)(void);
typedef unsigned (__stdcall* BlockedHeight)(const void*, unsigned);
typedef const unsigned char* (__stdcall* Resolve)(int*, unsigned, void*, void*, const void*);
typedef void (__stdcall* Order)(int, int, void*, void*, const void*, int, int);
typedef void (__stdcall* RemoveOrder)(void*, const void*);
typedef unsigned char* (__attribute__((thiscall))* Action)(void*, const char*);
typedef int (__stdcall* GroundHeight)(const int*);
typedef int (__stdcall* Mapped)(const void*, const int*);
typedef void (__stdcall* Queue)(int, void*, int, int);

/* the build cursor's site test, decoded once at attach; the seat's own, set once at attach when its patch is in */
static WqSiteFn s_cursor_site, s_seat_site;
SIM_VAR(s_cursor_site, SIM_INSTALL);
SIM_VAR(s_seat_site, SIM_INSTALL);

/* The action names 0x438760 is given, the engine's own strings in .rdata, in WQ_ACT_* order: the build
   cursor's (0x419670), the resolver's (0x440187, 0x43F694, 0x44004C) and the build menu's queue call for a unit
   with no mover, `BUILDINGBUILD` (0x419B76). */
static const uintptr_t ACT_NAME[WQ_ACTS] = { 0x00502654u, 0x00502640u, 0x005016C4u, 0x00501B68u, 0x005052F0u,
                                             0x00502684u };

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

void tagpu_worldq_seat_site_set(WqSiteFn thunk) { s_seat_site = thunk; }

static int footprint(const char* d, int* x, int* z)
{
    *x = *(short*)(d + 0x14A); *z = *(short*)(d + 0x14C);
    return *x > 0 && *z > 0 && *x <= WQ_FOOT_MAX && *z <= WQ_FOOT_MAX;
}

static const char* def_at(const WQ_ENGINE* c, int type)
{
    return type > 0 && (unsigned)type < c->ndefs ? c->defs + (unsigned)type * D_STRIDE : NULL;
}

/* an engine float as data: one not finite reads 0 */
static float finite_or_0(const char* p)
{
    float v = *(const float*)p;
    return isfinite(v) ? v : 0;
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

/* THE WIND'S MEAN. DISASSEMBLED 0x490C90..0x490D6B: each draw sets the speed main+0x37EDA = windmin +
   0x4B6C30(windmax - windmin) (main+0x1425B, +0x1425F), and 0x4B6C30(n) answers state mod n, 0 for n < 2
   (0x4B6C35): so the speed is uniform on [windmin, windmax - 1], or windmin alone. The ratio the economy pass
   pays a windgenerator by (0x40156F) is speed / main+0x37EC8 (fidiv of the two dwords), stored at
   main+0x37EDE and set to 1.0 when above it. Impure's wind fix draws by the same rules from its own stream
   (engine/map.md, The wind). So the mean ratio is the mean of min(s, D) / D over that range, taken here
   exactly: one sum of integers and one quotient, rounded once. Each speed is held to +-2^20 so the sums stay
   well inside 64 bits, which no map's keys reach [INFERRED: the OTA values are integers of a few thousand],
   and the answer to 0..1. A divisor not above 0 means no wind. */
static float wind_mean(const char* ta)
{
    const int64_t LIM = 1 << 20;
    int64_t lo = *(const int32_t*)(ta + 0x1425B), hi = *(const int32_t*)(ta + 0x1425F);
    const int64_t d = *(const int32_t*)(ta + 0x37EC8);
    int64_t a, b, n, sum;
    double m;
    if (d <= 0) return 0;
    lo = lo < -LIM ? -LIM : lo > LIM ? LIM : lo;
    hi = hi < -LIM ? -LIM : hi > LIM ? LIM : hi;
    a = lo;
    b = hi - lo >= 2 ? hi - 1 : lo;
    n = b - a + 1;
    if (b <= d) sum = (a + b) * n / 2;
    else if (a >= d) sum = d * n;
    else sum = (a + d) * (d - a + 1) / 2 + d * (b - d);
    m = (double)sum / ((double)n * (double)d);
    return m <= 0 ? 0.0f : m >= 1 ? 1.0f : (float)m;
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
    out->wind_mean = wind_mean(c->ta);
    out->tide = *(const float*)(c->ta + 0x14267);
    if (!isfinite(out->tide)) out->tide = 0;
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
   (tagpu_patches.c, LOS_MAPPED). DISASSEMBLED: it reads the bit 1 << main[0x2A43], the viewed player's
   (0x4080E0..0x4080F4), whatever record it is passed -- and so does the resolver's mode 12 for any unit
   (0x43F5E2, 0x43F5F0; engine/orders.md, *The order resolver's reclaim case*). So it is asked for two
   seats only: the viewed one, whose own bit it is, and the controlled one, main[0x2A42], whose reclaim
   orders the UI commits through that resolver, which judges them by the same viewed bit -- for that seat
   0x408090 predicts the commit exactly even where the two players differ. Every other seat answers
   unmapped: a rule that asks for a seat's own map (ImpAI's) must not be handed another player's bit. */
static int wq_mapped_point(const WORLDQ* q, const int32_t pos[3])
{
    const WQ_ENGINE* c = ctx_of(q);
    if (c->seat != *(const unsigned char*)(c->ta + OFF_LOCALPLAYER) &&
        c->seat != *(const unsigned char*)(c->ta + OFF_WATCHED)) return 0;
    return ((Mapped)0x00408090u)(c->rec, (const int*)pos) != 0;
}

static int wq_ground(const WORLDQ* q, const int32_t pos[3])
{
    (void)q;
    return ((GroundHeight)0x00485070u)((const int*)pos);
}

static int wq_ndefs(const WORLDQ* q) { return (int)ctx_of(q)->ndefs; }

/* The def's movement class: +0x1B6 holds what 0x440420 answered for its `movementclass` key, the address of
   one of the 32 class records 0x512358 + 0x20 c, or 0 when it found none, and is 0 with no key (DISASSEMBLED
   0x42CD0C..0x42CD29, 0x440420..0x440463): the record is then the unit's own, on the parser's stack, and no
   pathing map is the type's. The value is data until it is one of the 32 by range and exact division. */
static unsigned move_class(const char* d)
{
    uintptr_t p = *(const uintptr_t*)(d + 0x1B6);
    if (p < MC_BASE || p >= MC_BASE + WQ_MCLASSES * MC_STRIDE || (p - MC_BASE) % MC_STRIDE) return WQ_MCLASS_NONE;
    return (unsigned)((p - MC_BASE) / MC_STRIDE);
}

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
    out->mclass = (uint8_t)move_class(d);
    return 1;
}

/* The economy keys the FBI parser 0x42BF40 stores in the def (engine/keys.md): the costs +0x186 energy and
   +0x18A metal, buildtime dword +0x1EA, energymake +0x1C2, energyuse +0x1C6, metalmake +0x1CA, extractsmetal
   +0x1CE, windgenerator +0x1D2, tidalgenerator +0x1D6, energystorage +0x1E2, metalstorage +0x1E6, makesmetal
   byte +0x22D, minwaterdepth word +0x1C0, and the move class +0x22F, 0 for a type that stands where it is
   built. Floats, each read as 0 when not finite. */
static int wq_def_econ(const WORLDQ* q, int type, WQ_DEF_ECON* out)
{
    const char* d = def_at(ctx_of(q), type);
    memset(out, 0, sizeof *out);
    if (!d) return 0;
    out->cost_e = finite_or_0(d + 0x186);
    out->cost_m = finite_or_0(d + 0x18A);
    out->buildtime = *(const uint32_t*)(d + 0x1EA);
    out->energymake = finite_or_0(d + 0x1C2);
    out->energyuse = finite_or_0(d + 0x1C6);
    out->metalmake = finite_or_0(d + 0x1CA);
    out->extracts = finite_or_0(d + 0x1CE);
    out->wind = finite_or_0(d + 0x1D2);
    out->tidal = finite_or_0(d + 0x1D6);
    out->estore = finite_or_0(d + 0x1E2);
    out->mstore = finite_or_0(d + 0x1E6);
    out->makesmetal = *(const uint8_t*)(d + 0x22D);
    out->building = d[0x22F] == 0;
    out->minwater = *(const int16_t*)(d + 0x1C0);
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
    return c->menu(c->seat, (unsigned)builder_type, out, cap);
}

/* THE SITE TEST, the instance's own (WQ_SITE_*), with the unit `ignore` taken as gone -- Quick Build's
   upgrade, where 0x47D554 exempts exactly that unit number from the occupancy checks.
   THE SCRATCH. DISASSEMBLED: 0x47D2E0 zeroes 0x51E684 and 0x51E688 (0x47D2EE, 0x47D2F4) and fills them with
   the site's altitude, which 0x47C780 reads back; the AI's 0x47C770 and the build cursor read them too. Both
   words are put back as they were after both calls, so a view taken in the mouse handler or the packet fill
   leaves the engine's own reading of them as it was.
   THE PLAYER. With a player 0x47D2E0 refuses a site whose centre the VIEWED player has not mapped
   (main[0x2A43], 0x47D3D1..0x47D3FA) whatever record it is passed -- but through WQ_SITE_SEAT's thunk, where
   the patch at 0x47D3D1 tests the passed record's own bit +0x146 -- and under LosType bit 1 reads the passed
   record's line-of-sight counters +0x7C (0x47D40C..0x47D42C) -- DISASSEMBLED; a unit or a yard occupant
   refuses only while that flag is set (0x47D53B, 0x47D55B). The record passed is the seat's. */
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
   all three. The hop is taken per axis: a tile lies right of and below its anchor in the same footprint
   (the anchor is the footprint's first cell, the offsets unsigned bytes back to it), so a tile whose column
   offset exceeds its own column, or whose row offset its own row, is no tile's -- taken as one subtraction
   it would land on another row's cell -- and is refused. The grid is bounded as the feature pass bounds it:
   the cells and defs present, W and H 1..4096, the def count 1..0xFFFA. The footprint is held to 1..16
   cells. */
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
        if (cl[FT_WIDX + 1] > cx || cl[FT_WIDX] > cz) return 0;
        cell -= cl[FT_WIDX] * c->w + cl[FT_WIDX + 1];
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

/* 0x438760 writes the byte of an order name, 0 for a name it does not find (0x43880F, 0x438819); looked up
   once a view, on first use: the descriptor array it searches is fixed for a level. The view's context is the
   caller's, on its stack for the call (worldq.h, NEVER STORED), so the cache is written through it. */
static int wq_action_of(WQ_ENGINE* c, int act)
{
    if (act < 0 || act >= WQ_ACTS) return -1;
    if (!c->got[act]) {
        c->act[act] = 0;
        ((Action)0x00438760u)(&c->act[act], (const char*)ACT_NAME[act]);
        c->got[act] = 1;
    }
    return c->act[act] ? c->act[act] : -1;
}

/* A factory's count node: BuildingBuild's own byte, whose node holds the type at +0x36 and the units still to
   make at +0x3A, the one in progress included (0x43A116..0x43A119 store them, 0x43B10C adds to the count, the
   handler 0x402640 decrements it at each completion, 0x402B34..0x402B39; engine/orders.md). 0 for any other
   node. */
static int32_t build_count(WQ_ENGINE* c, const char* n)
{
    const int a = wq_action_of(c, WQ_ACT_BUILDINGBUILD);
    return a > 0 && (unsigned char)n[4] == (unsigned)a ? *(const int32_t*)(n + N_COUNT) : 0;
}

/* The unit's list, walked on the game thread, the one thread that links, unlinks and frees order nodes:
   bounded by `cap` and WQ_LIST_MAX, it stops at a node another unit owns. A target pointer becomes a slot by
   exact division against the unit array. */
static int wq_orders(const WORLDQ* q, int slot, WQ_ORDER* out, int cap)
{
    WQ_ENGINE* c = ctx_of(q);
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
        o->count = build_count(c, n);
    }
    return k;
}

/* the list's head, `orders` with room for one: the same walk, bound and copy */
static int wq_head(const WORLDQ* q, int slot, WQ_ORDER* out)
{
    memset(out, 0, sizeof *out);
    return wq_orders(q, slot, out, 1);
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

static int wq_action(const WORLDQ* q, int act) { return wq_action_of(ctx_of(q), act); }

/* A FACTORY'S QUEUE. 0x43B0B0(action, unit, type, amount), stdcall `ret 0x10` (DISASSEMBLED 0x43B0B0..0x43B129),
   the root the build menu's own queue call makes (0x419B8C, after resolving `BUILDINGBUILD` for a unit with no
   mover, 0x419B6B..0x419B76): with amount > 0 it takes the list +0x5C for an action whose descriptor
   (*0x512344 + 25 action, +0x11) lacks bit 0x40000 -- BuildingBuild's lacks it (scout I5, the descriptor
   table) -- walks to its last node and adds the amount to that node's count +0x3A when its action +4 and type
   +0x36 are these, else appends a new count node through 0x43ADC0(action, 1, unit, 0, 0, type, amount): with
   Shift, so it never wipes the list. THE BOUNDS: the unit is re-derived as for every command (below); the type
   is below the def count and in the unit's own build menu as the view's menu answers it for the seat; the
   count is 1..WQ_QUEUE_MAX, never 0 or below, which would take the removal path 0x43B12C. A menu past
   QUEUE_MENU types refuses the command rather than be searched in part [INFERRED: a builder's pages offer a
   few dozen types; the most any retail one offers is far below it]. */
#define QUEUE_MENU 1024
static int queue(WQ_ENGINE* c, char* u, const WQ_CMD* cmd)
{
    uint16_t menu[QUEUE_MENU];
    int n, k, act;
    const unsigned builder = def_id(c, u);
    if (cmd->count < 1 || cmd->count > WQ_QUEUE_MAX || !def_at(c, cmd->type) || !builder || !c->menu ||
        (act = wq_action_of(c, WQ_ACT_BUILDINGBUILD)) < 0)
        return WQ_REFUSED;
    n = c->menu(c->seat, builder, menu, QUEUE_MENU);
    for (k = 0; k < n && menu[k] != cmd->type; k++) {}
    if (k >= n) return WQ_REFUSED;
    ((Queue)0x0043B0B0u)(act, u, cmd->type, cmd->count);
    return WQ_OK;
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
   node's +6 bit 1 (0x2, started; the test at 0x43A1FE) is set, starts the StopBuilding script (0x505104)
   when +0x42 bit 0x400000 is set, and for the head calls 0x489800(unit, 3), which resets the unit's three
   slots and queues a COB start of TargetCleared (0x508D58, through 0x4B0A70); the roots 0x43ADC0 and
   0x43AFC0 allocate and free nodes. */
static int wq_order(const WORLDQ* q, const WQ_CMD* cmd)
{
    WQ_ENGINE* c = ctx_of(q);
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
    if (cmd->verb == WQ_CMD_QUEUE) return queue(c, u, cmd);
    return WQ_REFUSED;
}

/* The seat's record, fixed at the open (its bound there): stored energy +0x8C and metal +0x98, the last pass's
   production +0x90 / +0x9C and requests +0x94 / +0xA0, the storage maxima +0xA4 / +0xA8, as the economy pass
   0x401360 writes them once a pass, every seat on the same ticks (DISASSEMBLED: engine/network.md, the economy
   pass 0x401360). Floats; one not finite reads 0. */
static int wq_economy(const WORLDQ* q, WQ_ECON* out)
{
    const char* r = ctx_of(q)->rec;
    out->energy = finite_or_0(r + 0x8C);
    out->energy_in = finite_or_0(r + 0x90);
    out->energy_out = finite_or_0(r + 0x94);
    out->metal = finite_or_0(r + 0x98);
    out->metal_in = finite_or_0(r + 0x9C);
    out->metal_out = finite_or_0(r + 0xA0);
    out->energy_max = finite_or_0(r + 0xA4);
    out->metal_max = finite_or_0(r + 0xA8);
    return WQ_OK;
}

static int wq_gametime(const WORLDQ* q) { return *(const int*)(ctx_of(q)->ta + OFF_GAMETIME); }

/* ---- what a seat sees: 0x465AC0's line of sight and 0x467440's radar pass, for any seat ---- */

#define U_ALIVE    0x10000000u
#define U_DYING    0x4000u
#define D_LOS_X    0x15E        /* the line-of-sight box: its first point's offsets from the unit */
#define D_LOS_Y    0x16E
#define D_LOS_Z    0x166
#define D_BOX_X    0x176        /* and the box's extents                                          */
#define D_BOX_Y    0x17A
#define D_BOX_Z    0x17E
#define D_RADAR    0x204
#define D_SONAR    0x206
#define D_JAM_R    0x20A
#define D_JAM_S    0x20C
#define PL_LOS     0x7C         /* u8[] the line-of-sight counters, +0x80 x +0x84 blocks          */
#define PL_LOS_W   0x80
#define PL_LOS_H   0x84
#define PL_ALLY    0x108        /* u8[11] byte j: this record allies record j                     */
#define PL_MAPBIT  0x146        /* the record's bit in the mapped grid, its player index          */
#define PI_SHARE   0x97         /* PlayerInfo: bit 0x40 ShareRadar                                */
#define PI_WATCH   0x9B         /* PlayerInfo: bit 0x40 a watcher                                 */

static int asr5(int v) { return v >> 5; }      /* `sar r,5`: GCC shifts a negative int arithmetically */
static int hiword(int32_t v) { return (int16_t)((uint32_t)v >> 16); }   /* `movsx r,word [v+2]`          */

/* The seat's sight: its record's line-of-sight counters and its bit of the mapped grid. The player init sizes
   both grids from the map, +0x80 = W16 / 2 and +0x84 = H16 / 2 (0x46482B..0x46484F), and allocates the counters
   that size; LoadMap allocates the mapped grid W16 x H16 / 4 u16. So the record's sizes are held to the map's
   halves -- the open's even W and H -- before either grid is indexed by them. No sight at all unless the
   simulation fixes' table is in (sight_block, THE LIVE RULE). */
typedef struct SIGHT {
    const uint8_t*  los;
    const uint16_t* mapped;
    unsigned        w, h, bit;
    int             truelos;    /* LosType bit 1 (main+0x14281): the counters; else the mapped grid  */
} SIGHT;

static int sight_of(const WQ_ENGINE* c, SIGHT* s)
{
    s->w = *(const unsigned*)(c->rec + PL_LOS_W);
    s->h = *(const unsigned*)(c->rec + PL_LOS_H);
    s->los = *(const uint8_t* const*)(c->rec + PL_LOS);
    s->mapped = *(const uint16_t* const*)(c->ta + 0x14273);
    s->bit = *(const unsigned char*)(c->rec + PL_MAPBIT);
    s->truelos = (*(const unsigned char*)(c->ta + OFF_LOSTYPE) & 2) != 0;
    if (s->w != (unsigned)c->w / 2 || s->h != (unsigned)c->h / 2 || !tagpu_limits_in()) return 0;
    return s->truelos ? s->los != NULL : s->mapped != NULL && s->bit < 16;
}

/* Where 0x465AC0 reads a point, and loop 5 of 0x467440 its unit's origin: col = x >> 5 and the row sheared by
   the altitude, (z - (y >> 1)) >> 5, on the 16.16 values' high words (`movsx` and `sar`), each under an
   unsigned bound on the record's sizes (0x465B6A..0x465B93 and the three copies; 0x408095..0x4080BF;
   0x46776B..0x467790, 0x4677B1..0x4677D6; DISASSEMBLED). THE LIVE RULE, not the retail one: tagpu_patches.c's
   fix_los_shear rewrites every one of these row tests (LOS_UNIT, LOS_MAPPED, LOS_VIEWMAP), so a point whose
   sheared row is off the grid is read at its own row, z >> 5, when that one is on it. THE RULE IS TIED TO THE
   FIX BY A QUERY: fix_los_shear's rows are in the fail-closed table (tagpu_limits.h), installed whole at attach
   or not at all, and sight_of gives no sight unless tagpu_limits_in says the table is in, so this copy reads
   the fixed rule only in a process whose engine runs it. A process whose table is not in ends at the first
   DirectDraw call, before any level. The self-check (tagpu_impai.c) holds this copy to the engine's answers. */
static int sight_block(const SIGHT* s, int32_t x, int32_t y, int32_t z, unsigned* k)
{
    const int col = asr5(hiword(x));
    int row = asr5(hiword(z) - (hiword(y) >> 1));
    if ((unsigned)col >= s->w) return 0;
    if ((unsigned)row >= s->h && (unsigned)(row = asr5(hiword(z))) >= s->h) return 0;
    *k = (unsigned)row * s->w + (unsigned)col;
    return 1;
}

/* The block's answer: under True line of sight the record's counters, as 0x465AC0 reads them for whatever
   record it is given. Otherwise the mapped grid with THE SEAT'S OWN BIT, rec+0x146: 0x408090, the inline
   copy at 0x465DE0 and loop 5 test the bit of the viewed player main[0x2A43] whatever record asks
   (0x4080E0..0x4080F4, 0x465E00, 0x467743; DISASSEMBLED), so the engine has no per-seat answer in these modes.
   The stamp 0x481930 writes every record's own bit (1 << rec+0x146, 0x481940..0x481951), so the seat's bit is
   the seat's map, and for the viewed seat this is the engine's own answer. */
static int sight_lit(const SIGHT* s, unsigned k)
{
    return s->truelos ? s->los[k] != 0 : (s->mapped[k] >> s->bit) & 1u;
}

/* THE BLOCK 0x47D2E0 READS THE MAPPED BIT AT, DISASSEMBLED 0x47D36C..0x47D3FA: the centre (2 cx + fx) << 19,
   (2 cz + fz) << 19 and the height byte of the first cell (0x485010, the feature grid's +4: 0 off the map) as the
   altitude; column x >> 5, row (z - altitude / 2) >> 5 on the high words, each below the passed record's grid
   +0x80 / +0x84 (0x47D3B0, 0x47D3B8: under the shear fix, a sheared row off the grid is read at the point's own
   row, the rule sight_block applies); the mapped grid main+0x14273 at row x +0x80 + column. This reads the
   seat's own bit there, rec+0x146, as the patch at 0x47D3D1 makes the thunk's call do. The footprint is held
   first to the test's own bounds (0x47D302..0x47D352). */
int tagpu_worldq_site_mapped(const WORLDQ* q, int type, int cx, int cz)
{
    const WQ_ENGINE* c = ctx_of(q);
    const char* d = def_at(c, type);
    const uint16_t* mapped = *(const uint16_t* const*)(c->ta + 0x14273);
    SIGHT s;
    unsigned k, bit = *(const unsigned char*)(c->rec + PL_MAPBIT);
    int fx, fz, alt;
    if (!d || !footprint(d, &fx, &fz) || cx < 1 || cz < 1 || cx + fx >= c->w || cz + fz >= c->h || !mapped ||
        bit >= 16)
        return -1;
    memset(&s, 0, sizeof s);
    s.w = *(const unsigned*)(c->rec + PL_LOS_W);
    s.h = *(const unsigned*)(c->rec + PL_LOS_H);
    if (s.w != (unsigned)c->w / 2 || s.h != (unsigned)c->h / 2) return -1;
    alt = (unsigned char)c->grid[((size_t)cz * c->w + cx) * FT_STRIDE + 4];
    if (!sight_block(&s, (int32_t)((uint32_t)(2 * cx + fx) << 19), (int32_t)((uint32_t)alt << 16),
                     (int32_t)((uint32_t)(2 * cz + fz) << 19), &k))
        return -1;
    return (mapped[k] >> bit) & 1u;
}

/* A unit of any owner at `slot` with the alive bit, dying or not -- the line of sight and the radar pass test
   bit 28 alone -- whose definition is one, or NULL. */
static const char* seen_unit(const WQ_ENGINE* c, unsigned slot, const char** def)
{
    const char* u;
    unsigned id;
    if (!slot || slot >= c->nslots) return NULL;
    u = c->units + slot * UNIT_STRIDE;
    if (!(*(const unsigned*)(u + U_FLAGS) & U_ALIVE) || !(id = def_id(c, u))) return NULL;
    *def = c->defs + id * D_STRIDE;
    return u;
}

/* The unit's top is below the sea: y + def+0x16E < sea << 16, 0x465AC0's own test (0x465B41..0x465B4E). */
static int submerged(const WQ_ENGINE* c, const char* u, const char* d)
{
    const int32_t top = (int32_t)((uint32_t)*(const int32_t*)(u + U_ZFIX) + (uint32_t)*(const int32_t*)(d + D_LOS_Y));
    return top < (int32_t)((unsigned)*(const unsigned char*)(c->ta + OFF_SEALEVEL) << 16);
}

/* 0x465AC0(player, unit), stdcall `ret 8`, writes nothing (DISASSEMBLED 0x465AC0..0x465E1E), for the seat:
     - its own unit (+0x96 is the record) is seen; the callers here test ownership first;
     - a unit with +0x10E bit 2, cloaked, is not;
     - a SUBMERGED unit -- its first point below the sea -- is seen by the engine only when +0x110 bit 9 is set,
       which is the VIEWED player's sonar contact (loop 2 of 0x467440): for any other seat that bit is another
       player's radar. So ImpAI never takes a submerged unit as seen; it reaches the seat as its own sonar
       contact alone (radar_step);
     - otherwise four points, the first that is seen deciding: P = (x + def+0x15E, y + def+0x16E,
       z + def+0x166) (0x465AFD..0x465B3B), then x + def+0x176 (0x465BDB..0x465BED), then
       y - def+0x17A and z + def+0x17E (0x465C73..0x465C90), then x - def+0x176 (0x465D19..0x465D31): 16.16
       dwords added as the engine adds them, wrapping. */
static int unit_los(const WQ_ENGINE* c, const SIGHT* s, const char* u, const char* d)
{
    uint32_t x, y, z;
    unsigned k;
    if (*(const unsigned char*)(u + U_CLOAKF) & 4u || submerged(c, u, d)) return 0;
    x = (uint32_t)*(const int32_t*)(u + U_XFIX) + (uint32_t)*(const int32_t*)(d + D_LOS_X);
    y = (uint32_t)*(const int32_t*)(u + U_ZFIX) + (uint32_t)*(const int32_t*)(d + D_LOS_Y);
    z = (uint32_t)*(const int32_t*)(u + U_YFIX) + (uint32_t)*(const int32_t*)(d + D_LOS_Z);
    if (sight_block(s, (int32_t)x, (int32_t)y, (int32_t)z, &k) && sight_lit(s, k)) return 1;
    x += (uint32_t)*(const int32_t*)(d + D_BOX_X);
    if (sight_block(s, (int32_t)x, (int32_t)y, (int32_t)z, &k) && sight_lit(s, k)) return 1;
    z += (uint32_t)*(const int32_t*)(d + D_BOX_Z);
    y -= (uint32_t)*(const int32_t*)(d + D_BOX_Y);
    if (sight_block(s, (int32_t)x, (int32_t)y, (int32_t)z, &k) && sight_lit(s, k)) return 1;
    x -= (uint32_t)*(const int32_t*)(d + D_BOX_X);
    return sight_block(s, (int32_t)x, (int32_t)y, (int32_t)z, &k) && sight_lit(s, k);
}

/* A record's PlayerInfo, 0xB9 bytes the record's constructor allocates (0x463C14) and the reset keeps
   (engine/units.md, the player record); the record is one of main's ten by its index. NULL for none. */
static const unsigned char* player_info(const WQ_ENGINE* c, unsigned player)
{
    return player < PLAYER_COUNT
        ? *(const unsigned char* const*)(c->ta + OFF_PLAYERS + player * PLAYER_STRIDE + PL_INFO) : NULL;
}

/* Loop 1's share test for the seat (0x4674BA..0x4674D9): the owner's record allies the seat's index rec+0x146
   and the owner shares radar, PlayerInfo +0x97 bit 0x40. The engine reaches the owner's record through the
   unit's +0x96; here through its owner byte, below 10; the alliance row has 11 bytes, so the seat's index is
   bounded by it. In a skirmish ShareRadar cannot be set (every share command needs the network bit), so
   this never holds there. */
static int shares_radar(const WQ_ENGINE* c, unsigned owner)
{
    const unsigned char* info = player_info(c, owner);
    const unsigned idx = *(const unsigned char*)(c->rec + PL_MAPBIT);
    return info && idx < 11 && c->ta[OFF_PLAYERS + owner * PLAYER_STRIDE + PL_ALLY + idx] &&
           (info[PI_SHARE] & 0x40u);
}

static void block_of(const WQ_ENGINE* c, const char* u, WQ_OBS* o)
{
    int bx = asr5(hiword(*(const int32_t*)(u + U_XFIX))), bz = asr5(hiword(*(const int32_t*)(u + U_YFIX)));
    o->bx = (uint16_t)(bx < 0 ? 0 : bx >= c->w / 2 ? c->w / 2 - 1 : bx);
    o->bz = (uint16_t)(bz < 0 ? 0 : bz >= c->h / 2 ? c->h / 2 - 1 : bz);
}

/* The layer: in the air when the type flies (def +0x241 bit 11) and the unit is airborne, +0x110 & 3 == 2 --
   the engine's aircraft test 0x40F7FC..0x40F81D (DISASSEMBLED); under water as 0x465AC0 tests it. */
static uint8_t layer_of(const WQ_ENGINE* c, const char* u, const char* d)
{
    if ((*(const unsigned*)(d + D_FLAGS) & 0x800u) && (*(const unsigned*)(u + U_FLAGS) & 3u) == 2u)
        return WQ_LAYER_AIR;
    return submerged(c, u, d) ? WQ_LAYER_SUBMERGED : WQ_LAYER_SURFACE;
}

static int wq_observe(const WORLDQ* q, int slot, int contact, WQ_OBS* out)
{
    const WQ_ENGINE* c = ctx_of(q);
    const char *u, *d;
    SIGHT s;
    unsigned owner;
    memset(out, 0, sizeof *out);
    if (slot <= 0 || !(u = seen_unit(c, (unsigned)slot, &d))) return 0;
    owner = *(const unsigned char*)(u + U_OWNER);
    if (owner >= PLAYER_COUNT) return 0;
    if (owner == (unsigned)c->seat) out->seen = WQ_SEEN_OWN;
    else if (sight_of(c, &s) && unit_los(c, &s, u, d)) out->seen = WQ_SEEN_LOS;
    else if (shares_radar(c, owner)) out->seen = WQ_SEEN_SHARED;
    else if (contact) out->seen = WQ_SEEN_CONTACT;
    else return 0;
    if (out->seen <= WQ_SEEN_LOS) {
        out->owner = (uint8_t)owner;
        out->type = (uint16_t)def_id(c, u);
    } else {
        out->owner = 0xFF;
        out->type = (uint16_t)WQ_NONE;
    }
    block_of(c, u, out);
    out->layer = layer_of(c, u, d);
    out->birth_known = (uint8_t)tagpu_unit_birth((unsigned)slot, &out->birth);
    return out->seen;
}

static int wq_block_lit(const WORLDQ* q, int bx, int bz)
{
    SIGHT s;
    if (!sight_of(ctx_of(q), &s) || bx < 0 || bz < 0 || (unsigned)bx >= s.w || (unsigned)bz >= s.h) return 0;
    return sight_lit(&s, (unsigned)bz * s.w + (unsigned)bx);
}

int tagpu_worldq_unit_los(const WORLDQ* q, int slot, int* under)
{
    const WQ_ENGINE* c = ctx_of(q);
    const char *u, *d;
    SIGHT s;
    if (slot <= 0 || !(u = seen_unit(c, (unsigned)slot, &d)) || !sight_of(c, &s)) return -1;
    *under = submerged(c, u, d);
    return unit_los(c, &s, u, d);
}

/* THE RADAR PASS, 0x467440 (no argument, plain `ret`; DISASSEMBLED 0x467440..0x467836), for the seat. The
   engine runs it for the viewed record alone, in that record's turn of the players phase (the call at
   0x46556D, reached only when bl == main[0x2A43], 0x465565..0x46556B); for any other seat this copy is the
   only answer. Its gate: the word main+0x2A3C at least 2 (0x46744D), else nothing runs. Its loops walk slot 1
   to the array's end slot inclusive (0x467474, back edges `jbe`); loop 2 walks the viewer's block. For the
   seat s, with idx = rec(s)+0x146:
     1. every live unit (bit 28): the seat's own (owner byte == s), one whose owner's record allies idx and
        shares radar, or every unit when the seat is a watcher (PlayerInfo +0x9B bit 0x40), gets bits 8 and 9
        and keeps its bit 10; any other loses 8, 9 and 10 (0x46749F..0x46750C). Bit 12 is the engine's and is
        not copied;
     2. each emitter of the seat's block -- alive, not dying (bit 14), activated (+0x10E bit 0), with a radar
        R = def+0x204 or a sonar S = def+0x206 -- walks its radius max(R, S) (signed words, 0x46757F) with the
        visitor 0x467840 (0x46750E..0x4675CC): a target with a type (+0xA6), of another owner than s, not
        stealthy (def +0x241 bit 8), gets bit 9 when y <= sea << 16 and d2 < S*S, and bit 8 when
        y + def+0x16E >= sea << 16 and d2 < (R + 2 y_r)^2, y_r the emitter's altitude in whole pixels
        (`movsx word [unit+0x70]`), the squares 32-bit `imul`s (0x467565..0x46757C, 0x4678E6..0x467940);
     3. each live, activated unit whose owner is not idx (allies included) with a jammer walks def+0x20A with
        0x467960 (bit 8 off, bit 10 on), then def+0x20C with 0x467980 (bit 9 off, bit 10 on), the visitors
        testing nothing: the seat's own units are jammed too (0x4675EC..0x46766C; the vtables 0x4FD558 and
        0x4FD554 read from .rdata);
     4. (0x467680..0x4676FD, the shield bit 12) is the same for every viewer and is not copied;
     5. every live unit without bit 8 and not cloaked (+0x10E bit 2) whose origin's block is lit gets bit 8
        (0x46770D..0x46782F): one point, (x, y, z) itself, placed and read as sight_block and sight_lit place
        and read -- the seat's own bit, not the viewed player's.
   THE WALK is 0x47E890(pos, r16, visitor)'s (stdcall `ret 0xC`, 0x47E890..0x47EA36): the 128-px sort buckets
   (x -+ r) >> 23 and (z -+ r) >> 23, each clamped into the grid, row by row, column by column, each chain from
   its head +6 through the units' +0x8E, a unit visited when d2 = (dx dx >> 32) + (dz dz >> 32), 64-bit
   products of the 16.16 differences, is at most (r r >> 32). It walks the engine's own chains rather than a
   rebuilt index: a unit's bucket can lag its position by one (engine/map.md, the sort bucket), and a carried
   unit is in none, so only the chains give the engine's answer. */
typedef struct RADAR_RUN {
    const WQ_ENGINE* c;
    const char*  buckets;
    unsigned     cols, rows;
    int32_t      sea;
    unsigned     seat, idx;
    uint8_t*     bits;
    WQ_RADAR_HIT* hits;
    int32_t*     budget;
} RADAR_RUN;

/* The slot's record at a bit the pass sets (WQ_RADAR_HIT): the unit's incarnation and block now, so the
   publish can tell this unit from one the slot holds by the time the pass completes. */
static void radar_hit(const RADAR_RUN* r, unsigned slot, const char* u)
{
    WQ_RADAR_HIT* h = &r->hits[slot];
    WQ_OBS o;
    block_of(r->c, u, &o);
    h->bx = o.bx;
    h->bz = o.bz;
    h->known = (uint8_t)tagpu_unit_birth(slot, &h->birth);
    if (!h->known) h->birth = 0;
}

enum { VISIT_RADAR, VISIT_JAM_RADAR, VISIT_JAM_SONAR };

/* The slot of a chain's unit pointer, 0 when it is not one: inside the array on a record boundary. */
static unsigned chain_slot(const WQ_ENGINE* c, const char* p)
{
    uintptr_t off;
    if ((uintptr_t)p < (uintptr_t)c->units) return 0;
    off = (uintptr_t)p - (uintptr_t)c->units;
    return off % UNIT_STRIDE || off / UNIT_STRIDE >= c->nslots ? 0 : (unsigned)(off / UNIT_STRIDE);
}

static int64_t sq32(int32_t d) { return ((int64_t)d * d) >> 32; }

static int bucket_clamp(int v, unsigned n) { return (unsigned)v < n ? v : v < 0 ? 0 : (int)n - 1; }

/* One walk: the emitter `e` with R, S and its altitude's square already formed (VISIT_RADAR), or a jammer.
   Every chain node read costs one visit off the budget; a chain is followed for at most nslots nodes, so a
   cycle the engine never makes still ends, and a walk reads each unit once (a unit is in one chain). */
static void radar_walk(const RADAR_RUN* r, const char* e, int32_t r16, int kind, int32_t sonar2, int32_t radar2)
{
    const WQ_ENGINE* c = r->c;
    const int32_t ex = *(const int32_t*)(e + U_XFIX), ez = *(const int32_t*)(e + U_YFIX);
    const int32_t reach = (int32_t)sq32(r16);
    const int x0 = bucket_clamp((int32_t)((uint32_t)ex - (uint32_t)r16) >> 23, r->cols);
    const int x1 = bucket_clamp((int32_t)((uint32_t)ex + (uint32_t)r16) >> 23, r->cols);
    const int z0 = bucket_clamp((int32_t)((uint32_t)ez - (uint32_t)r16) >> 23, r->rows);
    const int z1 = bucket_clamp((int32_t)((uint32_t)ez + (uint32_t)r16) >> 23, r->rows);
    int bx, bz;
    for (bz = z0; bz <= z1; bz++) for (bx = x0; bx <= x1; bx++) {
        const char* t = *(const char* const*)(r->buckets + ((size_t)bz * r->cols + (unsigned)bx) * 10u + 6u);
        unsigned n;
        for (n = 0; t && n < c->nslots; n++, t = *(const char* const*)(t + U_CARGONEXT)) {
            const unsigned slot = chain_slot(c, t);
            int32_t d2;
            if (!slot) break;
            (*r->budget)--;
            d2 = (int32_t)(sq32((int32_t)((uint32_t)*(const int32_t*)(t + U_XFIX) - (uint32_t)ex)) +
                           sq32((int32_t)((uint32_t)*(const int32_t*)(t + U_YFIX) - (uint32_t)ez)));
            if (d2 > reach) continue;
            if (kind == VISIT_RADAR) {
                const unsigned id = def_id(c, t);
                const char* d = c->defs + id * D_STRIDE;
                const int32_t y = *(const int32_t*)(t + U_ZFIX);
                unsigned add = 0;
                if (!*(const unsigned short*)(t + U_MODELID) || *(const unsigned char*)(t + U_OWNER) == r->seat ||
                    !id || (*(const unsigned*)(d + D_FLAGS) & 0x100u))
                    continue;
                if (y <= r->sea && d2 < sonar2) add |= WQ_RB_SONAR;
                if ((int32_t)((uint32_t)y + (uint32_t)*(const int32_t*)(d + D_LOS_Y)) >= r->sea && d2 < radar2)
                    add |= WQ_RB_RADAR;
                if (add) {
                    wq_radar_set(r->bits, slot, wq_radar_bits(r->bits, slot) | add);
                    radar_hit(r, slot, t);
                }
            } else {
                const unsigned v = wq_radar_bits(r->bits, slot);
                wq_radar_set(r->bits, slot, (v & ~(kind == VISIT_JAM_RADAR ? WQ_RB_RADAR : WQ_RB_SONAR)) | WQ_RB_JAMMED);
            }
        }
    }
}

/* the live unit at a slot for the pass's loops, and its definition: NULL when the slot holds none */
static const char* pass_unit(const WQ_ENGINE* c, unsigned slot, const char** d)
{
    return seen_unit(c, slot, d);
}

static void radar_begin(const RADAR_RUN* r, WQ_RADAR* st)
{
    const WQ_ENGINE* c = r->c;
    const unsigned char* mine = player_info(c, (unsigned)c->seat);
    const int watcher = *(const uint32_t*)c->rec && mine && (mine[PI_WATCH] & 0x40u);
    unsigned slot;
    for (slot = 1; slot < c->nslots; slot++) {
        const char *u, *d;
        unsigned owner;
        if (!(u = pass_unit(c, slot, &d))) continue;
        owner = *(const unsigned char*)(u + U_OWNER);
        if (owner == r->seat || (owner < PLAYER_COUNT && shares_radar(c, owner)) || watcher) {
            wq_radar_set(r->bits, slot, WQ_RB_RADAR | WQ_RB_SONAR | WQ_RB_KEPT);
            radar_hit(r, slot, u);
        }
    }
    st->stage = WQ_RS_EMIT;
    st->sub = 0;
    st->cursor = (uint16_t)c->first;
    st->start = (uint32_t)*(const int*)(c->ta + OFF_GAMETIME);
}

static void radar_lit(const RADAR_RUN* r)
{
    const WQ_ENGINE* c = r->c;
    SIGHT s;
    unsigned slot, k;
    if (!sight_of(c, &s)) return;
    for (slot = 1; slot < c->nslots; slot++) {
        const char *u, *d;
        const unsigned v = wq_radar_bits(r->bits, slot);
        if (!(u = pass_unit(c, slot, &d)) || (v & WQ_RB_RADAR) || (*(const unsigned char*)(u + U_CLOAKF) & 4u))
            continue;
        if (sight_block(&s, *(const int32_t*)(u + U_XFIX), *(const int32_t*)(u + U_ZFIX), *(const int32_t*)(u + U_YFIX), &k)
            && sight_lit(&s, k)) {
            wq_radar_set(r->bits, slot, v | WQ_RB_RADAR);
            radar_hit(r, slot, u);
        }
    }
}

/* THE BUCKETS' BOUNDS: LoadMap sizes the grid main+0x142A3 columns x +0x142A7 rows and allocates
   main+0x142AB buckets of 10 bytes, (cols x rows + 7) & ~7 (0x482C84..0x482CC5; engine/recorder.md), so a
   grid of cols x rows within that count is inside the allocation. */
static int wq_radar_step(const WORLDQ* q, WQ_RADAR* st, uint8_t* bits, int nbytes, WQ_RADAR_HIT* hits, int nhits,
                         int32_t* budget)
{
    const WQ_ENGINE* c = ctx_of(q);
    RADAR_RUN r;
    unsigned slot;
    if (nbytes < (int)WQ_RADAR_BYTES(c->nslots) || nhits < 0 || (unsigned)nhits < c->nslots) return 0;
    r.c = c;
    r.buckets = *(const char* const*)(c->ta + 0x1429F);
    r.cols = *(const unsigned*)(c->ta + 0x142A3);
    r.rows = *(const unsigned*)(c->ta + 0x142A7);
    r.sea = (int32_t)((unsigned)*(const unsigned char*)(c->ta + OFF_SEALEVEL) << 16);
    r.seat = (unsigned)c->seat;
    r.idx = *(const unsigned char*)(c->rec + PL_MAPBIT);
    r.bits = bits;
    r.hits = hits;
    r.budget = budget;
    if (st->stage == WQ_RS_IDLE) {
        memset(bits, 0, WQ_RADAR_BYTES(c->nslots));
        if (*(const unsigned short*)(c->ta + 0x2A3C) < 2 || !r.buckets || !r.cols || !r.rows ||
            r.cols > 0xFFFFu || r.rows > 0xFFFFu || r.cols * r.rows > *(const unsigned*)(c->ta + 0x142AB))
            return 1;
        radar_begin(&r, st);
    } else if (!r.buckets || !r.cols || !r.rows || r.cols > 0xFFFFu || r.rows > 0xFFFFu ||
               r.cols * r.rows > *(const unsigned*)(c->ta + 0x142AB)) {
        st->stage = WQ_RS_IDLE;
        st->sub = 0;
        return 1;
    }
    if (st->stage == WQ_RS_EMIT) {
        for (slot = st->cursor; slot <= c->last && *budget > 0; slot++) {
            const char* e = c->units + slot * UNIT_STRIDE;
            const unsigned flags = *(const unsigned*)(e + U_FLAGS), id = def_id(c, e);
            const char* d = c->defs + id * D_STRIDE;
            int16_t R, S;
            int32_t yr;
            if (!(flags & U_ALIVE) || (flags & U_DYING) || !(*(const unsigned char*)(e + U_CLOAKF) & 1u) || !id)
                continue;
            R = *(const int16_t*)(d + D_RADAR);
            S = *(const int16_t*)(d + D_SONAR);
            if (!R && !S) continue;
            yr = hiword(*(const int32_t*)(e + U_ZFIX));
            radar_walk(&r, e, (int32_t)((uint32_t)(R > S ? R : S) << 16), VISIT_RADAR,
                       (int32_t)((uint32_t)S * (uint32_t)S),
                       (int32_t)((uint32_t)(R + 2 * yr) * (uint32_t)(R + 2 * yr)));
        }
        if (slot <= c->last) { st->cursor = (uint16_t)slot; return 0; }
        st->stage = WQ_RS_JAM;
        st->sub = 0;
        st->cursor = 1;
    }
    /* Loop 3. A jammer's two walks each start only with budget left, so a call ends at most one walk below 0:
       when its radar walk spends the budget and a sonar walk follows, the call ends with `sub` set and the
       next resumes at the same slot with the sonar walk. Every other step to the next slot clears `sub`. */
    for (slot = st->cursor; slot < c->nslots && *budget > 0; slot++, st->sub = 0) {
        const char* u = c->units + slot * UNIT_STRIDE;
        const unsigned id = def_id(c, u);
        const char* d = c->defs + id * D_STRIDE;
        int16_t J;
        if (!(*(const unsigned*)(u + U_FLAGS) & U_ALIVE) || *(const unsigned char*)(u + U_OWNER) == r.idx ||
            !(*(const unsigned char*)(u + U_CLOAKF) & 1u) || !id)
            continue;
        if (!st->sub && (J = *(const int16_t*)(d + D_JAM_R)) != 0)
            radar_walk(&r, u, (int32_t)((uint32_t)(int32_t)J << 16), VISIT_JAM_RADAR, 0, 0);
        if ((J = *(const int16_t*)(d + D_JAM_S)) == 0) continue;
        if (*budget <= 0) { st->sub = 1; break; }
        radar_walk(&r, u, (int32_t)((uint32_t)(int32_t)J << 16), VISIT_JAM_SONAR, 0, 0);
    }
    if (slot < c->nslots) { st->cursor = (uint16_t)slot; return 0; }
    radar_lit(&r);
    st->stage = WQ_RS_IDLE;
    st->sub = 0;
    st->cursor = 0;
    return 1;
}

/* ---- the pathing maps ---- */

/* THE MOVEMENT CLASSES, 32 records of 0x20 bytes at 0x512358 (engine/map.md, the pathing maps): +0x00 the
   class name, nonzero for a class that exists; +0x10 and +0x14 its map's W and H; +0x18 the map. The level
   loader's 0x440940 (called at 0x4918E3, before PLAYERS at 0x4919C8 in the same body) gives every existing
   class W = main+0x14233 and H = main+0x14237 and allocates ((H + 15) >> 4) x W dwords (0x44097E..0x4409B8,
   DISASSEMBLED), and the reader 0x40D7B0 takes cell (x, z) as dword (z >> 4) x W + x, bits 2 (z & 15)
   (0x40D83E..0x40D85E): 0 blocked, 1 passable but bad, 3 good, with the class's footprint and a one-cell ring
   already folded in at the cell its footprint starts at (0x440500). So a class whose W and H are the map's
   holds every cell of the map, and nothing else is read. The maps live from that load to the next, built on
   the loader thread and refreshed in play on the game thread (0x47E1F0), the two threads this is read on. */
int tagpu_worldq_passable_at(unsigned mclass, int cx, int cz)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const char* mc;
    const uint32_t* map;
    int w, h;
    if (!ta || mclass >= WQ_MCLASSES) return 0;
    w = *(const int*)(ta + OFF_MAP_W16);
    h = *(const int*)(ta + OFF_MAP_H16);
    mc = (const char*)(uintptr_t)(MC_BASE + mclass * MC_STRIDE);
    if (!*(const char* const*)mc || w <= 0 || h <= 0 || w > 0x7FFF || h > 0x7FFF ||
        *(const unsigned*)(mc + 0x10) != (unsigned)w || *(const unsigned*)(mc + 0x14) != (unsigned)h ||
        !(map = *(const uint32_t* const*)(mc + 0x18)) || cx < 0 || cz < 0 || cx >= w || cz >= h)
        return 0;
    return (int)((map[(size_t)(cz >> 4) * (unsigned)w + (unsigned)cx] >> (2u * ((unsigned)cz & 15u))) & 3u);
}

int tagpu_worldq_class_exists(unsigned mclass)
{
    return mclass < WQ_MCLASSES && *(const char* const*)(uintptr_t)(MC_BASE + mclass * MC_STRIDE) != NULL;
}

static int wq_passable(const WORLDQ* q, int mclass, int cx, int cz)
{
    (void)q;
    return mclass < 0 ? 0 : tagpu_worldq_passable_at((unsigned)mclass, cx, cz);
}

/* ---- the role classifier's input (tagpu_worldq.h, THE ROLE TABLE'S ENGINE FILLER) ----

   What it reads, each the level load's, on the loader thread inside PLAYERS 0x4919C8: the def array
   main+0x1439B below UNITINFOCount main+0x1438F, the weapon records the defs point at, the weapons' [DAMAGE]
   rows and the category map. THE LIFETIME: the unit-data load 0x42D2E0 (0x4918CA) wrote all of them on this
   thread before this point -- the weapons through the parser 0x42E440, the defs and their bad-target masks
   through the FBI reader 0x42BF40, the categories through 0x488E70 -- and only the teardown 0x491B60 frees
   them, on the game thread, before it creates the next load's loader thread (0x4982CA). THE CATEGORY MAP is
   written during a load only on this thread (the FBI reader, and the AI plan's `weight` and `limit` inside
   the players init 0x464990, both before the level init), and in play only by game-thread code the in-game
   frame reaches (the key switch 0x495E90, called at 0x496971 in 0x496790; the SHARE give 0x493673; the TALK
   line 0x493E36; engine/units.md, Who inserts), none of which runs while the game thread waits on a load
   [INFERRED for the two GUI handlers, as there]. The walk keeps no entry pointer past this call and never
   calls the lookup 0x488C50, which inserts on a miss. THE BOUNDS: the def count 1..TAGPU_LIM_TYPES; a
   weapon pointer a record of the weapon array (tagpu_limits_weapon_index above 0); a damage table and the
   category map a whole number of 8-byte entries, at most DMG_MAX and CATMAP_MAX of them; every name compared
   within its own cap; a mask bit only for a row below the count, which every mask holds (TAGPU_LIM_TYPES
   bits under the raised limits, tagpu_patches.c). ptr_ok is a filter on values, not the argument. */
#define VA_CATMAP_BEGIN 0x0051E6B4u    /* {char* name, u32* mask}: engine/units.md, The unit-category map */
#define VA_CATMAP_END   0x0051E6B8u
#define CATMAP_MAX      65536u
#define CAT_NAME_MAX    64              /* a category name's bytes compared                              */
#define D_NAME          0x20            /* char[32] unitname                                             */
#define D_BADTGT        0x231           /* u32* x3: wpri_/wsec_/wspe_badTargetCategory's masks            */
#define W_DMG           0x64            /* the [DAMAGE] rows: {+0x05 begin, +0x09 end} of {char*, i32}
                                           (0x499CD0: 0x499CEA, 0x499CFF, 0x499D02, 0x499D6D)               */
#define DMG_MAX         65536u
/* the bits the classifier tests, which are all the file filler sets (tools/tasim/tasim_run.c) */
#define ROLE_F241 (TASIM_F241_BUILDER | TASIM_F241_CANFLY | TASIM_F241_CANHOVER | TASIM_F241_AMPHIBIOUS | \
                   TASIM_F241_KAMIKAZE | TASIM_F241_ANTIWEAPONS)
#define ROLE_F245 (TASIM_F245_CANMOVE | TASIM_F245_COMMANDER)
#define ROLE_WF   (TASIM_WF_BALLISTIC | TASIM_WF_VLAUNCH | TASIM_WF_PARALYZER | TASIM_WF_DROPPED | \
                   TASIM_WF_WATER | TASIM_WF_TOAIR | TASIM_WF_COMMANDFIRE | TASIM_WF_STOCKPILE | \
                   TASIM_WF_INTERCEPTOR)

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p <= tagpu_user_top(); }

static int fold(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

/* `a` equals `b` as _stricmp 0x4F8A70 compares them (an ASCII A-Z fold), each read to its NUL or `cap` */
static int name_eq(const char* a, const char* b, int cap)
{
    int k;
    for (k = 0; k < cap; k++) {
        const int x = fold((unsigned char)a[k]), y = fold((unsigned char)b[k]);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 0;
}

/* `word` among the blank-separated words of `text`, read within `cap` bytes: the file filler's has_word */
static int has_word(const char* text, const char* word, int cap)
{
    int k = 0;
    while (k < cap && text[k]) {
        int n = 0, j;
        while (k < cap && (text[k] == ' ' || text[k] == '\t')) k++;
        while (k + n < cap && text[k + n] && text[k + n] != ' ' && text[k + n] != '\t') n++;
        for (j = 0; j < n && word[j] && fold((unsigned char)text[k + j]) == fold((unsigned char)word[j]); j++) {}
        if (n && j == n && !word[j]) return 1;
        k += n;
    }
    return 0;
}

typedef struct CATMAP { const char* begin; unsigned n; } CATMAP;

static int catmap_open(CATMAP* m)
{
    const char* begin = *(const char* const*)(uintptr_t)VA_CATMAP_BEGIN;
    const char* end = *(const char* const*)(uintptr_t)VA_CATMAP_END;
    m->begin = begin;
    m->n = 0;
    if (begin == end) return 1;
    if (!ptr_ok(begin) || !ptr_ok(end) || end < begin || (size_t)(end - begin) % 8 ||
        (size_t)(end - begin) / 8 > CATMAP_MAX)
        return 0;
    m->n = (unsigned)((size_t)(end - begin) / 8);
    return 1;
}

/* the mask of the category named `name`, or NULL */
static const uint32_t* catmap_mask(const CATMAP* m, const char* name)
{
    unsigned k;
    for (k = 0; k < m->n; k++) {
        const char* n = *(const char* const*)(m->begin + (size_t)k * 8);
        const uint32_t* mask = *(const uint32_t* const*)(m->begin + (size_t)k * 8 + 4);
        if (ptr_ok(n) && ptr_ok(mask) && name_eq(n, name, CAT_NAME_MAX)) return mask;
    }
    return NULL;
}

/* the name of the category whose mask is `mask`, or NULL: the FBI reader stores a bad-target tag as the
   mask the map holds under the tag's whole text (0x42C028..0x42C0A6) */
static const char* catmap_name(const CATMAP* m, const void* mask)
{
    unsigned k;
    if (!mask) return NULL;
    for (k = 0; k < m->n; k++) {
        const char* n = *(const char* const*)(m->begin + (size_t)k * 8);
        if (*(const void* const*)(m->begin + (size_t)k * 8 + 4) == mask && ptr_ok(n)) return n;
    }
    return NULL;
}

/* weapon `w`'s damage against the type named `tname`: its [DAMAGE] row for the type, else its default, as
   0x499CD0 takes them (the row's dword, the default's word zero-extended) */
static int32_t damage_vs(const char* w, const char* tname, int32_t dflt)
{
    const char* t = *(const char* const*)(w + W_DMG);
    const char *b, *e;
    size_t k, n;
    if (!t || !ptr_ok(t)) return dflt;
    b = *(const char* const*)(t + 5);
    e = *(const char* const*)(t + 9);
    if (b == e || !ptr_ok(b) || !ptr_ok(e) || e < b || (size_t)(e - b) % 8 || (size_t)(e - b) / 8 > DMG_MAX)
        return dflt;
    n = (size_t)(e - b) / 8;
    for (k = 0; k < n; k++) {
        const char* name = *(const char* const*)(b + k * 8);
        if (ptr_ok(name) && name_eq(name, tname, 33)) return *(const int32_t*)(b + k * 8 + 4);
    }
    return dflt;
}

int tagpu_worldq_role_count(void)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    unsigned n;
    if (!ta || !*(const char* const*)(ta + D_BASE)) return 0;
    n = *(const unsigned*)(ta + OFF_UDEFCOUNT);
    return n && n <= TAGPU_LIM_TYPES ? (int)n : 0;
}

/* THE FILLER: the file filler's keys from where the engine keeps them (engine/keys.md), every value
   already the parsers' own conversion, so a plain copy. Weapons: slots 0 .. min(the type's count,
   TASIM_ROLE_WEAPONS) - 1 through the extra-weapons module, weapon1..weapon10 as the file filler reads them;
   a slot whose record is the no-weapon entry 0 stays empty, as the file filler leaves a name it finds no
   section for. A weapon's air damage is the mean, over the types of category VTOL, of its damage against
   each. */
int tagpu_worldq_role_in(TASIM_ROLE_IN* in, int n)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const char* defs;
    const uint32_t* vtol;
    uint16_t* air;
    float* wair;                /* a weapon's air damage by its index, taken once: NAN until then */
    CATMAP cat;
    const int nw = tagpu_limits_weapon_slots();
    int i, k, nair = 0;
    if (n <= 0 || n != tagpu_worldq_role_count() || nw <= 0 || !catmap_open(&cat)) return -1;
    defs = *(const char* const*)(ta + D_BASE);
    air = (uint16_t*)tagpu_sim_scratch((size_t)n * sizeof *air);
    wair = (float*)tagpu_sim_scratch((size_t)nw * sizeof *wair);
    if (!air || !wair) {
        tagpu_sim_scratch_free(wair);
        tagpu_sim_scratch_free(air);
        return -1;
    }
    for (i = 0; i < nw; i++) wair[i] = NAN;
    if ((vtol = catmap_mask(&cat, "VTOL")) != NULL)
        for (i = 0; i < n; i++)
            if (vtol[(unsigned)i >> 5] >> ((unsigned)i & 31u) & 1u) air[nair++] = (uint16_t)i;
    for (i = 0; i < n; i++) {
        const char* d = defs + (size_t)i * D_STRIDE;
        TASIM_ROLE_IN* r = &in[i];
        memset(r, 0, sizeof *r);
        memcpy(r->name, d + D_NAME, sizeof r->name - 1);
        r->flags241 = *(const uint32_t*)(d + D_FLAGS) & ROLE_F241;
        r->flags245 = *(const uint32_t*)(d + D_TAGS) & ROLE_F245;
        r->bmcode = *(const uint8_t*)(d + 0x22F);
        r->cost_energy = *(const float*)(d + 0x186);
        r->cost_metal = *(const float*)(d + 0x18A);
        r->maxdamage = *(const uint32_t*)(d + 0x1FA);
        r->workertime = *(const uint16_t*)(d + 0x1FE);
        r->maxvelocity_fx = *(const int32_t*)(d + 0x192);
        r->sight = *(const uint16_t*)(d + 0x202);
        r->radar = *(const uint16_t*)(d + 0x204);
        r->jammer = *(const uint16_t*)(d + 0x20A);
        r->minwaterdepth = *(const int16_t*)(d + 0x1C0);
        r->maxwaterdepth = *(const int16_t*)(d + 0x1BE);
        r->energyuse = *(const float*)(d + 0x1C6);
        r->makesmetal = *(const uint8_t*)(d + 0x22D);
        r->transportcapacity = *(const uint8_t*)(d + 0x22B);
        for (k = 0; k < TASIM_ROLE_BADSLOTS; k++) {
            const char* tag = catmap_name(&cat, *(const void* const*)(d + D_BADTGT + 4 * k));
            r->slot_notair[k] = (uint8_t)(tag && has_word(tag, "NOTAIR", CAT_NAME_MAX));
        }
        for (k = 0; k < TASIM_ROLE_WEAPONS; k++) {
            const char* w = tagpu_weapons_type_weapon(d, (unsigned)k);
            TASIM_ROLE_WEAPON* o = &r->w[k];
            int wi, j;
            if (!w) break;
            if ((wi = tagpu_limits_weapon_index(w)) <= 0 || wi >= nw) continue;
            o->range = *(const int32_t*)(w + 0xDC);
            o->reload_ticks = (float)*(const uint16_t*)(w + 0xE4);
            o->burst = *(const uint16_t*)(w + 0xEA);
            o->damage_default = *(const uint16_t*)(w + 0xD4);
            if (isnan(wair[wi])) {
                double sum = 0.0;
                for (j = 0; j < nair; j++)
                    sum += (double)damage_vs(w, defs + (size_t)air[j] * D_STRIDE + D_NAME, o->damage_default);
                wair[wi] = nair ? (float)(sum / nair) : 0.0f;
            }
            o->damage_air = wair[wi];
            o->energy_per_shot = *(const float*)(w + 0xC0);
            o->metal_per_shot = *(const float*)(w + 0xC4);
            o->flags = (*(const uint32_t*)(w + 0x111) & ROLE_WF) |
                       ((tagpu_datakeys_wkey(w) & TAGPU_WK_NOTTOAIR) ? TASIM_WF_NOTTOAIR : 0u);
            r->nweapons = (uint16_t)(k + 1);
        }
    }
    tagpu_sim_scratch_free(wair);
    tagpu_sim_scratch_free(air);
    return 0;
}

static const WORLDQ_OPS OPS = {
    wq_map, wq_explored, wq_metal, wq_coverage, wq_mapped_point, wq_ground, wq_ndefs, wq_def, wq_def_econ, wq_yard,
    wq_build_options, wq_site_ok, wq_blocked_alt, wq_feature, wq_nslots, wq_seat_units, wq_unit, wq_orders, wq_head,
    wq_can, wq_action, wq_order, wq_economy, wq_gametime, wq_observe, wq_block_lit, wq_radar_step, wq_passable,
};

/* THE OPEN'S BOUNDS, each read value made data before it is an index: the map's W and H even and 2..0x7FFF,
   and W x H x 13 below 2^31 -- LoadMap's row product is the grid's allocation, so it is bounded before any
   index or byte product; the def count 1..TAGPU_LIM_TYPES; the unit array's span a whole number of 0x118
   records below TAGPU_PK_DESIGN_SLOTS and the slot count main+0x14351 one more than it; the seat's block
   (record +0x67 .. +0x6B) inside the array on record boundaries. The input path cannot run while the loader
   owns any of it. */
/* THE CONTENT VIEW. What its ops read at the level init: the map's size and sea level, the defs, and the
   wind and tide, which the loader body 0x4917D0 sets before PLAYERS 0x4919C8 (DISASSEMBLED: it writes the
   wind's divisor main+0x37EC8 at 0x4918ED and calls the wind step 0x490C40, which reads windmin and windmax,
   at 0x491903). Record 0 stands for the seat, so the economy and explored reads stay inside the main block;
   the slot count 0 refuses every unit (live_unit) and every site (wq_site_ok's ignore bound). */
int tagpu_worldq_open_content(WQ_ENGINE* ctx, WORLDQ* out)
{
    char* state;
    memset(ctx, 0, sizeof *ctx);
    memset(out, 0, sizeof *out);
    if (!(ctx->ta = *(char**)TA_MAIN_PP)) return 0;
    ctx->w = *(int*)(ctx->ta + OFF_MAP_W16); ctx->h = *(int*)(ctx->ta + OFF_MAP_H16);
    if (ctx->w < 2 || ctx->h < 2 || (ctx->w & 1) || (ctx->h & 1) || ctx->w > 0x7FFF || ctx->h > 0x7FFF ||
        (unsigned)ctx->w > 0x7FFFFFFFu / FT_STRIDE / (unsigned)ctx->h) return 0;
    ctx->ndefs = *(unsigned*)(ctx->ta + OFF_UDEFCOUNT);
    if (!ctx->ndefs || ctx->ndefs > TAGPU_LIM_TYPES) return 0;
    ctx->defs = *(char**)(ctx->ta + D_BASE);
    ctx->grid = *(char**)(ctx->ta + OFF_FEATMAP);
    state = *(char**)(ctx->ta + 0x391E9);
    if (!ctx->defs || !ctx->grid || !state) return 0;
    ctx->threshold = *(unsigned*)(state + 0xD30) > 255u ? 255 : (int)*(unsigned*)(state + 0xD30);
    ctx->rec = ctx->ta + OFF_PLAYERS;
    ctx->first = 1;
    ctx->site = (WqSiteFn)0x0047D2E0u;
    out->ops = &OPS;
    out->self = ctx;
    return 1;
}

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
    ctx->site = site == WQ_SITE_CURSOR ? s_cursor_site : site == WQ_SITE_SEAT ? s_seat_site : (WqSiteFn)0x0047D2E0u;
    if (!ctx->site) return 0;
    ctx->menu = menu;
    out->ops = &OPS;
    out->self = ctx;
    out->seat = seat;
    return 1;
}
