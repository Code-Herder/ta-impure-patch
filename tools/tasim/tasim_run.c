/* tasim_run -- tasim's command-line driver. It has one subcommand:

     tasim_run roles --cwd DIR --archives FILE [--language L] --json

   which reads every unit and weapon of an install through the engine's own file rules
   (tagpu_vfs: the loose files under DIR, then the archives FILE lists, one full path a line, in
   mount order, as tools/wiki/tavfs.py finds them) and prints tasim_roles' classification as one
   JSON object, schema tasim-roles/1. Exit 0 when it ran, 2 when it could not; never 1.

   THE FILE FILLER of TASIM_ROLE_IN: the keys the classifier reads, with the conversions the
   engine's parsers make (research/notes/engine/keys.md, the key inventory):
     - weapon: range (int, 0x7FFF when absent), reloadtime x 30 into a word (0x4FD250; truncated
       [INFERRED: the MSVC float-to-int rule]), burst, [DAMAGE] default and its rows (a unit name
       each), energypershot, metalpershot, the flag keys of +0x111, and Impure's `nottoair` as the
       weapon-key reader takes it (tagpu_datakeys.c: the integer's bit 0);
     - unit [UNITINFO]: the flag keys of +0x241/+0x245, bmcode, buildcostenergy/metal, maxdamage,
       workertime, maxvelocity x 65536 (0x4C4800's fx [INFERRED: truncated]), sightdistance,
       radardistance, radardistancejam, energyuse, makesmetal, transportcapacity,
       wpri_/wsec_/wspe_badTargetCategory (+0x231/+0x235/+0x239), weapon1..weapon10, and the
       water depths, its own keys over its movement class's [INFERRED: which wins];
     - the build options by the sidebar's page rule (tagpu_buildmenu): each page 1..7 that the
       builder's own "<NAME><page>.GUI" holds or a download entry of it names, read from its own
       file or else the side's "<prefix>DL" page, as 0x41ACE0 chooses. The engine's page count
       (def +0x22E) is not read from a file here [INFERRED: the pages that exist or are named].
   A weapon section is taken from the first weapons file that holds it [INFERRED: which of two
   duplicates the engine's weapon table keeps]. A weapon's air damage averages its [DAMAGE] over
   the units whose category names VTOL. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_buildmenu.h"
#include "tagpu_hpi.h"
#include "tagpu_tdf.h"
#include "tagpu_vfs.h"
#include "tasim_roles.h"

#define FILE_CAP        (4u << 20)      /* a text definition read whole                        */
#define MAX_UNITS       4096
#define MAX_WFILES      1024
#define ENTRY_MISSES    64              /* MENUENTRY<n> is probed until this many in a row miss */
#define CLASS_MAX       256             /* moveinfo's [CLASS<n>] probed 0..this                */
#define SIDE_MAX        16

typedef struct {
    TAGPU_TDF* tdf;
    int        info;                    /* [UNITINFO]                                          */
    char       name[32], side[16], weapon[TASIM_ROLE_WEAPONS][32];
    int        vtol, kbot, tank;
    int        footx, footz;
} Unit;

typedef struct { char unitmenu[32]; TAGPU_BM_ENTRY e; } Entry;

typedef struct {
    TAGPU_VFS*  vfs;
    Unit*       u;
    int         n;
    TAGPU_TDF*  wf[MAX_WFILES];
    int         nwf;
    TAGPU_TDF*  moveinfo;
    TAGPU_TDF*  sidedata;
    Entry*      ent;
    int         nent, capent;
    int         err;
} Install;

static int ieq(const char* a, const char* b) { return tagpu_hpi_stricmp(a, b, (size_t)-1) == 0; }

static void copy_name(char* dst, const char* src, size_t cap)
{
    size_t k = 0;
    if (src) for (; src[k] && k + 1 < cap; k++) dst[k] = src[k];
    dst[k] = 0;
}

