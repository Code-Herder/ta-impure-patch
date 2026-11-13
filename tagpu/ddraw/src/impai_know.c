/* impai_know.c -- what each seat knows of every unit slot (impai.h). Engine-free: it reads a WORLDQ and the
   caller's arrays, and nothing else. */
#include <string.h>
#include "impai.h"

int impai_radar_phase(int seat) { return (3 * seat + 1) % 30; }

/* A write only on a change: a row's bytes move when what the seat knows moves between 32-px blocks or classes,
   which bounds what a recording of the rows carries. */
static void row_put(IMPAI_SEEN* r, const IMPAI_SEEN* now, IMPAI_SEAT* st)
{
    if (memcmp(r, now, sizeof *r)) {
        *r = *now;
        st->known_gen++;
    }
}

static void row_fresh(IMPAI_SEEN* n)
{
    if (!n->how) {
        n->type = (uint16_t)WQ_NONE;
        n->owner = 0xFF;
    }
}

/* One slot of the line-of-sight slice. What observe shows now is written with its block; an identified
   observation (OWN, LOS) names the type and owner, an unidentified one (SHARED) keeps what the row last
   identified. Nothing shown: the classes of now are lost and the radar's kept -- the publish owns those --
   and a row left with neither is a GHOST at its block. A ghost whose block the seat sees now is emptied: the
   seat can see that nothing it knows of is there. */
static void slice_slot(const WORLDQ* q, IMPAI_SEAT* st, IMPAI_SEEN* r, int slot)
{
    IMPAI_SEEN n = *r;
    WQ_OBS o;
    if (q->ops->observe(q, slot, 0, &o)) {
        row_fresh(&n);
        n.how = (uint8_t)((n.how & IMPAI_PASS) |
                          (o.seen == WQ_SEEN_OWN ? IMPAI_OWN : o.seen == WQ_SEEN_LOS ? IMPAI_LOS : IMPAI_SHARED));
        if (o.type != WQ_NONE) {
            n.type = o.type;
            n.owner = o.owner;
        }
        n.bx = o.bx;
        n.bz = o.bz;
    } else if (n.how) {
        n.how &= (uint8_t)~IMPAI_NOW;
        if (!(n.how & IMPAI_PASS)) n.how = IMPAI_GHOST;
        if (n.how == IMPAI_GHOST && q->ops->block_lit(q, n.bx, n.bz)) memset(&n, 0, sizeof n);
    }
    row_put(r, &n, st);
}

/* A completed pass, into the rows. A contact takes the radar and sonar classes; a row that holds the slot by
   no class of now takes the block the pass recorded, and a new one knows neither type nor owner. A slot the
   pass did not find loses both classes, a ghost when nothing else is left. The seat's own units are the
   slice's.
   THE INVARIANT (impai.h, A RADAR CONTACT IS THE UNIT THE PASS FOUND): a bit counts only while the slot holds
   the incarnation the pass recorded when it set the bit, and the row takes the block of that moment. The pass
   writes the slot's record at every bit it sets (worldq.h, radar_step), so a set bit always has its unit's
   record; a slot that died or changed hands since fails the equality and is a slot the pass did not find, and
   a unit that left the coverage since is still where the pass saw it. No live position reaches a row through
   a contact, and an unknown incarnation on either side matches nothing. */
static void publish(const WORLDQ* q, IMPAI_SEAT* st, const IMPAI_ARRAYS* a, int n, IMPAI_WORK* w)
{
    const int seat = q->seat;
    int slot;
    for (slot = 1; slot < n; slot++) {
        IMPAI_SEEN* r = &a->rows[slot].seat[seat];
        IMPAI_SEEN nr = *r;
        const unsigned v = wq_radar_bits(a->bits, (unsigned)slot);
        unsigned pass = (v & WQ_RB_RADAR ? IMPAI_RADAR : 0u) | (v & WQ_RB_SONAR ? IMPAI_SONAR : 0u);
        if (pass) {
            const WQ_RADAR_HIT* h = wq_hit(&a->hits, (unsigned)slot);
            WQ_OBS o;
            const int seen = q->ops->observe(q, slot, 1, &o);
            w->slots++;
            if (seen == WQ_SEEN_OWN) continue;
            if (seen && h->known && o.birth_known && o.birth == h->birth) {
                row_fresh(&nr);
                nr.how = (uint8_t)((nr.how & IMPAI_NOW) | pass);
                if (!(nr.how & IMPAI_NOW)) {
                    nr.bx = h->bx;
                    nr.bz = h->bz;
                }
            } else {
                pass = 0;
            }
        }
        if (!pass && (nr.how & IMPAI_PASS)) {
            nr.how &= (uint8_t)~IMPAI_PASS;
            if (!(nr.how & IMPAI_NOW)) nr.how = IMPAI_GHOST;
        }
        row_put(r, &nr, st);
    }
}

