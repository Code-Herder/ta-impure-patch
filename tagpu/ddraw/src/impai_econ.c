/* impai_econ.c -- ImpAI's economy manager (impai.h, the brain): the ladder that gives each idle builder its
   task, the type it builds within a category, the plant the seat builds next (THE CHOICE), and the factories'
   orders (THE GOVERNOR, THE ARMY ORDERS). The sites and the orders are impai_build.c's. Engine-free, no statics. */
#include <float.h>
#include <string.h>
#include "impai_brain.h"
#include "impai_mil.h"
#include "tagpu_simreg.h"

/* The economy manager. OWNER: the brain calls it only inside a live ImpAI seat's turn, which plays no puppet
   seat (tagpu_impai.c, impai_play), so a puppet's orders come from the recording. */
SIM_FEATURE(impai_econ, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval impai-raid impai-persona");

/* the type's own menu offers an extractor */
static int builds_mex(IMPAI_DEC* d, int type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int k, n = d->q->ops->build_options(d->q, type, d->menu2, WQ_TYPES_MAX);
    for (k = 0; k < n; k++) if (d->menu2[k] < in->nti && in->ti[d->menu2[k]].cat == IMPAI_C_MEX) return 1;
    return 0;
}

/* ---- the type within a category, from the builder's menu; ties go to the lower type number ---- */

/* The ring places energy and plants around the anchor, so a type placed on water only (IMPAI_TF_WATER) can stand
   there only when the anchor's ground lies below the sea; on a dry anchor every site test of it fails. */
static int anchor_wet(IMPAI_DEC* d)
{
    const int32_t apos[3] = { (int32_t)((uint32_t)(d->in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(d->in->st->az * 16 + 8) << 16) };
    return d->q->ops->ground(d->q, apos) < (int)d->m.sea;
}

/* energy: the most egain per value, a water-only type only on a wet anchor */
static int choose_energy(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int wet = anchor_wet(d);
    int k, best = 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        const IMPAI_TINFO* ti;
        if (t >= (int)in->nti || in->ti[t].cat != IMPAI_C_ENERGY || in->ti[t].value <= 0) continue;
        ti = &in->ti[t];
        if ((ti->flags & IMPAI_TF_WATER) && !wet) continue;
        if (!best || ti->egain * in->ti[best].value > in->ti[best].egain * ti->value ||
            (ti->egain * in->ti[best].value == in->ti[best].egain * ti->value && t < best))
            best = t;
    }
    return best;
}

/* the extractors, one for each kind of ground (impai_brain.h IMPAI_MEX): `dry` among the menu's types placed on any
   ground, `wet` among the water-only ones (IMPAI_TF_WATER) with its minwaterdepth; each the most mgain per value,
   as energy is ranked (owner's decision, 2026-11-14), ties to the lower type. 1 when either is set. The two kinds
   are never ranked against each other: the patch's ground picks between them (impai_build.c, THE EXTRACTOR FOR THE
   PATCH), so no tie between a land and a water extractor arises, and retail's land one no longer rests on its type
   number being the lower. A water type whose minwaterdepth cannot be read or is not above 0 is dropped: no ground
   would tell it from the land one. */
static int choose_mex(IMPAI_DEC* d, const uint16_t* menu, int n, IMPAI_MEX* out)
{
    const IMPAI_BRAIN_IN* in = d->in;
    WQ_DEF_ECON e;
    int k;
    memset(out, 0, sizeof *out);
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        const IMPAI_TINFO* ti;
        uint16_t* best;
        if (t >= (int)in->nti || in->ti[t].cat != IMPAI_C_MEX || in->ti[t].value <= 0) continue;
        ti = &in->ti[t];
        best = (ti->flags & IMPAI_TF_WATER) ? &out->wet : &out->dry;
        if (!*best || ti->mgain * in->ti[*best].value > in->ti[*best].mgain * ti->value ||
            (ti->mgain * in->ti[*best].value == in->ti[*best].mgain * ti->value && t < *best))
            *best = (uint16_t)t;
    }
    if (out->wet && d->q->ops->def_econ(d->q, out->wet, &e) && e.minwater > 0)
        out->wet_min = e.minwater;
    else
        out->wet = 0;
    return out->dry || out->wet;
}

/* a maker: the most mgain per egain (its energy cost), then the least value */
static int choose_maker(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int k, best = 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        const IMPAI_TINFO* ti;
        float lhs, rhs;
        if (t >= (int)in->nti || in->ti[t].cat != IMPAI_C_MAKER || in->ti[t].egain <= 0) continue;
        ti = &in->ti[t];
        if (!best) { best = t; continue; }
        lhs = ti->mgain * in->ti[best].egain;
        rhs = in->ti[best].mgain * ti->egain;
        if (lhs > rhs || (lhs == rhs && (ti->value < in->ti[best].value || (ti->value == in->ti[best].value && t < best))))
            best = t;
    }
    return best;
}

/* ---- THE CHOICE: which plant the seat builds next ----

   THE REACH (reach_mask_of): bit c is set when movement class c reaches a plausible enemy base from the base.
   - THE BASE'S COMPONENT: the label most of the blocks within REACH_RINGS rings of the anchor's 32-px block carry
     (base_label). The anchor is where the commander stood, and the content map can hold a larger class's block
     there impassable (a feature, a slope) where the plants' yards around it are not, or a pocket beside it that
     the yards' ground does not join; a class with no label within the rings has no ground at the base, and does
     not reach. RESIDUAL: past BASE_LABELS distinct labels in the rings the later ones are not counted.
   - THE TARGETS: once enemy structures have been seen (impai_est_known), the military's estimate, their
     value-weighted centroid or the last one kept; before that, every start position but the seat's own (the one
     nearest the anchor):
     the military's estimate is then only the nearest other start, which on Trout Farm is cut off by water from
     the anchor while the enemy, at another start, was land-linked (MEASURED 2026-11-14, plantchoice r1 and r3's
     Trout Farm games, and governor play1's b5 naval note). The starts are public: the skirmish screen shows them.
     An empty start stays plausible: which starts the players hold is not on that screen. RESIDUAL: an ally's
     start is a target too -- the lobby shows the teams, but the WORLDQ has no read of a player's start index
     and alliance yet, which excluding them needs. With fewer than two starts, the estimate; with no estimate
     either, every bit is set.
   - A class reaches when one target has a block of the base's component within REACH_RINGS rings (a start's own
     block may be impassable to a large class as the anchor's may). Any, not all: the extra mobility of a class
     counts only where the map needs it, and before the enemy is known the map needs it only when no plausible
     enemy is reached otherwise -- "a hover only where the route to the enemy needs water" (owner, 2026-11-14).
     RESIDUAL: until a structure is seen, a seat whose real enemy sits on the one start cut off from it builds land
     plants; the first enemy structure seen moves the targets to it.
   - Every bit is set with no reach table, no military or no anchor, and a class with no labels (-1) keeps its bit.
   A type reaches when it is a VTOL, its class's bit is set, or its class is past the mask (no evidence). The reach
   only filters: a hover's mobility adds nothing where the land classes reach, and is what makes its plant score
   where they do not. A route much shorter over water is not counted (owner's decision,
   research/notes/impai/decisions.md). BOUND: once a decision, for each class with labels, one spiral at the
   anchor and one at each target, each at most (2 REACH_RINGS + 1)^2 label reads, at most PLANT_STARTS targets.

   THE SCORE of plant P at the tech level T (choice_tech), at most IMPAI_QTECH: max(ftop, 1), the enemy taken at the
   seat's own tech, and for a tech-up the level above it, the tech its candidates are built for (owner's decision,
   2026-11-14) -- against the tech-1 field a tech-2 unit saturated: Total Mayhem's CORAVP and Escalation's ARMAVP
   scored 1.000, and the same Mayhem seat at T = 2 scored CORAVP 0.831 against CORHP 0.288, the same order
   (MEASURED 2026-11-14) --: S(P) = the highest value (IMPAI_TVALUE.q[T - 1], impai_tinfo.c THE VALUES) of a type on
   P's menu of a gated class (IMPAI_GATED: the land fighting three) with a value above 0 that reaches -- P's LAND
   ARMY --; IMPAI_Q_NONE when there is none or no values. A plant is scored by its best unit; THE ARMY ORDERS then
   hold it to the classes in which its unit is that good.

   THE CANDIDATES: the distinct factory types on the menus of the seat's finished builder types (IMPAI_DEC.btype, at
   most IMPAI_SEA_BUILDERS, in census order, each menu ascending), then on the menus of THE UNLOCKS, at most
   IMPAI_PLANT_CANDS of them examined, each a menu read of at most PLANT_MENU types and one more for each
   constructor on it, kept when
   - its menu holds a constructor whose own menu holds an extractor;
   - it is placed on dry ground, or the anchor is wet (a water-only plant fits no site of the ring otherwise);
   - its way out has not been refused too often (impai_build.c, THE PLANT'S FALLBACK), unless `waived`;
   - THE BUILD TECH (IMPAI_TINFO.ctech) fits the want: a tech-up takes only a plant above the seat's highest
     (IMPAI_DEC.ftop); a first or further plant only one at or below max(ftop, lo), lo the least among the
     candidates, so the economy's income gate stays the only way up and a mod whose cheapest plant already
     brings tech 2 still has a first plant.
   THE TECH-UP FIRST: a seat that wants both -- a surplus's plant past factory_max beside a tech-up that is due
   (impai_brain.c, economy) -- takes the tech-up's passes first and the further plant's only when they find
   none, so a full store never builds a third plant of tech 1 in place of the tech-up, and a seat with nothing
   above its highest tech is not held at its count by a tech-up no menu offers.
   The seat's menus and not the idle builder's: a builder's own menu would hide the plant it cannot build (an ARMCV
   sees only ARMAVP and ARMHP). A builder whose menu holds the choice builds it; one whose menu does not passes the
   factory rule, and the plant waits for a builder that offers it.
   THE UNLOCKS: the constructor types the seat's factories make of which it has no finished unit (plant_unlocks,
   the census's IMPAI_DEC.cons_done, at most PLANT_VIA). A plant only such a constructor builds is a candidate like
   any other, and when it is chosen the governor's next constructor is that type, at a factory that makes it
   (impai_econ_factories), once a decision and while none is a frame; the plant then waits for it. WHY: CORE's
   opening plant is CORVP and its governor walks the cheapest-army plant first, so the seat made CORCVs only and
   CORCK's CORALAB (+0.517) was no candidate: 6 of 7 CORE seats teched up to CORAVP at -0.396 (MEASURED
   2026-11-14, plantchoice r1 and r3). The governor's rules still bound every constructor: an unlock changes
   which type, never how many. So the unlocks are candidates only while the constructors are below cons_max, the
   count gov_cons tests: at the cap no constructor can come, and the best of the seat's own plants is taken.
   RESIDUAL: below the cap the governor's other rules can still hold the unlock back -- the army's share and the
   pressure -- and the plant waits while they do.
   THE KEPT UNLOCK (IMPAI_BRAIN.unlock): the choice runs only in a decision that wants a plant, and the wants are
   gates that lapse for a decision -- an energy stall, the hold, the steady income dipping under tech_m -- so an
   unlock read from this decision's choice alone came and went with them, and a factory the unlock is not on the
   menu of made its own constructor in between. So the choice's unlock is kept
   until the next decision that runs the choice, whose answer replaces it (0 when it waits for none), and is
   dropped as soon as it is no unlock: a decision that does not run the choice keeps it only while plant_unlocks
   still names it -- a constructor type on the menu of a factory of the seat's with no finished unit -- so it ends
   at the first finished unit of it, or with the last factory that makes it. BOUND: that test is one
   plant_unlocks a decision.

   THE BEST: the highest score, then a plant the seat's own builders build before one an unlock waits for, then the
   least value, then the lower type. The first plant (no factory of the seat's yet, frames included) is the least
   value among the plants with a land army, by the same rule as the score but with no values read, and the least
   value of all when none has one: its first purpose is the constructors, and the army it can make next (owner's
   decision, 2026-11-14) -- the least value alone put TA Zero's GoK on GoKT1AF, an air plant with no land army
   (MEASURED 2026-11-14). On a wet anchor the least value of all: the score ranks land armies,
   and a ring around an anchor below the sea may hold no site for a land plant (no bar seat-game's first plant was
   a shipyard, MEASURED 2026-11-14, so no bar start is wet). With no values every score ties and the choice is the
   least value. The runner-up is kept for the plant line. A tech-up whose best candidate scores below 0 still goes
   ahead (owner's decision, 2026-11-14): it brings the tech-2 constructors and economy, and THE ARMY ORDERS keep a
   plant whose army is poor here to constructors.

   WHY, MEASURED 2026-11-14 over 377 bar seat-games: the least value with the offer -- the highest classifier tech
   of a plant's units, a graph distance -- took CORE's hover plant as the tech-up in 167 of 170 seats (CORHP 1 883
   against CORAVP 2 006 and CORALAB 2 032), whose constructor builds nothing above tech 1, and its offer 2 ended the
   tech-up for good: no CORE seat built a plant of tech 2. */
#define FACTORY_LOOK 8
#define PLANT_MENU   256
#define PLANT_VIA    8          /* THE UNLOCKS: constructor types a pass looks through                       */
#define PLANT_LOOK   (2 * IMPAI_PLANT_CANDS)  /* plant menus a pass of THE CHOICE reads               */

#define REACH_RINGS  8          /* 256 px round the anchor or a target                                       */
#define PLANT_STARTS 16

