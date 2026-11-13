/* tasim_wire_test.c -- the scenario wire parser's test (src/tasim_wire.h), driven by tests/tasim_wire_test.py,
   which builds it natively, under the sanitizers and with the DLL's i686 compiler under Wine.

     tasim_wire_test < LIST    parses each file LIST names, one path a line, with the DLL's caps and prints its
                               canonical dump: a `==` line naming the file, then every field the parse wrote, in
                               the order the struct holds them, floats as their bits. The driver requires every
                               build to print the same bytes, and each malformed wire to fail with the message it
                               names. The list comes on stdin because Windows bounds a command line to 32767
                               characters, which a few hundred paths pass. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_wire.h"

/* the caps the DLL passes (tagpu_scenario.c): ten players at the raised limit's 1500 units each, an order for
   every one of them, and 1024 features */
#define CAP_UNITS  15000
#define CAP_FEATS  1024
#define CAP_ORDERS 15000

static TASIM_WIRE_UNIT  units[CAP_UNITS];
static TASIM_WIRE_FEAT  feats[CAP_FEATS];
static TASIM_WIRE_ORDER orders[CAP_ORDERS];

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static void dump(const char* name, const TASIM_WIRE* w, const TASIM_WIRE_ERRS* e, int ok)
{
    int i;

    printf("== %s\nok %d errors %d dropped %d\n", name, ok, e->n, e->dropped);
    for (i = 0; i < e->n; i++)
        printf("err %s\n", e->msg[i]);
    printf("v1 seed %d clear %d abort %d limit %d\n", w->seed, w->clear, w->abort, w->limit);
    printf("map [%s]\n", w->map);
    printf("sw set %#x clr %#x\n", w->sw_set, w->sw_clr);
    printf("clearfeat %d objective %d\n", w->clearfeat, w->objective);
    printf("cam %d %d %d pin %d\n", w->cam_kind, w->cam_a, w->cam_b, w->cam_pin);
    for (i = 0; i < w->npl; i++)
    {
        const TASIM_WIRE_PLAYER* p = &w->pl[i];
        printf("player %d metal %d energy %d aioff %d hasstore %d mstore %08x estore %08x\n", p->slot, p->metal,
               p->energy, p->aioff, p->hasstore, (unsigned)bits(p->mstore), (unsigned)bits(p->estore));
    }
    for (i = 0; i < w->nunits; i++)
    {
        const TASIM_WIRE_UNIT* u = &w->units[i];
        printf("unit %d [%s] %d %d %d %d %d %d %d %d %d %d %d\n", i, u->type, u->owner, u->x, u->y, u->height,
               u->facing, u->health, u->stance, u->nano, u->kills, u->fire, u->stock);
    }
    for (i = 0; i < w->nfeats; i++)
    {
        const TASIM_WIRE_FEAT* f = &w->feats[i];
        printf("feat %d [%s] %d %d %d %d\n", i, f->type, f->x, f->y, f->height, f->facing);
    }
    for (i = 0; i < w->norders; i++)
    {
        const TASIM_WIRE_ORDER* o = &w->orders[i];
        printf("order %d subj %d %d [%s] cmd %d kind %d %d %d tlive %d [%s] after %d\n", i, o->subj, o->uord,
               o->sexp, o->cmd, o->kind, o->a, o->b, o->tlive, o->texp, o->after);
    }
}

/* the whole file, terminated; NULL when it cannot be read */
static char* slurp(const char* path)
{
    FILE* f = fopen(path, "rb");
    char* buf;
    long  n;

    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) ||
        !(buf = (char*)malloc((size_t)n + 1)))
    {
        fclose(f);
        return 0;
    }
    n = (long)fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

int main(void)
{
    static TASIM_WIRE w;
    static TASIM_WIRE_ERRS e;
    char path[4096];
    int  bad = 0;

    w.units = units;
    w.units_cap = CAP_UNITS;
    w.feats = feats;
    w.feats_cap = CAP_FEATS;
    w.orders = orders;
    w.orders_cap = CAP_ORDERS;

    while (fgets(path, sizeof path, stdin))
    {
        const char* name;
        char*       text;
        int         ok;

        path[strcspn(path, "\r\n")] = 0;
        if (!path[0])
            continue;
        name = strrchr(path, '/');
        name = name ? name + 1 : path;
        text = slurp(path);
        if (!text)
        {
            printf("== %s\ncannot read\n", name);
            bad = 1;
            continue;
        }
        tasim_wire_errs_reset(&e);
        ok = tasim_wire_parse(&w, text, &e);
        dump(name, &w, &e, ok);
        free(text);
    }
    return bad;
}
