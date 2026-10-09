#include "tagpu_recmp_pov.h"
#include <string.h>

typedef struct {
    RecMpPovState pov;
    RecMpClickState click;
    RecMpSelectState sel;
    struct {
        unsigned active, total, next, sequence, tick, applied;
        long last;                  /* the last staged slot: the next chunk's must be above it */
        GUID stream;
    } stage;
} PovActor;

struct RecMpPov {
    RecMpRoster roster;
    unsigned limit;                 /* entries a selection may hold: min(each, REC_SEL_MAX) */
    unsigned encoding;
    PovActor actor[RECMP_ACTORS];
    RecSelEntry entry[];            /* per actor: `limit` published, then `limit` staged */
};

static unsigned actor(const RecMpPov* c, unsigned origin)
{
    unsigned i;
    for (i=0;i<c->roster.count;i++) if (c->roster.actor[i].origin==origin) return i;
    return RECMP_ACTORS;
}

static RecSelEntry* entries(const RecMpPov* c, unsigned at, unsigned staged)
{
    return (RecSelEntry*)c->entry+((size_t)at*2+staged)*c->limit;
}

RecMpPov* tagpu_recmp_pov_begin(const RecMpRoster* roster)
{
    RecMpPov* c;
    unsigned limit;
    if (!tagpu_recmp_roster_valid(roster)) return NULL;
    limit=roster->each<REC_SEL_MAX ? roster->each : REC_SEL_MAX;
    c=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,
                sizeof *c+(size_t)roster->count*2*limit*sizeof(RecSelEntry));
    if (c) { c->roster=*roster; c->limit=limit; }
    return c;
}

void tagpu_recmp_pov_end(RecMpPov* c) { if (c) HeapFree(GetProcessHeap(),0,c); }

/* the envelope's own fields, and its origin's announced source roster on the same stream */
static int envelope(const RecMpPov* c, const RecMpSeatMaps* maps, const RecMpHeader* h,
                    unsigned bytes, unsigned* at)
{
    GUID stream;
    return h->version==RECMP_VERSION && h->header==sizeof *h && h->bytes==sizeof *h+bytes &&
           h->sequence && !memcmp(h->magic,"IMPUREMP",8) &&
           (*at=actor(c,h->origin))!=RECMP_ACTORS &&
           tagpu_recmp_seats_get(maps,h->origin,NULL,&stream) &&
           !memcmp(&stream,&h->stream,sizeof stream);
}

/* the envelope comes after a published one of its kind: a later sequence, neither clock back */
static int after(unsigned valid, unsigned sequence, unsigned source_tick, unsigned applied_tick,
                 const GUID* stream, const RecMpHeader* h, unsigned tick)
{
    return !valid || (h->sequence>sequence && h->tick>=source_tick && tick>=applied_tick &&
                      !memcmp(&h->stream,stream,sizeof *stream));
}

static int put_pov(PovActor* a, const RecMpHeader* h, const unsigned char* raw, unsigned bytes,
                   unsigned tick)
{
    RecMpPovState* s=&a->pov;
    const RecMpPovSample* last=s->count ? &s->sample[s->count-1] : NULL;
    RecMpPovSample next;
    if (bytes!=sizeof next.pov) return 0;
    memcpy(&next.pov,raw,sizeof next.pov);
    if (next.pov.player!=RECMP_POV_PLAYER || !rec_pov_ok(&next.pov) ||
        (last && !after(1,last->sequence,last->source_tick,last->applied_tick,&last->stream,h,tick)))
        return 0;
    next.origin=h->origin; next.source_tick=h->tick; next.sequence=h->sequence;
    next.applied_tick=tick; next.stream=h->stream;
    if (s->count==2) s->sample[0]=s->sample[1];
    else s->count++;
    s->sample[s->count-1]=next;
    return 1;
}

