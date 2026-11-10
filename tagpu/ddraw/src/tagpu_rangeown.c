/* tagpu_rangeown.c -- the weapon range rings' game-thread half.
   Contract: tagpu_rangeown.h. Design: research/notes/weapon-range-rings.md.

   EVERY ENGINE VALUE IS DATA UNTIL BOUNDED. The unit array is walked by the
   engine's own slot count, checked against its begin and end pointers as
   tagpu_route.c checks them; a unit type by UNITINFOCount; a weapon pointer by
   tagpu_limits_weapon_index (a record of the weapon array, and not the
   no-weapon entry 0); a player id by the ten records and the alliance row by
   its eleven bytes. Ranges, velocities and angles are clamped into the bounds
   the acquire tests (tagpu_packet.c), so a ring this file makes is never the
   reason a packet is refused. */

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "tagpu_rangeown.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_settings.h"
#include "tagpu_weapons.h"
#include "tagpu_log.h"
#include "dd.h"
#include "render_vk.h"

#define M_HOVER      0x2CBA      /* u16 the unit under the pointer: its slot      */
#define M_POINT      0x2CAA      /* 16.16 {x, altitude, z} the pointer's world point */
#define M_REGION     0x2CC6      /* bit 1: the pointer is over the world view     */
#define M_GRAVITY    0x14263     /* i32 16.16 world px a tick squared             */
#define PL_ALLY      0x108       /* u8[11]: 1 = this player allies player j       */
#define MODE_BUILD   14

#define ST_SELECTED  0x10u
#define ST_STRUCT    0x20000000u /* the def's bmcode is 0: it cannot move        */

#define UD_BUILDDIST 0x212       /* u16 builddistance                             */
#define UD_BMCODE    0x22F       /* u8, 0 = a structure                           */
#define UD_BUILDER   0x40u       /* UD_TYPEMASK: the FBI's builder                */
#define UD_AWB       (1u << 18)  /* UD_TYPEMASK: ActivateWhenBuilt (0x41B9AC)     */
#define UD_FLAGS2    0x245       /* u32, more FBI booleans                        */
#define UD_ONOFF     (1u << 2)   /* UD_FLAGS2: onoffable (0x40301A)               */
#define UD_RADAR     0x204       /* s16 radardistance                             */
#define UD_SONAR     0x206       /* s16 sonardistance                             */
#define UD_RADARJAM  0x20A       /* s16 radardistancejam                          */
#define UD_SONARJAM  0x20C       /* s16 sonardistancejam                          */

#define U_ACTIVATION 0x10E       /* u8, bit 0: activated -- 0x48B090 writes it    */
#define UA_ON        0x01u

#define W_VELOCITY   0x68        /* i32 16.16 world px a tick                     */
#define W_MINBARREL  0xC8        /* float radians                                 */
#define W_RANGE      0xDC        /* i32 world px                                  */
#define W_COVERAGE   0xE0        /* i32 world px: an interceptor's coverage       */
#define W_FLAGS      0x111       /* u32, the loader's bits (0x42E5E4..0x42EC05)   */
#define WF_BALLISTIC (1u << 1)
#define WF_PARALYZER (1u << 7)
#define WF_DROPPED   (1u << 8)
#define WF_WATER     (1u << 16)
#define WF_TOAIR     (1u << 17)
#define WF_CMDFIRE   (1u << 26)
#define WF_STOCKPILE (1u << 28)
#define WF_INTERCEPT (1u << 30)

#define VA_UNITINLOS 0x00465AC0u /* UnitInPlayerLOS(player, unit), stdcall, ret 8 */
#define VA_KEYDOWN   0x004C1B80u /* the engine's key state, injection-aware       */
enum { KEY_SHIFT = 0xF9, KEY_MENU = 0xFB };

/* BAR's thresholds (gui_attackrange_gl4.lua l.762-795) */
#define SEL_CAP      358         /* selectionDisableThreshold 512 x 0.7           */
#define LRPC_RANGE   2600
#define GROUND_MAX   700
#define RING_MAX     (TS_RTOGGLE_RINGS)   /* rings one type can have           */

