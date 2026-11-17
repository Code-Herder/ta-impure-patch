/* impai_table.c -- the classes, the picture's features, the response table and the buy (impai_table.h). */
#include <math.h>
#include <string.h>
#include "impai_table.h"
#include "tagpu_simreg.h"

/* The classes, the picture, the response table and the buy. OWNER: the military and the factories buy only
   for a live ImpAI seat, and the military's init loads the table for one; impai_class_of also classes every
   type for the content every level init builds, live or puppet, from the same defs. */
SIM_FEATURE(impai_table, SIM_PUPPET_OWNER, "impai-fight impai-eco impai-observe impai-observe-perm stock-impai-observe");

/* the file format is the struct's bytes: its size and its rows' layout are the format */
typedef char impai_table_size_is_the_format[sizeof(IMPAI_TABLE) == IMPAI_TABLE_BYTES ? 1 : -1];
typedef char impai_response_is_8_bytes[sizeof(IMPAI_RESPONSE) == 8 ? 1 : -1];
typedef char impai_response_rows_at_56[offsetof(IMPAI_TABLE, r) == 56 ? 1 : -1];
typedef char impai_classes_fit_a_mask[IMPAI_KS <= 32 ? 1 : -1];

/* ---- the classes ---- */

static int stockpiles(const TASIM_ROLE_IN* in)
{
    unsigned k, n = in->nweapons < TASIM_ROLE_WEAPONS ? in->nweapons : TASIM_ROLE_WEAPONS;
    for (k = 0; k < n; k++)
        if (in->w[k].flags & TASIM_WF_STOCKPILE) return 1;
    return 0;
}

/* a weapon the fight content counts (not an interceptor; a stockpile unit has no class at all) whose kind is
   DROPPED: the `dropped` bit without `beamweapon` (+0x111 bit 3), which the kind test takes first.
   INVARIANT: every weapon bit tested here and in stockpiles is one both role fillers copy (tasim_roles.h,
   TASIM_ROLE_WF), asserted below: a bit outside the mask reads 0 on both sides, so a beam would class as a
   bomber in the DLL and in tasim alike. */
_Static_assert(((TASIM_WF_DROPPED | TASIM_WF_BEAM | TASIM_WF_INTERCEPTOR | TASIM_WF_STOCKPILE) & ~TASIM_ROLE_WF) == 0,
               "the classes test only +0x111 bits the role fillers copy");
static int drops(const TASIM_ROLE_IN* in)
{
    unsigned k, n = in->nweapons < TASIM_ROLE_WEAPONS ? in->nweapons : TASIM_ROLE_WEAPONS;
    for (k = 0; k < n; k++) {
        uint32_t f = in->w[k].flags;
        if ((f & TASIM_WF_DROPPED) && !(f & (TASIM_WF_BEAM | TASIM_WF_INTERCEPTOR))) return 1;
    }
    return 0;
}

