/* tasim_hash.c -- the shared simulation core's state hash: one feeder per core item, each feeding exactly the bytes
   tagpu_wikisim_hash feeds for that item (tagpu_wikisim.h, the item list), and tasim_hash, the core's own value.
   research/notes/impai/shared-core.md, *The state hash*. Contract: tasim_hash.h.

   The feeders read the TASIM and the defs they are given and write only the running hash; no globals. */
#include "tasim.h"
#include "tasim_hash.h"

/* item 2: the map's size and SurfaceMetal, every cell's metal and then every cell's fog, row-major, the vents' centres */
void tasim_hash_map(uint64_t* h, const TASIM* s)
{
    const int cells = s->W * s->H;
    int i;
    fnv_int(h, s->W);
    fnv_int(h, s->H);
    fnv_double(h, s->SM);
    for (i = 0; i < cells; i++) fnv_double(h, s->metal[i]);
    for (i = 0; i < cells; i++) fnv_int(h, s->fog[i]);
    fnv_int(h, s->nvent);
    for (i = 0; i < s->nvent; i++) {
        fnv_double(h, s->vent_centre[i].x);
        fnv_double(h, s->vent_centre[i].z);
    }
}

/* item 3: the units in creation order, each with the orders of its id */
void tasim_hash_units(uint64_t* h, const TASIM* s, const TASIM_DEFS* defs)
{
    int i, j;
    fnv_int(h, s->nunit);
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        /* the orders of an id are its first unit's */
        const WIKIUNIT* by = tasim_by_id(s, u->id);
        fnv_str(h, u->id);
        fnv_str(h, tasim_def(defs, u->type)->name);
        fnv_int(h, u->owner);
        fnv_double(h, u->x);
        fnv_double(h, u->z);
        fnv_double(h, u->hx);
        fnv_double(h, u->hz);
        fnv_int(h, u->sel ? 1 : 0);
        fnv_double(h, u->built);
        fnv_double(h, u->reclaimed);
        fnv_str(h, u->label);
        fnv_int(h, by->norder);
        for (j = 0; j < by->norder; j++) {
            const WIKIORDER* o = &by->order[j];
            fnv_int(h, o->k);
            if (o->k == WIKI_ORDER_MOVE) {
                fnv_double(h, o->x);
                fnv_double(h, o->z);
            } else if (o->k == WIKI_ORDER_BUILD) {
                fnv_str(h, tasim_def(defs, o->type)->name);
                fnv_int(h, o->ox);
                fnv_int(h, o->oz);
            } else if (o->k == WIKI_ORDER_RECLAIM) {
                fnv_str(h, o->target);
                fnv_int(h, o->feat ? 1 : 0);
                if (o->feat) {
                    fnv_double(h, o->x);
                    fnv_double(h, o->z);
                }
            } else fnv_str(h, o->target);
        }
    }
}

/* item 8's tail: the COB host, 1 and tagpu_wikicob_hash from the offset basis as 8 bytes little-endian, or 0 */
void tasim_hash_cob(uint64_t* h, const TASIM* s)
{
    int i;
    fnv_int(h, s->cob != NULL);
    if (s->cob) {
        const uint64_t c = tagpu_wikicob_hash(s->cob, TASIM_FNV_BASIS);
        unsigned char b[8];
        for (i = 0; i < 8; i++) b[i] = (unsigned char)(c >> (8 * i));
        fnv_bytes(h, b, 8);
    }
}

