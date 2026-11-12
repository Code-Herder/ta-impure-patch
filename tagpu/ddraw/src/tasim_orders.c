/* tasim_orders.c -- the shared simulation core's orders: a unit's bounded order list (THE ONE APPEND, splice,
   shift), the orders a click makes and whether a build order has started. research/notes/impai/shared-core.md,
   tasim_orders. Contract: tasim.h.

   BOUNDED BY CONSTRUCTION: every append goes through tasim_push_order, which refuses past PLACE_LIST_CAP. A list is
   its unit's heap block in the WIKISIM its caller owns; no globals. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim.h"

/* THE ONE APPEND to a unit's orders, so a list never holds more than PLACE_LIST_CAP: an order
   past it is dropped. Every order a player's clicks add comes
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
