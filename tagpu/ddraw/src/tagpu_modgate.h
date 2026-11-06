#ifndef TAGPU_MODGATE_H
#define TAGPU_MODGATE_H
/* tagpu_modgate -- the battle room between Impure players: Start stays off while another player's fingerprint differs (tagpu_modprint.h), and a
   chat line names who and how (research/notes/modules/mods.md, *The fingerprint*). A player with no fingerprint is compared with nobody, as stock.

   THE GAME THREAD ALONE: the two calls of the engine's everyone-ready test, the click on START, and the menu's flip. */

/* At attach, once the recorder has armed the data that carries the print. All three sites or none. */
void tagpu_modgate_install(void);
/* Each menu flip. */
void tagpu_modgate_tick(void);

#endif
