/* tasim_load -- the install loader (tasim_load.h). */
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_buildmenu.h"
#include "tagpu_guifile.h"
#include "tagpu_hpi.h"
#include "tagpu_tdf.h"
#include "tagpu_vfs.h"
#include "tasim_fixed.h"
#include "tasim_load.h"

#define FILE_CAP        (4u << 20)      /* a text definition read whole                        */
#define SIDE_RECORDS    5               /* the engine's side records: 0x431A20's free loop walks five (0x431A4A..0x431A56) */
#define LISTLEN         (2 * TASIM_PROFILE_NAME)   /* a listing's filter: a profile pattern and "." + an extension */
#define PROFILE_LINE    512             /* past every key's bound with its value's                */
#define COPYRIGHT_YEAR_MAX 0x7B         /* the year's offset in the reference: 4 bytes end before the 0x80th */

/* A download entry the engine writes (each_download): its builder's row, whether it lies in a record the download fix
   appends (its file's section 5 on), and MENU, BUTTON and UNITNAME. */
typedef struct { int builder, cont; TAGPU_BM_ENTRY e; } Entry;

/* The load's working state: the install being filled, and what only the load reads. */
typedef struct {
    TASIM_INSTALL*       L;
    const TASIM_PROFILE* p;
    char                 unit_list[LISTLEN];        /* the unit listing's filter, <all_pattern> built with <unit_ext> */
    char                 download_list[LISTLEN];    /* the download listing's, with <tdf_ext>                         */
    char                 weapon_dir[TASIM_PROFILE_NAME], weapon_filter[TASIM_PROFILE_NAME];  /* <weapon_pattern> */
    FILE*                values;        /* tasim_load_values: every value, one a line          */
    Entry*               ent;           /* the download entries, in the engine's file order    */
    int                  nent, capent;
    char                 prefix[SIDE_RECORDS][4];   /* each side's nameprefix (read_sides)                */
    int                  nsides;                    /* min(the sides sidedata defines, SIDE_RECORDS)      */
    int                  err;
} Load;

typedef struct { const TAGPU_TDF* t; int s; } Sec;

static int ieq(const char* a, const char* b) { return tagpu_hpi_stricmp(a, b, (size_t)-1) == 0; }

/* the file name of a found or listed path: what follows its last '\' */
static const char* base_name(const char* path)
{
    const char* b = strrchr(path, '\\');
    return b ? b + 1 : path;
}

/* 0x4290F0's name rule applied to a listing's pattern (`*` and "FBI" give "*.FBI"): cut at its last '.' (0x4BB0F0),
   then "." and the extension. The language's folder is never taken for a pattern [INFERRED: 0x4BB5B0 opens no file
   named by a wildcard; the type list's equality with the engine's on five setups, loader spec F1, measures the
   outcome]. 0 when it does not fit. */
static int built_name(char* out, size_t cap, const char* pattern, const char* ext)
{
    char* dot;
    if ((size_t)snprintf(out, cap, "%s", pattern) >= cap) return 0;
    if ((dot = strrchr(out, '.')) != NULL) *dot = 0;
    if (strlen(out) + 1 + strlen(ext) >= cap) return 0;
    strcat(out, ".");
    strcat(out, ext);
    return 1;
}

static void copy_name(char* dst, const char* src, size_t cap)
{
    size_t k = 0;
    if (src) for (; src[k] && k + 1 < cap; k++) dst[k] = src[k];
    dst[k] = 0;
}

int tasim_load_has_word(const char* text, const char* word)
{
    size_t n = strlen(word);
    const char* p = text;
    if (!text) return 0;
    for (;;) {
        size_t k = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) return 0;
        while (p[k] && !isspace((unsigned char)p[k])) k++;
        if (k == n && !tagpu_hpi_stricmp(word, p, n)) return 1;
        p += k;
    }
}

static const char* sec_get(void* ctx, const char* key)
{
    const Sec* c = (const Sec*)ctx;
    return tagpu_tdf_get(c->t, c->s, key);
}

/* 0x4B6BA0 over a file's bytes (engine/network.md, *The key*): four 8-bit lanes with i the offset's low byte and b the
   byte, xor(i + b) << 24 | sum(i ^ b) << 16 | xor(b) << 8 | sum(b). */
static uint32_t file_key(const unsigned char* b, uint32_t n)
{
    uint8_t s = 0, x = 0, s2 = 0, x2 = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        s = (uint8_t)(s + b[i]);
        x ^= b[i];
        s2 = (uint8_t)(s2 + ((i & 255u) ^ b[i]));
        x2 ^= (uint8_t)(i + b[i]);
    }
    return (uint32_t)x2 << 24 | (uint32_t)s2 << 16 | (uint32_t)x << 8 | s;
}

/* The engine's text before its parse 0x4C2F60, which tagpu_tdf's own rules (tools/tascene's) do not give: the text
   ends at its first NUL (0x4C30A8), and 0x4C33A0 blanks the comments in one pass from the start, `//` up to the next
   LF and `/` `*` through the first `*` `/` after it (unterminated, to the end but the last byte), so a `//` hides
   an opener later on its line. tagpu_tdf strips block comments before line comments, and an opener inside a `//`
   line then swallows every section up to the next closer: Escalation's weapon TDFs mark their sections with banner
   lines of `//` and a run of stars, and 84 of their 240 sections were read (MEASURED 2026-11-13). With every comment
   blanked here, tagpu_tdf meets none. The rule is tools/wiki/tavfs.py's _strip_comments (DISASSEMBLED there).
   Returns the text's length. */
static uint32_t engine_text(unsigned char* t, uint32_t n)
{
    const unsigned char* nul = (const unsigned char*)memchr(t, 0, n);
    uint32_t i = 0;
    if (nul) n = (uint32_t)(nul - t);
    while (i < n) {
        if (t[i] == '/' && i + 1 < n && t[i + 1] == '/') {
            while (i < n && t[i] != '\n') t[i++] = ' ';
        } else if (t[i] == '/' && i + 1 < n && t[i + 1] == '*') {
            t[i] = t[i + 1] = ' ';
            /* the closer's test looks at the byte before i, which the opener's own star, blanked, never is */
            for (i += 2; i < n; i++) {
                if (t[i - 1] == '*' && t[i] == '/') {
                    t[i - 1] = t[i] = ' ';
                    i++;
                    break;
                }
                t[i - 1] = ' ';
            }
        } else {
            i++;
        }
    }
    return n;
}

/* The parse of `f` as the engine's text (engine_text), and the key of its bytes into `key` when that is not NULL. */
static TAGPU_TDF* load_tdf_key(TAGPU_VFS* v, const TAGPU_VFILE* f, uint32_t* key)
{
    char why[160];
    unsigned char* b = tagpu_vfs_load(v, f, FILE_CAP, why, sizeof why);
    TAGPU_TDF* t;
    if (!b) { fprintf(stderr, "tasim_load: %s: %s\n", f->path, why); return NULL; }
    if (key) *key = file_key(b, f->size);
    t = tagpu_tdf_parse((const char*)b, engine_text(b, f->size));
    free(b);
    if (!t) fprintf(stderr, "tasim_load: %s: past the parser's bounds\n", f->path);
    return t;
}

