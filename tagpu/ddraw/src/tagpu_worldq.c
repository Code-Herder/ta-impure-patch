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
#include "impai_table.h"
#include "tasim_class.h"
#include "tasim_cobfire.h"
#include "tagpu_patches.h"
#include "tagpu_allysight.h"
#include "tagpu_seatcontact.h"

SIM_FEATURE(worldq, SIM_PUPPET_OWNER, "resource-mex resource-upgrade resource-queued-upgrade resource-mixed resource-plain resource-uniform resource-locked resource-inherit resource-classic resource-geo-upgrade resource-nearby resource-priority resource-refusals resource-frontend place-basic stock-place-basic sweep-basic stock-sweep-basic");

#define D_STRIDE  0x249u
#define D_BASE    0x1439B
#define D_FLAGS   0x241
#define D_TAGS    0x245
#define U_FLAGS   0x110
#define U_ORDERS  0x5C
#define U_MADE_LAST_M 0xE4          /* float: the metal produced in the last pass's period (wq_unit)      */
#define U_REQ_LAST_M  0xE8          /* float: the metal requested in the last pass's period (wq_unit)     */
#define U_REQ_LAST_E  0xD0          /* float: the energy requested in the last pass's period (wq_unit)    */
#define U_ACTIVE      0x10E         /* byte: the activation bits, bit 0 on (wq_unit)                       */
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
   cursor's (0x419670), the resolver's (0x440187, 0x43F694, 0x44004C), the build menu's queue call for a unit
   with no mover, `BUILDINGBUILD` (0x419B76), GetBuilt's for a factory's finished unit, `PARK` (0x402EF0), and the
   on and off button's, `ACTIVATE` and `DEACTIVATE` (0x41A804, 0x41A83D; engine/orders.md, *The on and off*). */
static const uintptr_t ACT_NAME[WQ_ACTS] = { 0x00502654u, 0x00502640u, 0x005016C4u, 0x00501B68u, 0x005052F0u,
                                             0x00502684u, 0x00501358u, 0x00502768u, 0x0050275Cu };

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
/* the descriptor array's entries, 0 when its two ends are not a whole array */
static unsigned action_count(void)
{
    const char* first = *(const char* const*)0x00512344u;
    const char* last = *(const char* const*)0x00512348u;
    uintptr_t bytes;
    if (!first || (uintptr_t)last < (uintptr_t)first) return 0;
    bytes = (uintptr_t)last - (uintptr_t)first;
    return bytes % 0x19u ? 0 : (unsigned)(bytes / 0x19u);
}

