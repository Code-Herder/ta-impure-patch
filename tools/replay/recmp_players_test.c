#include "tagpu_recmp_players.h"
#include "tagpu_recmp_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)

static RecMpRoster roster(void)
{
    RecMpRoster r={ .version=1, .count=4, .slots=21, .each=2 };
    unsigned i;
    for (i=0;i<r.count;i++) {
        r.actor[i].origin=(unsigned[]){11,22,55,66}[i];
        r.actor[i].seat=i; r.actor[i].first=1+2*i; r.actor[i].last=2+2*i;
    }
    return r;
}

static RecMpHeader header(unsigned origin, unsigned sequence, unsigned tick, unsigned bytes)
{
    RecMpHeader h={ .version=RECMP_VERSION, .header=sizeof h, .bytes=sizeof h+bytes,
                   .origin=origin, .tick=tick, .sequence=sequence, .kind=RECMP_PLAYER,
                   .match={1}, .stream={origin} };
    memcpy(h.magic,"IMPUREMP",8);
    return h;
}

static void image(const RecMpSeat* seat, unsigned char* out)
{
    unsigned i, first=seat->first+1, last=seat->last+1;
    for (i=0;i<REC_PLAYER_IMAGE;i++) out[i]=(i*37+seat->origin)&255;
    memcpy(out+0x67,&first,4); memcpy(out+0x6B,&last,4);
}

static void rejected(RecMpPlayers* p, const RecMpHeader* h, const void* body,
                      unsigned bytes, unsigned tick)
{
    RecMpPlayerState before={0}, after={0};
    int valid=tagpu_recmp_players_get(p,11,&before);
    CHECK(!tagpu_recmp_players_put(p,h,body,bytes,tick));
    CHECK(tagpu_recmp_players_get(p,11,&after)==valid);
    CHECK(!memcmp(&before,&after,sizeof before));
}

static void roster_bounds(void)
{
    RecMpRoster r=roster(), bad;
    unsigned i;
    CHECK(tagpu_recmp_roster_valid(&r));
    for (i=0;i<18;i++) {
        bad=r;
        switch (i) {
        case 0: bad.version=0; break;
        case 1: bad.count=0; break;
        case 2: bad.count=11; break;
        case 3: bad.each=0; break;
        case 4: bad.each=6554; break;
        case 5: bad.slots++; break;
        case 6: bad.actor[0].origin=0; break;
        case 7: bad.actor[0].seat=10; break;
        case 8: bad.actor[0].first=0; break;
        case 9: bad.actor[0].last=21; break;
        case 10: bad.actor[0].last=0; break;
        case 11: bad.actor[0].last=1; break;
        case 12: bad.actor[0].first=2; bad.actor[0].last=3; break;
        case 13: bad.actor[1].origin=11; break;
        case 14: bad.actor[1].seat=0; break;
        case 15: bad.actor[1].first=1; bad.actor[1].last=2; break;
        case 16: bad.actor[4].origin=99; break;
        case 17: bad.actor[9].last=20; break;
        }
        CHECK(!tagpu_recmp_roster_valid(&bad));
        CHECK(!tagpu_recmp_players_begin(&bad));
    }
    r.count=10; r.each=6553; r.slots=65531;
    for (i=0;i<10;i++) r.actor[i]=(RecMpSeat){i+1,i,1+i*r.each,(i+1)*r.each};
    CHECK(tagpu_recmp_roster_valid(&r));
    CHECK(!tagpu_recmp_roster_valid(NULL));
}