static TAGPU_TDF* load_tdf(TAGPU_VFS* v, const TAGPU_VFILE* f)
{
    return load_tdf_key(v, f, NULL);
}

/* 0x4290F0's file `dir`\`name`.`ext`, the language's folder first, into `f` and parsed; NULL when neither opens. */
static TAGPU_TDF* load_built(Load* D, const char* dir, const char* name, const char* ext, TAGPU_VFILE* f)
{
    return tagpu_vfs_find_built(D->L->vfs, dir, name, ext, f) ? load_tdf(D->L->vfs, f) : NULL;
}

static void dump_values(FILE* out, const TAGPU_TDF* t, int node)
{
    const char *key, *val;
    int k, s;
    for (k = tagpu_tdf_next_key(t, node, -1, &key, &val); k >= 0; k = tagpu_tdf_next_key(t, node, k, &key, &val))
        if (!strpbrk(val, "\r\n")) fprintf(out, "%s\n", val);
    for (s = tagpu_tdf_next_section(t, node, -1); s >= 0; s = tagpu_tdf_next_section(t, node, s))
        dump_values(out, t, s);
}

static int grow(void** p, int* cap, int need, size_t size)
{
    void* grown;
    int n;
    if (need <= *cap) return 1;
    n = *cap ? *cap : 256;
    while (n < need) n *= 2;
    if (!(grown = realloc(*p, (size_t)n * size))) return 0;
    *p = grown;
    *cap = n;
    return 1;
}

/* ---- the definitions ---------------------------------------------------------------------------------------- */

/* The report's first two classes for one section: a key no row of `table` (or `also`) has is one the engine ignores;
   a key whose row a later landing models is named with that landing. */
static void scan_keys(Load* D, const TAGPU_TDF* t, int s, const TS_KEY* table, const TS_KEY* also,
                      const char* kind, const char* def, const char* file)
{
    const char *key, *val;
    int k;
    for (k = tagpu_tdf_next_key(t, s, -1, &key, &val); k >= 0; k = tagpu_tdf_next_key(t, s, k, &key, &val)) {
        const TS_KEY* row = tasim_keys_find(table, key);
        if (!row && also) row = tasim_keys_find(also, key);
        if (!row) tasim_report_add(D->L->rep, TSRP_IGNORED, TSL_OUT, kind, key, file);
        else if (row->landing >= TSL_T2) tasim_report_add(D->L->rep, TSRP_LATER, row->landing, kind, row->name, def);
    }
}

static void load_classes(Load* D)
{
    TASIM_INSTALL* L = D->L;
    TAGPU_VFILE f;
    /* 0x42D2F3..0x42D307: 0x4290F0(<gamedata>, <moveinfo>, <tdf_ext>) */
    TAGPU_TDF* t = load_built(D, D->p->gamedata, D->p->moveinfo, D->p->tdf_ext, &f);
    const char* file = f.path;
    int i;
    for (i = 0; i < TS_CLASSES; i++) tasim_moveclass_init(&L->cls[i]);
    if (!t) return;
    if (D->values) dump_values(D->values, t, 0);
    /* 0x42D328..0x42D3AA: CLASS%d for i = 0..31, `name` copied, 0x440340 over the record */
    for (i = 0; i < TS_CLASSES && !D->values; i++) {
        char sec[16];
        Sec c;
        const char *name, *key, *val;
        int k;
        snprintf(sec, sizeof sec, "CLASS%d", i);
        if ((c.s = tagpu_tdf_section(t, 0, sec)) < 0) continue;
        c.t = t;
        name = tagpu_tdf_get(t, c.s, "name");
        snprintf(L->cls[i].name, sizeof L->cls[i].name, "%s", name ? name : "");
        L->cls[i].present = 1;
        tasim_moveclass_read(&L->cls[i], sec_get, &c);
        for (k = tagpu_tdf_next_key(t, c.s, -1, &key, &val); k >= 0; k = tagpu_tdf_next_key(t, c.s, k, &key, &val))
            if (!tasim_keys_find(tasim_keys_class, key) && tagpu_hpi_stricmp(key, "name", (size_t)-1))
                tasim_report_add(L->rep, TSRP_IGNORED, TSL_OUT, "class", key, file);
    }
    tagpu_tdf_free(t);
}

/* The menu-time pre-scan 0x42A8D0's verdict on one FBI, def +0x241 bit 23 (DISASSEMBLED 0x42B01F..0x42B162): a
   type without it is dropped by the pre-scan's tail (0x42B248..0x42B2E8) before the def array is sized, and by
   the level's unit-data load 0x42D2E0 (0x42D493..0x42D510) before it sorts. Three tests, in this order, each against
   the running image's constants, which the profile carries:
   - VERSION: `Version` through 0x4C4760 (default 0.0) is V; major = _ftol(floor(V)), minor =
     _ftol(floor((V - major) * 10.0)), each floor the CRT's 0x4E5050 and the product stored as a double
     (0x42B01F..0x42B05E), so "3.3" has minor 2. Kept when major < main+1, or major == main+1 and minor <= main+2
     (0x42B066..0x42B0A7, signed bytes). WinMain writes them with `mov byte` immediates at 0x49E9BD and 0x49E9C6,
     which a Patch Loader's MultiplayerVersionMajor/Minor rewrites (Total Mayhem 11.3, ProTA 4.8) and a mod's own exe
     carries (Escalation 10.2, TA Twilight 2.0): `version_major`, `version_minor`. [INFERRED: the game's control word
     keeps 53-bit precision, so the x87's difference and product round as SSE2's.]
   - ARCHIVE: dropped when 0x4BB650 says the file did not come from an archive and 0x50289C is set
     (0x42B0AD..0x42B0DC, read through 0x41D8A0): `archive_only`. The same test also drops every type while 0x511DE4
     is set, the CD search's failure flag, which the Steam install never sets (engine/recorder.md, 0x51E6C4).
   - COPYRIGHT: `Copyright` through 0x4C48C0 into 0x80 bytes (strncpy, the last byte zeroed; absent, the default at
     0x5037BC, `copyright_default`), the year template 0x50390C (`copyright_year`) copied over it at the offset strstr
     finds the template in the reference 0x5038CC (`copyright`, 0x42AAB7..0x42AB20), then compared with the
     reference byte for byte (0x42B0E2..0x42B162). BOUND: tasim_profile_read keeps that offset at most 0x7B, so the
     four bytes end inside the buffer.
   Later writers of the bit -- the restriction store 0x46E160 and 0x431740 -- are named in tasim.md. */
static int prescan_dropped(TASIM_INSTALL* L, const TASIM_PROFILE* p, const TAGPU_VFILE* f, const Sec* c)
{
    const char* v = tagpu_tdf_get(c->t, c->s, "Version");
    const double version = v ? ts_tdf_atof(v) : 0.0;
    const int32_t major = ts_ftol(floor(version));
    const int32_t minor = ts_ftol(floor((version - (double)major) * 10.0));
    const size_t year_at = (size_t)(strstr(p->copyright, p->copyright_year) - p->copyright);
    char copyright[TASIM_PROFILE_TEXT];
    if (major > p->version_major || (major == p->version_major && minor > p->version_minor)) {
        L->dropped_version++;
        return 1;
    }
    if (p->archive_only && f->src < 0) {
        L->dropped_loose++;
        return 1;
    }
    v = tagpu_tdf_get(c->t, c->s, "Copyright");
    strncpy(copyright, v ? v : p->copyright_default, sizeof copyright);
    copyright[sizeof copyright - 1] = 0;
    memcpy(copyright + year_at, p->copyright_year, 4);
    if (strcmp(copyright, p->copyright)) {
        L->dropped_copyright++;
        return 1;
    }
    return 0;
}

