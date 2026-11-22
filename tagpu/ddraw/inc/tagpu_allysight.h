#ifndef TAGPU_ALLYSIGHT_H
#define TAGPU_ALLYSIGHT_H
/* Allied sight, explored map, radar and visibility (research/notes/modules/allied-sharing.md). */

#include <stdint.h>

/* Installs every site or none, in DllMain's sequence, before the engine runs. */
void tagpu_allysight_init(void);
/* 1 once the init has redirected every site, the step's players-phase call 0x49555F among them: what a module
   whose per-step work players_phase calls (tagpu_scatter_tick) asks before it installs anything of its own.
   Set at DllMain before any thread of the game runs. */
int tagpu_allysight_steps(void);

/* 1 when owner != viewer, both are below 10 and the snapshot S shares owner's sight and contacts
   with viewer. The game thread, and the loader thread during a load. */
int tagpu_allysight_shares(unsigned owner, unsigned viewer);

/* 1 when the radar pass gives viewer the contacts of owner's units, walks owner's radar and sonar
   units for viewer and skips owner's jammers: tagpu_allysight_shares, and the radar pass's sites
   installed. The game thread. */
int tagpu_allysight_contacts(unsigned owner, unsigned viewer);

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
