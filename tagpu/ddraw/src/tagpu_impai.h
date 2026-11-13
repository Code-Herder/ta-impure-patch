#ifndef TAGPU_IMPAI_H
#define TAGPU_IMPAI_H
#include "tagpu_packet.h"
/* tagpu_impai -- ImpAI's seam in the engine (research/notes/modules/impai.md).

   The players phase's call of the stock AI tick, 0x465031 `call 0x408C40`, is retargeted to a
   wrapper that observes the seats impure.cfg gives ImpAI (`seatai` in a skirmish, `netai` for the
   computer seats this peer adds in a network game) and then always calls the
   stock tick, so the stock AI still plays every seat. Which seats are ImpAI's is SIM_STATE,
   written at the level init. Sixteen Difficulty type tests skip those seats, and the
   console's `AI` takes a seat from ImpAI. */

/* DllMain, before tagpu_simreg_attach closes the lever list: ImpAI's test levers -- the self-checks,
   the overlay, the cost run's budgets. */
void tagpu_impai_levers(void);

/* DllMain, after tagpu_reclaim_init and tagpu_replay_init: when replay's sites are in -- among
   them the level init's PLAYERS call, the only caller of tagpu_impai_level_init -- and the
   teardown wrap is in, the only caller of tagpu_impai_level_end, byte-matches every site and
   writes them all: the seam 0x465031, the AI command's 0x4162F4 and 0x4162FE, the sixteen
   handicap sites and the site test's bit 0x47D3D1, with the thunk WQ_SITE_SEAT calls. Otherwise
   nothing is written, ImpAI is unavailable and every level says why. */
void tagpu_impai_init(void);

/* The level init's (tagpu_replay.c, tagpu_replay_level_players), right after the engine's
   players init 0x464990 on the LOADER thread: clears the seat table on every level, then marks
   the computer seats `seatai` names on a live skirmish level, or on a live network level the
   computer seats this peer added whose names begin ImpAI:; a puppet level's seats the recording
   restores. */
void tagpu_impai_level_init(void);

/* A takeover's (tagpu_replay.c, tk_end), on the GAME thread after the puppet mode has ended and the
   recording's tables are restored, before the fork is armed: the fresh AI seats' marks from `seatai`, with
   their content and no knowledge, as a skirmish's level init makes them. */
void tagpu_impai_takeover(void);

/* The level's teardown (tagpu_reclaim.c, reclaim_teardown_pre), on the game thread: logs each
   marked seat's observed turns and the handicap rows' counts, and clears the seat table. */
void tagpu_impai_level_end(void);

/* 1 when ImpAI plays record i -- active, controller 2 and marked -- else 0 (OTA AI, a human,
   an index past the ten): the recording header's RT_PLAYER byte 15 (tagpu_recorder.c,
   header_build). */
unsigned tagpu_impai_seat_ai(unsigned i);
/* 1 when tagpu_impai_init installed every site: ImpAI can play a seat in this process. */
int tagpu_impai_available(void);
/* ImpAI's WqMenuFn (tagpu_worldq.h): builder type `builder`'s build menu for seat `seat`'s side, from the
   menus the level init read; the count, 0 when the side has none, -1 when cap is short. Game thread. */
int tagpu_impai_menu(int seat, unsigned builder, uint16_t* out, int cap);

/* THE OVERLAY's fill, from the frame packet's publisher on the GAME thread, inside the in-play draw:
   while the test lever tagpu_impaiov.on names a seat that ImpAI plays or has knowledge for, its header
   and at most `max` items into the publisher's buffers; 1 when the overlay shows, 0 when not. `*cut` is
   set when items past `max` were left out. */
int tagpu_impai_overlay(TAGPU_PK_IMPAIHDR* h, TAGPU_PK_IMPAI* out, unsigned max, unsigned* n, int* cut);

/* THE KNOWLEDGE DIGESTS, under the check lever: from the end of every in-play draw, on the GAME thread,
   each ImpAI seat's knowledge digest at each GameTime that is a multiple of 30, once (tagpu_impai.c). */
void tagpu_impai_frame(void);

#endif