static TAGPU_TDF* load_tdf(TAGPU_VFS* v, const TAGPU_VFILE* f)
{
    char why[160];
    unsigned char* b = tagpu_vfs_load(v, f, FILE_CAP, why, sizeof why);
    TAGPU_TDF* t;
    if (!b) { fprintf(stderr, "tasim_run: %s: %s\n", f->path, why); return NULL; }
    t = tagpu_tdf_parse((const char*)b, f->size);
    free(b);
    if (!t) fprintf(stderr, "tasim_run: %s: past the parser's bounds\n", f->path);
    return t;
}

static TAGPU_TDF* load_path(TAGPU_VFS* v, const char* path)
{
    TAGPU_VFILE f;
    return tagpu_vfs_find(v, path, &f) ? load_tdf(v, &f) : NULL;
}

static double fnum(const TAGPU_TDF* t, int s, const char* key, double dflt)
{
    const char* v = tagpu_tdf_get(t, s, key);
    return v ? strtod(v, NULL) : dflt;
}

/* `word` among the white-space separated words of `text`, compared as the engine's _stricmp */
static int has_word(const char* text, const char* word)
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

/* ---- the files -------------------------------------------------------------------------- */

static int each_unit(void* ctx, const TAGPU_VFILE* f)
{
    Install* I = (Install*)ctx;
    Unit* u;
    TAGPU_TDF* t;
    int info, k;
    if (I->n >= MAX_UNITS) { fprintf(stderr, "tasim_run: more than %d units\n", MAX_UNITS); I->err = 1; return 0; }
    if (!(t = load_tdf(I->vfs, f))) return 1;
    if ((info = tagpu_tdf_section(t, 0, "UNITINFO")) < 0 || !tagpu_tdf_get(t, info, "unitname")) {
        tagpu_tdf_free(t);
        return 1;
    }
    u = &I->u[I->n++];
    memset(u, 0, sizeof *u);
    u->tdf = t;
    u->info = info;
    copy_name(u->name, tagpu_tdf_get(t, info, "unitname"), sizeof u->name);
    copy_name(u->side, tagpu_tdf_get(t, info, "side"), sizeof u->side);
    for (k = 0; k < TASIM_ROLE_WEAPONS; k++) {
        char key[16];
        snprintf(key, sizeof key, "weapon%d", k + 1);
        copy_name(u->weapon[k], tagpu_tdf_get(t, info, key), sizeof u->weapon[k]);
    }
    u->vtol = has_word(tagpu_tdf_get(t, info, "category"), "VTOL");
    u->kbot = has_word(tagpu_tdf_get(t, info, "category"), "KBOT");
    u->tank = has_word(tagpu_tdf_get(t, info, "category"), "TANK");
    u->footx = (int)tagpu_tdf_int(t, info, "footprintx", 0);
    u->footz = (int)tagpu_tdf_int(t, info, "footprintz", 0);
    return 1;
}

static int each_weapons_file(void* ctx, const TAGPU_VFILE* f)
{
    Install* I = (Install*)ctx;
    TAGPU_TDF* t;
    if (I->nwf >= MAX_WFILES) return 0;
    if ((t = load_tdf(I->vfs, f)) != NULL) I->wf[I->nwf++] = t;
    return 1;
}

static int each_download(void* ctx, const TAGPU_VFILE* f)
{
    Install* I = (Install*)ctx;
    TAGPU_TDF* t = load_tdf(I->vfs, f);
    int k, miss = 0;
    if (!t) return 1;
    for (k = 1; miss < ENTRY_MISSES; k++) {
        char sec[24];
        int s;
        Entry* e;
        snprintf(sec, sizeof sec, "MENUENTRY%d", k);
        if ((s = tagpu_tdf_section(t, 0, sec)) < 0) { miss++; continue; }
        miss = 0;
        if (I->nent == I->capent) {
            Entry* grown = (Entry*)realloc(I->ent, (size_t)(I->capent ? I->capent * 2 : 256) * sizeof *grown);
            if (!grown) { I->err = 1; break; }
            I->ent = grown;
            I->capent = I->capent ? I->capent * 2 : 256;
        }
        e = &I->ent[I->nent++];
        memset(e, 0, sizeof *e);
        copy_name(e->unitmenu, tagpu_tdf_get(t, s, "unitmenu"), sizeof e->unitmenu);
        copy_name(e->e.name, tagpu_tdf_get(t, s, "unitname"), sizeof e->e.name);
        e->e.menu = (unsigned char)tagpu_tdf_int(t, s, "menu", 0);
        e->e.button = (unsigned char)tagpu_tdf_int(t, s, "button", 0);
    }
    tagpu_tdf_free(t);
    return 1;
}

