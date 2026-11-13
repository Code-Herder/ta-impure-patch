/* tasim_run -- tasim's command-line driver. Every subcommand reads an install through the install loader
   (tasim_load.h): the loose files under DIR, then the archives FILE lists, one full path a line, in mount order, as
   tools/tasim/native.py writes it. --folders names the folders a mod's loader renames in memory, as
   tools/wiki/gfx.py's --folders does (`gui=guiP,gamedata=gamedatP,download=downloadP` for ProTA; `unitpics`, which
   no definition is read from, is taken and unused, so a setup record's folders pass whole). Exit 0 when it ran, 2
   when it could not; never 1.

     tasim_run roles --cwd DIR --archives FILE [--language L] [--folders F] --json
       tasim_roles' classification of every unit, one JSON object, schema tasim-roles/1;

     tasim_run defs --cwd DIR --archives FILE [--language L] [--folders F] [--features TSV] --json
       every unit, weapon, feature and move class of the install as the loader read them, as one JSON object,
       schema tasim-defs/1, the units in the def array's order (unit i the engine's type i + 1) and the counts of
       the FBIs the pre-scan drops: each definition's engine image (its fields' bytes at the engine's offsets, which
       tools/tasim/defcheck.py compares with a running game's), its references by name, and the loader's
       named-not-modelled report (`unmodelled`), with tools/tasim/engine-features.tsv's rows that are not
       `modelled` when --features names it;

     tasim_run values --cwd DIR --archives FILE [--language L] [--folders F]
       every value of every key of those files, one a line (a value holding a line break is left out): the sample
       tests/tasim_keys_test.py runs the number getters on.

   THE ROLE FILLER of TASIM_ROLE_IN is a copy of the loaded definitions, as the engine filler tagpu_worldq_role_in
   copies the engine's: the def's fields as 0x42BF40 stored them (the water depths the class record's when the unit
   names one), its weapons the records in the slots 0x49E5B0 resolves weapon1..weapon10 to, each weapon's air damage
   the mean of its [DAMAGE] over the types of category VTOL in def row order, and the flag bits the classifier
   tests. The build options are the loader's. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_keys.h"
#include "tasim_load.h"
#include "tasim_roles.h"

/* the bits the classifier tests, the same the engine filler copies (tagpu_worldq.c, ROLE_F241, ROLE_F245, ROLE_WF) */
#define ROLE_F241 (TASIM_F241_BUILDER | TASIM_F241_CANFLY | TASIM_F241_CANHOVER | TASIM_F241_AMPHIBIOUS | \
                   TASIM_F241_KAMIKAZE | TASIM_F241_ANTIWEAPONS)
#define ROLE_F245 (TASIM_F245_CANMOVE | TASIM_F245_COMMANDER)
#define ROLE_WF   (TASIM_WF_BALLISTIC | TASIM_WF_VLAUNCH | TASIM_WF_PARALYZER | TASIM_WF_DROPPED | \
                   TASIM_WF_WATER | TASIM_WF_TOAIR | TASIM_WF_COMMANDFIRE | TASIM_WF_STOCKPILE | \
                   TASIM_WF_INTERCEPTOR)
#define WK_NOTTOAIR 0x01u                   /* tagpu_datakeys.h, TAGPU_WK_NOTTOAIR                          */

static void copy_name(char* dst, const char* src, size_t cap)
{
    size_t k = 0;
    if (src) for (; src[k] && k + 1 < cap; k++) dst[k] = src[k];
    dst[k] = 0;
}

/* ---- the role filler ------------------------------------------------------------------------------------ */

/* Weapon slot `id`'s air damage: the mean of its damage against each type of category VTOL, in def row order. */
static float air_damage(const TASIM_INSTALL* L, int32_t id, const uint16_t* air, int nair)
{
    double sum = 0.0;
    int j;
    for (j = 0; j < nair; j++) sum += (double)tasim_load_damage(L, id, L->u[air[j]].unitname);
    return nair ? (float)(sum / nair) : 0.0f;
}

