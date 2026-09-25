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
#endif
