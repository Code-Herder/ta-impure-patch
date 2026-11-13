/* tasim_econ.c -- the shared simulation core's economy: an extractor's rate, a builder's request on its ledger,
   the seats' unit counts and the economy's pass, and a seat's army value (WIKI_PLAY_GAME and _STALL only;
   research/notes/ingame-wiki-replay-plan.md, decisions 2, 3, 7 and 18). research/notes/impai/shared-core.md,
   tasim_econ. Contract: tasim.h.

   A demo and Try it run none of the pass, so no state or hash of theirs can change by it. The ledgers and the seats'
   economies are fields of the TASIM its caller owns (WIKIUNIT.led, TASIM.econ); no globals. The arithmetic runs
   on SSE2 doubles, each operation rounded once, so every build agrees to the bit. */
#include <math.h>
#include "tasim.h"

/* JS's ToInt32 of a finite double's integer part, its low byte: a cell's metal as the engine's byte */
static int metal_byte(double m)
{
    double t = fmod(trunc(m), 4294967296.0);
    if (t < 0) t += 4294967296.0;
    return (int)((uint32_t)t & 0xFFu);
}

/* the extractor's SetSpeed (0x437840, from every create): the sum over its footprint cells on the
   map of the cell's metal + 1, kept in a 16-bit word and sign-extended (0x437866..0x437902).
   ExtractsMetal does not enter it. */
int32_t tasim_extractor_speed(const TASIM* s, const TASIM_DEFS* defs, const WIKIUNIT* u)
{
    int x, z, sum = 0;
    for (z = u->oz; z < u->oz + tasim_fpz(defs, u->type); z++)
        for (x = u->ox; x < u->ox + tasim_fpx(defs, u->type); x++)
            if (x >= 0 && x < s->W && z >= 0 && z < s->H) sum += metal_byte(s->metal[z * s->W + x]) + 1;
    sum &= 0xFFFF;
    return sum >= 0x8000 ? sum - 0x10000 : sum;
}

/* THE BUILD'S REQUEST, 0x4011C0 on the BUILDER's ledger (0x41BA60 charges it at 0x41BC5A,
   `lea ecx,[ebp+0xbc]`, ebp the builder): the request is always added (0x4011D1..0x4011E3) and
   granted unless either debt is above 0 (0x4011C0..0x4011F6, 0x4011F8..0x401212); the step applies
   its progress only on a grant (0x41BC65..0x41BC67) */
int tasim_econ_request(WIKIUNIT* u, double e, double m)
{
    WIKILEDGER* L = &u->led;
    L->eR += e;
    L->mR += m;
    if (L->eD > 0 || L->mD > 0) return 0;
    L->eG += e;
    L->mG += m;
    return 1;
}

/* the seats' unit counts at the end of a step, after the work; `out` once a count goes from above
   0 to 0. A nanoframe counts: +0x144 is incremented at every create, finished or not (0x486187,
   0x486322). */
void tasim_econ_counts(TASIM* s)
{
    int k, i;
    for (k = 0; k < s->nseat; k++) {
        WIKIECON* z = &s->econ[k];
        int n = 0;
        for (i = 0; i < s->nunit; i++) n += s->unit[i]->owner == k;
        if (z->units > 0 && n == 0) z->out = 1;
        z->units = n;
    }
}

/* one resource's settle, 0x401A4D..0x401AB1 then 0x401AB3..0x401B27: the debt paid first, then the
   grants, from what is stored plus produced; what passes the storage is wasted. The fractions of
   the debt and of the grants paid out. */
static void econ_settle(double* cur, double* waste, double P, double D, double G, double max, double* fD, double* fG)
{
    const double A = *cur + P;
    const double rest = A - (D <= A ? D : A);
    double c, w = 0;
    *fD = D <= A ? 1 : A / D;
    *fG = G <= rest ? 1 : rest / G;
    c = rest - (G <= rest ? G : rest);
    if (c > max) {
        w = c - max;
        c = max;
    }
    *cur = c;
    *waste += w;
}