static void fill_unit(const TASIM_INSTALL* L, int i, const uint16_t* air, int nair, TASIM_ROLE_IN* r)
{
    const TASIM_UNITDEF* d = &L->u[i];
    int k;
    memset(r, 0, sizeof *r);
    copy_name(r->name, d->unitname, sizeof r->name);
    r->flags241 = d->flags241 & ROLE_F241;
    r->flags245 = d->flags245 & ROLE_F245;
    r->bmcode = d->bmcode;
    r->cost_energy = d->buildcostenergy;
    r->cost_metal = d->buildcostmetal;
    r->maxdamage = d->maxdamage;
    r->workertime = d->workertime;
    r->maxvelocity_fx = d->maxvelocity;
    r->sight = d->sightdistance;
    r->radar = d->radardistance;
    r->jammer = d->radardistancejam;
    r->minwaterdepth = d->minwaterdepth;
    r->maxwaterdepth = d->maxwaterdepth;
    r->energyuse = d->energyuse;
    r->makesmetal = d->makesmetal;
    r->transportcapacity = d->transportcapacity;
    for (k = 0; k < TASIM_ROLE_BADSLOTS; k++)
        r->slot_notair[k] = (uint8_t)tasim_load_has_word(d->badtarget[k], "NOTAIR");
    for (k = 0; k < TASIM_ROLE_WEAPONS && k < TS_UNIT_WEAPONS; k++) {
        const TASIM_WEAPONDEF* w;
        TASIM_ROLE_WEAPON* o = &r->w[k];
        if (d->weaponid[k] == TS_NONE || !(w = L->ids[d->weaponid[k]])) continue;
        o->range = w->range;
        o->reload_ticks = (float)w->reloadtime;
        o->burst = w->burst;
        o->damage_default = w->damage;
        o->damage_air = air_damage(L, d->weaponid[k], air, nair);
        o->energy_per_shot = w->energypershot;
        o->metal_per_shot = w->metalpershot;
        o->flags = (w->flags & ROLE_WF) | ((w->wkeys & WK_NOTTOAIR) ? TASIM_WF_NOTTOAIR : 0u);
        r->nweapons = (uint16_t)(k + 1);
    }
    r->nbuild = (uint16_t)L->opt_count[i];
    r->build = L->opt + L->opt_first[i];
}

/* ---- the output ----------------------------------------------------------------------------------------- */

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

static void print_refused(const TASIM_INSTALL* L)
{
    int k, first = 1;
    for (k = 0; k < tagpu_vfs_narc(L->vfs); k++)
        if (tagpu_vfs_arc_refused(L->vfs, k)) {
            printf("%s", first ? "" : ",");
            jstr(tagpu_vfs_arc_name(L->vfs, k));
            first = 0;
        }
}

static const char* move_of(const TASIM_UNITDEF* d, const TASIM_ROLE_IN* r, const TASIM_ROLE_OUT* o)
{
    if (!(o->flags & TASIM_RF_MOBILE)) return "static";
    if (r->flags241 & TASIM_F241_CANFLY) return "air";
    if (r->flags241 & TASIM_F241_CANHOVER) return "hover";
    if (r->flags241 & TASIM_F241_AMPHIBIOUS) return "amphibious";
    if (o->layer == TASIM_LAYER_SEA) return "ship";
    if (tasim_load_has_word(d->category, "KBOT")) return "kbot";
    if (tasim_load_has_word(d->category, "TANK")) return "tank";
    return "other";
}

static void print_roles(const TASIM_INSTALL* L, const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* out,
                        const TASIM_ROLE_Q* q, double maker)
{
    static const char* const FLAG_NAMES[] = {
        "commander", "factory", "transport", "kamikaze", "antiweapons", "radar", "jammer", "mobile" };
    int i, l, k, first;
    printf("{\"schema\":\"tasim-roles/1\",\"R\":%.17g,\"r_maker\":", TASIM_ROLES_R);
    if (maker > 0.0) printf("%.17g", maker); else printf("null");
    printf(",\"quantiles\":{");
    for (l = 0; l < TASIM_LAYERS; l++)
        printf("%s\"%s\":{\"population\":%d,\"speed50\":%.17g,\"speed75\":%.17g,\"value10\":%.17g,"
               "\"range75\":%.17g}", l ? "," : "", tasim_roles_layer_name((TASIM_LAYER)l),
               q->population[l], q->speed50[l], q->speed75[l], q->value10[l], q->range75[l]);
    printf("},\"refused\":[");
    print_refused(L);
    printf("],\"units\":[");
    for (i = 0; i < L->nu; i++) {
        const TASIM_ROLE_OUT* o = &out[i];
        const TASIM_UNITDEF* d = &L->u[i];
        double e_tick = 0.0, m_tick = 0.0;
        for (k = 0; k < in[i].nweapons; k++)
            if (in[i].w[k].reload_ticks > 0.0f) {
                e_tick += (double)in[i].w[k].energy_per_shot / (double)in[i].w[k].reload_ticks;
                m_tick += (double)in[i].w[k].metal_per_shot / (double)in[i].w[k].reload_ticks;
            }
        printf("%s{\"name\":", i ? "," : "");
        jstr(in[i].name);
        printf(",\"side\":");
        jstr(d->side);
        printf(",\"role\":\"%s\",\"layer\":\"%s\",\"move\":\"%s\",\"tech\":%d,\"value\":%.17g,"
               "\"dps_ground\":%.17g,\"dps_air\":%.17g,\"dps_water\":%.17g,\"range_ground\":%d,\"speed\":%.17g,"
               "\"footprint\":[%d,%d],\"maxdamage\":%u,\"cost\":[%.9g,%.9g],"
               "\"shot_cost_per_tick\":[%.17g,%.17g],\"flags\":[",
               tasim_roles_name(o->role), tasim_roles_layer_name(o->layer), move_of(d, &in[i], o),
               o->tech, o->value, o->dps_ground, o->dps_air, o->dps_water, (int)o->range_ground, o->speed,
               (int)d->footx, (int)d->footz, (unsigned)in[i].maxdamage, (double)in[i].cost_metal,
               (double)in[i].cost_energy, m_tick, e_tick);
        for (k = 0, first = 1; k < 8; k++)
            if (o->flags & (1u << k)) { printf("%s\"%s\"", first ? "" : ",", FLAG_NAMES[k]); first = 0; }
        printf("]}");
    }
    printf("]}\n");
}