static int find_weapon(const Install* I, const char* name, const TAGPU_TDF** t)
{
    int i, s;
    if (!name[0]) return -1;
    for (i = 0; i < I->nwf; i++)
        if ((s = tagpu_tdf_section(I->wf[i], 0, name)) >= 0) { *t = I->wf[i]; return s; }
    return -1;
}

static int unit_index(const Install* I, const char* name)
{
    int i;
    for (i = 0; i < I->n; i++) if (ieq(I->u[i].name, name)) return i;
    return -1;
}

/* ---- the filler ------------------------------------------------------------------------- */

static uint32_t flag(const TAGPU_TDF* t, int s, const char* key, uint32_t bit)
{
    return tagpu_tdf_int(t, s, key, 0) ? bit : 0;
}

static void fill_weapon(const Install* I, const TAGPU_TDF* t, int s, TASIM_ROLE_WEAPON* w)
{
    int dmg = tagpu_tdf_section(t, s, "DAMAGE"), i, nair = 0;
    double sum = 0.0;
    memset(w, 0, sizeof *w);
    w->range = (int32_t)tagpu_tdf_int(t, s, "range", 0x7FFF);
    w->reload_ticks = (float)(int)(fnum(t, s, "reloadtime", 0.0) * 30.0);
    w->burst = (int32_t)tagpu_tdf_int(t, s, "burst", 0);
    w->damage_default = dmg >= 0 ? (int32_t)tagpu_tdf_int(t, dmg, "default", 0) : 0;
    for (i = 0; i < I->n; i++) {
        if (!I->u[i].vtol) continue;
        sum += dmg >= 0 ? (double)tagpu_tdf_int(t, dmg, I->u[i].name, w->damage_default) : 0.0;
        nair++;
    }
    w->damage_air = nair ? (float)(sum / nair) : 0.0f;
    w->energy_per_shot = (float)fnum(t, s, "energypershot", 0.0);
    w->metal_per_shot = (float)fnum(t, s, "metalpershot", 0.0);
    w->flags = flag(t, s, "ballistic", TASIM_WF_BALLISTIC) | flag(t, s, "vlaunch", TASIM_WF_VLAUNCH) |
               flag(t, s, "paralyzer", TASIM_WF_PARALYZER) | flag(t, s, "dropped", TASIM_WF_DROPPED) |
               flag(t, s, "waterweapon", TASIM_WF_WATER) | flag(t, s, "toairweapon", TASIM_WF_TOAIR) |
               flag(t, s, "commandfire", TASIM_WF_COMMANDFIRE) |
               flag(t, s, "stockpile", TASIM_WF_STOCKPILE) |
               flag(t, s, "interceptor", TASIM_WF_INTERCEPTOR) |
               ((tagpu_tdf_int(t, s, "nottoair", 0) & 1) ? TASIM_WF_NOTTOAIR : 0u);
}

static int move_class(const Install* I, const char* name)
{
    int k, s;
    if (!I->moveinfo || !name) return -1;
    for (k = 0; k < CLASS_MAX; k++) {
        char sec[16];
        snprintf(sec, sizeof sec, "CLASS%d", k);
        if ((s = tagpu_tdf_section(I->moveinfo, 0, sec)) >= 0 &&
            tagpu_tdf_get(I->moveinfo, s, "name") && ieq(tagpu_tdf_get(I->moveinfo, s, "name"), name))
            return s;
    }
    return -1;
}

