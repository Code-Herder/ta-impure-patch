#include "tagpu_simreg.h"
SIM_FEATURE(income_mult, SIM_PUPPET_OWNER, "income-mult income-stall");
/* THE INCOME LEVER: one seat's production times a multiplier m in [1, 8], for the handicap ladder, which finds
   the m at which OTA AI holds ImpAI Hard to half the points (tools/impai/ladder.py; research/notes/modules/
   income-lever.md). A harness lever, never a player setting: honoured only in a test launch on a skirmish level;
   with no lever every row is 0 and the economy pass runs as stock.

   THE ENGINE [DISASSEMBLED 2026-11-16]. The economy pass 0x401360(player), stdcall, `ret 4` (0x401C1C), runs
   once a 30-tick block for a record of type 1 or 2 (0x465539..0x46553F) from its only call, 0x46555A
   `E8 01 BE F9 FF`, in the players phase; nothing after the call reads eax (0x46555F reloads ecx). Each of its
   two production rates, +0x90 energy and +0x9C metal, is the sum over the seat's units plus the produced float
   of the player's own ledger, the +0xEC record: +0x00 for energy (loaded at 0x401914, added at 0x401927, the
   sum stored at 0x4019C6), +0x18 for metal (0x40191F, 0x401954, 0x401A05). The rates are then added to the
   stores (0x401A1D, 0x401A25), the consumption is taken up to what is stored, keeping the fraction met
   (0x401A4D..0x401AB1), each store is clamped to its maximum with the excess wasted (0x401AB3..0x401B27), and
   last the reset zeroes the ledger's produced floats (0x401BAE..0x401C16). A give credits the recipient's
   ledger there and nowhere else: energy 0x464C44, metal 0x464D77.

   THE BONUS goes in through the ledger, as a give does. Before the call, a seat whose row is k > 0
   (m = 1 + k / 1000) has (m - 1) x B_prev added to each produced float, B_prev the units' production of its
   previous pass; the pass folds it into the rate, so its own stall fraction, clamp and waste, the totals and
   the stock AI's planners (which read the rates: 0x408691, 0x40874A, 0x40BC53..0x40BCC5) all see it as
   production. After the call, B = the rate less the ledger's float as the pass read it -- the gives and this
   bonus taken out, so neither is ever multiplied, nor the bonus compounded -- is the units' production, kept
   for the next pass. Every bonus therefore arrives one pass, 30 ticks, after the production it multiplies,
   none is lost, and a level's first pass carries none.

   THE LEDGER, BOUND: the pointer at +0xEC is the one the pass itself dereferences unconditionally in this same
   call (0x40190E), made by the player init when there is none (0x4647CD..0x4647D7) and freed only by the level
   teardown's 0x464A00 (0x464A76, called at 0x491BA9), which never runs inside the players phase; the wrapper
   reads it on the thread that is about to make that call, and touches nothing when it is NULL.

   STATE. s_bonus and s_prev, SIM_STATE rows by player, are cleared at every level init and s_bonus is set
   from the lever only at a new skirmish's, in a test launch, so a recording carries both and a replay, a round trip, a
   seek or a takeover multiplies as recorded with no lever. On a puppet level the recording is the only
   writer, and the pass never runs for a puppet seat (type 3, 0x465539); the wrapper refuses one all the same.

   THREADS. The lever is read at attach. The rows are cleared and written on the LOADER thread at the PLAYERS
   call (0x4919C8, from 0x497581). The wrapper runs where the players phase runs: on the game thread in play,
   and once on the loader during a load (0x497C10), after the PLAYERS call and before the game thread's first
   step (bit 1 of main+0x38D75, set 0x497C5F, polled 0x498342). The level end reads GameTime on the game
   thread, in the teardown. */

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "tagpu_detour.h"
#include "tagpu_income.h"
#include "tagpu_log.h"
#include "tagpu_puppet.h"
#include "tagpu_regstore.h"

#define TA_MAIN        0x00511DE8u
#define OFF_PLAYERS    0x1B63
#define OFF_GAMETIME   0x38A47
#define OFF_SAVED      0x38D6B     /* dword: non-zero while a saved game loads (0x4978FA)            */
#define OFF_GSTATE     0x391E9     /* GameingState*: its +0 the mode, 1 campaign, 2 skirmish, 3 network */
#define MODE_SKIRMISH  2
#define PL_STRIDE      0x14B
#define PLAYERS        10
#define PL_E_RATE      0x90        /* float: the pass's production                                */
#define PL_M_RATE      0x9C
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

