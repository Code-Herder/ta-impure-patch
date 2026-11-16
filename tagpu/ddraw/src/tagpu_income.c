#include "tagpu_simreg.h"
SIM_FEATURE(income_mult, SIM_PUPPET_OWNER, "income-mult");
/* THE INCOME LEVER: one seat's production times a multiplier m in [1, 8], for the handicap ladder, which finds
   the m at which OTA AI holds ImpAI Hard to half the points (tools/impai/ladder.py; research/notes/modules/
   income-lever.md). A harness lever, never a player setting: with no lever every row is 0 and the economy pass
   runs as stock.

   THE ENGINE [DISASSEMBLED 2026-11-16]. The economy pass 0x401360(player), stdcall, `ret 4` (0x401C1C), runs
   once a 30-tick block for a record of type 1 or 2 (0x465539..0x46553F) from its only call, 0x46555A
   `E8 01 BE F9 FF`, in the players phase on the GAME thread; nothing after the call reads eax (0x46555F
   reloads ecx). It writes the pass's production as the energy and metal rates +0x90 (0x4019C6) and +0x9C
   (0x401A05), adds them to the double totals +0xAC and +0xB4, adds them to the stored +0x8C and +0x98
   (0x401A1D, 0x401A25), takes the consumption up to what is stored (0x401A4D..0x401AB1), then clamps each
   store to its maximum +0xA4 / +0xA8, adding the excess to the double wasted +0xCC / +0xD4 (0x401AB3..
   0x401B27). Each rate is the sum over the seat's units' ledgers plus the produced float of the player's own
   ledger, the +0xEC record: +0x00 for energy (loaded at 0x401914, added at 0x401927, the sum stored at
   0x4019C6), +0x18 for metal (0x40191F, 0x401954, 0x401A05); the pass's reset zeroes both (0x401BAE..
   0x401C16). A give credits the recipient's ledger there and nowhere else: energy 0x464C44, metal 0x464D77.

   THE BONUS. The wrapper calls the pass, then for a seat whose row is k > 0 (m = 1 + k / 1000) adds
   (m - 1) x B to each resource, B the pass's rate less what the player's ledger held before the call -- the
   units' production, so a give is never multiplied -- to the rate, to the total, and to the store, clamped as
   the pass clamps (strictly above the maximum, the excess to wasted). The stock AI's planners read the rates
   (0x408691, 0x40874A, 0x40BC53..0x40BCC5), so the seat plans on its multiplied income, as on a mod's.

   THE STALL, a property of the design: the bonus reaches the store after the pass's own consumption step, so
   in a pass whose consumption exceeds the store plus the production, the fraction met is the stock one and
   the bonus is spent at the next pass, 30 ticks later. Deterministic, monotone in m, and the same total
   bonus; only the pass in which it is spent differs. Applying it inside the pass needs a patch in the x87
   sequence at 0x4019B0, where the energy rate lives only on the FPU stack.

   STATE. s_bonus, SIM_STATE rows by player: written at the level init only, from the lever on a live
   skirmish level, so a recording carries it and a replay, a round trip or a takeover multiplies by the
   recorded rows with no lever. On a puppet level the recording is the only writer, and the pass never runs
   for a puppet seat (type 3, 0x465539); the wrapper refuses one all the same.

   THREADS. The lever is read at attach; the rows are cleared and written on the LOADER thread at the
   PLAYERS call, before the level's first step (the loader's done bit, main+0x38D75 bit 1, orders the two),
   and read on the GAME thread in the players phase. */

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "tagpu_detour.h"
#include "tagpu_income.h"
#include "tagpu_log.h"
#include "tagpu_puppet.h"

#define TA_MAIN        0x00511DE8u
#define OFF_PLAYERS    0x1B63
#define OFF_NETFLAGS   0x2A44      /* u8, bit 0: a network game                                  */
#define OFF_GAMETIME   0x38A47
#define PL_STRIDE      0x14B
#define PLAYERS        10
#define PL_E_STORED    0x8C        /* float                                                       */
#define PL_E_RATE      0x90        /* float: the pass's production                                */
#define PL_M_STORED    0x98
#define PL_M_RATE      0x9C
#define PL_E_MAX       0xA4
#define PL_M_MAX       0xA8
#define PL_E_TOTAL     0xAC        /* double                                                      */
#define PL_M_TOTAL     0xB4
#define PL_E_WASTED    0xCC        /* double                                                      */
#define PL_M_WASTED    0xD4
#define PL_LEDGER      0xEC        /* the player's ledger: +0x00 energy and +0x18 metal produced  */
#define LG_E_MADE      0x00
#define LG_M_MADE      0x18