/* One field of the engine image: [off, width, "bytes", mask], the bytes little-endian hex, the mask the bits the
   loader writes (-1 for all of them). */
static void image_row(int* first, unsigned off, int width, const void* p, long long mask)
{
    const unsigned char* b = (const unsigned char*)p;
    int i;
    printf("%s[%u,%d,\"", *first ? "" : ",", off, width);
    for (i = 0; i < width; i++) printf("%02x", b[i]);
    printf("\",%lld]", mask);
    *first = 0;
}

/* The table's numeric rows when `flag_off` is TS_NOOFF; else the flag word at `flag_off` alone, once, with the
   bits its rows write and `extra`. */
static void image_table(int* first, const TS_KEY* table, const void* def, unsigned flag_off, uint32_t extra)
{
    const unsigned char* base = (const unsigned char*)def;
    const TS_KEY* k;
    uint32_t mask = extra;
    int width = 0;
    const void* word = NULL;
    for (k = table; k->name; k++) {
        if (k->off == TS_NOOFF || k->kind == TSK_STR || k->kind == TSK_KNOWN) continue;
        if (k->kind == TSK_BIT || k->kind == TSK_BIT2 || k->kind == TSK_SDCOUNT) {
            if (k->off != flag_off) continue;
            mask |= (k->kind == TSK_BIT ? 1u : k->kind == TSK_BIT2 ? 3u : 7u) << k->bit;
            word = base + k->field;
            width = k->width;
        } else if (flag_off == TS_NOOFF)
            image_row(first, k->off, k->width, base + k->field, -1);
    }
    if (word) image_row(first, flag_off, width, word, (long long)mask);
}

static void print_report(const TS_REPORT* rep)
{
    static const char* const CLS[] = { "later", "ignored", "refused", "engine" };
    int i;
    for (i = 0; i < rep->n; i++) {
        const TS_REPORT_ROW* r = &rep->row[i];
        printf("%s{\"class\":\"%s\",\"landing\":\"%s\",\"kind\":", i ? "," : "", CLS[r->cls],
               tasim_landing_name(r->landing));
        jstr(r->kind);
        printf(",\"what\":");
        jstr(r->what);
        printf(",\"first\":");
        jstr(r->first);
        printf(",\"count\":%d}", (int)r->count);
    }
}

