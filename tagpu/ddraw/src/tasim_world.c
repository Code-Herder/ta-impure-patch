/* tasim_world.c -- the shared simulation core's world: units and features by id, a unit's entry into the world and
   its leaving, the engine's site test, a structure's centre, the scratch arrays' growth, and a game's kill with its
   wreck. research/notes/impai/shared-core.md, tasim_world. Contract: tasim.h.

   Each unit lives on the heap, so a pointer to it holds while the list grows, and the list is in creation order,
   the order every rule visits units in. The units, the features and every array grown here are the WIKISIM's, which
   its caller owns; no globals. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim.h"

/* a unit joins the world, appended in the room tasim_new_unit made for it. With the sidebar it carries
   its page state: 0x485CD5 creates a unit whose type has numbered pages with bit 22 set and page
   1; any other unit has neither. */
static WIKIUNIT* enter(WIKISIM* s, WIKIUNIT* u)
{
    if (s->lay) {
        const int n = tasim_npages(s, u->type);
        u->bp = n >= 1;
        u->page = n >= 1 ? 1 : 0;
    }
    s->unit[s->nunit++] = u;
    tasim_spawn(s, u);
    return u;
}

WIKIUNIT* tasim_new_unit(WIKISIM* s, int type, int owner, double built, const char* id, const char* label)
{
    WIKIUNIT* u;
    if (s->nunit == s->cunit) {
        int c = s->cunit ? s->cunit * 2 : 32;
        WIKIUNIT** p = (WIKIUNIT**)realloc(s->unit, (size_t)c * sizeof *p);
        if (!p) { s->oom = 1; return NULL; }
        s->unit = p;
        s->cunit = c;
    }
    if (!(u = (WIKIUNIT*)calloc(1, sizeof *u))) { s->oom = 1; return NULL; }
    if (id && *id) snprintf(u->id, sizeof u->id, "%s", id);
    else snprintf(u->id, sizeof u->id, "u%d", s->next_id++);
    u->type = type;
    u->owner = owner;
    u->built = built;
    u->label = label ? label : tasim_def(s, type)->name;
    return u;
}

/* a structure at its site. It faces (0, 1), toward the viewer, whatever its scene gives: TA's
   default heading 0x8000 is the centre of the spawn's 0x8000 - BuildAngle/2 + rand(BuildAngle)
   (0x485A40, tagpu_native.c ghost_one), and the simulation has no randomness. */
WIKIUNIT* tasim_place(WIKISIM* s, WIKIUNIT* u, int ox, int oz)
{
    u->ox = ox;
    u->oz = oz;
    u->x = (ox + tasim_fpx(s, u->type) / 2.0) * CELL;
    u->z = (oz + tasim_fpz(s, u->type) / 2.0) * CELL;
    u->hx = 0;
    u->hz = 1;
    return enter(s, u);
}

/* (hx, hz): the scene's facing, already a unit vector (tagpu_wikidata.c) */
WIKIUNIT* tasim_add(WIKISIM* s, int type, int owner, double x, double z, double built, const char* id, const char* label,
                    double hx, double hz)
{
    WIKIUNIT* u = tasim_new_unit(s, type, owner, built, id, label);
    if (!u) return NULL;
    if (tasim_mobile(s, type)) {
        u->x = x;
        u->z = z;
        u->hx = hx;
        u->hz = hz;
        return enter(s, u);
    }
    return tasim_place(s, u, tasim_bound_cell(tasim_js_round(x / CELL - tasim_fpx(s, type) / 2.0)), tasim_bound_cell(tasim_js_round(z / CELL - tasim_fpz(s, type) / 2.0)));
}

WIKIUNIT* tasim_by_id(const WIKISIM* s, const char* id)
{
    int i;
    for (i = 0; i < s->nunit; i++)
        if (!strcmp(s->unit[i]->id, id)) return s->unit[i];
    return NULL;
}

/* the first structure of that type on that site; a mobile unit has no site (sim.js: its ox is undefined) */
WIKIUNIT* tasim_structure_at(const WIKISIM* s, int t, int ox, int oz)
{
    int i;
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        if (u->type == t && !tasim_mobile(s, u->type) && u->ox == ox && u->oz == oz) return u;
    }
    return NULL;
}