#define VA_CALL        0x0046555Au
#define VA_PASS        0x00401360u
#define BONUS_MAX      7000u       /* m = 8                                                       */

typedef void (__stdcall* PFN_Pass)(char* player);

/* each record's bonus in thousandths: m = 1 + row / 1000; 0 runs the pass as stock */
static uint16_t s_bonus[PLAYERS];
SIM_ROWS(s_bonus, SIM_STATE, SIM_ID_PLAYER, NULL);

static struct { uint16_t bonus[PLAYERS]; uint8_t any; uint8_t installed; } s_lever;
SIM_VAR(s_lever, SIM_INSTALL);

/* the level's totals for the level-end line, which the simulation never reads */
static struct { uint32_t passes[PLAYERS]; double energy[PLAYERS], metal[PLAYERS]; } s_diag;
SIM_VAR(s_diag, SIM_PRIVATE);

static char* ta_main(void) { return *(char* volatile*)(size_t)TA_MAIN; }
static float rdf(const char* p) { float v; memcpy(&v, p, 4); return v; }
static void wrf(char* p, float v) { memcpy(p, &v, 4); }
static double rdd(const char* p) { double v; memcpy(&v, p, 8); return v; }
static void wrd(char* p, double v) { memcpy(p, &v, 8); }
static int is_finite(float v) { return v == v && v - v == 0.0f; }

/* p's index among records 0..9, or PLAYERS when p is not exactly one of them */
static unsigned rec_index(char* ta, const char* p)
{
    uintptr_t off = (uintptr_t)(p - (ta + OFF_PLAYERS));
    if (p < ta + OFF_PLAYERS || off >= (uintptr_t)PLAYERS * PL_STRIDE || off % PL_STRIDE) return PLAYERS;
    return (unsigned)(off / PL_STRIDE);
}

/* One resource's bonus: `extra` added to the rate, the total and the store, the store clamped as the pass
   clamps it (0x401AB3..0x401B27). */
static void credit(char* rec, unsigned rate, unsigned total, unsigned stored, unsigned max, unsigned wasted,
                   float extra)
{
    float s = rdf(rec + stored) + extra, cap = rdf(rec + max);
    wrf(rec + rate, rdf(rec + rate) + extra);
    wrd(rec + total, rdd(rec + total) + (double)extra);
    if (s > cap) {
        wrd(rec + wasted, rdd(rec + wasted) + ((double)s - (double)cap));
        s = cap;
    }
    wrf(rec + stored, s);
}

/* The production a resource's rate holds less what the player's ledger carried into the pass; 0 when that is
   not a finite positive number. */
static float units_made(float rate, float ledger)
{
    float b = rate - ledger;
    return is_finite(b) && b > 0.0f ? b : 0.0f;
}

static void __stdcall econ_pass(char* rec)
{
    char* ta = ta_main();
    unsigned i = ta ? rec_index(ta, rec) : PLAYERS;
    const char* ledger;
    float le = 0.0f, lm = 0.0f, k, de, dm;
    if (i < PLAYERS && s_bonus[i] && (ledger = *(char* const*)(rec + PL_LEDGER)) != NULL) {
        le = rdf(ledger + LG_E_MADE);
        lm = rdf(ledger + LG_M_MADE);
    }
    ((PFN_Pass)(size_t)VA_PASS)(rec);
    if (i >= PLAYERS || !s_bonus[i] || tagpu_puppet_seat(i) || !*(char* const*)(rec + PL_LEDGER)) return;
    k = (float)s_bonus[i] / 1000.0f;
    de = k * units_made(rdf(rec + PL_E_RATE), le);
    dm = k * units_made(rdf(rec + PL_M_RATE), lm);
    credit(rec, PL_E_RATE, PL_E_TOTAL, PL_E_STORED, PL_E_MAX, PL_E_WASTED, de);
    credit(rec, PL_M_RATE, PL_M_TOTAL, PL_M_STORED, PL_M_MAX, PL_M_WASTED, dm);
    s_diag.passes[i]++;
    s_diag.energy[i] += de;
    s_diag.metal[i] += dm;
}