#define BASE_LABELS  8          /* distinct labels base_label counts; a ninth and later one is not counted    */

/* class c's base component: the label most blocks within REACH_RINGS rings of block (bx, bz) carry, ties to the
   one met first in ring order -- the yards stand round the anchor, and one passable pocket next to it must not
   speak for the ground they stand on; 0 none; -1 the level has no labels for c. BOUND: (2 REACH_RINGS + 1)^2
   label reads and BASE_LABELS counters */
static int base_label(const IMPAI_REACH* r, unsigned c, int bx, int bz)
{
    int lab[BASE_LABELS], cnt[BASE_LABELS], n = 0, ring, i, k, dx, dz, best = -1;
    if (impai_reach_at(r, c, bx, bz) < 0) return -1;
    for (ring = 0; ring <= REACH_RINGS; ring++)
        for (i = 0; impai_ring_at(ring, i, &dx, &dz); i++) {
            const int l = impai_reach_at(r, c, bx + dx, bz + dz);
            if (l <= 0) continue;
            for (k = 0; k < n && lab[k] != l; k++) {}
            if (k == n) {
                if (n == BASE_LABELS) continue;
                lab[n] = l;
                cnt[n++] = 0;
            }
            cnt[k]++;
        }
    for (k = 0; k < n; k++)
        if (best < 0 || cnt[k] > cnt[best]) best = k;
    return best < 0 ? 0 : lab[best];
}

/* THE TARGETS' 32-px blocks into tx/tz: 0 none */
static int reach_targets(const IMPAI_DEC* d, int* tx, int* tz)
{
    const IMPAI_MIL* mil = d->in->mil;
    WQ_START st[PLANT_STARTS];
    const int64_t ax = (int64_t)d->in->st->ax * 16 + 8, az = (int64_t)d->in->st->az * 16 + 8;
    int ns = 0, own = 0, k, n = 0;
    if (!impai_est_known(mil) && (ns = d->q->ops->starts(d->q, st, PLANT_STARTS)) >= 2) {
        for (k = 1; k < ns; k++)
            if ((st[k].x - ax) * (st[k].x - ax) + (st[k].z - az) * (st[k].z - az) <
                (st[own].x - ax) * (st[own].x - ax) + (st[own].z - az) * (st[own].z - az))
                own = k;
        for (k = 0; k < ns; k++)
            if (k != own) {
                tx[n] = st[k].x >> 5;
                tz[n++] = st[k].z >> 5;
            }
        return n;
    }
    if (mil->ebx == IMPAI_NO_ANCHOR) return 0;
    tx[0] = mil->ebx;
    tz[0] = mil->ebz;
    return 1;
}

static uint32_t reach_mask_of(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int tx[PLANT_STARTS], tz[PLANT_STARTS], nt, k, ox, oz;
    unsigned c;
    if (d->reach_done) return d->reach_mask;
    d->reach_done = 1;
    d->reach_mask = 0xFFFFFFFFu;
    if (!in->reach || !in->mil || in->st->ax == IMPAI_NO_ANCHOR || !(nt = reach_targets(d, tx, tz)))
        return d->reach_mask;
    for (c = 0; c < WQ_MCLASSES; c++) {
        const int a = base_label(in->reach, c, in->st->ax >> 1, in->st->az >> 1);
        int any = 0;
        if (a < 0) continue;
        for (k = 0; k < nt && a > 0 && !any; k++)
            any = impai_reach_near(in->reach, c, (uint16_t)a, tx[k], tz[k], REACH_RINGS, &ox, &oz);
        if (!any) d->reach_mask &= ~(1u << c);
    }
    return d->reach_mask;
}

static int reaches(const IMPAI_DEC* d, uint32_t mask, unsigned type)
{
    const IMPAI_TINFO* t = &d->in->ti[type];
    return (t->flags & IMPAI_TF_VTOL) || t->mclass >= WQ_MCLASSES || (mask >> t->mclass & 1u);
}

/* THE METAL'S REACH. A constructor is worth its extractors only where it reaches the metal: the seat grows its
   income by the patches its constructors can stand at, and a map whose metal lies where the plants' constructors
   cannot go holds the seat to what its commander takes. So:
   - A CONSTRUCTOR TYPE REACHES THE METAL when an OPEN patch -- published, free of claims, failures and other
     players' buildings, on THE HELD GROUND (impai_build.c) -- has its anchor's block on the label of the type's
     class at the base (base_label: the label most blocks round the anchor carry; a VTOL type and a class with no
     labels reach every block) and an extractor of the type's own menu fits the patch's ground (choose_mex,
     impai_mex_at: a water extractor where the patch lies deep enough, the land one elsewhere).
   - THE SEAT IS SHORT of reach (impai_econ_metal_short) past its first FINISHED factory, with no plant frame
     standing -- a frame's constructors are not yet the seat's to judge, and a frame interrupted in the opening
     would otherwise read as a seat cut off from the metal on any map; THE ASSIST resumes a frame its builders
     can reach (impai_build.c), and RESIDUAL: one none resumes holds the reach off while it stands --, while
     MORE PATCHES LIE OPEN than
     the seat has extractors, built, framed or ordered, and neither a finished constructor of the seat's (the
     commander apart: THE COMMANDER'S REACH holds it near the anchor) nor a constructor type on the menu of one of
     its finished factories reaches one. The count is the seat's own measure of what it is cut off from: a patch
     on an island, or the last few under a cliff, against a base that holds the rest is not worth a plant, while
     a map whose metal lies mostly beyond reach leaves the seat with fewer extractors than open patches.
   - THE REACHING PLANT. While the seat is short and a plant on its builders' menus, of its top build tech or
     lower, makes a constructor that reaches the metal (impai_econ_reach_plant), a further plant is wanted
     whatever the income (impai_brain.c, economy), and THE CHOICE's further pass takes only such a plant
     (mr_only), whichever rule wanted it: a plant whose constructors reach no open patch adds nothing to the
     seat's metal, and a pass that skips the others before they take a candidate's place cannot run out of
     candidates on them.
   - THE GOVERNOR'S CONSTRUCTOR (choose_cons) is one that reaches the metal before one that does not, while more
     patches lie open than the seat has extractors and no finished constructor of the seat's reaches one.
   WHY, MEASURED 2026-11-15 on Steel Jungle, whose 43 metal patches are all WaterAquaOre lying 64 to 75 under the
   sea (sjmap.py over the core's load of the map): the land classes' content maps reach none of them, the
   commander -- amphibious -- took the one or two within its reach, and in round 3's eight p:L5 games ImpAI held
   one or two extractors for the whole game (a metal income of 2.7 to 8.4 a pass at the end) while the stock AI
   went to sea for six; ImpAI's plants were a vehicle plant and a lab, whose constructors reach no patch, and the
   hover plant's (CORCH, ARMCH) reaches all. The rule is the map's metal against the content's movement classes,
   so on a map whose land constructors reach the land patches it never fires until those are all taken.
   BOUND, a decision: the judgement once -- the published patches, IMPAI_CLUSTERS_HALF at most, and each finished
   factory's menu, one read -- and each constructor type judged at most once (IMPAI_MR_MEMO kept; a type past them
   is judged again when asked), each judgement one menu read of at most WQ_TYPES_MAX types, one base label of
   (2 REACH_RINGS + 1)^2 reads and the published patches, one claim, held, label and ground test each; THE
   REACHING PLANT at most PLANT_LOOK plant menus. RESIDUAL: a water-only plant (a shipyard) is no candidate on a
   dry anchor, as in THE CHOICE, so a seat whose only reaching constructor comes from a shipyard waits on the sea
   plant's own rules. */
static int cons_reaches_metal(IMPAI_DEC* d, unsigned type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_TINFO* t;
    IMPAI_MEX mx;
    int n, k, lab, v;
    if (type >= in->nti) return 0;
    for (k = 0; k < d->nmr; k++)
        if (d->mr_type[k] == type) return d->mr_val[k];
    t = &in->ti[type];
    n = d->q->ops->build_options(d->q, (int)type, d->menu2, WQ_TYPES_MAX);
    if (n <= 0 || !choose_mex(d, d->menu2, n, &mx)) {
        v = 0;
    } else {
        lab = !in->reach || (t->flags & IMPAI_TF_VTOL) || t->mclass >= WQ_MCLASSES
                  ? -1 : base_label(in->reach, t->mclass, in->st->ax >> 1, in->st->az >> 1);
        v = lab != 0 && impai_patch_open_to(d, t->mclass, lab, &mx);
    }
    if (d->nmr < IMPAI_MR_MEMO) {
        d->mr_type[d->nmr] = (uint16_t)type;
        d->mr_val[d->nmr++] = (uint8_t)v;
    }
    return v;
}

static void metal_judge(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int i, k;
    if (d->mr_done) return;
    d->mr_done = 1;
    if (in->st->ax == IMPAI_NO_ANCHOR ||
        !(d->mr_open = impai_patch_open_to(d, 0, -1, NULL) > d->count[IMPAI_C_MEX] + d->pending[IMPAI_C_MEX]))
        return;
    for (k = 0; k < d->nbtype && !d->mr_units; k++)
        d->mr_units = (uint8_t)(in->ti[d->btype[k]].cat == IMPAI_C_CONS && cons_reaches_metal(d, d->btype[k]));
    for (i = 0; i < d->nfidle + d->nfbusy && !d->mr_units && !d->mr_make; i++) {
        const unsigned f = i < d->nfidle ? d->fidle[i].type : d->fbusy[i - d->nfidle].type;
        const int n = d->q->ops->build_options(d->q, (int)f, d->menu3, WQ_TYPES_MAX);
        for (k = 0; k < n && !d->mr_make; k++)
            d->mr_make = (uint8_t)(d->menu3[k] < in->nti && in->ti[d->menu3[k]].cat == IMPAI_C_CONS &&
                                   cons_reaches_metal(d, d->menu3[k]));
    }
}

int impai_econ_metal_short(IMPAI_DEC* d)
{
    if (d->done[IMPAI_C_FACTORY] < 1 || d->count[IMPAI_C_FACTORY] != d->done[IMPAI_C_FACTORY]) return 0;
    metal_judge(d);
    return d->mr_open && !d->mr_units && !d->mr_make;
}

/* plant `type`'s menu, read into plant[], holds a constructor that reaches the metal */
static int plant_reaches_metal(IMPAI_DEC* d, unsigned type, uint16_t* plant, int cap)
{
    const int np = d->q->ops->build_options(d->q, (int)type, plant, cap);
    int j;
    for (j = 0; j < np; j++)
        if (plant[j] < d->in->nti && d->in->ti[plant[j]].cat == IMPAI_C_CONS && cons_reaches_metal(d, plant[j]))
            return 1;
    return 0;
}

int impai_econ_reach_plant(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int wet = anchor_wet(d);
    uint16_t plant[PLANT_MENU];
    int k, j, looked = 0;
    for (k = 0; k < d->nbtype; k++) {
        const int n = d->q->ops->build_options(d->q, d->btype[k], d->menu3, WQ_TYPES_MAX);
        for (j = 0; j < n && looked < PLANT_LOOK; j++) {
            const unsigned t = d->menu3[j];
            if (t >= in->nti || in->ti[t].cat != IMPAI_C_FACTORY || ((in->ti[t].flags & IMPAI_TF_WATER) && !wet) ||
                impai_plant_off(d, (int)t) || in->ti[t].ctech > d->ftop)
                continue;
            looked++;
            if (plant_reaches_metal(d, t, plant, PLANT_MENU)) return 1;
        }
    }
    return 0;
}

/* the tech level the values are read at: the seat's highest build tech, at least 1, at most IMPAI_QTECH */
static unsigned value_tech(const IMPAI_DEC* d)
{
    const unsigned T = d->ftop > 1 ? d->ftop : 1u;
    return T < IMPAI_QTECH ? T : IMPAI_QTECH;
}

/* 1 when `type` is of a gated class with a value above 0, reaching by `mask` */
static int gated_reaching(const IMPAI_DEC* d, uint32_t mask, unsigned type)
{
    const IMPAI_TINFO* t;
    if (type >= d->in->nti) return 0;
    t = &d->in->ti[type];
    return t->klass < IMPAI_KS && (IMPAI_GATED >> t->klass & 1u) && t->value > 0.0f && reaches(d, mask, type);
}

/* the tech level THE CHOICE scores at (THE SCORE): the seat's, and a tech-up pass's (`up`) the level above it, at
   most IMPAI_QTECH. plant_ordered reads it with the pass that found the plant (IMPAI_DEC.plant_up), so the plant
   line's T is the one the scores were read at */
static unsigned choice_tech(const IMPAI_DEC* d, int up)
{
    const unsigned T = value_tech(d) + (up ? 1u : 0u);
    return T < IMPAI_QTECH ? T : IMPAI_QTECH;
}

/* S(P) over P's menu (THE SCORE); *army 1 when the menu holds a land army, a gated type that reaches, whatever the
   values */
static float plant_score(const IMPAI_DEC* d, const uint16_t* menu, int n, unsigned T, uint32_t mask, uint8_t* army)
{
    float s = IMPAI_Q_NONE;
    int k;
    *army = 0;
    for (k = 0; k < n; k++) {
        if (!gated_reaching(d, mask, menu[k])) continue;
        *army = 1;
        if (d->in->tv && d->in->tv[menu[k]].q[T - 1] > s) s = d->in->tv[menu[k]].q[T - 1];
    }
    return s;
}

typedef struct PLANT_CAND { uint16_t type, via; uint8_t ctech, army; float s; } PLANT_CAND;