static void fill_unit(const Install* I, int i, TASIM_ROLE_IN* r)
{
    const Unit* u = &I->u[i];
    const TAGPU_TDF* t = u->tdf;
    int s = u->info, k, mc = move_class(I, tagpu_tdf_get(t, s, "movementclass"));
    static const char* const BAD[TASIM_ROLE_BADSLOTS] = {
        "wpri_badtargetcategory", "wsec_badtargetcategory", "wspe_badtargetcategory" };
    memset(r, 0, sizeof *r);
    copy_name(r->name, u->name, sizeof r->name);
    r->flags241 = flag(t, s, "builder", TASIM_F241_BUILDER) | flag(t, s, "canfly", TASIM_F241_CANFLY) |
                  flag(t, s, "canhover", TASIM_F241_CANHOVER) |
                  flag(t, s, "amphibious", TASIM_F241_AMPHIBIOUS) |
                  flag(t, s, "kamikaze", TASIM_F241_KAMIKAZE) |
                  flag(t, s, "antiweapons", TASIM_F241_ANTIWEAPONS);
    r->flags245 = flag(t, s, "canmove", TASIM_F245_CANMOVE) | flag(t, s, "commander", TASIM_F245_COMMANDER);
    r->bmcode = (uint8_t)tagpu_tdf_int(t, s, "bmcode", 0);
    /* the engine reads both costs as integers and stores them as floats (engine/keys.md, Costs and
       economy), so ARMPEEP's 40.28 metal costs 40 in the game */
    r->cost_energy = (float)tagpu_tdf_int(t, s, "buildcostenergy", 0);
    r->cost_metal = (float)tagpu_tdf_int(t, s, "buildcostmetal", 0);
    r->maxdamage = (uint32_t)tagpu_tdf_int(t, s, "maxdamage", 0);
    r->workertime = (uint16_t)tagpu_tdf_int(t, s, "workertime", 0);
    r->maxvelocity_fx = (int32_t)(fnum(t, s, "maxvelocity", 0.0) * 65536.0);
    r->sight = (uint16_t)tagpu_tdf_int(t, s, "sightdistance", 0);
    r->radar = (uint16_t)tagpu_tdf_int(t, s, "radardistance", 0);
    r->jammer = (uint16_t)tagpu_tdf_int(t, s, "radardistancejam", 0);
    r->minwaterdepth = (int16_t)tagpu_tdf_int(t, s, "minwaterdepth",
                                             mc >= 0 ? tagpu_tdf_int(I->moveinfo, mc, "minwaterdepth", 0) : 0);
    r->maxwaterdepth = (int16_t)tagpu_tdf_int(t, s, "maxwaterdepth",
                                             mc >= 0 ? tagpu_tdf_int(I->moveinfo, mc, "maxwaterdepth", 0) : 0);
    r->energyuse = (float)fnum(t, s, "energyuse", 0.0);
    r->makesmetal = (uint8_t)tagpu_tdf_int(t, s, "makesmetal", 0);
    r->transportcapacity = (uint8_t)tagpu_tdf_int(t, s, "transportcapacity", 0);
    for (k = 0; k < TASIM_ROLE_BADSLOTS; k++)
        r->slot_notair[k] = (uint8_t)has_word(tagpu_tdf_get(t, s, BAD[k]), "NOTAIR");
    for (k = 0; k < TASIM_ROLE_WEAPONS; k++) {
        const TAGPU_TDF* wt = NULL;
        int ws = find_weapon(I, u->weapon[k], &wt);
        if (ws < 0) continue;
        fill_weapon(I, wt, ws, &r->w[k]);
        r->nweapons = (uint16_t)(k + 1);
    }
}

/* The build-menu rule's engine: a type is its unit's index + 1 (0 unknown), and every press a
   button offers counts, buildings and mobile units alike, since the tech level follows both. */
static unsigned bm_type_of(void* ctx, const char* name)
{
    int i = unit_index((const Install*)ctx, name);
    return i < 0 ? 0u : (unsigned)i + 1u;
}

static int bm_armable(void* ctx, unsigned type)
{
    (void)ctx;
    (void)type;
    return 1;
}

