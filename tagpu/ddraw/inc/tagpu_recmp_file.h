#ifndef TAGPU_RECMP_FILE_H
#define TAGPU_RECMP_FILE_H

#include "tagpu_recmp_record.h"
#include "tagpu_recmp_stream.h"
#include "tagpu_recmp_players.h"

/* A footer carries application cuts, not proof-tail application. Local rows are
   zero; their final source seal is checked in the persisted packet ledger. */
#pragma pack(push, 1)
typedef struct {
    uint32_t version, count;
    uint64_t records;
    RecMpCutPoint cut[RECMP_ACTORS];
} RecMpRecordEnd;
#pragma pack(pop)
_Static_assert(sizeof(RecMpRecordEnd)==416, "contribution footer span");

typedef struct {
    RecMpRecordHeader header;
    RecMpStream source[RECMP_ACTORS];
    GUID stream[RECMP_ACTORS];
    RecMpCutPoint cut[RECMP_ACTORS];
    uint64_t records, consumed;
    unsigned tick, proof;
} RecMpFileState;

/* Writer and reader independently reconstruct these bounded ledgers from file
   bytes. Refusal leaves the state unchanged. No authorization key is persisted. */
int tagpu_recmp_file_state(RecMpFileState* s, const RecMpRecordHeader* header);
int tagpu_recmp_file_roster_valid(const RecMpRecordHeader* header, const RecMpRoster* roster);
int tagpu_recmp_file_record(RecMpFileState* s, const RecMpRecord* where,
                            const RecMpHeader* h, const void* body, unsigned bytes);
int tagpu_recmp_file_block(RecMpFileState* s, const void* body, unsigned bytes);
int tagpu_recmp_file_footer(const RecMpFileState* s, const RecMpRecordEnd* end);

typedef struct RecMpFile RecMpFile;
/* Before any channel publication/consumption. The returned context owns a fresh
   attached journal and a retained channel reference. Transfer it to RecRing
   before starting that ring's writer. */
RecMpFile* tagpu_recmp_file_begin(const RecMpConfig* config, RecMpChannel* channel,
                                 const RecMpRoster* roster);
const RecMpRecordHeader* tagpu_recmp_file_header(const RecMpFile* f);
const RecMpRoster* tagpu_recmp_file_roster(const RecMpFile* f);
RecMpJournal* tagpu_recmp_file_journal(RecMpFile* f);
/* Producer's final access. channel_ended is true only after the game has called
   channel end; otherwise detach the failed local recording while play continues.
   No other game producer/consumer access may follow channel_ended. */
void tagpu_recmp_file_end(RecMpFile* f, int channel_ended);
/* Writer: 0 producer owns channel consumer; 1 handed over; 2 local save stopped. */
unsigned tagpu_recmp_file_ended(const RecMpFile* f);
/* Only after ended==1 and application journal drain. */
int tagpu_recmp_file_proof(RecMpFile* f, RecMpPacket* packet);
int tagpu_recmp_file_complete(RecMpFile* f, const RecMpFileState* s, RecMpRecordEnd* end);
/* Only after acquiring nonzero ended. No game access remains. */
void tagpu_recmp_file_free(RecMpFile* f);

#endif
