#ifndef TAGPU_PUPPET_H
#define TAGPU_PUPPET_H
/* tagpu_puppet -- what the simulation reads about a level that plays a recording
   (research/notes/tadr-port/demo-recorder-s2-host.md; the slice note's S2b). Such a level seats
   every recorded player as a type-3 record that the recording drives, a puppet seat, and the
   viewer's own seat V beside them in record 10, which is no puppet seat (tagpu_replay.c, THE
   VIEWER'S SEAT); tagpu_replay.c seats them and applies the recording. This
   module holds whether this level is a puppet level, which records are puppet seats, what
   getter 75 answers for each, and the gates that keep the recording the only writer.

   THE TABLE is SIM_CONTENT (decision D3): derived from the recording's header at the seat fill,
   constant for the level but for the mark of its first step (tagpu_puppet_play), and never
   recorded or restored. The seat fill writes it on the game thread in the SKIRMISH Start or the
   browser's, before the loader thread exists: CreateThread orders every write before the new
   thread's first instruction. The level's teardown clears it on the game thread.
   Every reader runs between those two points, on the game thread or the loader thread. A Start
   always reaches the teardown: after the seat fill, the Start branch runs straight to the loading
   state (0x47B097..0x47B0B0) -- the Replays browser's Start too, whose commit refuses nothing
   after the fill and leaves the same shell state (tagpu_replay.c, THE BROWSER'S START) -- and the
   loader has one exit (0x497C6C), or the process ends at its crash handler (0x497CAB). */

#include <stddef.h>

#define PUPPET_PLAYERS 10   /* the records a level seats (REC_PLAYERS)                        */

/* 1 while this level plays a recording */
int tagpu_puppet_on(void);
/* 1 when `player` is below 10 and a puppet seat of this level */
int tagpu_puppet_seat(unsigned player);
/* getter 75 for a puppet seat's unit: the recording machine's answer, 1 for a seat that was
   local there (type 1 or 2), 0 for any other record */
int tagpu_puppet_local(unsigned player);
/* the mode as a byte that a stub can compare (tagpu_detour_cmp_flag) */
const volatile unsigned char* tagpu_puppet_flag(void);

/* The seat fill's (tagpu_replay.c): `seat` and `local` for the ten records, then the mode on.
   `engine_cells`: the grid's unit slots and yard bits are the engine's own on this level -- a
   multiplayer recording's, whose units are timed by their owners while the recorder's grid
   follows its own replicas, or a skirmish one that holds its cells (RECL_CELLS) -- so the
   footprint writers' writes stand (THE FOOTPRINT WRITERS). `mp_state`: a multiplayer segment,
   whose state bytes the state gate keeps to the replay's own write. A skirmish level that keeps
   the engine's cells also decides their contests by its recording machine's rule from its first
   step (THE CONTEST'S RULE, tagpu_puppet_play). The teardown's: everything off, the contest's
   rule the engine's own again. */
void tagpu_puppet_begin(const unsigned char seat[PUPPET_PLAYERS],
                        const unsigned char local[PUPPET_PLAYERS], int engine_cells,
                        int mp_state);
void tagpu_puppet_end(void);
/* engine_cells and mp_state for the segment the replay plays from here on: a multiplayer file's
   takeover timeline is a skirmish segment. Only between steps (the replay's seek); refused, and
   logged, inside a footprint writer. 1, or 0 when the contest's rule could not follow it, and
   then nothing changed. */
int tagpu_puppet_engine_cells(int engine_cells, int mp_state);
/* 1 when the six contest sites held the stock bytes at attach: a level that would need the
   recording machine's rule (THE CONTEST'S RULE) plays only then */
int tagpu_puppet_contest_in(void);

/* THE GATES (tagpu_puppet.c): the engine functions that would write what a recording carries on
   a puppet level, or take the viewer's own input to it, refuse on one. DllMain's attach lands them
   all, before the recorder's observers, which chain onto the two reply gates, the seven feature
   gates and the state gate; a level plays a recording only when every gate is in. */
