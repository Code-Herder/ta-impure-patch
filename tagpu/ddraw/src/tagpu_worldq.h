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
#include "impai_fight.h"

/* the site test a placement is judged by: stdcall (def, cell, ignore, player record), `ret 0x10` */
typedef int (__stdcall* WqSiteFn)(const void* def, unsigned cell, unsigned ignore, const void* player);
/* the build menu builder type `builder_type` offers seat `seat`, from that seat's own side's pages, ascending,
   into the caller's buffer: the count, -1 when cap is short */
typedef int (*WqMenuFn)(int seat, unsigned builder_type, uint16_t* out, int cap);

/* One open view's context, on the caller's stack beside its WORLDQ. */
typedef struct WQ_ENGINE {
    char*     ta;                 /* main, read once                                              */
    char*     units; unsigned nslots;
    char*     defs;  unsigned ndefs;
    char*     grid;  int w, h, threshold;
    char*     rec;                /* the seat's player record, main+0x1B63 + 0x14B * seat         */
    unsigned  first, last;        /* the seat's unit block, as slots                              */
    int       seat;
    WqSiteFn  site;               /* 0x47D2E0, the build cursor's call target at 0x4198C6, or the
                                     seat's own thunk (WQ_SITE_SEAT)                               */
    WqMenuFn  menu;               /* the build menu (tagpu_resource.c), or NULL for none          */
    int       layout;             /* the view knows the map's layout (tagpu_worldq_open_layout)    */
    uint8_t   act[WQ_ACTS];       /* the action bytes, looked up on first use (`got`)             */
    uint8_t   got[WQ_ACTS];
} WQ_ENGINE;

/* WQ_SITE_ENGINE: the engine's site test 0x47D2E0, which Quick Build's fits asks.
   WQ_SITE_CURSOR: the target of the build cursor's own call at 0x4198C6 -- stock 0x47D2E0, the queued-site
   fix's build_site_test when tagpu_patches.c armed it, or a foreign patch's -- so a placement's site is
   judged exactly as a single click there would be.
   WQ_SITE_SEAT: 0x47D2E0 through ImpAI's thunk, whose return address the patch at 0x47D3D1 recognises, so the
   site is not refused for a centre the viewed player has not mapped, and the flag a unit on the footprint refuses
   on is the SEAT'S OWN sight (tagpu_impai.c, THE SEAT'S OWN SITE TEST); the open refuses it while the thunk is
   not in. */
enum { WQ_SITE_ENGINE, WQ_SITE_CURSOR, WQ_SITE_SEAT };

/* DLL attach, after tagpu_patches.c has written its sites and before tagpu_place_init: decodes the call at
   0x4198C6 once. Its target is fixed from then on: nothing writes that call after attach. */
void     tagpu_worldq_init(void);
/* the build cursor's site test decoded at attach, NULL when 0x4198C6 was not a call */
WqSiteFn tagpu_worldq_cursor_site(void);
/* DLL attach, by tagpu_impai_init once the thunk and its patch at 0x47D3D1 are both written: WQ_SITE_SEAT's
   target from then on. Nothing else writes it. */
void     tagpu_worldq_seat_site_set(WqSiteFn thunk);

/* The site test 0x47D2E0 given no player for a site of `type` at first cell (cx, cz): it then makes no mapped
   test and every unit on the footprint refuses (0x47D366; the flag [esp+0x10] stays 1 from 0x47D35C). 1 or 0, or
   -1 where the test given the seat's record refuses before its mapped read (the footprint off the map, the
   centre's block off the record's grid, 0x47D3B0..0x47D3BE) or the seat has no sight to read. *lit is the flag the
   test given the seat's record sets at the site's block, the seat's own sight there (tagpu_worldq.c). ImpAI's
   self-check holds the thunk's answers to both: the thunk's call differs from this one only in that flag, so
   with *lit 1 it answers as this does, and with *lit 0 it accepts wherever this accepts. */
int      tagpu_worldq_site_free(const WORLDQ* q, int type, int cx, int cz, int* lit);

/* the resource building a unit definition is, WQ_KIND_*: what WQ_DEF.kind answers, for a caller that holds
   the definition already (the build menu's log) */
int      tagpu_worldq_def_kind(const void* def);

/* Opens seat `seat`'s view into `out` over `ctx`, both the caller's for one call; 0 and nothing opened when
   any bound fails (in play, the map, the defs, the unit array and the seat's block; the seat below 10 and
   not a puppet seat) or `site` is WQ_SITE_CURSOR with no decoded target or WQ_SITE_SEAT with no thunk. */