/* The patch scan: clu_rows map rows a tick through the game's own patch rule (tasim_snap_patches, which floods
   only explored metal), into the pending half; at the last row the pending half becomes the published one.
   A row whose floods need more than one tick's budget makes no progress, and is passed over with its patches
   unreported, so the scan always advances; a full pending half ends the scan at once. */
static void patch_scan(const WORLDQ* q, IMPAI_SEAT* st, SNAP_PATCH* clusters, const IMPAI_BUDGET* b,
                       SNAP_SCRATCH* scr, IMPAI_WORK* w)
{
    WQ_MAP m;
    uint32_t flood = b->clu_flood;
    int z1, next, found;
    if (!q->ops->map(q, &m) || m.w < 1 || m.h < 1) return;
    if (st->clu_row >= m.h) st->clu_row = 0;
    z1 = st->clu_row + (int)b->clu_rows < m.h ? st->clu_row + (int)b->clu_rows : m.h;
    found = tasim_snap_patches(q, st->clu_row, z1, clusters + IMPAI_CLUSTERS_HALF + st->npending,
                               IMPAI_CLUSTERS_HALF - st->npending, &flood, &next, scr);
    w->flood = b->clu_flood - flood;
    w->cells = (uint32_t)(z1 - st->clu_row) * (uint32_t)m.w;
    st->npending = (uint16_t)(st->npending + found);
    if (next <= st->clu_row) next = st->npending >= IMPAI_CLUSTERS_HALF ? m.h : st->clu_row + 1;
    st->clu_row = (uint16_t)next;
    if (st->clu_row >= m.h) {
        if (st->nclusters != st->npending ||
            memcmp(clusters, clusters + IMPAI_CLUSTERS_HALF, st->npending * sizeof *clusters)) {
            memcpy(clusters, clusters + IMPAI_CLUSTERS_HALF, st->npending * sizeof *clusters);
            memset(clusters + st->npending, 0, (IMPAI_CLUSTERS_HALF - st->npending) * sizeof *clusters);
            st->nclusters = st->npending;
            st->known_gen++;
        }
        memset(clusters + IMPAI_CLUSTERS_HALF, 0, IMPAI_CLUSTERS_HALF * sizeof *clusters);
        st->npending = 0;
        st->clu_row = 0;
        w->scans = 1;
    }
}

int impai_know_tick(const WORLDQ* q, IMPAI_SEAT* st, const IMPAI_ARRAYS* a, const IMPAI_BUDGET* b,
                    SNAP_SCRATCH* scr, IMPAI_WORK* w)
{
    const int n = q->ops->nslots(q), seat = q->seat;
    const unsigned now = (unsigned)q->ops->gametime(q);
    uint32_t i;
    memset(w, 0, sizeof *w);
    if (n < 1 || n > a->nrows || a->nbytes < (int)WQ_RADAR_BYTES(n) || a->hits.n < n ||
        a->hits.stride < sizeof(WQ_RADAR_HIT) || seat < 0 || seat >= WQ_SEATS)
        return -1;
    /* 1. the line-of-sight slice: up to obs_slice slots from the cursor, wrapping past the last */
    if (st->obs_cursor < 1 || st->obs_cursor >= n) st->obs_cursor = 1;
    for (i = 0; i < b->obs_slice && i + 1 < (uint32_t)n; i++) {
        slice_slot(q, st, &a->rows[st->obs_cursor].seat[seat], st->obs_cursor);
        w->slots++;
        st->obs_cursor = (uint16_t)(st->obs_cursor + 1 < n ? st->obs_cursor + 1 : 1);
    }
    /* 2. the radar pass: it starts on the seat's phase and continues every tick until it completes, so its
       answer grows staler under load while a tick's work stays bounded */
    if (st->radar.stage != WQ_RS_IDLE || now % 30u == (unsigned)impai_radar_phase(seat)) {
        int32_t budget = (int32_t)b->radar_visits;
        const int done = q->ops->radar_step(q, &st->radar, a->bits, a->nbytes, &a->hits, &budget);
        w->visits = (uint32_t)((int32_t)b->radar_visits - budget);
        if (done) {
            publish(q, st, a, n, w);
            st->radar_done = now;
        }
    }
    /* 3. the patch scan */
    if (scr) patch_scan(q, st, a->clusters, b, scr, w);
    return 0;
}
