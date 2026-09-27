#ifndef TAGPU_REFUSE_H
#define TAGPU_REFUSE_H
/* tagpu_refuse -- the one way Impure stops a launch it must not go through with.

   Called from the first DirectDraw call, never from DllMain: a MessageBox under the loader
   lock can deadlock. By then every DLL's start-up has run and the exe's WinMain has made
   the call, so nothing of a game exists yet -- stopping here is a refusal, not a crash.

   `text` is written for a player first and for whoever debugs it second: what happened, why,
   what to do, then a `--- report ---` block meant to be pasted in public, so it carries no
   path. It goes to the log, to `log\startup-failure.txt` (which the compatibility suite reads
   as the evidence of which refusal fired), and to a box titled
   "Total Annihilation: Impure cannot start".

   Never returns: the process ends with ERROR_BAD_EXE_FORMAT (0xC1), the exit code the suite
   matches. */

/* The refusal, whole. Any thread; call once -- a second call would box twice. */
void tagpu_refuse(const char* text);

#endif
