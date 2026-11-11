#ifndef TAGPU_ALLYSIGHT_H
#define TAGPU_ALLYSIGHT_H
/* Allied sight, explored map, radar and visibility (research/notes/modules/allied-sharing.md). */

#include <stdint.h>

/* Installs every site or none, in DllMain's sequence, before the engine runs. */
void tagpu_allysight_init(void);

/* 1 when owner != viewer, both are below 10 and the snapshot S shares owner's sight and contacts
   with viewer. The game thread, and the loader thread during a load. */
int tagpu_allysight_shares(unsigned owner, unsigned viewer);

/* The replay's table write (tagpu_replay.c, tables_put): begin before the poison and the first
   table is written, restored after the writes. restored returns 1 when S changed and the
   counters were migrated to it. The game thread, between two sim steps. */
void     tagpu_allysight_restore_begin(void);
unsigned tagpu_allysight_restored(void);

/* Record p's wide counters for the sight dump, `*cells` of them, or NULL when S does not manage
   that record's counters: for every cell of a record it returns, the engine's u8 counter is
   min(W, 255). The game thread, between two sim steps. */
const uint32_t* tagpu_allysight_wide(unsigned p, unsigned* cells);

#endif