typedef int (__stdcall* LosFn)(void* player, void* unit);
typedef int (__stdcall* KeyDown)(int);

typedef struct {
    uint8_t cls, style, ballistic;
    uint16_t range;
    float v, minbarrel;
} RDEF;

typedef struct {
    char*    ta;
    char*    units;
    char*    defs;
    unsigned nslots, ndefs;
    unsigned viewer;
    char*    player;             /* the viewed player's record                   */
} World;

static int s_inPlay;
static unsigned s_cFill, s_cCut, s_cKey;

/* the per-type choice, keyed by unit name; loaded once, saved when changed */
static TsRangeToggle s_tog[TS_RTOGGLE_SAVED];
static int s_ntog, s_togLoaded, s_togDirty;

static uint32_t rd32(const char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t  rdi32(const char* p) { int32_t v; memcpy(&v, p, 4); return v; }
static float    rdf(const char* p) { float v; memcpy(&v, p, 4); return v; }

static int world(World* w)
{
    char* end;
    uintptr_t dist;
    memset(w, 0, sizeof *w);
    w->ta = *(char**)TA_MAIN_PP;
    if (!w->ta) return 0;
    w->viewer = *(unsigned char*)(w->ta + OFF_LOCALPLAYER);
    if (w->viewer >= PLAYER_COUNT) return 0;
    w->ndefs = rd32(w->ta + OFF_UDEFCOUNT);
    if (!w->ndefs || w->ndefs > TAGPU_LIM_TYPES) return 0;
    w->defs = *(char**)(w->ta + OFF_UNITDEFS);
    w->units = *(char**)(w->ta + OFF_UNIT_BEGIN);
    end = *(char**)(w->ta + OFF_UNIT_END);
    if (!w->defs || !w->units || (uintptr_t)end < (uintptr_t)w->units) return 0;
    dist = (uintptr_t)end - (uintptr_t)w->units;
    if (dist % UNIT_STRIDE || dist / UNIT_STRIDE >= TAGPU_PK_DESIGN_SLOTS) return 0;
    w->nslots = *(unsigned short*)(w->ta + OFF_UNITSLOTS);
    if (w->nslots != (unsigned)(dist / UNIT_STRIDE) + 1) return 0;
    w->player = w->ta + OFF_PLAYERS + w->viewer * PLAYER_STRIDE;
    return 1;
}

static char* unit_at(const World* w, unsigned slot)
{
    char* u;
    uint32_t st;
    if (!slot || slot >= w->nslots) return NULL;
    u = w->units + (size_t)slot * UNIT_STRIDE;
    st = rd32(u + U_STATE);
    if (!(st & ST_ALIVE) || (st & ST_EXCLUDED)) return NULL;
    return u;
}

/* the unit's type, or NULL when its def pointer is not a row of the table */
static char* unit_def(const World* w, const char* u)
{
    char* d = *(char* const*)(u + U_TYPE);
    uintptr_t off;
    if ((uintptr_t)d < (uintptr_t)w->defs) return NULL;
    off = (uintptr_t)d - (uintptr_t)w->defs;
    if (off % UDEF_STRIDE || off / UDEF_STRIDE >= w->ndefs) return NULL;
    return d;
}

static int owner_of(const char* u) { return *(const unsigned char*)(u + U_OWNER); }

static int allied(const World* w, int owner)
{
    if (owner == (int)w->viewer) return 1;
    if (owner < 0 || owner > PLAYER_COUNT) return 0;
    return *(const unsigned char*)(w->player + PL_ALLY + owner) == 1;
}

/* what the engine shows the viewed player: its own units, and every other one
   UnitInPlayerLOS passes -- the test HotUnits takes (0x48BC42..0x48BC56) */
static int shown(const World* w, char* u)
{
    if (owner_of(u) == (int)w->viewer) return 1;
    return ((LosFn)VA_UNITINLOS)(w->player, u) != 0;
}

/* ---- a type's rings -------------------------------------------------------
   BAR's classes on TA's weapon flags: anti-air is toairweapon; a ballistic
   weapon is a cannon, lrpc past 2600; anything else ground, lrpc past 2600.
   Sub-styles: paralyzer emp, a command-fire weapon not stockpiled (the D-gun)
   dgun, waterweapon water. Identical rings collapse. */
static int weapon_of(const char* def, unsigned i, const char** wp)
{
    const char* p = tagpu_weapons_type_weapon(def, i);
    if (!p || tagpu_limits_weapon_index(p) <= 0) return 0;
    *wp = p;
    return 1;
}

static int attack_class(uint32_t f, int range)
{
    if (f & WF_TOAIR) return TAGPU_PK_RC_AA;
    if (f & WF_BALLISTIC) return range > LRPC_RANGE ? TAGPU_PK_RC_LRPC : TAGPU_PK_RC_CANNON;
    if (range < GROUND_MAX) return TAGPU_PK_RC_GROUND;
    return range > LRPC_RANGE ? TAGPU_PK_RC_LRPC : TAGPU_PK_RC_GROUND;
}

static int add_ring(RDEF* out, int n, int cls, int style, int range, int ballistic,
                    const char* wp)
{
    RDEF r;
    int k;
    if (n >= RING_MAX || range <= 0) return n;
    memset(&r, 0, sizeof r);
    r.cls = (uint8_t)cls; r.style = (uint8_t)style;
    r.range = (uint16_t)(range > TAGPU_PK_RING_RMAX ? TAGPU_PK_RING_RMAX : range);
    if (ballistic && wp) {
        float v = (float)rdi32(wp + W_VELOCITY) / 65536.0f, mb = rdf(wp + W_MINBARREL);
        /* a velocity the solver cannot use reaches nothing: the engine refuses
           every target of it, so no ring */
        if (!(v > 0.0f && v <= TAGPU_PK_RING_VMAX)) return n;
        if (!(mb >= -3.2f && mb <= 3.2f)) mb = 0.0f;
        r.ballistic = 1; r.v = v; r.minbarrel = mb;
    }
    for (k = 0; k < n; k++)
        if (out[k].cls == r.cls && out[k].style == r.style && out[k].range == r.range &&
            out[k].ballistic == r.ballistic && out[k].v == r.v && out[k].minbarrel == r.minbarrel)
            return n;
    out[n] = r;
    return n + 1;
}

static int attack_rings(const char* def, RDEF* out)
{
    unsigned i;
    int n = 0, bd;
    for (i = 0; i < TS_RTOGGLE_RINGS; i++) {
        const char* wp;
        uint32_t f;
        int range, style = TAGPU_PK_RS_NONE;
        if (!tagpu_weapons_type_weapon(def, i)) break;
        if (!weapon_of(def, i, &wp)) continue;
        f = rd32(wp + W_FLAGS);
        range = rdi32(wp + W_RANGE);
        if ((f & (WF_DROPPED | WF_INTERCEPT)) || range <= 0) continue;
        if (f & WF_PARALYZER) style = TAGPU_PK_RS_EMP;
        else if ((f & WF_CMDFIRE) && !(f & WF_STOCKPILE)) style = TAGPU_PK_RS_DGUN;
        else if (f & WF_WATER) style = TAGPU_PK_RS_WATER;
        n = add_ring(out, n, attack_class(f, range), style, range, (f & WF_BALLISTIC) != 0, wp);
    }
    bd = *(const unsigned short*)(def + UD_BUILDDIST);
    if ((rd32(def + UD_TYPEMASK) & UD_BUILDER) && bd > 0)
        n = add_ring(out, n, TAGPU_PK_RC_NANO, TAGPU_PK_RS_NONE, bd, 0, NULL);
    return n;
}

/* the defense widget's: an interceptor is a nuke ring at its coverage,
   anti-air an air ring, anything else the attack rule's ground, cannon or
   lrpc; only for a type that cannot move */
static int defense_rings(const char* def, RDEF* out)
{
    unsigned i;
    int n = 0;
    if (*(const unsigned char*)(def + UD_BMCODE) != 0) return 0;
    for (i = 0; i < TS_RTOGGLE_RINGS; i++) {
        const char* wp;
        uint32_t f;
        int cls, range;
        if (!tagpu_weapons_type_weapon(def, i)) break;
        if (!weapon_of(def, i, &wp)) continue;
        f = rd32(wp + W_FLAGS);
        if (f & WF_DROPPED) continue;
        range = rdi32(wp + W_RANGE);
        cls = (f & WF_INTERCEPT) ? TAGPU_PK_RC_NUKE
            : (f & WF_TOAIR) ? TAGPU_PK_RC_AIR : attack_class(f, range);
        if (cls == TAGPU_PK_RC_NUKE) range = rdi32(wp + W_COVERAGE);
        n = add_ring(out, n, cls, (f & WF_WATER) ? TAGPU_PK_RS_WATER : TAGPU_PK_RS_NONE, range,
                     cls != TAGPU_PK_RC_NUKE && (f & WF_BALLISTIC), wp);
    }
    return n;
}

/* ---- the sensors ---------------------------------------------------------
   The coverage the radar pass 0x467440 tests, in its four kinds. A radar or
   sonar emitter is one of the viewed player's own units, alive, not dying
   (`unit_at`) and activated (U_ACTIVATION bit 0), whose def has the kind's
   distance above 0 (0x467521..0x467542). The preview counts the player's own
   jammers by the same test, which is BAR's meaning and not the pass's: the
   jammer loop 0x4675EC..0x46766C walks every OTHER owner's activated jammers
   (0x4675F5..0x467600), so the player's own jam only the other players'
   passes. Activation is the engine's state, not the type's: a radar
   switched off emits nothing, and a factory emits while its build order runs
   (BuildingBuild 0x402640 sets it at 0x4027AE). The coverage is a plain 2D
   circle -- no terrain hides anything (0x47E890's test is dx^2 + dz^2). */
static const unsigned SENSOR_OFF[4] = { UD_RADAR, UD_SONAR, UD_RADARJAM, UD_SONARJAM };
static const uint8_t  SENSOR_CLS[4] = { TAGPU_PK_RC_RADAR, TAGPU_PK_RC_SONAR,
                                        TAGPU_PK_RC_RADARJAM, TAGPU_PK_RC_SONARJAM };

static int def_s16(const char* def, unsigned off)
{
    int16_t v;
    memcpy(&v, def + off, 2);
    return v;
}

/* The radius sensor `k` of `def` covers at altitude `alt`, 0 for none. Radar
   contacts at d^2 < (RadarDistance + 2 alt)^2, but the bucket walk that offers
   the targets reaches max(radar, sonar) (0x46755E..0x4675A3), so the bonus
   survives only past a larger sonar. */
static int sensor_radius(const char* def, int k, int alt)
{
    int v = def_s16(def, SENSOR_OFF[k]);
    if (v <= 0) return 0;
    if (k == 0) {
        int sonar = def_s16(def, UD_SONAR), walk = v > sonar ? v : sonar;
        v += 2 * alt;
        if (v < 0) v = -v;
        if (v > walk) v = walk;
        if (v <= 0) return 0;
    }
    return v > TAGPU_PK_RING_RMAX ? TAGPU_PK_RING_RMAX : v;
}

/* a building being placed will emit once built when its type switches on at
   completion or the player can switch it on: the two ways 0x48B090 is reached
   for a structure that is not a factory */
static int will_emit(const char* def)
{
    return (rd32(def + UD_TYPEMASK) & UD_AWB) || (rd32(def + UD_FLAGS2) & UD_ONOFF);
}

static int def_class_bit(int cls)
{
    switch (cls) {
    case TAGPU_PK_RC_AIR:    return TS_RC_AIR;
    case TAGPU_PK_RC_NUKE:   return TS_RC_NUKE;
    case TAGPU_PK_RC_CANNON: return TS_RC_CANNON;
    case TAGPU_PK_RC_LRPC:   return TS_RC_LRPC;
    default:                 return TS_RC_GROUND;
    }
}

/* ---- the per-type choice ------------------------------------------------ */
static void tog_load(void)
{
    if (s_togLoaded) return;
    s_ntog = tagpu_settings_rtoggle_load(s_tog, TS_RTOGGLE_SAVED);
    if (s_ntog < 0) s_ntog = 0;
    s_togLoaded = 1;
}

/* the def's name, bounded: the inline field is not trusted to end */
static void def_name(const char* def, char out[TS_SPACING_NAME])
{
    memset(out, 0, TS_SPACING_NAME);
    memcpy(out, def + UD_NAME, TS_SPACING_NAME - 1);
}

static TsRangeToggle* tog_find(const char* def)
{
    char name[TS_SPACING_NAME];
    int i;
    def_name(def, name);
    for (i = 0; i < s_ntog; i++)
        if (!lstrcmpiA(s_tog[i].name, name)) return &s_tog[i];
    return NULL;
}

/* 1 when ring k of n shows on this side, under the type's saved choice `t` */
static int ring_on(const TsRangeToggle* t, int ally, int k, int n)
{
    if (!t || t->n != n) return 1;
    return ((ally ? t->ally : t->enemy) >> k) & 1;
}

/* ---- the fill ------------------------------------------------------------ */
typedef struct {
    TAGPU_PK_RING* out;
    unsigned max, n;
    int cut;
} Sink;

static void emit(Sink* s, unsigned slot, const int32_t pos[3], const RDEF* r, int layer)
{
    TAGPU_PK_RING* e;
    int c;
    if (s->n >= s->max) { s->cut = 1; return; }
    e = &s->out[s->n++];
    memset(e, 0, sizeof *e);
    e->uidx = 0xFFFFu;
    e->slot = (uint16_t)slot;
    for (c = 0; c < 3; c++) {
        int32_t v = pos[c];
        if (v < -TAGPU_PK_PLACE_COORD) v = -TAGPU_PK_PLACE_COORD;
        if (v > TAGPU_PK_PLACE_COORD) v = TAGPU_PK_PLACE_COORD;
        e->pos[c] = v;
    }
    e->v = r->ballistic ? r->v : 0.0f;
    e->minbarrel = r->ballistic ? r->minbarrel : 0.0f;
    e->range = r->range;
    e->cls = r->cls;
    e->layer = (uint8_t)layer;
    e->style = r->style;
    e->flags = r->ballistic ? TAGPU_PK_RF_BALLISTIC : 0;
}

static void unit_pos(const char* u, int32_t pos[3])
{
    pos[0] = rdi32(u + U_XFIX) >> 16;
    pos[1] = rdi32(u + U_ZFIX) >> 16;
    pos[2] = rdi32(u + U_YFIX) >> 16;
}

static void emit_sensor(Sink* s, unsigned slot, const int32_t pos[3], int k, int radius, int isNew)
{
    RDEF r;
    unsigned before = s->n;
    memset(&r, 0, sizeof r);
    r.cls = SENSOR_CLS[k];
    r.style = TAGPU_PK_RS_NONE;
    r.range = (uint16_t)radius;
    emit(s, slot, pos, &r, TAGPU_PK_RL_SENSOR);
    if (isNew && s->n > before) s->out[before].flags |= TAGPU_PK_RF_NEW;
}

/* a unit's attack rings into its layer, the per-type choice applied */
static void emit_attack(Sink* s, const World* w, char* u, unsigned slot, const char* def,
                        int nanoOnly, unsigned* nbuild)
{
    RDEF r[RING_MAX];
    int32_t pos[3];
    int n = attack_rings(def, r), k, ally = allied(w, owner_of(u));
    const TsRangeToggle* t = n && !nanoOnly ? tog_find(def) : NULL;
    unit_pos(u, pos);
    for (k = 0; k < n; k++) {
        if (nanoOnly && r[k].cls != TAGPU_PK_RC_NANO) continue;
        if (!nanoOnly && !ring_on(t, ally, k, n)) continue;
        emit(s, slot, pos, &r[k], ally ? TAGPU_PK_RL_ATK_ALLY : TAGPU_PK_RL_ATK_ENEMY);
        if (ally && r[k].cls == TAGPU_PK_RC_NANO) (*nbuild)++;
    }
}

static int setting(TagpuSetting k, int def)
{
    int v;
    return tagpu_settings_get(k, &v) ? v : def;
}

/* THE LANE DRAWS RINGS: the Vulkan backend, until its range pass refuses.
   `g_ddraw.renderer` is stored before the first draw and once more by the
   hand-over to GDI, after which no frame is drawn from a packet
   (tagpu_packet_pub.c, wide_tick). `s_refused` is a one-way latch the render
   thread sets and clears only in the pass's teardown; what a draw reads of it
   decides which picture the markers draw, never what memory is read. */
static volatile LONG s_refused;

static int lane_draws(void)
{
    return g_ddraw.renderer == vk_render_main && !InterlockedCompareExchange(&s_refused, 0, 0);
}

void tagpu_rangeown_refuse(void)  { InterlockedExchange(&s_refused, 1); }
void tagpu_rangeown_forgive(void) { InterlockedExchange(&s_refused, 0); }

int tagpu_rangeown_on(void) { return setting(TS_RANGES, 1) != 0 && lane_draws(); }

int tagpu_rangeown_fill(TAGPU_PK_RANGE* h, TAGPU_PK_RING* out, unsigned max,
                        unsigned* n, int* cut)
{
    World w;
    Sink s;
    unsigned i, nsel = 0, nbuild = 0, hover;
    int on, shiftOnly, cursor, sensor, wantAlly, wantEnemy, showAttack, placing, region;
    int overrideBits = 0, sensorKinds = 0;
    int32_t g;
    char* ghostDef = NULL;
    int32_t ghost[3] = { 0, 0, 0 };

    *n = 0; *cut = 0;
    memset(h, 0, sizeof *h);
    if (!world(&w) || !lane_draws()) return 0;
    s_inPlay = 1;
    s_cFill++;
    tog_load();
    if (s_togDirty && tagpu_settings_put_rtoggle(s_tog, s_ntog, 0)) s_togDirty = 0;

    on        = setting(TS_RANGES, 1);
    shiftOnly = setting(TS_RANGESHIFT, 0);
    cursor    = setting(TS_RANGECURSOR, 1);
    sensor    = setting(TS_RANGESENSOR, 1);
    wantAlly  = setting(TS_RANGEALLY, TS_RC_NUKE);
    wantEnemy = setting(TS_RANGEENEMY, TS_RC_GROUND | TS_RC_AIR | TS_RC_NUKE | TS_RC_CANNON);
    if (!on) return 0;
    g = rdi32(w.ta + M_GRAVITY);
    if (g <= 0 || g > 65536) return 0;

    s.out = out; s.max = max; s.n = 0; s.cut = 0;
    region = *(const unsigned char*)(w.ta + M_REGION);
    placing = *(const unsigned char*)(w.ta + OFF_CURMODE) == MODE_BUILD;
    showAttack = !shiftOnly || ((KeyDown)VA_KEYDOWN)(KEY_SHIFT) != 0;

    /* the building being placed: its defense rings at the engine's own snapped
       site (0x4197D0 writes the rect only while the pointer is on the world) */
    if (placing && (region & 2)) {
        unsigned t = *(const unsigned short*)(w.ta + OFF_BUILDUNITID);
        if (t && t < w.ndefs) {
            const int32_t* rc = (const int32_t*)(w.ta + OFF_BUILDRECT);
            ghostDef = w.defs + (size_t)t * UDEF_STRIDE;
            ghost[0] = (rdi32((const char*)&rc[0]) + rdi32((const char*)&rc[3])) / 2;
            ghost[1] = rdi32((const char*)&rc[1]);
            ghost[2] = (rdi32((const char*)&rc[2]) + rdi32((const char*)&rc[5])) / 2;
        }
    }

    /* the defense layers, under the attack layers: allies first */
    if (ghostDef) {
        RDEF r[RING_MAX];
        int k, nr = defense_rings(ghostDef, r);
        for (k = 0; k < nr; k++) {
            overrideBits |= def_class_bit(r[k].cls);
            emit(&s, 0, ghost, &r[k], TAGPU_PK_RL_DEF_ALLY);
        }
        /* its sensors, before every other sensor ring: the draw lays the new
           ring's sheet first, and the existing coverage fills only around it */
        if (sensor && will_emit(ghostDef))
            for (k = 0; k < 4; k++) {
                int rad = sensor_radius(ghostDef, k, ghost[1]);
                if (!rad) continue;
                sensorKinds |= 1 << k;
                emit_sensor(&s, 0, ghost, k, rad, 1);
            }
    }
    for (i = 1; i < w.nslots; i++) {
        char* u = unit_at(&w, i);
        char* def;
        RDEF r[RING_MAX];
        int32_t pos[3];
        int k, nr, ally, want;
        if (!u || !(rd32(u + U_STATE) & ST_STRUCT)) continue;
        if (!(def = unit_def(&w, u))) continue;
        ally = allied(&w, owner_of(u));
        want = ally ? (wantAlly | overrideBits) : wantEnemy;
        if (!want) continue;
        nr = defense_rings(def, r);
        if (!nr || !shown(&w, u)) continue;
        unit_pos(u, pos);
        for (k = 0; k < nr; k++)
            if (want & def_class_bit(r[k].cls))
                emit(&s, i, pos, &r[k], ally ? TAGPU_PK_RL_DEF_ALLY : TAGPU_PK_RL_DEF_ENEMY);
    }

    if (showAttack) {
        for (i = 1; i < w.nslots && nsel < SEL_CAP; i++) {
            char* u = unit_at(&w, i);
            char* def;
            if (!u || !(rd32(u + U_STATE) & ST_SELECTED) || !(def = unit_def(&w, u))) continue;
            if (!shown(&w, u)) continue;
            nsel++;
            emit_attack(&s, &w, u, i, def, 0, &nbuild);
        }
        /* the unit under the cursor. The engine does not refresh the hover in
           build placement (0x499241 jumps past the query), so none there */
        hover = *(const unsigned short*)(w.ta + M_HOVER);
        if (cursor && !placing && hover) {
            char* u = unit_at(&w, hover);
            char* def;
            if (u && !(rd32(u + U_STATE) & ST_SELECTED) && (def = unit_def(&w, u)) && shown(&w, u))
                emit_attack(&s, &w, u, hover, def, 0, &nbuild);
        }
        /* a build command is active: every builder of the player's own */
        if (placing) {
            for (i = 1; i < w.nslots; i++) {
                char* u = unit_at(&w, i);
                char* def;
                if (!u || owner_of(u) != (int)w.viewer) continue;
                if (rd32(u + U_STATE) & ST_SELECTED) continue;
                if (!(def = unit_def(&w, u)) || !(rd32(def + UD_TYPEMASK) & UD_BUILDER)) continue;
                emit_attack(&s, &w, u, i, def, 1, &nbuild);
            }
        }
    }
    /* the existing coverage of the placed building's kinds, last: of everything
       here it is what a full table can best do without */
    if (sensorKinds) {
        for (i = 1; i < w.nslots; i++) {
            char* u = unit_at(&w, i);
            char* def;
            int32_t pos[3];
            int k;
            if (!u || owner_of(u) != (int)w.viewer) continue;
            if (!(*(const unsigned char*)(u + U_ACTIVATION) & UA_ON)) continue;
            if (!(def = unit_def(&w, u))) continue;
            unit_pos(u, pos);
            for (k = 0; k < 4; k++) {
                int rad;
                if (!(sensorKinds & (1 << k))) continue;
                if ((rad = sensor_radius(def, k, pos[1])) != 0) emit_sensor(&s, i, pos, k, rad, 0);
            }
        }
    }
    if (!s.n) return 0;

    h->nsel = (uint16_t)nsel;
    h->nbuild = (uint16_t)(nbuild > 0xFFFF ? 0xFFFF : nbuild);
    h->grav = (float)g / 65536.0f;
    if (region & 2) {
        int c;
        h->flags |= TAGPU_PK_RG_MOUSE;
        for (c = 0; c < 3; c++) {
            int32_t v = rdi32(w.ta + M_POINT + 4 * c) >> 16;
            if (v < -TAGPU_PK_PLACE_COORD) v = -TAGPU_PK_PLACE_COORD;
            if (v > TAGPU_PK_PLACE_COORD) v = TAGPU_PK_PLACE_COORD;
            h->mouse[c] = v;
        }
    }
    *n = s.n;
    *cut = s.cut;
    if (s.cut) s_cCut++;
    return 1;
}

void tagpu_rangeown_level_end(void)
{
    s_inPlay = 0;
    /* outside play: a choice still pending may wait for the store's lock here */
    if (s_togDirty && tagpu_settings_put_rtoggle(s_tog, s_ntog, 1)) s_togDirty = 0;
}

/* ---- alt+. / alt+, --------------------------------------------------------
   BAR's cycleUnitDisplay: with ONE unit selected, its type's rings on this
   side as a bitmap, plus or minus one modulo 2^n. */
int tagpu_rangeown_key(unsigned vk, int alt_known)
{
    World w;
    TsRangeToggle* t;
    RDEF r[RING_MAX];
    char* one = NULL;
    char* def;
    unsigned i, nsel = 0, mask;
    int n, bits, ally, dir;
    if (vk != VK_OEM_PERIOD && vk != VK_OEM_COMMA) return 0;
    if (!s_inPlay) return 0;
    if (!alt_known && !((KeyDown)VA_KEYDOWN)(KEY_MENU)) return 0;
    if (!world(&w) || !tagpu_rangeown_on()) return 0;
    s_cKey++;
    dir = vk == VK_OEM_PERIOD ? 1 : -1;
    for (i = 1; i < w.nslots; i++) {
        char* u = unit_at(&w, i);
        if (!u || !(rd32(u + U_STATE) & ST_SELECTED)) continue;
        if (++nsel > 1) break;
        one = u;
    }
    if (nsel != 1 || !(def = unit_def(&w, one))) {
        tagpu_logf("range: alt+%c needs exactly one selected unit", dir > 0 ? '.' : ',');
        return 1;
    }
    n = attack_rings(def, r);
    if (!n) {
        tagpu_logf("range: %.32s has no weapon range", def + UD_NAME);
        return 1;
    }
    ally = allied(&w, owner_of(one));
    tog_load();
    t = tog_find(def);
    mask = (1u << n) - 1u;
    bits = t && t->n == n ? (ally ? t->ally : t->enemy) : (int)mask;
    bits = (int)(((unsigned)bits + (unsigned)(dir + (int)mask + 1)) & mask);
    if (!t) {
        char name[TS_SPACING_NAME];
        def_name(def, name);
        if (s_ntog >= TS_RTOGGLE_SAVED || !tagpu_settings_spacing_name_ok(name, strlen(name))) {
            tagpu_logf("range: %.32s's ring choice is not kept: the table is full or the name "
                       "is not one impure.cfg keeps", name);
            return 1;
        }
        t = &s_tog[s_ntog++];
        memset(t, 0, sizeof *t);
        memcpy(t->name, name, sizeof name);
        t->n = n; t->ally = (int)mask; t->enemy = (int)mask;
    }
    if (t->n != n) { t->n = n; t->ally = (int)mask; t->enemy = (int)mask; }
    if (ally) t->ally = bits; else t->enemy = bits;
    /* every ring on, on both sides, is the default: the entry goes */
    if (t->ally == (int)mask && t->enemy == (int)mask) {
        int at = (int)(t - s_tog);
        memmove(&s_tog[at], &s_tog[at + 1], (size_t)(s_ntog - at - 1) * sizeof *s_tog);
        s_ntog--;
    }
    s_togDirty = 1;
    tagpu_logf("range: %.32s (%s) shows ring config %d of %u", def + UD_NAME,
               ally ? "ally" : "enemy", bits, mask + 1u);
    return 1;
}
