#ifndef TAGPU_ALLYPANEL_H
#define TAGPU_ALLYPANEL_H
/* The unit panel shows an ally's unit as the player's own (research/notes/modules/allied-sharing.md,
   *The unit panel*). */

/* The panel's five owner tests, byte-matched and all or none, in DllMain's sequence after
   tagpu_allysight_init, before the engine runs. */
void tagpu_allypanel_init(void);

/* 1 once every site is written */
int tagpu_allypanel_installed(void);

#endif
