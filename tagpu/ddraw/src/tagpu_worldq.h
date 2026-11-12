#ifndef TAGPU_WORLDQ_H
#define TAGPU_WORLDQ_H
/* tagpu_worldq.c -- the engine's implementation of worldq.h: one seat's view of the running game, every
   value read from engine memory bounded as data, every order issued through the engine's own roots. GAME
   THREAD ONLY: its callers are the in-play mouse handler 0x499200 (through tagpu_route.c's hooks), the window
   procedure's keys and the packet fill, and the engine owns everything it reads on that thread until the
   level's teardown, which runs there too. */
#include <stdint.h>
#include "worldq.h"
#include "tasim_roles.h"

/* the site test a placement is judged by: stdcall (def, cell, ignore, player record), `ret 0x10` */
typedef int (__stdcall* WqSiteFn)(const void* def, unsigned cell, unsigned ignore, const void* player);
/* a builder type's build menu, ascending, into the caller's buffer: the count, -1 when cap is short */
typedef int (*WqMenuFn)(unsigned builder_type, uint16_t* out, int cap);

/* One open view's context, on the caller's stack beside its WORLDQ. */
typedef struct WQ_ENGINE {
    char*     ta;                 /* main, read once                                              */
    char*     units; unsigned nslots;
    char*     defs;  unsigned ndefs;
    char*     grid;  int w, h, threshold;
    char*     rec;                /* the seat's player record, main+0x1B63 + 0x14B * seat         */
    unsigned  first, last;        /* the seat's unit block, as slots                              */
    int       seat;
    WqSiteFn  site;               /* 0x47D2E0, or the build cursor's call target at 0x4198C6      */
    WqMenuFn  menu;               /* the build menu (tagpu_resource.c), or NULL for none          */
    uint8_t   act[WQ_ACTS];       /* the action bytes, looked up on first use (`got`)             */
    uint8_t   got[WQ_ACTS];
} WQ_ENGINE;

/* WQ_SITE_ENGINE: the engine's site test 0x47D2E0, which Quick Build's fits asks.
   WQ_SITE_CURSOR: the target of the build cursor's own call at 0x4198C6 -- stock 0x47D2E0, the queued-site
   fix's build_site_test when tagpu_patches.c armed it, or a foreign patch's -- so a placement's site is
   judged exactly as a single click there would be. */
enum { WQ_SITE_ENGINE, WQ_SITE_CURSOR };

/* DLL attach, after tagpu_patches.c has written its sites and before tagpu_place_init: decodes the call at
   0x4198C6 once. Its target is fixed from then on: nothing writes that call after attach. */
void     tagpu_worldq_init(void);
/* the build cursor's site test decoded at attach, NULL when 0x4198C6 was not a call */
WqSiteFn tagpu_worldq_cursor_site(void);

/* the resource building a unit definition is, WQ_KIND_*: what WQ_DEF.kind answers, for a caller that holds
   the definition already (the build menu's log) */
int      tagpu_worldq_def_kind(const void* def);

/* Opens seat `seat`'s view into `out` over `ctx`, both the caller's for one call; 0 and nothing opened when
   any bound fails (in play, the map, the defs, the unit array and the seat's block; the seat below 10 and
   not a puppet seat) or `site` is WQ_SITE_CURSOR with no decoded target. */
int tagpu_worldq_open(WQ_ENGINE* ctx, WORLDQ* out, int seat, int site, WqMenuFn menu);

/* The line-of-sight rule `observe` applies, alone, for ImpAI's self-check against the engine's own
   0x465AC0: 1 when the seat's sight shows the live unit at `slot`, 0 when not, -1 for no live unit or no
   sight grid; *under is 1 for a unit whose top is below the sea, which the rule never shows. The caller
   tests ownership first, as 0x465AC0 does. */
int tagpu_worldq_unit_los(const WORLDQ* q, int slot, int* under);

/* The engine's pathing map of movement class `mclass` at cell (cx, cz), as the passable op answers it, with
   no seat: 0 blocked, off the map or for a class the level has not, 1 bad, 3 good. On the loader thread
   after the level's class maps are built (0x4918E3: from the level init on), or on the game thread. */
int tagpu_worldq_passable_at(unsigned mclass, int cx, int cz);
/* the level has movement class `mclass` (its record names one) */
int tagpu_worldq_class_exists(unsigned mclass);

/* THE ROLE TABLE'S ENGINE FILLER: the unit types the level loaded, as the role classifier's input
   (tasim_roles.h), from the def array, the weapon records and the category map the level load wrote. On the
   LOADER thread at the level init, inside PLAYERS 0x4919C8, after the unit-data load 0x42D2E0 (0x4918CA) has
   loaded every weapon, def and category: tagpu_impai.c is its one caller.
   tagpu_worldq_role_count: the level's UNITINFOCount, 0 when the def array or its count is not usable.
   tagpu_worldq_role_in: in[0..n) for the n role_count answered; 0, or -1 with nothing promised when a bound
   fails. */
int tagpu_worldq_role_count(void);
int tagpu_worldq_role_in(TASIM_ROLE_IN* in, int n);

#endif