/* The keys only the pre-scan reads (engine/keys.md, *Read only by the pre-scan*): 0x42BF40 leaves them as the
   pre-scan stored them, so a type keeps its pre-scan file's. */
static int prescan_key(const char* key)
{
    return ieq(key, "side") || ieq(key, "ai_weight") || ieq(key, "ai_limit") || ieq(key, "wacky");
}

/* A type's keys: the pre-scan's own from the file it read, every other from the file the level defines it by. */
typedef struct { Sec pre, def; } Route;

static const char* route_get(void* ctx, const char* key)
{
    Route* r = (Route*)ctx;
    return sec_get(prescan_key(key) ? &r->pre : &r->def, key);
}

static int same_file(const TAGPU_VFILE* a, const TAGPU_VFILE* b)
{
    if (a->src != b->src) return 0;
    return a->src >= 0 ? a->entry == b->entry : ieq(a->path, b->path);
}

/* One file of the unit listing (0x4BCA30 over <units>\<all_pattern>.<unit_ext>, 0x42AA17..0x42AA4F). A type is read
   from two files, as the engine reads it:
   - THE PRE-SCAN'S. 0x42A8D0 opens 0x4290F0(<units>, the listed name, <unit_ext>) (0x42AB3E..0x42AB6E) and reads it
     whole (0x4BB5B0, 0x4BB7C0): its verdict (prescan_dropped), its key at def +0x13E (0x4B6BA0, 0x42ABB3, unless a
     .OVR of the same name replaces it, 0x42ABDC: the .OVR's shape is [INFERRED], so the key is then left null) and
     the keys only it reads (prescan_key), `side` among them.
   - THE LEVEL'S. 0x42D2E0 re-reads each kept type from 0x4290F0(<units>, its UnitName, <unit_ext>) (0x42D6E9..
     0x42D709) and calls 0x42BF40 over it only when 0x4BBC40 sizes it above 0 (0x42D713, 0x42D722): that file defines
     every other key. MEASURED 2026-11-13: Total Mayhem 11.3.0's units\tronmakenuke.fbi names EMPMAKENUKE, so the
     engine holds two EMPMAKENUKE types, both defined by EMPMAKENUKE.FBI (ARM's EMP missile), one of them keeping
     tronmakenuke.fbi's CORE side and its key; no other FBI of the six setups is named other than its UnitName.
   A kept type whose own file does not open keeps the pre-scan's record in the engine, which this loader does not
   model, so the load is refused, naming it. */
static int each_unit(void* ctx, const TAGPU_VFILE* listed)
{
    Load* D = (Load*)ctx;
    TASIM_INSTALL* L = D->L;
    const TASIM_PROFILE* p = D->p;
    TAGPU_VFILE g1, g2, ovr;
    TAGPU_TDF *t1, *t2 = NULL;
    Route r;
    const char* name;
    uint32_t key = 0;
    int defined;
    if (!tagpu_vfs_find_built(L->vfs, p->units, base_name(listed->path), p->unit_ext, &g1)) return 1;
    if (!(t1 = load_tdf_key(L->vfs, &g1, &key))) return 1;
    if (D->values) {
        dump_values(D->values, t1, 0);
        tagpu_tdf_free(t1);
        return 1;
    }
    r.pre.t = t1;
    if ((r.pre.s = tagpu_tdf_section(t1, 0, "UNITINFO")) < 0 || !(name = tagpu_tdf_get(t1, r.pre.s, "unitname")) ||
        prescan_dropped(L, p, &g1, &r.pre)) {
        tagpu_tdf_free(t1);
        return 1;
    }
    if (L->nu >= TASIM_LOAD_UNITS) {
        fprintf(stderr, "tasim_load: more than %d units\n", TASIM_LOAD_UNITS);
        D->err = 1;
        tagpu_tdf_free(t1);
        return 0;
    }
    defined = tagpu_vfs_find_built(L->vfs, p->units, name, p->unit_ext, &g2) && g2.size > 0;
    if (defined && same_file(&g1, &g2)) {
        r.def = r.pre;
    } else if (defined && (t2 = load_tdf(L->vfs, &g2)) != NULL &&
               (r.def.s = tagpu_tdf_section(t2, 0, "UNITINFO")) >= 0) {
        r.def.t = t2;
        L->reread++;
    } else {
        fprintf(stderr, "tasim_load: type %s: %s\\%s.%s does not open: the engine keeps the menu-time record "
                        "(0x42D713 skips 0x42BF40), which the loader does not model\n",
                name, p->units, name, p->unit_ext);
        D->err = 1;
        if (t2) tagpu_tdf_free(t2);
        tagpu_tdf_free(t1);
        return 0;
    }
    memset(&L->u[L->nu], 0, sizeof L->u[L->nu]);
    tasim_unitdef_read(&L->u[L->nu], route_get, &r, L->cls);
    scan_keys(D, r.def.t, r.def.s, tasim_keys_unit, tasim_keys_class, "unit", L->u[L->nu].unitname, g2.path);
    L->key[L->nu] = key;
    L->keyed[L->nu] = !(tagpu_vfs_find_built(L->vfs, p->units, base_name(listed->path), "OVR", &ovr) && ovr.size);
    L->nu++;
    if (t2) tagpu_tdf_free(t2);
    tagpu_tdf_free(t1);
    return 1;
}

/* The [DAMAGE] rows of weapon section w (its index in L->w): every key but `default`, each once as tagpu_tdf answers
   it. A name of 32 characters or more can equal no unitname (def +0x20 holds 31) and is counted but not kept. */
static void damage_rows(Load* D, int wi, const TAGPU_TDF* t, int dmg)
{
    TASIM_INSTALL* L = D->L;
    TASIM_WEAPONDEF* w = &L->w[wi];
    const char *key, *val;
    int k;
    L->dmg_first[wi] = L->ndmg;
    L->dmg_count[wi] = 0;
    if (dmg < 0) return;
    for (k = tagpu_tdf_next_key(t, dmg, -1, &key, &val); k >= 0; k = tagpu_tdf_next_key(t, dmg, k, &key, &val)) {
        TASIM_LOAD_DMG* r;
        if (!tagpu_hpi_stricmp(key, "default", (size_t)-1)) continue;
        w->damage_rows++;
        if (strlen(key) >= TASIM_LOAD_DMGNAME) continue;
        if (!grow((void**)&L->dmg, &L->capdmg, L->ndmg + 1, sizeof *L->dmg)) { D->err = 1; return; }
        r = &L->dmg[L->ndmg++];
        copy_name(r->name, key, sizeof r->name);
        r->damage = ts_tdf_atol(val);
        L->dmg_count[wi]++;
    }
}

