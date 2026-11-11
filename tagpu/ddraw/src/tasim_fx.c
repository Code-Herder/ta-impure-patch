/* tasim_fx.c -- the shared simulation core's presentation events: the nano spray's particles and the sound's
   replies, named sounds and death sounds, each the engine's rule (tagpu_wikisim.h, THE NANO SPRAY and THE
   SOUND). research/notes/impai/shared-core.md, tasim_fx. Contract: tasim.h.

   They are state of the sim, in the WIKISIM its caller owns, and the state hash carries them (tagpu_wikisim.h,
   items 9 and 10), so a twin that draws or says something else is caught; nothing else the core computes reads
   them. Their randomness is the wiki's own stateless hashes, never a generator with state. No globals. */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "tasim.h"

/* ---- the nano spray (tagpu_wikisim.h, THE NANO SPRAY), sim.js's spray functions ---- */

static int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

/* a double truncated toward zero and held to the int32 range, as _ftol leaves an in-range value */
static int32_t trunc32(double v)
{
    const double t = trunc(v);
    return t < -2147483648.0 ? INT32_MIN : t > 2147483647.0 ? INT32_MAX : (int32_t)t;
}

/* lowbias32, a 32-bit finaliser: each output bit depends on every input bit */
static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* the emission's key: the builder's id (FNV-1a 32 over its bytes) and its place among the tick's
   emissions */
static uint32_t spray_key(const char* id, int nemit)
{
    uint32_t h = 0x811C9DC5u;
    for (; *id; id++) h = (h ^ (unsigned char)*id) * 0x01000193u;
    return mix32(h + (uint32_t)nemit * 0x9E3779B9u);
}

/* THE WIKI'S GENERATOR, in place of the CRT rand 0x4E4870 the spawn draws from: draw k (0..5) of
   particle i of the burst made on `tick`, 0..0x7FFF as rand's */
static int32_t spray_draw(uint32_t key, int tick, int i, int k)
{
    return (int32_t)(mix32(mix32(key ^ (uint32_t)tick) ^ (uint32_t)(i * 8 + k)) & 0x7FFFu);
}

/* 0x473B50's box (0x473B80..0x473D34): on each axis from lo to hi, the base at lo + 4/11 of the
   span and the extent to 7/11 of it, each 4 * span and 7 * span an int32 divided by 11 toward
   zero; {base x, y, z, extent x, y, z} */
static void spray_box(const int32_t lo[3], const int32_t hi[3], int32_t out[6])
{
    int k;
    for (k = 0; k < 3; k++) {
        const uint32_t d = (uint32_t)sub32(hi[k], lo[k]);
        const int32_t q4 = (int32_t)(4u * d) / 11, q7 = (int32_t)(7u * d) / 11;
        out[k] = add32(lo[k], q4);
        out[3 + k] = sub32(add32(lo[k], q7), out[k]);
    }
}

/* ONE BURST of 0x473D50 on `tick`: five particles, each drawn as the spawn draws them -- start x,
   y, z in the first box, then target x, y, z in the second, each base + trunc(draw * extent /
   0x8000) -- running the distance's integer length >> 18 steps (about 4 world units a tick), each
   step the distance over the steps toward zero, its end `tick` + steps; one of 0 steps is not
   made, its draws spent (0x473EDE). Its colour is 0xA1 + its place in the burst (0x473F13..
   0x473F41). Returns the particles the burst made, those past the pool's bound included. */
static int spray_burst(WIKISIM* s, const WIKISPRAY* e, int tick)
{
    int i, k, made = 0;
    for (i = 0; i < 5; i++) {
        int32_t at[2][3], d[3], len, steps;
        WIKIVPART* p;
        for (k = 0; k < 6; k++) {
            const int32_t* b = e->box[k / 3];
            at[k / 3][k % 3] = add32(b[k % 3], (int32_t)((int64_t)spray_draw(e->key, tick, i, k) * b[3 + k % 3] / 0x8000));
        }
        for (k = 0; k < 3; k++) d[k] = sub32(at[1][k], at[0][k]);
        len = trunc32(sqrt((double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2]));
        steps = (int16_t)(uint16_t)((uint32_t)len >> 18);
        if (!steps) continue;
        made++;
        if (s->npart >= WIKI_SPRAY_PARTS) { s->part_cut = 1; continue; }
        p = &s->part[s->npart++];
        p->x = at[0][0];
        p->y = at[0][1];
        p->z = at[0][2];
        p->sx = d[0] / steps;
        p->sy = d[1] / steps;
        p->sz = d[2] / steps;
        p->end = tick + steps;
        p->col = 0xA1 + i % 7;
    }
    return made;
}