static void seat_maps(void)
{
    unsigned seed=17, trials=0, players=0;
    const unsigned vector[]={0x108,0x113,0x11E,0x129,0x134};
    for (unsigned count=1;count<=RECMP_ACTORS;count++) for (unsigned trial=0;trial<64;trial++) {
        RecMpRoster a={ .version=1, .count=count, .slots=21, .each=2 }, b=a;
        uint8_t map[RECMP_ACTORS+1], inverse[RECMP_ACTORS+1];
        unsigned row[2][RECMP_ACTORS], seen=0;
        for (unsigned peer=0;peer<2;peer++) {
            for (unsigned i=0;i<RECMP_ACTORS;i++) row[peer][i]=i;
            for (unsigned i=RECMP_ACTORS-1;i;i--) {
                unsigned j, save;
                seed=seed*1664525u+1013904223u; j=seed%(i+1);
                save=row[peer][i]; row[peer][i]=row[peer][j]; row[peer][j]=save;
            }
        }
        for (unsigned i=0;i<count;i++) {
            a.actor[i]=(RecMpSeat){i+1,row[0][i],1+2*i,2+2*i};
            b.actor[count-1-i]=(RecMpSeat){i+1,row[1][i],1+2*i,2+2*i};
        }
        CHECK(tagpu_recmp_seat_map(&a,&b,map) && tagpu_recmp_seat_map(&b,&a,inverse));
        for (unsigned i=0;i<=RECMP_ACTORS;i++) {
            CHECK(map[i]<=RECMP_ACTORS && !(seen>>map[i]&1) && inverse[map[i]]==i);
            seen|=1u<<map[i];
        }
        CHECK(map[RECMP_ACTORS]==RECMP_ACTORS);
        for (unsigned i=0;i<count;i++) {
            unsigned char original[REC_PLAYER_IMAGE], mapped[REC_PLAYER_IMAGE+2], restored[REC_PLAYER_IMAGE];
            unsigned origin=a.actor[i].origin, destination=row[1][i];
            image(&a.actor[i],original); original[0x146]=a.actor[i].seat;
            for (unsigned v=0;v<5;v++) for (unsigned r=0;r<=RECMP_ACTORS;r++)
                original[vector[v]+r]=(unsigned char)(1+v*23+r*7);
            memset(mapped,0xA5,sizeof mapped);
            CHECK(tagpu_recmp_player_map(&a,&b,origin,original,sizeof original,mapped+1,REC_PLAYER_IMAGE));
            CHECK(mapped[0]==0xA5 && mapped[sizeof mapped-1]==0xA5 && mapped[1+0x146]==destination);
            for (unsigned v=0;v<5;v++) for (unsigned r=0;r<=RECMP_ACTORS;r++)
                CHECK(mapped[1+vector[v]+map[r]]==original[vector[v]+r]);
            for (unsigned byte=0;byte<REC_PLAYER_IMAGE;byte++) {
                unsigned indexed=byte==0x146;
                for (unsigned v=0;v<5;v++) indexed|=byte>=vector[v] && byte<=vector[v]+RECMP_ACTORS;
                if (!indexed) CHECK(mapped[1+byte]==original[byte]);
            }
            CHECK(tagpu_recmp_player_map(&b,&a,origin,mapped+1,REC_PLAYER_IMAGE,restored,sizeof restored));
            CHECK(!memcmp(restored,original,sizeof original));
            CHECK(tagpu_recmp_player_map(&a,&b,origin,restored,sizeof restored,restored,sizeof restored));
            CHECK(!memcmp(restored,mapped+1,sizeof restored));
            players++;
        }
        trials++;
    }
    printf("PASS: %u seat permutations, %u exact player round trips, active identity, vacant/dummy bijection and in-place mapping\n",
           trials,players);
}

