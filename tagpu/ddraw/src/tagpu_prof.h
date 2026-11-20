#ifndef TAGPU_PROF_H
#define TAGPU_PROF_H
/* tagpu_prof.h -- WHERE A FRAME'S WALL TIME GOES, on the game thread and on the render thread.

   A lane is one thread's frames. A frame is cut into SPANS by marks: a mark reads the clock,
   charges the time since the last mark to the span that was open, and opens the span it names.
   Every instant of a frame is therefore in exactly one span and a row's spans sum to its total;
   a mark that is missing (a hook that did not install) merges its span into the one before it
   and loses no time.

   MARKS GO ONLY WHERE THE CODE PASSES A FEW TIMES A FRAME: a phase of the tick, a section of the
   draw, a pass. A mark costs one clock read; a mark inside a per-unit or per-pixel loop would
   cost more than what it measures and is not what this module is for.

   THE LEVER IS `tagpu_prof.on`. Each lane polls it at its own frame boundary and writes one row
   a frame to `tagpu_prof_game.csv` / `tagpu_prof_render.csv` in the log directory, and a summary
   line to the log every few seconds. The game lane's marks also need the file at launch: its
   phase boundaries are the engine's own clock stamps, taken by a hook tagpu_packet_pub.c
   installs only when the lever is present at attach.

   WITH `tagpu_prof_sample.on` AS WELL, a sampler notes where the game thread stands about a
   thousand times a second, for the spans whose inside the marks cannot time: the samples go to
   `tagpu_prof_samples.bin`, and tools/profsample.py reads them (tagpu_prof.c, THE SAMPLER).

   ONE THREAD OWNS A LANE. The lane's frame mark names its owner, and every other mark is dropped
   unless it comes from that thread, so a lane's accumulators have one writer. */

enum { TAGPU_PROF_GAME, TAGPU_PROF_RENDER, TAGPU_PROF_LANES };

/* the game lane's spans, in the frame's order */
enum {
    TAGPU_PG_OUTSIDE,     /* from the frame function's last stamp to its next entry: the rest of
                             the main loop, the message pump                                    */
    TAGPU_PG_FRAME,       /* the frame function's own: the scheduler, keys, scroll, the cull that
                             lists the units in view, what follows the draw                     */
    TAGPU_PG_NET,         /* a network game's pump and its tail                                 */
    TAGPU_PG_TICK_OURS,   /* Impure's work around the unit tick: the recorder's capture, a
                             replay's step                                                      */
    TAGPU_PG_UNITS,       /* the unit tick                                                      */
    TAGPU_PG_PROJ,        /* projectiles                                                        */
    TAGPU_PG_EXPL,        /* explosions and flying pieces                                       */
    TAGPU_PG_PLAYERS,     /* the players phase: sight, the AIs, radar, the minimap, the economy */
    TAGPU_PG_FEATURES,    /* features, and the small steppers after them: wind, meteors, camera */
    TAGPU_PG_PARTICLES,
    TAGPU_PG_TICK_TAIL,   /* what ends an iteration and the step                                */
    TAGPU_PG_BEFORE_DRAW, /* Impure's commands before the draw                                  */
    TAGPU_PG_TERRAIN,     /* the draw's setup and the terrain blit                              */
    TAGPU_PG_BARS,        /* the resource bar, the bottom bar, the minimap                      */
    TAGPU_PG_SORT,        /* the unit sort                                                      */
    TAGPU_PG_SWEEP,       /* particles, features, units, weapons, explosions                    */
    TAGPU_PG_MARKERS,     /* order markers, health bars, group digits                           */
    TAGPU_PG_FOG,
    TAGPU_PG_HUD,         /* the build cursor and the HUD's tail                                */
    TAGPU_PG_UI_CAPTURE,  /* Impure's work at the flip: the UI capture, the tools               */
    TAGPU_PG_FLIP,
    TAGPU_PG_PUB_WORLD,   /* the frame packet's unit, piece and wreck tables                    */
    TAGPU_PG_PUBLISH,     /* the rest of the frame packet's fill and its hand-over              */
    TAGPU_PG_PROF,        /* this module's own row and file write                               */
    TAGPU_PG_N
};

/* the render lane's spans */
enum {
    TAGPU_PR_LOOP,        /* the loop's own: pacing, the lever polls                            */
    TAGPU_PR_GATHER,      /* the packet taken and turned into the passes' inputs                */
    TAGPU_PR_FENCE,       /* the wait for this slot's last frame to leave the device            */
    TAGPU_PR_ACQUIRE,
    TAGPU_PR_PREPARE,     /* every pass's prepare but the unit pass's                           */
    TAGPU_PR_UNIT_UPLOAD,
    TAGPU_PR_UNIT_PREPARE,
    TAGPU_PR_RECORD,
    TAGPU_PR_SUBMIT,
    TAGPU_PR_PRESENT,
    TAGPU_PR_PROF,
    TAGPU_PR_N
};

/* the lever was there when the DLL attached: the game lane's hook is wanted */
int  tagpu_prof_wanted(void);

/* A lane's frame boundary, from the thread that owns the lane: closes the frame before it (its
   row), polls the lever, and opens `span`. `tag` is written in the row: the game lane's GameTime. */
void tagpu_prof_frame(int lane, int span, unsigned tag);
/* closes the open span and opens `span` */
void tagpu_prof_mark(int lane, int span);
/* A boundary that knows what ended: the time since the last mark is `ended`'s, whatever span was
   open, and `next` opens. The engine's own clock stamps are these, each taken after its phase. */
void tagpu_prof_stamp(int lane, int ended, int next);
/* the same, remembering the span it closed for the matching leave: our own work inside a span */
void tagpu_prof_enter(int lane, int span);
void tagpu_prof_leave(int lane);
/* one more of what the row counts: the game lane's ticks */
void tagpu_prof_count(int lane);
#endif