/* One file of the weapon listing: 0x4BCA30 lists <weapon_pattern> and 0x4290F0(<weapons>, the listed name, <tdf_ext>)
   opens each (the pre-scan 0x42A8EC/0x42A98B, the level's load 0x42E350/0x42E39B, DISASSEMBLED). The listing's folder
   and the open's are two strings, which Total Mayhem's and ProTA's loader lists both rename; a listed name the open's
   folder does not hold is no file. */
static int each_weapons_file(void* ctx, const TAGPU_VFILE* listed)
{
    Load* D = (Load*)ctx;
    TASIM_INSTALL* L = D->L;
    TAGPU_VFILE file;
    const TAGPU_VFILE* f = &file;
    TAGPU_TDF* t;
    int s;
    if (!(t = load_built(D, D->p->weapons, base_name(listed->path), D->p->tdf_ext, &file))) return 1;
    if (D->values) dump_values(D->values, t, 0);
    /* each section in the file's order [INFERRED: the order 0x4C3490 hands them to 0x42E440] */
    for (s = tagpu_tdf_next_section(t, 0, -1); s >= 0 && !D->values; s = tagpu_tdf_next_section(t, 0, s)) {
        const char* name = tagpu_tdf_section_name(t, s);
        TASIM_WEAPONDEF* w;
        Sec c, dmg;
        int sub;
        if (L->nw >= TASIM_LOAD_WSECTIONS) { D->err = 1; break; }
        w = &L->w[L->nw];
        memset(w, 0, sizeof *w);
        c.t = dmg.t = t;
        c.s = s;
        dmg.s = tagpu_tdf_section(t, s, "DAMAGE");
        tasim_weapondef_read(w, name, sec_get, &c, dmg.s >= 0 ? sec_get : NULL, &dmg);
        /* Impure's floor before the loader's closing call 0x49E010 (0x42F314, 0x42F32E; tagpu_patches.c,
           wpn_velocity_floor): a ballistic weapon (+0x111 bit 1) whose stored +0x68 is 0 gets 1, since the ballistic
           fire divides by it (0x49CE6A). TA Zero's Gasbag, ballistic with no weaponvelocity, holds 1 in the engine
           (MEASURED 2026-11-13, defcheck --setup). */
        if ((w->flags & 0x2u) && w->weaponvelocity == 0) w->weaponvelocity = 1;
        damage_rows(D, L->nw, t, dmg.s);
        if (w->damage_rows) tasim_report_add(L->rep, TSRP_LATER, TSL_T2, "weapon", "[DAMAGE] per type", name);
        scan_keys(D, t, s, tasim_keys_weapon, NULL, "weapon", name, f->path);
        for (sub = tagpu_tdf_next_section(t, s, -1); sub >= 0; sub = tagpu_tdf_next_section(t, s, sub))
            if (sub != dmg.s)
                tasim_report_add(L->rep, TSRP_IGNORED, TSL_OUT, "weapon", tagpu_tdf_section_name(t, sub), f->path);
        if (!tasim_weapon_fits(w, name) || strlen(name) >= sizeof w->section) {
            if (!tasim_weapon_fits(w, name)) L->refused++;
            else tasim_report_add(L->rep, TSRP_REFUSED, TSL_T2, "weapon", "section name of 32 or more", name);
            L->ndmg = L->dmg_first[L->nw];          /* the section's rows go with it */
            continue;
        }
        if (L->ids[w->id]) L->replaced++;
        L->ids[w->id] = w;
        L->nw++;
    }
    tagpu_tdf_free(t);
    return 1;
}

static int each_feature_file(void* ctx, const TAGPU_VFILE* f)
{
    Load* D = (Load*)ctx;
    TASIM_INSTALL* L = D->L;
    TAGPU_TDF* t;
    int s, i;
    if (!(t = load_tdf(L->vfs, f))) return 1;
    if (D->values) dump_values(D->values, t, 0);
    for (s = tagpu_tdf_next_section(t, 0, -1); s >= 0 && !D->values; s = tagpu_tdf_next_section(t, 0, s)) {
        const char* name = tagpu_tdf_section_name(t, s);
        Sec c;
        /* LoadFeature takes a name from the first file enumerated that holds it (engine/map.md) */
        for (i = 0; i < L->nf; i++) if (ieq(L->f[i].name, name)) break;
        if (i < L->nf) continue;
        if (L->nf >= TASIM_LOAD_FEATURES) { D->err = 1; break; }
        c.t = t;
        c.s = s;
        memset(&L->f[L->nf], 0, sizeof L->f[L->nf]);
        tasim_featuredef_read(&L->f[L->nf], name, sec_get, &c);
        scan_keys(D, t, s, tasim_keys_feature, NULL, "feature", name, f->path);
        L->nf++;
    }
    tagpu_tdf_free(t);
    return 1;
}

/* A kept type in load order: its record, and the key that goes with it. */
typedef struct { const TASIM_UNITDEF* d; uint32_t key; uint8_t keyed; } Row;

static int by_unitname(const void* a, const void* b)
{
    return tagpu_hpi_stricmp(((const Row*)a)->d->unitname, ((const Row*)b)->d->unitname, (size_t)-1);
}

/* The def array's order: 0x42D2E0 sorts types [1, count) by _stricmp of the unitname (0x42DB60) and leaves None as
   type 0, so row i is type i + 1. Two types sharing a name keep the CRT qsort's tie order, which this qsort's need not
   be. 0 when memory runs out. */
static int sort_units(TASIM_INSTALL* L)
{
    TASIM_UNITDEF* was = (TASIM_UNITDEF*)malloc((size_t)L->nu * sizeof *was);
    Row* row = (Row*)malloc((size_t)L->nu * sizeof *row);
    int i;
    if (!was || !row) {
        free(was);
        free(row);
        return 0;
    }
    memcpy(was, L->u, (size_t)L->nu * sizeof *was);
    for (i = 0; i < L->nu; i++) {
        row[i].d = &was[i];
        row[i].key = L->key[i];
        row[i].keyed = L->keyed[i];
    }
    qsort(row, (size_t)L->nu, sizeof *row, by_unitname);
    for (i = 0; i < L->nu; i++) {
        L->u[i] = *row[i].d;
        L->key[i] = row[i].key;
        L->keyed[i] = row[i].keyed;
    }
    free(was);
    free(row);
    return 1;
}

/* ---- the build options -------------------------------------------------------------------------------------- */

static int unit_row(const TASIM_INSTALL* L, const char* name)
{
    int i;
    for (i = 0; i < L->nu; i++) if (ieq(L->u[i].unitname, name)) return i;
    return -1;
}

/* `f` parsed by position as the engine's parser 0x4C2F60 builds it (tagpu_guifile): NULL for a 0-byte file, which
   0x4C2F60 fails (0x4C2F9A), and, said on stderr, for one that does not read or that the parser refuses (the engine
   exits on it, 0x4B6290). */
static TAGPU_GUIFILE* load_positional(TAGPU_VFS* v, const TAGPU_VFILE* f)
{
    char why[160];
    unsigned char* b;
    TAGPU_GUIFILE* g;
    if (!f->size) return NULL;
    if (!(b = tagpu_vfs_load(v, f, FILE_CAP, why, sizeof why))) {
        fprintf(stderr, "tasim_load: %s: %s\n", f->path, why);
        return NULL;
    }
    if (!(g = tagpu_guifile_parse(b, f->size, why, sizeof why)))
        fprintf(stderr, "tasim_load: %s: %s\n", f->path, why);
    free(b);
    return g;
}

