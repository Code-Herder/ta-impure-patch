/* tasim_load -- the install loader (tasim_load.h). */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_buildmenu.h"
#include "tagpu_hpi.h"
#include "tagpu_tdf.h"
#include "tagpu_vfs.h"
#include "tasim_load.h"

#define FILE_CAP        (4u << 20)      /* a text definition read whole                        */
#define ENTRY_MISSES    64              /* MENUENTRY<n> is probed until this many in a row miss */
#define SIDE_MAX        16
#define DIRLEN          64

typedef struct { char unitmenu[32]; TAGPU_BM_ENTRY e; } Entry;

/* The load's working state: the install being filled, and what only the load reads. */
typedef struct {
    TASIM_INSTALL* L;
    char           gamedata[DIRLEN], download[DIRLEN], gui[DIRLEN];
    FILE*          values;              /* tasim_load_values: every value, one a line          */
    Entry*         ent;                 /* the download entries, in the engine's file order    */
    int            nent, capent;
    TAGPU_TDF*     sidedata;
    int            err;
} Load;

typedef struct { const TAGPU_TDF* t; int s; } Sec;

static int ieq(const char* a, const char* b) { return tagpu_hpi_stricmp(a, b, (size_t)-1) == 0; }

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

static TAGPU_TDF* load_tdf(TAGPU_VFS* v, const TAGPU_VFILE* f)
{
    char why[160];
    unsigned char* b = tagpu_vfs_load(v, f, FILE_CAP, why, sizeof why);
    TAGPU_TDF* t;
    if (!b) { fprintf(stderr, "tasim_load: %s: %s\n", f->path, why); return NULL; }
    t = tagpu_tdf_parse((const char*)b, f->size);
    free(b);
    if (!t) fprintf(stderr, "tasim_load: %s: past the parser's bounds\n", f->path);
    return t;
}