/* a before b in THE BEST's order; `scored` 0 ranks by the rest alone, and `armed` (the opening on dry ground) puts
   a plant with a land army first. A plant the seat's own builders build goes before one that waits for an unlock
   at an equal score, and always with no values, so the choice with no values is the least value among the plants
   the seat can build now */
static int plant_before(const IMPAI_DEC* d, const PLANT_CAND* a, const PLANT_CAND* b, int scored, int armed)
{
    const float va = d->in->ti[a->type].value, vb = d->in->ti[b->type].value;
    if (scored && a->s != b->s) return a->s > b->s;
    if (armed && a->army != b->army) return a->army;
    if (!a->via != !b->via) return !a->via;
    return va < vb || (va == vb && a->type < b->type);
}

static int type_bit(const uint32_t* bits, unsigned t) { return t < WQ_TYPES_MAX && (bits[t >> 5] >> (t & 31) & 1u); }

/* THE CONSTRUCTOR ROOM: 1 while the governor's caps leave room for another constructor -- below cons_max, and with
   the seat's finished commanders below att_eco under a persona that limits it (THE ATTENTION: the governor keeps
   no more builders than the seat keeps busy, and the attention counts the commander's economy jobs; counting the
   constructors alone kept one more than the attention ever lets work while the commander builds). One predicate for the governor (gov_cons) and THE UNLOCKS (plant_pick), so an unlock is a candidate only
   while the governor can make its constructor: with the unlocks admitted to cons_max and the governor stopping at
   att_eco, a plant that waited for an unlock between the two waited for ever, and while it stood no factory made a
   constructor (THE GOVERNOR). The pressure and the army's share still hold an unlock back below the caps, for as
   long as they last. */
static int cons_room(const IMPAI_DEC* d)
{
    const int64_t c = d->gov_c, att = d->in->persona->att_eco;
    return c < (int64_t)d->in->plan->cons_max && !(att && c + d->done[IMPAI_C_COMMANDER] >= att);
}

/* THE UNLOCKS: the constructor types on the menus of the seat's factory types (fidle, fbusy) of which it has no
   finished unit (IMPAI_DEC.cons_done), into via[], at most PLANT_VIA; their count */
static int plant_unlocks(IMPAI_DEC* d, uint16_t* via)
{
    const IMPAI_BRAIN_IN* in = d->in;
    uint16_t seen[IMPAI_GATE_TYPES];
    int nseen = 0, nv = 0, i, k, j;
    for (i = 0; i < d->nfidle + d->nfbusy && nseen < IMPAI_GATE_TYPES; i++) {
        const unsigned f = i < d->nfidle ? d->fidle[i].type : d->fbusy[i - d->nfidle].type;
        int n, dup = 0;
        for (j = 0; j < nseen; j++) dup |= seen[j] == f;
        if (dup) continue;
        seen[nseen++] = (uint16_t)f;
        n = d->q->ops->build_options(d->q, (int)f, d->menu3, WQ_TYPES_MAX);
        for (k = 0; k < n && nv < PLANT_VIA; k++) {
            const unsigned t = d->menu3[k];
            if (t >= in->nti || in->ti[t].cat != IMPAI_C_CONS || in->ti[t].value <= 0.0f || type_bit(d->cons_done, t))
                continue;
            for (j = 0, dup = 0; j < nv; j++) dup |= via[j] == t;
            if (!dup) via[nv++] = (uint16_t)t;
        }
    }
    return nv;
}

/* THE KEPT UNLOCK: the constructor the governor makes first this decision, 0 none (while one is a frame, none) */
static unsigned unlock_kept(IMPAI_DEC* d)
{
    IMPAI_BRAIN* st = d->in->st;
    if (d->plant_done) {
        st->unlock = d->plant_t || d->plant_tw ? d->plant_via : 0;
        st->unlock_plant = st->unlock ? (d->plant_t ? d->plant_t : d->plant_tw) : 0;
    } else if (st->unlock) {
        uint16_t via[PLANT_VIA];
        const int n = plant_unlocks(d, via);
        int k, still = 0;
        for (k = 0; k < n; k++) still |= via[k] == st->unlock;
        if (!still) st->unlock = st->unlock_plant = 0;
    }
    return type_bit(d->cons_frame, st->unlock) ? 0 : st->unlock;
}

/* One pass of THE CHOICE, a tech-up's when `up`: its plant, 0 none, the constructor it needs first (THE UNLOCKS)
   into *via, 0 none, and its runner-up and their scores. A plant is read once (looked[]) and takes a candidate's
   place only when it passes the filters a single plant can fail -- its constructor, and a tech-up's build tech --
   so a plant that can never be chosen holds no place. BOUND: at most PLANT_LOOK plant menus read,
   IMPAI_PLANT_CANDS kept */
static int plant_pick(IMPAI_DEC* d, int waived, int up, uint16_t* via, uint16_t* next, float* s, float* next_s)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int wet = anchor_wet(d);
    const int scored = in->tv && !wet && d->count[IMPAI_C_FACTORY] > 0;
    const int armed = !wet && d->count[IMPAI_C_FACTORY] == 0;
    const unsigned T = choice_tech(d, up);
    const uint32_t mask = reach_mask_of(d);
    PLANT_CAND c[IMPAI_PLANT_CANDS];
    uint16_t plant[PLANT_MENU], src[IMPAI_SEA_BUILDERS + PLANT_VIA], looked[PLANT_LOOK];
    int nc = 0, nl = 0, b, k, j, best = -1, second = -1, nsrc = d->nbtype, nown = d->nbtype;
    unsigned lo = 256;
    memcpy(src, d->btype, (size_t)d->nbtype * sizeof src[0]);
    if (d->count[IMPAI_C_FACTORY] > 0 && cons_room(d)) nsrc += plant_unlocks(d, src + nsrc);
    for (b = 0; b < nsrc; b++) {
        const int n = d->q->ops->build_options(d->q, src[b], d->menu3, WQ_TYPES_MAX);
        for (k = 0; k < n && nc < IMPAI_PLANT_CANDS && nl < PLANT_LOOK; k++) {
            const unsigned t = d->menu3[k];
            int seen = 0, np, ok = 0;
            if (t >= in->nti || in->ti[t].cat != IMPAI_C_FACTORY) continue;
            for (j = 0; j < nl; j++) seen |= looked[j] == t;
            if (seen || ((in->ti[t].flags & IMPAI_TF_WATER) && !wet) || (!waived && impai_plant_off(d, (int)t)) ||
                (up && in->ti[t].ctech <= d->ftop))
                continue;
            looked[nl++] = (uint16_t)t;
            np = d->q->ops->build_options(d->q, (int)t, plant, PLANT_MENU);
            for (j = 0; j < np && !ok; j++)
                ok = plant[j] < in->nti && in->ti[plant[j]].cat == IMPAI_C_CONS && builds_mex(d, plant[j]);
            if (!ok || (d->mr_only && !up && !plant_reaches_metal(d, t, plant, PLANT_MENU))) continue;
            c[nc].type = (uint16_t)t;
            c[nc].via = (uint16_t)(b < nown ? 0 : src[b]);
            c[nc].ctech = in->ti[t].ctech;
            c[nc].s = plant_score(d, plant, np, T, mask, &c[nc].army);
            if (c[nc].ctech < lo) lo = c[nc].ctech;
            nc++;
        }
    }
    for (k = 0; k < nc; k++) {
        if (!up && c[k].ctech > (d->ftop > lo ? d->ftop : lo)) continue;
        if (best < 0 || plant_before(d, &c[k], &c[best], scored, armed)) {
            second = best;
            best = k;
        } else if (second < 0 || plant_before(d, &c[k], &c[second], scored, armed))
            second = k;
    }
    *via = best >= 0 ? c[best].via : 0;
    *next = second >= 0 ? c[second].type : 0;
    *s = best >= 0 ? c[best].s : IMPAI_Q_NONE;
    *next_s = second >= 0 ? c[second].s : IMPAI_Q_NONE;
    return best >= 0 ? c[best].type : 0;
}

/* THE CHOICE, once a decision: the plant, else the waived pass's (the cramped exit, THE PLANT'S FALLBACK); the
   tech-up's two passes first when it is due, then the further plant's when that is wanted too (THE TECH-UP
   FIRST). At most four passes. */
static void plant_choice(IMPAI_DEC* d)
{
    uint16_t next, via;
    float s, next_s;
    int up;
    d->plant_done = 1;
    for (up = d->want_tech ? 1 : 0; up >= 0 && (up || d->want_factory); up--) {
        d->plant_up = up;
        d->plant_t = (uint16_t)plant_pick(d, 0, up, &d->plant_via, &d->plant_next, &d->plant_s, &d->plant_next_s);
        if (d->plant_t) return;
        d->plant_tw = (uint16_t)plant_pick(d, 1, up, &via, &next, &s, &next_s);
        if (!d->plant_tw) continue;
        d->plant_via = via;
        d->plant_next = next;
        d->plant_s = s;
        d->plant_next_s = next_s;
        return;
    }
}

static int on_menu(const uint16_t* menu, int n, unsigned type)
{
    int k;
    for (k = 0; k < n; k++)
        if (menu[k] == type) return 1;
    return 0;
}

/* the plant ordered, for the plant line (IMPAI_BRAIN_OUT.plant*), before the wants are cleared */
static void plant_ordered(IMPAI_DEC* d, int type)
{
    IMPAI_BRAIN_OUT* o = d->out;
    o->plant = (uint16_t)type;
    o->plant_next = d->plant_next;
    o->plant_s = d->plant_s;
    o->plant_next_s = d->plant_next_s;
    o->plant_want = (uint8_t)(d->count[IMPAI_C_FACTORY] == 0 ? 1 : d->plant_up ? 3 : 2);
    o->plant_T = (uint8_t)choice_tech(d, d->plant_up);
    o->plant_reach = reach_mask_of(d);
}

/* THE SEA PLANT's type: the least value among the menu's water factories (impai_water_plant) whose own menu
   offers a sea army type (klass IMPAI_K_SRAIDER..IMPAI_K_SAA, a value above 0), ties to the lower type; at most
   FACTORY_LOOK candidates examined, each a menu read of at most PLANT_MENU types. Writes neither d->menu nor
   d->menu2. Sea army types are bought only where a factory offers them (THE ARMY ORDERS), so this is the plant
   that can make them. */
static int choose_sea_plant(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    uint16_t plant[PLANT_MENU];
    int k, j, best = 0, looked = 0;
    for (k = 0; k < n && looked < FACTORY_LOOK; k++) {
        const int t = menu[k];
        int np, ok = 0;
        if (!impai_water_plant(in, (unsigned)t)) continue;
        if (best && (in->ti[t].value > in->ti[best].value || (in->ti[t].value == in->ti[best].value && t > best))) continue;
        looked++;
        np = d->q->ops->build_options(d->q, t, plant, (int)(sizeof plant / sizeof plant[0]));
        for (j = 0; j < np && !ok; j++)
            ok = plant[j] < in->nti && in->ti[plant[j]].klass >= IMPAI_K_SRAIDER && in->ti[plant[j]].klass <= IMPAI_K_SAA &&
                 in->ti[plant[j]].value > 0.0f;
        if (ok) best = t;
    }
    return best;
}

int impai_econ_sea_able(IMPAI_DEC* d)
{
    int k, n;
    for (k = 0; k < d->nbtype; k++)
        if ((n = d->q->ops->build_options(d->q, d->btype[k], d->menu, WQ_TYPES_MAX)) > 0 && choose_sea_plant(d, d->menu, n))
            return 1;
    return 0;
}

/* an anti-air structure: the cheapest of the menu's (impai_aa_struct), ties to the lower type */
static int choose_aa(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int k, best = 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        if (!impai_aa_struct(in, (unsigned)t)) continue;
        if (!best || in->ti[t].value < in->ti[best].value || (in->ti[t].value == in->ti[best].value && t < best))
            best = t;
    }
    return best;
}

/* THE GROUND DEFENCES. While THE PRESSURE stands at the base (impai_mil.c) the ladder builds a ground defence
   (impai_def_struct) before an anti-air structure: one, and one more for each def_v of pressing value, at most
   def_max (impai_brain.c, economy), on the ring site nearest THE DEFENCE'S POINT, the anchor moved rally_px
   towards the pressing member (impai_build_def). Its site and its walk obey the ring's threat rules like any
   other: a site inside known ground threat is skipped, so the tower stands at the threat's edge, and a missile
   tower's range covers it from there. No defence is built before a raid: the governor's army is the seat's
   answer to what it has not seen yet.
   THE TYPE is the one worth most against what the seat sees, by cost:
       score(t) = sum over the picture's enemy types e of share_e x rate(t, e) x [reach_t >= reach_e] / value_t
   with rate the fight predictor's pair rate (HP a tick), share the seen enemy mobile value's, reach the ground
   reach (IMPAI_THREAT.hit_range) and value the type's. A tower the enemy outranges takes its damage and returns
   none, so its rate counts only against what it outranges. Ties to the lower type; a water-only type only on a
   wet anchor; only a score above 0 qualifies, so with no picture or no predictor no defence is built. No unit
   list: a mod's towers are scored by the same rule. OFFLINE, against the opponent's T1 lab and vehicle-plant
   army at equal shares: retail ARM takes ARMRL (7.19 against ARMLLT 6.57), CORE CORRL (7.32 against CORLLT
   4.91), the LLT winning both without the reach term; Total Mayhem ARM ARMRL, CORE CORHLT (6.99 against CORRL
   6.84); ProTA ARM ARMRL, CORE CORPLAS.
   WHY, MEASURED: 895 constructor frames killed on the yard at the bar's second start (2026-11-13), and no seat
   ever built a ground defence. BARb's AiMakeDefence answers a mobile threat with a tower, read for its design
   only (GPL v2: no code taken). BOUND: def_max, at most 4 (impai_brain_test.c, check_plan); the score walks the
   menu (WQ_TYPES_MAX) x the picture (IMPAI_PIC_ENEMY). */