unsigned impai_class_of(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* out)
{
    const uint32_t none = TASIM_RF_COMMANDER | TASIM_RF_TRANSPORT | TASIM_RF_KAMIKAZE;
    if (!(out->flags & TASIM_RF_MOBILE) || (out->flags & none) || stockpiles(in)) return IMPAI_K_NONE;
    /* NO ARMY CLASS WITHOUT DAMAGE, a bound on data: a type takes a class only when the classifier's own damage
       per second is above 0 on the ground or in the air -- the [DAMAGE] default, or the anti-air rows, times the
       burst over the reload, summed over the regular slots (tasim_roles.c, derive; dps_water is a part of
       dps_ground there). So no factory buys, and no picture counts as army, a type whose every weapon deals 0
       or which has none: a jammer or a radar unit takes role SCOUT for having no regular weapon (role_of), and
       as the cheapest of its class the class share and the surplus's buy took it. The retail types this leaves
       out are 17 (MEASURED 2026-11-14 over the fight content -- no slot, or [DAMAGE] default and every row 0 --
       and over `tasim_run roles`' damage per second):
       ARMASER, ARMJAM, ARMMARK, ARMSEER, ARMSPY, CORETER, CORSPEC, CORSPY, CORTRUCK, CORVOYR and CORVRAD, which
       were land scouts, and ARMAWAC, ARMPEEP, ARMSEHAK, CORAWAC, CORFINK and CORHUNT, the air scouts, which THE AIR
       SCOUT below classes again. The negated compares class a NaN as none. NOT CAUGHT HERE: a slot whose weapon deals damage but the engine
       never fires -- Total Mayhem's jammers carry a vertical-launch rocket their scripts have no aim script for
       (research/notes/impai/decisions.md, No army class without damage).
       THE AIR SCOUT, the owner's one exception (decisions.md, *One air scout a seat*): an aircraft of role SCOUT
       that deals nothing -- CORFINK, ARMPEEP, the AWACs, the seaplane scouts -- takes IMPAI_K_ASCOUT, so a seat can
       buy its one air scout (impai_econ.c, THE SCOUT FIRST) to see what no land or sea unit of it can. No share
       buys one (THE SCOUT IS ONE), and an enemy one presses nothing: THE PRESSURE and THE BASE'S THREATS take
       only armed members (impai_mil.c). */
    if (!(out->dps_ground > 0.0) && !(out->dps_air > 0.0))
        return out->layer == TASIM_LAYER_AIR && out->role == TASIM_ROLE_SCOUT ? IMPAI_K_ASCOUT : IMPAI_K_NONE;
    switch (out->role) {
    case TASIM_ROLE_RAIDER: case TASIM_ROLE_ASSAULT: case TASIM_ROLE_ARTILLERY: case TASIM_ROLE_ANTIAIR:
    case TASIM_ROLE_SCOUT:
        break;
    default:                            /* builder, defence, other */
        return IMPAI_K_NONE;
    }
    if (out->layer == TASIM_LAYER_AIR) {
        if (out->role == TASIM_ROLE_ANTIAIR) return IMPAI_K_FIGHTER;
        if (drops(in)) return IMPAI_K_BOMBER;
        if (out->role == TASIM_ROLE_SCOUT) return IMPAI_K_ASCOUT;
        return IMPAI_K_GUNSHIP;
    }
    if (out->layer == TASIM_LAYER_SEA) {
        switch (out->role) {
        case TASIM_ROLE_RAIDER:    return IMPAI_K_SRAIDER;
        case TASIM_ROLE_ASSAULT:   return IMPAI_K_SASSAULT;
        case TASIM_ROLE_ARTILLERY: return IMPAI_K_SARTY;
        case TASIM_ROLE_ANTIAIR:   return IMPAI_K_SAA;
        default:                   return IMPAI_K_SSCOUT;
        }
    }
    switch (out->role) {
    case TASIM_ROLE_RAIDER:    return IMPAI_K_LRAIDER;
    case TASIM_ROLE_ASSAULT:   return IMPAI_K_LASSAULT;
    case TASIM_ROLE_ARTILLERY: return IMPAI_K_LARTY;
    case TASIM_ROLE_ANTIAIR:   return IMPAI_K_LAA;
    default:                   return IMPAI_K_LSCOUT;
    }
}

const char* impai_class_name(unsigned k)
{
    static const char* const name[IMPAI_KS] = {
        "lraider", "lassault", "lartillery", "lantiair", "lscout", "sraider", "sassault", "sartillery",
        "santiair", "sscout", "fighter", "bomber", "gunship", "ascout" };
    return k < IMPAI_KS ? name[k] : "none";
}

/* ---- the picture ---- */

/* [0, 1], and 0 for a NaN */
static float clamp01(float x)
{
    return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f;
}

static float share_of(float v, float total)
{
    return total > 0.0f ? clamp01(v / total) : 0.0f;
}

void impai_picture_features(const IMPAI_PICTURE_IN* in, IMPAI_PICTURE* p)
{
    float sea = in->kval[IMPAI_K_SRAIDER] + in->kval[IMPAI_K_SASSAULT] + in->kval[IMPAI_K_SARTY] +
                in->kval[IMPAI_K_SAA] + in->kval[IMPAI_K_SSCOUT];
    float ratio;
    p->f[IMPAI_F_LRAIDER]  = share_of(in->kval[IMPAI_K_LRAIDER], in->total);
    p->f[IMPAI_F_LASSAULT] = share_of(in->kval[IMPAI_K_LASSAULT], in->total);
    p->f[IMPAI_F_LARTY]    = share_of(in->kval[IMPAI_K_LARTY], in->total);
    p->f[IMPAI_F_SEA]      = share_of(sea, in->total);
    p->f[IMPAI_F_FIGHTER]  = share_of(in->kval[IMPAI_K_FIGHTER], in->total);
    p->f[IMPAI_F_BOMBER]   = share_of(in->kval[IMPAI_K_BOMBER], in->total);
    p->f[IMPAI_F_GUNSHIP]  = share_of(in->kval[IMPAI_K_GUNSHIP], in->total);
    p->f[IMPAI_F_DEFENCE]  = share_of(in->defence, in->total);
    p->f[IMPAI_F_AA]       = share_of(in->kval[IMPAI_K_LAA] + in->aa_struct, in->total);
    if (in->enemy_extractors == 0)
        ratio = in->own_extractors > 0 ? 2.0f : 1.0f;
    else
        ratio = (float)in->own_extractors / (float)in->enemy_extractors;
    p->f[IMPAI_F_INCOME] = (ratio < 2.0f ? ratio : 2.0f) * 0.5f;
    p->minute = (float)in->gametime / 1800.0f;
    p->f[IMPAI_F_TIME] = clamp01(p->minute / 40.0f);
    p->f[IMPAI_F_TECH] = clamp01((float)in->tech / 3.0f);
}

