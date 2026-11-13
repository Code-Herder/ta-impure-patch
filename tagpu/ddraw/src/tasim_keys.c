/* tasim_keys.c -- the key table and the one rule per key (tasim_keys.h). Each table runs in its parser's order, read
   from the disassembly of pristine/TotalA.exe.pristine: 0x42BF40 (a unit's FBI), 0x440340 (a move class), 0x42E440
   (a weapon), 0x4224B0 and 0x422EA0 (a feature); research/notes/engine/keys.md lists every key with its store. */
#include <string.h>
#include "tasim_fixed.h"
#include "tasim_keys.h"

/* ---- the number getters ------------------------------------------------------------------------------------- */

/* isspace in the C locale, the ctype bit 0x08 the CRT's atol (0x4E4ED8..0x4E4F07) and atof test */
static int c_space(unsigned char c) { return c == 0x20 || (c >= 0x09 && c <= 0x0D); }
static int c_digit(unsigned char c) { return c >= '0' && c <= '9'; }

int32_t ts_tdf_atol(const char* s)
{
    const unsigned char* p = (const unsigned char*)s;
    unsigned char c, sign;
    uint32_t n = 0;
    while (c_space(*p)) p++;
    c = *p++;
    sign = c;
    if (c == '-' || c == '+') c = *p++;
    while (c_digit(c)) {
        n = n * 10u + (uint32_t)(c - '0');                          /* the 32-bit wrap of `lea`, unchecked */
        c = *p++;
    }
    return (int32_t)(sign == '-' ? 0u - n : n);
}

/* A big unsigned integer, little-endian 32-bit limbs. THE BOUND: ts_tdf_atof keeps at most 41 significant digits
   (< 2^137) and an exponent in [-365, 310] past which the answer is 0 or infinity without arithmetic, so no value
   here is wider than 10^41 x 10^310 or 2^56 x 10^365 (< 2^1270): BN_LIMBS = 48 holds 1536 bits. */
#define BN_LIMBS 48
typedef struct { uint32_t v[BN_LIMBS]; int n; } BN;

static void bn_set(BN* a, uint32_t x) { memset(a, 0, sizeof *a); a->v[0] = x; a->n = x ? 1 : 0; }

static void bn_muladd(BN* a, uint32_t m, uint32_t add)
{
    uint64_t carry = add;
    int i;
    for (i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->v[i] * m + carry;
        a->v[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && a->n < BN_LIMBS) a->v[a->n++] = (uint32_t)carry;
}

static int bn_bits(const BN* a)
{
    int b;
    uint32_t top;
    if (!a->n) return 0;
    top = a->v[a->n - 1];
    for (b = 0; top; b++) top >>= 1;
    return (a->n - 1) * 32 + b;
}

static void bn_shl(BN* a, int k)
{
    int w = k / 32, b = k % 32, i;
    if (!a->n || k <= 0) return;
    for (i = a->n - 1 + w + 1; i >= 0; i--) {
        uint32_t hi = (i - w >= 0 && i - w < a->n) ? a->v[i - w] : 0u;
        uint32_t lo = (i - w - 1 >= 0 && i - w - 1 < a->n) ? a->v[i - w - 1] : 0u;
        if (i < BN_LIMBS) a->v[i] = b ? (hi << b) | (lo >> (32 - b)) : hi;
    }
    a->n = a->n + w + 1 < BN_LIMBS ? a->n + w + 1 : BN_LIMBS;
    while (a->n && !a->v[a->n - 1]) a->n--;
}

static void bn_shr1(BN* a)
{
    int i;
    for (i = 0; i < a->n; i++)
        a->v[i] = (a->v[i] >> 1) | (i + 1 < a->n ? a->v[i + 1] << 31 : 0u);
    while (a->n && !a->v[a->n - 1]) a->n--;
}

static int bn_cmp(const BN* a, const BN* b)
{
    int i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return 0;
}

static void bn_sub(BN* a, const BN* b)                             /* a -= b, a >= b */
{
    int64_t borrow = 0;
    int i;
    for (i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->v[i] - (i < b->n ? (int64_t)b->v[i] : 0) - borrow;
        borrow = t < 0;
        a->v[i] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
    }
    while (a->n && !a->v[a->n - 1]) a->n--;
}

static double bits_double(uint64_t b)
{
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}

/* digits x 10^dexp, correctly rounded to a double (round half to even), its sign `neg` */
static double decimal_to_double(const char* dig, int nd, int dexp, int neg)
{
    const uint64_t sign = neg ? (uint64_t)1 << 63 : 0;
    BN num, den;
    uint64_t q = 0, m;
    int s, e, drop, b, sticky, i;
    if (nd + dexp > 310) return bits_double(sign | 0x7FF0000000000000ull);
    if (nd + dexp < -324) return bits_double(sign);                 /* below 2^-1075, half the least subnormal */
    bn_set(&num, 0);
    for (i = 0; i < nd; i++) bn_muladd(&num, 10, (uint32_t)(dig[i] - '0'));
    bn_set(&den, 1);
    for (i = 0; i < (dexp < 0 ? -dexp : dexp); i++) bn_muladd(dexp < 0 ? &den : &num, 10, 0);
    /* s puts the quotient in [2^54, 2^56): num 2^s / den >= 2^(bits(num) - 1 + s - bits(den)) = 2^54 */
    s = 55 + bn_bits(&den) - bn_bits(&num);
    if (s >= 0) bn_shl(&num, s); else bn_shl(&den, -s);
    bn_shl(&den, 56);
    for (b = 56; b >= 0; b--) {
        if (bn_cmp(&num, &den) >= 0) { bn_sub(&num, &den); q |= (uint64_t)1 << b; }
        bn_shr1(&den);
    }
    sticky = num.n != 0;
    if (q >> 55) { sticky |= (int)(q & 1); q >>= 1; s--; }
    e = 54 - s;                                                     /* the value is in [2^e, 2^(e+1)) */
    drop = e >= -1022 ? 2 : 2 + (-1022 - e);                        /* 53 bits kept, fewer for a subnormal */
    if (drop > 56) return bits_double(sign);
    m = q >> drop;
    {
        const uint64_t r = q & (((uint64_t)1 << drop) - 1), half = (uint64_t)1 << (drop - 1);
        if (r > half || (r == half && (sticky || (m & 1)))) m++;
    }
    if (e < -1022) return bits_double(sign | m);                    /* m = 2^52 is the least normal, as bits */
    if (m >> 53) { m >>= 1; e++; }
    if (e > 1023) return bits_double(sign | 0x7FF0000000000000ull);
    return bits_double(sign | ((uint64_t)(e + 1023) << 52) | (m & 0xFFFFFFFFFFFFFull));
}

#define DIG_KEEP 40

double ts_tdf_atof(const char* s)
{
    const unsigned char* p = (const unsigned char*)s;
    char dig[DIG_KEEP + 1];
    int nd = 0, dexp = 0, any = 0, sticky = 0, neg = 0;
    while (c_space(*p)) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    for (; c_digit(*p); p++) {
        any = 1;
        if (!nd && *p == '0') continue;
        if (nd < DIG_KEEP) dig[nd++] = (char)*p;
        else { dexp++; sticky |= *p != '0'; }
    }
    if (*p == '.')
        for (p++; c_digit(*p); p++) {
            any = 1;
            if (!nd && *p == '0') { dexp--; continue; }
            if (nd < DIG_KEEP) { dig[nd++] = (char)*p; dexp--; }
            else sticky |= *p != '0';
        }
    if (!any) return 0.0;                                           /* _fltin's no-digits answer */
    if (*p == 'e' || *p == 'E' || *p == 'd' || *p == 'D') {
        const unsigned char* x = p + 1;
        int eneg = 0;
        long ev = 0;
        if (*x == '-' || *x == '+') eneg = *x++ == '-';
        if (c_digit(*x)) {
            for (; c_digit(*x); x++)
                if (ev < 99999) ev = ev * 10 + (*x - '0');
            dexp += (int)(eneg ? -ev : ev);
        }
    }
    if (!nd) return neg ? -0.0 : 0.0;
    if (sticky) { dig[nd++] = '1'; dexp--; }                        /* only a tie can turn on the digits cut */
    return decimal_to_double(dig, nd, dexp, neg);
}

int32_t ts_tdf_fx(const char* s)
{
    return ts_ftol(ts_tdf_atof(s) * 65536.0);
}

/* ---- the tables --------------------------------------------------------------------------------------------- */

#define FW(T, f) ((uint16_t)sizeof(((T*)0)->f))
#define ROW(T, n, o, f, k, w, bit, rule, land, from, d, dd, sc) \
    { n, o, (uint16_t)offsetof(T, f), w, k, bit, rule, land, from, d, dd, sc }

/* the unit rows */
#define UI(n, o, f, l, d)   ROW(TASIM_UNITDEF, n, o, f, TSK_INT, FW(TASIM_UNITDEF, f), 0, TSR_CONST, l, 0, d, 0, 0)
#define UIF(n, o, f, l)     ROW(TASIM_UNITDEF, n, o, f, TSK_INT_FLT, 4, 0, TSR_CONST, l, 0, 0, 0, 0)
#define UFX(n, o, f, l, d)  ROW(TASIM_UNITDEF, n, o, f, TSK_FX, 4, 0, TSR_CONST, l, 0, d, 0, 0)
#define UFX2(n, o, f, l)    ROW(TASIM_UNITDEF, n, o, f, TSK_FX, 4, 0, TSR_TWICE, l, \
                                (uint16_t)offsetof(TASIM_UNITDEF, maxvelocity), 0, 0, 0)
