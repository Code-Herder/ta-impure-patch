#include "tagpu_recmp_units.h"
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
                   .match={ .Data1=1 }, .stream={ .Data1=origin } };
    memcpy(h.magic,"IMPUREMP",8);
    return h;
}

static void word_put(void* image, unsigned offset, unsigned value) { memcpy((char*)image+offset,&value,4); }
static unsigned word_get(const void* image, unsigned offset) { unsigned value; memcpy(&value,(char*)image+offset,4); return value; }

static void image(const RecMpSeat* seat, unsigned char* out)
{
    for (unsigned i=0;i<REC_UNIT_IMAGE;i++) out[i]=(i*37+seat->origin)&255;
    word_put(out,0x96,seat->seat+1); word_put(out,0xEC,seat->seat+1);
    word_put(out,0x110,0x10000000u); out[0xFF]=seat->seat; out[0xF4]=10;
}

static RecMpSeatMaps* maps(const RecMpRoster* destination, const RecMpRoster* source)
{
    RecMpSeatMaps* m=tagpu_recmp_seats_begin(destination); CHECK(m);
    for (unsigned i=0;i<source->count;i++) {
        RecMpHeader h=header(source->actor[i].origin,1,10,RECMP_SEATS,sizeof *source);
        CHECK(tagpu_recmp_seats_put(m,&h,source,sizeof *source));
    }
    return m;
}

static void permutations(void)
{
    unsigned seed=17, units=0;
    for (unsigned count=1;count<=10;count++) for (unsigned trial=0;trial<64;trial++) {
        RecMpRoster a={ .version=1, .count=count, .each=2, .slots=21 }, b=a;
        unsigned row[2][10];
        uint8_t permutation[11];
        for (unsigned peer=0;peer<2;peer++) {
            for (unsigned i=0;i<10;i++) row[peer][i]=i;
            for (unsigned i=9;i;i--) {
                unsigned j, save;
                seed=seed*1664525u+1013904223u; j=seed%(i+1);
                save=row[peer][i]; row[peer][i]=row[peer][j]; row[peer][j]=save;
            }
        }
        for (unsigned i=0;i<count;i++) {
            a.actor[i]=(RecMpSeat){i+1,row[0][i],1+2*i,2+2*i};
            b.actor[count-1-i]=(RecMpSeat){i+1,row[1][i],1+2*i,2+2*i};
        }
        CHECK(tagpu_recmp_seat_map(&a,&b,permutation));
        for (unsigned i=0;i<count;i++) for (unsigned attacker=0;attacker<=10;attacker++)
            for (unsigned store=0;store<=11;store++) {
                unsigned char original[REC_UNIT_IMAGE], mapped[REC_UNIT_IMAGE+2], inverse[REC_UNIT_IMAGE];
                image(&a.actor[i],original); original[0xF4]=attacker; word_put(original,0xEC,store);
                memset(mapped,0xA5,sizeof mapped);
                CHECK(tagpu_recmp_unit_map(&a,&b,i+1,a.actor[i].first,original,sizeof original,mapped+1,REC_UNIT_IMAGE));
                CHECK(mapped[0]==0xA5 && mapped[sizeof mapped-1]==0xA5);
                CHECK(word_get(mapped+1,0x96)==row[1][i]+1 && mapped[1+0xFF]==row[1][i] &&
                      mapped[1+0xF4]==permutation[attacker] &&
                      word_get(mapped+1,0xEC)==(store ? permutation[store-1]+1u : 0u));
                for (unsigned byte=0;byte<REC_UNIT_IMAGE;byte++)
                    if (!(byte>=0x96 && byte<0x9A) && !(byte>=0xEC && byte<0xF0) && byte!=0xF4 && byte!=0xFF)
                        CHECK(mapped[1+byte]==original[byte]);
                CHECK(tagpu_recmp_unit_map(&b,&a,i+1,a.actor[i].first,mapped+1,REC_UNIT_IMAGE,inverse,sizeof inverse));
                CHECK(!memcmp(inverse,original,sizeof original));
                CHECK(tagpu_recmp_unit_map(&a,&b,i+1,a.actor[i].first,inverse,sizeof inverse,inverse,sizeof inverse));
                CHECK(!memcmp(inverse,mapped+1,sizeof inverse)); units++;
            }
    }
    printf("PASS: 640 roster permutations, %u exact unit inverse round trips, every attacker row and nullable player ID\n",units);
}