/* ---- the table ---- */

/* The rows' pairs, and the hand-written numbers: A2's base shares on each surface layer, its anti-air response
   on the AA group, no aircraft (every air base and air gain 0), its 0.35 anti-air cap and Hard's tech-up
   income; the type choice at counter 1, which prefers a type that counters the picture best while it costs no
   more than e times the cheapest of its class. */
#define IMPAI_TABLE_HAND_INIT {                                                                             \
    { 0.25f, 0.45f, 0.15f, 0.0f, 0.05f,  0.25f, 0.45f, 0.15f, 0.0f, 0.05f,  0.0f, 0.0f, 0.0f, 0.0f },              \
    { { IMPAI_F_AIR,      IMPAI_KG_AA,      0, 1.5f },                                                       \
      { IMPAI_F_DEFENCE,  IMPAI_K_LARTY,    0, 0.0f },                                                       \
      { IMPAI_F_LARTY,    IMPAI_K_LRAIDER,  0, 0.0f },                                                       \
      { IMPAI_F_LRAIDER,  IMPAI_K_LASSAULT, 0, 0.0f },                                                       \
      { IMPAI_F_BOMBER,   IMPAI_K_FIGHTER,  0, 0.0f },                                                       \
      { IMPAI_F_GUNSHIP,  IMPAI_K_FIGHTER,  0, 0.0f },                                                       \
      { IMPAI_F_LOWAA,    IMPAI_K_BOMBER,   0, 0.0f },                                                       \
      { IMPAI_F_SEA,      IMPAI_KG_SEA,     0, 0.0f } },                                                     \
    1.0f, 10.0f, 0.35f, 8.0f }

const IMPAI_TABLE IMPAI_TABLE_HAND = IMPAI_TABLE_HAND_INIT;
/* No tuning run has emitted a table yet: the default plays the hand-written numbers. */
const IMPAI_TABLE IMPAI_TABLE_DEFAULT = IMPAI_TABLE_HAND_INIT;

