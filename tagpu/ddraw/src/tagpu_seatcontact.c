#include "tagpu_simreg.h"
SIM_FEATURE(seat_contacts, SIM_PUPPET_RUNS, "battle battle-allied battle-los stock-battle-allied naval-push b7-radar-hail tenseat");
/* Every seat targets with its own contacts and its allies', never the viewed player's.

   THE ENGINE. The periodic acquisition 0x40AA40 (thiscall on a seat's acquisition record, plain
   `ret`; its only calls are 0x40AD3E and 0x40B2E9, and no pointer to it is in the image) lists, for
   one seat, the enemy units it may target. Its radar list takes a unit whose +0x110 bit 8 is set
   (0x40AB3F) and its sight list asks UnitInPlayerLOS 0x465AC0(seat, unit) (0x40AB11), which
   passes a submerged unit on bit 9 (0x465B30). Bits 8 and 9 are written only by the radar pass
   0x467440, and that pass runs for the VIEWED player alone (its one call 0x46556D, in the viewed
   record's iteration of the players phase), so stock gives every seat the viewed player's
   contacts: an AI seat targets what the human's radar sees, and an AI's own radar towers give it
   nothing [DISASSEMBLED 2026-11-15].

   THE FIX. Each acquisition computes its own seat's contacts at its start, by the pass's own rules
   and with the engine's own bucket walk 0x47E890, into the four bitmaps below, which the two reads
   then take instead of the bits:
     radar(s, u)  a radar or sonar unit of s or of an owner whose contacts s receives
                  (tagpu_allysight_contacts) -- alive, not dying (+0x110 bit 14), activated
                  (+0x10E bit 0), def +0x204 R or +0x206 S non-zero -- whose walk over max(R, S)
                  admits u, u not stealthy (def +0x241 bit 8), u's top (+0x6E plus def +0x16E) at
                  or above the sea and d^2 < (R + 2 * the emitter's height)^2   [0x46750E, 0x467840]
     sonar(s, u)  the same walk, u's +0x6E at or below the sea and d^2 < S^2
     jam(s, u)    a live, activated unit of an owner neither s nor one whose contacts s receives,
                  with def +0x20A (radar) or +0x20C (sonar) non-zero, whose walk admits u
                                                                           [0x4675EC, 0x467960/80]
     sight(s, u)  u alive and not cloaked (+0x10E bit 2), its origin cell lit for s: s's counters
                  under true LOS, else s's bit of the explored map, the row taken as the shear fix
                  takes it (tagpu_patches.c, LOS_VIEWMAP)                       [0x46770D..0x467829]
   contact = (radar and not radar-jammed) or sight -- the pass's loop order: jam clears bit 8 after
   radar sets it and sight sets it after both. A sonar contact = sonar and not sonar-jammed. The
   distance d^2 is the walk's: (dx*dx >> 32) + (dz*dz >> 32) on the 16.16 positions.

   WHAT IT CHANGES. A seat's contacts are its own and its allies', at the moment it acquires rather
   than as of the viewed player's last 30-tick pass; the viewed player's seat is no exception, so
   nothing the simulation reads depends on who is viewed. Presentation keeps the bits: the minimap,
   the HotUnits cull and every other UnitInPlayerLOS caller (0x439761, 0x46AF3D, 0x46B5B4, 0x480EE5,
   0x48BC56, 0x494396) read them as stock, since the sonar gate answers per seat only for the seat
   whose acquisition is running.

   NOT CHANGED, the gaps: under Permanent or Circular LOS, UnitInPlayerLOS's explored-map reads
   (0x4080E0 inside 0x408090, 0x465E00) still take the viewed player's bit, so an acquisition there
   sees what the viewed player has explored; the watcher rule of the pass's loop 1 (PlayerInfo
   +0x9B bit 6) is not carried over, a watching seat having no acquisition record.

   STATE. The bitmaps and the scope are SIM_TRANSIENT: written at an acquisition's start, read only
   inside it, the scope cleared at its end, so nothing passes from one tick to the next.

   THREADS. The game thread: the acquisition runs in the unit step, the API is for the game thread
   too. */

#include <string.h>

#include "tagpu_allysight.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "tagpu_seatcontact.h"

