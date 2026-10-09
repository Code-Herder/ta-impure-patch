#include "tagpu_recmp_lifecycle.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    RecMpLifeState state;
    unsigned tick, before;
    unsigned char create[24], death[12 + REC_UNIT_IMAGE];
} LifeSlot;

typedef struct { unsigned slot; LifeSlot next; } LifeChange;
#define LIFE_BATCH_MAX RECMP_ROOTS_COUNT
_Static_assert(LIFE_BATCH_MAX >= RECMP_UNIT_MAX, "lifetime staging bounds every unit batch");

struct RecMpLifecycle {
    RecMpRoster roster;
    RecMpFileState ledger;
    RecMpSeatMaps* seats;
    RecMpUnits* units;
    RecMpRoots* roots;
    unsigned definitions, failed, busy, changes, actions;
    char error[160];
    unsigned char body[RECMP_BODY_MAX];
    LifeChange change[LIFE_BATCH_MAX];
    RecMpLifeAction action[LIFE_BATCH_MAX];
    LifeSlot slot[];
};

static int fail(RecMpLifecycle* l, const char* reason, unsigned slot, unsigned birth)
{
    if (!l->failed)
        snprintf(l->error,sizeof l->error,"%s: slot %u, birth %u, after application %llu",
                 reason,slot,birth,(unsigned long long)l->ledger.records);
    l->failed=1;
    return 0;
}

RecMpLifecycle* tagpu_recmp_lifecycle_begin(const RecMpRecordHeader* h,
                                            const RecMpRoster* roster, unsigned definitions)
{
    RecMpLifecycle* l;
    if (!definitions || definitions>0x10000u || !tagpu_recmp_file_roster_valid(h,roster)) return NULL;
    l=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,
                sizeof *l+(size_t)roster->slots*sizeof *l->slot);
    if (!l) return NULL;
    l->roster=*roster; l->definitions=definitions;
    tagpu_recmp_file_state(&l->ledger,h);
    l->seats=tagpu_recmp_seats_begin(roster);
    l->units=tagpu_recmp_units_begin(roster);
    l->roots=tagpu_recmp_roots_begin(roster);
    if (!l->seats || !l->units || !l->roots) {
        tagpu_recmp_lifecycle_end(l); return NULL;
    }
    return l;
}

void tagpu_recmp_lifecycle_end(RecMpLifecycle* l)
{
    if (!l) return;
    tagpu_recmp_roots_end(l->roots);
    tagpu_recmp_units_end(l->units);
    tagpu_recmp_seats_end(l->seats);
    HeapFree(GetProcessHeap(),0,l);
}

static unsigned image_type(const void* image)
{
    uint16_t type;
    memcpy(&type,(const unsigned char*)image+0xA6,2);
    return type;
}

static LifeSlot* change(RecMpLifecycle* l, unsigned slot, unsigned origin)
{
    unsigned i;
    if (!slot || slot>=l->roster.slots) return NULL;
    for (i=0;i<l->roster.count;i++) {
        const RecMpSeat* a=&l->roster.actor[i];
        if (a->origin==origin && slot>=a->first && slot<=a->last) break;
    }
    if (i==l->roster.count) return NULL;
    for (i=0;i<l->changes;i++) if (l->change[i].slot==slot) return &l->change[i].next;
    if (l->changes==LIFE_BATCH_MAX) return NULL;
    l->change[i].slot=slot; l->change[i].next=l->slot[slot]; l->changes++;
    l->change[i].next.state.origin=origin;
    return &l->change[i].next;
}

static RecMpLifeAction* action(RecMpLifecycle* l, const RecMpRecord* where,
                               const RecMpHeader* h, unsigned index, unsigned slot,
                               unsigned birth, unsigned what, unsigned kind,
                               unsigned before, const void* image, unsigned bytes)
{
    RecMpLifeAction* a;
    if (l->actions==LIFE_BATCH_MAX || bytes>sizeof a->image) return NULL;
    a=&l->action[l->actions++];
    memset(a,0,sizeof *a);
    a->where=*where; a->source=*h;
    a->action=what; a->index=index; a->slot=slot; a->kind=kind;
    a->birth=birth; a->before_capture=before; a->bytes=bytes;
    if (bytes) memcpy(a->image,image,bytes);
    return a;
}