/* THE PASS, 0x401360, on ticks 1, 30, 60, ...: the level's init seeds the player's next pass +0xF0
   with GameTime 0 (0x46470C..0x464715), and the players phase runs a record's pass when +0xF0 <=
   GameTime and adds 30 (0x46507D..0x465092), after GameTime's increment; every seat on the same
   tick, in seat order, after the units' work (research/notes/engine/sim.md, the players phase
   follows the unit tick). Per unit of the seat in slot order (0x401375..0x4013A6; here in creation
   order, below): a structure (+0x110 bit 29, 0x4013BA) that is active (+0x10E bit 0, 0x4013C5)
   requests its EnergyUse (0x4013ED..0x401421) or, negative, produces it (0x401429..0x401478), and
   an extractor whose energy was granted produces ExtractsMetal x its SetSpeed (0x401480..0x4014C8);
   a mobile unit active or moving (0x401617..0x401622) requests or produces its EnergyUse
   (0x401641..0x401668); a finished unit (+0x104 == 0.0, 0x4016C5..0x4016D6) produces its
   EnergyMake and MetalMake (0x4016DC..0x401795) and adds its storages (0x40179B..0x4017C5). Then
   the seat's sums: the storage with the base while the commander lives (0x401941,
   0x401988..0x4019AA), income the produced and spending the REQUESTED (0x4019C6..0x401A2D); per
   resource the debt paid first, then the grants, from what is stored plus produced
   (0x401A4D..0x401AB1), and what passes the storage wasted (0x401AB3..0x401B27); and the settle
   (0x401B3D..0x401BAC), each unit's unpaid share of its grants and its debt its new debt, the
   rest of its ledger zeroed, the engine's fmul, fsubr and faddp in the engine's order.
   WHERE THE WIKI DEPARTS: activation is a finished unit whose FBI says ActivateWhenBuilt, which the
   engine sets at a completion (0x41B9A6) and at a finished create (0x48612F) [INFERRED: no cast
   unit's script changes its own], and moving is the step this tick [INFERRED that it agrees with
   the move-rate class +0x110 & 0xC]; the units are walked in creation order, where the engine
   walks its slots, which the allocator reuses: the sums are the same terms added in another
   order, so only their rounding can differ; MakesMetal, wind, tidal, cloak, the AI's scaling, the
   transfers and the reclaim payouts are not modelled (no cast takes such a unit, no seat is an
   AI, the owner left the payouts out). The ledger is the engine's f32 as a double, as every
   figure here is. */
void tasim_econ_pass(TASIM* s, const TASIM_DEFS* defs)
{
    int k, i;
    for (k = 0; k < s->nseat; k++) {
        WIKIECON* z = &s->econ[k];
        double sm = 0, se = 0, EP = 0, ER = 0, EG = 0, ED = 0, MP = 0, MR = 0, MG = 0, MD = 0;
        double fDe, fGe, fDm, fGm;
        for (i = 0; i < s->nunit; i++) {
            WIKIUNIT* u = s->unit[i];
            const WIKIDEF* d = tasim_def(defs, u->type);
            WIKILEDGER* L = &u->led;
            const double eu = d->energy_use;
            const int active = u->built >= 1 && d->activate_when_built;
            if (u->owner != k) continue;
            if (!tasim_mobile(defs, u->type)) {
                if (active) {
                    int granted = 0;
                    if (eu >= 0) {
                        L->eR += eu;
                        if (!(L->eD > 0)) {
                            L->eG += eu;
                            granted = 1;
                        }
                    } else
                        L->eP += -eu;
                    if (d->extracts > 0 && granted) L->mP += d->extracts * tasim_extractor_speed(s, defs, u);
                }
            } else if (active || u->stepped) {
                if (eu >= 0) {
                    L->eR += eu;
                    if (!(L->eD > 0)) L->eG += eu;
                } else
                    L->eP += -eu;
            }
            if (u->built >= 1) {
                L->eP += d->energy_make;
                L->mP += d->metal_make;
                se += d->energy_storage;
                sm += d->metal_storage;
            }
        }
        for (i = 0; i < s->nunit; i++) {
            const WIKILEDGER* L = &s->unit[i]->led;
            if (s->unit[i]->owner != k) continue;
            EP += L->eP; ER += L->eR; EG += L->eG; ED += L->eD;
            MP += L->mP; MR += L->mR; MG += L->mG; MD += L->mD;
        }
        z->emax = se + (z->cmd_lost ? 0 : z->be);
        z->mmax = sm + (z->cmd_lost ? 0 : z->bm);
        z->ei = EP;
        z->eo = ER;
        z->mi = MP;
        z->mo = MR;
        econ_settle(&z->e, &z->we, EP, ED, EG, z->emax, &fDe, &fGe);
        econ_settle(&z->m, &z->wm, MP, MD, MG, z->mmax, &fDm, &fGm);
        for (i = 0; i < s->nunit; i++) {
            WIKILEDGER* L = &s->unit[i]->led;
            if (s->unit[i]->owner != k) continue;
            L->eD = (L->eG - fGe * L->eG) + (L->eD - fDe * L->eD);
            L->eP = L->eR = L->eG = 0;
            L->mD = (L->mG - fGm * L->mG) + (L->mD - fDm * L->mD);
            L->mP = L->mR = L->mG = 0;
        }
        if (s->econ_mode == WIKI_ECON_REFUSE && (fDe < 1 || fGe < 1 || fDm < 1 || fGm < 1)) s->starved = 1;
    }
}

/* THE ARMY VALUE (tagpu_replay_army_values, tagpu_hudmarks.c sample): the game values a type whose
   BMcode +0x22F is 1 and that is not a commander (+0x245 bit 0x40000) at its metal cost +0x18A,
   every other at 0, and sums the floats of its live, finished units in slot order; the wiki's
   mobile type is one with a max velocity, its commander the cast. The sum stays in f32 (SSE2,
   the Makefile's THE WIKI'S SIMULATION), each add rounded once to f32. */
float tasim_army(const TASIM* s, const TASIM_DEFS* defs, int seat)
{
    float a = 0;
    int i;
    for (i = 0; s->game && i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        if (u->owner == seat && u->built >= 1 && tasim_mobile(defs, u->type) && tasim_def(defs, u->type)->cast != WIKI_CAST_COMMANDER)
            a += (float)tasim_def(defs, u->type)->build_cost_metal;
    }
    return a;
}