/* THE UNIT'S SPRAY on tick `t`, in its turn after its build calls: QueryNanoPiece run-now names the
   piece (0 when the script has none, as 0x43E400 presets it), the host gives that piece's origin,
   and the unit's facing turns it as the stage turns the unit's model (tagpu_wikiscene.c `model`:
   (cs, sn) = (-hz, hx) about the up axis, the model's z out of the world's; a type that does not
   move at the heading (0, 1) whatever its facing says), so the spray starts at the piece the
   stage draws. The target is the structure's box (0x403E5B..0x403EC8): its centre
   +- FootprintX and FootprintZ x 8 world units (the def loader's 0x42D079..0x42D0C5) and from the
   ground to its model's height (0x42D7BF). A build sprays from the piece into the box
   (0x4720D0), a reclaim from the box into the piece (0x472200) and only every second tick from
   its first on that structure: a new order's first step sprays at once (its counter reset at
   0x404845), and no two ticks running ever spray the same reclaim (0x404A4E re-arms the order at
   2 ticks, 0x439E80). */
/* the nano piece's point: QueryNanoPiece names the piece and the host gives its origin, turned by
   the unit's facing */
static void nano_point(WIKISIM* s, const WIKIUNIT* u, int32_t pt[3])
{
    int32_t io[4] = { 0, 0, 0, 0 }, turned[3], base[3];
    const int fixed = !tasim_mobile(s, u->type);
    const double cs = fixed ? -1.0 : -u->hz, sn = fixed ? 0.0 : u->hx;
    int piece = 0;
    const int n = tasim_facts(s);
    if (tagpu_wikicob_query(s->cob, u->cob, "QueryNanoPiece", io, s->fact, n) > 0) piece = io[0];
    tagpu_wikicob_origin(s->cob, u->cob, piece, turned, base);
    pt[0] = add32(add32(tasim_fix16(u->x), trunc32(cs * turned[0] + sn * turned[2])), base[0]);
    pt[1] = add32(turned[1], base[1]);
    pt[2] = sub32(sub32(tasim_fix16(u->z), trunc32(-sn * turned[0] + cs * turned[2])), base[2]);
}

/* one reverse emission from box `target` to point `pt` (0x472200), its second burst due on the
   next tick */
static void emit_reverse(WIKISIM* s, const WIKIUNIT* u, const int32_t target[6], const int32_t pt[3], int t)
{
    WIKISPRAY e;
    memcpy(e.box[0], target, sizeof e.box[0]);
    spray_box(pt, pt, e.box[1]);
    e.key = spray_key(u->id, s->nemit++);
    e.due = t + 1;
    if (!spray_burst(s, &e, t)) return;
    if (s->nburst < WIKI_SPRAY_BURSTS) s->burst[s->nburst++] = e;
    else s->part_cut = 1;
}

/* A FEATURE'S RECLAIM SPRAYS from the feature's box to the nano piece, twice a call with the same
   arguments (0x404D3B, 0x404D4C): the box from the anchor cell's corner, FootprintX x FootprintZ
   cells wide and the feature's height (+0xFA) tall over the ground (0x404CC9..0x404D37), the
   stage's ground being 0 */
static void spray_feature(WIKISIM* s, WIKIUNIT* u, int t)
{
    const int* f = u->spray_box;
    int32_t pt[3], lo[3], hi[3], target[6];
    nano_point(s, u, pt);
    lo[0] = tasim_fix16(f[0] * CELL);
    lo[1] = 0;
    lo[2] = tasim_fix16(f[1] * CELL);
    hi[0] = tasim_fix16((f[0] + f[2]) * CELL);
    hi[1] = tasim_fix16(f[4]);
    hi[2] = tasim_fix16((f[1] + f[3]) * CELL);
    spray_box(lo, hi, target);
    emit_reverse(s, u, target, pt, t);
    emit_reverse(s, u, target, pt, t);
}