/* ---- THE FEATURES (research/notes/ingame-wiki-reclaim-plan.md), sim.js's: the entry's `features`
   table names each by its TDF section, and a scene places one at its anchor cell, the footprint's
   top-left, as the TNT and LoadFeature 0x4224B0 place a map's. A blocking one stands in the way as
   a structure does; a reclaimable one is a reclaim's target, and goes when its reclaim pays out
   (FeatureDie at 0x423965). ---- */

BOX tasim_feat_box(const WIKIFEAT* f)
{
    BOX b;
    b.ox = f->ox;
    b.oz = f->oz;
    b.fx = f->fx;
    b.fz = f->fz;
    return b;
}

WIKIFEAT* tasim_feat_by_id(const WIKISIM* s, const char* id)
{
    int i;
    for (i = 0; i < s->nfeat; i++)
        if (!strcmp(s->feat[i].id, id)) return &s->feat[i];
    return NULL;
}

/* the feature whose footprint holds the cell under (x, z), as 0x4815A0 finds one at a position
   through its anchor */
WIKIFEAT* tasim_feat_at(const WIKISIM* s, double x, double z)
{
    const int cx = tasim_cell_of(x), cz = tasim_cell_of(z);
    int i;
    for (i = 0; i < s->nfeat; i++) {
        WIKIFEAT* f = &s->feat[i];
        if (cx >= f->ox && cx < f->ox + f->fx && cz >= f->oz && cz < f->oz + f->fz) return f;
    }
    return NULL;
}

/* the feature leaves the list and its anchor cell is gone, which the game look's image no longer
   draws. THE BOUND: gone holds WIKI_FEATS_MAX cells and each feature leaves once. */
void tasim_feat_remove(WIKISIM* s, WIKIFEAT* f)
{
    const int k = (int)(f - s->feat);
    if (s->ngone < WIKI_FEATS_MAX) {
        s->gone[2 * s->ngone] = f->ox;
        s->gone[2 * s->ngone + 1] = f->oz;
        s->ngone++;
    }
    memmove(&s->feat[k], &s->feat[k + 1], (size_t)(s->nfeat - k - 1) * sizeof *s->feat);
    s->nfeat--;
}

/* THE ENGINE'S SITE TEST (0x47D2E0), simplified as sim.js simplifies it: the footprint inside the
   map's placeable interior, cells 1 to w - 2 (tagpu_resource.c site_known's bound), clear of every
   structure but `ignore` (an upgrade's old building), and for a geothermal plant a vent under one
   of its G cells: 0x47D2E0 refuses a yard holding G (0x47D80F) unless some G cell stands on a
   feature whose FeatureDef +0xFE has bit 0x20 (0x47D684..0x47D711, 0x47D75B..0x47D769),
   DISASSEMBLED; a vent under any other cell of the yard counts for nothing. Mobile units never
   block. */
int tasim_interior(const WIKISIM* s, int t, int ox, int oz)
{
    return ox >= 1 && oz >= 1 && ox + tasim_fpx(s, t) < s->W && oz + tasim_fpz(s, t) < s->H;
}

/* vent v lies under a G cell of type t's footprint at (ox, oz), sim.js's ventsUnderG */
int tasim_vent_under_g(const WIKISIM* s, const PlaceCell* v, int t, int ox, int oz)
{
    return v->cx >= ox && v->cx < ox + tasim_fpx(s, t) && v->cz >= oz && v->cz < oz + tasim_fpz(s, t) && tasim_g_cell(s, t, v->cx - ox, v->cz - oz);
}

int tasim_site_ok(const WIKISIM* s, int t, int ox, int oz, const WIKIUNIT* ignore)
{
    BOX b = tasim_box(s, t, ox, oz);
    int i;
    if (!tasim_interior(s, t, ox, oz)) return 0;
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        if (!tasim_mobile(s, u->type) && u != ignore && tasim_overlap(b, tasim_box(s, u->type, u->ox, u->oz))) return 0;
    }
    /* a blocking feature in the yard refuses it [INFERRED: the site test 0x47D2E0's feature cells;
       no entry builds over one] */
    for (i = 0; i < s->nfeat; i++)
        if (s->feat[i].blocking && tasim_overlap(b, tasim_feat_box(&s->feat[i]))) return 0;
    if (tasim_kind(s, t) == KIND_GEO) {
        for (i = 0; i < s->nvent && !tasim_vent_under_g(s, &s->vent[i], t, ox, oz); i++) {}
        if (i == s->nvent) return 0;
    }
    return 1;
}