static const char* side_prefix(const Install* I, const char* side)
{
    int k, s;
    if (!I->sidedata) return NULL;
    for (k = 0; k < SIDE_MAX; k++) {
        char sec[16];
        snprintf(sec, sizeof sec, "SIDE%d", k);
        if ((s = tagpu_tdf_section(I->sidedata, 0, sec)) >= 0 && tagpu_tdf_get(I->sidedata, s, "name") &&
            ieq(tagpu_tdf_get(I->sidedata, s, "name"), side))
            return tagpu_tdf_get(I->sidedata, s, "nameprefix");
    }
    return NULL;
}

static unsigned char* page_blob(Install* I, const char* name, size_t* n)
{
    TAGPU_VFILE f;
    char why[160];
    unsigned char* b;
    if (!tagpu_vfs_find_built(I->vfs, "guis", name, "gui", &f)) return NULL;
    if (!(b = tagpu_vfs_load(I->vfs, &f, TAGPU_BM_FILE_CAP, why, sizeof why))) {
        fprintf(stderr, "tasim_run: %s: %s\n", f.path, why);
        return NULL;
    }
    *n = f.size;
    return b;
}

/* The builder's build options into `opts` (indices), returning how many. */
static int build_options(Install* I, int i, unsigned char* offered, uint16_t* opts)
{
    const Unit* u = &I->u[i];
    TAGPU_BM_ENTRY* mine;
    TAGPU_BM_ENGINE eng;
    int nm = 0, k, pages = 0, m = 0;
    unsigned p;
    if (!(mine = (TAGPU_BM_ENTRY*)malloc((size_t)(I->nent ? I->nent : 1) * sizeof *mine))) return 0;
    for (k = 0; k < I->nent; k++)
        if (ieq(I->ent[k].unitmenu, u->name)) {
            mine[nm++] = I->ent[k].e;
            if (I->ent[k].e.menu >= 2 && I->ent[k].e.menu - 1 > pages) pages = I->ent[k].e.menu - 1;
        }
    for (p = 1; p < TAGPU_BM_PAGES; p++) {
        char name[64];
        TAGPU_VFILE f;
        if (tagpu_buildmenu_page_name(name, sizeof name, u->name, p) &&
            tagpu_vfs_find_built(I->vfs, "guis", name, "gui", &f) && (int)p > pages)
            pages = (int)p;
    }
    if (pages > TAGPU_BM_PAGES - 1) pages = TAGPU_BM_PAGES - 1;
    memset(offered, 0, (size_t)I->n + 1);
    eng.type_of = bm_type_of;
    eng.armable = bm_armable;
    eng.ctx = I;
    eng.ndefs = (unsigned)I->n + 1;
    for (p = 1; p <= (unsigned)pages; p++) {
        char name[64], why[160];
        const char* prefix;
        unsigned char* blob = NULL;
        size_t n = 0;
        if (tagpu_buildmenu_page_name(name, sizeof name, u->name, p))
            blob = page_blob(I, name, &n);
        if (!blob && (prefix = side_prefix(I, u->side)) != NULL) {
            snprintf(name, sizeof name, "%.3sDL", prefix);
            blob = page_blob(I, name, &n);
        }
        if (!blob) continue;
        if (tagpu_buildmenu_page(blob, n, p, mine, (unsigned)nm, &eng, offered, why, sizeof why) < 0)
            fprintf(stderr, "tasim_run: %s page %u: %s\n", u->name, p, why);
        free(blob);
    }
    free(mine);
    for (k = 1; k <= I->n; k++) if (offered[k] && k - 1 != i) opts[m++] = (uint16_t)(k - 1);
    return m;
}

/* ---- the output ------------------------------------------------------------------------- */

static void jstr(const char* s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20 || c >= 0x7F) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static const char* move_of(const Unit* u, const TASIM_ROLE_IN* r, const TASIM_ROLE_OUT* o)
{
    if (!(o->flags & TASIM_RF_MOBILE)) return "static";
    if (r->flags241 & TASIM_F241_CANFLY) return "air";
    if (r->flags241 & TASIM_F241_CANHOVER) return "hover";
    if (r->flags241 & TASIM_F241_AMPHIBIOUS) return "amphibious";
    if (o->layer == TASIM_LAYER_SEA) return "ship";
    if (u->kbot) return "kbot";
    if (u->tank) return "tank";
    return "other";
}