static void mapping_bounds(void)
{
    RecMpRoster a=roster(1), b=roster(0);
    unsigned char original[REC_UNIT_IMAGE], out[REC_UNIT_IMAGE];
    for (unsigned control=0;control<16;control++) {
        unsigned origin=11, slot=1, bytes=sizeof original, capacity=sizeof out;
        RecMpRoster source=a, destination=b;
        image(&a.actor[0],original); memset(out,0xA5,sizeof out);
        switch (control) {
        case 0: origin=99; break;
        case 1: slot=0; break;
        case 2: slot=3; break;
        case 3: slot=a.slots; break;
        case 4: bytes--; break;
        case 5: bytes++; break;
        case 6: capacity--; break;
        case 7: word_put(original,0x96,0); break;
        case 8: word_put(original,0x96,11); break;
        case 9: word_put(original,0xEC,12); break;
        case 10: word_put(original,0xEC,REC_ID_BAD); break;
        case 11: original[0xFF]=255; break;
        case 12: original[0xFF]=0; break;
        case 13: original[0xF4]=11; break;
        case 14: source.actor[0].seat=10; break;
        case 15: destination.actor[0].first=9; destination.actor[0].last=10; break;
        }
        CHECK(!tagpu_recmp_unit_map(&source,&destination,origin,slot,original,bytes,out,capacity));
        for (unsigned i=0;i<sizeof out;i++) CHECK(out[i]==0xA5);
    }
    puts("PASS: 16 unit mapping refusals retain output; slot zero and 255 sentinel refused");
}

static unsigned batch(RecMpUnits* c, unsigned birth, unsigned char* body, unsigned char* first_image)
{
    unsigned count=2, bytes=4;
    int n;
    memcpy(body,&count,4);
    n=tagpu_recmp_units_delta(c,11,1,birth,1,first_image,REC_UNIT_IMAGE,body+bytes,RECMP_BODY_MAX-bytes);
    CHECK(n>0); bytes+=n;
    n=tagpu_recmp_units_delta(c,11,2,0,0,NULL,0,body+bytes,RECMP_BODY_MAX-bytes);
    CHECK(n>=0);
    if (n) bytes+=n;
    else { count=1; memcpy(body,&count,4); }
    return bytes;
}

static void refused(RecMpUnits* c, RecMpSeatMaps* m, const RecMpHeader* h,
                     const void* body, unsigned bytes, unsigned tick)
{
    RecMpUnitState before[2], after[2];
    for (unsigned i=0;i<2;i++) CHECK(tagpu_recmp_units_get(c,m,i+1,&before[i]));
    CHECK(!tagpu_recmp_units_put(c,m,h,body,bytes,tick));
    for (unsigned i=0;i<2;i++) {
        CHECK(tagpu_recmp_units_get(c,m,i+1,&after[i]));
        CHECK(!memcmp(&before[i],&after[i],sizeof before[i]));
    }
}