static void mapping_refusals(void)
{
    RecMpRoster a=roster(), b, bad;
    unsigned char original[REC_PLAYER_IMAGE], mapped[REC_PLAYER_IMAGE], map[RECMP_ACTORS+1];
    for (unsigned control=0;control<15;control++) {
        unsigned bytes=sizeof original, capacity=sizeof mapped, origin=11;
        b=a; bad=a; image(&a.actor[0],original); original[0x146]=0;
        memset(mapped,0xA5,sizeof mapped); memset(map,0xA5,sizeof map);
        switch (control) {
        case 0: bad.actor[0].seat=10; break;
        case 1: b.actor[0].origin=99; break;
        case 2: b.actor[0].first=9; b.actor[0].last=10; break;
        case 3: b.count=3; memset(&b.actor[3],0,sizeof b.actor[3]); break;
        case 4: b.each=3; b.slots=31;
            for (unsigned i=0;i<b.count;i++) { b.actor[i].first=1+3*i; b.actor[i].last=3+3*i; }
            break;
        case 5: b.actor[1].seat=0; break;
        case 6: bad.actor[1].origin=11; break;
        case 7: bad.version=0; break;
        case 8: bad.actor[4].origin=99; break;
        case 9: origin=99; break;
        case 10: original[0x146]=10; break;
        case 11: original[0x67]++; break;
        case 12: bytes--; break;
        case 13: bytes++; break;
        case 14: capacity--; break;
        }
        if (control<9) {
            CHECK(!tagpu_recmp_seat_map(&bad,&b,map));
            for (unsigned i=0;i<sizeof map;i++) CHECK(map[i]==0xA5);
        }
        CHECK(!tagpu_recmp_player_map(&bad,&b,origin,original,bytes,mapped,capacity));
        for (unsigned i=0;i<sizeof mapped;i++) CHECK(mapped[i]==0xA5);
    }
    CHECK(!tagpu_recmp_seat_map(NULL,&a,map) && !tagpu_recmp_seat_map(&a,NULL,map) &&
          !tagpu_recmp_seat_map(&a,&a,NULL));
    CHECK(!tagpu_recmp_player_map(&a,&a,11,NULL,sizeof original,mapped,sizeof mapped));
    CHECK(!tagpu_recmp_player_map(&a,&a,11,original,sizeof original,NULL,sizeof mapped));
    puts("PASS: 15 mapping refusal controls preserve destination bytes");
}

static void source_maps(void)
{
    RecMpRoster r=roster(), source=r, copied;
    RecMpSeatMaps* m=tagpu_recmp_seats_begin(&r);
    RecMpHeader h=header(11,1,8,sizeof source), bad;
    GUID stream, wrong={ .Data1=99 };
    unsigned char img[REC_PLAYER_IMAGE], mapped[REC_PLAYER_IMAGE];
    CHECK(m && !tagpu_recmp_seats_get(m,11,NULL,NULL));
    h.kind=RECMP_SEATS;
    for (unsigned i=0;i<r.count;i++) source.actor[i].seat=9-i;
    for (unsigned control=0;control<8;control++) {
        unsigned bytes=sizeof source;
        bad=h;
        switch (control) {
        case 0: bytes--; break;
        case 1: bytes++; break;
        case 2: bad.sequence=2; break;
        case 3: bad.version=1; break;
        case 4: bad.kind=RECMP_PLAYER; break;
        case 5: bad.origin=99; break;
        case 6: memset(&bad.stream,0,sizeof bad.stream); break;
        case 7: bad.magic[0]^=1; break;
        }
        CHECK(!tagpu_recmp_seats_put(m,&bad,&source,bytes));
        CHECK(!tagpu_recmp_seats_get(m,11,NULL,NULL));
    }
    CHECK(tagpu_recmp_seats_put(m,&h,&source,sizeof source));
    CHECK(tagpu_recmp_seats_get(m,11,&copied,&stream) && !memcmp(&copied,&source,sizeof source) &&
          !memcmp(&stream,&h.stream,sizeof stream));
    image(&source.actor[0],img); img[0x146]=9;
    CHECK(tagpu_recmp_seats_image(m,11,&stream,img,sizeof img,mapped,sizeof mapped) && mapped[0x146]==0);
    memset(mapped,0xA5,sizeof mapped);
    CHECK(!tagpu_recmp_seats_image(m,11,&wrong,img,sizeof img,mapped,sizeof mapped));
    CHECK(!tagpu_recmp_seats_image(m,22,&stream,img,sizeof img,mapped,sizeof mapped));
    for (unsigned i=0;i<sizeof mapped;i++) CHECK(mapped[i]==0xA5);
    CHECK(!tagpu_recmp_seats_put(m,&h,&source,sizeof source));
    memset(&source,0,sizeof source); memset(&r,0,sizeof r);
    CHECK(tagpu_recmp_seats_get(m,11,&source,NULL) && !memcmp(&copied,&source,sizeof source));
    tagpu_recmp_seats_end(m);
    puts("PASS: immutable copied source rosters, first-sequence/version/stream prerequisites and unchanged refusals");
}