/* One lever item, `<seat>=<m>`, m a decimal with at most three places in [1, 8]: the seat and its row, or -1. */
static int lever_item(const char* p, unsigned* seat, unsigned* bonus)
{
    unsigned whole = 0, frac = 0, places = 0;
    if (!(p[0] >= '0' && p[0] <= '9' && p[1] == '=')) return -1;
    *seat = (unsigned)(p[0] - '0');
    p += 2;
    if (!(*p >= '0' && *p <= '9')) return -1;
    while (*p >= '0' && *p <= '9' && whole < 10) whole = whole * 10 + (unsigned)(*p++ - '0');
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && places < 3) { frac = frac * 10 + (unsigned)(*p++ - '0'); places++; }
        if (!places) return -1;
        while (places < 3) { frac *= 10; places++; }
    }
    if (*p || whole < 1 || whole * 1000 + frac > 1000 + BONUS_MAX) return -1;
    *bonus = whole * 1000 + frac - 1000;
    return 0;
}

static void read_lever(void)
{
    char t[PLAYERS * 16];
    char* p;
    if (tagpu_sim_lever_read("tagpu_impai_income.on", t, (int)sizeof t) <= 0) return;
    for (p = strtok(t, ", \r\n\t"); p; p = strtok(NULL, ", \r\n\t")) {
        unsigned seat, bonus;
        if (lever_item(p, &seat, &bonus)) {
            tagpu_logf("income: tagpu_impai_income.on: `%s` refused (<seat>=<m>, m in 1..8, three places at most)", p);
            continue;
        }
        s_lever.bonus[seat] = (uint16_t)bonus;
        s_lever.any = 1;
    }
}

/* Byte-matched, written at attach whatever the lever says, so a recording that carries rows plays on with
   none: with every row 0 the wrapper is the stock call. No other module writes 0x46555A (hook-map.md). */
void tagpu_income_init(void)
{
    read_lever();
    if (!tagpu_detour_redirect_ok(VA_CALL, VA_PASS)) {
        tagpu_log("income: NOT installed: 0x46555A does not hold the retail call of the economy pass");
        return;
    }
    if (!tagpu_detour_redirect(VA_CALL, VA_PASS, (void*)econ_pass)) {
        tagpu_log("income: NOT installed: 0x46555A could not be written");
        return;
    }
    s_lever.installed = 1;
    tagpu_logf("income: installed: the economy pass's call 0x46555A%s",
               s_lever.any ? "; the lever names seats" : "");
}

void tagpu_income_level_init(void)
{
    char* ta = ta_main();
    unsigned i;
    memset(s_bonus, 0, sizeof s_bonus);
    memset(&s_diag, 0, sizeof s_diag);
    if (!s_lever.any || tagpu_puppet_on() || !ta) return;
    if (!s_lever.installed) {
        tagpu_log("income: the lever is ignored: the call site is not installed");
        return;
    }
    if (*(const unsigned char*)(ta + OFF_NETFLAGS) & 1) {
        tagpu_log("income: the lever is ignored in a network game, whose pass runs on each seat's own peer");
        return;
    }
    for (i = 0; i < PLAYERS; i++) {
        s_bonus[i] = s_lever.bonus[i];
        if (s_bonus[i])
            tagpu_logf("income: seat=%u mult=%u.%03u from=lever", i, 1u + s_bonus[i] / 1000u, s_bonus[i] % 1000u);
    }
}

void tagpu_income_level_end(void)
{
    char* ta = ta_main();
    unsigned i, now = ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0;
    for (i = 0; i < PLAYERS; i++)
        if (s_bonus[i] || s_diag.passes[i])
            tagpu_logf("income: level end seat=%u mult=%u.%03u passes=%u energy=%.1f metal=%.1f gametime=%u", i,
                       1u + s_bonus[i] / 1000u, s_bonus[i] % 1000u, s_diag.passes[i], s_diag.energy[i],
                       s_diag.metal[i], now);
}
