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

/* The kill-counts landing's heartbeat line (sim-fixes.md B8), a line of its own: deaths
   carried out and in with the owner's build fraction, copies made with their owner's state,
   the holds, and the 0x12's bound. DLL counters only; returns _snprintf's count, and never
   needs more than TAGPU_KILLS_LINE bytes (a compile-time bound in tagpu_patches.c). */
#define TAGPU_KILLS_LINE 1024
int tagpu_kills_format(char* buf, unsigned int cap);

/* Around a create whose caller then sets the unit's HP or build fraction itself (tacli's
   scenario applier): this thread's messages wait from tagpu_kill_hold until tagpu_kill_flush,
   which sends them with the unit's state as it is then (sim-fixes.md B8). */
void tagpu_kill_hold(void);
void tagpu_kill_flush(void);

/* The demo recorder's view of a destructor call about to run on `unit`: 1 when the transported
   blast will treat it as carried (sim-fixes.md C4) though its carrier link is gone by then --
   the decision tx_wrap armed for this call, or a mark of the kill-all this thread is running.
   The game thread, inside the destructor's entry. */
int tagpu_tx_carried(const char* unit);

/* A recorded death replayed on a puppet (tagpu_replay.c): the destructor 0x4866D0(rec, 0) entered
   as every destructor call of this build is, through tx_wrap, with C4's decision for the call
   armed from the recording (`carried`), as the 0x4C's receiver arms it. `rec` is the destructor's
   record: +1 the unit's slot (u16), +3 the killer's netid, +7 the killer's slot (u16), +9 the
   severity, +0xA the corpse and death kind. The game thread, between two iterations. 0 when the
   wrap could not be built, and nothing was called. */
int tagpu_tx_replay_death(const unsigned char* rec, int carried);
#endif