static int units_plan(RecMpLifecycle* l, const RecMpRecord* where, const RecMpHeader* h)
{
    unsigned count, at=4, i;
    memcpy(&count,l->body,4);
    for (i=0;i<count;i++) {
        RecMpUnitDelta e;
        RecMpUnitState u;
        LifeSlot* s;
        unsigned type, what;
        memcpy(&e,l->body+at,sizeof e); at+=sizeof e;
        for (unsigned word=0;word<REC_UNIT_DWORDS;word++)
            if (e.bits[word>>3]>>(word&7)&1) at+=4;
        /* units_put validated the copied span and unique slots before this
           read. Its mapped image is this entry's whole reconstructed value. */
        if (!(s=change(l,e.slot,h->origin)) ||
            !tagpu_recmp_units_get(l->units,l->seats,e.slot,&u))
            return fail(l,"unit authority unavailable",e.slot,e.birth);
        type=e.live ? image_type(u.image) : 0;
        if (e.live && (!type || type>=l->definitions))
            return fail(l,"unit type outside definitions",e.slot,e.birth);
        if (s->state.phase==RECMP_LIFE_UNKNOWN) {
            if (!e.full) return fail(l,"initial unit image is not full",e.slot,e.birth);
            s->state.birth=e.birth; s->state.type=type;
            s->state.phase=e.live ? RECMP_LIFE_LIVE : RECMP_LIFE_ABSENT;
            what=e.live ? RECMP_LIFE_BOOTSTRAP : RECMP_LIFE_ABSENCE;
        } else {
            if (e.birth!=s->state.birth ||
                s->state.phase!=(e.live ? RECMP_LIFE_LIVE : RECMP_LIFE_ABSENT) ||
                (e.live && type!=s->state.type))
                return fail(l,"unit image has no matching completed lifetime",e.slot,e.birth);
            what=e.live ? RECMP_LIFE_IMAGE : RECMP_LIFE_ABSENCE;
        }
        if (!action(l,where,h,i,e.slot,e.birth,what,0,0,u.image,e.live ? sizeof u.image : 0))
            return fail(l,"unit action capacity",e.slot,e.birth);
    }
    return 1;
}

static int roots_plan(RecMpLifecycle* l, const RecMpRecord* where, const RecMpHeader* h)
{
    RecMpRoster source;
    uint8_t rows[RECMP_ACTORS+1];
    GUID stream;
    unsigned count, at=4, i;
    if (!tagpu_recmp_seats_get(l->seats,h->origin,&source,&stream) ||
        !tagpu_recmp_seat_map(&source,&l->roster,rows))
        return fail(l,"root source mapping unavailable",0,0);
    memcpy(&count,l->body,4);
    for (i=0;i<count;i++) {
        RecMpRootsEntry e;
        unsigned id, type, what=RECMP_LIFE_ROOT;
        unsigned char image[RECMP_ROOTS_MAX];
        const void* out=image;
        unsigned bytes;
        LifeSlot* s;
        memcpy(&e,l->body+at,sizeof e); at+=sizeof e;
        memcpy(image,l->body+at,e.bytes); at+=e.bytes;
        memcpy(&id,image,4); bytes=e.bytes;
        if (!(s=change(l,id-1,h->origin))) return fail(l,"root slot outside owner",id-1,e.birth);
        if (e.kind==RK_EV_CREATE || e.kind==RECMP_ROOT_CREATE_BEGIN) {
            uint16_t raw_type;
            memcpy(&raw_type,image+4,2); type=raw_type;
            image[6]=rows[image[6]];
            if (type>=l->definitions) return fail(l,"constructor type outside definitions",id-1,e.birth);
            if (e.kind==RECMP_ROOT_CREATE_BEGIN) {
                if ((s->state.phase!=RECMP_LIFE_UNKNOWN && s->state.phase!=RECMP_LIFE_ABSENT) ||
                    (s->state.phase==RECMP_LIFE_ABSENT && e.birth<=s->state.birth))
                    return fail(l,"constructor reuses an occupied or old lifetime",id-1,e.birth);
                s->state.birth=e.birth; s->state.type=type; s->state.phase=RECMP_LIFE_CREATING;
                s->tick=h->tick; s->before=e.before_capture;
                memcpy(s->create,image,sizeof s->create); what=RECMP_LIFE_CREATE_BEGIN;
            } else {
                if (s->state.phase!=RECMP_LIFE_CREATING || e.birth!=s->state.birth ||
                    h->tick!=s->tick || e.before_capture!=s->before ||
                    memcmp(image,s->create,sizeof s->create))
                    return fail(l,"constructor success lacks its matching begin",id-1,e.birth);
                s->state.phase=RECMP_LIFE_LIVE; what=RECMP_LIFE_CREATE_END;
            }
        } else {
            if (e.birth!=s->state.birth ||
                (s->state.phase!=RECMP_LIFE_CREATING && s->state.phase!=RECMP_LIFE_LIVE &&
                 s->state.phase!=RECMP_LIFE_DYING))
                return fail(l,"private root has no matching live incarnation",id-1,e.birth);
            if (e.kind==RK_EV_DEATH) {
                if (s->state.phase!=RECMP_LIFE_LIVE || image_type(image+12)!=s->state.type ||
                    !tagpu_recmp_unit_map(&source,&l->roster,h->origin,id-1,image+12,
                                          REC_UNIT_IMAGE,image+12,REC_UNIT_IMAGE))
                    return fail(l,"death has no matching live unit image",id-1,e.birth);
                image[8]=rows[image[8]];
                s->state.phase=RECMP_LIFE_DYING; s->tick=h->tick; s->before=e.before_capture;
                memcpy(s->death,image,sizeof s->death); what=RECMP_LIFE_DEATH_BEGIN;
            } else if (e.kind==RK_EV_DEATH_END) {
                if (s->state.phase!=RECMP_LIFE_DYING || h->tick!=s->tick ||
                    e.before_capture!=s->before)
                    return fail(l,"death return lacks its matching entry",id-1,e.birth);
                out=s->death; bytes=sizeof s->death;
                s->state.phase=RECMP_LIFE_ABSENT; s->state.type=0; what=RECMP_LIFE_DEATH_END;
            }
        }
        if (!action(l,where,h,i,id-1,e.birth,what,e.kind,e.before_capture,out,bytes))
            return fail(l,"root action capacity",id-1,e.birth);
    }
    return 1;
}