#define TA_MAIN        0x00511DE8u
#define OFF_PLAYERS    0x1B63
#define PL_STRIDE      0x14B
#define PL_FIRST       0x67        /* the record's unit block, first and last, inclusive          */
#define PL_LAST        0x6B
#define PL_SIGHT       0x7C        /* u8* the sight counters                                      */
#define PL_COLS        0x80
#define PL_ROWS        0x84
#define PL_CELLS       0x88        /* the counters' length                                        */
#define PL_MAPBIT      0x146       /* the record's bit in the explored map                        */
#define OFF_MAPPED     0x14273     /* u16* the explored map, one bit per player                   */
#define OFF_SEA        0x1427F     /* u8 the sea level                                            */
#define OFF_LOSTYPE    0x14281     /* bit 1: true LOS                                             */
#define OFF_UNITS      0x14357     /* the unit array's first and last slot                        */
#define OFF_UNITS_LAST 0x1435B
#define U_STRIDE       0x118
#define U_X            0x6A        /* 16.16; the high words +0x6C, +0x70, +0x74                    */
#define U_Y            0x6E
#define U_Z            0x72
#define U_DEF          0x92
#define U_OWNER        0x96
#define U_TYPE         0xA6
#define U_STATE        0x10E       /* bit 0 activated, bit 2 cloaked                              */
#define U_OWNERBYTE    0xFF
#define U_FLAGS        0x110       /* bit 14 dying, bit 28 alive                                  */
#define D_TOP          0x16E
#define D_RADAR        0x204
#define D_SONAR        0x206
#define D_JAM_RADAR    0x20A
#define D_JAM_SONAR    0x20C
#define D_STEALTH      0x241       /* bit 8                                                       */
#define PLAYERS        10
#define SLOTS_MAX      0x10000u    /* the slot count is a u16 (main+0x14351)                      */

#define VA_ACQUIRE     0x0040AA40u
#define VA_WALK        0x0047E890u /* (pos*, r << 16, visitor*), stdcall ret 0xC; visits each unit
                                      of every bucket the square reaches whose d^2 <= r^2, calling
                                      the visitor's first virtual, thiscall(unit) ret 4           */
#define VA_UNITINLOS   0x00465AC0u /* UnitInPlayerLOS(player, unit), stdcall ret 8                */

typedef void (__thiscall* PFN_Acquire)(char* acq);
typedef void (__stdcall* PFN_Walk)(const int* pos, int r16, void* visitor);
typedef int (__stdcall* PFN_InLos)(char* player, char* unit);

enum { B_RADAR, B_SONAR, B_JAM_RADAR, B_JAM_SONAR, B_N };

static uint32_t s_bits[B_N][SLOTS_MAX / 32];
SIM_VAR(s_bits, SIM_TRANSIENT);
/* the seat whose bitmaps are built, and the slots they cover; rec NULL outside a scope */
static struct { char* rec; unsigned slots; } s_scope;
SIM_VAR(s_scope, SIM_TRANSIENT);

static int s_installed;
SIM_VAR(s_installed, SIM_INSTALL);

static char* ta_main(void) { return *(char* volatile*)(size_t)TA_MAIN; }
static int32_t rd32(const char* p) { int32_t v; memcpy(&v, p, 4); return v; }
static int16_t rd16(const char* p) { int16_t v; memcpy(&v, p, 2); return v; }
static char* rdptr(const char* p) { char* v; memcpy(&v, p, 4); return v; }

/* p's index among records 0..9, or PLAYERS when p is not exactly one of them */
static unsigned rec_index(char* ta, const char* p)
{
    char* base = ta + OFF_PLAYERS;
    if (p < base || p >= base + PLAYERS * PL_STRIDE || (size_t)(p - base) % PL_STRIDE) return PLAYERS;
    return (unsigned)((size_t)(p - base) / PL_STRIDE);
}

/* the unit array's slots and its first: data until bounded by SLOTS_MAX and the stride */
static unsigned slot_count(char* ta, char** begin)
{
    char* b = rdptr(ta + OFF_UNITS);
    char* e = rdptr(ta + OFF_UNITS_LAST);
    *begin = b;
    if (!b || e < b || (size_t)(e - b) % U_STRIDE) return 0;
    size_t n = (size_t)(e - b) / U_STRIDE + 1;
    return n > SLOTS_MAX ? 0 : (unsigned)n;
}

static unsigned slot_of(char* begin, unsigned n, const char* u)
{
    if (u < begin || (size_t)(u - begin) % U_STRIDE) return SLOTS_MAX;
    size_t s = (size_t)(u - begin) / U_STRIDE;
    return s < n ? (unsigned)s : SLOTS_MAX;
}