static void print_defs(const TASIM_INSTALL* L)
{
    static const unsigned BOX[7] = { 0x15E, 0x166, 0x16A, 0x172, 0x176, 0x17E, 0x182 };
    static const unsigned WOFF[5] = { 0x1EE, 0x1F2, 0x1F6, 0x220, 0x224 };
    int i, k, first;
    printf("{\"schema\":\"tasim-defs/1\",\"counts\":{\"units\":%d,\"units_dropped\":{\"version\":%d,\"loose\":%d,"
           "\"copyright\":%d},\"weapons\":%d,\"weapon_slots\":%d,\"weapons_replaced\":%d,\"weapons_refused\":%d,"
           "\"features\":%d},\"refused_archives\":[", L->nu, L->dropped_version, L->dropped_loose,
           L->dropped_copyright, L->nw, TS_WEAPON_IDS, L->replaced, L->refused, L->nf);
    print_refused(L);
    printf("],\"classes\":[");
    for (i = 0, first = 1; i < TS_CLASSES; i++) {
        int f2 = 1;
        if (!L->cls[i].present) continue;
        printf("%s{\"index\":%d,\"name\":", first ? "" : ",", i);
        jstr(L->cls[i].name);
        printf(",\"image\":[");
        image_table(&f2, tasim_keys_class, &L->cls[i], TS_NOOFF, 0);
        printf("]}");
        first = 0;
    }
    printf("],\"units\":[");
    for (i = 0; i < L->nu; i++) {
        const TASIM_UNITDEF* d = &L->u[i];
        uint8_t yard[64 * 64];
        int f2 = 1, cells;
        printf("%s{\"unitname\":", i ? "," : "");
        jstr(d->unitname);
        printf(",\"strings\":{\"0x0\":");
        jstr(d->name);
        printf(",\"0x40\":");
        jstr(d->description);
        printf(",\"0x80\":");
        jstr(d->objectname);
        printf("},\"image\":[");
        image_table(&f2, tasim_keys_unit, d, TS_NOOFF, 0);
        image_table(&f2, tasim_keys_unit, d, 0x241, 0x10000u);   /* bit 16: tasim_unitdef_weapons */
        image_table(&f2, tasim_keys_unit, d, 0x245, 0x2200u);    /* bits 9, 13: tasim_unitdef_read */
        image_row(&f2, 0x14A, 2, &d->footx, -1);
        image_row(&f2, 0x14C, 2, &d->footz, -1);
        image_row(&f2, 0x1BE, 2, &d->maxwaterdepth, -1);
        image_row(&f2, 0x1C0, 2, &d->minwaterdepth, -1);
        image_row(&f2, 0x228, 1, &d->maxslope, -1);
        image_row(&f2, 0x229, 1, &d->maxwaterslope, -1);
        image_row(&f2, 0x196, 4, &d->slopevelocity, -1);
        for (k = 0; k < 7; k++) image_row(&f2, BOX[k], 4, &d->box[k], -1);
        printf("],\"weapons\":{");
        for (k = 0; k < 5; k++) {
            const int32_t id = k < 3 ? d->weaponid[k]
                             : tasim_weapon_lookup(L->ids, TS_WEAPON_IDS, k == 3 ? d->explodeas : d->selfdestructas);
            printf("%s\"0x%X\":", k ? "," : "", WOFF[k]);
            jstr(id == TS_NONE ? "" : L->ids[id]->section);
        }
        printf("},\"corpse\":");
        jstr(d->corpse);
        printf(",\"class\":%d,\"yard\":", (int)d->classidx);
        cells = tasim_yard_cells(d, yard, (int)sizeof yard);
        if (cells) {
            printf("\"");
            for (k = 0; k < cells; k++) printf("%02x", yard[k]);
            printf("\"");
        } else printf("null");
        printf("}");
    }
    printf("],\"weapons\":[");
    for (i = 0, first = 1; i < TS_WEAPON_IDS; i++) {
        const TASIM_WEAPONDEF* w = L->ids[i];
        int f2 = 1;
        if (!w) continue;
        printf("%s{\"id\":%d,\"section\":", first ? "" : ",", i);
        jstr(w->section);
        printf(",\"name\":");
        jstr(w->name);
        printf(",\"damage_rows\":%d,\"image\":[", (int)w->damage_rows);
        image_table(&f2, tasim_keys_weapon, w, TS_NOOFF, 0);
        image_table(&f2, tasim_keys_weapon, w, 0x111, 0);
        image_row(&f2, 0xD4, 2, &w->damage, -1);
        printf("]}");
        first = 0;
    }
    printf("],\"features\":[");
    for (i = 0; i < L->nf; i++) {
        const TASIM_FEATUREDEF* f = &L->f[i];
        const int32_t bw = tasim_weapon_lookup(L->ids, TS_WEAPON_IDS, f->burnweapon);
        int f2 = 1;
        printf("%s{\"name\":", i ? "," : "");
        jstr(f->name);
        printf(",\"image\":[");
        image_table(&f2, tasim_keys_feature, f, TS_NOOFF, 0);
        image_table(&f2, tasim_keys_feature, f, 0xFE, 0x1u);    /* bit 0: no object */
        printf("],\"burnweapon\":");
        jstr(bw == TS_NONE ? "" : L->ids[bw]->section);
        printf("}");
    }
    printf("],\"unmodelled\":[");
    print_report(L->rep);
    printf("],\"report_dropped\":%d}\n", (int)L->rep->dropped);
}