/* The type 0x42DE3C..0x42DE7D gives a download section's UNITMENU: the first def, from type 0 up, whose name +0x20
   equals it by _stricmp. -1 for type 0 `None`, -2 for none, else the row. */
static int builder_row(const TASIM_INSTALL* L, const char* unitmenu)
{
    int i;
    if (ieq(unitmenu, "None")) return -1;
    return (i = unit_row(L, unitmenu)) < 0 ? -2 : i;
}

/* THE DOWNLOAD RECORDS (DISASSEMBLED 0x42DCF0..0x42E0C1, the download loader, called at 0x4918CF right after the
   unit-data load 0x42D2E0, so over the level's sorted types; research/notes/engine/ui.md, *The download menus'
   records*):
     - each listed file has a record of 0xBD bytes: a count, then entries of 0x25 bytes (the builder's type, a word at
       +4; MENU +6; BUTTON +7; UNITNAME, 0x20 bytes, at +8);
     - entry k comes from the file's k-th top-level section in file order (0x4C3490(k), whatever the section's name),
       and is written only when the section's UNITMENU (0x4C48C0 into 0x20 bytes, so cut at 31 characters) names a
       def (builder_row); the builder's type, MENU and BUTTON (each 0x4C46C0 with default 0, stored as a byte) and
       UNITNAME ("" when absent) are then written (0x42DE89..0x42DF02);
     - then every def whose name equals, by _stricmp, a file's record's first UNITNAME (record +8) gets 0x20 ORed into
       +0x241, the downloadable flag (0x42E031..0x42E0BF).
   Impure's build (tagpu_patches.c, fix_download_records) zeroes the block, gives each file its own record at
   file x 0xBD for its sections 0..4, puts its sections from 5 on into records appended at the block's end as the files
   are read, and bounds the check to the files' own records. So the first section in file order alone flags a type, an
   entry left unwritten names "" and builder 0, which no builder is, and a reader walking the records meets every
   file's sections 0..4, in file order, before any file's fifth (Entry.cont). A file that does not open writes no
   entry, nor does a 0-byte one (Twilight blanks over 300 of its download files so), nor one the parser refuses.
   The sections are read by position, never probed by a name such as MENUENTRY<n>: TA Zero's files number theirs
   from MenuEntry0, which a probe from MENUENTRY1 misses.
   MEASURED 2026-11-13 (defcheck --setup): Escalation's 34, Twilight's 3 and TA Zero's 116 types flagged by the check
   alone are exactly the rule's. Of Escalation's 169 files, 90 open with `UNITMENU=BLANK`, which names no type, and 42
   name a type whose FBI sets `downloadable`. */
static int each_download(void* ctx, const TAGPU_VFILE* listed)
{
    Load* D = (Load*)ctx;
    TASIM_INSTALL* L = D->L;
    TAGPU_VFILE f;
    TAGPU_GUIFILE* g = NULL;
    char first[32] = "";
    int i, k, n;
    /* 0x4BCA30(<download>\<all_pattern>.<tdf_ext>) lists it (0x42DCFE..0x42DD34), 0x4290F0(<download>, the listed
       name, <tdf_ext>) opens it (0x42DDA3..0x42DDB6) */
    if (tagpu_vfs_find_built(L->vfs, D->p->download, base_name(listed->path), D->p->tdf_ext, &f))
        g = load_positional(L->vfs, &f);
    n = g ? tagpu_guifile_count(g, 0) : 0;
    for (k = 0; k < n && !D->err; k++) {
        const int s = tagpu_guifile_child(g, 0, k);
        const char* menu = tagpu_guifile_get(g, s, "unitmenu");
        char builder[32];
        uint32_t bits;
        Entry* e;
        int row;
        copy_name(builder, menu, sizeof builder);
        if (!menu || (row = builder_row(L, builder)) == -2) continue;
        if (k == 0) copy_name(first, tagpu_guifile_get(g, s, "unitname"), sizeof first);
        if (row < 0) continue;                                  /* builder 0, None, which no builder is */
        if (!grow((void**)&D->ent, &D->capent, D->nent + 1, sizeof *D->ent)) { D->err = 1; break; }
        e = &D->ent[D->nent++];
        memset(e, 0, sizeof *e);
        e->builder = row;
        e->cont = k >= 5;
        tagpu_guifile_num(tagpu_guifile_get(g, s, "menu"), NULL, &bits);
        e->e.menu = (unsigned char)bits;
        tagpu_guifile_num(tagpu_guifile_get(g, s, "button"), NULL, &bits);
        e->e.button = (unsigned char)bits;
        copy_name(e->e.name, tagpu_guifile_get(g, s, "unitname"), sizeof e->e.name);
    }
    for (i = 0; i < L->nu; i++)
        if (ieq(L->u[i].unitname, first)) L->u[i].flags241 |= 0x20u;
    if (g) tagpu_guifile_free(g);
    return 1;
}

/* THE SIDES (DISASSEMBLED 0x431A60, called once at 0x491481): <gamedata>\<sidedata>.<tdf_ext>, which it opens through
   0x4290F0 (0x431A73..0x431A87, the strings the def load pushes at 0x42D937..0x42D959, where a file that does not load
   exits the game, 0x4B6290) and loads (0x431A95), is read [SIDE0], [SIDE1], ..., each the first section of that name
   (0x4C3410), until one is missing, and their count is stored at main+0x37F39 (0x4324FB). Each side's `nameprefix` is
   read into 4 bytes, so cut at 3 characters, and copied into the side record only when the key is there
   (0x431B26..0x431B66, the test at 0x431B42); an absent one leaves the record's prefix as it was, taken here as empty
   [INFERRED: the side records start zeroed]. ImpAI reads its menus for every side below min(that count, the five side
   records) (tagpu_impai.c, impai_menus_build), so these sides are the same. */
static void read_sides(Load* D)
{
    TAGPU_VFILE f;
    TAGPU_GUIFILE* g = NULL;
    int s, sec;
    D->nsides = 0;
    if (tagpu_vfs_find_built(D->L->vfs, D->p->gamedata, D->p->sidedata, D->p->tdf_ext, &f))
        g = load_positional(D->L->vfs, &f);
    if (!g) return;
    for (s = 0; s < tagpu_guifile_count(g, 0); s++) {     /* SIDE<s> is missing past the file's last section */
        char name[24];
        snprintf(name, sizeof name, "SIDE%d", s);
        if ((sec = tagpu_guifile_find(g, 0, name)) < 0) break;
        if (s < SIDE_RECORDS)
            copy_name(D->prefix[s], tagpu_guifile_get(g, sec, "nameprefix"), sizeof D->prefix[s]);
    }
    D->nsides = s < SIDE_RECORDS ? s : SIDE_RECORDS;
    tagpu_guifile_free(g);
}

/* The build-menu rule's engine: a type is its row + 1 (0 unknown), and every press a button offers counts, buildings
   and mobile units alike, since what a builder can make follows both. */
static unsigned bm_type_of(void* ctx, const char* name)
{
    int i = unit_row((const TASIM_INSTALL*)ctx, name);
    return i < 0 ? 0u : (unsigned)i + 1u;
}

