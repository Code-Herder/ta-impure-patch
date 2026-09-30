#ifndef TAGPU_PUPPET_H
#define TAGPU_PUPPET_H
/* tagpu_puppet -- what the simulation reads about a level that plays a recording
   (research/notes/tadr-port/demo-recorder-s2-host.md; the slice note's S2b). Such a level seats
   every recorded player as a type-3 record that the recording drives, a puppet seat, and the
   viewer's own seat V beside them; tagpu_replay.c seats them and applies the recording. This
   module holds whether this level is a puppet level, which records are puppet seats, and what
   getter 75 answers for each.

   THE TABLE is SIM_CONTENT (decision D3): derived from the recording's header at the seat fill,
   constant for the level, and never recorded or restored. The seat fill writes it on the game
   thread in the SKIRMISH Start, before the loader thread exists: CreateThread orders every write
   before the new thread's first instruction. The level's teardown clears it on the game thread.
   Every reader runs between those two points, on the game thread or the loader thread. A Start
   always reaches the teardown: after the seat fill, the Start branch runs straight to the loading
   state (0x47B097..0x47B0B0), and the loader has one exit (0x497C6C), or the process ends at its
   crash handler (0x497CAB). */

#define PUPPET_PLAYERS 10   /* the records a level seats (REC_PLAYERS)                        */

/* 1 while this level plays a recording */
int tagpu_puppet_on(void);
/* 1 when `player` is below 10 and a puppet seat of this level, V included */
int tagpu_puppet_seat(unsigned player);
/* getter 75 for a puppet seat's unit: the recording machine's answer, 1 for a seat that was
   local there (type 1 or 2), 0 for V */
int tagpu_puppet_local(unsigned player);
/* the mode as a byte that a stub can compare (tagpu_detour_cmp_flag) */
const volatile unsigned char* tagpu_puppet_flag(void);

/* The seat fill's (tagpu_replay.c): `seat` and `local` for the ten records, then the mode on.
   The teardown's: everything off. */
void tagpu_puppet_begin(const unsigned char seat[PUPPET_PLAYERS],
                        const unsigned char local[PUPPET_PLAYERS]);
void tagpu_puppet_end(void);
/* A recorded controller change (RK_CONTROLLER), at the step that applies it: the seat's getter-75
   answer follows the recording machine's controller. */
void tagpu_puppet_set_local(unsigned player, int local);

#endif