static void transactions(void)
{
    RecMpRoster r=roster();
    RecMpPlayers* p=tagpu_recmp_players_begin(&r);
    RecMpPlayerState state;
    unsigned char img[REC_PLAYER_IMAGE], body[sizeof(RecMpPlayerDelta)+REC_PLAYER_IMAGE], bad[sizeof body];
    RecMpHeader h, invalid;
    unsigned i, bytes;
    int n;
    CHECK(p && !tagpu_recmp_players_get(p,11,&state));
    image(&r.actor[0],img);
    n=tagpu_recmp_players_delta(p,11,1,img,body,sizeof body);
    CHECK(n==sizeof body && ((RecMpPlayerDelta*)body)->full==1);
    CHECK(!tagpu_recmp_players_get(p,11,&state));
    h=header(11,1,90,n);
    CHECK(tagpu_recmp_players_put(p,&h,body,n,50));
    CHECK(tagpu_recmp_players_get(p,11,&state) && state.controller==1 && state.source_tick==90 &&
          state.sequence==1 && state.applied_tick==50 && !memcmp(state.image,img,sizeof img) && state.stream.Data1==11);
    CHECK(!tagpu_recmp_players_delta(p,11,1,img,body,sizeof body));
    image(&r.actor[1],img);
    n=tagpu_recmp_players_delta(p,22,2,img,body,sizeof body);
    invalid=header(22,7,110,n);
    CHECK(tagpu_recmp_players_put(p,&invalid,body,n,51));
    CHECK(tagpu_recmp_players_get(p,22,&state) && state.controller==2 && !memcmp(state.image,img,sizeof img));
    CHECK(tagpu_recmp_players_get(p,11,&state) && state.source_tick==90 && state.applied_tick==50);

    image(&r.actor[0],img);
    /* Exercise all value words, including the PlayerInfo tail and image padding.
       The two block IDs are immutable and may straddle a word boundary. */
    for (i=0;i<REC_PLAYER_DWORDS;i++) {
        if (i*4<0x6F && i*4+4>0x67) continue;
        img[i*4]^=1;
        n=tagpu_recmp_players_delta(p,11,1,img,body,sizeof body);
        CHECK(n==sizeof(RecMpPlayerDelta)+4 && !((RecMpPlayerDelta*)body)->full);
        h.sequence+=2; h.tick++; h.bytes=sizeof h+n;
        CHECK(tagpu_recmp_players_put(p,&h,body,n,52+i));
        CHECK(tagpu_recmp_players_get(p,11,&state) && !memcmp(state.image,img,sizeof img));
    }
    n=tagpu_recmp_players_delta(p,11,2,img,body,sizeof body);
    CHECK(n==sizeof(RecMpPlayerDelta));
    h.sequence++; h.bytes=sizeof h+n;
    CHECK(tagpu_recmp_players_put(p,&h,body,n,200));
    CHECK(tagpu_recmp_players_get(p,11,&state) && state.controller==2);
    rejected(p,&h,body,n,200);
    img[0]^=1;
    n=tagpu_recmp_players_delta(p,11,2,img,body,sizeof body);
    h.sequence++; h.tick++; h.bytes=sizeof h+n;
    for (i=0;i<13;i++) {
        memcpy(bad,body,n); invalid=h; bytes=n;
        switch (i) {
        case 0: ((RecMpPlayerDelta*)bad)->origin=22; break;
        case 1: ((RecMpPlayerDelta*)bad)->controller=0; break;
        case 2: ((RecMpPlayerDelta*)bad)->controller=3; break;
        case 3: ((RecMpPlayerDelta*)bad)->full=2; break;
        case 4: ((RecMpPlayerDelta*)bad)->reserved[2]=1; break;
        case 5: ((RecMpPlayerDelta*)bad)->bits[16]|=4; break;
        case 6: ((RecMpPlayerDelta*)bad)->full=1; break;
        case 7: invalid.sequence=0; break;
        case 8: invalid.tick=0; break;
        case 9: invalid.stream.Data1++; break;
        case 10: invalid.header--; break;
        case 11: invalid.bytes--; break;
        case 12: bytes++; bad[n]=0; invalid.bytes++; break;
        }
        rejected(p,&invalid,bad,bytes,201);
    }
    rejected(p,&h,body,n,199);
    CHECK(tagpu_recmp_players_put(p,&h,body,n,201));
    img[0x67]^=1;
    CHECK(tagpu_recmp_players_delta(p,11,2,img,body,sizeof body)==-1);
    CHECK(tagpu_recmp_players_delta(p,99,2,img,body,sizeof body)==-1);
    CHECK(!tagpu_recmp_players_get(p,99,&state));
    tagpu_recmp_players_end(p);
}

