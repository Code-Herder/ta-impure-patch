/* tasim_sight.c -- the shared simulation core's sight: what a seat of a game sees, its lit grid and its radar
   contacts, the engine's line of sight and radar pass reduced to the stage. research/notes/impai/shared-core.md,
   tasim_sight. Contract: tagpu_wikisim.h (tagpu_wikisim_sight_lit, tagpu_wikisim_contacts) and tasim.h.

   PURE: both read a view (WIKIVIEW) and the layout's sight table and write only the caller's array; no state, no
   globals. sim.js sightLit and sightContacts are their twins. */
#include <math.h>
#include <string.h>
#include "tasim.h"

/* ---- sight: what a seat sees in a game's view, for the replay's Sight (tagpu_wikireplay.c) ----

   THE LIT GRID, the engine's line of sight reduced to the stage (research/notes/line-of-sight.md;
   DISASSEMBLED, the scout's P1): every live unit -- a nanoframe from its create (0x482AC0 at
   0x486178) -- stamps its own owner's grid alone (PD7: an ally shares no sight), one byte a 32-unit
   block on the (w >> 1) x (h >> 1) grid (0x482F1B..0x482F45 halves the map's cells truncating).
   The emit 0x4827B0 raises the unit's altitude to at least the sea level + 1 (0x48280B..0x48281B);
   the stage is a world whose ground and sea level are both 0 (PD16: every stage unit stands at
   altitude 0, and the sea byte main+0x1427F cannot be below the ground), so the altitude is 1 and
   the emitter 0x4825B0 takes the height h = min(255, modelHeight + 1), the column floor(x) >> 5
   and the row (floor(z) - (h >> 1)) >> 5, arithmetic shifts (0x4825D4..0x48261C). A column or row
   off the grid -- the unsigned bound 0x48265B..0x482669 -- stamps nothing at all (0x4826A9..
   0x4826AC), so a unit within h / 2 of the north edge lights nothing, as in the game. Else the
   stamp 0x482270 adds one to the unit's own block (0x48239E) and one to the block under each point
   of each ray of TABLE(idx) (0x48247F) whose height test passes; on flat ground every point passes
   (MEASURED M0-P1, no block of the set unlit), so the table's count grid -- the loader's four images
   (x, -y), (y, x), (-x, y), (-y, -x) of each line, no point merged (tagpu_wikidata.h WIKISIGHT) --
   loses nothing: each offset on the grid adds its count. The counters are bytes and wrap at 256
   as the engine's inc does; only lit or unlit is read, and a block wraps to 0 only under 256
   stamps, far past the players fixture's game (15 at most). MEASURED M0-P1 (a
   139-high flat plate, True LOS): ARMCOM at idx 8 lights exactly its table's 212 offsets and its
   own block, 213, centred on row (z - h/2) >> 5 with h = def+0x170 + altitude (178 -> row 183 for
   z 5975). THE DEATH VISION: the destructor makes a temporary sight record (0x482910 at 0x486748)
   for each unit of the viewed player killed, at its place with its sight and height, stamped
   through the same emitter; the expiry 0x482130 removes it once its tick + 60 is below GameTime
   (0x482153..0x48215E), so the last tick drawn with it is tick + WIKI_SIGHT_SPAN, a step being one
   tick. A death of `seat` with g <= tick + WIKI_SIGHT_SPAN stamps as its unit did; the list holds
   only the deaths the engine's add keeps, at most WIKI_PLAYERS_DEATHS live (kill states the
   rule). BOUND: every index is checked against the grid,
   the view's type count and the table's cells before it is read. */
static int sight_asr5(int v) { return v >= 0 ? v / 32 : -((-v + 31) / 32); }

static void sight_stamp(const WIKISIGHT* t, const WIKIVTYPE* k, double x, double z, int gw, int gh, uint8_t* out)
{
    const int h = k->model_height + 1 < 255 ? k->model_height + 1 : 255;
    const int c = sight_asr5((int)floor(x)), r = sight_asr5((int)floor(z) - (h >> 1));
    const int q = tagpu_wikidata_sight_table(t, k->sight_distance);
    int i;
    if (c < 0 || c >= gw || r < 0 || r >= gh) return;
    out[r * gw + c] = (uint8_t)(out[r * gw + c] + 1);
    if (q < 0) return;
    for (i = t->first[q]; i < t->first[q + 1] && i < WIKI_SIGHT_CELLS; i++) {
        const int cc = c + t->cell[i].dc, rr = r + t->cell[i].dr;
        if (cc >= 0 && cc < gw && rr >= 0 && rr < gh) out[rr * gw + cc] = (uint8_t)(out[rr * gw + cc] + t->cell[i].n);
    }
}