static int choose_def(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_PICTURE* pic = in->mil ? &in->mil->pic : NULL;
    const uint32_t ne = pic && pic->ne <= IMPAI_PIC_ENEMY ? pic->ne : 0;
    const int wet = anchor_wet(d);
    float best_s = 0.0f;
    int k, best = 0;
    uint32_t e;
    if (!in->pred || !in->pred->rate || !ne) return 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        float s = 0.0f;
        if (!impai_def_struct(in, (unsigned)t) || !(in->ti[t].value > 0.0f)) continue;
        if ((in->ti[t].flags & IMPAI_TF_WATER) && !wet) continue;
        for (e = 0; e < ne; e++) {
            const unsigned et = pic->e[e].type;
            if (et >= (unsigned)in->view->ntypes || !(pic->e[e].share > 0.0f)) continue;
            if (in->view->threat[t].hit_range >= in->view->threat[et].hit_range)
                s += pic->e[e].share * in->pred->rate(in->pred->ctx, (uint16_t)t, (uint16_t)et);
        }
        s /= in->ti[t].value;
        if (s > best_s) {
            best_s = s;
            best = t;
        }
    }
    return best;
}

/* THE SENSORS. Past its sight a seat knows only its radar and sonar contacts (tagpu_worldq.c, THE SEAT'S RADAR
   PASS: its own acquisition's), which feed its picture, the threat maps, the lead's evidence and the hunts; its
   commander's and its army's own distances reach little past their sight. So once its factory_max plants are
   finished the ladder stands sensor structures (impai_sensor_struct) by COVERAGE, NOT BY COUNT: a point is covered when it lies
   within three quarters of the distance of a building of the seat's with a sensor of its kind, built, framed or
   ordered (the census, sens: a plant with a radar covers too), and the want (impai_brain.c, sensor_want) is the
   first point not covered of: the anchor; the finished extractors, nearest the anchor first; the front, the
   anchor moved half way to the estimate of the enemy base, at most base_r; on a map with water a sea army class
   can use, the anchor for sonar. One a decision, at most sensor_max standing, none in a stall or the hold, and a
   tower lost is wanted again by the same rule. ONE TRY A DECISION: the first builder that tries the want ends it,
   ordered or not, so a point no site serves does not spend every builder's site tests; the commander tries the
   base ring alone and leaves the want to a constructor. A sensor is no army: the classes are mobile types', and
   a sensor stands where it is built. THE PLANTS FIRST: the plants the
   pacing asks for are the economy's, and a sensor bought before them delayed them -- the second plant 960 and 1740
   ticks later on Red Triangle ac-1 and Trout Farm ca-1 with the want past the first factory (MEASURED 2026-11-15,
   b5's two yard games to 12 000).
   THE TYPE is the menu's sensor of the want's kind with the most coverage per value, distance^2 / value, ties to
   the lower type, whose energy upkeep (energyuse) the seat's flows pay -- income less spending, before it is
   built; once it runs, its upkeep is in the spending the energy rung keeps the income above (THE LADDER, 1.1
   times energy_out) --, and a water-only type only on a wet anchor, where it takes the base ring alone. No unit
   list: a content's sensors are its types with a radardistance or a sonardistance and no weapon.
   THE SITE (impai_build.c, impai_build_sensor) covers the point BY CONSTRUCTION: a site whose footprint centre
   lies past three quarters of the type's distance from the point is never taken. The base ring's sites first,
   nearest the point first; then a ring around the point itself, on held ground only.
   WHY: the brain had no rule for a sensor, and no seat ever built one (MEASURED 2026-11-15: the owner's game of
   three Hard seats at 57d39d71, 20 minutes, no radar built), so beyond its sight a seat knew what its commander's
   own radar reached. BOUND: the menu (WQ_TYPES_MAX), one economy-keys read for each sensor type of it. */
static int choose_sensor(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const float room = d->e.energy_in - d->e.energy_out;
    const int wet = anchor_wet(d);
    float best_s = 0.0f;
    int k, best = 0;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        const IMPAI_TINFO* ti;
        WQ_DEF_ECON e;
        uint32_t r;
        float s;
        if (!impai_sensor_struct(in, (unsigned)t)) continue;
        ti = &in->ti[t];
        r = d->want_sensor == IMPAI_SENSE_SONAR ? ti->sonar : ti->radar;
        if (!r || !(ti->value > 0.0f)) continue;
        if ((ti->flags & IMPAI_TF_WATER) && !wet) continue;
        if (!d->q->ops->def_econ(d->q, t, &e) || !(e.energyuse <= 0.0f || e.energyuse <= room)) continue;
        s = (float)r * (float)r / ti->value;
        if (s > best_s || (s == best_s && best && t < best)) {
            best_s = s;
            best = t;
        }
    }
    return best;
}

/* a factory's constructor: the most workertime per value among those whose own menu holds an extractor; while more
   patches lie open than the seat has extractors and no finished constructor of the seat's reaches one, a
   constructor that reaches the metal before one that does not (THE METAL'S REACH) */
static int choose_cons(IMPAI_DEC* d, const uint16_t* menu, int n)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int k, best = 0, best_r = 0, pref;
    metal_judge(d);
    pref = d->mr_open && !d->mr_units;
    for (k = 0; k < n; k++) {
        const int t = menu[k];
        const IMPAI_TINFO* ti;
        float lhs, rhs;
        int r;
        if (t >= (int)in->nti || in->ti[t].cat != IMPAI_C_CONS || in->ti[t].value <= 0) continue;
        ti = &in->ti[t];
        r = pref && cons_reaches_metal(d, (unsigned)t);
        if (best && r != best_r) {
            if (r < best_r) continue;
        } else if (best) {
            lhs = (float)ti->workertime * in->ti[best].value;
            rhs = (float)in->ti[best].workertime * ti->value;
            if (lhs < rhs || (lhs == rhs && t > best)) continue;
        }
        if (builds_mex(d, t)) {
            best = t;
            best_r = r;
        }
    }
    return best;
}

/* ---- 4. the builders ---- */

/* A METAL MAP (impai_metal_map): the threshold is 255 (SurfaceMetal, game state +0xD30, held to 255), so no cell can stand above
   it and the patch scan publishes no patch whatever the seat explores; the loader seeds every cell with the
   background (0x4839B3..0x4839E6, engine/map.md), and an extractor yields extractsmetal x its footprint's metal
   wherever it stands. Core Prime Industrial Area is one: SurfaceMetal 255, no metal feature, every cell 255.
   The stock AI places its extractors there by its general site search: it takes the spot search only when
   SurfaceMetal is below a draw of 0x4B6C30(255), 0..254 (0x40C150..0x40C16C), and the general search 0x40A5D0
   refuses a site only when its footprint holds more than twice the background over its area
   (0x40A5EC..0x40A602, 0x40A76F), which a footprint of 255s never does (DISASSEMBLED). So does ImpAI: the
   extractor goes to the base ring like any other building, and the ladder alone decides how many. */
int impai_metal_map(const IMPAI_DEC* d) { return d->m.threshold >= 255; }

/* THE LADDER for builder b, the first rule with a type in b's menu and a site winning; a rule that finds no
   site falls through. A HELPER (a builder assisting) answers the rules of a want the seat has now -- the stall's
   energy, the plant (the sea plant, the next factory or the tech-up), the ground defence and the anti-air -- and
   the reclaim, and keeps its assist for the rest: a help adds its build power to work under way, which is worth
   more than an extractor or an energy building the ladder would pace anyway, while a want waits for a builder, and
   a seat whose constructors all help -- the factories' products, frame after frame -- otherwise answers none of
   them. impai_econ_assign asks a helper only while one of those wants stands. The second
   is the factory: while the seat wants its sea plant (impai_brain.c, economy) a builder whose menu offers one
   searches for its site, and a land plant waits; a search that finds no site for any sea plant ends the want,
   so the next factory is a land one, in the same decision. A builder with no sea plant on its menu builds no
   factory while the want lasts: another builder offers one (impai_econ_sea_able). A builder whose far record is
   live takes neither rule (impai_brain.c, THE FAR RECORD), so the plant goes to one that can walk. An
   extractor's site is a patch, or on a metal map the ring, and its type is the one for the ground there -- the
   patch's centre, or on a metal map the anchor (impai_build.c, THE EXTRACTOR FOR THE PATCH). A constructor
   explores only when no patch is left for it -- none free, reachable on its class, unthreatened and with an
   extractor of its menu for its ground -- so a patch it can never reach or build on does not keep it home.
   THE RECLAIM (impai_build.c) is asked twice, each time for a feature worth more than what the builder would
   do instead, and never before the plants or the seat's first open_mex extractors:
   - BEFORE AN EXTRACTOR, once open_mex are finished: a feature wins over the extractor b has a site for when it
     pays b more metal a tick than an extractor makes, the seat's steady metal income a pass (impai_brain.c,
     THE STEADY INCOME) over its finished extractors (the share pm_in projects by) over the pass's 30 ticks
     (engine/weapons.md, The settle). A
     reclaim pays in ticks what an extractor makes in minutes -- the countdown has no worker-time term and runs
     about half a tick a metal (engine/orders.md) -- so while the store has room a builder clears the features near it
     first; a full store makes every feature worth nothing, and the extractors come back.
   - BEFORE THE BUILDINGS THAT SPEND, while the seat is short of metal (impai_brain.c, economy: m_short): any
     feature with worth, ahead of energy, makers, exploring and the assist (for a builder with no menu, the
     assist alone). A helper's assist is a build's help, so a seat short of metal takes a helper to a feature as
     well.
   IN A SURPLUS THE METAL PRODUCERS WAIT, past the opening (surplus_waits): the extractor rule (its reclaim before
   the extractor included) and the maker rule are skipped while the store is at SURPLUS_FRAC of its maximum or
   more (impai_brain.c, hold_read) AND the income already pays for a further plant -- below factory_max,
   factory_per_m a pass for each factory, at least one, want_factory's own income test; at factory_max or more,
   THE PLANT'S PRICE's overflow, the seat making a busy plant's request a pass more than it asks (impai_brain.c,
   economy). While the store is at its maximum the economy pass adds every further unit of
   metal to MetalWasted, the double at player +0xD4, holding the store +0x98 to the storage +0xA8
   (0x401AF0..0x401B27, DISASSEMBLED), so an extractor or a maker built then adds only waste. The extractor's
   type is still chosen, so a constructor explores only when no patch is left for it, as outside a surplus. The
   ladder reaches energy first, at its unchanged target, then exploring and the assist, which helps the factories'
   products and adds its worker time to the spending. ONE THRESHOLD, NO HYSTERESIS: below SURPLUS_FRAC the
   extractors come back by the same test. Its 80 % is a chosen margin below the full store, where nothing is
   wasted yet, so the producers wait before the waste begins rather than after.
   INVARIANT, a bound: whenever the extractor and maker rules wait, the seat already makes more metal than it asks
   by the second factory's income or by a busy plant's measured request (impai_brain.c, IN A SURPLUS THE FACTORIES
   ARE PACED BY WHAT THEY SPEND), so the full store has a spender: the next plant, the energy it waits for
   (plant_e_need), or the factories standing, which the governor keeps busy as the persona's actions allow. Below
   that income no plant comes, the extractors are the only way to it, and they never wait; with no factory asking
   metal -- one waiting at its yard -- there is no price, and they never wait either.
   WHY THE INCOME AND NOT THE STORE ALONE: the waste the rule is for is a flow, income above spending, and a store is
   a level. On Trout Farm a seat with one factory and an income of 3.3-3.8 a pass against the plant's 8 kept its
   store full on reclaim payouts (4 887 of 10 929 metal by GameTime 52 207) while its one factory waited on a
   blocked yard (THE YARD, impai_brain.c), so it never built its second extractor and held one for 40 000 ticks
   (MEASURED 2026-11-14, the closing run's four Trout Farm games, mex 1 in each). WHY: in long games against the stock AI 34-53 % of the metal produced
   was wasted while energy stayed short; on Pin Cushion at GameTime 36 000 the seat had 71 extractors, 36 energy
   buildings, 3 factories and 2 makers, energy 109 of 2 050, +427 -619 a pass (MEASURED 2026-11-14 from the
   recordings' player images). The makers already built are switched by THE MAKERS. */
/* 1 when the metal producers wait (IN A SURPLUS THE METAL PRODUCERS WAIT): a surplus past the opening -- the seat's
   open_mex extractors finished and open_energy energy buildings standing, or open_t reached, the first factory's
   own terms (impai_brain.c, economy) less THE BLIND START, which wants the plant without ending the opening --, with the steady income at factory_per_m a pass for
   each factory, at least one. A seat's store starts full -- 1000 of 1000 in impai_brain_test's economy game and the
   replay scenarios -- so without the opening's exemption the rule would hold back the very extractors the opening
   is for (the first at GameTime 3282 against 529 in impai_brain_test's economy game) */