static void guard_bounds(void)
{
    RecMpRoster r=roster();
    RecMpPlayers* p=tagpu_recmp_players_begin(&r);
    unsigned char img[REC_PLAYER_IMAGE], body[sizeof(RecMpPlayerDelta)+REC_PLAYER_IMAGE];
    unsigned char* pages=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    RecMpHeader h=header(11,1,9,sizeof body);
    DWORD protection;
    unsigned i;
    CHECK(p && pages && VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&protection));
    image(&r.actor[0],img);
    img[0x146]=r.actor[0].seat;
    memcpy(pages+4096-sizeof img,img,sizeof img);
    CHECK(tagpu_recmp_player_map(&r,&r,11,pages+4096-sizeof img,sizeof img,
                                 pages+4096-sizeof img,sizeof img));
    CHECK(!memcmp(pages+4096-sizeof img,img,sizeof img));
    CHECK(!tagpu_recmp_player_map(&r,&r,11,pages+4096,sizeof img-1,img,sizeof img));
    CHECK(!tagpu_recmp_player_map(&r,&r,11,pages+4096,sizeof img,img,sizeof img-1));
    CHECK(tagpu_recmp_players_delta(p,11,1,pages+4096-sizeof img,body,sizeof body)==sizeof body);
    CHECK(tagpu_recmp_players_delta(p,11,1,img,pages+4096,sizeof body-1)==-1);
    for (i=0;i<sizeof body;i++) {
        memcpy(pages+4096-i,body,i); h.bytes=sizeof h+i;
        rejected(p,&h,pages+4096-i,i,10);
    }
    memcpy(pages+4096-sizeof body,body,sizeof body); h.bytes=sizeof h+sizeof body;
    CHECK(tagpu_recmp_players_put(p,&h,pages+4096-sizeof body,sizeof body,10));
    CHECK(!tagpu_recmp_players_delta(p,11,1,img,pages+4096,0));
    h.sequence++;
    rejected(p,&h,pages+4096,sizeof body+1,11);
    memcpy(body+sizeof(RecMpPlayerDelta)+0x67,"bad!",4);
    rejected(p,&h,body,sizeof body,11);
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
    tagpu_recmp_players_end(p);
}