static void print_roles(const Install* I, const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* out,
                        const TASIM_ROLE_Q* q, double maker)
{
    static const char* const FLAG_NAMES[] = {
        "commander", "factory", "transport", "kamikaze", "antiweapons", "radar", "jammer", "mobile" };
    int i, L, k, first;
    printf("{\"schema\":\"tasim-roles/1\",\"R\":%.17g,\"r_maker\":", TASIM_ROLES_R);
    if (maker > 0.0) printf("%.17g", maker); else printf("null");
    printf(",\"quantiles\":{");
    for (L = 0; L < TASIM_LAYERS; L++)
        printf("%s\"%s\":{\"population\":%d,\"speed50\":%.17g,\"speed75\":%.17g,\"value10\":%.17g,"
               "\"range75\":%.17g}", L ? "," : "", tasim_roles_layer_name((TASIM_LAYER)L),
               q->population[L], q->speed50[L], q->speed75[L], q->value10[L], q->range75[L]);
    printf("},\"refused\":[");
    for (k = 0, first = 1; k < tagpu_vfs_narc(I->vfs); k++)
        if (tagpu_vfs_arc_refused(I->vfs, k)) {
            printf("%s", first ? "" : ",");
            jstr(tagpu_vfs_arc_name(I->vfs, k));
            first = 0;
        }
    printf("],\"units\":[");
    for (i = 0; i < I->n; i++) {
        const TASIM_ROLE_OUT* o = &out[i];
        double e_tick = 0.0, m_tick = 0.0;
        for (k = 0; k < in[i].nweapons; k++)
            if (in[i].w[k].reload_ticks > 0.0f) {
                e_tick += (double)in[i].w[k].energy_per_shot / (double)in[i].w[k].reload_ticks;
                m_tick += (double)in[i].w[k].metal_per_shot / (double)in[i].w[k].reload_ticks;
            }
        printf("%s{\"name\":", i ? "," : "");
        jstr(in[i].name);
        printf(",\"side\":");
        jstr(I->u[i].side);
        printf(",\"role\":\"%s\",\"layer\":\"%s\",\"move\":\"%s\",\"tech\":%d,\"value\":%.17g,"
               "\"dps_ground\":%.17g,\"dps_air\":%.17g,\"dps_water\":%.17g,\"range_ground\":%d,\"speed\":%.17g,"
               "\"footprint\":[%d,%d],\"maxdamage\":%u,\"cost\":[%.9g,%.9g],"
               "\"shot_cost_per_tick\":[%.17g,%.17g],\"flags\":[",
               tasim_roles_name(o->role), tasim_roles_layer_name(o->layer), move_of(&I->u[i], &in[i], o),
               o->tech, o->value, o->dps_ground, o->dps_air, o->dps_water, (int)o->range_ground, o->speed,
               I->u[i].footx, I->u[i].footz, (unsigned)in[i].maxdamage, (double)in[i].cost_metal,
               (double)in[i].cost_energy, m_tick, e_tick);
        for (k = 0, first = 1; k < 8; k++)
            if (o->flags & (1u << k)) { printf("%s\"%s\"", first ? "" : ",", FLAG_NAMES[k]); first = 0; }
        printf("]}");
    }
    printf("]}\n");
}

/* ---- main ------------------------------------------------------------------------------- */

static int read_list(const char* path, char (**arc)[TAGPU_VFS_PATHMAX], int* n)
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
        if (*n == cap) {
            char (*grown)[TAGPU_VFS_PATHMAX] = realloc(*arc, (size_t)(cap ? cap * 2 : 64) * sizeof *grown);
            if (!grown) { fclose(f); return 0; }
            *arc = grown;
            cap = cap ? cap * 2 : 64;
        }
        memcpy((*arc)[(*n)++], line, k + 1);
    }
    fclose(f);
    return 1;
}

