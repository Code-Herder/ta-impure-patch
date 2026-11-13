/* tasim_keys_test.c -- the self-test of the key table and the number getters (src/tasim_keys.h), driven by
   tests/tasim_keys_test.py, which builds it natively, under the sanitizers and with the DLL's i686 compiler under
   Wine.

     tasim_keys_test            the rules: scout-Jb J7's five values through the three getters, the 32-bit wraps
                                and the 16.16 overflow, every row kind of tasim_def_key with its absent default
                                (a 16.16 default stored unscaled), the move class's defaults and clamps, the unit's
                                derived fields and its yard map; exit 1 on a failure
     tasim_keys_test libc       (the native build) each stdin line, a plain decimal, through ts_tdf_atof against
                                glibc's strtod, which is correctly rounded: every one equal bit for bit
     tasim_keys_test digest     one 64-bit FNV-1a digest of the three getters on every stdin line and on 10^5
                                generated values; the driver requires every build to print the same digest
     tasim_keys_test x87        (the i686 build) each stdin line through the engine's own sequence run on the x87
                                over the toolchain's MSVCRT atof -- fld, fmul 65536.0, fistp QWORD under a chop
                                control word, the low dword -- against ts_tdf_fx; the double against ts_tdf_atof;
                                and MSVCRT's atol against ts_tdf_atol where no 32-bit wrap is in play */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_fixed.h"
#include "tasim_keys.h"
#ifdef _WIN32
#include <windows.h>
#endif

static int fails;

#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                                       printf("\n"); } } while (0)