static int put_click(PovActor* a, const RecMpHeader* h, const unsigned char* raw, unsigned bytes,
                     unsigned tick)
{
    RecClick k;
    if (bytes!=sizeof k) return 0;
    memcpy(&k,raw,sizeof k);
    if (k.player!=RECMP_POV_PLAYER || !rec_click_ok(&k) ||
        !after(a->click.valid,a->click.sequence,a->click.source_tick,a->click.applied_tick,
               &a->click.stream,h,tick)) return 0;
    a->click.valid=1; a->click.origin=h->origin; a->click.source_tick=h->tick;
    a->click.sequence=h->sequence; a->click.applied_tick=tick; a->click.stream=h->stream;
    a->click.click=k;
    return 1;
}

static int put_select(RecMpPov* c, unsigned at, const RecMpHeader* h, const unsigned char* raw,
                      unsigned bytes, unsigned tick)
{
    PovActor* a=&c->actor[at];
    const RecMpSeat* seat=&c->roster.actor[at];
    RecSel s;
    unsigned begin, end;
    long last;
    if (!rec_sel_head(raw,bytes,c->limit,&s) || s.player!=RECMP_POV_PLAYER ||
        !after(a->sel.valid,a->sel.sequence,a->sel.source_tick,a->sel.applied_tick,&a->sel.stream,
               h,tick)) return 0;
    begin=s.flags&REC_SEL_BEGIN; end=s.flags&REC_SEL_END;
    /* only an empty selection has an empty chunk, and only END reaches the total */
    if ((!s.count && (s.total || !begin || !end)) ||
        (end ? (unsigned)s.first+s.count!=s.total : (unsigned)s.first+s.count>=s.total)) return 0;
    if (begin) {
        if (a->stage.active || s.first) return 0;
        last=-1;
    } else {
        if (!a->stage.active || a->stage.sequence==UINT32_MAX ||
            h->sequence!=a->stage.sequence+1u || h->tick!=a->stage.tick ||
            tick<a->stage.applied || s.total!=a->stage.total || s.first!=a->stage.next ||
            memcmp(&h->stream,&a->stage.stream,sizeof h->stream)) return 0;
        last=a->stage.last;
    }
    if (!rec_sel_entries_ok(raw+sizeof s,s.count,seat->first,seat->last,&last)) return 0;
    /* The copied chunk is bounded whole before anything changes, first + count by the total and
       the total by `limit`, so the staged copy stays inside the actor's rows; nothing after it
       can fail, and only END publishes. */
    memcpy(entries(c,at,1)+s.first,raw+sizeof s,(size_t)s.count*sizeof(RecSelEntry));
    a->stage.active=!end; a->stage.total=s.total; a->stage.next=(unsigned)s.first+s.count;
    a->stage.sequence=h->sequence; a->stage.tick=h->tick; a->stage.applied=tick;
    a->stage.last=last; a->stage.stream=h->stream;
    if (end) {
        memcpy(entries(c,at,0),entries(c,at,1),(size_t)s.total*sizeof(RecSelEntry));
        a->sel.valid=1; a->sel.origin=h->origin; a->sel.source_tick=h->tick;
        a->sel.sequence=h->sequence; a->sel.applied_tick=tick; a->sel.count=s.total;
        a->sel.stream=h->stream;
    }
    return 1;
}

int tagpu_recmp_pov_put(RecMpPov* c, const RecMpSeatMaps* maps, const RecMpHeader* h,
                        const void* body, unsigned bytes, unsigned tick)
{
    unsigned char raw[RECMP_BODY_MAX];
    unsigned at;
    if (!c || !maps || !h || !body || bytes>sizeof raw || !envelope(c,maps,h,bytes,&at)) return 0;
    /* every check reads this copy, and the copy is what publishes */
    memcpy(raw,body,bytes);
    switch (h->kind) {
    case RECMP_POV:    return put_pov(&c->actor[at],h,raw,bytes,tick);
    case RECMP_CLICK:  return put_click(&c->actor[at],h,raw,bytes,tick);
    case RECMP_SELECT: return put_select(c,at,h,raw,bytes,tick);
    default:           return 0;
    }
}

