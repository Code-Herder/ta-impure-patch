#include "tagpu_recmp_record.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)

static RecMpHeader header(unsigned sequence, unsigned bytes)
{
    RecMpHeader h={0};
    memcpy(h.magic,"IMPUREMP",8); h.version=RECMP_VERSION;
    h.header=sizeof h; h.bytes=sizeof h+bytes;
    h.kind=RECMP_RESOURCES; h.origin=55; h.sequence=sequence; h.tick=8;
    h.match.Data1=1; h.stream.Data1=2;
    return h;
}

static RecMpJournal* begin(void)
{
    RecMpConfig config={0};
    RecMpJournal* j;
    config.origin=11; config.count=2; config.net.match.Data1=1;
    config.actor[0].origin=11; config.actor[0].local=1;
    config.actor[1].origin=55;
    j=tagpu_recmp_record_begin(&config); CHECK(j);
    memset(&config,0,sizeof config);
    return j;
}

typedef struct {
    RecMpJournal* journal;
    HANDLE batch, done;
} Reader;

static DWORD WINAPI reader(void* arg)
{
    Reader* owner=arg;
    RecMpJournal* j=owner->journal;
    RecMpRecorded got;
    unsigned n=0;
    DWORD deadline=GetTickCount()+30000;
    while (n<15000) {
        int taken=tagpu_recmp_record_take(j,&got);
        CHECK(taken>=0 && (LONG)(deadline-GetTickCount())>0);
        if (!taken) { Sleep(1); continue; }
        CHECK(got.where.ordinal==n+1 && got.where.tick==100+n &&
              got.where.phase==RECMP_RECORD_CONSUMED && got.packet.h.sequence==n+1);
        for (unsigned k=0;k<got.packet.h.bytes-sizeof got.packet.h;k++)
            CHECK(got.packet.body[k]==(unsigned char)n);
        n++;
        if (!(n%1024) || n==15000) SetEvent(owner->batch);
    }
    while (!tagpu_recmp_record_done(j)) {
        CHECK((LONG)(deadline-GetTickCount())>0); Sleep(1);
    }
    tagpu_recmp_record_free(j);
    SetEvent(owner->done);
    return 0;
}

static void threaded(void)
{
    Reader owner={begin(),CreateEvent(NULL,FALSE,FALSE,NULL),CreateEvent(NULL,TRUE,FALSE,NULL)};
    HANDLE thread;
    unsigned char body[RECMP_BODY_MAX];
    CHECK(owner.batch && owner.done);
    thread=CreateThread(NULL,0,reader,&owner,0,NULL); CHECK(thread);
    for (unsigned i=0;i<15000;i++) {
        unsigned bytes=i%3 ? sizeof body : 7;
        RecMpHeader h=header(i+1,bytes);
        memset(body,(unsigned char)i,bytes);
        CHECK(tagpu_recmp_record_put(owner.journal,&h,body,bytes,100+i,RECMP_RECORD_CONSUMED));
        memset(body,0xEE,bytes);
        /* A consumed-batch event bounds outstanding bytes by construction,
           independent of which thread gets more CPU time. */
        if (!((i+1)%1024) || i==14999) CHECK(WaitForSingleObject(owner.batch,30000)==WAIT_OBJECT_0);
    }
    tagpu_recmp_record_end(owner.journal);  /* final producer access; reader frees */
    CHECK(WaitForSingleObject(owner.done,30000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(thread,30000)==WAIT_OBJECT_0);
    CloseHandle(thread); CloseHandle(owner.batch); CloseHandle(owner.done);
}

int main(void)
{
    RecMpJournal* j=begin();
    RecMpRecorded got;
    unsigned char body[RECMP_BODY_MAX];
    RecMpHeader h;
    unsigned i, count=0;
    CHECK(tagpu_recmp_record_header(j)->recorder==11);
    CHECK(tagpu_recmp_record_claim(j) && !tagpu_recmp_record_claim(j));
    /* Physical wraps must preserve copied odd-length bodies and one ordinal
       across all record sizes. */
    for (i=0;i<15000;i++) {
        unsigned bytes=i%3 ? sizeof body : 7;
        memset(body,(unsigned char)i,bytes); h=header(i+1,bytes);
        CHECK(tagpu_recmp_record_put(j,&h,body,bytes,100+i,RECMP_RECORD_CONSUMED));
        memset(body,0xEE,bytes);
        CHECK(tagpu_recmp_record_take(j,&got)==1 && got.where.ordinal==i+1 &&
              got.where.tick==100+i && got.where.phase==RECMP_RECORD_CONSUMED &&
              !memcmp(&got.packet.h,&h,sizeof h));
        for (unsigned k=0;k<bytes;k++) CHECK(got.packet.body[k]==(unsigned char)i);
        CHECK(!tagpu_recmp_record_take(j,&got));
    }
    CHECK(!tagpu_recmp_record_done(j)); tagpu_recmp_record_end(j);
    CHECK(tagpu_recmp_record_done(j) && !tagpu_recmp_record_put(j,&h,body,sizeof body,1,2));
    tagpu_recmp_record_free(j);
    j=begin(); h=header(1,sizeof body);
    while (count<5000 && tagpu_recmp_record_put(j,&h,body,sizeof body,1,RECMP_RECORD_CONSUMED)) count++;
    CHECK(count>4000 && count<5000 && tagpu_recmp_record_take(j,&got)==-1);
    CHECK(!tagpu_recmp_record_done(j)); tagpu_recmp_record_end(j);
    CHECK(tagpu_recmp_record_done(j)); tagpu_recmp_record_free(j);
    for (i=0;i<5;i++) {
        j=begin(); h=header(1,7);
        if (i==0) h.origin=999;
        if (i==1) h.match.Data1++;
        if (i==2) h.bytes++;
        if (i==3) h.kind=RECMP_BIND;
        if (i==4) h.sequence=0;
        CHECK(!tagpu_recmp_record_put(j,&h,body,7,100,RECMP_RECORD_CONSUMED));
        CHECK(tagpu_recmp_record_take(j,&got)==-1);
        tagpu_recmp_record_end(j); tagpu_recmp_record_free(j);
    }
    j=begin(); h=header(1,7);
    CHECK(!tagpu_recmp_record_put(j,&h,body,7,100,RECMP_RECORD_PRODUCED));
    tagpu_recmp_record_end(j); tagpu_recmp_record_free(j);
    threaded();
    puts("PASS: copied source/application journal, physical wraps, bounded overflow, frozen producer hand-over and role/match/span refusal");
    return 0;
}