static int bm_armable(void* ctx, unsigned type)
{
    (void)ctx;
    (void)type;
    return 1;
}

/* 0x4290F0(<gui>, name, <gui_ext>) when 0x4BBC40 sizes it above 0, in the page count probe (0x42D824..0x42D83F) and
   in the page loader (0x41AD17..0x41AD5F): a 0-byte page is a missing one, and the loader then takes the side's
   "<prefix>DL" page (0x41AD8E). */
static int page_file(Load* D, const char* name, TAGPU_VFILE* f)
{
    return tagpu_vfs_find_built(D->L->vfs, D->p->gui, name, D->p->gui_ext, f) && f->size > 0;
}

/* The builder's own page "<NAME><page>", the name the def load probes ("%s%d", 0x502B30) and the router builds (cut at
   31 characters, 0x41B7A6..0x41B7E9), with no bound on the page */
static int own_page(Load* D, const char* unitname, unsigned page, TAGPU_VFILE* f)
{
    char name[48];
    snprintf(name, sizeof name, "%.31s%u", unitname, page);
    return page_file(D, name, f);
}

/* def +0x241 bit 31 into `orders` and def +0x22E, the page count, as the def load sets them (DISASSEMBLED
   0x42D800..0x42D8CB; engine/ui.md, *The build pages*): bit 31 when "<NAME>0" answers; the count the first of 1, 2,
   ... that does not answer when "<NAME>1" does, stored as a byte (0x42D8AD), else 1 with bit 31, else 0. Then the
   download load raises the count to each of the builder's entries' MENU (0x42DF60..0x42DFB4, an unsigned byte
   compare). The probe stops: a finite install answers for finitely many names. */
static unsigned page_count(Load* D, const char* unitname, const TAGPU_BM_ENTRY* mine, unsigned nm, int* orders)
{
    TAGPU_VFILE f;
    unsigned count, p, k;
    *orders = own_page(D, unitname, 0, &f);
    if (own_page(D, unitname, 1, &f)) {
        for (p = 2; own_page(D, unitname, p, &f); p++)
            ;
        count = p & 0xFFu;
    } else
        count = *orders ? 1u : 0u;
    for (k = 0; k < nm; k++)
        if (mine[k].menu > count) count = mine[k].menu;
    return count;
}

static void read_page(Load* D, const char* unitname, const TAGPU_VFILE* f, unsigned page, const TAGPU_BM_ENTRY* mine,
                      unsigned nm, const TAGPU_BM_ENGINE* eng, unsigned char* offered)
{
    char why[160];
    unsigned char* b = tagpu_vfs_load(D->L->vfs, f, TAGPU_BM_FILE_CAP, why, sizeof why);
    if (!b) {
        fprintf(stderr, "tasim_load: %s: %s\n", f->path, why);
        return;
    }
    if (tagpu_buildmenu_page(b, f->size, page, mine, nm, eng, offered, why, sizeof why) < 0)
        fprintf(stderr, "tasim_load: %s page %u: %s\n", unitname, page, why);
    free(b);
}

/* Row i's build options appended to L->opt: every type a press of one of its pages reaches (tagpu_buildmenu), its
   entries in the order the engine's reader meets them (each_download), its pages tagpu_buildmenu_pages(bit 31, the
   count), each page its own file when that answers and otherwise every side's "<prefix>DL" in turn -- 0x41ACE0's choice
   for a seat of each side (0x41AD61..0x41AD94), merged as ImpAI merges a builder's sides (impai_menu_union). The
   engine's role filler takes a builder's options from that merge (tagpu_impai.c, role_options), so these are the
   engine's, side for side; with no side, ImpAI reads no menu and no builder has an option. MEASURED 2026-11-13 on
   TA Zero's 78 builders: 819 options summed over them here and in each of the engine's three sides (`impai: menus`);
   435 when the sections were probed from MENUENTRY1 and a page with no file was read for the builder's own side only
   (its `NotForAI_Arm` names no side record), when the tech levels of 108 types differed from the engine's
   (roles_check --setup tazero-alpha5); 598 with every side read but each file's first section skipped. */
static void build_options(Load* D, int i, TAGPU_BM_ENTRY* mine, unsigned char* offered)
{
    TASIM_INSTALL* L = D->L;
    const char* unitname = L->u[i].unitname;
    TAGPU_BM_ENGINE eng;
    unsigned char pages[TAGPU_BM_PAGES];
    unsigned nm = 0, np = 0, p;
    int k, pass, s, orders;
    for (pass = 0; pass < 2; pass++)
        for (k = 0; k < D->nent; k++)
            if (D->ent[k].builder == i && D->ent[k].cont == pass) mine[nm++] = D->ent[k].e;
    if (D->nsides) {
        const unsigned count = page_count(D, unitname, mine, nm, &orders);
        np = tagpu_buildmenu_pages(orders, count, pages);
    }
    memset(offered, 0, (size_t)L->nu + 1);
    eng.type_of = bm_type_of;
    eng.armable = bm_armable;
    eng.ctx = L;
    eng.ndefs = (unsigned)L->nu + 1;
    for (p = 0; p < np; p++) {
        TAGPU_VFILE f;
        if (own_page(D, unitname, pages[p], &f)) {
            read_page(D, unitname, &f, pages[p], mine, nm, &eng, offered);
            continue;
        }
        for (s = 0; s < D->nsides; s++) {
            char name[16];
            snprintf(name, sizeof name, "%sDL", D->prefix[s]);
            if (page_file(D, name, &f)) read_page(D, unitname, &f, pages[p], mine, nm, &eng, offered);
        }
    }
    L->opt_first[i] = L->nopt;
    for (k = 1; k <= L->nu; k++) {
        if (!offered[k] || k - 1 == i) continue;
        if (!grow((void**)&L->opt, &L->capopt, L->nopt + 1, sizeof *L->opt)) { D->err = 1; return; }
        L->opt[L->nopt++] = (uint16_t)(k - 1);
        L->opt_count[i]++;
    }
}

static void load_build_options(Load* D)
{
    TASIM_INSTALL* L = D->L;
    TAGPU_BM_ENTRY* mine;
    unsigned char* offered;
    int i;
    tagpu_vfs_enum(L->vfs, D->p->download, D->download_list, 0, each_download, D);
    read_sides(D);
    mine = (TAGPU_BM_ENTRY*)malloc((size_t)(D->nent ? D->nent : 1) * sizeof *mine);
    offered = (unsigned char*)malloc((size_t)L->nu + 1);
    if (!mine || !offered) D->err = 1;
    for (i = 0; i < L->nu && !D->err; i++)
        if (L->u[i].flags241 & (1u << 6))                     /* builder, +0x241 bit 6 */
            build_options(D, i, mine, offered);
    free(mine);
    free(offered);
}

/* ---- the report's fourth class ------------------------------------------------------------------------------ */

/* engine-features.tsv: after its comments, the column line `name tasim where evidence`, then a feature a row. Every
   row whose `tasim` is not `modelled` is reported with its landing: a later one, or `outside` the simulation. */
