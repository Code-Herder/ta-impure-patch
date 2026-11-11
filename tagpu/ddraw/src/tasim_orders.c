/* tasim_orders.c -- the shared simulation core's orders: a unit's bounded order list (THE ONE APPEND, splice,
   shift), the orders a click makes, whether a build order has started, and arming a building (the seat's build-menu
   mode). research/notes/impai/shared-core.md, tasim_orders. Contract: tasim.h.

   BOUNDED BY CONSTRUCTION: every append goes through tasim_push_order, which refuses past PLACE_LIST_CAP. A list is
   its unit's heap block in the WIKISIM its caller owns, and the seat's mode is that WIKISIM's input state; no
   globals. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim.h"

/* THE ONE APPEND to a unit's orders, so a list never holds more than PLACE_LIST_CAP: an order
   past it is dropped, as sim.js's append drops it. Every order a player's clicks add comes
   through here, so they cannot grow a list until realloc fails, and take_offs' scratch
   (WIKISIM's q and by) holds any list. 1 when the order went in; 0 when the list was full or
   memory ran out, which sets `oom`. */
int tasim_push_order(WIKISIM* s, WIKIUNIT* u, const WIKIORDER* o)
{
    if (u->norder >= PLACE_LIST_CAP) return 0;
    if (u->norder == u->corder) {
        int c = u->corder ? u->corder * 2 : 8;
        WIKIORDER* p = (WIKIORDER*)realloc(u->order, (size_t)c * sizeof *p);
        if (!p) { s->oom = 1; return 0; }
        u->order = p;
        u->corder = c;
    }
    u->order[u->norder] = *o;
    u->order[u->norder++].serial = ++s->next_serial;
    return 1;
}

void tasim_splice_orders(WIKIUNIT* u, int i, int n)
{
    memmove(u->order + i, u->order + i + n, (size_t)(u->norder - i - n) * sizeof *u->order);
    u->norder -= n;
}

/* a build order has started once its building stands at its site */
int tasim_started(const WIKISIM* s, const WIKIORDER* o) { return tasim_structure_at(s, o->type, o->ox, o->oz) != NULL; }

/* leaving the build-menu mode ends the gesture and clears the Shift-placement bit with it */
void tasim_disarm(WIKISIM* s)
{
    ST(s).mode = -1;
    ST(s).has_gesture = 0;
    ST(s).shift_placed = 0;
    ST(s).rmode = 0;
    ST(s).has_rg = 0;
}

/* ARMING a building, the build button's branch of 0x41AA00, DISASSEMBLED: main+0x2CC3 = 0x0E
   (0x41AB89) and the type at main+0x2CC4 (0x41AB9C), and no write to main+0x2CC6. So a
   Shift-placement made earlier in this Shift hold keeps bit 5 (shift_placed), and releasing
   Shift disarms the building armed since as well: 0x4992D8..0x49930F tests bit 5, not the mode.
   tagpu_place.c leaves the bit too (it sets it only on a commit), and ends its gesture when the
   armed type changes (its evaluate, `s_gesture.type != c.type`), so a gesture outlives a re-arm
   of its own type only. With the sidebar off as well as on, as sim.js's `arm`. */
void tasim_arm(WIKISIM* s, int t)
{
    if (ST(s).mode != t) ST(s).has_gesture = 0;
    ST(s).mode = t;
    ST(s).has_drag = 0;
    ST(s).rmode = 0;
    ST(s).has_rg = 0;
}

/* a click's orders for one unit: they replace its list, or with Shift are appended to it -- all
   of them or, when they would take the list past PLACE_LIST_CAP, none, so an upgrade's reclaim
   never goes in without its build */
void tasim_assign(WIKISIM* s, WIKIUNIT* u, const WIKIORDER* os, int n, int shift)
{
    int i;
    if (!shift) u->norder = 0;
    else if (u->norder + n > PLACE_LIST_CAP) return;
    for (i = 0; i < n; i++) tasim_push_order(s, u, &os[i]);
}

WIKIORDER tasim_order_build(int t, int ox, int oz)
{
    WIKIORDER o;
    memset(&o, 0, sizeof o);
    o.k = WIKI_ORDER_BUILD;
    o.type = t;
    o.ox = ox;
    o.oz = oz;
    return o;
}

WIKIORDER tasim_order_target(int k, const char* id)
{
    WIKIORDER o;
    memset(&o, 0, sizeof o);
    o.k = k;
    snprintf(o.target, sizeof o.target, "%s", id);
    return o;
}

void tasim_shift_order(WIKIUNIT* u) { tasim_splice_orders(u, 0, 1); }