static int bit(int b, unsigned slot) { return s_bits[b][slot >> 5] >> (slot & 31) & 1; }
static void set_bit(int b, unsigned slot) { s_bits[b][slot >> 5] |= 1u << (slot & 31); }

/* the walk's and the pass's distance: 16.16 differences, each square's high dword */
static int32_t dist2(const char* u, const int pos[3])
{
    int32_t dx = (int32_t)((uint32_t)rd32(u + U_X) - (uint32_t)pos[0]);
    int32_t dz = (int32_t)((uint32_t)rd32(u + U_Z) - (uint32_t)pos[2]);
    return (int32_t)((uint32_t)(((int64_t)dx * dx) >> 32) + (uint32_t)(((int64_t)dz * dz) >> 32));
}

/* the walk's visitor: its first dword is the vtable the walk calls through */
typedef struct Visitor {
    void* const* vtbl;
    char* begin;
    unsigned slots;
    int pos[3];
    int32_t rt2, s2;                              /* (R + 2 * height)^2 and S^2, as 0x467560..  */
    int jam;                                      /* B_JAM_RADAR or B_JAM_SONAR for a jammer    */
    unsigned seatbit;
} Visitor;

/* 0x467840's rules for an emitter; 0x467960 / 0x467980's for a jammer (no test of its own) */
static void __thiscall visit(Visitor* v, char* u)
{
    char* ta = ta_main();
    unsigned slot = slot_of(v->begin, v->slots, u);
    if (slot >= v->slots) return;
    if (v->jam) { set_bit(v->jam, slot); return; }
    if (!rd16(u + U_TYPE) || (unsigned char)u[U_OWNERBYTE] == v->seatbit) return;
    char* def = rdptr(u + U_DEF);
    if (!def || (rd32(def + D_STEALTH) >> 8 & 1)) return;
    int32_t d2 = dist2(u, v->pos);
    int32_t y = rd32(u + U_Y);
    int32_t sea = (int32_t)((uint32_t)(unsigned char)ta[OFF_SEA] << 16);
    if (y <= sea && d2 < v->s2) set_bit(B_SONAR, slot);
    if (y + rd32(def + D_TOP) >= sea && d2 < v->rt2) set_bit(B_RADAR, slot);
}
static void* const s_visitVtbl[1] = { (void*)visit };

static void walk_from(Visitor* v, const char* u, int r)
{
    v->pos[0] = rd32(u + U_X);
    v->pos[1] = rd32(u + U_Y);
    v->pos[2] = rd32(u + U_Z);
    ((PFN_Walk)VA_WALK)(v->pos, (int)((uint32_t)r << 16), v);
}

/* the emitters of record o's block [+0x67, +0x6B], bounded to the array on its stride */
static void walk_block(char* ta, Visitor* v, unsigned o)
{
    char* r = ta + OFF_PLAYERS + (size_t)o * PL_STRIDE;
    char *first = rdptr(r + PL_FIRST), *last = rdptr(r + PL_LAST), *u;
    char* end = v->begin + (size_t)(v->slots - 1) * U_STRIDE;
    if (!first || first < v->begin || last > end || first > last ||
        (size_t)(first - v->begin) % U_STRIDE || (size_t)(last - v->begin) % U_STRIDE)
        return;
    for (u = first; u <= last; u += U_STRIDE) {
        uint32_t f = (uint32_t)rd32(u + U_FLAGS);
        char* def;
        int16_t R, S;
        if (!(f >> 28 & 1) || (f >> 14 & 1) || !(u[U_STATE] & 1)) continue;
        if (!(def = rdptr(u + U_DEF))) continue;
        R = rd16(def + D_RADAR);
        S = rd16(def + D_SONAR);
        if (!R && !S) continue;
        int32_t rt = (int32_t)R + 2 * (int32_t)rd16(u + U_Y + 2);
        v->rt2 = (int32_t)((uint32_t)rt * (uint32_t)rt);
        v->s2 = (int32_t)((uint32_t)((int32_t)S) * (uint32_t)((int32_t)S));
        walk_from(v, u, R > S ? R : S);
    }
}