int tagpu_wikisim_sight_lit(const WIKIVIEW* v, const WIKISIGHT* t, int seat, int g, uint8_t* out)
{
    const int gw = v->w >> 1, gh = v->h >> 1;
    int i;
    if (gw <= 0 || gh <= 0 || gw * gh > WIKI_SIGHT_GRID) return 0;
    if (!t || !t->numtables) {
        memset(out, 1, (size_t)gw * gh);
        return gw * gh;
    }
    memset(out, 0, (size_t)gw * gh);
    if (!v->game) return gw * gh;
    for (i = 0; i < v->nunit; i++)
        if (v->unit[i].owner == seat && v->unit[i].type >= 0 && v->unit[i].type < v->ntype)
            sight_stamp(t, &v->type[v->unit[i].type], v->unit[i].x, v->unit[i].z, gw, gh, out);
    for (i = 0; i < v->ndeath; i++) {
        const WIKIDEATH* d = &v->death[i];
        if (d->owner == seat && g <= d->tick + WIKI_SIGHT_SPAN && d->type >= 0 && d->type < v->ntype)
            sight_stamp(t, &v->type[d->type], d->x, d->z, gw, gh, out);
    }
    return gw * gh;
}

/* THE CONTACTS (OQ-C A; DISASSEMBLED, the scout's P2.2 and P3): the radar pass 0x467440 runs for
   the viewed record alone. Its loop 2 walks each of the viewer's live, not dying, activated
   (+0x10E bit 0) units with a radar +0x204 or sonar +0x206: a target of another owner at a
   horizontal centre distance d gets bit 8 when its y + def+0x16E >= sea << 16 and d^2 < (radar +
   2 y_r)^2, y_r the radar unit's altitude, and bit 9 when its y <= sea << 16 and d^2 < sonar^2
   (0x467840..0x46793D), d^2 = (dx dx >> 32) + (dz dz >> 32) on the 16.16 differences, 64-bit
   (0x47E995..0x47E9EF). On the stage every unit stands at altitude 0 = the sea level (PD16), so
   both tests hold at once and y_r is 0: a unit is a contact when d^2 < max(radar, sonar)^2 of a
   live, finished, `activates` unit of the seat with either above 0 (a finished ActivateWhenBuilt
   unit is activated, 0x486135..0x486148, and the wiki has no on/off). Loop 5 sets bit 8 on any unit
   over a lit block of the viewer's counters, the block of (x >> 5, (z - y/2) >> 5), unsheared at
   altitude 0 (0x46770D..0x467814). The seat's own units are dotted by ownership, not by a bit
   (0x466E64..0x466E7D), so they are 0 here. Jamming (loop 3, bit 10) and stealth (def+0x241 bit 8)
   are left out: no wiki cast has either. */
static int64_t sight_fix(double x) { return (int64_t)floor(x * 65536.0); }

void tagpu_wikisim_contacts(const WIKIVIEW* v, const uint8_t* lit, int seat, uint8_t* out)
{
    const int gw = v->w >> 1, gh = v->h >> 1;
    int i, j;
    for (i = 0; i < v->nunit; i++) {
        const WIKIVUNIT* u = &v->unit[i];
        const int c = sight_asr5((int)floor(u->x)), r = sight_asr5((int)floor(u->z));
        int64_t ux, uz;
        out[i] = 0;
        if (u->owner == seat || !v->game) continue;
        if (lit && c >= 0 && c < gw && r >= 0 && r < gh && gw * gh <= WIKI_SIGHT_GRID && lit[r * gw + c]) {
            out[i] = 1;
            continue;
        }
        ux = sight_fix(u->x);
        uz = sight_fix(u->z);
        for (j = 0; j < v->nunit && !out[i]; j++) {
            const WIKIVUNIT* q = &v->unit[j];
            const WIKIVTYPE* k = q->type >= 0 && q->type < v->ntype ? &v->type[q->type] : NULL;
            const int64_t R = k ? (k->radar_distance > k->sonar_distance ? k->radar_distance : k->sonar_distance) : 0;
            int64_t dx, dz;
            if (q->owner != seat || !(q->built >= 1) || !k || !k->activates || R <= 0) continue;
            dx = ux - sight_fix(q->x);
            dz = uz - sight_fix(q->z);
            if (((dx * dx) >> 32) + ((dz * dz) >> 32) < R * R) out[i] = 1;
        }
    }
}
