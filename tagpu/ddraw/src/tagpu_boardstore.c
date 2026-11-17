/* tagpu_boardstore.c -- the drawing board's marks as a state machine (tagpu_boardstore.h). */
#include <string.h>
#include "tagpu_boardstore.h"

void tagpu_boardstore_clear(TagpuBoardStore* s, uint32_t life)
{
    memset(s, 0, sizeof *s);
    s->life = life;
}

TagpuBoardMark* tagpu_boardstore_find(TagpuBoardStore* s, uint32_t id)
{
    unsigned i;
    if (!id) return NULL;
    for (i = 0; i < TAGPU_BS_MARKS; i++) if (s->mark[i].id == id) return &s->mark[i];
    return NULL;
}

/* the age of something born at `born` at `now`: a tick that went back counts as no age */
static uint32_t age(uint32_t born, uint32_t now)
{
    return (int32_t)(now - born) > 0 ? now - born : 0;
}

static int may_touch(unsigned seat, unsigned owner, unsigned may)
{
    return owner == seat || (owner < TAGPU_BW_SEATS && (may >> owner & 1u));
}

static int point(TagpuBoardStore* s, unsigned seat, const TagpuBoardOp* op, uint32_t now)
{
    unsigned i, slot = 0;
    uint32_t oldest = 0;
    TagpuBoardMark* m;
    if (tagpu_boardstore_find(s, op->id)) return TAGPU_BS_NONE;
    for (i = 0; i < TAGPU_BS_MARKS; i++) {
        if (!s->mark[i].id) { slot = i; break; }
        if (i == 0 || age(s->mark[i].born, now) > oldest) { oldest = age(s->mark[i].born, now); slot = i; }
    }
    m = &s->mark[slot];
    memset(m, 0, sizeof *m);
    m->id = op->id;
    m->owner = (uint8_t)seat;
    m->x = op->a[0]; m->h = op->a[1]; m->z = op->a[2];
    m->born = now;
    m->len = (uint8_t)op->len;
    memcpy(m->text, op->text, op->len);
    return TAGPU_BS_NEW;
}

/* the distance from (x, z) to a segment, on the ground plane, squared */
static double seg_d2(const TagpuBoardSeg* g, double x, double z)
{
    double dx = (double)g->b[0] - g->a[0], dz = (double)g->b[2] - g->a[2];
    double l2 = dx * dx + dz * dz, t = 0.0, px, pz;
    if (l2 > 0.0) {
        t = ((x - g->a[0]) * dx + (z - g->a[2]) * dz) / l2;
        t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    }
    px = g->a[0] + t * dx - x; pz = g->a[2] + t * dz - z;
    return px * px + pz * pz;
}

static int erase(TagpuBoardStore* s, unsigned seat, const int32_t p[3], unsigned may)
{
    const double r2 = (double)TAGPU_BW_ERASE_R * TAGPU_BW_ERASE_R;
    unsigned i;
    int did = 0;
    for (i = 0; i < s->nseg; i++) {
        TagpuBoardSeg* g = &s->seg[(s->head + i) % TAGPU_BS_SEGS];
        if (g->live && may_touch(seat, g->owner, may) && seg_d2(g, p[0], p[2]) < r2) { g->live = 0; did = 1; }
    }
    for (i = 0; i < TAGPU_BS_MARKS; i++) {
        TagpuBoardMark* m = &s->mark[i];
        double dx = (double)m->x - p[0], dz = (double)m->z - p[2];
        if (m->id && may_touch(seat, m->owner, may) && dx * dx + dz * dz < r2) { m->id = 0; did = 1; }
    }
    return did ? TAGPU_BS_CHANGED : TAGPU_BS_NONE;
}

int tagpu_boardstore_apply(TagpuBoardStore* s, unsigned seat, const TagpuBoardOp* op, uint32_t now,
                           unsigned may)
{
    TagpuBoardMark* m;
    TagpuBoardSeg* g;
    if (!tagpu_boardwire_ok(op, seat)) return TAGPU_BS_NONE;
    switch (op->op) {
    case TAGPU_BW_POINT:
        return point(s, seat, op, now);
    case TAGPU_BW_EDIT: case TAGPU_BW_MOVE: case TAGPU_BW_DELETE:
        m = tagpu_boardstore_find(s, op->id);
        if (!m || !may_touch(seat, m->owner, may)) return TAGPU_BS_NONE;
        if (op->op == TAGPU_BW_DELETE) m->id = 0;
        else if (op->op == TAGPU_BW_MOVE) { m->x = op->a[0]; m->h = op->a[1]; m->z = op->a[2]; }
        else { memset(m->text, 0, sizeof m->text); memcpy(m->text, op->text, op->len); m->len = (uint8_t)op->len; }
        return TAGPU_BS_CHANGED;
    case TAGPU_BW_LINE:
        if (s->nseg == TAGPU_BS_SEGS) { s->head = (s->head + 1u) % TAGPU_BS_SEGS; s->nseg--; }
        g = &s->seg[(s->head + s->nseg) % TAGPU_BS_SEGS];
        g->owner = (uint8_t)seat; g->live = 1;
        memcpy(g->a, op->a, sizeof g->a); memcpy(g->b, op->b, sizeof g->b);
        g->born = now;
        s->nseg++;
        return TAGPU_BS_CHANGED;
    case TAGPU_BW_ERASE:
        return erase(s, seat, op->a, may);
    }
    return TAGPU_BS_NONE;
}

void tagpu_boardstore_expire(TagpuBoardStore* s, uint32_t now)
{
    unsigned i;
    while (s->nseg && (!s->seg[s->head].live || (s->life && age(s->seg[s->head].born, now) >= s->life))) {
        s->head = (s->head + 1u) % TAGPU_BS_SEGS;
        s->nseg--;
    }
    if (s->life)
        for (i = 0; i < s->nseg; i++) {
            TagpuBoardSeg* g = &s->seg[(s->head + i) % TAGPU_BS_SEGS];
            if (age(g->born, now) >= s->life) g->live = 0;
        }
    if (s->life)
        for (i = 0; i < TAGPU_BS_MARKS; i++)
            if (s->mark[i].id && age(s->mark[i].born, now) >= s->life) s->mark[i].id = 0;
}