#define UDF(n, o, f, l)     ROW(TASIM_UNITDEF, n, o, f, TSK_DBL_FLT, 4, 0, TSR_CONST, l, 0, 0, 0, 0)
#define UB(n, w, b, l)      ROW(TASIM_UNITDEF, n, 0x##w, flags##w, TSK_BIT, 4, b, TSR_CONST, l, 0, 0, 0, 0)
#define US(n, o, f, l)      ROW(TASIM_UNITDEF, n, o, f, TSK_STR, FW(TASIM_UNITDEF, f), 0, TSR_CONST, l, 0, 0, 0, 0)
#define UK(n, l)            ROW(TASIM_UNITDEF, n, TS_NOOFF, name, TSK_KNOWN, 0, 0, TSR_CONST, l, 0, 0, 0, 0)

const TS_KEY tasim_keys_unit[] = {
    US("unitname",              0x020, unitname,           TSL_T1A),
    US("name",                  0x000, name,               TSL_OUT),   /* 0x4C58A0, a language's key first [INFERRED] */
    US("description",           0x040, description,        TSL_OUT),
    US("defaultmissiontype",    0x230, defaultmissiontype, TSL_T1B),
    US("wpri_badTargetCategory", 0x231, badtarget[0],      TSL_T2),
    US("wsec_badTargetCategory", 0x235, badtarget[1],      TSL_T2),
    US("wspe_badTargetCategory", 0x239, badtarget[2],      TSL_T2),
    US("noChaseCategory",       0x23D, nochase,            TSL_T2),
    ROW(TASIM_UNITDEF, "objectname", 0x080, objectname, TSK_STR, FW(TASIM_UNITDEF, objectname), 0, TSR_COPY,
        TSL_T1B, (uint16_t)offsetof(TASIM_UNITDEF, unitname), 0, 0, 0),
    UIF("buildcostenergy",      0x186, buildcostenergy,    TSL_T1C),
    UIF("buildcostmetal",       0x18A, buildcostmetal,     TSL_T1C),
    UFX("maxvelocity",          0x192, maxvelocity,        TSL_T1B, 0),
    UFX("brakerate",            0x19A, brakerate,          TSL_T1B, 0),
    UFX("acceleration",         0x19E, acceleration,       TSL_T1B, 0),
    UFX("bankscale",            0x1A2, bankscale,          TSL_T3, 0x10000),
    UFX("pitchscale",           0x1A6, pitchscale,         TSL_T3, 0),
    UFX("damagemodifier",       0x1AA, damagemodifier,     TSL_T2, 0x10000),
    UFX2("moverate1",           0x1AE, moverate1,          TSL_T1B),
    UFX2("moverate2",           0x1B2, moverate2,          TSL_T1B),
    UI("turnrate",              0x1BA, turnrate,           TSL_T1B, 0),
    UI("waterline",             0x22C, waterline,          TSL_T1B, 0),
    UI("transportsize",         0x22A, transportsize,      TSL_T3, 0),
    UI("transportcapacity",     0x22B, transportcapacity,  TSL_T3, 0),
    UDF("energymake",           0x1C2, energymake,         TSL_T1C),
    UDF("energyuse",            0x1C6, energyuse,          TSL_T1C),
    UDF("metalmake",            0x1CA, metalmake,          TSL_T1C),
    UDF("extractsmetal",        0x1CE, extractsmetal,      TSL_T1C),
    UI("makesmetal",            0x22D, makesmetal,         TSL_T1C, 0),
    UDF("windgenerator",        0x1D2, windgenerator,      TSL_T1C),
    UDF("tidalgenerator",       0x1D6, tidalgenerator,     TSL_T1C),
    UDF("energystorage",        0x1E2, energystorage,      TSL_T1C),
    UDF("metalstorage",         0x1E6, metalstorage,       TSL_T1C),
    UI("buildtime",             0x1EA, buildtime,          TSL_T1C, 0),
    UI("workertime",            0x1FE, workertime,         TSL_T1C, 0),
    UI("healtime",              0x200, healtime,           TSL_T1C, 0),
    UI("maxdamage",             0x1FA, maxdamage,          TSL_T1C, 0),
    UI("sightdistance",         0x202, sightdistance,      TSL_T1D, 0),
    UI("radardistance",         0x204, radardistance,      TSL_T1D, 0),
    UI("sonardistance",         0x206, sonardistance,      TSL_T1D, 0),
    UI("radardistancejam",      0x20A, radardistancejam,   TSL_T1D, 0),
    UI("sonardistancejam",      0x20C, sonardistancejam,   TSL_T1D, 0),
    UI("bmcode",                0x22F, bmcode,             TSL_T1A, 0),
    ROW(TASIM_UNITDEF, "standingmoveorder", 0x241, flags241, TSK_BIT2, 4, 0, TSR_CONST, TSL_T1B, 0, 2, 0, 0),
    ROW(TASIM_UNITDEF, "standingfireorder", 0x241, flags241, TSK_BIT2, 4, 2, TSR_CONST, TSL_T2, 0, 2, 0, 0),
    UB("init_cloaked",          241, 4,  TSL_T3),
    UB("downloadable",          241, 5,  TSL_OUT),
    UB("builder",               241, 6,  TSL_T1C),
    UB("stealth",               241, 8,  TSL_T3),
    /* cloakcost twice: the int the default of cloakcostmoving is made from (0x42C512..0x42C525: _ftol of the
       fild'ed int, which fst left on the stack), then the float stored at +0x1DA */
    ROW(TASIM_UNITDEF, "cloakcost", TS_NOOFF, cloakcost_read, TSK_INT, 4, 0, TSR_CONST, TSL_T3, 0, 0, 0, 0),
    ROW(TASIM_UNITDEF, "cloakcost", 0x1DA, cloakcost, TSK_INT_FLT, 4, 0, TSR_CONST, TSL_T3, 0, 0, 0, 0),
    ROW(TASIM_UNITDEF, "cloakcostmoving", 0x1DE, cloakcostmoving, TSK_INT_FLT, 4, 0, TSR_COPY, TSL_T3,
        (uint16_t)offsetof(TASIM_UNITDEF, cloakcost_read), 0, 0, 0),
    UI("mincloakdistance",      0x208, mincloakdistance,   TSL_T3, 0),
    UI("buildangle",            0x210, buildangle,         TSL_T1C, 0),
    UI("builddistance",         0x212, builddistance,      TSL_T1C, 0),
    UI("sortbias",              0x21A, sortbias,           TSL_OUT, 0),
    UI("cruisealt",             0x21C, cruisealt,          TSL_T3, 0),
    UB("zbuffer",               241, 7,  TSL_OUT),
    UB("isairbase",             241, 9,  TSL_T3),
    UB("istargetingupgrade",    241, 10, TSL_AI),
    UB("teleporter",            241, 13, TSL_OUT),                     /* no reader (keys.md) */
    UB("hidedamage",            241, 14, TSL_OUT),
    UB("shootme",               241, 15, TSL_T2),
    UB("armoredstate",          241, 17, TSL_OUT),                     /* no reader */
    UB("activatewhenbuilt",     241, 18, TSL_T1C),
    UB("canfly",                241, 11, TSL_T3),
    UB("canhover",              241, 12, TSL_T1B),
    UB("upright",               241, 20, TSL_T1B),
    UB("floater",               241, 19, TSL_T1B),
    UB("amphibious",            241, 21, TSL_T1B),
    UB("isfeature",             241, 24, TSL_T1C),
    UB("noshadow",              241, 25, TSL_OUT),
    UB("immunetoparalyzer",     241, 26, TSL_T2),
    UB("hoverattack",           241, 27, TSL_OUT),                     /* no reader */
    UB("antiweapons",           241, 29, TSL_T2),
    UB("digger",                241, 30, TSL_OUT),
    UB("onoffable",             245, 2,  TSL_T1C),
    UB("mobilestandorders",     245, 0,  TSL_T1B),
    UB("firestandorders",       245, 1,  TSL_T2),
    UB("canstop",               245, 3,  TSL_OUT),                     /* no reader */
    UB("canattack",             245, 4,  TSL_T2),
    UB("canguard",              245, 5,  TSL_T1B),
    UB("canpatrol",             245, 6,  TSL_T1B),
    UB("canmove",               245, 7,  TSL_T1B),
    UB("canload",               245, 8,  TSL_T3),
    UB("canreclamate",          245, 10, TSL_T1C),
    UB("canresurrect",          245, 11, TSL_T1C),
    UB("cancapture",            245, 12, TSL_T1C),
    UB("candgun",               245, 14, TSL_T2),
    UI("maneuverleashlength",   0x214, maneuverleashlength, TSL_T2, 0),
    UI("attackrunlength",       0x216, attackrunlength,    TSL_T3, 0),
    UB("kamikaze",              241, 28, TSL_T2),
    UI("kamikazedistance",      0x218, kamikazedistance,   TSL_T2, 0),
    UB("norestrict",            245, 15, TSL_OUT),
    UB("showplayername",        245, 17, TSL_OUT),
    UB("commander",             245, 18, TSL_OUT),                     /* read only by the bottom bar */
    UB("cantbetransported",     245, 19, TSL_T3),
    ROW(TASIM_UNITDEF, "selfdestructcountdown", 0x245, flags245, TSK_SDCOUNT, 4, 20, TSR_CONST, TSL_T1C, 0, 5, 0, 0),
    US("category",              TS_NOOFF, category,        TSL_T2),
    US("soundcategory",         0x20E, soundcategory,      TSL_OUT),
    US("corpse",                0x1BC, corpse,             TSL_T1C),
    US("movementclass",         0x1B6, movementclass,      TSL_T1B),
    US("weapon1",               0x1EE, weapon[0],          TSL_T2),
    US("weapon2",               0x1F2, weapon[1],          TSL_T2),
    US("weapon3",               0x1F6, weapon[2],          TSL_T2),
    US("explodeas",             0x220, explodeas,          TSL_T2),
    US("selfdestructas",        0x224, selfdestructas,     TSL_T2),
    /* read by the tail of tasim_unitdef_read, not in this pass: YardMap only for bmcode 0 */
    US("YardMap",               0x14E, yardmap,            TSL_T1A),
    /* the pre-scan 0x42A8D0's own keys */
    US("side",                  TS_NOOFF, side,            TSL_T1C),
    UK("ai_weight",             TSL_AI),
    UK("ai_limit",              TSL_AI),
    UK("wacky",                 TSL_OUT),
    UK("Version",               TSL_OUT),
    UK("Copyright",             TSL_OUT),
    /* Impure's DLL (research/notes/modules/data-keys.md) */
    US("weapon4",  TS_NOOFF, weapon[3], TSL_T2), US("weapon5",  TS_NOOFF, weapon[4], TSL_T2),
    US("weapon6",  TS_NOOFF, weapon[5], TSL_T2), US("weapon7",  TS_NOOFF, weapon[6], TSL_T2),
    US("weapon8",  TS_NOOFF, weapon[7], TSL_T2), US("weapon9",  TS_NOOFF, weapon[8], TSL_T2),
    US("weapon10", TS_NOOFF, weapon[9], TSL_T2),
    UK("w4_badTargetCategory", TSL_T2), UK("w5_badTargetCategory", TSL_T2), UK("w6_badTargetCategory", TSL_T2),
    UK("w7_badTargetCategory", TSL_T2), UK("w8_badTargetCategory", TSL_T2), UK("w9_badTargetCategory", TSL_T2),
    UK("w10_badTargetCategory", TSL_T2),
    UK("VeterancyThresholds",   TSL_T2),
    UK("VeterancyAccuracyBuffRate", TSL_T2),
    UK("TransportedExplodeAs",  TSL_T3),
    UK("TransportedSelfDestructAs", TSL_T3),
    UK("PreviewPieces",         TSL_OUT),
    { NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/* the move class rows: 0x440340's six getters and two derived defaults */
#define CI(n, o, f, d, r)   ROW(TASIM_MOVECLASS, n, o, f, TSK_INT, FW(TASIM_MOVECLASS, f), 0, r, TSL_T1B, \
                                (uint16_t)offsetof(TASIM_MOVECLASS, f), d, 0, 0)

const TS_KEY tasim_keys_class[] = {
    CI("FootPrintX",     0x04, footx,         0, TSR_CONST),
    CI("FootPrintZ",     0x06, footz,         0, TSR_CONST),
    CI("maxwaterdepth",  0x08, maxwaterdepth, 0, TSR_COPY),           /* the record's value is the default */
    CI("minwaterdepth",  0x0A, minwaterdepth, 0, TSR_COPY),
    CI("maxslope",       0x0C, maxslope,      0, TSR_COPY),
    CI("badslope",       0x0D, badslope,      0, TSR_CONST),          /* tasim_moveclass_read: maxslope >> 1 */
    CI("maxwaterslope",  0x0E, maxwaterslope, 0, TSR_COPY),
    CI("badwaterslope",  0x0F, badwaterslope, 0, TSR_CONST),
    { NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/* the weapon rows */
#define WI(n, o, f, d)      ROW(TASIM_WEAPONDEF, n, o, f, TSK_INT, FW(TASIM_WEAPONDEF, f), 0, TSR_CONST, TSL_T2, 0, d, 0, 0)
#define WDF(n, o, f)        ROW(TASIM_WEAPONDEF, n, o, f, TSK_DBL_FLT, 4, 0, TSR_CONST, TSL_T2, 0, 0, 0, 0)
#define WDT(n, o, f, sc)    ROW(TASIM_WEAPONDEF, n, o, f, TSK_DBL_FTOL, FW(TASIM_WEAPONDEF, f), 0, TSR_CONST, TSL_T2, \
                                0, 0, 0, sc)
#define WB(n, b)            ROW(TASIM_WEAPONDEF, n, 0x111, flags, TSK_BIT, 4, b, TSR_CONST, TSL_T2, 0, 0, 0, 0)
#define WS(n, o, f, l)      ROW(TASIM_WEAPONDEF, n, o, f, TSK_STR, FW(TASIM_WEAPONDEF, f), 0, TSR_CONST, l, 0, 0, 0, 0)
#define WK(n, l)            ROW(TASIM_WEAPONDEF, n, TS_NOOFF, name, TSK_KNOWN, 0, 0, TSR_CONST, l, 0, 0, 0, 0)
#define WKB(n, b, l)        ROW(TASIM_WEAPONDEF, n, TS_NOOFF, wkeys, TSK_BIT, 1, b, TSR_CONST, l, 0, 0, 0, 0)

/* The exe's constants, MEASURED 2026-11-12 (the doubles' bytes read from pristine/TotalA.exe.pristine): 0x4FD240 is
   0x1.1111111111111p+11 and 0x4FD248 0x1.23456789abcdfp+6, the doubles nearest 65536/30 and 65536/900; 0x4FD250
   and 0x4FD038 are 30.0; 0x4FD258 0x1.1111111111111p-5, nearest 1/30; 0x4FD260 0x1.1df46a2529d34p-6, which is
   NOT the double nearest pi/180 (0x1.1df46a2529d39p-6, 5 ulp above): the literal below is the exe's. */
#define K_PER_TICK   2184.5333333333333
#define K_PER_TICK2  72.81777777777778
#define K_SECONDS    30.0
#define K_PER_SECOND 0.03333333333333333
#define K_DEGREES    0.017453292519943278

const TS_KEY tasim_keys_weapon[] = {
    WI("ID",                 TS_NOOFF, id, -1),
    WS("name",               0x020, name, TSL_T2),
    WDT("weaponvelocity",    0x068, weaponvelocity, K_PER_TICK),
    WDT("startvelocity",     0x06C, startvelocity, K_PER_TICK),
    WDT("weaponacceleration", 0x070, weaponacceleration, K_PER_TICK2),
    WI("range",              0x0DC, range, 0x7FFF),
    WI("coverage",           0x0E0, coverage, 0),
    WDT("reloadtime",        0x0E4, reloadtime, K_SECONDS),
    WDF("energypershot",     0x0C0, energypershot),
    WDF("metalpershot",      0x0C4, metalpershot),
    WI("areaofeffect",       0x0D6, areaofeffect, 0),
    WDF("edgeeffectiveness", 0x0D8, edgeeffectiveness),
    WDT("weapontimer",       0x0E6, weapontimer, K_SECONDS),
    WB("noautorange",        27),
    WDT("turnrate",          0x0E8, turnrate, K_PER_SECOND),
    WI("burst",              0x0EA, burst, 0),
    WDT("burstrate",         0x0EC, burstrate, K_SECONDS),
    WI("sprayangle",         0x0EE, sprayangle, 0),
    WDT("duration",          0x0F0, duration, K_SECONDS),
    WDT("randomdecay",       0x0F2, randomdecay, K_SECONDS),
    WDT("smokedelay",        0x0FA, smokedelay, K_SECONDS),
    WDT("flighttime",        0x0FC, flighttime, K_SECONDS),
    WDT("holdtime",          0x0FE, holdtime, K_SECONDS),
    ROW(TASIM_WEAPONDEF, "minbarrelangle", 0x0C8, minbarrelangle, TSK_DBL_FLT_SC, 4, 0, TSR_CONST, TSL_T2, 0, 0,
        -11.25, K_DEGREES),                                          /* the pushed default 0xC0268000:00000000 */
    WI("firestarter",        0x10B, firestarter, 0),
    WI("rendertype",         0x10C, rendertype, 0),
    WI("color",              0x10D, color, 0),
    WI("color2",             0x10E, color2, 0),
    WB("soundtrigger", 11), WB("guidance", 12), WB("tracks", 13), WB("lineofsight", 0), WB("ballistic", 1),
    WB("unitsonly", 14), WB("groundbounce", 15), WB("waterweapon", 16), WB("toairweapon", 17), WB("smoketrail", 18),
    WB("turret", 19), WB("selfprop", 20), WB("propeller", 21), WB("noexplode", 22), WB("burnblow", 23),
    WB("twophase", 24), WB("cruise", 25), WB("commandfire", 26), WB("stockpile", 28), WB("targetable", 29),
    WB("interceptor", 30), WB("beamweapon", 3), WB("shellweapon", 2), WB("dropped", 8), WB("vlaunch", 4),
    WB("meteor", 5), WB("noradar", 6), WB("paralyzer", 7), WB("startsmoke", 9), WB("endsmoke", 10),
    WI("accuracy",           0x104, accuracy, 0),
    WI("tolerance",          0x106, tolerance, 0),
    WI("pitchtolerance",     0x108, pitchtolerance, 0),
    WI("shakemagnitude",     0x0CC, shakemagnitude, 0),
    WDT("shakeduration",     0x0D0, shakeduration, K_SECONDS),
    WS("model",              0x074, model, TSL_OUT),
    WK("explosiongaf", TSL_OUT), WK("explosionart", TSL_OUT), WK("lavaexplosiongaf", TSL_OUT),
    WK("lavaexplosionart", TSL_OUT), WK("waterexplosiongaf", TSL_OUT), WK("waterexplosionart", TSL_OUT),
    WK("soundstart", TSL_OUT), WK("soundhit", TSL_OUT), WK("soundwater", TSL_OUT),
    /* Impure's DLL: tagpu_datakeys_weapon_read takes each as E_TdfGetInt(sec, key, 0) & 1 into its TAGPU_WK_* bit */
    WKB("nottoair", 0, TSL_T2), WKB("nottounderwater", 1, TSL_T2), WKB("surfacefire", 2, TSL_T2),
    WKB("notoverwater", 3, TSL_T2), WKB("notoverland", 4, TSL_T2), WKB("nomapweaponalert", 5, TSL_OUT),
    { NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/* the feature rows */
#define FI(n, o, f, l, d)   ROW(TASIM_FEATUREDEF, n, o, f, TSK_INT, FW(TASIM_FEATUREDEF, f), 0, TSR_CONST, l, 0, d, 0, 0)
#define FB(n, b, l, d)      ROW(TASIM_FEATUREDEF, n, 0xFE, flags, TSK_BIT, 2, b, TSR_CONST, l, 0, d, 0, 0)
#define FS(n, o, f, l)      ROW(TASIM_FEATUREDEF, n, o, f, TSK_STR, FW(TASIM_FEATUREDEF, f), 0, TSR_CONST, l, 0, 0, 0, 0)

const TS_KEY tasim_keys_feature[] = {
    FS("Description",        0x80, description, TSL_OUT),
    FI("footprintx",         0x94, footx, TSL_T1A, 0),
    FI("footprintz",         0x96, footz, TSL_T1A, 0),
    FI("height",             0xFA, height, TSL_T2, 0),
    FS("object",             TS_NOOFF, object, TSL_T1A),
    /* the GAF keys, read only when there is no object (0x42261B jumps past them) */
    FS("filename",           TS_NOOFF, filename, TSL_OUT),
    FS("seqname",            TS_NOOFF, seq[0], TSL_OUT),
    FS("seqnameshad",        TS_NOOFF, seq[1], TSL_OUT),
    FS("seqnameburn",        TS_NOOFF, seq[2], TSL_OUT),
    FS("seqnameburnshad",    TS_NOOFF, seq[3], TSL_OUT),
    FS("seqnamedie",         TS_NOOFF, seq[4], TSL_OUT),
    FS("seqnamedieshad",     TS_NOOFF, seq[5], TSL_OUT),
    FS("seqnamereclamate",   TS_NOOFF, seq[6], TSL_OUT),
    FS("seqnamereclamateshad", TS_NOOFF, seq[7], TSL_OUT),
    FI("spreadchance",       0xFB, spreadchance, TSL_T2, 0),
    FI("reproduce",          0xFC, reproduce, TSL_T1C, 0),
    FI("reproducearea",      0xFD, reproducearea, TSL_T1C, 0),
    ROW(TASIM_FEATUREDEF, "metal",  0xF0, metal,  TSK_INT_U16FLT, 4, 0, TSR_CONST, TSL_T1C, 0, 0, 0, 0),
    ROW(TASIM_FEATUREDEF, "energy", 0xEC, energy, TSK_INT_U16FLT, 4, 0, TSR_CONST, TSL_T1C, 0, 0, 0, 0),
    FI("damage",             0xEA, damage, TSL_T2, 0),
    FB("animating", 1, TSL_OUT, 0), FB("animtrans", 2, TSL_OUT, 0), FB("shadtrans", 3, TSL_OUT, 0),
    FB("flamable", 4, TSL_T2, 0), FB("geothermal", 5, TSL_T1C, 0), FB("blocking", 6, TSL_T1A, 0),
    FB("reclaimable", 7, TSL_T1C, 0), FB("autoreclaimable", 8, TSL_T1C, 1), FB("indestructible", 9, TSL_T2, 0),
    FB("nodisplayinfo", 10, TSL_OUT, 0), FB("nodrawundergray", 11, TSL_OUT, 0),
    ROW(TASIM_FEATUREDEF, "sparktime", 0xE8, sparktime, TSK_DBL_FTOL, 2, 0, TSR_CONST, TSL_T2, 0, 0, 0, K_SECONDS),
    FS("burnweapon",         0xE4, burnweapon, TSL_T2),
    /* 0x422EA0, the successors */
    FS("featuredead",        TS_NOOFF, successor[0], TSL_T2),
    FS("featurereclamate",   TS_NOOFF, successor[1], TSL_T1C),
    FS("featureburnt",       TS_NOOFF, successor[2], TSL_T2),
    { NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/* the map OTA's keys (0x435DA0), each with the landing whose rules read it. No harness reads an OTA through this
   table: ts_map_load takes SurfaceMetal and lavaworld as values (TS_MAPLOAD), which classcheck.py reads. */
#define MK(n, l)            { n, TS_NOOFF, 0, 0, TSK_KNOWN, 0, TSR_CONST, l, 0, 0, 0, 0 }

const TS_KEY tasim_keys_map[] = {
    MK("missionfile", TSL_OUT), MK("missionname", TSL_OUT), MK("brief", TSL_OUT), MK("narration", TSL_OUT),
    MK("missionhint", TSL_OUT), MK("maxunits", TSL_T1A), MK("glamour", TSL_OUT), MK("glamoursound", TSL_OUT),
    MK("UseOnlyUnits", TSL_OUT), MK("mapping", TSL_T1D), MK("lineofsight", TSL_T1D), MK("memory", TSL_OUT),
    MK("numplayers", TSL_OUT), MK("Planet", TSL_OUT), MK("nomovie", TSL_OUT), MK("missiondescription", TSL_OUT),
    MK("minwindspeed", TSL_T1C), MK("maxwindspeed", TSL_T1C), MK("gravity", TSL_T2), MK("tidalstrength", TSL_T1C),
    MK("lavaworld", TSL_T2), MK("nosealeveltrigger", TSL_OUT), MK("waterdoesdamage", TSL_T2),
    MK("waterdamage", TSL_T2), MK("killmul", TSL_OUT), MK("timemul", TSL_OUT), MK("HumanMetal", TSL_T1C),
    MK("HumanEnergy", TSL_T1C), MK("ComputerMetal", TSL_T1C), MK("ComputerEnergy", TSL_T1C),
    MK("SurfaceMetal", TSL_T1C), MK("aiprofile", TSL_AI), MK("MeteorWeapon", TSL_T2), MK("MeteorRadius", TSL_T2),
    MK("MeteorDensity", TSL_T2), MK("MeteorDuration", TSL_T2), MK("MeteorInterval", TSL_T2), MK("type", TSL_OUT),
    MK("specialwhat", TSL_OUT),
    { NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/* ---- the one rule per key ---------------------------------------------------------------------------------- */

/* tagpu_hpi_stricmp's rule, the engine's _stricmp 0x4F8A70: ASCII letters folded, compared as unsigned bytes */
static int ieq(const char* a, const char* b)
{
    for (;; a++, b++) {
        unsigned ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

const TS_KEY* tasim_keys_find(const TS_KEY* table, const char* key)
{
    for (; table->name; table++)
        if (ieq(table->name, key)) return table;
    return NULL;
}

static void store(unsigned char* f, int width, int32_t v)
{
    if (width == 1) { uint8_t b = (uint8_t)v; memcpy(f, &b, 1); }
    else if (width == 2) { uint16_t w = (uint16_t)v; memcpy(f, &w, 2); }
    else memcpy(f, &v, 4);
}

static int32_t load32(const unsigned char* f)
{
    int32_t v;
    memcpy(&v, f, 4);
    return v;
}

static int32_t load_width(const unsigned char* f, int width)
{
    if (width == 1) return *f;
    if (width == 2) { int16_t w; memcpy(&w, f, 2); return w; }
    return load32(f);
}

static uint32_t word_get(const unsigned char* f, int width)
{
    uint32_t w = 0;
    memcpy(&w, f, (size_t)width);                                  /* little-endian, as every build is */
    return w;
}

static void word_put(unsigned char* f, int width, uint32_t w) { memcpy(f, &w, (size_t)width); }

int tasim_def_key(void* def, const TS_KEY* k, const char* value)
{
    unsigned char* const base = (unsigned char*)def;
    unsigned char* const f = base + k->field;
    int32_t iv;
    if (k->kind == TSK_KNOWN) return -1;
    switch (k->kind) {
    case TSK_STR: {
        /* 0x4C48C0: present, strncpy of `width` bytes and the last one zeroed; absent, the default copied */
        const char* src = value ? value : (k->rule == TSR_COPY ? (const char*)(base + k->from) : "");
        size_t n = strlen(src);
        if (n > (size_t)k->width - 1) n = (size_t)k->width - 1;
        memset(f, 0, k->width);
        memcpy(f, src, n);
        return value != NULL;
    }
    case TSK_INT:
        iv = value ? ts_tdf_atol(value) : k->rule == TSR_COPY ? load_width(base + k->from, k->width) : k->dflt;
        store(f, k->width, iv);
        break;
    case TSK_INT_FLT:
        iv = value ? ts_tdf_atol(value) : k->rule == TSR_COPY ? load32(base + k->from) : k->dflt;
        { float x = (float)iv; memcpy(f, &x, 4); }
        break;
    case TSK_INT_U16FLT:
        iv = (value ? ts_tdf_atol(value) : k->dflt) & 0xFFFF;        /* 0x422A45: and eax,0xFFFF before fild */
        { float x = (float)iv; memcpy(f, &x, 4); }
        break;
    case TSK_FX:
        /* the default is stored as pushed, already 16.16 (0x4C48A2..0x4C48AD) */
        iv = value ? ts_tdf_fx(value)
                   : k->rule == TSR_TWICE ? (int32_t)((uint32_t)load32(base + k->from) << 1) : k->dflt;
        memcpy(f, &iv, 4);
        break;
    case TSK_DBL_FLT:
        { float x = (float)(value ? ts_tdf_atof(value) : k->dfltd); memcpy(f, &x, 4); }
        break;
    case TSK_DBL_FTOL:
        store(f, k->width, ts_ftol((value ? ts_tdf_atof(value) : k->dfltd) * k->scale));
        break;
    case TSK_DBL_FLT_SC:
        { float x = (float)((value ? ts_tdf_atof(value) : k->dfltd) * k->scale); memcpy(f, &x, 4); }
        break;
    case TSK_BIT:
    case TSK_BIT2: {
        const uint32_t mask = k->kind == TSK_BIT ? 1u : 3u;
        const uint32_t v = (uint32_t)(value ? ts_tdf_atol(value) : k->dflt) & mask;
        word_put(f, k->width, (word_get(f, k->width) & ~(mask << k->bit)) | (v << k->bit));
        break;
    }
    case TSK_SDCOUNT: {
        /* 0x42CBD9..0x42CC19: the raw string through 0x4C4630 and atoi, & 7; absent, bit 21 cleared and
           bits 20 and 22 set, which is 5 */
        const uint32_t v = value ? (uint32_t)ts_tdf_atol(value) & 7u : (uint32_t)k->dflt;
        word_put(f, 4, (word_get(f, 4) & ~(7u << k->bit)) | (v << k->bit));
        break;
    }
    default:
        return -1;
    }
    return value != NULL;
}

/* ---- the readers -------------------------------------------------------------------------------------------- */

void tasim_moveclass_init(TASIM_MOVECLASS* c)
{
    memset(c, 0, sizeof *c);
    c->maxwaterdepth = 10000;
    c->minwaterdepth = -10000;
    c->maxslope = c->badslope = c->maxwaterslope = c->badwaterslope = 255;
}

void tasim_moveclass_read(TASIM_MOVECLASS* c, TS_GET get, void* ctx)
{
    const TS_KEY* k;
    for (k = tasim_keys_class; k->name; k++) {
        if (k->field == offsetof(TASIM_MOVECLASS, badslope)) {
            /* 0x4403AE..0x4403BB: the default is maxslope's getter result, its low byte, shifted right once */
            const char* v = get(ctx, k->name);
            c->badslope = (uint8_t)(v ? ts_tdf_atol(v) : (int32_t)(c->maxslope >> 1));
            continue;
        }
        if (k->field == offsetof(TASIM_MOVECLASS, badwaterslope)) {
            const char* v = get(ctx, k->name);
            c->badwaterslope = (uint8_t)(v ? ts_tdf_atol(v) : (int32_t)(c->maxwaterslope >> 1));
            continue;
        }
        tasim_def_key(c, k, get(ctx, k->name));
    }
    if (c->maxslope > c->maxwaterslope) c->maxslope = c->maxwaterslope;
    if (c->badslope > c->maxslope) c->badslope = c->maxslope;
    if (c->badwaterslope > c->maxwaterslope) c->badwaterslope = c->maxwaterslope;
}

void tasim_unitdef_derive(TASIM_UNITDEF* d)
{
    const int64_t den = (int64_t)(((uint32_t)d->maxslope + 1u) << 16);
    const int32_t fx = d->footx, fz = d->footz;
    d->slopevelocity = (int32_t)(((int64_t)d->maxvelocity << 16) / den);
    d->box[0] = (int32_t)((uint32_t)(-fx) << 20) / 2;             /* +0x15E; cdq, sub, sar 1: toward zero */
    d->box[1] = (int32_t)((uint32_t)(-fz) << 20) / 2;             /* +0x166 */
    d->box[2] = (int32_t)((uint32_t)fx << 20) / 2;                /* +0x16A */
    d->box[3] = (int32_t)((uint32_t)fz << 20) / 2;                /* +0x172 */
    d->box[4] = (int32_t)((uint32_t)d->box[2] - (uint32_t)d->box[0]);   /* +0x176 */
    d->box[5] = (int32_t)((uint32_t)d->box[3] - (uint32_t)d->box[1]);   /* +0x17E */
    d->box[6] = (int32_t)((uint32_t)d->box[5] + (uint32_t)d->box[4]) / 3; /* +0x182 */
}

void tasim_unitdef_read(TASIM_UNITDEF* d, TS_GET get, void* ctx, const TASIM_MOVECLASS classes[TS_CLASSES])
{
    const TS_KEY* k;
    const TS_KEY* const yard = tasim_keys_find(tasim_keys_unit, "YardMap");
    TASIM_MOVECLASS own;
    const TASIM_MOVECLASS* mc = NULL;
    int i;
    for (k = tasim_keys_unit; k->name; k++)
        if (k != yard) tasim_def_key(d, k, get(ctx, k->name));
    /* bit 9 of +0x245 is bit 10's copy (0x42CA3B..0x42CA4F), bit 13 is cloakcost > 0 (0x42CA5A..0x42CA99) */
    d->flags245 = (d->flags245 & ~0x200u) | ((d->flags245 >> 1) & 0x200u);
    d->flags245 = (d->flags245 & ~0x2000u) | (d->cloakcost > 0.0f ? 0x2000u : 0u);
    /* the class: 0x440420 matches the name with _stricmp over the 32 records that have one; no key, an empty
       one or no match leaves +0x1B6 NULL and reads the unit's own section (0x42CD0C..0x42CD51) */
    d->classidx = TS_NONE;
    if (get(ctx, "movementclass"))
        for (i = 0; i < TS_CLASSES; i++)
            if (classes[i].present && ieq(classes[i].name, d->movementclass)) { d->classidx = i; break; }
    if (d->classidx != TS_NONE) mc = &classes[d->classidx];
    else {
        tasim_moveclass_init(&own);
        tasim_moveclass_read(&own, get, ctx);
        mc = &own;
    }
    d->footx = mc->footx;
    d->footz = mc->footz;
    d->maxwaterdepth = mc->maxwaterdepth;
    d->minwaterdepth = mc->minwaterdepth;
    d->maxslope = mc->maxslope;
    d->maxwaterslope = mc->maxwaterslope;
    /* YardMap only for a building (0x42CF2A..0x42CF38); a mobile type's +0x14E is NULL */
    if (d->bmcode == 0) tasim_def_key(d, yard, get(ctx, yard->name));
    else memset(d->yardmap, 0, sizeof d->yardmap);
    tasim_unitdef_derive(d);
    /* 0x42D12A..0x42D13F: a type that cloaks gets 80 when the key left 0 */
    if ((d->flags245 & 0x2000u) && d->mincloakdistance == 0) d->mincloakdistance = 80;
    for (i = 0; i < TS_UNIT_WEAPONS; i++) d->weaponid[i] = TS_NONE;
}

int32_t tasim_weapon_lookup(const TASIM_WEAPONDEF* const* ids, int nids, const char* name)
{
    int s;
    if (!name || !*name) return TS_NONE;
    for (s = 0; s < nids; s++)
        if (ids[s] && ieq(ids[s]->section, name)) return s;
    return TS_NONE;
}

void tasim_unitdef_weapons(TASIM_UNITDEF* d, const TASIM_WEAPONDEF* const* ids, int nids)
{
    int i, armed = 0;
    for (i = 0; i < TS_UNIT_WEAPONS; i++) d->weaponid[i] = tasim_weapon_lookup(ids, nids, d->weapon[i]);
    for (i = 0; i < 3; i++) armed |= d->weaponid[i] != TS_NONE;
    d->flags241 = (d->flags241 & ~0x10000u) | (armed ? 0x10000u : 0u);
}

void tasim_weapondef_read(TASIM_WEAPONDEF* w, const char* section, TS_GET get, void* ctx, TS_GET damage, void* dctx)
{
    const TS_KEY* k;
    size_t n = strlen(section);
    if (n > sizeof w->section - 1) n = sizeof w->section - 1;
    memset(w->section, 0, sizeof w->section);
    memcpy(w->section, section, n);
    for (k = tasim_keys_weapon; k->name; k++) tasim_def_key(w, k, get(ctx, k->name));
    /* 0x42EF7F..0x42EFA9: [DAMAGE]'s `default` (0x4C46C0, default 0) into the word +0xD4 */
    w->damage = 0;
    if (damage) {
        const char* v = damage(dctx, "default");
        w->damage = (uint16_t)(v ? ts_tdf_atol(v) : 0);
    }
}

int tasim_weapon_fits(const TASIM_WEAPONDEF* w, const char* section)
{
    /* Impure's loader skips an ID outside the array; a name of 0x115 characters or more does not fit the record.
       A name of 32 or more runs into +0x20, which the `name` key then overwrites: the core holds 31, and the
       caller reports a longer one. */
    return w->id >= 0 && w->id < TS_WEAPON_IDS && strlen(section) < 0x115;
}

void tasim_featuredef_read(TASIM_FEATUREDEF* f, const char* section, TS_GET get, void* ctx)
{
    const TS_KEY* k;
    const TS_KEY* const object = tasim_keys_find(tasim_keys_feature, "object");
    const TS_KEY* const gaf_end = tasim_keys_find(tasim_keys_feature, "spreadchance");
    size_t n = strlen(section);
    int has_object = 0;
    if (n > sizeof f->name - 1) n = sizeof f->name - 1;
    memset(f->name, 0, sizeof f->name);
    memcpy(f->name, section, n);
    for (k = tasim_keys_feature; k->name; k++) {
        if (k > object && k < gaf_end && has_object) continue;      /* 0x42261B: a 3DO reads no GAF key */
        if (k == object) {
            has_object = tasim_def_key(f, k, get(ctx, k->name)) == 1;
            f->flags = (uint16_t)((f->flags & ~1u) | (has_object ? 0u : 1u));
            continue;
        }
        tasim_def_key(f, k, get(ctx, k->name));
        if (k->kind == TSK_BIT && k->bit == 11)
            /* 0x422C50..0x422CDA: four names the engine draws under the gray, whatever the key says */
            if (ieq(section, "DragonsTeeth") || ieq(section, "DragonsTeeth_Core") || ieq(section, "Fortification") ||
                ieq(section, "Fortification_Core"))
                f->flags |= 0x800;
    }
}

int tasim_yard_cells(const TASIM_UNITDEF* d, uint8_t* out, int cap)
{
    static const char codes[] = ".CGOYcfowy";
    static const uint8_t bytes[] = { 0x00, 0x35, 0x8F, 0x2B, 0x31, 0x2D, 0x6F, 0x2F, 0x37, 0x29 };
    const char* p = d->yardmap;
    const char* const end = d->yardmap + sizeof d->yardmap;
    int cells, k = 0, last = -1;
    if (d->bmcode != 0 || d->footx <= 0 || d->footz <= 0) return 0;
    cells = (int)d->footx * (int)d->footz;
    if (cells > cap) return 0;
    while (k < cells) {
        const char* c;
        if (p >= end || *p == 0) { out[k++] = (uint8_t)(last >= 0 ? last : 0x2F); continue; }
        c = strchr(codes, *p);
        if (!c) { p++; continue; }
        out[k++] = bytes[c - codes];
        last = bytes[c - codes];
        if (p + 1 < end && p[1] != 0) p++;
    }
    return cells;
}

/* ---- the report --------------------------------------------------------------------------------------------- */

static void copy_text(char* dst, const char* src)
{
    size_t n = src ? strlen(src) : 0;
    if (n > TS_REPORT_TEXT - 1) n = TS_REPORT_TEXT - 1;
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}

void tasim_report_add(TS_REPORT* r, int cls, int landing, const char* kind, const char* what, const char* first)
{
    int i;
    TS_REPORT_ROW* row;
    for (i = 0; i < r->n; i++) {
        row = &r->row[i];
        if (row->cls == cls && !strcmp(row->kind, kind) && ieq(row->what, what)) { row->count++; return; }
    }
    if (r->n >= TS_REPORT_MAX) { r->dropped++; return; }
    row = &r->row[r->n++];
    memset(row, 0, sizeof *row);
    row->cls = (uint8_t)cls;
    row->landing = (uint8_t)landing;
    copy_text(row->kind, kind);
    row->kind[sizeof row->kind - 1] = 0;
    copy_text(row->what, what);
    copy_text(row->first, first);
    row->count = 1;
}

int tasim_unitdef_refusals(const TASIM_UNITDEF* d, TS_REPORT* r)
{
    /* a mobile type (bmcode not 0) that can move and does not fly runs the ground motion 0x43CD20 */
    const int ground = d->bmcode != 0 && (d->flags245 & 0x80u) && !(d->flags241 & 0x800u);
    int n = 0;
    if (ground && d->turnrate == 0) { tasim_report_add(r, TSRP_REFUSED, TSL_T1B, "unit", "turnrate 0", d->unitname); n++; }
    if (ground && ((uint32_t)d->brakerate << 1) == 0) {         /* 0x43D05A: lea eax,[esi+esi], 32 bits */
        tasim_report_add(r, TSRP_REFUSED, TSL_T1B, "unit", "brakerate x 2 is 0", d->unitname);
        n++;
    }
    if (d->footx > 64 || d->footz > 64) {
        tasim_report_add(r, TSRP_REFUSED, TSL_T1A, "unit", "footprint above 64", d->unitname);
        n++;
    }
    if ((d->flags241 & 0x1000u) && d->maxvelocity / 2 == 0) {
        tasim_report_add(r, TSRP_REFUSED, TSL_T1B, "unit", "canhover, maxvelocity / 2 is 0", d->unitname);
        n++;
    }
    if (((d->flags245 >> 20) & 7u) > 5u) {
        tasim_report_add(r, TSRP_REFUSED, TSL_T1C, "unit", "selfdestructcountdown above 5", d->unitname);
        n++;
    }
    return n;
}

const char* tasim_landing_name(int landing)
{
    static const char* const names[TSL_COUNT] = { "T1a", "T1b", "T1c", "T1d", "T2", "T3", "T4", "AI", "outside" };
    return landing >= 0 && landing < TSL_COUNT ? names[landing] : "?";
}