XZ tasim_centre(const WIKISIM* s, int t, int ox, int oz)
{
    XZ p;
    p.x = (ox + tasim_fpx(s, t) / 2.0) * CELL;
    p.z = (oz + tasim_fpz(s, t) / 2.0) * CELL;
    return p;
}

/* the unit leaves the list and its memory goes; the sidebar's shown unit goes with it, which
   tagpu_wikisim.c's route() would make so on this tick's end anyway */
void tasim_remove_unit(WIKISIM* s, WIKIUNIT* u)
{
    int i, k = 0;
    if (s->shown == u) s->shown = NULL;
    for (i = 0; i < s->nunit; i++)
        if (s->unit[i] != u) s->unit[k++] = s->unit[i];
    s->nunit = k;
    free(u->order);
    free(u);
}

/* THE WRECK of a finished unit killed, sim.js's `corpse`, which gives the engine's placement rules
   (0x486360, 0x423C50) with their addresses. BOUND: a game's features array holds WIKI_FEATS_MAX
   (scene_feats), and the features standing plus those gone are the scene's plus one per wreck made,
   at most one a kill: the loader holds the scene's features and the game script's kills to
   WIKI_FEATS_MAX, and a fork refuses a part whose kills would pass it (tagpu_wikisim_fork). So
   s->nfeat never passes the capacity, and `gone` (WIKI_FEATS_MAX cells) takes each feature at most
   once. The row is the type's corpse, an index of the bound features table the binder or the
   loader checked. */
static void corpse(WIKISIM* s, const WIKIUNIT* u)
{
    const WIKIDEF* d = tasim_def(s, u->type);
    const WIKIFEATDEF* fd;
    WIKIFEAT* f;
    int ox, oz, x, z, j;
    if (d->corpse < 0 || d->corpse >= s->entry->nfeat) return;
    fd = &s->entry->feat[d->corpse];
    ox = tasim_mobile(s, u->type) ? tasim_foot_cell(u->x, d->fx) : u->ox;
    oz = tasim_mobile(s, u->type) ? tasim_foot_cell(u->z, d->fz) : u->oz;
    if (ox < 0 || oz < 0 || ox >= s->W || oz >= s->H || ox + fd->fx > s->W || oz + fd->fz > s->H) return;
    for (z = oz; z < oz + fd->fz; z++)
        for (x = ox; x < ox + fd->fx; x++)
            for (j = 0; j < s->nfeat; j++) {
                const WIKIFEAT* g = &s->feat[j];
                if (x >= g->ox && x < g->ox + g->fx && z >= g->oz && z < g->oz + g->fz) {
                    tasim_feat_remove(s, &s->feat[j]);
                    break;
                }
            }
    f = &s->feat[s->nfeat++];
    memset(f, 0, sizeof *f);
    snprintf(f->id, sizeof f->id, "%s", u->id);
    f->def = d->corpse;
    f->ox = ox;
    f->oz = oz;
    f->fx = fd->fx;
    f->fz = fd->fz;
    f->x = (ox + fd->fx / 2.0) * CELL;
    f->z = (oz + fd->fz / 2.0) * CELL;
    f->metal = fd->metal;
    f->energy = fd->energy;
    f->height = fd->height;
    f->blocking = fd->blocking;
    f->reclaimable = fd->reclaimable;
}

