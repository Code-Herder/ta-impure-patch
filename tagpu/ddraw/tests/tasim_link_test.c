/* tasim_link_test.c -- the shared simulation core without the in-game wiki's front end: a TASIM made by hand,
   played for 300 ticks with no tagpu_wikisim.o in the link (tests/tasim_link_test.py builds it), so the boundary
   tasim_state.h states -- the core reaches its owner only through the unit hook -- is one the linker checks.

   One builder walks to a point, then builds a structure, which finishes; a kill then removes the structure. The
   test holds the walk, the build and the hook's events to what the core's rules give, and prints the core's state
   hash (tasim_hash) at the end, which the builds must print alike. Exit 0 when every claim holds, 1 when one does
   not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim.h"
#include "tasim_hash.h"
#ifdef _WIN32
#include <windows.h>
/* tagpu_pak.c, which the entry loader links, names the DLL's module handle; this test never reads an archive */
HMODULE g_ddraw_module;
#endif

#define TICKS 300

static int fails;

static void claim(int ok, const char* what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

/* the hook's events, counted by kind, and the last unit each named */
typedef struct { int n[3]; char last[3][WIKI_ID_MAX]; } EVENTS;

static void hook(void* ctx, WIKIUNIT* u, int ev)
{
    EVENTS* e = (EVENTS*)ctx;
    e->n[ev]++;
    snprintf(e->last[ev], sizeof e->last[ev], "%s", u->id);
}

/* one tick as the wiki's step runs the core's part of it, with no script, no economy and no sound source */
static void step(TASIM* s, const TASIM_DEFS* defs)
{
    const int t = s->tick + 1;
    s->now = t;
    tasim_drop_work(s);
    tasim_advance(s, defs);
    tasim_spray_tick(s, t);
    tasim_sound_tick(s);
    s->tick = t;
}

int main(void)
{
    WIKIDEF def[2];
    WIKIENTRY entry;
    TASIM_DEFS defs;
    TASIM s;
    EVENTS ev;
    WIKIUNIT* u;
    WIKIORDER o;
    int i;

    memset(def, 0, sizeof def);
    snprintf(def[0].name, sizeof def[0].name, "BUILDER");
    def[0].cast = -1;
    def[0].fx = def[0].fz = 2;
    def[0].max_velocity = 1.5;
    def[0].worker_time = 300;
    def[0].build_distance = 64;
    def[0].corpse = -1;
    def[0].builds = 2;
    snprintf(def[1].name, sizeof def[1].name, "STRUCTURE");
    def[1].cast = -1;
    def[1].fx = def[1].fz = 3;
    def[1].build_time = 300;
    def[1].corpse = -1;
    memset(&entry, 0, sizeof entry);
    entry.id = "link";
    entry.ndef = 2;
    entry.def = def;
    entry.pace = 1;
    defs.entry = &entry;

    memset(&s, 0, sizeof s);
    memset(&ev, 0, sizeof ev);
    s.hook = hook;
    s.hook_ctx = &ev;
    s.W = s.H = 32;
    s.PACE = entry.pace;
    s.next_id = 1;
    if (!tasim_map_alloc(&s)) {
        printf("FAIL out of memory\n");
        return 1;
    }
    for (i = 0; i < s.W * s.H; i++) s.metal[i] = 0;

    u = tasim_add(&s, &defs, 0, 0, 100, 100, 1, "b1", NULL, 0, 1);
    claim(u != NULL && s.nunit == 1, "the builder enters");
    claim(ev.n[TASIM_UNIT_ENTER] == 1 && !strcmp(ev.last[TASIM_UNIT_ENTER], "b1"), "the hook hears it enter");
    memset(&o, 0, sizeof o);
    o.k = WIKI_ORDER_MOVE;
    o.x = 260;
    o.z = 200;
    claim(tasim_push_order(&s, u, &o), "the move goes in");
    o = tasim_order_build(1, 20, 6);
    claim(tasim_push_order(&s, u, &o), "the build goes in");

    for (i = 0; i < TICKS; i++) step(&s, &defs);

    claim(s.tick == TICKS && !s.oom, "300 ticks, no allocation failed");
    claim(s.nunit == 2 && u->norder == 0, "both orders are done and the structure stands");
    claim(s.nunit == 2 && s.unit[1]->type == 1 && s.unit[1]->built >= 1, "the structure is finished");
    claim(ev.n[TASIM_UNIT_ENTER] == 2 && ev.n[TASIM_UNIT_BUILT] == 1 && ev.n[TASIM_UNIT_LEAVE] == 0,
          "the hook hears the structure enter and finish");

    tasim_kill(&s, &defs, s.unit[1]->id, -1, s.tick);
    claim(s.nunit == 1 && ev.n[TASIM_UNIT_LEAVE] == 1 && !strcmp(ev.last[TASIM_UNIT_LEAVE], "u1"),
          "a kill removes the structure, and the hook hears it leave");
    claim(s.nkill == 1 && !s.bad_kill, "the kill is counted");

    printf("x %.17g z %.17g\n", u->x, u->z);
    printf("hash %016llx\n", (unsigned long long)tasim_hash(&s, &defs));
    tasim_free(&s);
    printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
