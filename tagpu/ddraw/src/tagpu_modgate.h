#ifndef TAGPU_MODGATE_H
#define TAGPU_MODGATE_H
/* tagpu_modgate -- the battle room between Impure players: Start stays off while another player's fingerprint differs (tagpu_modprint.h), and a
   chat line names who and how (research/notes/modules/mods.md, *The fingerprint*). A joiner whose data has no fingerprint never reaches the room:
   the host refuses it (tagpu_recorder.c mp_join_admit), and its line is said here. One whose tail this build cannot read (a later build's) is
   compared with nobody.

   THE GAME THREAD, except tagpu_modgate_refused: the two calls of the engine's everyone-ready test, the click on START, and the menu's flip. */

/* At attach, once the recorder has armed the data that carries the print. All three sites are checked before any is written. */
void tagpu_modgate_install(void);
/* A joiner the host refused, `older` for an Impure build with no fingerprint, else not Impure: said in the battle room at its next flip. Any
   thread: the pump that refuses runs on the game or the loader thread. */
void tagpu_modgate_refused(const char* name, int older);
/* Each menu flip. */
void tagpu_modgate_tick(void);

#endif