static int load_engine_rows(TS_REPORT* rep, const char* path)
{
    FILE* tsv = fopen(path, "r");
    char line[2048];
    if (!tsv) { fprintf(stderr, "tasim_load: cannot read %s\n", path); return -1; }
    while (fgets(line, sizeof line, tsv)) {
        char *tasim, *where, *end;
        int landing;
        if (line[0] == '#' || !(tasim = strchr(line, '\t'))) continue;
        *tasim++ = 0;
        while (*tasim == '\t') tasim++;
        if (!(where = strchr(tasim, '\t'))) continue;
        *where++ = 0;
        while (*where == '\t') where++;
        if ((end = strchr(where, '\t')) != NULL) *end = 0;
        if (!strcmp(line, "name") || !strcmp(tasim, "modelled")) continue;
        for (landing = 0; landing < TSL_COUNT && strcmp(tasim_landing_name(landing), tasim); landing++) {}
        if (landing == TSL_COUNT) {
            fprintf(stderr, "tasim_load: %s: %s: `%s` is no landing the report names\n", path, line, tasim);
            fclose(tsv);
            return -1;
        }
        tasim_report_add(rep, TSRP_ENGINE, landing, "engine", line, where);
    }
    fclose(tsv);
    return 0;
}

/* ---- the profile -------------------------------------------------------------------------------------------- */

enum { PK_TEXT, PK_PATH, PK_NUMBER, PK_MOUNT };

/* One key of the profile file (tasim_load.h, THE PROFILE): where it goes and its bound. */
typedef struct {
    const char* key;
    int         kind;
    size_t      off, cap;       /* a text's field and its size, NUL included          */
    int         lo, hi;         /* a number's range                                   */
} PKEY;

#define PTEXT(f)        { #f, PK_TEXT, offsetof(TASIM_PROFILE, f), sizeof(((TASIM_PROFILE*)0)->f), 0, 0 }
#define PPATH(f)        { #f, PK_PATH, offsetof(TASIM_PROFILE, f), sizeof(((TASIM_PROFILE*)0)->f), 0, 0 }
#define PNUMBER(f, a, b) { #f, PK_NUMBER, offsetof(TASIM_PROFILE, f), 0, a, b }

static const PKEY PROFILE_KEYS[] = {
    PPATH(root), PTEXT(image), PTEXT(language), { "mount", PK_MOUNT, 0, 0, 0, 0 },
    PTEXT(units), PTEXT(all_pattern), PTEXT(unit_ext), PTEXT(weapons), PTEXT(weapon_pattern), PTEXT(tdf_ext),
    PTEXT(gamedata), PTEXT(moveinfo), PTEXT(sidedata), PTEXT(download), PTEXT(gui), PTEXT(gui_ext), PTEXT(features),
    PTEXT(feature_pattern),
    PNUMBER(version_major, -128, 127), PNUMBER(version_minor, -128, 127), PNUMBER(archive_only, 0, 1),
    PTEXT(copyright), PTEXT(copyright_year), PTEXT(copyright_default),
};
#define NPKEYS ((int)(sizeof PROFILE_KEYS / sizeof PROFILE_KEYS[0]))

static int absolute(const char* path)
{
    return path[0] == '/' || path[0] == '\\' || (isalpha((unsigned char)path[0]) && path[1] == ':');
}

/* `v` as a decimal in [lo, hi]: an optional '-' and one to four digits, nothing else. */
static int number(const char* v, int lo, int hi, int* out)
{
    const char* d = v + (*v == '-');
    long x = 0;
    size_t k;
    for (k = 0; d[k]; k++) {
        if (k >= 4 || !isdigit((unsigned char)d[k])) return 0;
        x = x * 10 + (d[k] - '0');
    }
    if (!k) return 0;
    if (*v == '-') x = -x;
    if (x < lo || x > hi) return 0;
    *out = (int)x;
    return 1;
}

/* One `key<TAB>value` line into `p`; 0 with the reason in `why`. */
static int profile_line(TASIM_PROFILE* p, char* line, int no, unsigned char* seen, int* capmount, char* why,
                        size_t nwhy)
{
    char* value = strchr(line, '\t');
    const PKEY* k = NULL;
    size_t n;
    int i;
    if (!value) { snprintf(why, nwhy, "line %d has no TAB", no); return 0; }
    *value++ = 0;
    for (i = 0; i < NPKEYS; i++) if (!strcmp(line, PROFILE_KEYS[i].key)) { k = &PROFILE_KEYS[i]; break; }
    if (!k) { snprintf(why, nwhy, "line %d: an unknown key `%.40s`", no, line); return 0; }
    if (k->kind != PK_MOUNT && seen[i]++) { snprintf(why, nwhy, "line %d: %s given twice", no, k->key); return 0; }
    if (strpbrk(value, "\t\r")) { snprintf(why, nwhy, "%s: its value holds a TAB or a CR", k->key); return 0; }
    n = strlen(value);
    switch (k->kind) {
    case PK_NUMBER:
        if (!number(value, k->lo, k->hi, (int*)((char*)p + k->off))) {
            snprintf(why, nwhy, "%s: `%.20s` is not a number from %d to %d", k->key, value, k->lo, k->hi);
            return 0;
        }
        return 1;
    case PK_MOUNT:
        if (!n || n >= TAGPU_VFS_PATHMAX || !absolute(value) || p->nmount >= TAGPU_VFS_ARCSANE) {
            snprintf(why, nwhy, "mount: line %d is not an absolute path shorter than %d, or one past %d mounts", no,
                     TAGPU_VFS_PATHMAX, TAGPU_VFS_ARCSANE);
            return 0;
        }
        if (!grow((void**)&p->mount, capmount, p->nmount + 1, sizeof *p->mount)) {
            snprintf(why, nwhy, "mount: out of memory");
            return 0;
        }
        memcpy(p->mount[p->nmount++], value, n + 1);
        return 1;
    default:
        if (!n || n >= k->cap || (k->kind == PK_PATH && !absolute(value))) {
            snprintf(why, nwhy, "%s: its value is empty, not shorter than %u bytes%s", k->key, (unsigned)k->cap,
                     k->kind == PK_PATH ? " or not absolute" : "");
            return 0;
        }
        memcpy((char*)p + k->off, value, n + 1);
        return 1;
    }
}

int tasim_profile_read(TASIM_PROFILE* p, const char* path, char* why, size_t nwhy)
{
    FILE* f = fopen(path, "rb");
    char line[PROFILE_LINE];
    unsigned char seen[NPKEYS];
    const char* cut;
    const char* year;
    int no = 0, capmount = 0, i, ok = 1;
    memset(p, 0, sizeof *p);
    memset(seen, 0, sizeof seen);
    if (!f) { snprintf(why, nwhy, "cannot read %s", path); return -1; }
    while (ok && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        no++;
        if (n && line[n - 1] == '\n') line[--n] = 0;
        else if (!feof(f)) { snprintf(why, nwhy, "line %d is longer than %d bytes", no, PROFILE_LINE - 2); ok = 0; }
        if (!ok) break;
        if (no == 1) {
            if (strcmp(line, "tasim-profile/1")) { snprintf(why, nwhy, "not a tasim-profile/1 file"); ok = 0; }
            continue;
        }
        ok = profile_line(p, line, no, seen, &capmount, why, nwhy);
    }
    fclose(f);
    if (ok && !no) { snprintf(why, nwhy, "an empty file, not a tasim-profile/1 one"); ok = 0; }
    for (i = 0; ok && i < NPKEYS; i++)
        if (PROFILE_KEYS[i].kind != PK_MOUNT && !seen[i]) {
            snprintf(why, nwhy, "%s is missing", PROFILE_KEYS[i].key);
            ok = 0;
        }
    if (ok && (!(cut = strrchr(p->weapon_pattern, '\\')) || cut == p->weapon_pattern || !cut[1])) {
        snprintf(why, nwhy, "weapon_pattern: `%s` has no folder and filter either side of its last '\\'",
                 p->weapon_pattern);
        ok = 0;
    }
    if (ok && (strlen(p->copyright_year) != 4 || !(year = strstr(p->copyright, p->copyright_year)) ||
               year - p->copyright > COPYRIGHT_YEAR_MAX)) {
        snprintf(why, nwhy, "copyright_year: `%s` is not four bytes found in copyright at an offset of at most 0x%X",
                 p->copyright_year, COPYRIGHT_YEAR_MAX);
        ok = 0;
    }
    return ok ? 0 : -1;
}