static void cache(void)
{
    RecMpRoster source=roster(1), destination=roster(0);
    RecMpUnits *sender=tagpu_recmp_units_begin(&source), *receiver=tagpu_recmp_units_begin(&destination);
    RecMpSeatMaps *local=maps(&source,&source), *remote=maps(&destination,&source);
    unsigned char raw[REC_UNIT_IMAGE], body[RECMP_BODY_MAX], changed[RECMP_BODY_MAX], entry[400];
    RecMpUnitState state;
    unsigned bytes;
    RecMpHeader h;
    CHECK(sender && receiver); image(&source.actor[0],raw);
    memset(&state,0xA5,sizeof state); CHECK(!tagpu_recmp_units_get(receiver,remote,1,&state));
    for (unsigned i=0;i<sizeof state;i++) CHECK(((unsigned char*)&state)[i]==0xA5);
    bytes=batch(sender,9,body,raw); h=header(11,2,10,RECMP_UNITS,bytes);
    CHECK(bytes==4+2*sizeof(RecMpUnitDelta)+REC_UNIT_IMAGE);
    CHECK(tagpu_recmp_units_put(sender,local,&h,body,bytes,10));
    CHECK(tagpu_recmp_units_put(receiver,remote,&h,body,bytes,100));
    CHECK(tagpu_recmp_units_get(receiver,remote,1,&state) && state.live && state.origin==11 && state.birth==9 &&
          state.source_tick==10 && state.sequence==2 && state.applied_tick==100 &&
          !memcmp(&state.stream,&h.stream,sizeof h.stream) && word_get(state.image,0x96)==1 &&
          word_get(state.image,0xEC)==1 && ((unsigned char*)state.image)[0xFF]==0 && ((unsigned char*)state.image)[0xF4]==10);
    CHECK(tagpu_recmp_units_get(receiver,remote,2,&state) && !state.live && state.birth==0);
    for (unsigned i=0;i<REC_UNIT_DWORDS;i++) CHECK(!state.image[i]);
    CHECK(!tagpu_recmp_units_get(receiver,remote,3,&state));
    CHECK(!tagpu_recmp_units_delta(sender,11,1,9,1,raw,sizeof raw,entry,sizeof entry));
    word_put(raw,0x68,123); raw[0xF4]=4; word_put(raw,0xEC,1);
    bytes=batch(sender,9,body,raw); h=header(11,3,11,RECMP_UNITS,bytes);
    CHECK(!((RecMpUnitDelta*)(body+4))->full && bytes<4+sizeof(RecMpUnitDelta)+REC_UNIT_IMAGE);
    for (unsigned control=0;control<27;control++) {
        RecMpHeader bad=h;
        unsigned n=bytes, tick=101;
        memcpy(changed,body,bytes);
        RecMpUnitDelta* e=(RecMpUnitDelta*)(changed+4);
        switch (control) {
        case 0: n--; break;
        case 1: changed[n++]=0; break;
        case 2: word_put(changed,0,0); break;
        case 3: word_put(changed,0,RECMP_UNIT_MAX+1); break;
        case 4: e->slot=0; break;
        case 5: e->slot=3; break;
        case 6: e->live=2; break;
        case 7: e->full=2; break;
        case 8: e->reserved=1; break;
        case 9: e->birth|=0x80000000u; break;
        case 10: e->bits[REC_UNIT_BITS-1]|=4; break;
        case 11: e->full=1; break;
        case 12: e->live=0; break;
        case 13: e->birth++; break;
        case 14: bad.sequence=2; break;
        case 15: bad.tick=9; break;
        case 16: tick=99; break;
        case 17: bad.stream.Data1++; break;
        case 18: bad.version=1; break;
        case 19: bad.magic[0]=0; break;
        case 20: bad.kind=RECMP_PLAYER; break;
        case 21: bad.origin=99; break;
        case 22: bad.header--; break;
        case 23: bad.sequence=0; break;
        case 24: bad.bytes++; break;
        case 25: word_put(changed,0,2); memcpy(changed+n,changed+4,n-4); n+=n-4; break;
        case 26: e->bits[0]|=1; break;
        }
        if (control!=24) bad.bytes=sizeof bad+n;
        refused(receiver,remote,&bad,changed,n,tick);
    }
    CHECK(tagpu_recmp_units_put(sender,local,&h,body,bytes,11));
    CHECK(tagpu_recmp_units_put(receiver,remote,&h,body,bytes,101));
    CHECK(tagpu_recmp_units_get(receiver,remote,1,&state) && word_get(state.image,0x68)==123 &&
          word_get(state.image,0xEC)==3 && ((unsigned char*)state.image)[0xF4]==1 && state.sequence==3);
    word_put(raw,0x92,1234);
    bytes=batch(sender,12,body,raw); h=header(11,4,12,RECMP_UNITS,bytes);
    CHECK(((RecMpUnitDelta*)(body+4))->full);
    CHECK(tagpu_recmp_units_put(sender,local,&h,body,bytes,12));
    CHECK(tagpu_recmp_units_put(receiver,remote,&h,body,bytes,102));
    CHECK(tagpu_recmp_units_get(receiver,remote,1,&state) && state.birth==12 && word_get(state.image,0x92)==1234);
    {
        unsigned count=1;
        int n=tagpu_recmp_units_delta(sender,11,1,12,0,NULL,0,body+4,RECMP_BODY_MAX-4);
        CHECK(n==sizeof(RecMpUnitDelta)); memcpy(body,&count,4); bytes=4+n;
        h=header(11,5,13,RECMP_UNITS,bytes);
        CHECK(tagpu_recmp_units_put(receiver,remote,&h,body,bytes,103));
        CHECK(tagpu_recmp_units_put(sender,local,&h,body,bytes,13));
        CHECK(tagpu_recmp_units_get(receiver,remote,1,&state) && !state.live && state.birth==12 && state.applied_tick==103);
        CHECK(!tagpu_recmp_units_delta(sender,11,1,12,0,NULL,0,entry,sizeof entry));
    }
    {
        RecMpRoster other=source;
        RecMpSeatMaps* wrong;
        RecMpUnitState before, after;
        RecMpUnitDelta delta={ .slot=26212, .full=1 };
        unsigned count=1;
        other.each=6553; other.slots=65531;
        for (unsigned i=0;i<other.count;i++) { other.actor[i].first=1+6553*i; other.actor[i].last=6553*(i+1); }
        wrong=maps(&other,&other);
        CHECK(tagpu_recmp_units_get(receiver,remote,2,&before));
        memset(&after,0xA5,sizeof after); CHECK(!tagpu_recmp_units_get(receiver,wrong,2,&after));
        for (unsigned i=0;i<sizeof after;i++) CHECK(((unsigned char*)&after)[i]==0xA5);
        memcpy(body,&count,4); memcpy(body+4,&delta,sizeof delta);
        h=header(44,2,10,RECMP_UNITS,4+sizeof delta);
        CHECK(!tagpu_recmp_units_put(receiver,wrong,&h,body,h.bytes-sizeof h,100));
        CHECK(tagpu_recmp_units_get(receiver,remote,2,&after) && !memcmp(&before,&after,sizeof before));
        tagpu_recmp_seats_end(wrong);
    }
    tagpu_recmp_units_end(sender); tagpu_recmp_units_end(receiver);
    tagpu_recmp_seats_end(local); tagpu_recmp_seats_end(remote);
    puts("PASS: mapped full/delta/incarnation/death authority, unknown versus absent, provenance and 27 atomic refusal controls");
}