/* every bitmap of seat s, at slots 0..slots-1 */
static void build(char* ta, char* rec, unsigned s, char* begin, unsigned slots)
{
    Visitor v;
    unsigned o, slot;
    memset(s_bits, 0, sizeof s_bits);
    memset(&v, 0, sizeof v);
    v.vtbl = s_visitVtbl;
    v.begin = begin;
    v.slots = slots;
    v.seatbit = (unsigned char)rec[PL_MAPBIT];
    walk_block(ta, &v, s);
    for (o = 0; o < PLAYERS; o++)
        if (tagpu_allysight_contacts(o, s)) walk_block(ta, &v, o);
    for (slot = 1; slot < slots; slot++) {             /* the jammer loop's slots 1..end      */
        char* u = begin + (size_t)slot * U_STRIDE;
        unsigned owner = (unsigned char)u[U_OWNERBYTE];
        char* def;
        if (!((uint32_t)rd32(u + U_FLAGS) >> 28 & 1) || owner == v.seatbit) continue;
        if (tagpu_allysight_contacts(owner, s) || !(u[U_STATE] & 1)) continue;
        if (!(def = rdptr(u + U_DEF))) continue;
        if (rd16(def + D_JAM_RADAR)) { v.jam = B_JAM_RADAR; walk_from(&v, u, rd16(def + D_JAM_RADAR)); }
        if (rd16(def + D_JAM_SONAR)) { v.jam = B_JAM_SONAR; walk_from(&v, u, rd16(def + D_JAM_SONAR)); }
    }
}

/* the pass's loop 5 for seat rec: the unit's origin cell lit, under the shear fix's row rule */
static int sight(char* ta, char* rec, const char* u)
{
    uint32_t cols = (uint32_t)rd32(rec + PL_COLS), rows = (uint32_t)rd32(rec + PL_ROWS);
    int32_t x = rd16(u + U_X + 2), y = rd16(u + U_Y + 2), z = rd16(u + U_Z + 2);
    uint32_t col = (uint32_t)(x >> 5), row = (uint32_t)((z - (y >> 1)) >> 5);
    if (row >= rows && (uint32_t)(z >> 5) < rows) row = (uint32_t)(z >> 5);
    if (col >= cols || row >= rows) return 0;
    if (rd16(ta + OFF_LOSTYPE) & 2) {
        const unsigned char* c = (const unsigned char*)rdptr(rec + PL_SIGHT);
        uint64_t i = (uint64_t)row * cols + col;
        return c && i < (uint32_t)rd32(rec + PL_CELLS) && c[i] != 0;
    } else {
        const uint16_t* m = (const uint16_t*)rdptr(ta + OFF_MAPPED);
        unsigned b = (unsigned char)rec[PL_MAPBIT];
        uint16_t w;
        if (!m || b >= 16) return 0;
        memcpy(&w, m + (size_t)row * cols + col, 2);
        return w >> b & 1;
    }
}

static int contact(char* ta, char* rec, unsigned slot, const char* u)
{
    if (bit(B_RADAR, slot) && !bit(B_JAM_RADAR, slot)) return 1;
    return !(u[U_STATE] & 4) && ((uint32_t)rd32(u + U_FLAGS) >> 28 & 1) && sight(ta, rec, u);
}

/* Opens a scope for the seat record rec (a record 0..9): its bitmaps built from the world as it
   stands. Returns 0 and opens nothing when rec or the unit array is not data. */
static int scope_open(char* ta, char* rec)
{
    char* begin;
    unsigned s = rec_index(ta, rec), slots = slot_count(ta, &begin);
    if (s >= PLAYERS || !slots) return 0;
    build(ta, rec, s, begin, slots);
    s_scope.rec = rec;
    s_scope.slots = slots;
    return 1;
}

/* the acquisition, from its two calls: its seat's contacts for its whole run */
static void __fastcall acquire_hook(char* acq)
{
    char* ta = ta_main();
    char* rec = acq ? rdptr(acq) : NULL;
    int open = ta && scope_open(ta, rec);
    ((PFN_Acquire)VA_ACQUIRE)(acq);
    if (open) s_scope.rec = NULL;
}

/* 0x40AB3F: whether the seat rec has the unit as a radar contact. Outside a scope (no acquisition
   reaches here but through acquire_hook) the seat's bitmaps are built for this one answer. */
static int __cdecl radar_at(char* rec, char* u)
{
    char* ta = ta_main();
    char* begin;
    unsigned slot, slots;
    int r;
    if (!ta) return 0;
    if (s_scope.rec == rec) {
        slots = slot_count(ta, &begin);
        slot = slot_of(begin, s_scope.slots < slots ? s_scope.slots : slots, u);
        return slot < SLOTS_MAX && contact(ta, rec, slot, u);
    }
    if (!scope_open(ta, rec)) return 0;
    slot = slot_of(rdptr(ta + OFF_UNITS), s_scope.slots, u);
    r = slot < SLOTS_MAX && contact(ta, rec, slot, u);
    s_scope.rec = NULL;
    return r;
}