void tasim_profile_free(TASIM_PROFILE* p)
{
    free(p->mount);
    p->mount = NULL;
    p->nmount = 0;
}

/* ---- the load ----------------------------------------------------------------------------------------------- */

static int open_install(Load* D, const TASIM_LOAD_IN* in)
{
    const TASIM_PROFILE* p = in->profile;
    const char* cut = strrchr(p->weapon_pattern, '\\');
    TASIM_INSTALL* L = D->L;
    D->p = p;
    /* the weapon listing's folder and filter, its pattern split at its last '\' (tasim_profile_read bounds both) */
    snprintf(D->weapon_dir, sizeof D->weapon_dir, "%.*s", (int)(cut - p->weapon_pattern), p->weapon_pattern);
    snprintf(D->weapon_filter, sizeof D->weapon_filter, "%s", cut + 1);
    if (!built_name(D->unit_list, sizeof D->unit_list, p->all_pattern, p->unit_ext) ||
        !built_name(D->download_list, sizeof D->download_list, p->all_pattern, p->tdf_ext)) {
        fprintf(stderr, "tasim_load: a listing's pattern does not fit\n");
        return -1;
    }
    L->vfs = tagpu_vfs_open(p->root, (const char (*)[TAGPU_VFS_PATHMAX])p->mount, p->nmount, p->language);
    if (!L->vfs) { fprintf(stderr, "tasim_load: out of memory opening %s\n", p->root); return -1; }
    return 0;
}

/* The definition files in the engine's order of reading them: the move classes, the units, the weapons, the
   features. */
static void read_definitions(Load* D)
{
    TAGPU_VFS* v = D->L->vfs;
    load_classes(D);
    tagpu_vfs_enum(v, D->p->units, D->unit_list, 0, each_unit, D);
    tagpu_vfs_enum(v, D->weapon_dir, D->weapon_filter, 0, each_weapons_file, D);
    /* 0x422310..0x422331: 0x4BCB50(<features>, <feature_pattern>, -1, 1) */
    tagpu_vfs_enum(v, D->p->features, D->p->feature_pattern, 1, each_feature_file, D);
}

int tasim_load(TASIM_INSTALL* L, const TASIM_LOAD_IN* in)
{
    Load D;
    int i;
    memset(L, 0, sizeof *L);
    memset(&D, 0, sizeof D);
    D.L = L;
    if (open_install(&D, in) < 0) return -1;
    L->u = (TASIM_UNITDEF*)calloc(TASIM_LOAD_UNITS, sizeof *L->u);
    L->key = (uint32_t*)calloc(TASIM_LOAD_UNITS, sizeof *L->key);
    L->keyed = (uint8_t*)calloc(TASIM_LOAD_UNITS, sizeof *L->keyed);
    L->w = (TASIM_WEAPONDEF*)calloc(TASIM_LOAD_WSECTIONS, sizeof *L->w);
    L->ids = (const TASIM_WEAPONDEF**)calloc(TS_WEAPON_IDS, sizeof *L->ids);
    L->dmg_first = (int32_t*)calloc(TASIM_LOAD_WSECTIONS, sizeof *L->dmg_first);
    L->dmg_count = (int32_t*)calloc(TASIM_LOAD_WSECTIONS, sizeof *L->dmg_count);
    L->f = (TASIM_FEATUREDEF*)calloc(TASIM_LOAD_FEATURES, sizeof *L->f);
    L->opt_first = (int32_t*)calloc(TASIM_LOAD_UNITS, sizeof *L->opt_first);
    L->opt_count = (int32_t*)calloc(TASIM_LOAD_UNITS, sizeof *L->opt_count);
    L->rep = (TS_REPORT*)calloc(1, sizeof *L->rep);
    if (!L->u || !L->key || !L->keyed || !L->w || !L->ids || !L->dmg_first || !L->dmg_count || !L->f ||
        !L->opt_first || !L->opt_count || !L->rep) {
        fprintf(stderr, "tasim_load: out of memory\n");
        return -1;
    }
    read_definitions(&D);
    if (!D.err && L->nu) {
        if (!sort_units(L)) D.err = 1;
        for (i = 0; i < L->nu && !D.err; i++) {
            tasim_unitdef_weapons(&L->u[i], L->ids, TS_WEAPON_IDS);
            tasim_unitdef_refusals(&L->u[i], L->rep);
        }
        if (!D.err) load_build_options(&D);
    }
    if (!D.err && in->features && load_engine_rows(L->rep, in->features) < 0) D.err = 1;
    free(D.ent);
    if (D.err || !L->nu) {
        fprintf(stderr, "tasim_load: %s\n", L->nu ? "the install could not be read" : "no unit in the install");
        return -1;
    }
    return 0;
}

int tasim_load_values(const TASIM_LOAD_IN* in, FILE* out)
{
    TASIM_INSTALL L;
    Load D;
    int rc = -1;
    memset(&L, 0, sizeof L);
    memset(&D, 0, sizeof D);
    D.L = &L;
    D.values = out;
    if (open_install(&D, in) == 0) {
        read_definitions(&D);
        rc = D.err ? -1 : 0;
    }
    tasim_load_free(&L);
    return rc;
}

void tasim_load_free(TASIM_INSTALL* L)
{
    if (L->vfs) tagpu_vfs_close(L->vfs);
    free(L->u); free(L->key); free(L->keyed); free(L->w); free(L->ids); free(L->dmg_first); free(L->dmg_count);
    free(L->dmg); free(L->f); free(L->opt_first); free(L->opt_count); free(L->opt); free(L->rep);
    memset(L, 0, sizeof *L);
}

int32_t tasim_load_damage(const TASIM_INSTALL* L, int32_t id, const char* type)
{
    const TASIM_WEAPONDEF* w;
    int wi, k;
    if (id < 0 || id >= TS_WEAPON_IDS || !(w = L->ids[id])) return 0;
    wi = (int)(w - L->w);
    for (k = 0; k < L->dmg_count[wi]; k++)
        if (ieq(L->dmg[L->dmg_first[wi] + k].name, type)) return L->dmg[L->dmg_first[wi] + k].damage;
    return (int32_t)w->damage;
}