static int surplus_waits(const IMPAI_DEC* d)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const int nf = d->count[IMPAI_C_FACTORY] > 1 ? d->count[IMPAI_C_FACTORY] : 1;
    return d->surplus && ((d->done[IMPAI_C_MEX] >= plan->open_mex && d->count[IMPAI_C_ENERGY] >= plan->open_energy) ||
                          d->now >= plan->open_t) &&
           ((d->count[IMPAI_C_FACTORY] < plan->factory_max && d->m_steady >= d->factory_per_m * (float)nf) ||
            d->over_m);
}

/* THE MAKERS: the seat's makers are switched off and on with the engine's own orders, so energy is traded for metal
   only while the metal is wanted and the energy is spare.
   - THE ENGINE. The orders Activate 0x403010 and Deactivate 0x403040 -- the on and off button's ACTIVATE and
     DEACTIVATE, resolved through 0x438760 (0x41A804, 0x41A83D) -- call the state setter 0x48B090(1, 1) or (1, 0)
     when the def is onoffable (+0x245 bit 2; 0x40301A..0x40302C, 0x40304A..0x40305C) and return 5, the node done;
     0x48B090 sets +0x10E bit 0 and starts the script's Activate or Deactivate. The economy pass charges a
     structure's energyuse and pays its makesmetal only while that bit is set (0x4013C5 jumps past both to
     0x4016C5), and pays makesmetal only on a granted request (0x4014CA..0x4014F7). A remote copy takes +0x10E
     through the 0x11 receiver and the round robin (engine/cob.md, COB on a puppet). DISASSEMBLED 2026-11-14.
   - THE RULE, on the store levels in thousandths of the storage (whole units, metal_whole's bound) and the last
     pass's flows:
     OFF, the least metal per energy first (mgain x egain compared crosswise, ties to the lower slot), while
     energy is short -- the store below MK_E_LOW and the energy made below the energy asked -- until the makers
     switched off cost the shortfall, and while metal is full -- the store at MK_M_FULL or more and m_steady above
     the metal asked -- until they made the overflow: a maker's metal past the store's maximum is MetalWasted
     (0x401AF0..0x401B27) and its energy is the builds'. ON, the most metal per energy first, while energy is
     spare -- the store at MK_E_HIGH or more, the energy made above the energy asked -- and the metal has room (no
     surplus): each one whose energy cost the spare still pays.
   - HYSTERESIS, by construction: OFF needs the energy store below MK_E_LOW or the metal store at MK_M_FULL, and ON
     needs the energy store at MK_E_HIGH or more and the metal store below SURPLUS_FRAC, so no decision holds both
     and a maker switched by one store's crossing comes back only after the store has crossed the band to the
     other side. A maker switched on is paid for by the spare it was switched on with, and one switched off frees
     what the shortfall asked, so a switch does not of itself bring the store back across. And a maker switched
     either way waits MK_DWELL ticks before its next switch (IMPAI_JOB_SWITCH, `since`): a maker whose own script
     turns it back on (SET ACTIVATION, engine/cob.md) is switched at most once in MK_DWELL, never once a decision.
   - BOUND: at most MK_SWITCH_MAX switches a decision, each one root within root_cap less the root THE RELEASE
     keeps (impai_release_keep: IT IS NEVER STARVED BY A BUDGET), over the census's
     IMPAI_MAKERS_MAX makers; a maker's flags and costs are its type's (IMPAI_TINFO), its activation bit the
     WORLDQ's. A recorded `since` past now reads as an old switch, so a restored job delays one maker at most
     MK_DWELL.
   - THE FLOWS' LAG, an ordering: the flows are the last pass's, and a seat decides up to twice a pass (look_eco
     15 at Hard against the pass's 30 ticks), so each maker switched since that pass is put into them before the
     rule reads them (IMPAI_DEC.mk_lag_e, mk_lag_m, the census's): an active maker whose ledger asked no energy in
     the last pass (+0xD0, WQ_UNIT.req_e) was off through it, so its energyuse is added to the energy asked and its
     metal to the metal made; an inactive one that asked was on through it, so they are taken off. The pass
     charges a structure's EnergyUse to the energy asked only while it is active (0x4013C5, 0x4013ED..0x401421;
     engine/weapons.md, the economy pass), so the ask and the bit disagree exactly for a maker switched since the
     pass. Without it the decision after a switch reads the same shortfall or spare and switches a second batch
     for it, about twice the energy. Each corrected flow is held at 0 or more.
   - The ladder's maker rule builds no maker while one stands off, and counts the flows' lag and the energy this
     decision switched on (THE LADDER); the plants' energy term reads the lag too (impai_brain.c, economy).
   WHY, MEASURED 2026-11-14 from the recordings, GameTime 9 000 to the end: the owner's game on Comet Catcher, its
   ImpAI seat 3 with 5 makers, held its metal store at 95 % or more 76 % of the time while in an energy stall 30 %
   of it, and wasted 26 % of the metal it made. RESIDUAL: a maker past the census's IMPAI_MAKERS_MAX is counted in
   the costs but never switched; a maker that is not onoffable, or costs no energy, is left as it stands. */
#define MK_E_LOW      300       /* thousandths of the energy storage: below it, with energy falling, OFF          */
#define MK_E_HIGH     600       /* at or above it, with energy rising and metal room, ON                          */
#define MK_M_FULL     950       /* thousandths of the metal storage: at or above it, with metal overflowing, OFF */
#define MK_SWITCH_MAX 4
#define MK_DWELL      300       /* ticks: ten game seconds                                                        */

static int64_t whole(float v) { return v >= 1.0f ? (v < 1e9f ? (int64_t)v : 1000000000) : 0; }

/* thousandths of the storage the store holds; -1 with no storage */
static int64_t per_mille(float store, float storage)
{
    const int64_t m = whole(storage);
    return m > 0 ? whole(store) * 1000 / m : -1;
}

/* 1 when maker a gives less metal per energy than maker b (ties: a the higher slot) */
static int mk_worse(const IMPAI_DEC* d, const IMPAI_UNITREF* a, const IMPAI_UNITREF* b)
{
    const IMPAI_TINFO* ta = &d->in->ti[a->type];
    const IMPAI_TINFO* tb = &d->in->ti[b->type];
    const double l = (double)ta->mgain * tb->egain, r = (double)tb->mgain * ta->egain;
    return l < r || (l == r && a->slot > b->slot);
}

void impai_econ_makers(IMPAI_DEC* d)
{
    const WQ_ECON* e = &d->e;
    const int64_t es = per_mille(e->energy, e->energy_max), ms = per_mille(e->metal, e->metal_max);
    const float e_out = e->energy_out + d->mk_lag_e > 0.0f ? e->energy_out + d->mk_lag_e : 0.0f;
    const float m_made = d->m_steady + d->mk_lag_m > 0.0f ? d->m_steady + d->mk_lag_m : 0.0f;
    float need_e = es >= 0 && es < MK_E_LOW && e->energy_in < e_out ? e_out - e->energy_in : 0.0f;
    float need_m = ms >= MK_M_FULL && m_made > e->metal_out ? m_made - e->metal_out : 0.0f;
    float spare = es >= MK_E_HIGH && !d->surplus && e->energy_in > e_out ? e->energy_in - e_out : 0.0f;
    const int keep = impai_release_keep(d);
    uint8_t tried[IMPAI_MAKERS_MAX];
    int k;
    if (d->act_on < 0 || d->act_off < 0 || !d->nmk) return;
    memset(tried, 0, sizeof tried);
    for (k = 0; k < MK_SWITCH_MAX && d->roots + keep < d->root_cap; k++) {
        const int off = need_e > 0.0f || need_m > 0.0f;
        const IMPAI_UNITREF* m;
        IMPAI_JOB* j;
        WQ_CMD c;
        int i, best = -1;
        if (!off && !(spare > 0.0f)) break;
        for (i = 0; i < d->nmk; i++) {
            const IMPAI_JOB* ji = &d->in->job[d->mk[i].slot];
            if (tried[i] || d->mk_active[i] != off) continue;
            if (ji->kind == IMPAI_JOB_SWITCH && d->now - ji->since < MK_DWELL) continue;
            if (!off && d->in->ti[d->mk[i].type].egain > spare) continue;
            if (best < 0 || (off ? mk_worse(d, &d->mk[i], &d->mk[best]) : mk_worse(d, &d->mk[best], &d->mk[i])))
                best = i;
        }
        if (best < 0) break;
        tried[best] = 1;
        m = &d->mk[best];
        memset(&c, 0, sizeof c);
        c.verb = WQ_CMD_NEW;
        c.action = (uint8_t)(off ? d->act_off : d->act_on);
        c.unit = m->slot;
        c.target = (uint16_t)WQ_NONE;
        memcpy(c.pos, m->pos, sizeof c.pos);
        if (impai_give(d, &c) != WQ_OK) continue;
        d->out->orders++;
        j = &d->in->job[m->slot];
        impai_job_end(j);
        j->type = m->type;
        j->kind = IMPAI_JOB_SWITCH;
        j->since = d->now;
        d->mk_active[best] = (uint8_t)!off;
        if (off) {
            const IMPAI_TINFO* t = &d->in->ti[m->type];
            need_e -= t->egain;
            need_m -= t->mgain;
            d->mk_on_e -= t->egain;
            d->mk_off_n++;
            d->out->makers_off++;
        } else {
            const float eg = d->in->ti[m->type].egain;
            spare -= eg;
            d->mk_on_e += eg;
            d->mk_new_e += eg;
            d->mk_off_n--;
            d->out->makers_on++;
        }
    }
}

/* THE LADDER's energy target, a pass: the economy's keep (e_per_m of the projected metal), 1.1 x what the seat asks,
   and with a surplus plant held by energy alone (impai_brain.c, THE PLANT'S PRICE) the energy made plus what the
   plant is short of, whichever is most */
static float energy_target(const IMPAI_DEC* d, float keep)
{
    float t = keep > 1.1f * d->e.energy_out ? keep : 1.1f * d->e.energy_out;
    if (d->plant_e_need > 0.0f && d->e.energy_in + d->plant_e_need > t) t = d->e.energy_in + d->plant_e_need;
    return t;
}

