#ifndef TAGPU_MODGATE_H
#define TAGPU_MODGATE_H
/* tagpu_modgate -- the battle room between Impure players: Start stays off while another player's fingerprint differs (tagpu_modprint.h), and a
   chat line names who and how (research/notes/modules/mods.md, *The fingerprint*). A joiner whose data has no fingerprint, or whose build plays
   another revision of the network game, never reaches the room: the host refuses it (tagpu_recorder.c mp_join_admit), and its line is said
   here. One whose tail this build cannot read is compared with nobody.

   THE GAME THREAD, except tagpu_modgate_refused: the two calls of the engine's everyone-ready test, the click on START, and the menu's flip. */

/* At attach, once the recorder has armed the data that carries the print. All three sites are checked before any is written. */
void tagpu_modgate_install(void);
/* Why the host refused a joiner: its data is not Impure's, or it is an Impure build older or newer than the host's. */
typedef enum { MODGATE_NOT_IMPURE, MODGATE_OLDER, MODGATE_NEWER } MODGATE_REFUSAL;
/* A joiner the host refused: said in the battle room at its next flip. Any thread: the pump that refuses runs on the game or the loader thread. */
void tagpu_modgate_refused(const char* name, MODGATE_REFUSAL why);
/* Each menu flip. */
void tagpu_modgate_tick(void);

#endif