void tasim_spray(WIKISIM* s, WIKIUNIT* u, int t)
{
    const WIKIDEF* d;
    int32_t pt[3], lo[3], hi[3];
    WIKISPRAY e;
    int k;
    if (!s->part) return;
    if (u->spray - 1 == HOW_FEATURE) {
        spray_feature(s, u, t);
        return;
    }
    d = tasim_def(s, u->spray_type);
    if (u->spray - 1 == HOW_RECLAIM) {
        if (u->spray_at && !strcmp(u->spray_on, u->spray_id) && t - u->spray_at < 2) return;
        u->spray_at = t;
        snprintf(u->spray_on, sizeof u->spray_on, "%s", u->spray_id);
    }
    nano_point(s, u, pt);
    lo[0] = sub32(tasim_fix16(u->spray_x), d->fx * 0x80000);
    hi[0] = add32(tasim_fix16(u->spray_x), d->fx * 0x80000);
    lo[1] = 0;
    hi[1] = tagpu_wikicob_top(s->cob, u->spray_type);
    lo[2] = sub32(tasim_fix16(u->spray_z), d->fz * 0x80000);
    hi[2] = add32(tasim_fix16(u->spray_z), d->fz * 0x80000);
    k = u->spray - 1 == HOW_RECLAIM;
    spray_box(pt, pt, e.box[k]);
    spray_box(lo, hi, e.box[!k]);
    e.key = spray_key(u->id, s->nemit++);
    e.due = t + 1;
    if (!spray_burst(s, &e, t)) return;
    if (s->nburst < WIKI_SPRAY_BURSTS) s->burst[s->nburst++] = e;
    else s->part_cut = 1;
}

/* THE WALKER on tick `t` (0x471EB0, after the units' turns): every particle moves a step, its
   colour's low nibble on from 1 to 7 and round (0x4739B0), and dies once `t` is past its end
   (0x473B30); then each burst due now is made (0x472F60's second spawn) */
void tasim_spray_tick(WIKISIM* s, int t)
{
    int i, k = 0;
    for (i = 0; i < s->npart; i++) {
        WIKIVPART* p = &s->part[i];
        const int low = (p->col & 0xF) + 1;
        p->x = add32(p->x, p->sx);
        p->y = add32(p->y, p->sy);
        p->z = add32(p->z, p->sz);
        p->col = (p->col & 0xFFF0) + (low > 7 ? 1 : low);
        if (t <= p->end) s->part[k++] = *p;
    }
    s->npart = k;
    for (i = 0, k = 0; i < s->nburst; i++) {
        if (s->burst[i].due == t) spray_burst(s, &s->burst[i], t);
        else s->burst[k++] = s->burst[i];
    }
    s->nburst = k;
}

/* ---- THE SOUND (tagpu_wikisim.h), sim.js's sound functions ---- */

static const unsigned char SND_PRIORITY[WIKI_SND_KINDS] = WIKI_SND_PRIORITIES;
static const unsigned char SND_INTERVAL[WIKI_SND_KINDS] = WIKI_SND_INTERVALS;
/* THE WIKI'S GENERATOR for a reply's variant, in place of the CRT rand 0x47FD70 draws from: the
   unit's id (FNV-1a 32 over its bytes), the kind and the tick, 0..0x7FFF as rand's */
static int32_t snd_draw(const char* id, int kind, int tick)
{
    uint32_t h = 0x811C9DC5u;
    for (; *id; id++) h = (h ^ (unsigned char)*id) * 0x01000193u;
    return (int32_t)(mix32(mix32(h + (uint32_t)kind * 0x9E3779B9u) ^ (uint32_t)tick) & 0x7FFFu);
}

/* an event into the ring; the count stops at INT_MAX, the ring's place wraps by itself */
static void sound_event(WIKISIM* s, int kind, int name, int cat, int variant)
{
    WIKISNDEV* v = &s->snd[s->snd_total % WIKI_SND_LOG];
    v->tick = s->now;
    v->kind = kind;
    v->name = name;
    v->cat = cat;
    v->variant = variant;
    if (s->snd_total < INT_MAX - WIKI_SND_LOG) s->snd_total++;
}

/* 0x47F1A0's play by name: at once, no queue, no cooldown */
void tasim_sound_named(WIKISIM* s, int name)
{
    if (s->snd_on && name >= 0 && name < WIKI_NSNDNAME) sound_event(s, 0, name, -1, 0);
}

/* 0x47F780 then 0x47FAD0: the viewed player's unit (s->viewed's), then the kind's cooldown, a notice
   of its kind already queued, a full queue's last entry dropped, and the notice in before the first
   entry of a lower priority */