/* each record's units' production in its last pass, energy and metal: the next pass's bonus is m - 1 times it */
typedef struct { float e, m; } IncomePrev;
static IncomePrev s_prev[PLAYERS];
SIM_ROWS(s_prev, SIM_STATE, SIM_ID_PLAYER, NULL);

static struct { uint16_t bonus[PLAYERS]; uint8_t any; uint8_t installed; } s_lever;
SIM_VAR(s_lever, SIM_INSTALL);

/* the level's totals for the level-end line, which the simulation never reads */
static struct { uint32_t passes[PLAYERS]; double energy[PLAYERS], metal[PLAYERS]; } s_diag;
SIM_VAR(s_diag, SIM_PRIVATE);

static char* ta_main(void) { return *(char* volatile*)(size_t)TA_MAIN; }
static float rdf(const char* p) { float v; memcpy(&v, p, 4); return v; }
static void wrf(char* p, float v) { memcpy(p, &v, 4); }
static char* rdptr(const char* p) { char* v; memcpy(&v, p, sizeof v); return v; }
static int is_finite(float v) { return v == v && v - v == 0.0f; }

/* p's index among records 0..9, or PLAYERS when p is not exactly one of them */
static unsigned rec_index(char* ta, const char* p)
{
    uintptr_t off = (uintptr_t)(p - (ta + OFF_PLAYERS));
    if (p < ta + OFF_PLAYERS || off >= (uintptr_t)PLAYERS * PL_STRIDE || off % PL_STRIDE) return PLAYERS;
    return (unsigned)(off / PL_STRIDE);
}

/* The units' production in a resource's rate: the rate less the ledger's produced float as the pass read it;
   0 when that is not a finite positive number. */
static float units_made(float rate, float ledger)
{
    float b = rate - ledger;
    return is_finite(b) && b > 0.0f ? b : 0.0f;
}

/* (m - 1) x the units' last production added to one of the ledger's produced floats; the float as the pass will
   read it is returned. */
static float credit(char* ledger, unsigned off, float k, float prev)
{
    float v = rdf(ledger + off) + k * prev;
    wrf(ledger + off, v);
    return v;
}

static void __stdcall econ_pass(char* rec)
{
    char* ta = ta_main();
    unsigned i = ta ? rec_index(ta, rec) : PLAYERS;
    char* ledger = NULL;
    float k = 0.0f, le = 0.0f, lm = 0.0f;
    if (i < PLAYERS && s_bonus[i] && !tagpu_puppet_seat(i) && (ledger = rdptr(rec + PL_LEDGER)) != NULL) {
        k = (float)s_bonus[i] / 1000.0f;
        s_diag.passes[i]++;
        s_diag.energy[i] += k * s_prev[i].e;
        s_diag.metal[i] += k * s_prev[i].m;
        le = credit(ledger, LG_E_MADE, k, s_prev[i].e);
        lm = credit(ledger, LG_M_MADE, k, s_prev[i].m);
    }
    ((PFN_Pass)(size_t)VA_PASS)(rec);
    if (ledger) {
        s_prev[i].e = units_made(rdf(rec + PL_E_RATE), le);
        s_prev[i].m = units_made(rdf(rec + PL_M_RATE), lm);
    }
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
    if (!tagpu_regstore_active()) {
        tagpu_log("income: tagpu_impai_income.on ignored: not a test launch (tagpu_regstore.h)");
        return;
    }
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

/* A new skirmish: GameingState's mode 2 and no saved game, the test the loader's seat setup makes
   (0x4978DA..0x497902). GameingState is not NULL here: the loader body called 0x435100, `mov eax,[ecx]`, on it
   at 0x4971CD, on the only path into the level init (0x497CA1 -> 0x497581). */
static int new_skirmish(char* ta)
{
    char* gs = rdptr(ta + OFF_GSTATE);
    int mode;
    uint32_t saved;
    if (!gs) return 0;
    memcpy(&mode, gs, sizeof mode);
    memcpy(&saved, ta + OFF_SAVED, sizeof saved);
    return mode == MODE_SKIRMISH && !saved;
}

void tagpu_income_level_init(void)
{
    char* ta = ta_main();
    unsigned i;
    memset(s_bonus, 0, sizeof s_bonus);
    memset(s_prev, 0, sizeof s_prev);
    memset(&s_diag, 0, sizeof s_diag);
    if (!s_lever.any || tagpu_puppet_on() || !ta) return;
    if (!s_lever.installed) {
        tagpu_log("income: the lever is ignored: the call site is not installed");
        return;
    }
    if (!new_skirmish(ta)) {
        tagpu_log("income: the lever is ignored: the level is not a new skirmish (a network game's pass runs on "
                  "each seat's own peer)");
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