/* A KILL, the game script's {at, kill}, fed before its tick's tasim_drop_work, COB slice and tasim_advance:
   sim.js's `kill`, which gives the engine's facts with their addresses. Its script goes first,
   through the host's own removal; a finished unit leaves its wreck and makes its death sound; a
   commander counts as its seat's loss; a builder whose running build is the unfinished structure
   killed loses that order and says cant. THE KILLS AND THE LOSSES (PD6): the victim's seat loses it
   whatever killed it (+0xFE, ungated at 0x4868C1); `by`, a seat of the game other than the
   victim's, counts a kill when the victim was finished (+0xFC, behind a killer, +0x104 == 0 and
   another player; engine/memory.md); a `by` that is no seat counts nothing. The death is recorded
   for the replay's death vision at the unit's place (WIKIDEATH) when the engine's temporary sight
   record add would keep it.
   THE DEATH VISION'S BOUND (DISASSEMBLED 2026-11-02, 0x482910..0x482935, 0x482130..0x482260,
   0x4866E0..0x486748): the records are one list for the whole game, main+0x1427B, 36 bytes each,
   its count main+0x14277. The add 0x482910 refuses the new record, touching nothing, when the count
   is already 20 (`cmp eax,0x14; jge` at 0x48292C): the records live stay and the newest death
   lights nothing. Its one caller, the destructor at 0x486748, adds only for a unit whose owner's
   +0x146 is the viewed byte main+0x2A43 (0x486716..0x48672A), and a replay's Sight change drops
   every record and makes the new seat's again (tagpu_replay.c sight_change), so the list holds the
   viewed seat's deaths alone: the bound is the seat's. The expiry 0x482130, after each step's tick
   loop, removes a record whose tick + 60 is below GameTime, so at an add on tick t a death of tick
   d is live while d + WIKI_SIGHT_SPAN >= t - 1, a step being one tick. So a kill of a seat that
   already has WIKI_PLAYERS_DEATHS deaths live is not recorded, whatever the script or a fork part
   kills; a players entry's data rule keeps its own script inside that bound (tagpu_wikidata.h).
   THE INVARIANT, an ordering: between two ticks the only references to a WIKIUNIT are s->unit[] and
   s->shown -- tasim_advance clears every job pointer by the tick's end, and orders, the spray and the
   sounds name units by id -- and a kill runs in feed, between advances, so tasim_remove_unit clearing
   both leaves nothing that can read the freed unit. The COB handle goes before the unit, and the
   host never gives a handle twice. */
void tasim_kill(WIKISIM* s, const char* id, int by)
{
    WIKIUNIT* u = id ? tasim_by_id(s, id) : NULL;
    int i, live;
    if (!u) {
        s->bad_kill = 1;
        return;
    }
    s->nkill++;
    if (u->owner >= 0 && u->owner < s->nseat) s->econ[u->owner].losses++;
    if (by >= 0 && by < s->nseat && by != u->owner && u->built >= 1) s->econ[by].kills++;
    for (i = live = 0; i < s->ndeath; i++)
        live += s->death[i].owner == u->owner && s->death[i].tick + WIKI_SIGHT_SPAN + 1 >= s->feeding;
    if (s->ndeath < WIKI_FEATS_MAX && live < WIKI_PLAYERS_DEATHS) {
        WIKIDEATH* d = &s->death[s->ndeath++];
        d->tick = s->feeding;
        d->owner = u->owner;
        d->type = u->type;
        d->x = u->x;
        d->z = u->z;
    }
    if (s->cob && u->cob) tagpu_wikicob_remove(s->cob, u->cob);
    if (u->built >= 1) {
        corpse(s, u);
        tasim_death_sound(s, u);
    }
    if (u->owner >= 0 && u->owner < s->nseat && tasim_def(s, u->type)->cast == WIKI_CAST_COMMANDER) {
        s->econ[u->owner].lost++;
        s->econ[u->owner].cmd_lost = 1;
    }
    if (!tasim_mobile(s, u->type) && u->built < 1)
        for (i = 0; i < s->nunit; i++) {
            WIKIUNIT* v = s->unit[i];
            const WIKIORDER* o = v->norder ? &v->order[0] : NULL;
            if (v != u && o && o->k == WIKI_ORDER_BUILD && o->type == u->type && o->ox == u->ox && o->oz == u->oz) {
                tasim_shift_order(v);
                tasim_notice(s, v, WIKI_SND_CANT);
            }
        }
    tasim_remove_unit(s, u);
}

int tasim_grow(WIKISIM* s, void** p, int* cap, int need, size_t size)
{
    void* q;
    int c;
    if (need <= *cap) return 1;
    for (c = *cap ? *cap : 16; c < need; c *= 2) {}
    if (!(q = realloc(*p, (size_t)c * size))) { s->oom = 1; return 0; }
    *p = q;
    *cap = c;
    return 1;
}