void tagpu_puppet_init(void);
int tagpu_puppet_gates_in(void);
/* The replay's own create and reply, bracketed on the game thread: the unit gates let through
   what they nest (tagpu_puppet.c, *The replay's own calls*). */
void tagpu_puppet_open(int open);
/* A seek's rebuild, bracketed on the game thread (tagpu_replay.c, seek_run): while it runs the
   skirmish elimination 0x47BD70 is refused, since the rebuild's kills are not the recording's */
void tagpu_puppet_rebuild(int on);
/* 1 while a seek's rebuild runs: the recorder's counts leave its calls out */
int tagpu_puppet_rebuilding(void);
/* The replay's own write of a state byte from the owner's image, bracketed on the game thread
   (tagpu_replay.c, book_state): on a multiplayer segment the state gate lets through only it */
void tagpu_puppet_state_open(int open);
/* COB's engine writes on a puppet this level, by write site (tagpu_puppet.c, THE COB WRITES ON A
   PUPPET): each site's return address in va[k] and its count in n[k]; the sites, or 0 when the
   observers are not in */
#define TAGPU_PUPPET_COB_SITES 5
unsigned tagpu_puppet_cob_writes(unsigned va[TAGPU_PUPPET_COB_SITES],
                                 unsigned n[TAGPU_PUPPET_COB_SITES]);
/* the calls each gate refused this level, as one line for the replay's log */
void tagpu_puppet_refusals(char* out, size_t cap);

/* THE FEATURES (the slice note's S3a). From the level's first step (tagpu_puppet_play) the feature
   functions and the feature tick refuse every call on a puppet level but the replay's own, which
   it opens them for on the game thread: a feature root it plays, the tick where the recording ran
   it, and a death, whose corpse the destructor makes. tagpu_puppet_features sets whether they are
   open and returns what it was, which the caller puts back: a death's explosion plays its roots
   inside the death. Before the first step they are open: the loader places the map's features
   through the spawn. tagpu_puppet_play also writes THE CONTEST'S RULE when the level needs it:
   1, or 0 when its sites hold other bytes or could not be written -- nothing is written or
   marked then, and the playing must stop. */
int tagpu_puppet_play(void);
int tagpu_puppet_features(int open);

/* THE DRAWS a replayed root makes at the feature code's six calls of the simulation RNG, and on
   a level that keeps the engine's cells at reproduction's source test (FS_SOURCE)
   (tagpu_recfmt.h, RecDrawSite):
   the recording's, in order, armed by the replay around the call.
   tagpu_puppet_draw answers 1 with the next one while its site and argument are the call's;
   otherwise 0, and the call draws for itself, as do the rest of the root's calls. */
typedef struct PuppetDraw { unsigned site, n, v; } PuppetDraw;
void tagpu_puppet_draws(const PuppetDraw* q, unsigned n);
int tagpu_puppet_draw(unsigned site, unsigned n, unsigned* v);
/* disarms; the draws the call did not take as recorded: left over, or after a disagreement */
unsigned tagpu_puppet_draws_end(void);

/* THE FOOTPRINT WRITERS (tagpu_puppet.c): on a puppet level whose cells are the recording's (not
   engine_cells) each call of the stamp 0x47CC30, the clear 0x47D0E0 or the re-claim 0x47C790 has
   its writes to the grid's unit slots and yard bit undone at its return. The recorder's observer at each entry (tagpu_recorder.c, ob_foot) hands
   over the unit and the call's return address: tagpu_puppet_foot_enter answers 1 when it saved the
   cells, and then the observer's `after` calls tagpu_puppet_foot_leave, which puts them back and
   gives the return address back. */
int tagpu_puppet_foot_enter(const char* unit, unsigned ret);
unsigned tagpu_puppet_foot_leave(void);
/* the calls undone this level and the ones that could not be, as one line for the replay's log */
void tagpu_puppet_footprints(char* out, size_t cap);

#endif