int tagpu_worldq_open(WQ_ENGINE* ctx, WORLDQ* out, int seat, int site, WqMenuFn menu);
/* tagpu_worldq_open for a seat that KNOWS THE MAP'S LAYOUT, an ImpAI seat's: `explored` answers 1 for every cell
   of the map, so `metal` and `coverage` read the map itself, whatever the seat's bit of the explored map
   main+0x14273 holds. In a level that starts Mapped the bit is set in every word and never cleared in play
   (0x4816A0's fill; engine/map.md), so there the two opens answer alike. Nothing the seat SEES follows from it:
   unit_los, the radar pass and the site test's occupancy flag read the seat's sight as in any view. */
int tagpu_worldq_open_layout(WQ_ENGINE* ctx, WORLDQ* out, int seat, int site, WqMenuFn menu);
/* Opens the level's content view: ndefs, def, def_econ and map answer as a seat's view does; no unit is live
   in it (its slot count is 0), so every unit op answers nothing and site_ok, can, order and queue refuse.
   It needs the map and the defs only, so the level init opens it on the LOADER thread inside 0x4919C8,
   live or puppet (tagpu_impai.c, the type information). 0 when a bound fails. */
int tagpu_worldq_open_content(WQ_ENGINE* ctx, WORLDQ* out);

/* The line-of-sight rule `observe` applies, alone, for ImpAI's self-check against the engine's own
   0x465AC0: 1 when the seat's sight shows the live unit at `slot`, 0 when not, -1 for no live unit or no
   sight grid; *under is 1 for a unit whose top is below the sea, which the rule never shows. The caller
   tests ownership first, as 0x465AC0 does. */
int tagpu_worldq_unit_los(const WORLDQ* q, int slot, int* under);

/* The engine's pathing map of movement class `mclass` at cell (cx, cz) as it stands, every unit's stamp in it,
   with no seat: 0 blocked, off the map or for a class the level has not, 1 bad, 3 good. On the loader thread
   after the level's class maps are built (0x4918E3: from the level init on), or on the game thread. A seat's
   rule reads the passable op instead, the map's content alone (tagpu_worldq.c, THE CONTENT MAP). */
int tagpu_worldq_passable_at(unsigned mclass, int cx, int cz);
/* the level has movement class `mclass` (its record names one) */
int tagpu_worldq_class_exists(unsigned mclass);

/* THE ROLE TABLE'S ENGINE FILLER: the unit types the level loaded, as the role classifier's input
   (tasim_roles.h), from the def array, the weapon records and the category map the level load wrote. On the
   LOADER thread at the level init, inside PLAYERS 0x4919C8, after the unit-data load 0x42D2E0 (0x4918CA) has
   loaded every weapon, def and category: tagpu_impai.c is its one caller.
   tagpu_worldq_role_count: the level's UNITINFOCount, 0 when the def array or its count is not usable.
   tagpu_worldq_role_in: in[0..n) for the n role_count answered, each builder's build options what `options`
   answers for its type (the count, or -1 when cap is short), packed into opt[0..optcap), which must outlive
   `in`; 0, or -1 with nothing promised when a bound fails or opt is short. */
typedef int (*WqRoleOptFn)(unsigned type, uint16_t* out, int cap);
int tagpu_worldq_role_count(void);
int tagpu_worldq_role_in(TASIM_ROLE_IN* in, int n, WqRoleOptFn options, uint16_t* opt, unsigned optcap);

/* THE FIGHT CONTENT'S ENGINE FILLER: the fight predictor's tables (impai_fight.h) for the n unit types the
   role filler gave `rin` and the classifier made `rout` of, from the same def array and weapon records on the
   same thread at the same point (tagpu_impai.c is its one caller): t[0..n), the weapon rows the types name in
   w[0..*nw) (below wcap and the weapon array's slots; the caller zeroes w, and a row no type names stays
   zero), their resolved [DAMAGE] rows in d[0..*nd) (at most dcap). 0, or -1 with nothing promised when a
   bound fails or a table is full. */
int tagpu_worldq_fight_in(const TASIM_ROLE_IN* rin, const TASIM_ROLE_OUT* rout, int n, IMPAI_FTYPE* t,
                          IMPAI_FWEAPON* w, unsigned wcap, unsigned* nw, IMPAI_FDMG* d, unsigned dcap, unsigned* nd);

#endif