void tasim_notice(WIKISIM* s, const WIKIUNIT* u, int kind)
{
    WIKISNDQ* e;
    int i, k;
    if (!s->snd_on || s->snd_quiet || u->owner != s->viewed || kind <= 0 || kind >= WIKI_SND_KINDS) return;
    if (s->now < s->snd_next[kind]) return;
    for (i = 0; i < s->nsndq; i++)
        if (s->sndq[i].kind == kind) return;
    if (s->nsndq == WIKI_SND_QUEUE) s->nsndq--;
    for (k = 0; k < s->nsndq && SND_PRIORITY[s->sndq[k].kind] >= SND_PRIORITY[kind]; k++) {}
    memmove(&s->sndq[k + 1], &s->sndq[k], (size_t)(s->nsndq - k) * sizeof *s->sndq);
    e = &s->sndq[k];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->tick = s->now;
    e->type = u->type;
    e->priority = SND_PRIORITY[kind];
    snprintf(e->unit, sizeof e->unit, "%s", u->id);
    s->nsndq++;
}

/* ACTIVATION as the state setter 0x48B090 notices it, for a unit whose script runs: a change since
   the last look says activate or deactivate */
void tasim_notice_active(WIKISIM* s, WIKIUNIT* u)
{
    int on;
    if (!s->cob || !u->cob || tagpu_wikicob_resting(s->cob, u->cob)) return;
    on = tagpu_wikicob_value(s->cob, u->cob, WIKICOB_ACTIVATION) != 0;
    if (on == u->active) return;
    u->active = on;
    tasim_notice(s, u, on ? WIKI_SND_ACTIVATE : WIKI_SND_DEACTIVATE);
}

/* THE CONSUMER 0x47F680, once a tick after its work: entry 0 leaves the queue, with sound only a
   gap after the last reply, which it then is; with sound and a variant to pick (0x47FD70's count
   gate), the reply is made and the kind's next time written. BOUND: the queue's type is a def index
   notice() took from a unit, below WIKI_MAX_DEFS, and its kind is below WIKI_SND_KINDS. */
void tasim_sound_tick(WIKISIM* s)
{
    WIKISNDQ e;
    int32_t count;
    int play;
    if (!s->snd_on || !s->nsndq) return;
    e = s->sndq[0];
    memmove(&s->sndq[0], &s->sndq[1], (size_t)(s->nsndq - 1) * sizeof *s->sndq);
    s->nsndq--;
    play = s->now >= s->snd_last + WIKI_SND_GAP;
    if (play) s->snd_last = s->now;
    count = e.type >= 0 && e.type < WIKI_MAX_DEFS ? s->snd_count[e.type][e.kind] : 0;
    if (!play || count <= 0) return;
    sound_event(s, e.kind, -1, s->snd_cat[e.type], (int)((int64_t)snd_draw(e.unit, e.kind, s->now) * count / 0x8000));
    s->snd_next[e.kind] = s->now + 30 * SND_INTERVAL[e.kind];
}

/* an order's first run says ok (0x438880 at its handler's start, the one kind-5 push, 0x438899),
   unless the scene gave it; a build order's never does: MobileBuild, 0x403A20..0x403F6F, does not
   call 0x438880 */
void tasim_begin(WIKISIM* s, WIKIUNIT* u, const WIKIORDER* o)
{
    if (o->serial == u->begun) return;
    u->begun = o->serial;
    /* a feature's Reclaim never says ok: 0x404AD0 calls no 0x438880, where ReclaimUnit does
       (0x4047C1) */
    if (o->serial > s->scene_serial && o->k != WIKI_ORDER_BUILD && !(o->k == WIKI_ORDER_RECLAIM && o->feat))
        tasim_notice(s, u, WIKI_SND_OK);
}

/* u's first order has said cant for its taken site */
int tasim_cant_said(const WIKIUNIT* u)
{
    return u->norder && u->cant == u->order[0].serial;
}

/* THE DEATH SOUND of a finished unit killed, sim.js's `deathSound` (tagpu_wikisim.h,
   WIKI_SND_DEATH): one event at once, for a unit of any seat, the cell under its centre held to
   the map */
void tasim_death_sound(WIKISIM* s, const WIKIUNIT* u)
{
    int cx = (int)floor(u->x / CELL), cz = (int)floor(u->z / CELL);
    if (!s->snd_on) return;
    cx = cx < 0 ? 0 : cx > s->W - 1 ? s->W - 1 : cx;
    cz = cz < 0 ? 0 : cz > s->H - 1 ? s->H - 1 : cz;
    sound_event(s, WIKI_SND_DEATH, u->type, -1, cz * s->W + cx);
}
