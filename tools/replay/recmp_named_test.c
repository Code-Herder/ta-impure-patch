#include "tagpu_recmp_named.h"
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

static unsigned image(unsigned origin, unsigned serial, unsigned names, unsigned char* b)
{
    unsigned id=origin==11 ? 2 : 4;
    memset(b,0,24+names); memcpy(b,&id,4);
    b[4]=4; b[5]=1;
    memcpy(b+8,&serial,4);
    for (unsigned i=0;i<names;i++) b[24+i]=(unsigned char)('A'+i%26);
    return 24+names;
}

static unsigned batch(unsigned count, unsigned names, unsigned char* b)
{
    unsigned at=4;
    memcpy(b,&count,4);
    for (unsigned i=0;i<count;i++) {
        RecMpNamedEntry e={24+names,i&1,0,17+i};
        memcpy(b+at,&e,sizeof e); at+=sizeof e;
        at+=image(11,i,names,b+at);
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
    RecMpSeatMaps* m=maps(&r);
    RecMpNamed* c=tagpu_recmp_named_begin(&r);
    unsigned char valid[RECMP_BODY_MAX+1], bad[RECMP_BODY_MAX+1];
    unsigned bytes=batch(2,13,valid);
    RecMpHeader h=header(11,2,10,RECMP_NAMED,bytes);
    RecMpNamedState before, after;
    CHECK(c && tagpu_recmp_named_put(c,m,&h,valid,bytes,100));
    CHECK(tagpu_recmp_named_get(c,11,&before));
    CHECK(before.count==2 && before.source_tick==10 && before.applied_tick==100 &&
          !memcmp(before.body,valid,bytes)); /* Birth is a monotonic token, not a tick bound. */
    memset(valid,0xCC,bytes);
    CHECK(tagpu_recmp_named_get(c,11,&after) && !memcmp(&before,&after,sizeof before));
    bytes=batch(2,13,valid);
    for (unsigned control=0;control<28;control++) {
        unsigned n=bytes, applied=101;
        RecMpHeader badh=header(11,3,11,RECMP_NAMED,bytes);
        memcpy(bad,valid,bytes);
        switch (control) {
        case 0: memset(bad,0,4); break;
        case 1: bad[0]=123; break;
        case 2: bad[4]=24; break;
        case 3: bad[4]=57; break;
        case 4: bad[6]=2; break;
        case 5: bad[7]=1; break;
        case 6: bad[11]=0x80; break;
        case 7: bad[12]=0; break;
        case 8: bad[12]=4; break;
        case 9: bad[16]=5; break;
        case 10: bad[17]=2; break;
        case 11: bad[18]=1; break;
        case 12: bad[19]=1; break;
        case 13: bad[36]=0; break;
        case 14: bad[bytes]=1; n++; break;
        case 15: badh.kind=RECMP_UNITS; break;
        case 16: badh.version--; break;
        case 17: badh.header--; break;
        case 18: badh.magic[0]=0; break;
        case 19: badh.sequence=0; break;
        case 20: memset(&badh.stream,0,sizeof badh.stream); break;
        case 21: badh.origin=99; break;
        case 22: badh.sequence=2; break;
        case 23: badh.tick=9; break;
        case 24: applied=99; break;
        case 25: badh.stream.Data1=99; break;
        case 26: bad[4+8+37+3]=1; break;
        case 27: n=RECMP_BODY_MAX+1; break;
        }
        badh.bytes=sizeof badh+n;
        CHECK(!tagpu_recmp_named_put(c,m,&badh,bad,n,applied));
        CHECK(tagpu_recmp_named_get(c,11,&after) && !memcmp(&before,&after,sizeof before));
    }
    {
        unsigned char* guard=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        DWORD old;
        CHECK(guard && VirtualProtect(guard+4096,4096,PAGE_NOACCESS,&old));
        for (unsigned n=0;n<bytes;n++) {
            RecMpHeader badh=header(11,3,11,RECMP_NAMED,n);
            unsigned char* b=guard+4096-n;
            memcpy(b,valid,n);
            CHECK(!tagpu_recmp_named_put(c,m,&badh,b,n,101));
            CHECK(tagpu_recmp_named_get(c,11,&after) && !memcmp(&before,&after,sizeof before));
        }
        VirtualFree(guard,0,MEM_RELEASE);
    }
    bytes=batch(122,1,valid); h=header(11,3,11,RECMP_NAMED,bytes);
    CHECK(bytes==4030 && tagpu_recmp_named_put(c,m,&h,valid,bytes,101));
    bytes=batch(62,32,valid); h=header(11,4,12,RECMP_NAMED,bytes);
    CHECK(tagpu_recmp_named_put(c,m,&h,valid,bytes,102));
    memset(&after,0xA5,sizeof after); CHECK(!tagpu_recmp_named_get(c,99,&after));
    for (unsigned i=0;i<sizeof after;i++) CHECK(((unsigned char*)&after)[i]==0xA5);
    tagpu_recmp_named_end(c); tagpu_recmp_seats_end(m);
    puts("PASS: copied named roots, future-valued incarnation tokens, row permutations, 28 atomic refusals, all 94 truncations and maximum batches");
}

int main(void) { codec(); return 0; }