/* ---- the subcommands ------------------------------------------------------------------------------------ */

static int cmd_defs(const TASIM_LOAD_IN* in)
{
    TASIM_INSTALL L;
    int rc = 2;
    if (tasim_load(&L, in) == 0) {
        print_defs(&L);
        rc = 0;
    }
    tasim_load_free(&L);
    return rc;
}

static int cmd_roles(const TASIM_LOAD_IN* in)
{
    TASIM_INSTALL L;
    TASIM_ROLE_IN* r = NULL;
    TASIM_ROLE_OUT* out = NULL;
    TASIM_ROLE_Q q;
    uint16_t* air = NULL;
    void* work = NULL;
    int i, nair = 0, rc = 2;
    if (tasim_load(&L, in) < 0) goto done;
    r = (TASIM_ROLE_IN*)calloc((size_t)L.nu, sizeof *r);
    out = (TASIM_ROLE_OUT*)calloc((size_t)L.nu, sizeof *out);
    air = (uint16_t*)malloc((size_t)L.nu * sizeof *air);
    work = malloc(tasim_roles_work(L.nu));
    if (!r || !out || !air || !work) goto done;
    for (i = 0; i < L.nu; i++)
        if (tasim_load_has_word(L.u[i].category, "VTOL")) air[nair++] = (uint16_t)i;
    for (i = 0; i < L.nu; i++) fill_unit(&L, i, air, nair, &r[i]);
    switch (tasim_roles_classify(r, L.nu, out, &q, work)) {
    case 0: print_roles(&L, r, out, &q, tasim_roles_maker_ratio(r, L.nu)); rc = 0; break;
    case -1: fprintf(stderr, "tasim_run: no unit has the commander bit: no tech level is defined\n"); break;
    default: fprintf(stderr, "tasim_run: a build option is past the unit list\n"); break;
    }
done:
    tasim_load_free(&L);
    free(r); free(out); free(air); free(work);
    return rc;
}

static int usage(void)
{
    fprintf(stderr, "usage: tasim_run roles --cwd DIR --archives FILE [--language L] [--folders F] --json\n"
                    "       tasim_run defs --cwd DIR --archives FILE [--language L] [--folders F] [--features TSV] --json\n"
                    "       tasim_run values --cwd DIR --archives FILE [--language L] [--folders F]\n");
    return 2;
}

/* `name=folder` pairs, comma-separated, into `in`; the folder strings live in `buf`. 0 when one is malformed. */
static int parse_folders(char* buf, TASIM_LOAD_IN* in)
{
    char* p = buf;
    while (p && *p) {
        char* next = strchr(p, ',');
        char* eq = strchr(p, '=');
        if (next) *next++ = 0;
        if (!eq || eq == p || !eq[1]) return 0;
        *eq++ = 0;
        if (!strcmp(p, "gui")) in->gui = eq;
        else if (!strcmp(p, "gamedata")) in->gamedata = eq;
        else if (!strcmp(p, "download")) in->download = eq;
        else if (strcmp(p, "unitpics")) return 0;
        p = next;
    }
    return 1;
}

int main(int argc, char** argv)
{
    TASIM_LOAD_IN in;
    const char* folders = NULL;
    char fbuf[512];
    int i, json = 0, values;
    memset(&in, 0, sizeof in);
    in.language = "english";
    if (argc < 2 || (strcmp(argv[1], "roles") && strcmp(argv[1], "defs") && strcmp(argv[1], "values")))
        return usage();
    values = !strcmp(argv[1], "values");
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "--cwd")) in.cwd = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--archives")) in.mounts = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--language")) in.language = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--folders") && !folders) folders = argv[++i];
        else if (i + 1 < argc && !strcmp(argv[i], "--features") && !strcmp(argv[1], "defs")) in.features = argv[++i];
        else return usage();
    }
    if (!in.cwd || !in.mounts || json == values) return usage();
    if (folders && (strlen(folders) >= sizeof fbuf || !parse_folders(strcpy(fbuf, folders), &in))) {
        fprintf(stderr, "tasim_run: --folders %s: name=folder pairs of gui, gamedata, download, unitpics\n", folders);
        return 2;
    }
    if (values) return tasim_load_values(&in, stdout) == 0 ? 0 : 2;
    return !strcmp(argv[1], "roles") ? cmd_roles(&in) : cmd_defs(&in);
}
