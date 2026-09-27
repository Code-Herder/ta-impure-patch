#ifndef TAGPU_PATCHES_H
#define TAGPU_PATCHES_H
/* Apply our small engine byte-patches to the loaded TotalA.exe image. Called once
   from DllMain (before TA's startup code runs). All patches are guarded: a byte is
   only written if it currently holds the expected value, so a different build is
   left untouched. The exe on disk is never modified. */
void tagpu_apply_patches(void);

/* The wire-robustness landing's heartbeat section (sim-fixes.md B3): records accepted per
   receiver, drops, the morph/recreate/ghost oracles (evidence §10) and the sender-block observe.
   DLL counters only; returns _snprintf's count. */
int tagpu_wire_format(char* buf, unsigned int cap);

/* The stale-hits landing's heartbeat section (sim-fixes.md B4): companions out and in, the
   owner's and the bystanders' verdicts, bare messages dropped, copies' stamps, the hold, and
   the tagpu_dmgdelay.on oracle. DLL counters only; returns _snprintf's count. */
int tagpu_hits_format(char* buf, unsigned int cap);

/* The ghost-commander landing's heartbeat section (sim-fixes.md B5): creates held during the
   load, replayed or dropped at the in-play entry, and the dirty creates' positions by kind.
   DLL counters only; returns _snprintf's count. */
int tagpu_ghost_format(char* buf, unsigned int cap);

/* The kill-counts landing's heartbeat section (sim-fixes.md B8): deaths carried out and in with
   the owner's build fraction, copies made with their owner's state, the holds, and the 0x12's
   bound. DLL counters only; returns _snprintf's count. */
int tagpu_kills_format(char* buf, unsigned int cap);

/* Around a create whose caller then sets the unit's HP or build fraction itself (tacli's
   scenario applier): this thread's messages wait from tagpu_kill_hold until tagpu_kill_flush,
   which sends them with the unit's state as it is then (sim-fixes.md B8). */
void tagpu_kill_hold(void);
void tagpu_kill_flush(void);
#endif