static void persisted(const char* path)
{
    RecMpRoster r=roster();
    RecMpConfig cfg={ .origin=11, .count=4, .net={ .match={1} } };
    RecMpPlayers* source=tagpu_recmp_players_begin(&r);
    RecMpPlayers* restored=tagpu_recmp_players_begin(&r);
    RecMpFileState ledger;
    RecMpJournal* j;
    RecMpRecorded record;
    RecMpStream stream[4]={{0}};
    unsigned char img[REC_PLAYER_IMAGE], body[sizeof(RecMpPlayerDelta)+REC_PLAYER_IMAGE];
    unsigned i, round, size, phase;
    RecMpPlayerState state;
    FILE* file;
    for (i=0;i<4;i++) cfg.actor[i]=(RecMpActor){ .origin=r.actor[i].origin, .local=i<2 };
    j=tagpu_recmp_record_begin(&cfg);
    CHECK(j && source && restored && tagpu_recmp_file_state(&ledger,tagpu_recmp_record_header(j)));
    file=fopen(path,"wb"); CHECK(file);
    CHECK(fwrite(tagpu_recmp_record_header(j),112,1,file)==1);
    for (round=0;round<2;round++) for (i=0;i<4;i++) {
        RecMpHeader h;
        image(&r.actor[i],img); img[0]^=round;
        size=tagpu_recmp_players_delta(source,r.actor[i].origin,1+(i&1),img,body,sizeof body);
        CHECK(size==(round ? sizeof(RecMpPlayerDelta)+4 : sizeof body));
        h=header(r.actor[i].origin,round+1,30+round,size);
        CHECK(tagpu_recmp_stream_put(&stream[i],&h,body,size));
        CHECK(tagpu_recmp_players_put(source,&h,body,size,100+round));
        phase=i<2 ? RECMP_RECORD_PRODUCED : RECMP_RECORD_CONSUMED;
        CHECK(tagpu_recmp_record_put(j,&h,body,size,100+round,phase));
    }
    for (i=0;i<4;i++) {
        RecMpSeal seal;
        RecMpHeader h=header(r.actor[i].origin,3,31,sizeof seal); h.kind=RECMP_SEAL;
        CHECK(tagpu_recmp_stream_seal(&stream[i],31,1,&seal));
        CHECK(tagpu_recmp_record_put(j,&h,&seal,sizeof seal,102,i<2 ? RECMP_RECORD_PRODUCED : RECMP_RECORD_CONSUMED));
    }
    tagpu_recmp_record_end(j);
    while (tagpu_recmp_record_take(j,&record)==1) {
        size=record.packet.h.bytes-sizeof record.packet.h;
        CHECK(tagpu_recmp_file_record(&ledger,&record.where,&record.packet.h,record.packet.body,size));
        if (record.packet.h.kind==RECMP_PLAYER)
            CHECK(tagpu_recmp_players_put(restored,&record.packet.h,record.packet.body,size,record.where.tick));
        CHECK(fwrite(&record.where,sizeof record.where,1,file)==1 && fwrite(&record.packet,record.packet.h.bytes,1,file)==1);
    }
    CHECK(tagpu_recmp_record_done(j) && ledger.records==12 && ledger.consumed==6);
    for (i=0;i<4;i++) {
        image(&r.actor[i],img); img[0]^=1;
        CHECK(tagpu_recmp_players_get(restored,r.actor[i].origin,&state) && state.sequence==2 &&
              state.source_tick==31 && state.applied_tick==101 && state.controller==1+(i&1) &&
              !memcmp(state.image,img,sizeof img) && ledger.source[i].ended);
    }
    CHECK(!fclose(file));
    tagpu_recmp_record_free(j);
    tagpu_recmp_players_end(source); tagpu_recmp_players_end(restored);
}

int main(int argc, char** argv)
{
    CHECK(argc==2);
    roster_bounds(); seat_maps(); mapping_refusals(); source_maps(); transactions(); guard_bounds(); persisted(argv[1]);
    puts("PASS: full owner player images, human/AI isolation, transactional deltas, clocks/stream provenance, bounded rosters, guard-page truncations and persisted journal reconstruction");
    return 0;
}