static int usage(void)
{
    fprintf(stderr, "usage: tasim_run roles --cwd DIR --archives FILE [--language L] --json\n");
    return 2;
}

static int cmd_roles(const char* cwd, const char* list, const char* language)
{
    Install I;
    char (*arc)[TAGPU_VFS_PATHMAX] = NULL;
    TASIM_ROLE_IN* in = NULL;
    TASIM_ROLE_OUT* out = NULL;
    TASIM_ROLE_Q q;
    uint16_t* opts = NULL;
    unsigned char* offered = NULL;
    void* work = NULL;
    int narc = 0, i, rc = 2, total = 0;
    memset(&I, 0, sizeof I);
    if (!read_list(list, &arc, &narc)) { fprintf(stderr, "tasim_run: cannot read the mount list %s\n", list); return 2; }
    if (!(I.vfs = tagpu_vfs_open(cwd, (const char (*)[TAGPU_VFS_PATHMAX])arc, narc, language))) goto done;
    if (!(I.u = (Unit*)calloc(MAX_UNITS, sizeof *I.u))) goto done;
    tagpu_vfs_enum(I.vfs, "units", "*.fbi", 0, each_unit, &I);
    tagpu_vfs_enum(I.vfs, "weapons", "*.tdf", 0, each_weapons_file, &I);
    tagpu_vfs_enum(I.vfs, "download", "*.tdf", 0, each_download, &I);
    I.moveinfo = load_path(I.vfs, "gamedata\\moveinfo.tdf");
    I.sidedata = load_path(I.vfs, "gamedata\\sidedata.tdf");
    if (I.err || !I.n) { fprintf(stderr, "tasim_run: %s\n", I.n ? "the install could not be read" : "no unit in the install"); goto done; }
    in = (TASIM_ROLE_IN*)calloc((size_t)I.n, sizeof *in);
    out = (TASIM_ROLE_OUT*)calloc((size_t)I.n, sizeof *out);
    offered = (unsigned char*)malloc((size_t)I.n + 1);
    opts = (uint16_t*)malloc((size_t)I.n * (size_t)I.n * sizeof *opts);
    work = malloc(tasim_roles_work(I.n));
    if (!in || !out || !offered || !opts || !work) goto done;
    for (i = 0; i < I.n; i++) fill_unit(&I, i, &in[i]);
    for (i = 0; i < I.n; i++) {
        if (!(in[i].flags241 & TASIM_F241_BUILDER)) continue;
        in[i].build = opts + total;
        in[i].nbuild = (uint16_t)build_options(&I, i, offered, opts + total);
        total += in[i].nbuild;
    }
    switch (tasim_roles_classify(in, I.n, out, &q, work)) {
    case 0: print_roles(&I, in, out, &q, tasim_roles_maker_ratio(in, I.n)); rc = 0; break;
    case -1: fprintf(stderr, "tasim_run: no unit has the commander bit: no tech level is defined\n"); break;
    default: fprintf(stderr, "tasim_run: a build option is past the unit list\n"); break;
    }
done:
    for (i = 0; i < I.n; i++) tagpu_tdf_free(I.u[i].tdf);
    for (i = 0; i < I.nwf; i++) tagpu_tdf_free(I.wf[i]);
    if (I.moveinfo) tagpu_tdf_free(I.moveinfo);
    if (I.sidedata) tagpu_tdf_free(I.sidedata);
    if (I.vfs) tagpu_vfs_close(I.vfs);
    free(I.u); free(I.ent); free(arc); free(in); free(out); free(offered); free(opts); free(work);
    return rc;
}

int main(int argc, char** argv)
{
    const char *cwd = NULL, *list = NULL, *language = "english";
    int i, json = 0;
    if (argc < 2 || strcmp(argv[1], "roles")) return usage();
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "--cwd")) cwd = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--archives")) list = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--language")) language = argv[++i];
        else return usage();
    }
    if (!cwd || !list || !json) return usage();
    return cmd_roles(cwd, list, language);
}
