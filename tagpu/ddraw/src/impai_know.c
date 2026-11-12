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
   no class of now takes the contact's block, and a new one knows neither type nor owner. A slot the pass did
   not find loses both classes, a ghost when nothing else is left. The seat's own units are the slice's. */
static void publish(const WORLDQ* q, IMPAI_SEAT* st, IMPAI_ROW* rows, const uint8_t* bits, int n, IMPAI_WORK* w)
{
    const int seat = q->seat;
    int slot;
    for (slot = 1; slot < n; slot++) {
        IMPAI_SEEN* r = &rows[slot].seat[seat];
        IMPAI_SEEN nr = *r;
        const unsigned v = wq_radar_bits(bits, (unsigned)slot);
        unsigned pass = (v & WQ_RB_RADAR ? IMPAI_RADAR : 0u) | (v & WQ_RB_SONAR ? IMPAI_SONAR : 0u);
        if (pass) {
            WQ_OBS o;
            const int seen = q->ops->observe(q, slot, 1, &o);
            w->slots++;
            if (seen == WQ_SEEN_OWN) continue;
            if (seen) {
                row_fresh(&nr);
                nr.how = (uint8_t)((nr.how & IMPAI_NOW) | pass);
                if (!(nr.how & IMPAI_NOW)) {
                    nr.bx = o.bx;
                    nr.bz = o.bz;
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

int impai_know_tick(const WORLDQ* q, IMPAI_SEAT* st, IMPAI_ROW* rows, int nrows, uint8_t* bits, int nbytes,
                    const IMPAI_BUDGET* b, IMPAI_WORK* w)
{
    const int n = q->ops->nslots(q), seat = q->seat;
    const unsigned now = (unsigned)q->ops->gametime(q);
    uint32_t i;
    memset(w, 0, sizeof *w);
    if (n < 1 || n > nrows || nbytes < (int)WQ_RADAR_BYTES(n) || seat < 0 || seat >= WQ_SEATS) return -1;
    /* 1. the line-of-sight slice: up to obs_slice slots from the cursor, wrapping past the last */
    if (st->obs_cursor < 1 || st->obs_cursor >= n) st->obs_cursor = 1;
    for (i = 0; i < b->obs_slice && i + 1 < (uint32_t)n; i++) {
        slice_slot(q, st, &rows[st->obs_cursor].seat[seat], st->obs_cursor);
        w->slots++;
        st->obs_cursor = (uint16_t)(st->obs_cursor + 1 < n ? st->obs_cursor + 1 : 1);
    }
    /* 2. the radar pass: it starts on the seat's phase and continues every tick until it completes, so its
       answer grows staler under load while a tick's work stays bounded */
    if (st->radar.stage != WQ_RS_IDLE || now % 30u == (unsigned)impai_radar_phase(seat)) {
        int32_t budget = (int32_t)b->radar_visits;
        const int done = q->ops->radar_step(q, &st->radar, bits, nbytes, &budget);
        w->visits = (uint32_t)((int32_t)b->radar_visits - budget);
        if (done) {
            publish(q, st, rows, bits, n, w);
            st->radar_done = now;
        }
    }
    return 0;
}