static TAGPU_TDF* load_in(Load* D, const char* dir, const char* name)
{
    char path[TAGPU_VFS_PATHMAX];
    TAGPU_VFILE f;
    snprintf(path, sizeof path, "%s\\%s", dir, name);
    return tagpu_vfs_find(D->L->vfs, path, &f) ? load_tdf(D->L->vfs, &f) : NULL;
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
    TAGPU_TDF* t = load_in(D, D->gamedata, "moveinfo.tdf");
    char file[TAGPU_VFS_PATHMAX];
    int i;
    for (i = 0; i < TS_CLASSES; i++) tasim_moveclass_init(&L->cls[i]);
    if (!t) return;
    if (D->values) dump_values(D->values, t, 0);
    snprintf(file, sizeof file, "%s\\moveinfo.tdf", D->gamedata);
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

static int each_unit(void* ctx, const TAGPU_VFILE* f)
{
    Load* D = (Load*)ctx;
    TASIM_INSTALL* L = D->L;
    TAGPU_TDF* t;
    Sec c;
    if (L->nu >= TASIM_LOAD_UNITS) {
        fprintf(stderr, "tasim_load: more than %d units\n", TASIM_LOAD_UNITS);
        D->err = 1;
        return 0;
    }
    if (!(t = load_tdf(L->vfs, f))) return 1;
    if (D->values) dump_values(D->values, t, 0);
    c.t = t;
    if (!D->values && (c.s = tagpu_tdf_section(t, 0, "UNITINFO")) >= 0 && tagpu_tdf_get(t, c.s, "unitname")) {
        TASIM_UNITDEF* d = &L->u[L->nu++];
        memset(d, 0, sizeof *d);
        tasim_unitdef_read(d, sec_get, &c, L->cls);
        scan_keys(D, t, c.s, tasim_keys_unit, tasim_keys_class, "unit", d->unitname, f->path);
    }
    tagpu_tdf_free(t);
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

static int each_weapons_file(void* ctx, const TAGPU_VFILE* f)
{
    Load* D = (Load*)ctx;
    TASIM_INSTALL* L = D->L;
    TAGPU_TDF* t;
    int s;
    if (!(t = load_tdf(L->vfs, f))) return 1;
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

static int by_unitname(const void* a, const void* b)
{
    return tagpu_hpi_stricmp(((const TASIM_UNITDEF*)a)->unitname, ((const TASIM_UNITDEF*)b)->unitname, (size_t)-1);
}

/* ---- the build options -------------------------------------------------------------------------------------- */

static int each_download(void* ctx, const TAGPU_VFILE* f)
{
    Load* D = (Load*)ctx;
    TAGPU_TDF* t = load_tdf(D->L->vfs, f);
    int k, miss = 0;
    if (!t) return 1;
    for (k = 1; miss < ENTRY_MISSES; k++) {
        char sec[24];
        int s;
        Entry* e;
        snprintf(sec, sizeof sec, "MENUENTRY%d", k);
        if ((s = tagpu_tdf_section(t, 0, sec)) < 0) { miss++; continue; }
        miss = 0;
        if (!grow((void**)&D->ent, &D->capent, D->nent + 1, sizeof *D->ent)) { D->err = 1; break; }
        e = &D->ent[D->nent++];
        memset(e, 0, sizeof *e);
        copy_name(e->unitmenu, tagpu_tdf_get(t, s, "unitmenu"), sizeof e->unitmenu);
        copy_name(e->e.name, tagpu_tdf_get(t, s, "unitname"), sizeof e->e.name);
        e->e.menu = (unsigned char)tagpu_tdf_int(t, s, "menu", 0);
        e->e.button = (unsigned char)tagpu_tdf_int(t, s, "button", 0);
    }
    tagpu_tdf_free(t);
    return 1;
}

static int unit_row(const TASIM_INSTALL* L, const char* name)
{
    int i;
    for (i = 0; i < L->nu; i++) if (ieq(L->u[i].unitname, name)) return i;
    return -1;
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

static const char* side_prefix(const Load* D, const char* side)
{
    int k, s;
    if (!D->sidedata) return NULL;
    for (k = 0; k < SIDE_MAX; k++) {
        char sec[16];
        snprintf(sec, sizeof sec, "SIDE%d", k);
        if ((s = tagpu_tdf_section(D->sidedata, 0, sec)) >= 0 && tagpu_tdf_get(D->sidedata, s, "name") &&
            ieq(tagpu_tdf_get(D->sidedata, s, "name"), side))
            return tagpu_tdf_get(D->sidedata, s, "nameprefix");
    }
    return NULL;
}

static unsigned char* page_blob(Load* D, const char* name, size_t* n)
{
    TAGPU_VFILE f;
    char why[160];
    unsigned char* b;
    if (!tagpu_vfs_find_built(D->L->vfs, D->gui, name, "gui", &f)) return NULL;
    if (!(b = tagpu_vfs_load(D->L->vfs, &f, TAGPU_BM_FILE_CAP, why, sizeof why))) {
        fprintf(stderr, "tasim_load: %s: %s\n", f.path, why);
        return NULL;
    }
    *n = f.size;
    return b;
}

/* Row i's build options appended to L->opt. The pages are each 1..7 that the builder's own "<NAME><page>.GUI" holds
   or a download entry of it names [INFERRED: the engine's page count, def +0x22E, is not read from a file]; a page
   is read from its own file or else the side's "<prefix>DL" page, as 0x41ACE0 chooses. */
static void build_options(Load* D, int i, TAGPU_BM_ENTRY* mine, unsigned char* offered)
{
    TASIM_INSTALL* L = D->L;
    const TASIM_UNITDEF* u = &L->u[i];
    TAGPU_BM_ENGINE eng;
    int nm = 0, k, pages = 0;
    unsigned p;
    for (k = 0; k < D->nent; k++)
        if (ieq(D->ent[k].unitmenu, u->unitname)) {
            mine[nm++] = D->ent[k].e;
            if (D->ent[k].e.menu >= 2 && D->ent[k].e.menu - 1 > pages) pages = D->ent[k].e.menu - 1;
        }
    for (p = 1; p < TAGPU_BM_PAGES; p++) {
        char name[64];
        TAGPU_VFILE f;
        if (tagpu_buildmenu_page_name(name, sizeof name, u->unitname, p) &&
            tagpu_vfs_find_built(L->vfs, D->gui, name, "gui", &f) && (int)p > pages)
            pages = (int)p;
    }
    if (pages > TAGPU_BM_PAGES - 1) pages = TAGPU_BM_PAGES - 1;
    memset(offered, 0, (size_t)L->nu + 1);
    eng.type_of = bm_type_of;
    eng.armable = bm_armable;
    eng.ctx = L;
    eng.ndefs = (unsigned)L->nu + 1;
    for (p = 1; p <= (unsigned)pages; p++) {
        char name[64], why[160];
        const char* prefix;
        unsigned char* blob = NULL;
        size_t n = 0;
        if (tagpu_buildmenu_page_name(name, sizeof name, u->unitname, p))
            blob = page_blob(D, name, &n);
        if (!blob && (prefix = side_prefix(D, u->side)) != NULL) {
            snprintf(name, sizeof name, "%.3sDL", prefix);
            blob = page_blob(D, name, &n);
        }
        if (!blob) continue;
        if (tagpu_buildmenu_page(blob, n, p, mine, (unsigned)nm, &eng, offered, why, sizeof why) < 0)
            fprintf(stderr, "tasim_load: %s page %u: %s\n", u->unitname, p, why);
        free(blob);
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
    tagpu_vfs_enum(L->vfs, D->download, "*.tdf", 0, each_download, D);
    D->sidedata = load_in(D, D->gamedata, "sidedata.tdf");
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

/* engine-features.tsv: after its comments, the column line `name tasim where evidence`, then a feature a row. A row
   whose `tasim` is neither `modelled` nor a T1 landing is an engine behaviour outside T1. */
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
        if (!strcmp(line, "name") || !strcmp(tasim, "modelled") || !strncmp(tasim, "T1", 2)) continue;
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

/* ---- the load ----------------------------------------------------------------------------------------------- */

static int read_mounts(const char* path, char (**arc)[TAGPU_VFS_PATHMAX], int* n)
{
    FILE* f = fopen(path, "r");
    char line[TAGPU_VFS_PATHMAX + 2];
    int cap = 0;
    *arc = NULL;
    *n = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        size_t k = strlen(line);
        while (k && (line[k - 1] == '\n' || line[k - 1] == '\r')) line[--k] = 0;
        if (!k) continue;
        if (k >= TAGPU_VFS_PATHMAX || *n >= TAGPU_VFS_ARCSANE) { fclose(f); return 0; }
        if (!grow((void**)arc, &cap, *n + 1, sizeof **arc)) { fclose(f); return 0; }
        memcpy((*arc)[(*n)++], line, k + 1);
    }
    fclose(f);
    return 1;
}

static int open_install(Load* D, const TASIM_LOAD_IN* in)
{
    char (*arc)[TAGPU_VFS_PATHMAX] = NULL;
    int narc = 0;
    TASIM_INSTALL* L = D->L;
    copy_name(D->gamedata, in->gamedata ? in->gamedata : "gamedata", sizeof D->gamedata);
    copy_name(D->download, in->download ? in->download : "download", sizeof D->download);
    copy_name(D->gui, in->gui ? in->gui : "guis", sizeof D->gui);
    if (!read_mounts(in->mounts, &arc, &narc)) {
        fprintf(stderr, "tasim_load: cannot read the mount list %s\n", in->mounts);
        free(arc);
        return -1;
    }
    L->vfs = tagpu_vfs_open(in->cwd, (const char (*)[TAGPU_VFS_PATHMAX])arc, narc,
                            in->language ? in->language : "english");
    free(arc);
    if (!L->vfs) { fprintf(stderr, "tasim_load: out of memory opening %s\n", in->cwd); return -1; }
    return 0;
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
    L->w = (TASIM_WEAPONDEF*)calloc(TASIM_LOAD_WSECTIONS, sizeof *L->w);
    L->ids = (const TASIM_WEAPONDEF**)calloc(TS_WEAPON_IDS, sizeof *L->ids);
    L->dmg_first = (int32_t*)calloc(TASIM_LOAD_WSECTIONS, sizeof *L->dmg_first);
    L->dmg_count = (int32_t*)calloc(TASIM_LOAD_WSECTIONS, sizeof *L->dmg_count);
    L->f = (TASIM_FEATUREDEF*)calloc(TASIM_LOAD_FEATURES, sizeof *L->f);
    L->opt_first = (int32_t*)calloc(TASIM_LOAD_UNITS, sizeof *L->opt_first);
    L->opt_count = (int32_t*)calloc(TASIM_LOAD_UNITS, sizeof *L->opt_count);
    L->rep = (TS_REPORT*)calloc(1, sizeof *L->rep);
    if (!L->u || !L->w || !L->ids || !L->dmg_first || !L->dmg_count || !L->f || !L->opt_first || !L->opt_count ||
        !L->rep) {
        fprintf(stderr, "tasim_load: out of memory\n");
        return -1;
    }
    load_classes(&D);
    tagpu_vfs_enum(L->vfs, "units", "*.fbi", 0, each_unit, &D);
    tagpu_vfs_enum(L->vfs, "weapons", "*.tdf", 0, each_weapons_file, &D);
    tagpu_vfs_enum(L->vfs, "features", "*.tdf", 1, each_feature_file, &D);
    if (!D.err && L->nu) {
        /* the def array's order: 0x42D2E0 sorts it by _stricmp of the unitname (0x42DB60) */
        qsort(L->u, (size_t)L->nu, sizeof *L->u, by_unitname);
        for (i = 0; i < L->nu; i++) {
            tasim_unitdef_weapons(&L->u[i], L->ids, TS_WEAPON_IDS);
            tasim_unitdef_refusals(&L->u[i], L->rep);
        }
        load_build_options(&D);
    }
    if (!D.err && in->features && load_engine_rows(L->rep, in->features) < 0) D.err = 1;
    free(D.ent);
    if (D.sidedata) tagpu_tdf_free(D.sidedata);
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
        load_classes(&D);
        tagpu_vfs_enum(L.vfs, "units", "*.fbi", 0, each_unit, &D);
        tagpu_vfs_enum(L.vfs, "weapons", "*.tdf", 0, each_weapons_file, &D);
        tagpu_vfs_enum(L.vfs, "features", "*.tdf", 1, each_feature_file, &D);
        rc = D.err ? -1 : 0;
    }
    tasim_load_free(&L);
    return rc;
}

void tasim_load_free(TASIM_INSTALL* L)
{
    if (L->vfs) tagpu_vfs_close(L->vfs);
    free(L->u); free(L->w); free(L->ids); free(L->dmg_first); free(L->dmg_count); free(L->dmg); free(L->f);
    free(L->opt_first); free(L->opt_count); free(L->opt); free(L->rep);
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