int tagpu_recmp_pov_idle(const RecMpPov* c, unsigned origin)
{
    unsigned at;
    return c && (at=actor(c,origin))!=RECMP_ACTORS && !c->actor[at].stage.active;
}

int tagpu_recmp_pov_get(const RecMpPov* c, unsigned origin, RecMpPovState* out)
{
    unsigned at;
    if (!c || !out || (at=actor(c,origin))==RECMP_ACTORS || !c->actor[at].pov.count) return 0;
    *out=c->actor[at].pov;
    return 1;
}

int tagpu_recmp_pov_click(const RecMpPov* c, unsigned origin, RecMpClickState* out)
{
    unsigned at;
    if (!c || !out || (at=actor(c,origin))==RECMP_ACTORS || !c->actor[at].click.valid) return 0;
    *out=c->actor[at].click;
    return 1;
}

int tagpu_recmp_pov_select(const RecMpPov* c, unsigned origin, RecMpSelectState* out,
                           RecSelEntry* list, unsigned capacity)
{
    unsigned at;
    if (!c || !out || (at=actor(c,origin))==RECMP_ACTORS || !c->actor[at].sel.valid ||
        (list && capacity<c->actor[at].sel.count)) return 0;
    if (list) memcpy(list,entries(c,at,0),(size_t)c->actor[at].sel.count*sizeof *list);
    *out=c->actor[at].sel;
    return 1;
}

int tagpu_recmp_pov_due(const RecMpPov* c, unsigned origin, const RecPov* now, unsigned tick,
                        unsigned room)
{
    unsigned at;
    const RecMpPovState* s;
    if (!c || !now || (at=actor(c,origin))==RECMP_ACTORS) return -1;
    s=&c->actor[at].pov;
    if (s->count) {
        const RecMpPovSample* last=&s->sample[s->count-1];
        if (tick-last->source_tick<now->every || !memcmp(&last->pov,now,sizeof *now)) return 0;
    }
    return room>=RECMP_POV_ROOM;
}

int tagpu_recmp_select_encode(RecMpPov* c, unsigned origin, const RecSelEntry* input,
                              unsigned count, RecMpSelectEmit emit, void* ctx)
{
    unsigned char body[RECMP_BODY_MAX];
    RecSelEntry copy[REC_SEL_MAX];
    unsigned at, first=0, ok=0;
    long last=-1;
    if (!c || (count && !input) || !emit || c->encoding || (at=actor(c,origin))==RECMP_ACTORS ||
        c->actor[at].stage.active || count>c->limit) return 0;
    if (count) memcpy(copy,input,(size_t)count*sizeof *copy);
    if (!rec_sel_entries_ok((const unsigned char*)copy,count,c->roster.actor[at].first,
                            c->roster.actor[at].last,&last)) return 0;
    c->encoding=1;
    do {
        RecSel s;
        s.player=RECMP_POV_PLAYER;
        s.total=(uint16_t)count;
        s.first=(uint16_t)first;
        s.count=(uint16_t)(count-first>RECMP_SELECT_CHUNK ? RECMP_SELECT_CHUNK : count-first);
        s.flags=(uint8_t)((!first ? REC_SEL_BEGIN : 0) | (first+s.count==count ? REC_SEL_END : 0));
        memcpy(body,&s,sizeof s);
        if (s.count) memcpy(body+sizeof s,copy+first,(size_t)s.count*sizeof *copy);
        if (!emit(ctx,origin,body,(unsigned)REC_SEL_BYTES(s.count))) goto done;
        first+=s.count;
    } while (first<count);
    ok=c->actor[at].sel.valid && !c->actor[at].stage.active && c->actor[at].sel.count==count &&
       !memcmp(entries(c,at,0),copy,(size_t)count*sizeof *copy);
done:
    c->encoding=0;
    return ok;
}