static int assign(IMPAI_DEC* d, const IMPAI_UNITREF* b, int helper)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_TINFO* bt = &in->ti[b->type];
    const int n = d->q->ops->build_options(d->q, b->type, d->menu, WQ_TYPES_MAX);
    const float keep = d->e_per_m * d->pm_in;
    const int far = impai_far(d, b);
    IMPAI_MEX mx = { 0, 0, 0 };
    int t, have_mex = 0;
    if (n > 0) {
        if (d->e_stall && (t = choose_energy(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->pe_in += in->ti[t].egain;
            return 1;
        }
        if (d->sea_far && !far && !d->hold && !d->m_stall && !d->e_stall && impai_build_sea_frame(d, b)) return 1;
        if (d->want_sea && !far && (t = choose_sea_plant(d, d->menu, n))) {
            const int y = impai_build_sea_plant(d, b, t);
            if (y > 0) {
                d->want_sea = d->want_factory = d->want_tech = 0;
                return 1;
            }
            if (y < 0) d->want_sea = 0;
        }
        if ((d->want_factory || d->want_tech) && !d->want_sea && !far) {
            /* THE CHOICE is the seat's, built by a builder whose menu holds it; with every plant type it would
               take passed over (THE PLANT'S FALLBACK), the waived pass's, the cramped exit. plant_choice reads
               menu2 and menu3 only, so this builder's menu is still d->menu */
            if (!d->plant_done) plant_choice(d);
            if ((d->plant_t && on_menu(d->menu, n, d->plant_t) && impai_build_ring(d, b, (t = d->plant_t))) ||
                (!d->plant_t && d->plant_tw && on_menu(d->menu, n, d->plant_tw) &&
                 impai_build_plant_waived(d, b, (t = d->plant_tw)))) {
                plant_ordered(d, t);
                d->want_factory = d->want_tech = 0;
                return 1;
            }
        }
    }
    if (n > 0) {
        if (d->want_def && (t = choose_def(d, d->menu, n)) && impai_build_def(d, b, t)) {
            d->want_def = 0;
            return 1;
        }
        if (d->want_aa && (t = choose_aa(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->want_aa = 0;
            return 1;
        }
        if (d->want_sensor && (t = choose_sensor(d, d->menu, n))) {
            const int y = impai_build_sensor(d, b, t);
            if (y >= 0) d->want_sensor = 0;
            if (y > 0) {
                d->out->sensors++;
                return 1;
            }
        }
    }
    if (helper) return d->m_short && impai_build_reclaim(d, b, 0.0);
    if (n > 0) {
        if ((have_mex = choose_mex(d, d->menu, n, &mx)) && !surplus_waits(d)) {
            if (d->done[IMPAI_C_MEX] > 0 && d->done[IMPAI_C_MEX] >= in->plan->open_mex &&
                (impai_metal_map(d) || impai_spot_pick(d, b, &mx, NULL)) &&
                impai_build_reclaim(d, b, (double)d->m_steady / (double)d->done[IMPAI_C_MEX] / 30.0))
                return 1;
            if (impai_metal_map(d) ? ((t = impai_mex_at(d, &mx, in->st->ax * 16 + 8, in->st->az * 16 + 8)) &&
                                impai_build_ring(d, b, t))
                             : impai_build_spot(d, b, &mx))
                return 1;
        }
        if (d->m_short && impai_build_reclaim(d, b, 0.0)) return 1;
        if (d->pe_in < energy_target(d, keep) && (t = choose_energy(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->pe_in += in->ti[t].egain;
            return 1;
        }
        if (!surplus_waits(d) && !d->mk_off_n && d->e.energy > 0.8f * d->e.energy_max &&
            (t = choose_maker(d, d->menu, n)) &&
            d->e.energy_in - d->e.energy_out - d->mk_lag_e - d->mk_new_e > in->ti[t].egain &&
            impai_build_ring(d, b, t))
            return 1;
    }
    if (bt->cat == IMPAI_C_CONS && !d->explorer_live &&
        !impai_spot_pick(d, b, have_mex ? &mx : NULL, NULL) && impai_build_explore(d, b))
        return 1;
    /* THE HARVEST: a builder the ladder has nothing else for reclaims outside a surplus, where the metal it brings
       is what the seat spends next, and not in one, where the assist spends faster what the store holds; the
       reclaim's worth is held to the store's room, so a full store harvests nothing. A builder with a menu was
       asked with min_rate 0 above while the seat is short; this is its one other zero-rate ask */
    if ((n > 0 ? !d->m_short && !d->surplus : d->m_short || !d->surplus) && impai_build_reclaim(d, b, 0.0)) return 1;
    return impai_build_assist(d, b);
}

/* Idle builders in slot order, then the helpers for the wants they answer
   (THE LADDER's helper), at most assign_max of them, while the decision's commands last. An idle builder whose BUILD or
   reclaim job this decision did not check waits for the check, which may record its feature as one it could not
   reach. A helper whose job is no longer an assist is skipped: the census lists the helpers before the walk checks
   run (impai_brain.c), and a helper the walk check turned back this decision has had its order, the turn back's
   move, which a second WQ_CMD_NEW here would replace, spending a second root. It is idle at the next census and
   takes the ladder then.
   THE ATTENTION: a persona's att_eco holds the builders busy at once -- the census's busy ones, the commander's
   economy job among them, and the idle builders this decision makes busy (IMPAI_DEC.busied; impai_econ_room) --
   so an idle builder past it waits. A helper given another task here stays one busy builder:
   it needs no room and takes none, though it spends one of the decision's assign_max orders. The governor keeps
   no more builders than this (cons_room): its constructors and the commander.
   THE RELEASE, in THE HOLD (impai_brain.c, economy): a helper whose target is not an economy building's frame --
   a factory it guards, a factory's product, a plant's frame -- is given the ladder an idle builder gets, and is
   stopped (resolver mode 10, then the order, as the job checks' stop) when the ladder has nothing for it.
   INVARIANT, an ordering by the job field: the helper's job ends only together with an order that replaces its
   head -- the ladder's order, or a stop the resolver accepted and the WORLDQ queued -- so no finished builder is
   left with a busy head and no job, and THE HAND-BACK's invariant stays true: every finished builder is listed
   idle, as a helper or as jobbed. A guard never ends by itself, so an unlisted guard would be lost for the game.
   [INFERRED: a stopped builder's list empties and the controller's Standby, flagged 0x4000 (0x43B9AD..0x43BA3A,
   engine/orders.md), makes it idle at the next census, as the job checks' stop relies on.] BOUND: the census's
   helpers, at most IMPAI_IDLE_CAP, each with one search of at most IMPAI_NANO_MAX frames; every order within
   assign_max and the roots. RESIDUAL: an economy frame past the census's IMPAI_NANO_MAX is not found, and its
   helper is released (one order).
   THE RELEASE runs in its own pass, after the idle builders and before the economy helpers' reassignment, so the
   helpers on economy frames never take a root or an assignment a release needs. IT IS NEVER STARVED BY A
   BUDGET: with the economy's bucket limiting the decision (IMPAI_BRAIN_IN.acts 0 or more) and a helper to release
   in the hold (impai_release_keep), THE YARD, THE MAKERS and the idle loop leave the decision's last root, and
   the last two its last assignment, to the release pass, so each decision with a command to give releases one;
   idle builders, which appear whenever a build ends, yard moves and maker switches cannot
   take every root decision after decision. With no budget nothing is kept. The job checks and the walk checks
   run first and are not held back: a stop and a turn back are each one order a builder's job asks for once. */
int impai_econ_room(const IMPAI_DEC* d)
{
    const int att = d->in->persona->att_eco;
    return !att || d->nbusy + d->busied < att;
}

/* 1 when helper h assists an economy building's frame the census found (THE RELEASE's test) */
static int helper_eco(const IMPAI_DEC* d, const IMPAI_UNITREF* h)
{
    const uint16_t target = d->in->job[h->slot].target;
    int k;
    for (k = 0; k < d->nnano; k++)
        if (d->nano[k].slot == target) return IMPAI_ECO_CAT(d->in->ti[d->nano[k].type].cat);
    return 0;
}

/* 1 when helper h is one THE RELEASE takes off its work: in an assist, on no economy building's frame */
static int releasable(const IMPAI_DEC* d, const IMPAI_UNITREF* h)
{
    return d->in->job[h->slot].kind == IMPAI_JOB_ASSIST && !helper_eco(d, h);
}

int impai_release_keep(const IMPAI_DEC* d)
{
    int i, keep = 0;
    if (d->in->acts >= 0 && d->hold)
        for (i = 0; i < d->nhelpers && !keep; i++) keep = releasable(d, &d->helpers[i]);
    return keep;
}

/* THE RELEASE of helper h: the ladder an idle builder gets, else a stop; 1 when either was given */
static int release(IMPAI_DEC* d, const IMPAI_UNITREF* h)
{
    IMPAI_JOB* j = &d->in->job[h->slot];
    int a;
    if (assign(d, h, 0)) return 1;
    if (d->roots < d->root_cap && (a = d->q->ops->can(d->q, h->slot, 10, (int)WQ_NONE, h->pos)) > 0) {
        WQ_CMD c;
        memset(&c, 0, sizeof c);
        c.verb = WQ_CMD_NEW;
        c.action = (uint8_t)a;
        c.unit = h->slot;
        c.target = (uint16_t)WQ_NONE;
        memcpy(c.pos, h->pos, sizeof c.pos);
        if (impai_give(d, &c) == WQ_OK) {
            impai_job_end(j);
            return 1;
        }
    }
    return 0;
}

void impai_econ_assign(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int keep = impai_release_keep(d);
    int i;
    for (i = 0; i < d->nidle && d->assigned + keep < in->plan->assign_max && d->roots + keep < d->root_cap &&
                impai_econ_room(d);
         i++) {
        const IMPAI_JOB* j = &in->job[d->idle[i].slot];
        if (j->kind == IMPAI_JOB_BUILD || j->kind == IMPAI_JOB_RECLAIM || j->kind == IMPAI_JOB_RECLAIM_GO) continue;
        if (assign(d, &d->idle[i], 0)) {
            d->assigned++;
            d->busied++;
        }
    }
    /* THE STALL TAKES THE RECLAIMERS: in an energy stall (impai_brain.c, THE STALLS) a builder at a reclaim, or on
       its way to one, is given the stall's energy -- the ladder's first rung alone, replacing the job only when
       a site takes it, so a builder with no energy to place keeps its reclaim. A job given in this decision (its
       `since` now: THE RECLAIM'S CHECK may have ordered the reclaim on arrival) is left as it is. WHY: the job
       checks list only an idle builder idle, so a reclaimer never answered the stall -- on Escalation the
       commander reclaimed into a full metal store while the energy stood at 0 (MEASURED 2026-11-15, the economy
       diagnosis at d665a00a). A builder on a BUILD is left at it: its frame stands where it was ordered, and THE
       STALLS keep the plants that frame would be from being ordered in a stall. */
    for (i = 0; d->e_stall && i < d->njobbed && d->assigned + keep < in->plan->assign_max && d->roots + keep < d->root_cap;
         i++) {
        const IMPAI_UNITREF* b = &d->jobbed[i];
        const IMPAI_JOB* j = &in->job[b->slot];
        int t, n, ordered;
        if (d->jobbed_idle[i] || (j->kind != IMPAI_JOB_RECLAIM && j->kind != IMPAI_JOB_RECLAIM_GO) || j->since == d->now)
            continue;
        /* read before the build replaces the job: stall_taken counts the ordered reclaims, not the walks to one */
        ordered = j->kind == IMPAI_JOB_RECLAIM;
        n = d->q->ops->build_options(d->q, b->type, d->menu, WQ_TYPES_MAX);
        if (n > 0 && (t = choose_energy(d, d->menu, n)) && impai_build_ring(d, b, t)) {
            d->pe_in += in->ti[t].egain;
            d->assigned++;
            if (ordered) d->out->stall_taken++;
        }
    }
    /* the busy builders the decision leaves; the helpers' new tasks below change no count */
    d->out->busy = (uint16_t)(d->nbusy + d->busied);
    if (d->hold)
        for (i = 0; i < d->nhelpers && d->assigned < in->plan->assign_max && d->roots < d->root_cap; i++)
            if (releasable(d, &d->helpers[i]) && release(d, &d->helpers[i])) {
                d->assigned++;
                d->out->released++;
            }
    for (i = 0; i < d->nhelpers && d->assigned < in->plan->assign_max && d->roots < d->root_cap; i++) {
        const IMPAI_UNITREF* h = &d->helpers[i];
        if (in->job[h->slot].kind != IMPAI_JOB_ASSIST || (d->hold && !helper_eco(d, h))) continue;
        if ((d->e_stall || d->want_factory || d->want_tech || d->want_sea || d->want_def || d->want_aa || d->m_short) &&
            assign(d, h, 1))
            d->assigned++;
    }
}

/* ---- 5. the factories ---- */

/* THE FINISH'S BUY: 1 while the finish at sea has a plant to make its army -- the seat holds a water plant, a
   frame included (sea_plants), or can still place one: a builder offers one (IMPAI_DEC.sea_able), the refusals
   are not spent, and the search has not found the place empty (impai_sea_none_now). Read at each order, so a
   search that finds none in this decision's ladder lets the factories that follow buy as usual. */
static int finish_buys(const IMPAI_DEC* d)
{
    return d->sea_plants || (d->sea_able && d->in->st->sea_refused < IMPAI_SEA_REFUSALS && !impai_sea_none_now(d));
}

/* factory type `type`'s army types into mt (at most IMPAI_MENU_MAX): its menu's types with an army class and a
   value; reads into d->menu. THE FINISH AT SEA (impai_brain.c, economy): while the military's dry hunt holds and
   the finish has a plant (finish_buys), only types with a water weapon (IMPAI_TA_WATER1, IMPAI_TA_WATER2), the
   weapons that hit a unit under the sea (can_attack, impai_mil.c) -- so a land plant offers none and stands idle,
   and the army's rows (THE ARMY'S ROWS) are left to the units that can finish. With no plant to make them the
   factories buy as usual: a dry hunt with no water it can reach would otherwise build no army at all.
   RESIDUAL: a water-weapon type with no sonar of its own sees nothing under the sea, and the finish then waits for
   a sonar elsewhere. */
static uint32_t army_menu(IMPAI_DEC* d, unsigned type, IMPAI_MENU_TYPE* mt)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int n = d->q->ops->build_options(d->q, (int)type, d->menu, WQ_TYPES_MAX);
    const int dry = in->mil && in->mil->dry && finish_buys(d);
    uint32_t k, m = 0;
    for (k = 0; n > 0 && k < (uint32_t)n && m < IMPAI_MENU_MAX; k++) {
        const unsigned t = d->menu[k];
        if (t >= in->nti || in->ti[t].klass >= IMPAI_KS || !(in->ti[t].value > 0.0f)) continue;
        if (dry && !(in->ti[t].attack & (IMPAI_TA_WATER1 | IMPAI_TA_WATER2))) continue;
        mt[m].type = (uint16_t)t;
        mt[m].klass = in->ti[t].klass;
        mt[m].pad = 0;
        mt[m].value = in->ti[t].value;
        m++;
    }
    return m;
}

/* a QUEUE of `count` units of `type` at factory f, 1..WQ_QUEUE_MAX (the WORLDQ refuses any other), counted
   against the decision's commands: its order's answer */
static int queue_units(IMPAI_DEC* d, const IMPAI_UNITREF* f, int type, int count)
{
    WQ_CMD c;
    memset(&c, 0, sizeof c);
    c.verb = WQ_CMD_QUEUE;
    c.unit = f->slot;
    c.target = (uint16_t)WQ_NONE;
    c.type = (uint16_t)type;
    c.count = (int16_t)count;
    return impai_give(d, &c);
}

/* THE ARMY ORDERS: which army classes each idle factory may take.
       fbest(f, k) = the highest value (impai_tinfo.c, THE VALUES, at the seat's tech level max(ftop, 1),
                     value_tech: the factories it orders stand at it) of f's menu types of
                     gated class k with a value above 0 that reach (THE REACH); IMPAI_Q_NONE for none
       kbest(k)    = the highest fbest(f, k) over the distinct types of the seat's finished factories, idle or busy
       eligible(f) = f's army classes outside IMPAI_GATED, and each gated class k with
                     fbest(f, k) > IMPAI_Q_NONE and fbest(f, k) >= min(0, kbest(k))
       poor(f)     = f offers a gated class and none is eligible
       A(f)        = the highest fbest(f, k), IMPAI_Q_NONE for none
   - A class's unit that holds its value against the field (q >= 0) may be built on any factory that has one;
     below 0 only on the factories whose unit is the seat's best of the class. So a plant whose army is poor here
     builds none of it, and a class only one plant offers stays that plant's, which keeps every class the seat
     can build in the table's composition. RESIDUAL: such a class is built there however low its value, because
     the table wants it.
   - A poor plant takes the governor's constructor when the governor allows one, else no army order outside a
     surplus (A SURPLUS below). It is counted (IMPAI_BRAIN_OUT.poor) when it is left without an order.
   - The governor walks the idle factories that offer a gated class in ascending A, then those that offer none --
     a shipyard, an air plant -- ties in slot order, so a constructor goes to the land plant whose army is worth
     least here; and a plant the values do not score makes a constructor only when no scored plant is idle this
     decision, so a land plant that took an army order instead does not pass the constructor to the yard behind
     it. WHY: such a plant's A is IMPAI_Q_NONE, below every scored plant's, and sorted by it alone the shipyard and
     the air plant took the governor's constructors (CORCS, CORCA) before any land plant; sorted after them alone,
     the yard still took 6 of 7 in the brain test's twenty decisions, each land army order meeting the army's
     share for the yard's constructor after it. A naval seat whose land plants are all busy still makes sea
     constructors, which is meant: its sea plant has work for them.
   - A CLASS NO UNIT OF THE SEAT REACHES: when no type of gated class k on the seat's factories reaches, class k
     is read without the reach, as on a linked map, so the land plants still build that class by value for the
     base's defence (owner's decision, 2026-11-14: with no land route, land factories still build land army by
     value). Per class and not per seat: one amphibious unit that reaches on the seat's advanced plant made every
     other plant poor, and Trout Farm's two vehicle plants sat without an order for 726 factory-decisions
     (MEASURED 2026-11-14, plantchoice r3, bar-eota-troutfarm-cc-1). The plant choice keeps the reach either way.
   - A SURPLUS: a poor plant beside a store in surplus buys its cheapest army type of any class of its menu, as an
     ungated factory's surplus buy: the store is full, so the metal buys nothing else (THE GOVERNOR, A SURPLUS).
   - With no values, or a factory type past IMPAI_GATE_TYPES, the factory is ungated: every class of its menu is
     eligible, it is not poor, and it walks with the plants that offer no gated class.
   - RESIDUAL, THE TYPE AFTER CONTACT: with a picture, a gated class's type is impai_class_type's -- the pair rate
     against the seen enemy per value -- which reads neither the values nor the reach, so an eligible factory may
     then build a type of the class that does not reach the enemy, where THE TYPE BEFORE CONTACT would not.
   WHY, MEASURED 2026-11-14 over the bar's recordings: army orders went to any idle plant, and 31 % of CORE's army
   units were hovers from the hover plant whatever their value. BOUND: one menu read for each distinct factory type
   of the census's lists, at most IMPAI_GATE_TYPES, and one for each idle factory of a type past them. */
/* type `type`'s army classes and its best value in each gated class by `mask` and without the reach, into
   *classes, best[] and all[]. BOUND: a menu type's class indexes best[] and all[] only once tested below
   IMPAI_GATE_K -- an army class runs to IMPAI_KS (13), and the rows sit inside IMPAI_DEC, where an overrun
   would write the memo's next fields unseen by any sanitizer */
static void gate_read(IMPAI_DEC* d, unsigned type, unsigned T, uint32_t mask, uint32_t* classes,
                      float best[IMPAI_GATE_K], float all[IMPAI_GATE_K])
{
    IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
    IMPAI_MENU m;
    uint32_t i;
    unsigned k;
    for (k = 0; k < IMPAI_GATE_K; k++) best[k] = all[k] = IMPAI_Q_NONE;
    m.t = mt;
    m.n = army_menu(d, type, mt);
    *classes = impai_menu_classes(&m);
    for (i = 0; i < m.n; i++) {
        const unsigned t = mt[i].type;
        float q;
        if ((k = mt[i].klass) >= IMPAI_GATE_K) continue;
        q = gated_reaching(d, 0xFFFFFFFFu, t) ? d->in->tv[t].q[T - 1] : IMPAI_Q_NONE;
        if (q > all[k]) all[k] = q;
        if (gated_reaching(d, mask, t) && q > best[k]) best[k] = q;
    }
}

static int gate_find(const IMPAI_DEC* d, unsigned type)
{
    int g;
    for (g = 0; g < d->ngate; g++)
        if (d->gate_type[g] == type) return g;
    return -1;
}

/* the distinct factory types of fidle and fbusy into the memo, read by `mask` and without it; the gated classes
   no type reaches by `mask` into d->gate_noreach */
static void gate_types(IMPAI_DEC* d, unsigned T, uint32_t mask)
{
    int i, k;
    uint32_t reached = 0;
    d->ngate = 0;
    for (i = 0; i < d->nfidle + d->nfbusy; i++) {
        const unsigned type = i < d->nfidle ? d->fidle[i].type : d->fbusy[i - d->nfidle].type;
        if (gate_find(d, type) >= 0 || d->ngate >= IMPAI_GATE_TYPES) continue;
        d->gate_type[d->ngate] = (uint16_t)type;
        gate_read(d, type, T, mask, &d->gate_classes[d->ngate], d->gate_best[d->ngate], d->gate_all[d->ngate]);
        for (k = 0; k < IMPAI_GATE_K; k++)
            if (d->gate_best[d->ngate][k] > IMPAI_Q_NONE) reached |= 1u << k;
        d->ngate++;
    }
    d->gate_noreach = IMPAI_GATED & ~reached;
}

/* factory type g's best value in gated class k as the gate reads it: by the reach, or without it for a class no
   type of the seat reaches */
static float gate_fb(const IMPAI_DEC* d, int g, int k)
{
    if ((unsigned)g >= IMPAI_GATE_TYPES || (unsigned)k >= IMPAI_GATE_K) return IMPAI_Q_NONE;
    return (d->gate_noreach >> k & 1u) ? d->gate_all[g][k] : d->gate_best[g][k];
}

/* each idle factory's eligible classes, poverty, A (above) and whether it is unscored -- it offers no gated
   class, or the gate does not read it; with no values, every factory ungated and unscored */
static void plant_gate(IMPAI_DEC* d, uint32_t* elig, uint8_t* poor, float* A, uint8_t* unscored)
{
    IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
    const unsigned T = value_tech(d);
    float kbest[IMPAI_GATE_K] = { IMPAI_Q_NONE, IMPAI_Q_NONE, IMPAI_Q_NONE };
    int i, g, k;
    d->gate_mask = reach_mask_of(d);
    d->gate_noreach = 0;
    if (d->in->tv) gate_types(d, T, d->gate_mask);
    for (g = 0; d->in->tv && g < d->ngate; g++)
        for (k = 0; k < IMPAI_GATE_K; k++)
            if (gate_fb(d, g, k) > kbest[k]) kbest[k] = gate_fb(d, g, k);
    for (i = 0; i < d->nfidle; i++) {
        const int at = d->in->tv ? gate_find(d, d->fidle[i].type) : -1;
        uint32_t e;
        poor[i] = 0;
        A[i] = IMPAI_Q_NONE;
        unscored[i] = (uint8_t)(at < 0 || !(d->gate_classes[at] & IMPAI_GATED));
        if (at < 0) {
            IMPAI_MENU m;
            m.t = mt;
            m.n = army_menu(d, d->fidle[i].type, mt);
            elig[i] = impai_menu_classes(&m);
            continue;
        }
        e = d->gate_classes[at] & ~IMPAI_GATED;
        for (k = 0; k < IMPAI_GATE_K; k++) {
            const float fb = gate_fb(d, at, k), floor_k = kbest[k] < 0.0f ? kbest[k] : 0.0f;
            if (fb > IMPAI_Q_NONE && fb >= floor_k) e |= 1u << k;
            if (fb > A[i]) A[i] = fb;
        }
        elig[i] = e;
        poor[i] = (uint8_t)((d->gate_classes[at] & IMPAI_GATED) && !(e & IMPAI_GATED));
    }
}

/* THE TYPE BEFORE CONTACT for gated class k on menu m: with the picture empty, the type of most value per metal
   (THE VALUES) among m's class-k types that reach as the gate reads class k, ties to the lower type; -1 none. A plant
   chosen for its best unit then builds that unit, not the cheapest of its class, which is what the type choice
   gives an empty picture (impai_table.c, impai_class_type) */
static int class_best(const IMPAI_DEC* d, const IMPAI_MENU* m, unsigned k, unsigned T)
{
    const uint32_t mask = (d->gate_noreach >> k & 1u) ? 0xFFFFFFFFu : d->gate_mask;
    float best = IMPAI_Q_NONE;
    int t = -1;
    uint32_t i;
    for (i = 0; i < m->n; i++) {
        const unsigned u = m->t[i].type;
        if (m->t[i].klass != k || !gated_reaching(d, mask, u)) continue;
        if (t < 0 || d->in->tv[u].q[T - 1] > best || (d->in->tv[u].q[T - 1] == best && (int)u < t)) {
            best = d->in->tv[u].q[T - 1];
            t = (int)u;
        }
    }
    return t;
}

/* THE ARMY for idle factory f, one unit, while the decision's commands last. The class is A3's (impai_table.h):
   of the classes `eligible` holds (THE ARMY ORDERS), the one furthest below its share of the army's value -- built,
   building and queued, kspent -- (impai_buy_next), the shares those of the seat's table over its military's
   picture for the classes some idle factory not poor may take (`share`, impai_table_shares). The type: for a gated
   class with the picture empty and the values given, THE TYPE BEFORE CONTACT; else the type choice's of the class
   (impai_class_type, the predictor's pair rate against the picture's enemy composition). `cheapest`: instead the
   cheapest army type of the factory's menu in an eligible class with a value above 0, ties to the lower type --
   the surplus's buy (THE GOVERNOR). 1 when ordered. One unit a factory a decision: the decision comes every
   look_eco ticks of the persona and its order is given react ticks later, so a factory is idle at most look_eco +
   react ticks between two units, and the composition answers the picture as it moves. BOUND: a menu past IMPAI_MENU_MAX army types is cut there.
   THE ARMY'S ROWS, a bound: no army order takes the governor's count past IMPAI_ARMY_MAX, the rows the military's
   census lists (impai_mil.c, census), so every army unit the seat's factories make is a unit a squad can hold.
   gov_a counts the finished army units and every count node's units, a superset of the finished army the military
   lists, so the bound holds for as long as no unit joins the seat but from its own factories. Past the rows a unit
   is in no squad and takes no order: MEASURED 2026-11-15 on bar-nota-troutfarm-aa-2 at cf3f68c9, the census filled
   its 512 rows and left 1 183 army units out over the game's decisions (IMPAI_MIL_OUT.left_army). A factory the
   bound refuses is idle, and counted as one (IMPAI_BRAIN_OUT.fidle) in a surplus.
   THE FINISH'S ROWS: on a level that can hold THE DRY HUNT (impai_finish_level), an order of a type with no water
   weapon may take the count to IMPAI_ARMY_MAX - IMPAI_FINISH_ROWS at most, and one with a water weapon (the types
   THE FINISH AT SEA buys, army_menu) to IMPAI_ARMY_MAX (army_rows). INVARIANT, a bound: the seat's army units with
   no water weapon never pass IMPAI_ARMY_MAX - IMPAI_FINISH_ROWS -- gov_a counts them all, and each order of them
   holds the whole count below that --, so at least IMPAI_FINISH_ROWS rows hold units with a water weapon or are
   free, whatever the land army has grown to: the finish always has rows for the units that can hit what is left
   under the water. WHY: the dry menus offer water weapons alone, so a land army at 512 rows when the dry hunt began
   left the finish's sea plant nothing to make for the rest of the game. A level that cannot be dry keeps all IMPAI_ARMY_MAX rows for any type. Each order these
   rows refuse below IMPAI_ARMY_MAX is counted (IMPAI_BRAIN_OUT.finish_rows, `finish_rows=` on the eco line): the
   cap holds on every level that can be dry, whether or not the seat ever builds a sea plant, and a pick at the
   cap is refused outright rather than turned to a type with a water weapon -- a GAP the count measures. */
#define IMPAI_FINISH_ROWS 64    /* the finish's first ten waves, 1 + 2 + ... + 10 = 55 median units (impai_mil.c,
                                   THE FINISH'S WAVE), with room [hand-written: the bar's to move]             */
_Static_assert(IMPAI_FINISH_ROWS < IMPAI_ARMY_MAX, "the finish's rows leave the land army rows");
static int64_t army_rows(const IMPAI_DEC* d, int t)
{
    const int water = t > 0 && (unsigned)t < d->in->nti &&
                      (d->in->ti[t].attack & (IMPAI_TA_WATER1 | IMPAI_TA_WATER2)) != 0;
    return impai_finish_level(d->in->reach) && !water ? IMPAI_ARMY_MAX - IMPAI_FINISH_ROWS : IMPAI_ARMY_MAX;
}

/* THE FIVE, bounded (THE GOVERNOR, A SURPLUS): the units of one surplus order of type t on a budgeted seat. The
   store above THE HOLD's line (stall_pm of the storage), less the metal already committed past the unit each
   factory makes (IMPAI_DEC.surplus_m: the busy factories' queues at the census, THE QUEUED METAL in impai_brain.c,
   and this decision's earlier orders past their first unit), is shared by the orders the decision may still give
   -- the fewer of queue_max - queued and the idle factories not yet ordered, nfidle - queued at most -- and bought
   at t's metal cost: at least 1, at most WQ_QUEUE_MAX, at most what THE ARMY'S ROWS leave. INVARIANT, a bound: a
   decision queues a unit past an order's first only while every factory's queued units past the one it makes
   cost at most the store above the hold's line as the decision read it, decision after decision: the next one
   sees the queues this one left. It bounds the queues, not the store: the units in production, the builders' work
   and the one unit an order always buys spend it too, and THE HOLD answers a store that falls through the line. WHY: the hold refuses only new orders, so five-unit queues the store cannot pay commit the
   spending the hold exists to stop -- four orders of five 150-metal units against a store of 800 --, and counted
   within one decision alone, the next would commit the same store again to the factories newly idle. A type with
   no metal cost takes WQ_QUEUE_MAX; a type the world gives no economy keys for, a cost below 0 or not finite, or
   a store not finite, takes 1. t's metal cost into *cost_m, 0 with none. */
static int surplus_count(const IMPAI_DEC* d, int t, float* cost_m)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const int by_q = plan->queue_max - d->queued, by_f = d->nfidle - d->queued;
    const int left = by_q < by_f ? by_q : by_f;
    const int64_t rows = army_rows(d, t) - d->gov_a;
    WQ_DEF_ECON e;
    double n;
    int c;
    *cost_m = 0.0f;
    if (!d->q->ops->def_econ(d->q, t, &e) || !(e.cost_m >= 0.0f && e.cost_m <= FLT_MAX)) c = 1;
    else if (!(e.cost_m > 0.0f)) c = WQ_QUEUE_MAX;
    else {
        *cost_m = e.cost_m;
        n = (double)d->e.metal - (double)plan->stall_pm / 1000.0 * (double)d->e.metal_max - (double)d->surplus_m;
        n /= (double)e.cost_m * (left > 1 ? left : 1);
        c = !(n >= 1.0) ? 1 : n >= WQ_QUEUE_MAX ? WQ_QUEUE_MAX : (int)n;
    }
    return rows < c ? (rows > 1 ? (int)rows : 1) : c;
}

static int queue_army(IMPAI_DEC* d, const IMPAI_UNITREF* f, const float* share, uint32_t eligible, int cheapest)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_MIL* mil = in->mil;
    const unsigned T = value_tech(d);
    IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
    float cost[IMPAI_KS];
    int32_t type[IMPAI_KS];
    IMPAI_MENU m;
    float cost_m = 0.0f;
    int k, t = -1, count;
    if (d->queued >= in->plan->queue_max || d->roots >= d->roots_max) return 0;
    m.t = mt;
    m.n = army_menu(d, f->type, mt);
    if (cheapest) {
        uint32_t i;
        for (i = 0; i < m.n; i++)
            if ((eligible >> mt[i].klass & 1u) &&
                (t < 0 || mt[i].value < in->ti[t].value || (mt[i].value == in->ti[t].value && mt[i].type < t)))
                t = mt[i].type;
        if (t < 0) return 0;
        k = in->ti[t].klass;
    } else {
        for (k = 0; k < IMPAI_KS; k++) {
            type[k] = -1;
            cost[k] = 0.0f;
            if (!(eligible >> k & 1u)) continue;
            if (in->tv && mil->pic.ne == 0 && (IMPAI_GATED >> k & 1u)) type[k] = class_best(d, &m, (unsigned)k, T);
            if (type[k] < 0) type[k] = impai_class_type(&mil->table, &mil->pic, &m, (unsigned)k, in->pred);
            if (type[k] > 0 && (unsigned)type[k] < in->nti) cost[k] = in->ti[type[k]].value;
        }
        if ((k = impai_buy_next(share, d->kspent, cost, eligible, FLT_MAX)) < 0 || type[k] <= 0) return 0;
        t = type[k];
    }
    count = d->surplus && in->acts >= 0 ? surplus_count(d, t, &cost_m) : 1;
    if ((int64_t)d->gov_a + count > army_rows(d, t)) {
        if ((int64_t)d->gov_a + count <= IMPAI_ARMY_MAX && d->out->finish_rows < 0xFFFF) d->out->finish_rows++;
        return 0;
    }
    if (queue_units(d, f, t, count) != WQ_OK) return 0;
    d->kspent[k] += in->ti[t].value * (float)count;
    d->surplus_m += cost_m * (float)(count - 1);
    d->gov_a += count;
    d->queued++;
    d->out->orders++;
    d->out->army = (uint16_t)(d->out->army + count);
    return 1;
}

/* THE GOVERNOR: 1 when an idle factory may queue a constructor now */
static int gov_cons(const IMPAI_DEC* d)
{
    const IMPAI_PLAN* plan = d->in->plan;
    const int64_t c = d->gov_c;
    if (!cons_room(d) || d->pressed) return 0;                               /* the caps; THE PRESSURE      */
    if (c < plan->cons_open) return 1;                                        /* THE OPENING                 */
    return (int64_t)d->gov_a >= (int64_t)plan->army_per_cons * (c - plan->cons_open + 1);   /* THE ARMY'S SHARE */
}

/* THE GOVERNOR. Each idle factory, in THE ARMY ORDERS' walk (the plants offering a gated class in ascending A, then
   the rest, ties in slot order), while the decision's commands last, is given one order: a constructor -- its best
   (choose_cons), or while the plant choice waits for an unlock (THE UNLOCKS) that constructor at a factory that
   makes it and none at any other until one is queued -- when all three rules
   hold, otherwise one army unit in a class it may take (queue_army), and a poor plant nothing outside a surplus.
   A constructor order the factory refuses leaves it for the army in the same decision.
   - THE OPENING. The first cons_open constructors need nothing.
   - THE ARMY'S SHARE. Past them, a constructor needs army >= army_per_cons x (constructors - cons_open + 1).
   - THE PRESSURE. No constructor while the military's stamp lasts (impai_mil.c, THE PRESSURE).
   - A SURPLUS. With the store at SURPLUS_FRAC of its maximum or more, income outruns spending: a factory whose
     table buy finds nothing buys its cheapest army type of a class it may take, and a poor plant its cheapest army
     type of any class, so no factory idles beside a full store; one with no such type is counted
     (IMPAI_BRAIN_OUT.fidle, a poor one .poor). The share, the cap and the pressure still hold. A SEAT WITH AN
     ACTION BUDGET (in->acts >= 0) QUEUES UP TO WQ_QUEUE_MAX UNITS AN ARMY ORDER IN A SURPLUS, the engine's own
     shift-click (the build button queues 5 when its shift key is down, 0x41AC0E..0x41AC13, through 0x419B00 to
     0x43B0B0): one order, which the budget counts as one action, keeps a plant busy for five products -- as many
     as the store above the hold's line pays and THE ARMY'S ROWS leave (THE FIVE, bounded: surplus_count). Otherwise
     every order is one unit: a seat with no budget reaches each idle plant at its next decision. WHY, MEASURED
     2026-11-14 on Pincushion (bar-eota-pincushion-ac-1, the Easy persona, apm_eco 10): from GameTime 34 000 the
     store stood full while 1 to 3 of the seat's 5 plants asked metal at any sample, spending 2-29 of 28.7 a pass,
     and 40.6 % of the steady income was wasted -- the plants waited on the budget's actions, not on metal. Why
     not every seat: the Hard seat on Trout Farm (bar-hota-troutfarm-ac-1) given five an order grew its army past
     the military's 512 rows and reached the game's cap at 81 000 with 677 units against 28, where one an order
     won at 28 542 (MEASURED 2026-11-14). COST: a surplus order commits up to five units' metal over the next
     minutes, which the store held.
   - THE HOLD'S FLOOR (impai_brain.c, economy). In a hold an idle factory is given no order once the busy ones and
     this walk's orders reach hold_fac, and is counted (IMPAI_BRAIN_OUT.held). The walk's order is unchanged: the
     hold decides how many factories run, not which plant does what, and the floor's factory takes whatever the
     governor gives it, a constructor included; a poor plant left without an order uses up no floor. A unit in
     progress is never stopped: a factory busy at the hold's start is held from its next idle decision on, after
     at most WQ_QUEUE_MAX units of an order a budgeted seat gave in a surplus (a hold needs none: impai_brain.c,
     economy). The hold refuses no constructor: constructors are the economy's growth, and the
     measured stall is one of too little income; a rule that refused them in a hold would hold them for most of
     the game [INFERRED from the store's runs below 20 % of its storage: 239 passes at the median, p90 543,
     MEASURED 2026-11-14 over the same seat-games as below]. INVARIANT, a bound on counts: d->queued counts only
     this walk's orders, so nfbusy + queued is the factories busy once the walk ends, and in a hold the walk gives
     at most max(0, hold_fac - nfbusy) orders -- at most max(nfbusy, hold_fac) finished factories busy after it. nfbusy is the census's, held to IMPAI_IDLE_CAP: a census past the cap
     counts fewer, never more, and hold_fac <= IMPAI_IDLE_CAP (impai_brain_test.c, check_plan).
   NO RULE TIES THE CONSTRUCTORS TO THE INCOME: constructors build the extractors that pay for them, so a cap
   proportional to income holds a seat to the income it already makes. MEASURED 2026-11-14: such a cap (24
   workertime a metal a pass) was the only rule refusing a constructor in 50 % of the samples past GameTime 9 000,
   and one of two in 8.5 % (57 ImpAI seat-games of the bar), and the seats held 2-4 constructors against 6-11
   before the governor.
   INVARIANT, a bound on counts: at every decision that queues a constructor, (1) the constructors are below
   cons_max; (2) below cons_open, or the army share holds; (3) not pressed. Every count is the census's of the
   same decision plus this decision's own orders, in
   the walk's order (THE GOVERNOR'S COUNTS, impai_brain.h): no state, no timing. Without a military or a predictor
   the factories buy no army, as at A1, and are not gated.
   WHY, MEASURED: the first army unit came at a median GameTime of 13 500-15 000 against a first factory at 3 300-
   3 600, and at 12 000 the seats held 5-6 constructors and no army at every level (196 seat-games of the bar,
   2026-11-13); a constructor takes a factory 1 485-1 745 ticks against 436-521 for a raider; in a game against a
   human on Comet Catcher, 95 % of the factory-ticks went to constructors and 27 of them were lost. BOUND: the
   factories looked at are the census's, IMPAI_IDLE_CAP, each one menu read for its constructor and at most two
   for its army, beside THE ARMY ORDERS' reads; the order is an insertion sort of at most IMPAI_IDLE_CAP. */
void impai_econ_factories(IMPAI_DEC* d)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_PLAN* plan = in->plan;
    const int army = in->mil && in->pred;
    uint32_t elig[IMPAI_IDLE_CAP];
    uint8_t poor[IMPAI_IDLE_CAP], order[IMPAI_IDLE_CAP], unscored[IMPAI_IDLE_CAP];
    float A[IMPAI_IDLE_CAP], share[IMPAI_KS];
    unsigned buildable = 0, unlock;
    int i, j, t, scored_idle = 0;
    memset(share, 0, sizeof share);
    memset(poor, 0, sizeof poor);
    memset(unscored, 1, sizeof unscored);
    for (i = 0; i < d->nfidle; i++) A[i] = IMPAI_Q_NONE;
    if (army) {
        plant_gate(d, elig, poor, A, unscored);
        for (i = 0; i < d->nfidle; i++)
            if (!poor[i]) buildable |= elig[i];
        if (buildable) impai_table_shares(&in->mil->table, &in->mil->pic, buildable, share);
    }
    for (i = 0; i < d->nfidle; i++) {
        for (j = i; j > 0 && (unscored[order[j - 1]] > unscored[i] ||
                              (unscored[order[j - 1]] == unscored[i] && A[order[j - 1]] > A[i]));
             j--)
            order[j] = order[j - 1];
        order[j] = (uint8_t)i;
        scored_idle |= !unscored[i];
    }
    if ((d->want_factory || d->want_tech) && !d->want_sea && !d->plant_done) plant_choice(d);
    unlock = unlock_kept(d);
    for (j = 0; j < d->nfidle && d->queued < plan->queue_max && d->roots < d->roots_max; j++) {
        const int at = order[j];
        const IMPAI_UNITREF* f = &d->fidle[at];
        int n;
        if (d->hold && d->nfbusy + d->queued >= plan->hold_fac) {
            d->out->held++;
            continue;
        }
        n = d->q->ops->build_options(d->q, f->type, d->menu, WQ_TYPES_MAX);
        t = n <= 0 || (unscored[at] && scored_idle) ? 0
            : !unlock ? choose_cons(d, d->menu, n) : on_menu(d->menu, n, unlock) ? (int)unlock : 0;
        if (t && gov_cons(d) && queue_units(d, f, t, 1) == WQ_OK) {
            d->gov_c++;
            d->queued++;
            d->out->orders++;
            if (unlock && t == (int)unlock) {
                d->out->unlock = (uint16_t)unlock;
                d->out->unlock_plant = in->st->unlock_plant;
                unlock = 0;
            }
            continue;
        }
        if (poor[at] && !d->surplus) {
            d->out->poor++;
            continue;
        }
        if (poor[at]) {
            IMPAI_MENU_TYPE mt[IMPAI_MENU_MAX];
            IMPAI_MENU m;
            m.t = mt;
            m.n = army_menu(d, f->type, mt);
            if (queue_army(d, f, share, impai_menu_classes(&m), 1)) continue;
            d->out->poor++;
            continue;
        }
        if (army && (queue_army(d, f, share, elig[at], 0) || (d->surplus && queue_army(d, f, share, elig[at], 1))))
            continue;
        if (d->surplus && d->queued < plan->queue_max && d->roots < d->roots_max) d->out->fidle++;
    }
}