static void guard_and_capacity(void)
{
    SYSTEM_INFO system;
    unsigned char *guard, image_bytes[REC_UNIT_IMAGE], body[RECMP_BODY_MAX], out[REC_UNIT_IMAGE];
    RecMpRoster source=roster(1), destination=roster(0);
    RecMpUnits *sender=tagpu_recmp_units_begin(&source), *receiver=tagpu_recmp_units_begin(&destination);
    RecMpSeatMaps* m=maps(&destination,&source);
    RecMpHeader h;
    DWORD old;
    unsigned bytes;
    CHECK(sender && receiver); GetSystemInfo(&system);
    guard=VirtualAlloc(NULL,2*system.dwPageSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); CHECK(guard);
    CHECK(VirtualProtect(guard+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&old));
    image(&source.actor[0],image_bytes);
    memcpy(guard+system.dwPageSize-sizeof image_bytes,image_bytes,sizeof image_bytes);
    CHECK(tagpu_recmp_unit_map(&source,&destination,11,1,guard+system.dwPageSize-sizeof image_bytes,
                               sizeof image_bytes,out,sizeof out));
    CHECK(!tagpu_recmp_unit_map(&source,&destination,11,1,guard+system.dwPageSize,
                                REC_UNIT_IMAGE-1,out,sizeof out));
    CHECK(!tagpu_recmp_unit_map(&source,&destination,11,1,guard+system.dwPageSize,
                                REC_UNIT_IMAGE+1,out,sizeof out));
    CHECK(!tagpu_recmp_unit_map(&source,&destination,11,1,guard+system.dwPageSize,
                                REC_UNIT_IMAGE,out,sizeof out-1));
    bytes=batch(sender,9,body,image_bytes); h=header(11,2,10,RECMP_UNITS,bytes);
    memcpy(guard+system.dwPageSize-bytes,body,bytes);
    CHECK(tagpu_recmp_units_put(receiver,m,&h,guard+system.dwPageSize-bytes,bytes,100));
    h.sequence=3;
    for (unsigned n=0;n<bytes;n++) {
        unsigned char* raw=guard+system.dwPageSize-n;
        memcpy(raw,body,n); h.bytes=sizeof h+n;
        refused(receiver,m,&h,raw,n,101);
    }
    h.bytes=sizeof h+RECMP_BODY_MAX+1;
    refused(receiver,m,&h,guard+system.dwPageSize,RECMP_BODY_MAX+1,101);
    VirtualFree(guard,0,MEM_RELEASE);
    tagpu_recmp_units_end(sender); tagpu_recmp_units_end(receiver); tagpu_recmp_seats_end(m);
    {
        RecMpRoster r={ .version=1, .count=1, .each=RECMP_UNIT_MAX, .slots=10*RECMP_UNIT_MAX+1 };
        RecMpUnitState state;
        RecMpUnitDelta entry={ .full=1 };
        unsigned count=RECMP_UNIT_MAX, at=4;
        r.actor[0]=(RecMpSeat){11,9,1,RECMP_UNIT_MAX};
        receiver=tagpu_recmp_units_begin(&r); m=maps(&r,&r); CHECK(receiver);
        memcpy(body,&count,4);
        for (unsigned slot=1;slot<=count;slot++) {
            entry.slot=slot; memcpy(body+at,&entry,sizeof entry); at+=sizeof entry;
        }
        h=header(11,2,10,RECMP_UNITS,at); CHECK(tagpu_recmp_units_put(receiver,m,&h,body,at,100));
        for (unsigned slot=1;slot<=count;slot++) CHECK(tagpu_recmp_units_get(receiver,m,slot,&state) && !state.live);
        CHECK(!tagpu_recmp_units_get(receiver,m,count+1,&state));
        tagpu_recmp_units_end(receiver); tagpu_recmp_seats_end(m);
    }
    {
        RecMpRoster r={ .version=1, .count=10, .each=6553, .slots=65531 }, wrong=r;
        RecMpUnitDelta entry={ .slot=65530, .full=1 };
        RecMpUnitState state;
        unsigned count=1;
        for (unsigned i=0;i<10;i++) r.actor[i]=(RecMpSeat){i+1,i,1+6553*i,6553*(i+1)};
        receiver=tagpu_recmp_units_begin(&r); m=maps(&r,&r); CHECK(receiver);
        memcpy(body,&count,4); memcpy(body+4,&entry,sizeof entry);
        h=header(10,2,10,RECMP_UNITS,4+sizeof entry);
        CHECK(tagpu_recmp_units_put(receiver,m,&h,body,h.bytes-sizeof h,100));
        CHECK(tagpu_recmp_units_get(receiver,m,65530,&state) && !state.live);
        CHECK(!tagpu_recmp_units_get(receiver,m,65531,&state));
        entry.slot=65531; memcpy(body+4,&entry,sizeof entry); h.sequence=3;
        CHECK(!tagpu_recmp_units_put(receiver,m,&h,body,h.bytes-sizeof h,101));
        wrong.version=0; CHECK(!tagpu_recmp_units_begin(&wrong));
        tagpu_recmp_units_end(receiver); tagpu_recmp_seats_end(m);
    }
    puts("PASS: guard-page exact spans and all 372 truncations, maximum 201-entry batch, maximum 65531-slot allocation and final-slot bounds");
}

int main(void)
{
    permutations(); mapping_bounds(); cache(); guard_and_capacity();
    return 0;
}
