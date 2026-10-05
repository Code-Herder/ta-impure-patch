#include "tagpu_recmp_file.h"
#include <string.h>

struct RecMpFile {
    RecMpJournal* journal;
    RecMpChannel* channel;
    RecMpRoster roster;
    volatile LONG ended;
};

RecMpFile* tagpu_recmp_file_begin(const RecMpConfig* config, RecMpChannel* channel,
                                 const RecMpRoster* roster)
{
    RecMpFile* f;
    if (!channel || !tagpu_recmp_roster_valid(roster)) return NULL;
    f=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *f);
    if (!f) return NULL;
    f->journal=tagpu_recmp_record_begin(config);
    if (!f->journal || !tagpu_recmp_file_roster_valid(tagpu_recmp_record_header(f->journal),roster) ||
        !tagpu_recmp_record_attach(channel,f->journal)) {
        tagpu_recmp_record_free(f->journal); HeapFree(GetProcessHeap(),0,f); return NULL;
    }
    f->roster=*roster;
    tagpu_recmp_retain(channel); f->channel=channel;
    return f;
}

const RecMpRoster* tagpu_recmp_file_roster(const RecMpFile* f) { return f ? &f->roster : NULL; }

const RecMpRecordHeader* tagpu_recmp_file_header(const RecMpFile* f)
{
    return f ? tagpu_recmp_record_header(f->journal) : NULL;
}

RecMpJournal* tagpu_recmp_file_journal(RecMpFile* f) { return f ? f->journal : NULL; }

void tagpu_recmp_file_end(RecMpFile* f, int channel_ended)
{
    if (!f) return;
    tagpu_recmp_record_detach(f->channel,f->journal);
    tagpu_recmp_record_end(f->journal);
    InterlockedExchange(&f->ended,channel_ended ? 1 : 2);
}

unsigned tagpu_recmp_file_ended(const RecMpFile* f)
{
    return f ? (unsigned)InterlockedCompareExchange((volatile LONG*)&f->ended,0,0) : 0;
}

int tagpu_recmp_file_proof(RecMpFile* f, RecMpPacket* packet)
{
    if (!f || tagpu_recmp_file_ended(f)!=1 || !tagpu_recmp_record_done(f->journal)) return 0;
    return tagpu_recmp_take(f->channel,packet);
}

int tagpu_recmp_file_complete(RecMpFile* f, const RecMpFileState* s, RecMpRecordEnd* end)
{
    RecMpStats stats;
    RecMpRecordEnd next={0};
    unsigned i;
    if (!f || !s || !end || tagpu_recmp_file_ended(f)!=1 || !tagpu_recmp_record_done(f->journal)) return 0;
    tagpu_recmp_stats(f->channel,&stats);
    if (stats.failed) return -1;
    if (!stats.finished) return 0;
    next.version=1; next.count=s->header.count; next.records=s->records;
    for (i=0;i<next.count;i++) if (!s->header.actor[i].local &&
        !tagpu_recmp_cut(f->channel,s->header.actor[i].origin,&next.cut[i])) return -1;
    if (!tagpu_recmp_file_footer(s,&next)) return -1;
    *end=next;
    return 1;
}

void tagpu_recmp_file_free(RecMpFile* f)
{
    if (!f) return;
    tagpu_recmp_record_free(f->journal);
    tagpu_recmp_release(f->channel);
    HeapFree(GetProcessHeap(),0,f);
}