int tagpu_recmp_lifecycle_record(RecMpLifecycle* l, const RecMpRecord* where,
                                  const RecMpHeader* header, const void* body, unsigned bytes,
                                  RecMpLifeVisit visit, void* context)
{
    RecMpFileState next;
    RecMpRecord w;
    RecMpHeader h;
    int valid=1;
    if (!l || l->failed) return 0;
    if (l->busy || !where || !header || !visit || bytes>sizeof l->body || (bytes && !body))
        return fail(l,"invalid lifetime consumer call",0,0);
    w=*where; h=*header;
    if (bytes) memcpy(l->body,body,bytes);
    next=l->ledger;
    if (!tagpu_recmp_file_record(&next,&w,&h,l->body,bytes))
        return fail(l,"invalid application envelope",0,0);
    l->busy=1; l->changes=l->actions=0;
    if (h.kind==RECMP_SEATS) valid=tagpu_recmp_seats_put(l->seats,&h,l->body,bytes);
    else if (h.kind==RECMP_UNITS) valid=tagpu_recmp_units_put(l->units,l->seats,&h,l->body,bytes,
                                                           w.phase==RECMP_RECORD_PROOF ? l->ledger.tick : w.tick);
    else if (h.kind==RECMP_ROOTS) valid=tagpu_recmp_roots_put(l->roots,l->seats,&h,l->body,bytes,
                                                           w.phase==RECMP_RECORD_PROOF ? l->ledger.tick : w.tick);
    if (!valid) fail(l,"invalid source seat/unit/root body",0,0);
    else if (w.phase!=RECMP_RECORD_PROOF) {
        if (h.kind==RECMP_UNITS) valid=units_plan(l,&w,&h);
        else if (h.kind==RECMP_ROOTS) valid=roots_plan(l,&w,&h);
    }
    if (valid && !l->failed) {
        /* All dependency checks finish before the first irreversible effect.
           A callback failure poisons the context; no subsequent action or retry
           can run against the partly applied engine world. */
        for (unsigned i=0;i<l->actions;i++) {
            if (!visit(context,&l->action[i]) || l->failed) {
                fail(l,"engine action refused",l->action[i].slot,l->action[i].birth); break;
            }
        }
        if (!l->failed) {
            for (unsigned i=0;i<l->changes;i++) l->slot[l->change[i].slot]=l->change[i].next;
            l->ledger=next;
        }
    }
    l->busy=0;
    return !l->failed;
}

int tagpu_recmp_lifecycle_get(const RecMpLifecycle* l, unsigned slot, RecMpLifeState* out)
{
    if (!l || l->failed || l->busy || !out || !slot || slot>=l->roster.slots) return -1;
    if (l->slot[slot].state.phase==RECMP_LIFE_UNKNOWN) return 0;
    *out=l->slot[slot].state;
    return 1;
}

const char* tagpu_recmp_lifecycle_error(const RecMpLifecycle* l)
{
    return l ? l->error : "no lifetime consumer";
}

int tagpu_recmp_lifecycle_complete(const RecMpLifecycle* l, const RecMpRecordEnd* end)
{
    if (!l || l->failed || l->busy || !tagpu_recmp_file_footer(&l->ledger,end)) return 0;
    for (unsigned i=0;i<l->roster.count;i++)
        for (unsigned slot=l->roster.actor[i].first;slot<=l->roster.actor[i].last;slot++) {
            unsigned phase=l->slot[slot].state.phase;
            if (phase!=RECMP_LIFE_LIVE && phase!=RECMP_LIFE_ABSENT) return 0;
        }
    return 1;
}
