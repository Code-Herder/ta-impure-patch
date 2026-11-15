#ifndef TAGPU_FACTORYQ_H
#define TAGPU_FACTORYQ_H
/* tagpu_factoryq.c -- the factory queue's hundred: Ctrl+Shift on a build-menu click queues or
   takes off 100 of the type, where Shift alone is stock's 5 (TADR's binding). Everything else
   about the click is stock's: the engine's own queue call 0x419B00 does the work, on the GAME
   thread, inside the build menus' handler 0x41AA00. */

/* DLL attach: byte-matches the factory branch of the click (0x41ABFA..0x41AC3B) and sends its
   call of 0x419B00 to the wrapper; a different exe arms nothing. */
void tagpu_factoryq_init(void);

#endif
