/* Missing tails, corrupt manifests and closed source ticks must refuse a prefix. */
#include "tagpu_recmp_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)

static RecMpHeader head(unsigned origin, unsigned sequence, unsigned tick,
                          unsigned kind, unsigned bytes)
{
    RecMpHeader h = {0};
    memcpy(h.magic,"IMPUREMP",8);
    h.version=RECMP_VERSION; h.header=sizeof h; h.bytes=sizeof h+bytes;
    h.origin=origin; h.sequence=sequence; h.tick=tick; h.kind=kind;
    h.match.Data1=1; h.stream.Data1=origin;
    return h;
}

static void rejects(RecMpStream* s, const RecMpHeader* h, const void* body, unsigned bytes)
{
    RecMpStream before=*s;
    CHECK(!tagpu_recmp_stream_put(s,h,body,bytes));
    CHECK(!memcmp(&before,s,sizeof before));
}

int main(void)
{
    RecMpStream owner={0}, peer={0}, missing={0}, altered={0}, other={0}, empty={0};
    RecMpStream full, before;
    RecMpHeader first=head(55,1,8,RECMP_RESOURCES,4), second, marker, data;
    RecMpSeal seal, bad, unchanged, human_frame;
    unsigned char* pages;
    DWORD protection;
    void* at;
    unsigned i;
    CHECK(tagpu_recmp_stream_put(&owner,&first,"peer",4));
    CHECK(tagpu_recmp_stream_put(&peer,&first,"peer",4));
    CHECK(tagpu_recmp_stream_put(&missing,&first,"peer",4));
    CHECK(tagpu_recmp_stream_put(&altered,&first,"peEr",4));
    CHECK(!owner.ended && !owner.has_seal);     /* receiving data never proves its final tail */
    second=head(55,2,8,RECMP_ORDERS,4);
    CHECK(tagpu_recmp_stream_put(&owner,&second,"tail",4));
    CHECK(tagpu_recmp_stream_put(&peer,&second,"tail",4));
    CHECK(tagpu_recmp_stream_put(&altered,&second,"tail",4));
    CHECK(tagpu_recmp_stream_seal(&owner,8,0,&seal));
    human_frame=seal;
    marker=head(55,3,8,RECMP_SEAL,sizeof seal);
    rejects(&missing,&marker,&seal,sizeof seal); /* valid manifest follows a missing last DATA */
    rejects(&altered,&marker,&seal,sizeof seal); /* same counts, different carried bytes */
    CHECK(!missing.ended && !altered.ended);
    for (i=0;i<6;i++) {
        bad=seal;
        if (i==0) bad.previous++;
        if (i==1) bad.tick++;
        if (i==2) bad.data_bytes++;
        if (i==3) bad.crc^=1;
        if (i==4) bad.final=2;
        if (i==5) marker.sequence++;
        rejects(&peer,&marker,&bad,sizeof bad);
        marker.sequence=3;
    }
    pages=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(pages && VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&protection));
    at=pages+4096-sizeof seal; memcpy(at,&seal,sizeof seal);
    data=marker; data.bytes=sizeof data+sizeof seal-1;
    rejects(&peer,&data,pages+4096,sizeof seal-1);
    data.bytes=sizeof data+sizeof seal+1;
    rejects(&peer,&data,at,sizeof seal+1);
    CHECK(tagpu_recmp_stream_put(&peer,&marker,at,sizeof seal));
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
    CHECK(tagpu_recmp_stream_put(&owner,&marker,&seal,sizeof seal));
    CHECK(peer.has_seal && !peer.ended && peer.sealed_tick==8 && peer.sealed_sequence==3);
    CHECK(peer.data_bytes==2*(sizeof first+4));
    CHECK(!memcmp(&owner,&peer,sizeof owner));
    data=head(55,4,8,RECMP_RESOURCES,4);
    rejects(&owner,&data,"late",4);              /* a closed capture tick cannot receive new data */
    data.tick=7; rejects(&owner,&data,"late",4);
    data.tick=9;
    CHECK(tagpu_recmp_stream_put(&owner,&data,"next",4));
    CHECK(tagpu_recmp_stream_put(&peer,&data,"next",4));
    CHECK(tagpu_recmp_stream_seal(&owner,9,1,&seal));
    marker=head(55,5,9,RECMP_SEAL,sizeof seal);
    CHECK(tagpu_recmp_stream_put(&owner,&marker,&seal,sizeof seal));
    CHECK(tagpu_recmp_stream_put(&peer,&marker,&seal,sizeof seal));
    CHECK(owner.ended && peer.ended && !memcmp(&owner,&peer,sizeof owner));
    data=head(55,6,10,RECMP_RESOURCES,4);
    rejects(&owner,&data,"late",4);
    rejects(&peer,&marker,&seal,sizeof seal);
    memset(&unchanged,0xA5,sizeof unchanged); bad=unchanged;
    CHECK(!tagpu_recmp_stream_seal(&owner,10,1,&unchanged));
    CHECK(!memcmp(&bad,&unchanged,sizeof bad));
    CHECK(tagpu_recmp_stream_seal(&empty,0,0,&seal));
    marker=head(66,1,0,RECMP_SEAL,sizeof seal);
    CHECK(tagpu_recmp_stream_put(&empty,&marker,&seal,sizeof seal));
    CHECK(tagpu_recmp_stream_seal(&empty,0,1,&seal));
    marker.sequence=2;
    CHECK(tagpu_recmp_stream_put(&empty,&marker,&seal,sizeof seal) && empty.ended);
    data=first; data.origin=66; data.stream.Data1=66;
    CHECK(tagpu_recmp_stream_put(&other,&data,"peer",4));
    data=second; data.origin=66; data.stream.Data1=66;
    CHECK(tagpu_recmp_stream_put(&other,&data,"tail",4));
    marker=head(66,3,8,RECMP_SEAL,sizeof seal);
    rejects(&other,&marker,&human_frame,sizeof human_frame);
    full=missing; full.sequence=UINT32_MAX;
    data=head(55,0,9,RECMP_RESOURCES,4);
    rejects(&full,&data,"wrap",4);
    CHECK(!tagpu_recmp_stream_seal(&full,9,1,&seal));
    full=missing; full.data_bytes=UINT64_MAX-1;
    data=head(55,2,9,RECMP_RESOURCES,4);
    rejects(&full,&data,"size",4);
    before=missing;
    CHECK(!tagpu_recmp_stream_put(&missing,&data,NULL,4));
    CHECK(!memcmp(&before,&missing,sizeof before));
    puts("PASS: exact sealed prefixes, missing tail/corrupt data refusal, manifest and guard-page bounds, closed ticks, empty/final sources, count/sequence overflow, unchanged state on refusal");
    return 0;
}