static int build_node(const WQ_ENGINE* c, const char* n)
{
    const char* first = *(const char* const*)0x00512344u;
    unsigned action = (unsigned char)n[4], type = *(unsigned short*)(n + N_TYPE);
    return action < action_count() && (*(unsigned*)(first + action * 0x19u + 0x0C) & 1u) &&
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
    if (tags & 0x4u) out->flags |= WQ_DF_ONOFF;
    if (d[0x22F] == 1) out->flags |= WQ_DF_MOVER;
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
   cells. `playing` reads the anchor's flags, the cell FeatureDie marks (engine/map.md): MEASURED 2026-11-13 in
   ImpAI's recordings, a rock's anchor read 0x50 until its reclaim paid, 0x51 for the 21 ticks its sequence
   played with its def unchanged, then 0x50 again under its successor's def. THE DRAW'S INPUTS, each a field of
   the anchor already bounded above: `gated` the def's +0xFF bit 3 (nodrawundergray), `owner` the anchor's
   flags bits 3-6 (a nibble, 0..15), `alt` its height byte -- what DrawGameScreen's feature pre-pass reads
   (0x4698D8..0x46990F, engine/map.md, *What the feature draw shows a player*). */
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
    out->playing = (d[FD_MASK] & 1u) && (((const unsigned char*)c->grid)[(size_t)cell * FT_STRIDE + FT_FLAGS] & 1u);
    out->gated = (d[FD_MASKHI] & 8u) != 0;
    out->owner = (uint8_t)((((const unsigned char*)c->grid)[(size_t)cell * FT_STRIDE + FT_FLAGS] >> 3) & 15u);
    out->alt = ((const unsigned char*)c->grid)[(size_t)cell * FT_STRIDE + FT_HEIGHT];
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
    out->hp = *(const short*)(u + U_HEALTH);
    if (out->type) out->hp_max = *(const uint32_t*)(def_at(c, out->type) + 0x1FA);
    /* THE LEDGER'S LAST PASS: +0xE4, +0xE8 and +0xD0 are the unit ledger's (+0xBC) +0x28, +0x2C and +0x14, written
       only by the economy pass's settle 0x401360 -- its loop with eax at the unit's +0xC0 (0x401B37) -- which moves
       the period's produced metal +0xD4 to +0xE4 (`mov edx,[eax+0x14]; mov [eax+0x24],edx; mov [eax+0x14],ebx`,
       0x401B7E..0x401B8E), the metal requested +0xD8 to +0xE8 (0x401B75..0x401B7B) and the energy requested +0xC0
       to +0xD0 (0x401B49..0x401B4D), zeroing each, in the call that sums every alive unit's +0xD4 into the
       player's +0x9C (0x401A05); all are zeroed with the ledger at the unit's creation (0x401070 from 0x485D0A).
       A request is added whether or not it is granted (0x4011C0, 0x4011D1..0x4011E3), so the two are what the unit
       asked. DISASSEMBLED: engine/weapons.md, The ledger and The settle. The record is Mode B memory read as data:
       a NaN fails both compares, and any value outside [0, 1e9) reads 0. */
    {
        const float m = *(const float*)(u + U_MADE_LAST_M), rm = *(const float*)(u + U_REQ_LAST_M),
                    re = *(const float*)(u + U_REQ_LAST_E);
        out->made_m = m >= 0.0f && m < 1.0e9f ? m : 0.0f;
        out->req_m = rm >= 0.0f && rm < 1.0e9f ? rm : 0.0f;
        out->req_e = re >= 0.0f && re < 1.0e9f ? re : 0.0f;
    }
    out->active = (uint8_t)(*(const unsigned char*)(u + U_ACTIVE) & 1u);
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
static int queue_check(WQ_ENGINE* c, const char* u, const WQ_CMD* cmd, int* act)
{
    uint16_t menu[QUEUE_MENU];
    int n, k;
    const unsigned builder = def_id(c, u);
    if (cmd->count < 1 || cmd->count > WQ_QUEUE_MAX || !def_at(c, cmd->type) || !builder || !c->menu ||
        (*act = wq_action_of(c, WQ_ACT_BUILDINGBUILD)) < 0)
        return WQ_REFUSED;
    n = c->menu(c->seat, builder, menu, QUEUE_MENU);
    for (k = 0; k < n && menu[k] != cmd->type; k++) {}
    return k < n ? WQ_OK : WQ_REFUSED;
}

/* ORDERS OUT. Every command names units by slot, and before each root call the slot is re-derived: below
   nslots, alive and not excluded, owned by the seat, whose record's controller is a local human (1) or a
   local AI (2) and which is not a puppet seat (tagpu_puppet_seat, refused at the open as well) -- so on a
   puppet nothing is ordered, the recorder being the only writer of a puppet's state. A target slot is
   re-derived the same way without the owner test. A command that names an incarnation (WQ_CMD.inc) is refused
   unless the slot's unit is that one by the creation stamp (tagpu_unit_birth, the stamp WQ_OBS.birth gives),
   and a slot with no stamp is refused, so a command applied after its decision never acts on a unit that took
   the slot since. A REMOVE walks the list again, bounded at WQ_LIST_MAX, to the node's place, and acts only on
   the node whose action, x, z and flags equal what `orders` copied. A NEW or CLICK names an action within the
   descriptor array (*0x512344 to *0x512348, stride 0x19, fixed for the level), a type of 0 or below the def
   count, and a point on the map (worldq.h, wq_cmd_formed): the root call's node keeps the action, and its
   destructor calls through the descriptor with no bound of its own (0x43A202..0x43A21A). So a recycled slot or
   a list another call changed costs a refused command and never a call on memory the engine has freed: a bound,
   and a lifetime -- the game thread owns the unit array and the lists until the teardown, and every caller of
   this runs on that thread. order_check is every refusal, and both `order` and `order_test` answer by it.
   NO NODE POINTER OUTLIVES 0x439F80: its destructor 0x43A1F0 runs the action's handler with event 2 when the
   node's +6 bit 1 (0x2, started; the test at 0x43A1FE) is set, starts the StopBuilding script (0x505104)
   when +0x42 bit 0x400000 is set, and for the head calls 0x489800(unit, 3), which resets the unit's three
   slots and queues a COB start of TargetCleared (0x508D58, through 0x4B0A70); the roots 0x43ADC0 and
   0x43AFC0 allocate and free nodes. */
typedef struct ORDER_OK {
    char* u;                    /* the actor                                                               */
    char* target;               /* the target, NULL for none                                               */
    char* node;                 /* REMOVE: the node found again                                            */
    int   act;                  /* QUEUE: BuildingBuild's action byte                                      */
} ORDER_OK;

static int order_check(WQ_ENGINE* c, const WQ_CMD* cmd, ORDER_OK* ok)
{
    const unsigned controller = *(const unsigned char*)(c->rec + PL_CONTROL);
    unsigned b;
    memset(ok, 0, sizeof *ok);
    ok->u = cmd->unit ? own_unit(c, cmd->unit) : NULL;
    if (!ok->u || (controller != 1 && controller != 2) || tagpu_puppet_seat((unsigned)c->seat)) return WQ_REFUSED;
    if (cmd->target != WQ_NONE && !(ok->target = live_unit(c, cmd->target))) return WQ_REFUSED;
    if ((cmd->inc & WQ_INC_UNIT) && !(tagpu_unit_birth(cmd->unit, &b) && b == cmd->birth)) return WQ_REFUSED;
    if ((cmd->inc & WQ_INC_TARGET) &&
        !(ok->target && tagpu_unit_birth(cmd->target, &b) && b == cmd->tbirth))
        return WQ_REFUSED;
    if (cmd->verb == WQ_CMD_NEW || cmd->verb == WQ_CMD_CLICK)
        return wq_cmd_formed(cmd, action_count(), c->ndefs, c->w, c->h) ? WQ_OK : WQ_REFUSED;
    if (cmd->verb == WQ_CMD_REMOVE) {
        const WQ_ORDER* e = &cmd->expect;
        char* node;
        unsigned place;
        for (node = *(char* const*)(ok->u + U_ORDERS), place = 0;
             node && place < e->place && place < WQ_LIST_MAX && *(const char* const*)(node + N_OWNER) == ok->u;
             node = *(char* const*)(node + N_NEXT), place++) {}
        if (!node || place != e->place || *(const char* const*)(node + N_OWNER) != ok->u ||
            (unsigned char)node[4] != e->action || *(const int*)(node + N_POS) != e->pos[0] ||
            *(const int*)(node + N_POS + 8) != e->pos[2] || *(const unsigned*)(node + N_FLAGS) != e->flags)
            return WQ_REFUSED;
        ok->node = node;
        return WQ_OK;
    }
    if (cmd->verb == WQ_CMD_QUEUE) return queue_check(c, ok->u, cmd, &ok->act);
    return WQ_REFUSED;
}

static int wq_order(const WORLDQ* q, const WQ_CMD* cmd)
{
    WQ_ENGINE* c = ctx_of(q);
    ORDER_OK ok;
    if (order_check(c, cmd, &ok) != WQ_OK) return WQ_REFUSED;
    if (cmd->verb == WQ_CMD_NEW)
        ((Order)0x0043ADC0u)(cmd->action, cmd->shift, ok.u, ok.target, cmd->pos, cmd->type, 0);
    else if (cmd->verb == WQ_CMD_CLICK)
        ((Order)0x0043AFC0u)(cmd->action, cmd->shift, ok.u, ok.target, cmd->pos, cmd->type, 0);
    else if (cmd->verb == WQ_CMD_REMOVE)
        ((RemoveOrder)0x00439F80u)(ok.u, ok.node);
    else
        ((Queue)0x0043B0B0u)(ok.act, ok.u, cmd->type, cmd->count);
    return WQ_OK;
}

static int wq_order_test(const WORLDQ* q, const WQ_CMD* cmd)
{
    ORDER_OK ok;
    return order_check(ctx_of(q), cmd, &ok);
}

/* The seat's record, fixed at the open (its bound there): stored energy +0x8C and metal +0x98, the last pass's
   production +0x90 / +0x9C and requests +0x94 / +0xA0, the storage maxima +0xA4 / +0xA8, as the economy pass
   0x401360 writes them once a pass, every seat on the same ticks (DISASSEMBLED: engine/network.md, the economy
   pass 0x401360). Floats; one not finite reads 0. */
/* THE START POSITIONS, through the engine's own two readers of the game-state object [main+0x391E9]
   (DISASSEMBLED, scout N-b1): 0x437300, thiscall, plain `ret`, counts the records of its specials array
   +0xDB4 (count +0xDB8, the allocation's: count x 12 bytes at 0x437043..0x437055) whose +0 is 1, a StartPos;
   0x437320(&pos, idx), thiscall, `ret 8`, gives the first such record of 0-based index idx as {XPos << 16, 0,
   ZPos << 16}, 1, or 0 for none. Both walk the array within that count, so nothing here reads it. THE BOUND:
   the object pointer is a value tested before the calls; the indices asked are 0 up, until as many as the
   count were found or WQ_STARTS_MAX were asked, so gaps in the map's numbering are walked over and the walk
   ends whatever the count says. THE LIFETIME: the array is written by the .ota read before the level and
   lives for the level -- nothing frees or rewrites it in play but another .ota read or a mode change, none
   inside a tick -- and this runs on the game thread (N-b1). Which schema's positions they are depends on
   the seats a skirmish enabled (the schema whose count equals them, else the one with most).
   WHO STANDS AT EACH (WQ_START.seated), the engine's own rule (DISASSEMBLED 0x4978DA..0x497972): a new skirmish
   -- the game state's mode, 0x435100's [gs+0], 2, and no saved game at main+0x38D6B -- with the settings' fixed
   starts [main+0x29A0]+0x118 set calls the seat setup 0x496EE0(r, r) for each record r of the ten whose first
   dword is set, controller +0x73 1..3 and +0x146 not 10, so record r's commander starts at start r and a start
   whose index is no such record's holds nobody. Any other level -- random starts (0x497979..0x497ADC shuffle the
   list), a network game, a saved game -- answers 0, any record. The records and the settings word are kept by the
   recording, and a replay writes the recorded fixed starts back into +0x118 (tagpu_replay.c, replay_options).
   RESIDUAL [INFERRED]: main+0x38D6B is read at the call, not at the loader's test, taken to hold what it held
   there. */
typedef int (__attribute__((thiscall))* StartCount)(const void* gs);
typedef int (__attribute__((thiscall))* StartAt)(const void* gs, int32_t* pos, int idx);
#define VA_START_COUNT 0x00437300u
#define VA_START_AT    0x00437320u

#define MODE_SKIRMISH    2
#define OFF_SAVED_GAME   0x38D6B
#define OFF_SETTINGS     0x29A0
#define SET_FIXED_STARTS 0x118
#define SEATED_RECORDS   10
#define SEAT_ROW         0x146      /* the record's own row, 10 for none: the seat loop's test at 0x497958 */

/* 1 + the record a new skirmish with fixed starts seated at start idx, WQ_SEATED_NONE for none; 0 for any level
   whose seats the starts do not fix (THE START POSITIONS, WHO STANDS AT EACH) */
static int32_t start_seated(const char* ta, const char* gs, int idx)
{
    const char* set = *(const char* const*)(ta + OFF_SETTINGS);
    const char* r;
    if (*(const int*)gs != MODE_SKIRMISH || *(const uint32_t*)(ta + OFF_SAVED_GAME) || !set ||
        (uintptr_t)set < 0x10000u || !*(const uint32_t*)(set + SET_FIXED_STARTS))
        return 0;
    if (idx < 0 || idx >= SEATED_RECORDS) return WQ_SEATED_NONE;
    r = ta + OFF_PLAYERS + idx * PLAYER_STRIDE;
    return *(const uint32_t*)r && (uint8_t)r[PL_CONTROL] >= 1 && (uint8_t)r[PL_CONTROL] <= 3 &&
           (uint8_t)r[SEAT_ROW] != 10 ? idx + 1 : WQ_SEATED_NONE;
}

static int wq_starts(const WORLDQ* q, WQ_START* out, int cap)
{
    const char* ta = ctx_of(q)->ta;
    const char* gs = *(const char* const*)(ta + 0x391E9);
    int n, idx, k = 0;
    if (!gs || (uintptr_t)gs < 0x10000u) return 0;
    n = ((StartCount)(uintptr_t)VA_START_COUNT)(gs);
    for (idx = 0; idx < WQ_STARTS_MAX && k < n; idx++) {
        int32_t pos[3];
        if (!((StartAt)(uintptr_t)VA_START_AT)(gs, pos, idx)) continue;
        if (k < cap) {
            out[k].x = pos[0] >> 16;
            out[k].z = pos[2] >> 16;
            out[k].index = idx;
            out[k].seated = start_seated(ta, gs, idx);
        }
        k++;
    }
    return k < cap ? k : cap;
}

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

/* ---- what a seat sees: 0x465AC0's line of sight for any seat, and the seat's acquisition contacts ---- */

#define U_ALIVE    0x10000000u
#define D_LOS_X    0x15E        /* the line-of-sight box: its first point's offsets from the unit */
#define D_LOS_Y    0x16E
#define D_LOS_Z    0x166
#define D_BOX_X    0x176        /* and the box's extents                                          */
#define D_BOX_Y    0x17A
#define D_BOX_Z    0x17E
#define PL_LOS     0x7C         /* u8[] the line-of-sight counters, +0x80 x +0x84 blocks          */
#define PL_LOS_W   0x80
#define PL_LOS_H   0x84
#define PL_MAPBIT  0x146        /* the record's bit in the mapped grid, its player index          */

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
     - a SUBMERGED unit -- its first point below the sea -- is seen by the engine only when its sonar gate
       0x465B30 answers: inside the seat's own acquisition the seat's sonar contact (tagpu_seatcontact.c),
       everywhere else +0x110 bit 9, the VIEWED player's (loop 2 of 0x467440), for any other seat another
       player's. So ImpAI never takes a submerged unit as seen; it reaches the seat as its own sonar contact
       alone (radar_step);
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

/* The owners whose units the seat keeps as the radar pass's first loop keeps them, its own aside: every owner
   whose contacts it receives by allied sharing's S (tagpu_allysight.h) -- the rule its acquisition walks their
   radar and sonar by (tagpu_seatcontact.c) and the viewed player's pass keeps their units by. None while that
   module's sites are not in, as the seat's acquisition then receives none. */
static int shares_contacts(const WQ_ENGINE* c, unsigned owner)
{
    return tagpu_allysight_contacts(owner, (unsigned)c->seat);
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

/* WQ_AIM_* of unit u of def d, the resolver's own reads (worldq.h, WQ_OBS.aim) */
static uint8_t aim_of(const WQ_ENGINE* c, const char* u, const char* d)
{
    const int under = *(const int16_t*)(u + U_ZFIX + 2) + *(const int16_t*)(d + D_LOS_Y + 2) <
                      (int)*(const unsigned char*)(c->ta + OFF_SEALEVEL);
    return (uint8_t)(((*(const unsigned*)(u + U_FLAGS) & 3u) == 2u ? WQ_AIM_AIRBORNE : 0u) |
                     (under ? WQ_AIM_UNDER : 0u) |
                     ((*(const unsigned*)(d + D_FLAGS) & 0x800u) ? WQ_AIM_FLIES : 0u));
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
    else if (shares_contacts(c, owner)) out->seen = WQ_SEEN_SHARED;
    else if (contact) out->seen = WQ_SEEN_CONTACT;
    else return 0;
    if (out->seen <= WQ_SEEN_LOS) {
        out->owner = (uint8_t)owner;
        out->type = (uint16_t)def_id(c, u);
        out->aim = aim_of(c, u, d);
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

/* THE SEAT'S RADAR PASS is its acquisition's contacts (tagpu_seatcontact.h): what the seat's own targeting takes --
   its radar and sonar and those of every owner whose contacts it receives, by the radar pass 0x467440's rules --
   never the +0x110 bits 8 to 10 that pass writes for the VIEWED record alone, which are the display's
   (research/notes/modules/allied-sharing.md, *Per-seat contacts*). The pass is whole in the call that starts it,
   which is the seat's turn in the players phase, outside any acquisition, so every record it writes is of now:
     1. the seat's own units and those of every owner whose contacts it receives (shares_contacts) get radar,
        sonar and WQ_RB_KEPT, as the pass's first loop keeps them; an own unit's record is its incarnation alone
        (radar_mine);
     2. every other live unit takes the scan's TAGPU_SEAT_CONTACT as radar -- a radar contact not radar-jammed,
        or its origin's block lit for the seat -- and TAGPU_SEAT_SONAR, a sonar contact not sonar-jammed, as
        sonar; its record is the unit and block of now (radar_hit).
   TAGPU_SEAT_SEES is not taken: observe answers sight by the seat's own copy (unit_los), which reads the seat's
   explored bit where UnitInPlayerLOS reads the viewed player's (sight_lit). The scan writes a scratch block of
   the call, one byte a slot, read only below the count the scan says it wrote and only for its two bits. A scan
   that answers nothing -- the module's sites not in, a seat without an acquisition record -- leaves the seat its
   own units and its sharers': the pass still completes, so the publish expires every contact the seat held.
   The cost, one visit a slot, comes off the budget at once. */

/* The slot's record at a bit the pass sets (WQ_RADAR_HIT): the unit's incarnation and block now. */
static void radar_hit(const WQ_ENGINE* c, const WQ_HITS* hits, unsigned slot, const char* u)
{
    WQ_RADAR_HIT* h = wq_hit(hits, slot);
    WQ_OBS o;
    block_of(c, u, &o);
    h->bx = o.bx;
    h->bz = o.bz;
    h->known = (uint8_t)tagpu_unit_birth(slot, &h->birth);
    if (!h->known) h->birth = 0;
    if (hits->stamp) h->at = (uint32_t)*(const int*)(c->ta + OFF_GAMETIME);
}

/* An own unit's record: its incarnation alone, each byte written only when it changes, so the pass dirties no
   page of the records for the units the seat keeps (the publish skips them, WQ_SEEN_OWN). */
static void radar_mine(const WQ_ENGINE* c, const WQ_HITS* hits, unsigned slot)
{
    WQ_RADAR_HIT* h = wq_hit(hits, slot);
    unsigned birth;
    const uint8_t known = (uint8_t)tagpu_unit_birth(slot, &birth);
    if (!known) birth = 0;
    if (h->known != known) h->known = known;
    if (h->birth != birth) h->birth = birth;
    if (hits->stamp) h->at = (uint32_t)*(const int*)(c->ta + OFF_GAMETIME);
}

static int wq_radar_step(const WORLDQ* q, WQ_RADAR* st, uint8_t* bits, int nbytes, const WQ_HITS* hits,
                         int32_t* budget)
{
    const WQ_ENGINE* c = ctx_of(q);
    unsigned char* scan;
    unsigned slot, got = 0;
    if (nbytes < (int)WQ_RADAR_BYTES(c->nslots) || hits->n < 0 || (unsigned)hits->n < c->nslots ||
        hits->stride < sizeof(WQ_RADAR_HIT))
        return 0;
    memset(bits, 0, WQ_RADAR_BYTES(c->nslots));
    if ((scan = (unsigned char*)tagpu_sim_scratch(c->nslots)) != NULL)
        got = tagpu_seatcontact_scan((unsigned)c->seat, scan, c->nslots);
    for (slot = 1; slot < c->nslots; slot++) {
        const char *u, *d;
        unsigned owner, v;
        if (!(u = seen_unit(c, slot, &d))) continue;
        owner = *(const unsigned char*)(u + U_OWNER);
        if (owner == (unsigned)c->seat) {
            wq_radar_set(bits, slot, WQ_RB_RADAR | WQ_RB_SONAR | WQ_RB_KEPT);
            radar_mine(c, hits, slot);
            continue;
        }
        if (shares_contacts(c, owner)) v = WQ_RB_RADAR | WQ_RB_SONAR | WQ_RB_KEPT;
        else if (slot < got)
            v = (scan[slot] & TAGPU_SEAT_CONTACT ? WQ_RB_RADAR : 0u) | (scan[slot] & TAGPU_SEAT_SONAR ? WQ_RB_SONAR : 0u);
        else
            v = 0;
        if (!v) continue;
        wq_radar_set(bits, slot, v);
        radar_hit(c, hits, slot, u);
    }
    if (scan) tagpu_sim_scratch_free(scan);
    *budget -= (int32_t)c->nslots;
    st->stage = WQ_RS_IDLE;
    st->sub = 0;
    st->cursor = 0;
    st->start = (uint32_t)*(const int*)(c->ta + OFF_GAMETIME);
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

/* THE CONTENT MAP (worldq.h, passable): the level load's fill 0x440500 recomputed over the window from the map's
   content, with the per-cell rule 0x47DE60 less its unit rule -- slot A, which holds every player's structures and
   movers, seen or not -- so the op answers what the seat may know and nothing else. The rule's halves are the core's
   ports (tasim_class.c: the feature rule here over the engine's own cells, the ground half ts_class_ground, the fill
   ts_class_fold), which tests/tasim_class_test.c holds to the core's fill and classcheck.py the core's fill to the
   engine's; ImpAI's check lever holds this op to the engine's maps at the level init, where no unit stands yet
   (tagpu_impai.c, THE REACH BUILD). The fill's values are the map's as the load leaves it: at the last column and
   row a re-mark gives 0 or 1 where the fill gives 1 or 3 (engine/map.md, the fill and the refresh disagree), and
   this answers the fill's.
   THE CLASS, record 0x512358 + 0x20 c: a name (+0x00) or none; its footprint (+0x04, +0x06), held to
   1..WQ_FOOT_MAX, else the class answers 0 everywhere; its depths (+0x08, +0x0A) and slopes (+0x0C..+0x0F).
   THE CELL, 13 bytes at grid + (z W + x) 13 (engine/map.md, The cell): the feature word +0x08, the back-reference
   +0x0A (rows) and +0x0B (columns), the 2x2 maximum +0x05 and minimum +0x06; the sea level main+0x1427F.
   THE FEATURE RULE, the core's feature_blocks over these bytes: 0xFFFF free; a definition below 0xFFFB blocks by
   FeatureDef +0xFE bit 6, one at or past the count blocks; 0xFFFB..0xFFFD block; 0xFFFE hops (rows W + columns)
   back to its anchor, which blocks when the hop lands before the grid, reads free from 0xFFFB up, and otherwise
   as a definition. BOUNDS: W and H as the open holds them; the FeatureDefs are read only below their count, which is
   held to 0..0xFFFA, with no array none is read; the window held so the grown window's cells fit an int. */
typedef struct CONTENT { const unsigned char* grid; const unsigned char* fdefs; int nfd, w, h, sea; } CONTENT;

static int content_feature_blocks(const CONTENT* k, int idx)
{
    const unsigned char* cl = k->grid + (size_t)idx * FT_STRIDE;
    unsigned word = *(const uint16_t*)(cl + FT_DEFIDX);
    if (word == 0xFFFFu) return 0;
    if (word == 0xFFFEu) {
        const int a = idx - (cl[FT_WIDX] * k->w + cl[FT_WIDX + 1]);
        if (a < 0) return 1;
        word = *(const uint16_t*)(k->grid + (size_t)a * FT_STRIDE + FT_DEFIDX);
        if (word >= 0xFFFBu) return 0;
    } else if (word >= 0xFFFBu)
        return 1;
    if ((int)word >= k->nfd || !k->fdefs) return 1;
    return k->fdefs[(size_t)word * FD_STRIDE + FD_MASK] >> 6 & 1;
}

static int content_cell(const CONTENT* k, const TS_CLASSMAP* c, int cx, int cz)
{
    int idx;
    const unsigned char* cl;
    if (cx < 0 || cz < 0 || cx >= k->w || cz >= k->h) return 0;
    idx = cz * k->w + cx;
    if (content_feature_blocks(k, idx)) return 0;
    cl = k->grid + (size_t)idx * FT_STRIDE;
    return ts_class_ground(c, k->sea, cl[6], cl[5]);
}

static int wq_passable(const WORLDQ* q, int mclass, int x0, int z0, int w, int h, uint8_t* out, size_t cap)
{
    const WQ_ENGINE* e = ctx_of(q);
    const char* rec;
    TS_CLASSMAP c;
    CONTENT k;
    int fx, fz, i, j;
    if (w < 1 || h < 1 || w > 0x7FFF || h > 0x7FFF || x0 < -0x7FFF || z0 < -0x7FFF || x0 > 0x7FFF ||
        z0 > 0x7FFF || cap < WQ_PASS_CAP(w, h))
        return 0;
    rec = mclass >= 0 && mclass < WQ_MCLASSES ? (const char*)(uintptr_t)(MC_BASE + (unsigned)mclass * MC_STRIDE) : NULL;
    fx = rec ? *(const int16_t*)(rec + 4) : 0;
    fz = rec ? *(const int16_t*)(rec + 6) : 0;
    if (!rec || !*(const char* const*)rec || fx < 1 || fz < 1 || fx > WQ_FOOT_MAX || fz > WQ_FOOT_MAX) {
        memset(out, 0, (size_t)w * (size_t)h);
        return 1;
    }
    memset(&c, 0, sizeof c);
    c.present = 1;
    c.fx = (int16_t)fx;
    c.fz = (int16_t)fz;
    c.maxwaterdepth = *(const int16_t*)(rec + 0x08);
    c.minwaterdepth = *(const int16_t*)(rec + 0x0A);
    c.maxslope = *(const uint8_t*)(rec + 0x0C);
    c.badslope = *(const uint8_t*)(rec + 0x0D);
    c.maxwaterslope = *(const uint8_t*)(rec + 0x0E);
    c.badwaterslope = *(const uint8_t*)(rec + 0x0F);
    k.grid = (const unsigned char*)e->grid;
    k.fdefs = *(const unsigned char* const*)(e->ta + OFF_FEATDEF);
    k.nfd = *(const int*)(e->ta + OFF_FEATCOUNT);
    if (k.nfd < 0 || k.nfd > 0xFFFA) k.nfd = 0;
    k.w = e->w;
    k.h = e->h;
    k.sea = *(const uint8_t*)(e->ta + OFF_SEALEVEL);
    for (j = 0; j < h + fz + 2; j++)
        for (i = 0; i < w + fx + 2; i++)
            out[(size_t)j * (size_t)(w + fx + 2) + (size_t)i] = (uint8_t)content_cell(&k, &c, x0 - 1 + i, z0 - 1 + j);
    ts_class_fold(out, w, h, fx, fz);
    return 1;
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
   each. Build options: a builder's (+0x241 bit 6, the bit the classifier's factory test reads) are what
   `options` answers for it, without the builder itself, as the file filler leaves a builder's own type out.
   BOUND: every option is a type below n, else the fill fails; they are packed into opt[0..optcap). */
#define WF_TURRET (1u << 19)    /* +0x111 `turret`: AutoAim's aim path (0x49E205..0x49E20B) */

/* THE SCRIPTED RELOAD of slot k of `def`, its weapon record wr (tasim_cobfire.h): the ticks between its shots as the
   type's own script times them under AutoAim, at least the weapon's reload +0xE4. The reload itself for a slot past
   the three the engine starts an aim script for (0x509688), a weapon that starts none -- neither `turret` (bit 19)
   nor `vlaunch` (bit 4): AutoAim's tests at 0x49E208 and 0x49E342 --, or a type with no validated program
   (tagpu_cob_def_program). SetMaxReloadTime's argument is the largest reload of the def's first three slots, as
   0x49E089..0x49E14B takes it. On the LOADER thread in the level init, where the COB load (0x42D8EF, inside the
   unit-data load 0x42D2E0) has bound every def's script and the game thread waits: the program lives until the
   teardown. BOUND: three probes a type, each TASIM_FIRE_SPAN ticks of the VM's bounded steps. */
static uint32_t slot_reload(const char* def, unsigned k, const char* wr, int nw)
{
    const uint32_t reload = *(const uint16_t*)(wr + 0xE4);
    const uint32_t flags = *(const uint32_t*)(wr + 0x111);
    const TagpuCobProgram* prog;
    uint32_t most = 0, got;
    unsigned j;
    size_t bytes;
    void* work;
    if (k >= TASIM_FIRE_SLOTS || !(flags & (WF_TURRET | TASIM_WF_VLAUNCH))) return reload;
    if (!(prog = (const TagpuCobProgram*)tagpu_cob_def_program(def))) return reload;
    for (j = 0; j < TASIM_FIRE_SLOTS; j++) {
        const char* w = tagpu_weapons_type_weapon(def, j);
        const int wi = w ? tagpu_limits_weapon_index(w) : -1;
        if (wi > 0 && wi < nw && *(const uint16_t*)(w + 0xE4) > most) most = *(const uint16_t*)(w + 0xE4);
    }
    bytes = tasim_cobfire_work(prog);
    if (!(work = tagpu_sim_scratch(bytes))) return reload;
    got = tasim_cobfire_interval(prog, k, reload, most, work, bytes);
    tagpu_sim_scratch_free(work);
    return got;
}

int tagpu_worldq_role_in(TASIM_ROLE_IN* in, int n, WqRoleOptFn options, uint16_t* opt, unsigned optcap)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const char* defs;
    const uint32_t* vtol;
    uint16_t* air;
    float* wair;                /* a weapon's air damage by its index, taken once: NAN until then */
    CATMAP cat;
    const int nw = tagpu_limits_weapon_slots();
    unsigned used = 0;
    int i, k, nair = 0;
    if (n <= 0 || n != tagpu_worldq_role_count() || nw <= 0 || !options || !opt || !catmap_open(&cat)) return -1;
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
        r->flags241 = *(const uint32_t*)(d + D_FLAGS) & TASIM_ROLE_F241;
        r->flags245 = *(const uint32_t*)(d + D_TAGS) & TASIM_ROLE_F245;
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
            o->reload_ticks = (float)slot_reload(d, (unsigned)k, w, nw);
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
            o->aoe = *(const uint16_t*)(w + 0xD6);
            o->flags = (*(const uint32_t*)(w + 0x111) & TASIM_ROLE_WF) |
                       ((tagpu_datakeys_wkey(w) & TAGPU_WK_NOTTOAIR) ? TASIM_WF_NOTTOAIR : 0u);
            r->nweapons = (uint16_t)(k + 1);
        }
        /* THE DEATH BLAST: def +0x220 is ExplodeAs, the record the death explosion takes for a death that is
           not a self-destruct (+0x224 is SelfDestructAs, taken when its second argument is nonzero:
           0x49B017..0x49B026, DISASSEMBLED); +0xD6 is the record's areaofeffect (the weapon parser 0x42E440, read
           as the fight filler reads it). BOUND: the pointer is read through only when it is a record of the
           weapon array, by its index, as the slot loop above takes a weapon; anything else is no blast. */
        {
            const char* w = *(const char* const*)(d + 0x220);
            const int wi = w ? tagpu_limits_weapon_index(w) : -1;
            if (wi > 0 && wi < nw) r->explode_aoe = *(const uint16_t*)(w + 0xD6);
        }
        if (r->flags241 & TASIM_F241_BUILDER) {
            const int got = options((unsigned)i, opt + used, (int)(optcap - used < 0xFFFFu ? optcap - used : 0xFFFFu));
            unsigned m = 0;
            if (got < 0) break;
            for (k = 0; k < got && opt[used + k] < (unsigned)n; k++)
                if (opt[used + k] != (unsigned)i) opt[used + m++] = opt[used + k];
            if (k < got) break;
            r->build = opt + used;
            r->nbuild = (uint16_t)m;
            used += m;
        }
    }
    tagpu_sim_scratch_free(wair);
    tagpu_sim_scratch_free(air);
    return i < n ? -1 : 0;
}

/* ---- the fight predictor's content (tagpu_worldq.h, THE FIGHT CONTENT'S ENGINE FILLER) ----

   What it reads, where and when, and every bound: the role filler's comment above, the same records on the
   same thread at the same point, through the same name_eq, ptr_ok and DMG_MAX. Beyond the role filler's
   fields it reads, from a def (engine/keys.md, the FBI parser 0x42BF40): the footprint +0x14A/+0x14C (the
   class record's when the unit names one, 0x42CD0C..0x42CD97), the waterline byte +0x22C, the move class
   +0x22F, the `floater` and `upright` bits of +0x241 (bits 19 and 20), and the model's top +0x16E, a 16.16 offset the model load
   writes before the parser derives the box +0x17A from it (0x42D079..0x42D11F); from a weapon record (the weapon parser 0x42E440): velocity +0x68 (16.16 a tick),
   areaofeffect +0xD6, edgeeffectiveness +0xD8, sprayangle +0xEE, accuracy +0x104 and the whole flag word
   +0x111, whose bit 31 the parser never writes (TASIM_WF_NOTTOAIR carries Impure's key there).

   WHICH WEAPONS COUNT: every slot the extra-weapons module gives the type, as the role filler takes them,
   but not a stockpile or interceptor weapon, which fires only on a stockpile order or at a missile, never at
   what a fight puts in front of it; a CommandFire weapon counts, as the stock weapon scan fires it on an AI
   seat (0x408A01..0x408A96).

   THE [DAMAGE] ROWS: 0x499CD0 takes, for a target, the first row whose name equals the target def's name
   under _stricmp's fold, else the default. So a row resolves to every type of that name through an index of
   the defs sorted by folded name, a row naming no type is dropped, and each weapon's rows are sorted by type
   with only the first row of a type kept: the row 0x499CD0 would take. BOUND: the rows of all the weapons
   together at most dcap, and of one weapon at most 0xFFFF (its count's width), else the whole fill fails. */
#define D_FOOT_X     0x14A
#define D_FOOT_Z     0x14C
#define D_TOP        0x16E             /* i32 16.16: the model's top over the unit's altitude            */
#define D_WATERLINE  0x22C
#define D_MOVECLASS  0x22F             /* u8: 1 for a type the creates give a mover                  */
#define D_F241_FLOATER (1u << 19)   /* def +0x241 `floater`                                         */
#define D_F241_UPRIGHT (1u << 20)   /* def +0x241 `upright`                                         */
#define NAME_CAP     33                /* a def name and a row name compared as 0x499CD0's do (damage_vs) */

/* a and b ordered as _stricmp orders them, each read to its NUL or `cap` */
static int name_cmp(const char* a, const char* b, int cap)
{
    int k;
    for (k = 0; k < cap; k++) {
        const int x = fold((unsigned char)a[k]), y = fold((unsigned char)b[k]);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}

/* a[0..n) of def indices sorted by name, stably: a bottom-up merge through tmp. BOUND: n <= TAGPU_LIM_TYPES. */
static void sort_by_name(uint16_t* a, uint16_t* tmp, unsigned n, const char* defs)
{
    unsigned w, i;
    for (w = 1; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            const unsigned mid = i + w < n ? i + w : n, hi = i + 2 * w < n ? i + 2 * w : n;
            unsigned p = i, q = mid, k = i;
            while (p < mid && q < hi)
                tmp[k++] = name_cmp(defs + (size_t)a[q] * D_STRIDE + D_NAME, defs + (size_t)a[p] * D_STRIDE + D_NAME,
                                    NAME_CAP) < 0 ? a[q++] : a[p++];
            while (p < mid) tmp[k++] = a[p++];
            while (q < hi) tmp[k++] = a[q++];
        }
        memcpy(a, tmp, (size_t)n * sizeof *a);
    }
}

/* d[0..n) sorted by type, stably, so a type's rows keep the table's order. BOUND: n <= IMPAI_FDMG_POOL. */
static void sort_by_type(IMPAI_FDMG* d, IMPAI_FDMG* tmp, unsigned n)
{
    unsigned w, i;
    for (w = 1; w < n; w *= 2) {
        for (i = 0; i < n; i += 2 * w) {
            const unsigned mid = i + w < n ? i + w : n, hi = i + 2 * w < n ? i + 2 * w : n;
            unsigned p = i, q = mid, k = i;
            while (p < mid && q < hi) tmp[k++] = d[q].type < d[p].type ? d[q++] : d[p++];
            while (p < mid) tmp[k++] = d[p++];
            while (q < hi) tmp[k++] = d[q++];
        }
        memcpy(d, tmp, (size_t)n * sizeof *d);
    }
}

/* the first of idx[0..n) whose def's name is `name`, as name_eq decides it, or n */
static unsigned name_find(const uint16_t* idx, unsigned n, const char* defs, const char* name)
{
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        if (name_cmp(defs + (size_t)idx[mid] * D_STRIDE + D_NAME, name, NAME_CAP) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && name_eq(name, defs + (size_t)idx[lo] * D_STRIDE + D_NAME, NAME_CAP) ? lo : n;
}

/* weapon `w`'s [DAMAGE] rows resolved into d[at ..): the count, or -1 when past cap */
static int damage_rows(const char* w, const char* defs, const uint16_t* idx, unsigned n, IMPAI_FDMG* d,
                       IMPAI_FDMG* tmp, unsigned at, unsigned cap)
{
    const char* t = *(const char* const*)(w + W_DMG);
    const char *b, *e;
    size_t k, rows;
    unsigned got = 0, kept = 0, j;
    if (!t || !ptr_ok(t)) return 0;
    b = *(const char* const*)(t + 5);
    e = *(const char* const*)(t + 9);
    if (b == e || !ptr_ok(b) || !ptr_ok(e) || e < b || (size_t)(e - b) % 8 || (size_t)(e - b) / 8 > DMG_MAX)
        return 0;
    rows = (size_t)(e - b) / 8;
    for (k = 0; k < rows; k++) {
        const char* name = *(const char* const*)(b + k * 8);
        unsigned i;
        if (!ptr_ok(name)) continue;
        for (i = name_find(idx, n, defs, name);
             i < n && name_eq(name, defs + (size_t)idx[i] * D_STRIDE + D_NAME, NAME_CAP); i++) {
            if (at + got >= cap) return -1;
            d[at + got].type = idx[i];
            d[at + got].pad = 0;
            d[at + got].dmg = *(const int32_t*)(b + k * 8 + 4);
            got++;
        }
    }
    sort_by_type(d + at, tmp, got);
    for (j = 0; j < got; j++)
        if (!kept || d[at + kept - 1].type != d[at + j].type) d[at + kept++] = d[at + j];
    memset(d + at + kept, 0, (size_t)(got - kept) * sizeof *d);
    return (int)kept;
}

int tagpu_worldq_fight_in(const TASIM_ROLE_IN* rin, const TASIM_ROLE_OUT* rout, int n, IMPAI_FTYPE* t,
                          IMPAI_FWEAPON* w, unsigned wcap, unsigned* nw, IMPAI_FDMG* d, unsigned dcap, unsigned* nd)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const char* defs;
    uint16_t *idx = NULL, *itmp = NULL;
    uint8_t* seen = NULL;
    IMPAI_FDMG* dtmp = NULL;
    const int slots = tagpu_limits_weapon_slots();
    const unsigned wlim = slots > 0 && (unsigned)slots < wcap ? (unsigned)slots : wcap;
    unsigned at = 0, top = 0, i;
    int k, rc = -1;
    *nw = *nd = 0;
    if (n <= 0 || n != tagpu_worldq_role_count() || slots <= 0) return -1;
    defs = *(const char* const*)(ta + D_BASE);
    if (!(idx = (uint16_t*)tagpu_sim_scratch((size_t)n * sizeof *idx)) ||
        !(itmp = (uint16_t*)tagpu_sim_scratch((size_t)n * sizeof *itmp)) ||
        !(seen = (uint8_t*)tagpu_sim_scratch(wlim)) ||
        !(dtmp = (IMPAI_FDMG*)tagpu_sim_scratch((size_t)dcap * sizeof *dtmp)))
        goto out;
    memset(seen, 0, wlim);
    for (i = 0; i < (unsigned)n; i++) idx[i] = (uint16_t)i;
    sort_by_name(idx, itmp, (unsigned)n, defs);
    for (i = 0; i < (unsigned)n; i++) {
        const char* def = defs + (size_t)i * D_STRIDE;
        const short fx = *(const short*)(def + D_FOOT_X), fz = *(const short*)(def + D_FOOT_Z);
        const unsigned wl = *(const uint8_t*)(def + D_WATERLINE);
        IMPAI_FTYPE* o = &t[i];
        memset(o, 0, sizeof *o);
        o->hp = *(const uint32_t*)(def + 0x1FA);
        o->value = (float)rout[i].value;
        o->speed = *(const int32_t*)(def + 0x192);
        o->layer = (uint8_t)rout[i].layer;
        o->role = (uint8_t)rout[i].role;
        o->klass = (uint8_t)impai_class_of(&rin[i], &rout[i]);
        o->fx = (uint8_t)(fx < 0 ? 0 : fx > 255 ? 255 : fx);
        o->fz = (uint8_t)(fz < 0 ? 0 : fz > 255 ? 255 : fz);
        if (rout[i].flags & TASIM_RF_MOBILE) o->flags |= IMPAI_FT_MOBILE;
        if (rout[i].flags & TASIM_RF_COMMANDER) o->flags |= IMPAI_FT_COMMANDER;
        if (rin[i].flags241 & TASIM_F241_CANFLY) o->flags |= IMPAI_FT_FLIES;
        /* SUBMERGED: the top below the sea, 0x465AC0's test (0x465B41..0x465B4E), y + def +0x16E < sea << 16,
           which setSFXoccupy's state 3 makes on the whole parts (0x43DBBA..0x43DBC7). Where a sea type stands
           decides it [DISASSEMBLED 2026-11-14]:
           - a structure (+0x22F 0) at the lowest of its yard cells, or at the sea less its waterline +0x22C when
             its ground lies below that (0x47DDC0 through 0x47D820, 0x47D92A..0x47D94A);
           - a type with a mover (+0x22F 1, the creates' rule) where the altitude fix-up 0x48A870 puts it: the
             creates run it (0x486109, 0x486306), every mover sets its due bit (+0x110 bit 16, engine/units.md)
             and the unit tick runs it after the movement pass (0x48AFAA..0x48AFB0). An `upright` type (+0x241
             bit 20, tested first) goes to the terrain height (0x48A8CD..0x48A935; only `canhover`, which no sea
             type has, raises it), a `floater` (bit 19) to the sea less its waterline (0x48A938..0x48A966), any
             other to the fit to its footprint's corners, the terrain's own heights for a type that does not
             hover (0x48A490, 0x48A60C..0x48A664). At sea the terrain is the sea floor.
           So a structure and a floater that is not upright are submerged when the top is below the waterline,
           and every other type with a mover wherever the water is deeper than its top: its waterline plays no
           part (impai_fight_submerged). RESIDUAL: such a type in water shallower than its top -- 9.7 to 42.5 for
           the submarines that are all of that class in retail, Total Mayhem 11.3 and TA Zero (MEASURED
           2026-11-14), and never shallower than its minwaterdepth +0x1C0, 15 to 20 for them -- stands surfaced,
           and the flag still calls it submerged. No sea type of the three has a move class past 1 (tasim_run
           defs, MEASURED 2026-11-14); one would get neither a mover nor 0x47DDC0's altitude, and the waterline's
           test stands for it [INFERRED]. */
        if (rout[i].layer == TASIM_LAYER_SEA &&
            impai_fight_submerged(*(const int32_t*)(def + D_TOP), wl, *(const uint8_t*)(def + D_MOVECLASS) == 1,
                                  (*(const uint32_t*)(def + D_FLAGS) & D_F241_FLOATER) != 0,
                                  (*(const uint32_t*)(def + D_FLAGS) & D_F241_UPRIGHT) != 0))
            o->flags |= IMPAI_FT_SUBMERGED;
        for (k = 0; k < IMPAI_FW_SLOTS; k++) {
            const char* wr = tagpu_weapons_type_weapon(def, (unsigned)k);
            uint32_t flags;
            int wi, got;
            if (!wr) break;
            if ((wi = tagpu_limits_weapon_index(wr)) <= 0 || (unsigned)wi >= wlim) continue;
            flags = *(const uint32_t*)(wr + 0x111);
            if (flags & (TASIM_WF_STOCKPILE | TASIM_WF_INTERCEPTOR)) continue;
            o->w[k] = (uint16_t)wi;
            if (rin[i].w[k].reload_ticks > (float)*(const uint16_t*)(wr + 0xE4))
                o->rl[k] = (uint16_t)(rin[i].w[k].reload_ticks < 65535.0f ? rin[i].w[k].reload_ticks : 65535.0f);
            if (tasim_roles_slot_antiair(&rin[i], k)) o->air |= (uint16_t)(1u << k);
            if ((unsigned)wi + 1 > top) top = (unsigned)wi + 1;
            if (seen[wi]) continue;
            seen[wi] = 1;
            {
                IMPAI_FWEAPON* f = &w[wi];
                memset(f, 0, sizeof *f);
                f->range = *(const int32_t*)(wr + 0xDC);
                f->velocity = *(const int32_t*)(wr + 0x68);
                f->reload = *(const uint16_t*)(wr + 0xE4);
                f->burst = *(const uint16_t*)(wr + 0xEA);
                f->dmg = *(const uint16_t*)(wr + 0xD4);
                f->aoe = *(const uint16_t*)(wr + 0xD6);
                f->edge = *(const float*)(wr + 0xD8);
                f->accuracy = *(const uint16_t*)(wr + 0x104);
                f->spray = *(const uint16_t*)(wr + 0xEE);
                f->flags = (flags & ~TASIM_WF_NOTTOAIR) |
                           ((tagpu_datakeys_wkey(wr) & TAGPU_WK_NOTTOAIR) ? TASIM_WF_NOTTOAIR : 0u);
                f->kind = (uint8_t)impai_fight_kind(f->flags);
                if ((got = damage_rows(wr, defs, idx, (unsigned)n, d, dtmp, at, dcap)) < 0 || got > 0xFFFF)
                    goto out;
                f->d0 = at;
                f->nd = (uint16_t)got;
                at += (unsigned)got;
            }
        }
    }
    *nw = top;
    *nd = at;
    rc = 0;
out:
    tagpu_sim_scratch_free(dtmp);
    tagpu_sim_scratch_free(seen);
    tagpu_sim_scratch_free(itmp);
    tagpu_sim_scratch_free(idx);
    return rc;
}

static const WORLDQ_OPS OPS = {
    wq_map, wq_explored, wq_metal, wq_coverage, wq_mapped_point, wq_ground, wq_ndefs, wq_def, wq_def_econ, wq_yard,
    wq_build_options, wq_site_ok, wq_blocked_alt, wq_feature, wq_nslots, wq_seat_units, wq_unit, wq_orders, wq_head,
    wq_can, wq_action, wq_order, wq_economy, wq_gametime, wq_observe, wq_block_lit, wq_radar_step, wq_passable,
    wq_starts, wq_order_test,
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