static uint64_t dbits(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

static uint32_t fbits(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

/* a section of key=value pairs for the readers: `pairs` is "key=value\n..." */
typedef struct { const char* pairs; char buf[256]; } Fake;

static const char* fake_get(void* ctx, const char* key)
{
    Fake* f = (Fake*)ctx;
    const char* p = f->pairs;
    size_t n = strlen(key);
    while (p && *p) {
        const char* eq = strchr(p, '=');
        const char* nl = strchr(p, '\n');
        if (!eq) break;
        if ((size_t)(eq - p) == n) {
            size_t i, same = 1;
            for (i = 0; i < n; i++) {
                char a = p[i], b = key[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) same = 0;
            }
            if (same) {
                size_t len = nl ? (size_t)(nl - eq - 1) : strlen(eq + 1);
                if (len >= sizeof f->buf) len = sizeof f->buf - 1;
                memcpy(f->buf, eq + 1, len);
                f->buf[len] = 0;
                return f->buf;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
}

static const TS_KEY* row(const TS_KEY* t, const char* name)
{
    const TS_KEY* k = tasim_keys_find(t, name);
    if (!k) { ++fails; printf("FAIL: no row %s\n", name); }
    return k;
}

static void getters(void)
{
    /* scout-Jb J7's table: the int, the double's bits, the 16.16 */
    static const struct { const char* s; int32_t i; uint64_t d; int32_t fx; } J7[] = {
        { "1.6",  1, 0x3FF999999999999Aull, 104857 },
        { "0.05", 0, 0x3FA999999999999Aull, 3276 },
        { "-0.5", 0, 0xBFE0000000000000ull, -32768 },
        { "3",    3, 0x4008000000000000ull, 196608 },
        { "abc",  0, 0x0000000000000000ull, 0 },
    };
    size_t i;
    for (i = 0; i < sizeof J7 / sizeof *J7; i++) {
        CHECK(ts_tdf_atol(J7[i].s) == J7[i].i, "atol(%s) = %d", J7[i].s, ts_tdf_atol(J7[i].s));
        CHECK(dbits(ts_tdf_atof(J7[i].s)) == J7[i].d, "atof(%s) bits %016llx", J7[i].s,
              (unsigned long long)dbits(ts_tdf_atof(J7[i].s)));
        CHECK(ts_tdf_fx(J7[i].s) == J7[i].fx, "fx(%s) = %d", J7[i].s, ts_tdf_fx(J7[i].s));
    }
    /* atol: blanks, one sign, the digits; the 32-bit wrap with no overflow test */
    CHECK(ts_tdf_atol(" \t+12abc") == 12, "atol blanks and sign");
    CHECK(ts_tdf_atol("--5") == 0, "a second sign stops the digits");
    CHECK(ts_tdf_atol("") == 0, "atol empty");
    CHECK(ts_tdf_atol("2147483647") == 2147483647, "atol INT32_MAX");
    CHECK(ts_tdf_atol("2147483648") == (-2147483647 - 1), "atol wraps to INT32_MIN");
    CHECK(ts_tdf_atol("-2147483649") == 2147483647, "atol -2^31 - 1 wraps");
    CHECK(ts_tdf_atol("4294967297") == 1, "atol 2^32 + 1 wraps to 1");
    CHECK(ts_tdf_atol("99999999999") == (int32_t)(uint32_t)(99999999999ull & 0xFFFFFFFFull), "atol 11 digits");
    /* atof: grammar and limits */
    CHECK(ts_tdf_atof("  .5") == 0.5 && ts_tdf_atof("5.") == 5.0, "atof point forms");
    CHECK(ts_tdf_atof("1e3") == 1000.0 && ts_tdf_atof("1D3") == 1000.0 && ts_tdf_atof("25e-1") == 2.5, "exponents");
    CHECK(ts_tdf_atof("1e") == 1.0 && ts_tdf_atof("1e+") == 1.0, "an exponent with no digits is no exponent");
    CHECK(ts_tdf_atof(".") == 0.0 && ts_tdf_atof("-") == 0.0 && ts_tdf_atof("e5") == 0.0, "no digits");
    CHECK(dbits(ts_tdf_atof("-0")) == 0x8000000000000000ull, "-0 keeps its sign");
    CHECK(dbits(ts_tdf_atof("1e999")) == 0x7FF0000000000000ull, "overflow is infinity");
    CHECK(dbits(ts_tdf_atof("1e-999")) == 0, "underflow is 0");
    CHECK(dbits(ts_tdf_atof("4.9406564584124654e-324")) == 1, "the least subnormal");
    CHECK(dbits(ts_tdf_atof("2.2250738585072014e-308")) == 0x0010000000000000ull, "the least normal");
    CHECK(dbits(ts_tdf_atof("1.7976931348623157e308")) == 0x7FEFFFFFFFFFFFFFull, "the greatest double");
    CHECK(dbits(ts_tdf_atof("9007199254740993")) == 0x4340000000000000ull, "2^53 + 1 ties to even, down");
    CHECK(dbits(ts_tdf_atof("9007199254740995")) == 0x4340000000000002ull, "2^53 + 3 ties to even, up");
    CHECK(dbits(ts_tdf_atof("9007199254740993.0000000000000000000000000000000000001")) == 0x4340000000000001ull,
          "a tail past the kept digits breaks the tie");
    /* 0x4C4800: truncation toward zero, the low dword of the int64 */
    CHECK(ts_tdf_fx("-1.6") == -104857, "fx toward zero");
    CHECK(ts_tdf_fx("32767.99999") == 2147483647, "fx just below 2^31");
    CHECK(ts_tdf_fx("32768") == (-2147483647 - 1), "fx 32768 wraps to INT32_MIN");
    CHECK(ts_tdf_fx("65536") == 0, "fx 65536: the low dword of 2^32");
    CHECK(ts_tdf_fx("-32768.5") == 2147450880, "fx -32768.5: the low dword of -2147516416");
    CHECK(ts_tdf_fx("1e30") == 0 && ts_tdf_fx("1e999") == 0, "fx past the int64 range: the indefinite's low dword");
}

static void kinds(void)
{
    TASIM_UNITDEF* d = (TASIM_UNITDEF*)calloc(1, sizeof *d);
    TASIM_WEAPONDEF w;
    TASIM_FEATUREDEF* f = (TASIM_FEATUREDEF*)calloc(1, sizeof *f);
    TASIM_MOVECLASS cls[TS_CLASSES], c;
    uint8_t cells[64];
    int i;
    Fake k;
    memset(&w, 0, sizeof w);
    /* TSK_INT into a word, wrapped; TSK_FX present and absent; the default unscaled */
    CHECK(tasim_def_key(d, row(tasim_keys_unit, "turnrate"), "70000") == 1 && d->turnrate == 4464, "a word wraps");
    CHECK(tasim_def_key(d, row(tasim_keys_unit, "maxvelocity"), "1.6") == 1 && d->maxvelocity == 104857, "fx");
    CHECK(tasim_def_key(d, row(tasim_keys_unit, "bankscale"), NULL) == 0 && d->bankscale == 0x10000,
          "an absent 16.16 key keeps its pushed default 0x10000, unscaled");
    CHECK(tasim_def_key(d, row(tasim_keys_unit, "bankscale"), "abc") == 1 && d->bankscale == 0,
          "a present key with no number is 0, never the default");
    CHECK(tasim_def_key(d, row(tasim_keys_unit, "moverate1"), NULL) == 0 && d->moverate1 == 209714,
          "moverate1 defaults to twice maxvelocity");
    d->maxvelocity = 0x40000001;
    tasim_def_key(d, row(tasim_keys_unit, "moverate2"), NULL);
    CHECK(d->moverate2 == (int32_t)0x80000002u, "twice maxvelocity in 32 bits");
    /* TSK_INT_FLT and the cloakcost pair */
    tasim_def_key(d, row(tasim_keys_unit, "buildcostmetal"), "40.28");
    CHECK(d->buildcostmetal == 40.0f, "an int cost stored as a float");
    for (i = 0; tasim_keys_unit[i].name; i++)
        if (!strcmp(tasim_keys_unit[i].name, "cloakcost")) tasim_def_key(d, &tasim_keys_unit[i], "16777217");
    tasim_def_key(d, row(tasim_keys_unit, "cloakcostmoving"), NULL);
    CHECK(d->cloakcost == 16777216.0f && d->cloakcost_read == 16777217 && d->cloakcostmoving == 16777216.0f,
          "cloakcostmoving's default is the int, then rounded to a float as it is stored");
    /* TSK_DBL_FLT, TSK_DBL_FTOL, TSK_DBL_FLT_SC */
    tasim_def_key(d, row(tasim_keys_unit, "energymake"), "0.1");
    CHECK(fbits(d->energymake) == fbits(0.1f), "a double stored as a float");
    tasim_def_key(&w, row(tasim_keys_weapon, "reloadtime"), "1.6");
    CHECK(w.reloadtime == 48, "1.6 s x 30, chopped");
    tasim_def_key(&w, row(tasim_keys_weapon, "reloadtime"), "0.1");
    CHECK(w.reloadtime == 3, "0.1 s x 30 = 3.0000000000000004, chopped");
    tasim_def_key(&w, row(tasim_keys_weapon, "weaponvelocity"), "300");
    CHECK(w.weaponvelocity == 655360, "300 px/s x 65536/30");
    tasim_def_key(&w, row(tasim_keys_weapon, "turnrate"), "20000");
    CHECK(w.turnrate == 666, "x 1/30 chopped");
    tasim_def_key(&w, row(tasim_keys_weapon, "minbarrelangle"), NULL);
    CHECK(w.minbarrelangle == (float)(-11.25 * 0.017453292519943278), "the default angle in the exe's radians");
    tasim_def_key(&w, row(tasim_keys_weapon, "range"), NULL);
    CHECK(w.range == 0x7FFF, "range's default");
    tasim_def_key(&w, row(tasim_keys_weapon, "ID"), NULL);
    CHECK(w.id == -1, "ID's default");
    /* TSK_BIT: bit 0 of the int; TSK_BIT2; TSK_SDCOUNT */
    d->flags241 = 0;
    tasim_def_key(d, row(tasim_keys_unit, "canhover"), "2");
    CHECK(!(d->flags241 & 0x1000u), "canhover=2 is bit 0 clear");
    tasim_def_key(d, row(tasim_keys_unit, "canhover"), "-1");
    CHECK(d->flags241 & 0x1000u, "canhover=-1 sets the bit");
    tasim_def_key(d, row(tasim_keys_unit, "standingfireorder"), NULL);
    CHECK((d->flags241 & 0xCu) == 0x8u, "standingfireorder defaults to 2");
    tasim_def_key(d, row(tasim_keys_unit, "standingmoveorder"), "7");
    CHECK((d->flags241 & 0x3u) == 0x3u, "standingmoveorder takes two bits");
    d->flags245 = 0x00200000u;
    tasim_def_key(d, row(tasim_keys_unit, "selfdestructcountdown"), NULL);
    CHECK(((d->flags245 >> 20) & 7u) == 5u, "selfdestructcountdown 5 when absent");
    tasim_def_key(d, row(tasim_keys_unit, "selfdestructcountdown"), "9");
    CHECK(((d->flags245 >> 20) & 7u) == 1u, "selfdestructcountdown & 7");
    /* TSK_STR: at most width - 1 characters; objectname defaults to the unitname */
    tasim_def_key(d, row(tasim_keys_unit, "unitname"), "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    CHECK(strlen(d->unitname) == 31, "unitname keeps 31 characters");
    tasim_def_key(d, row(tasim_keys_unit, "objectname"), NULL);
    CHECK(!strcmp(d->objectname, d->unitname), "objectname defaults to the unitname");
    CHECK(tasim_def_key(d, row(tasim_keys_unit, "ai_weight"), "1") == -1, "a known row stores nothing");
    CHECK(tasim_keys_find(tasim_keys_unit, "TEDClass") == NULL, "a key no parser reads has no row");
    CHECK(tasim_keys_find(tasim_keys_unit, "MAXVELOCITY") != NULL, "rows match without case");
    /* TSK_INT_U16FLT: the feature's metal through & 0xFFFF */
    tasim_def_key(f, row(tasim_keys_feature, "metal"), "70000");
    CHECK(f->metal == 4464.0f, "feature metal keeps its low 16 bits");

    /* the move class: 0x4402E0's defaults, badslope from maxslope's read, the clamps */
    tasim_moveclass_init(&c);
    k.pairs = "maxslope=300\nmaxwaterslope=20\n";
    tasim_moveclass_read(&c, fake_get, &k);
    CHECK(c.maxwaterdepth == 10000 && c.minwaterdepth == -10000, "class depths' defaults");
    CHECK(c.maxwaterslope == 20 && c.maxslope == 20, "maxslope clamped to maxwaterslope");
    CHECK(c.badslope == 20 && c.badwaterslope == 10, "badslope from the read byte 44 >> 1 = 22, clamped to 20");

    /* a unit through its reader: a named class wins over the unit's own keys */
    for (i = 0; i < TS_CLASSES; i++) tasim_moveclass_init(&cls[i]);
    strcpy(cls[3].name, "TANKSH2");
    cls[3].present = 1;
    cls[3].footx = cls[3].footz = 2;
    cls[3].maxslope = 15;
    memset(d, 0, sizeof *d);
    k.pairs = "unitname=TEST\nmovementclass=tanksh2\nfootprintx=9\nmaxvelocity=1.6\nbmcode=1\ncanmove=1\n"
              "cloakcost=5\ncanreclamate=1\n";
    tasim_unitdef_read(d, fake_get, &k, cls);
    CHECK(d->classidx == 3 && d->footx == 2, "the class's footprint, not the unit's");
    CHECK(d->slopevelocity == 104857 / 16, "+0x196 = maxvelocity / (maxslope + 1)");
    CHECK(d->box[0] == -0x100000 && d->box[2] == 0x100000 && d->box[4] == 0x200000 && d->box[6] == 0x155555,
          "the footprint box");
    CHECK((d->flags245 & 0x200u) && (d->flags245 & 0x2000u), "bit 9 copies bit 10, bit 13 from cloakcost");
    CHECK(d->mincloakdistance == 80, "mincloakdistance 80 for a type that cloaks");
    CHECK(!strcmp(d->objectname, "TEST"), "objectname from the unitname");
    memset(d, 0, sizeof *d);
    k.pairs = "unitname=OWN\nfootprintx=3\nfootprintz=2\nmovementclass=nosuch\nbmcode=0\nYardMap=oc xy\n";
    tasim_unitdef_read(d, fake_get, &k, cls);
    CHECK(d->classidx == TS_NONE && d->footx == 3 && d->footz == 2, "no such class: the unit's own section");
    CHECK(tasim_yard_cells(d, cells, (int)sizeof cells) == 6 && cells[0] == 0x2F && cells[1] == 0x2D &&
          cells[2] == 0x29 && cells[3] == 0x29 && cells[5] == 0x29, "the yard map: the blank skipped, 'x' skipped, "
          "the last 'y' repeated");
    k.pairs = "unitname=OWN\nfootprintx=2\nfootprintz=2\nbmcode=0\nYardMap=q\n";
    tasim_unitdef_read(d, fake_get, &k, cls);
    CHECK(tasim_yard_cells(d, cells, (int)sizeof cells) == 4 && cells[0] == 0x2F && cells[3] == 0x2F,
          "no valid char: every cell 'o'");

    /* the refusals: the ground step's two divisions, and nothing for a building */
    {
        TS_REPORT* r = (TS_REPORT*)calloc(1, sizeof *r);
        memset(d, 0, sizeof *d);
        d->bmcode = 1;
        d->flags245 = 0x80u;
        d->turnrate = 5;
        d->brakerate = (int32_t)0x80000000u;
        d->maxvelocity = 0x10000;
        CHECK(tasim_unitdef_refusals(d, r) == 1, "a brakerate whose double wraps to 0 is refused");
        d->turnrate = 0;
        d->brakerate = 1;
        CHECK(tasim_unitdef_refusals(d, r) == 1 && r->n == 2, "turnrate 0 is refused");
        d->bmcode = 0;
        CHECK(tasim_unitdef_refusals(d, r) == 0, "a building runs no ground step");
        d->flags241 = 0x1000u;
        d->maxvelocity = 1;
        CHECK(tasim_unitdef_refusals(d, r) == 1, "canhover with maxvelocity / 2 = 0");
        free(r);
    }
    free(d);
    free(f);
}

static uint64_t fnv = 0xCBF29CE484222325ull;
static void feed(const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; ++i) fnv = (fnv ^ b[i]) * 0x100000001B3ull;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
}

/* a value as a TDF writes one: a sign, digits, a point, a fraction, an exponent, each sometimes */
static void gen(char* s, size_t cap)
{
    size_t n = 0;
    int i, nd = 1 + (int)(next() % 12), nf = (int)(next() % 9);
    if (next() % 4 == 0) s[n++] = ' ';
    if (next() % 5 == 0) s[n++] = next() % 2 ? '-' : '+';
    for (i = 0; i < nd && n + 4 < cap; i++) s[n++] = (char)('0' + next() % 10);
    if (nf) { s[n++] = '.'; for (i = 0; i < nf && n + 8 < cap; i++) s[n++] = (char)('0' + next() % 10); }
    if (next() % 6 == 0) n += (size_t)snprintf(s + n, cap - n, "e%d", (int)(next() % 61) - 30);
    s[n] = 0;
}

static void digest_one(const char* s)
{
    int32_t i = ts_tdf_atol(s), fx = ts_tdf_fx(s);
    double d = ts_tdf_atof(s);
    feed(&i, 4);
    feed(&d, 8);
    feed(&fx, 4);
}

static void strip(char* line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
}

static int digest(void)
{
    char line[4096];
    long n = 0, k;
    while (fgets(line, sizeof line, stdin)) { strip(line); digest_one(line); n++; }
    for (k = 0; k < 100000; k++) { gen(line, sizeof line); digest_one(line); }
    printf("digest %016llx over %ld lines and %ld generated values\n", (unsigned long long)fnv, n, k);
    return 0;
}

static int libc(void)
{
    char line[4096];
    long n = 0, bad = 0, k;
    while (fgets(line, sizeof line, stdin)) {
        strip(line);
        n++;
        if (dbits(ts_tdf_atof(line)) != dbits(strtod(line, NULL)) && bad++ < 10)
            printf("differs from strtod: \"%s\" %016llx %016llx\n", line, (unsigned long long)dbits(ts_tdf_atof(line)),
                   (unsigned long long)dbits(strtod(line, NULL)));
    }
    for (k = 0; k < 1000000; k++) {
        gen(line, sizeof line);
        if (dbits(ts_tdf_atof(line)) != dbits(strtod(line, NULL)) && bad++ < 10)
            printf("differs from strtod: \"%s\"\n", line);
    }
    printf("libc: %ld of %ld stdin lines and %ld generated values differ from strtod\n", bad, n, k);
    return bad ? 1 : 0;
}

#ifdef _WIN32
typedef double(__cdecl* ATOF)(const char*);
typedef long(__cdecl* ATOL)(const char*);

/* 0x4C4800's tail on the x87: fld the double atof returned, fmul QWORD 65536.0 (0x4C4884, at the 53-bit precision
   the game runs at), then _ftol 0x4E43A0: the control word's rounding set to chop (or ah,0x0C), fistp QWORD, the
   low dword */
static int32_t x87_fx(double d)
{
    static const double k65536 = 65536.0;
    uint16_t old, game = 0x027F, chop = 0x0E7F;
    int64_t q;
    __asm__ __volatile__("fnstcw %0" : "=m"(old));
    __asm__ __volatile__("fldcw %0" : : "m"(game));
    __asm__ __volatile__("fldl %1\n\tfmull %2\n\tfldcw %3\n\tfistpll %0"
                         : "=m"(q) : "m"(d), "m"(k65536), "m"(chop));
    __asm__ __volatile__("fldcw %0" : : "m"(old));
    return (int32_t)(uint32_t)(uint64_t)q;
}

/* the digits of an atol argument fit 31 bits: neither CRT wraps or saturates */
static int no_wrap(const char* s)
{
    int n = 0;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    if (*s == '-' || *s == '+') s++;
    while (*s == '0') s++;
    while (*s >= '0' && *s <= '9') { s++; n++; }
    return n <= 9;
}

static int x87(void)
{
    HMODULE crt = LoadLibraryA("msvcrt.dll");
    ATOF crt_atof = crt ? (ATOF)(void*)GetProcAddress(crt, "atof") : NULL;
    ATOL crt_atol = crt ? (ATOL)(void*)GetProcAddress(crt, "atol") : NULL;
    char line[4096];
    long n = 0, bfx = 0, bd = 0, bi = 0, ni = 0;
    if (!crt_atof || !crt_atol) { printf("x87: no MSVCRT atof/atol\n"); return 2; }
    while (fgets(line, sizeof line, stdin)) {
        double ref;
        strip(line);
        n++;
        ref = crt_atof(line);
        if (x87_fx(ref) != ts_tdf_fx(line) && bfx++ < 10)
            printf("16.16 differs: \"%s\" x87 %d core %d\n", line, x87_fx(ref), ts_tdf_fx(line));
        if (dbits(ref) != dbits(ts_tdf_atof(line)) && bd++ < 10)
            printf("double differs: \"%s\" msvcrt %016llx core %016llx\n", line, (unsigned long long)dbits(ref),
                   (unsigned long long)dbits(ts_tdf_atof(line)));
        if (no_wrap(line)) {
            ni++;
            if ((int32_t)crt_atol(line) != ts_tdf_atol(line) && bi++ < 10)
                printf("int differs: \"%s\" msvcrt %ld core %d\n", line, crt_atol(line), ts_tdf_atol(line));
        }
    }
    printf("x87: %ld lines: 16.16 %ld differ, double %ld differ, int %ld of %ld differ\n", n, bfx, bd, bi, ni);
    return bfx || bd || bi ? 1 : 0;
}
#endif

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "digest")) return digest();
    if (argc > 1 && !strcmp(argv[1], "libc")) return libc();
#ifdef _WIN32
    if (argc > 1 && !strcmp(argv[1], "x87")) return x87();
#endif
    if (argc > 1) { printf("unknown mode %s\n", argv[1]); return 2; }
    getters();
    kinds();
    printf("%s: %d failures\n", fails ? "FAILED" : "ok", fails);
    return fails ? 1 : 0;
}