static uint64_t fnv64(const uint8_t* b, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint64_t impai_table_hash(const IMPAI_TABLE* t)
{
    return fnv64((const uint8_t*)t, IMPAI_TABLE_BYTES);
}

void impai_table_file(const IMPAI_TABLE* t, uint8_t out[IMPAI_TABLE_FILE_BYTES])
{
    uint64_t h = impai_table_hash(t);
    unsigned i;
    memcpy(out, t, IMPAI_TABLE_BYTES);
    for (i = 0; i < 8; i++) out[IMPAI_TABLE_BYTES + i] = (uint8_t)(h >> (8 * i));
}

int impai_table_load(const void* bytes, size_t n, IMPAI_TABLE* out, uint64_t* hash)
{
    const uint8_t* b = (const uint8_t*)bytes;
    IMPAI_TABLE t;
    uint64_t want = 0;
    unsigned i;
    if (n != IMPAI_TABLE_FILE_BYTES) return IMPAI_TABLE_ESIZE;
    for (i = 0; i < 8; i++) want |= (uint64_t)b[IMPAI_TABLE_BYTES + i] << (8 * i);
    if (fnv64(b, IMPAI_TABLE_BYTES) != want) return IMPAI_TABLE_EHASH;
    memcpy(&t, b, IMPAI_TABLE_BYTES);
    for (i = 0; i < IMPAI_RESPONSES; i++) {
        if (t.r[i].feature >= IMPAI_FROWS || t.r[i].klass >= IMPAI_KROWS) return IMPAI_TABLE_EROW;
        if (t.r[i].pad != 0) return IMPAI_TABLE_EPAD;
        if (!isfinite(t.r[i].gain)) return IMPAI_TABLE_ENAN;
    }
    for (i = 0; i < IMPAI_KS; i++)
        if (!isfinite(t.base[i])) return IMPAI_TABLE_ENAN;
    if (!isfinite(t.counter) || !isfinite(t.air_from_t) || !isfinite(t.aa_cap) || !isfinite(t.tech_m))
        return IMPAI_TABLE_ENAN;
    for (i = 0; i < IMPAI_KS; i++) t.base[i] = clamp01(t.base[i]);
    t.aa_cap = clamp01(t.aa_cap);
    *out = t;
    *hash = want;
    return IMPAI_TABLE_OK;
}

const char* impai_table_error(int code)
{
    switch (code) {
    case IMPAI_TABLE_OK:    return "ok";
    case IMPAI_TABLE_ESIZE: return "the file is not 140 bytes";
    case IMPAI_TABLE_EHASH: return "the FNV-1a 64 trailer does not match the table";
    case IMPAI_TABLE_EROW:  return "a response row names a feature or class past its count";
    case IMPAI_TABLE_EPAD:  return "a response row's pad is not zero";
    case IMPAI_TABLE_ENAN:  return "a number is not finite";
    default:                return "unknown";
    }
}

static float feature_of(const IMPAI_PICTURE* p, unsigned f)
{
    if (f < IMPAI_FEATURES) return clamp01(p->f[f]);
    if (f == IMPAI_F_AIR)
        return clamp01(clamp01(p->f[IMPAI_F_FIGHTER]) + clamp01(p->f[IMPAI_F_BOMBER]) +
                       clamp01(p->f[IMPAI_F_GUNSHIP]));
    if (f == IMPAI_F_LOWAA) return clamp01(clamp01(p->f[IMPAI_F_DEFENCE]) - clamp01(p->f[IMPAI_F_AA]));
    return 0.0f;
}

static unsigned group_mask(unsigned klass)
{
    if (klass < IMPAI_KS) return 1u << klass;
    if (klass == IMPAI_KG_AA) return IMPAI_K_AAMASK;
    if (klass == IMPAI_KG_SEA)
        return (1u << IMPAI_K_SRAIDER) | (1u << IMPAI_K_SASSAULT) | (1u << IMPAI_K_SARTY) | (1u << IMPAI_K_SAA);
    return 0;
}

void impai_table_shares(const IMPAI_TABLE* t, const IMPAI_PICTURE* p, unsigned buildable, float s[IMPAI_KS])
{
    float aa = 0.0f, rest = 0.0f, aa_scale = 0.0f, rest_scale = 0.0f;
    unsigned i, k;
    for (k = 0; k < IMPAI_KS; k++) s[k] = t->base[k];
    for (i = 0; i < IMPAI_RESPONSES; i++) {
        unsigned members = group_mask(t->r[i].klass) & buildable, m = 0;
        float g;
        for (k = 0; k < IMPAI_KS; k++) m += (members >> k) & 1u;
        if (m == 0) continue;
        g = t->r[i].gain * feature_of(p, t->r[i].feature) / (float)m;
        for (k = 0; k < IMPAI_KS; k++)
            if ((members >> k) & 1u) s[k] += g;
    }
    for (k = 0; k < IMPAI_KS; k++) {
        s[k] = clamp01(s[k]);
        if (!((buildable >> k) & 1u)) s[k] = 0.0f;
        if (((IMPAI_K_AIRMASK >> k) & 1u) && !(p->minute >= t->air_from_t)) s[k] = 0.0f;
        if ((IMPAI_K_AAMASK >> k) & 1u) aa += s[k]; else rest += s[k];
    }
    if (rest > 0.0f) {
        float cap = aa < t->aa_cap ? aa : t->aa_cap;
        aa_scale = aa > 0.0f ? cap / aa : 0.0f;
        rest_scale = (1.0f - cap) / rest;
    } else if (aa > 0.0f) {
        aa_scale = 1.0f / aa;           /* only anti-air can be built: the cap cannot be met, so it is all */
    }
    for (k = 0; k < IMPAI_KS; k++) s[k] *= ((IMPAI_K_AAMASK >> k) & 1u) ? aa_scale : rest_scale;
}

/* ---- the buy ---- */

unsigned impai_menu_classes(const IMPAI_MENU* m)
{
    unsigned i, mask = 0;
    for (i = 0; i < m->n && i < IMPAI_MENU_MAX; i++)
        if (m->t[i].klass < IMPAI_KS) mask |= 1u << m->t[i].klass;
    return mask;
}

static int usable(const IMPAI_MENU_TYPE* t, unsigned k)
{
    return t->klass == k && t->value > 0.0f && isfinite(t->value);
}

/* the rate of one unit of `type` against the picture's enemy mobile composition; a rate that is not a finite
   non-negative number is the predictor's fault and counts 0 */
static float against(const IMPAI_PICTURE* p, const IMPAI_PREDICTOR* pred, uint16_t type)
{
    unsigned e, ne = p->ne < IMPAI_PIC_ENEMY ? p->ne : IMPAI_PIC_ENEMY;
    float sum = 0.0f;
    for (e = 0; e < ne; e++) {
        float r = pred->rate(pred->ctx, type, p->e[e].type);
        if (!(r >= 0.0f && isfinite(r))) r = 0.0f;
        sum += clamp01(p->e[e].share) * r;
    }
    return sum;
}

int impai_class_type(const IMPAI_TABLE* t, const IMPAI_PICTURE* p, const IMPAI_MENU* m, unsigned k,
                     const IMPAI_PREDICTOR* pred)
{
    float raw[IMPAI_MENU_MAX], top = 0.0f, best_score = 0.0f;
    unsigned i, n = m->n < IMPAI_MENU_MAX ? m->n : IMPAI_MENU_MAX;
    int best = -1, rated = pred && pred->rate && pred->expf;
    for (i = 0; i < n; i++) {
        raw[i] = 0.0f;
        if (!usable(&m->t[i], k) || !rated) continue;
        raw[i] = against(p, pred, m->t[i].type);
        if (raw[i] > top) top = raw[i];
    }
    for (i = 0; i < n; i++) {
        float e, score;
        if (!usable(&m->t[i], k)) continue;
        e = top > 0.0f ? raw[i] / top : 0.0f;
        score = (top > 0.0f ? pred->expf(t->counter * e) : 1.0f) / m->t[i].value;
        if (best < 0 || score > best_score || (score == best_score && m->t[i].type < (uint16_t)best)) {
            best = m->t[i].type;
            best_score = score;
        }
    }
    return best;
}

int impai_buy_next(const float share[IMPAI_KS], const float spent[IMPAI_KS], const float cost[IMPAI_KS],
                   unsigned eligible, float left)
{
    float total = 0.0f, best_deficit = 0.0f;
    int k, best = -1;
    for (k = 0; k < IMPAI_KS; k++) total += spent[k];
    for (k = 0; k < IMPAI_KS; k++) {
        float deficit;
        if (!((eligible >> k) & 1u) || !(share[k] > 0.0f) || !(cost[k] > 0.0f) || !(cost[k] <= left)) continue;
        deficit = share[k] * total - spent[k];
        if (best < 0 || deficit > best_deficit) {
            best = k;
            best_deficit = deficit;
        }
    }
    return best;
}

int impai_buy(const IMPAI_TABLE* t, const IMPAI_PICTURE* p, float V, const IMPAI_MENU* m,
              const IMPAI_PREDICTOR* pred, IMPAI_ARMY* out)
{
    float cost[IMPAI_KS], spent[IMPAI_KS];
    unsigned i, k;
    if (m->n > IMPAI_MENU_MAX || p->ne > IMPAI_PIC_ENEMY || !(V >= 0.0f) || !isfinite(V)) return -1;
    for (i = 0; i < m->n; i++)
        if (m->t[i].klass >= IMPAI_KS) return -1;
    memset(out, 0, sizeof *out);
    impai_table_shares(t, p, impai_menu_classes(m), out->share);
    for (k = 0; k < IMPAI_KS; k++) {
        spent[k] = 0.0f;
        cost[k] = 0.0f;
        out->type[k] = out->share[k] > 0.0f ? impai_class_type(t, p, m, k, pred) : -1;
        for (i = 0; out->type[k] >= 0 && i < m->n; i++)
            if (m->t[i].type == (uint16_t)out->type[k] && m->t[i].klass == k) {
                cost[k] = m->t[i].value;
                break;
            }
    }
    while (out->units < IMPAI_BUY_UNITS) {
        int c = impai_buy_next(out->share, spent, cost, (1u << IMPAI_KS) - 1u, V - out->spent);
        if (c < 0) break;
        out->n[c]++;
        out->units++;
        spent[c] += cost[c];
        out->spent += cost[c];
    }
    return 0;
}
