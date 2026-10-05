#include "tagpu_recmp_roots.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)

static RecMpRoster roster(unsigned source)
{
    RecMpRoster r={ .version=1, .count=4, .each=2, .slots=21 };
    const unsigned rows[]={7,4,0,2};
    for (unsigned i=0;i<4;i++) r.actor[i]=(RecMpSeat){11+11*i,source ? rows[i] : i,1+2*i,2+2*i};
    return r;
}

static RecMpHeader header(unsigned origin, unsigned sequence, unsigned tick, unsigned kind, unsigned bytes)
{
    RecMpHeader h={ .version=RECMP_VERSION, .header=sizeof h, .bytes=sizeof h+bytes,
                   .origin=origin, .sequence=sequence, .tick=tick, .kind=kind,
                   .match={1}, .stream={ .Data1=origin } };
    memcpy(h.magic,"IMPUREMP",8); return h;
}

static unsigned payload(unsigned kind, unsigned origin, unsigned serial, unsigned char* b)
{
    unsigned id=origin==11 ? 2 : 4, n=kind==RK_EV_START_NAMED ? 56 : kind==RK_EV_REPLY ? 140 : 48;
    memset(b,0,n); memcpy(b,&id,4);
    if (kind==RK_EV_START_NAMED) {
        b[4]=4; b[5]=1; memcpy(b+8,&serial,4); memset(b+24,'N',32);
    } else if (kind==RK_EV_REPLY) {
        unsigned reply=serial%24; memcpy(b+4,&reply,4); b[8]=serial&1; b[9]=1;
        char text[9]; snprintf(text,sizeof text,"%08X",serial); memcpy(b+12,text,8); memset(b+20,'R',120);
    } else {
        unsigned reverse=serial&1, layer=0xABCD0004; memcpy(b+4,&reverse,4);
        memcpy(b+8,&serial,4); memset(b+12,0xA5,32); memcpy(b+44,&layer,4);
    }
    return n;
}
static const unsigned kinds[]={RK_EV_START_NAMED,RK_EV_REPLY,RK_EV_SPRAY};
static unsigned batch(unsigned count, unsigned char* b)
{
    unsigned at=4; memcpy(b,&count,4);
    for (unsigned i=0;i<count;i++) {
        unsigned char image[RECMP_ROOTS_MAX];
        unsigned n=payload(kinds[i%3],11,i,image);
        RecMpRootsEntry e={kinds[i%3],n,i&1,{0},i+17};
        memcpy(b+at,&e,sizeof e); at+=sizeof e; memcpy(b+at,image,n); at+=n;
    }
    return at;
}
static RecMpSeatMaps* maps(const RecMpRoster* destination)
{
    RecMpRoster r=roster(1);
    RecMpSeatMaps* m=tagpu_recmp_seats_begin(destination); CHECK(m);
    for (unsigned i=0;i<r.count;i++) {
        RecMpHeader h=header(r.actor[i].origin,1,10,RECMP_SEATS,sizeof r);
        CHECK(tagpu_recmp_seats_put(m,&h,&r,sizeof r));
    }
    return m;
}
static void codec(void)
{
    RecMpRoster r=roster(0);
    RecMpSeatMaps* m=maps(&r); RecMpRoots* c=tagpu_recmp_roots_begin(&r);
    unsigned char original[RECMP_BODY_MAX+1], b[RECMP_BODY_MAX+1];
    unsigned bytes=batch(3,original), second=4+12+56, third=second+12+140;
    RecMpHeader h=header(11,2,10,RECMP_ROOTS,bytes);
    RecMpRootsState before, after;
    CHECK(c && bytes==284 && tagpu_recmp_roots_put(c,m,&h,original,bytes,100));
    CHECK(tagpu_recmp_roots_get(c,11,&before) && !memcmp(before.body,original,bytes));
    memset(original,0xCC,bytes); CHECK(tagpu_recmp_roots_get(c,11,&after) && !memcmp(&before,&after,sizeof before));
    bytes=batch(3,original);
    for (unsigned control=0;control<36;control++) {
        unsigned n=bytes, applied=101; RecMpHeader bad=header(11,3,11,RECMP_ROOTS,bytes);
        memcpy(b,original,bytes);
        switch (control) {
        case 0: b[0]=0; break; case 1: b[0]=168; break;
        case 2: b[4]=0; b[5]=0; break; case 3: b[6]=11; break;
        case 4: b[8]=2; break; case 5: b[9]=1; break; case 6: b[10]=1; break;
        case 7: b[11]=1; break; case 8: b[15]=0x80; break;
        case 9: b[16]=0; break; case 10: b[16]=4; break;
        case 11: b[20]=5; break; case 12: b[21]=2; break;
        case 13: b[22]=1; break; case 14: b[23]=1; break; case 15: b[40]=0; break;
        case 16: b[second+16]=24; break; case 17: b[second+20]=2; break;
        case 18: b[second+21]=2; break; case 19: b[second+22]=1; break;
        case 20: b[second+23]=1; break; case 21: b[second+21]=0; break;
        case 22: b[second+24]=0; break; case 23: b[third+16]=2; break;
        case 24: b[third+56]=10; break; case 25: b[third+56]=255; b[third+57]=255; break;
        case 26: b[bytes]=1; n++; break; case 27: bad.kind=RECMP_NAMED; break;
        case 28: bad.version--; break; case 29: bad.sequence=2; break;
        case 30: bad.tick=9; break; case 31: applied=99; break;
        case 32: bad.origin=99; break; case 33: bad.stream.Data1=99; break;
        case 34: bad.header--; break; case 35: n=RECMP_BODY_MAX+1; break;
        }
        bad.bytes=sizeof bad+n;
        CHECK(!tagpu_recmp_roots_put(c,m,&bad,b,n,applied));
        CHECK(tagpu_recmp_roots_get(c,11,&after) && !memcmp(&before,&after,sizeof before));
    }
    unsigned char* page=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); DWORD old;
    CHECK(page && VirtualProtect(page+4096,4096,PAGE_NOACCESS,&old));
    for (unsigned n=0;n<bytes;n++) {
        RecMpHeader bad=header(11,3,11,RECMP_ROOTS,n); unsigned char* p=page+4096-n;
        memcpy(p,original,n); CHECK(!tagpu_recmp_roots_put(c,m,&bad,p,n,101));
        CHECK(tagpu_recmp_roots_get(c,11,&after) && !memcmp(&before,&after,sizeof before));
    }
    VirtualFree(page,0,MEM_RELEASE);
    unsigned count=167, at=4, id=2; memcpy(b,&count,4);
    for (unsigned i=0;i<count;i++) {
        RecMpRootsEntry e={RK_EV_REPLY,12,i&1,{0},17};
        memcpy(b+at,&e,sizeof e); at+=sizeof e; memset(b+at,0,12); memcpy(b+at,&id,4); at+=12;
    }
    h=header(11,3,11,RECMP_ROOTS,at); CHECK(at==4012 && tagpu_recmp_roots_put(c,m,&h,b,at,101));
    memset(&after,0xA5,sizeof after); CHECK(!tagpu_recmp_roots_get(c,99,&after));
    for (unsigned i=0;i<sizeof after;i++) CHECK(((unsigned char*)&after)[i]==0xA5);
    tagpu_recmp_roots_end(c); tagpu_recmp_seats_end(m);
    puts("PASS: mixed roots, future birth tokens, exact high layer bits, 36 atomic refusals, all 284 guarded truncations, 167-call maximum and unchanged unknown output");
}
typedef struct { unsigned next, packets, alternating, tick, fail, replies; } Emissions;
static int emit(void* context, unsigned origin, unsigned tick, const void* body, unsigned bytes)
{
    Emissions* e=context; const unsigned char* b=body; unsigned count, at=4;
    if (e->fail && e->packets+1==e->fail) { e->packets++; return 0; }
    memcpy(&count,b,4); CHECK(count && bytes<=RECMP_BODY_MAX && tick==e->tick);
    for (unsigned i=0;i<count;i++) {
        RecMpRootsEntry entry; unsigned char expected[RECMP_ROOTS_MAX];
        unsigned kind=e->replies ? RK_EV_REPLY : kinds[e->next%3];
        unsigned owner=e->alternating && e->next&1 ? 22 : 11;
        unsigned n=payload(kind,owner,e->next,expected);
        memcpy(&entry,b+at,sizeof entry); at+=sizeof entry;
        CHECK(origin==owner && entry.kind==kind && entry.bytes==n && entry.birth==e->next+17 &&
              entry.before_capture==(e->next&1) && !memcmp(b+at,expected,n));
        at+=n; e->next++;
    }
    CHECK(at==bytes); e->packets++; return 1;
}
static void queues(void)
{
    for (unsigned alternating=0;alternating<2;alternating++) {
        RecMpRootsQueue* q=tagpu_recmp_roots_queue_begin();
        Emissions e={ .alternating=alternating, .tick=10 }; unsigned char b[RECMP_ROOTS_MAX];
        CHECK(q);
        for (unsigned i=0;i<4096;i++) {
            unsigned origin=alternating && i&1 ? 22 : 11, kind=kinds[i%3];
            unsigned n=payload(kind,origin,i,b);
            CHECK(tagpu_recmp_roots_queue_push(q,origin,10,i+17,i&1,kind,b,n)); memset(b,0xCC,sizeof b);
        }
        CHECK(tagpu_recmp_roots_queue_flush(q,9,emit,&e) && !e.next);
        CHECK(tagpu_recmp_roots_queue_flush(q,10,emit,&e) && e.next==4096);
        if (alternating) CHECK(e.packets==4096);
        e.tick=11;
        for (unsigned i=4096;i<4196;i++) {
            unsigned origin=alternating && i&1 ? 22 : 11, kind=kinds[i%3];
            unsigned n=payload(kind,origin,i,b);
            CHECK(tagpu_recmp_roots_queue_push(q,origin,11,i+17,i&1,kind,b,n));
        }
        CHECK(tagpu_recmp_roots_queue_flush(q,11,emit,&e) && e.next==4196);
        tagpu_recmp_roots_queue_end(q);
    }
    for (unsigned control=0;control<3;control++) {
        RecMpRootsQueue* q=tagpu_recmp_roots_queue_begin();
        Emissions e={ .fail=control==2 ? 2 : 0, .tick=10, .replies=1 }; unsigned char b[RECMP_ROOTS_MAX];
        unsigned n=payload(RK_EV_REPLY,11,0,b); CHECK(q);
        unsigned count=control==0 ? 4096 : control==1 ? 1 : 100;
        for (unsigned i=0;i<count;i++) {
            n=payload(RK_EV_REPLY,11,i,b);
            CHECK(tagpu_recmp_roots_queue_push(q,11,10,i+17,i&1,RK_EV_REPLY,b,n));
        }
        if (control<2) CHECK(!tagpu_recmp_roots_queue_push(q,11,control ? 9 : 10,17,0,RK_EV_REPLY,b,n));
        else CHECK(!tagpu_recmp_roots_queue_flush(q,10,emit,&e) && e.next==26 && e.packets==2);
        CHECK(!tagpu_recmp_roots_queue_flush(q,10,emit,&e));
        CHECK(!tagpu_recmp_roots_queue_push(q,11,10,17,0,RK_EV_REPLY,b,n));
        tagpu_recmp_roots_queue_end(q);
    }
    puts("PASS: 8392 mixed script/reply/spray calls preserve copied bytes/order across actor switches, packet splits, phase/tick and ring wrap; overflow/backwards clock/failed emission latch incomplete");
}
int main(void) { codec(); queues(); return 0; }