/* 0x465B30: the sonar gate's answer -- per seat for the seat whose scope is open, else the
   unit's bit 9 as stock */
static int __cdecl sonar_gate(char* rec, char* u, int stockbit)
{
    char* ta = ta_main();
    char* begin;
    unsigned slot;
    if (!ta || !s_scope.rec || s_scope.rec != rec) return stockbit;
    slot_count(ta, &begin);
    slot = slot_of(begin, s_scope.slots, u);
    return slot < SLOTS_MAX && bit(B_SONAR, slot) && !bit(B_JAM_SONAR, slot);
}

unsigned tagpu_seatcontact_scan(unsigned seat, unsigned char* out, unsigned n)
{
    char* ta = ta_main();
    char *rec, *begin;
    unsigned slot, slots;
    if (!s_installed || !ta || seat >= PLAYERS || !out) return 0;
    if (s_scope.rec) return 0;                     /* inside an acquisition: not re-entrant     */
    rec = ta + OFF_PLAYERS + (size_t)seat * PL_STRIDE;
    if (!scope_open(ta, rec)) return 0;
    slots = s_scope.slots;
    begin = rdptr(ta + OFF_UNITS);
    if (n > slots) n = slots;
    for (slot = 0; slot < n; slot++) {
        char* u = begin + (size_t)slot * U_STRIDE;
        unsigned char f = 0;
        if ((uint32_t)rd32(u + U_FLAGS) >> 28 & 1 && rd16(u + U_TYPE)) {
            if (contact(ta, rec, slot, u)) f |= TAGPU_SEAT_CONTACT;
            if (bit(B_SONAR, slot) && !bit(B_JAM_SONAR, slot)) f |= TAGPU_SEAT_SONAR;
            if (((PFN_InLos)VA_UNITINLOS)(rec, u)) f |= TAGPU_SEAT_SEES;
        }
        out[slot] = f;
    }
    s_scope.rec = NULL;
    return n;
}

int tagpu_seatcontact_installed(void) { return s_installed; }

/* ---- install ------------------------------------------------------------------------------- */

#define VA_RADAR_READ 0x0040AB3Fu  /* mov eax,[ecx+0x110]; shr eax,8; test bl,al; je 0x40AC81    */
#define VA_SONAR_GATE 0x00465B30u  /* mov eax,[ebx+0x110]; add edi,ecx; test ah,2;
                                      mov [esp+0x18],edi; jne 0x465B5C                            */
static const unsigned char RADAR_WAS[17] = { 0x8B, 0x81, 0x10, 0x01, 0x00, 0x00, 0xC1, 0xE8, 0x08,
                                             0x84, 0xC3, 0x0F, 0x84, 0x31, 0x01, 0x00, 0x00 };
static const unsigned char SONAR_WAS[17] = { 0x8B, 0x83, 0x10, 0x01, 0x00, 0x00, 0x03, 0xF9, 0xF6,
                                             0xC4, 0x02, 0x89, 0x7C, 0x24, 0x18, 0x75, 0x1B };
static const unsigned s_calls[2] = { 0x0040AD3Eu, 0x0040B2E9u };

static unsigned char* rel(unsigned char* p, unsigned char op, unsigned target)
{
    *p++ = op;
    tagpu_detour_rel(p, target);
    return p + 4;
}

static unsigned char* jcc(unsigned char* p, unsigned char cc, unsigned target)
{
    *p++ = 0x0F;
    *p++ = cc;
    tagpu_detour_rel(p, target);
    return p + 4;
}

/* push ecx; push ecx; push dword [esi]; call radar_at; add esp,8; pop ecx; test eax,eax;
   je 0x40AC81; jmp 0x40AB50 -- ecx the unit (the skip path 0x40AC81 steps it), [esi] the seat's
   record; eax and edx dead on both exits (0x40AB50 and 0x40AC81 reload them), flags too */
static void stub_radar(unsigned char* p)
{
    *p++ = 0x51; *p++ = 0x51; *p++ = 0xFF; *p++ = 0x36;
    p = rel(p, 0xE8, (unsigned)(size_t)radar_at);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x59;
    *p++ = 0x85; *p++ = 0xC0;
    p = jcc(p, 0x84, 0x0040AC81u);
    rel(p, 0xE9, 0x0040AB50u);
}