/* items 9 and 10: the nano spray's particles and waiting bursts, then the sound */
void tasim_hash_fx(uint64_t* h, const TASIM* s, const TASIM_DEFS* defs)
{
    int i;
    fnv_int(h, s->npart);
    for (i = 0; i < s->npart; i++) {
        const WIKIVPART* q = &s->part[i];
        fnv_int(h, q->x);
        fnv_int(h, q->y);
        fnv_int(h, q->z);
        fnv_int(h, q->sx);
        fnv_int(h, q->sy);
        fnv_int(h, q->sz);
        fnv_int(h, q->end);
        fnv_int(h, q->col);
    }
    fnv_int(h, s->part_cut);
    fnv_int(h, s->nburst);
    for (i = 0; i < s->nburst; i++) {
        const WIKISPRAY* e = &s->burst[i];
        int k;
        for (k = 0; k < 12; k++) fnv_int(h, e->box[k / 6][k % 6]);
        fnv_int(h, e->due);
        fnv_int(h, (int)e->key);
    }
    fnv_int(h, s->snd_on);
    if (s->snd_on) {
        const int n = s->snd_total < WIKI_SND_LOG ? s->snd_total : WIKI_SND_LOG;
        for (i = 0; i < WIKI_SND_KINDS; i++) fnv_int(h, s->snd_next[i]);
        fnv_int(h, s->snd_last);
        fnv_int(h, s->nsndq);
        for (i = 0; i < s->nsndq; i++) {
            const WIKISNDQ* e = &s->sndq[i];
            fnv_int(h, e->kind);
            fnv_int(h, e->tick);
            fnv_str(h, e->unit);
            fnv_str(h, tasim_def(defs, e->type)->name);
            fnv_int(h, e->priority);
        }
        fnv_int(h, s->snd_total);
        for (i = 0; i < n; i++) {
            const WIKISNDEV* e = &s->snd[(s->snd_total - n + i) % WIKI_SND_LOG];
            fnv_int(h, e->tick);
            fnv_int(h, e->kind);
            fnv_int(h, e->name);
            fnv_int(h, e->cat);
            fnv_int(h, e->variant);
        }
        for (i = 0; i < s->nunit; i++) fnv_int(h, tasim_cant_said(s->unit[i]));
    }
}

/* item 11's head: the features standing and the anchor cells of those gone */
void tasim_hash_feats(uint64_t* h, const TASIM* s, const TASIM_DEFS* defs)
{
    int i;
    fnv_int(h, s->nfeat);
    for (i = 0; i < s->nfeat; i++) {
        fnv_str(h, s->feat[i].id);
        fnv_str(h, defs->entry->feat[s->feat[i].def].name);
        fnv_int(h, s->feat[i].ox);
        fnv_int(h, s->feat[i].oz);
    }
    fnv_int(h, s->ngone);
    for (i = 0; i < 2 * s->ngone; i++) fnv_int(h, s->gone[i]);
}

/* item 12, a game's only: each seat's economy, the economy's mode and stops, the kills, each unit's ledger */
void tasim_hash_econ(uint64_t* h, const TASIM* s)
{
    int i;
    if (!s->game) return;
    fnv_int(h, s->nseat);
    for (i = 0; i < s->nseat; i++) {
        const WIKIECON* z = &s->econ[i];
        fnv_int(h, z->units);
        fnv_int(h, z->lost);
        fnv_int(h, z->out);
        fnv_int(h, z->cmd_lost);
        fnv_double(h, z->m);
        fnv_double(h, z->mmax);
        fnv_double(h, z->mi);
        fnv_double(h, z->mo);
        fnv_double(h, z->wm);
        fnv_double(h, z->e);
        fnv_double(h, z->emax);
        fnv_double(h, z->ei);
        fnv_double(h, z->eo);
        fnv_double(h, z->we);
        fnv_int(h, z->kills);
        fnv_int(h, z->losses);
    }
    fnv_int(h, s->econ_mode);
    fnv_int(h, s->starved);
    fnv_int(h, s->bad_kill);
    fnv_int(h, s->nkill);
    for (i = 0; i < s->nunit; i++) {
        const WIKILEDGER* L = &s->unit[i]->led;
        fnv_double(h, L->eP);
        fnv_double(h, L->eR);
        fnv_double(h, L->eG);
        fnv_double(h, L->eD);
        fnv_double(h, L->mP);
        fnv_double(h, L->mR);
        fnv_double(h, L->mG);
        fnv_double(h, L->mD);
    }
}

/* item 14, a game's only: the deaths in the order made */
void tasim_hash_deaths(uint64_t* h, const TASIM* s, const TASIM_DEFS* defs)
{
    int i;
    if (!s->game) return;
    fnv_int(h, s->ndeath);
    for (i = 0; i < s->ndeath; i++) {
        const WIKIDEATH* d = &s->death[i];
        fnv_int(h, d->tick);
        fnv_int(h, d->owner);
        fnv_double(h, d->x);
        fnv_double(h, d->z);
        fnv_str(h, tasim_def(defs, d->type)->name);
    }
}

uint64_t tasim_hash(const TASIM* s, const TASIM_DEFS* defs)
{
    uint64_t h = TASIM_FNV_BASIS;
    fnv_int(&h, s->tick);
    tasim_hash_map(&h, s);
    tasim_hash_units(&h, s, defs);
    tasim_hash_cob(&h, s);
    tasim_hash_fx(&h, s, defs);
    tasim_hash_feats(&h, s, defs);
    tasim_hash_econ(&h, s);
    tasim_hash_deaths(&h, s, defs);
    return h;
}