/* stock's three effects (eax the flags, edi += ecx, [esp+0x18] = edi), then push ecx; push edx;
   shr eax,9; and eax,1; push eax; push ebx; push esi; call sonar_gate; add esp,0xC; pop edx;
   pop ecx; test eax,eax; jne 0x465B5C; jmp 0x465B41 -- esi the player, ebx the unit; edx (the
   unit's top, compared at 0x465B4C) and ecx kept; eax dead on both exits (0x465B41 zeroes it, the
   0x465B5C path writes it before reading it) */
static void stub_sonar(unsigned char* p)
{
    *p++ = 0x8B; *p++ = 0x83; *p++ = 0x10; *p++ = 0x01; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x03; *p++ = 0xF9;
    *p++ = 0x89; *p++ = 0x7C; *p++ = 0x24; *p++ = 0x18;
    *p++ = 0x51; *p++ = 0x52;
    *p++ = 0xC1; *p++ = 0xE8; *p++ = 0x09;
    *p++ = 0x83; *p++ = 0xE0; *p++ = 0x01;
    *p++ = 0x50; *p++ = 0x53; *p++ = 0x56;
    p = rel(p, 0xE8, (unsigned)(size_t)sonar_gate);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x0C;
    *p++ = 0x5A; *p++ = 0x59;
    *p++ = 0x85; *p++ = 0xC0;
    p = jcc(p, 0x85, 0x00465B5Cu);
    rel(p, 0xE9, 0x00465B41u);
}

static void site_jmp(unsigned va, const unsigned char* stub, unsigned char* b, int n)
{
    unsigned r = (unsigned)(size_t)stub - (va + 5);
    b[0] = 0xE9;
    memcpy(b + 1, &r, 4);
    memset(b + 5, 0x90, (size_t)(n - 5));
}

/* Both reads and both calls byte-matched before any is written, all or none, each read's range
   entered by no branch but at its first byte (0x40AB1C and 0x40AB27 -> 0x40AB3F; the only other
   jump near the gate, 0x465B4E -> 0x465B5C, lands past it). No other module writes any of them
   (hook-map.md). At DllMain, before the engine runs. */
void tagpu_seatcontact_init(void)
{
    unsigned char *sr = NULL, *ss = NULL, b[17];
    int i;
    if (memcmp((const void*)(size_t)VA_RADAR_READ, RADAR_WAS, 17) ||
        memcmp((const void*)(size_t)VA_SONAR_GATE, SONAR_WAS, 17) ||
        !tagpu_detour_redirect_ok(s_calls[0], VA_ACQUIRE) ||
        !tagpu_detour_redirect_ok(s_calls[1], VA_ACQUIRE)) {
        tagpu_log("seatcontact: NOT installed: a site does not hold the retail bytes; every seat "
                  "targets with the viewed player's contacts");
        return;
    }
    if (!(sr = tagpu_detour_stub()) || !(ss = tagpu_detour_stub())) {
        tagpu_detour_stub_free(sr);
        tagpu_detour_stub_free(ss);
        tagpu_log("seatcontact: NOT installed: no stub memory");
        return;
    }
    stub_radar(sr);
    stub_sonar(ss);
    site_jmp(VA_RADAR_READ, sr, b, 17);
    if (!tagpu_detour_write(VA_RADAR_READ, b, 17)) goto fail0;
    site_jmp(VA_SONAR_GATE, ss, b, 17);
    if (!tagpu_detour_write(VA_SONAR_GATE, b, 17)) goto fail1;
    for (i = 0; i < 2; i++)
        if (!tagpu_detour_redirect(s_calls[i], VA_ACQUIRE, (void*)acquire_hook)) goto fail2;
    s_installed = 1;
    tagpu_log("seatcontact: installed: the acquisition's calls 0x40AD3E 0x40B2E9, its radar read "
              "0x40AB3F and UnitInPlayerLOS's sonar gate 0x465B30 -- each seat targets with its "
              "own and its allies' contacts");
    return;
fail2:
    while (--i >= 0) tagpu_detour_redirect(s_calls[i], (unsigned)(size_t)acquire_hook,
                                           (void*)(size_t)VA_ACQUIRE);
    tagpu_detour_write(VA_SONAR_GATE, SONAR_WAS, 17);
fail1:
    tagpu_detour_write(VA_RADAR_READ, RADAR_WAS, 17);
fail0:
    tagpu_detour_stub_free(sr);
    tagpu_detour_stub_free(ss);
    tagpu_log("seatcontact: NOT installed: a site could not be written");
}
