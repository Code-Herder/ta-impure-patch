#ifndef TAGPU_NODRAW_H
#define TAGPU_NODRAW_H
/* tagpu_nodraw -- NO DRAW, a test's GDI pacing, and THE POSE CHECK (src/tagpu_nodraw.c).

   NO DRAW: tagpu_nodraw.on present, on GDI (tagpu_gdi.on) with a turbo k (tagpu_replay.h, THE
   TURBO). While the turbo runs, a frame whose batch was the turbo's and whose scoreboard could not
   write its rank runs, in DrawGameScreen's place, the render-options screen's tick, the draw's
   commands, DrawUnit's repose of the units the draw would reach and what a test reads; every other
   frame draws. Its line `nodraw: on at 0x4969CD, marked at 0x49692D` or a refusal comes at
   tagpu_replay_init, and its account at each level's end.
   THE POSE CHECK: tagpu_posecheck.on present, its content a period in GameTime (300 when empty).
   Every period, a `posecheck:` line digests every model's pose cache and SweetSpot points, for
   tools/replay/nodraweq.py.
   THE SHADOW: tagpu_ndshadow.on present, on GDI. On every drawn in-play frame the repose's selection
   is made without calling DrawUnit and compared with the units the draw itself passes to DrawUnit
   from its two sweeps; a difference is logged by name, and each level ends with the count of frames
   compared and differing. Beside NO DRAW every other frame it would skip draws, so one game runs
   both. Its line `nodraw: shadow on at 0x469A00 and 0x469BA3` or a refusal comes at
   tagpu_replay_init; the test group nodraw-shadow plays under it.

   The replay owns the sites this rides on -- the frame visit at 0x4969CD and the pacing guard at
   0x49692D (tagpu_replay.c) -- and calls in here; every call is on the game thread but the
   install's, which DllMain makes in the order below. */

/* the three levers, honoured only in a test launch (tagpu_regstore_active) and refused with a line
   otherwise; `turbo_k` is the turbo lever's k, 0 when it is off. tagpu_replay_pacing_init, before
   tagpu_simreg_attach closes the lever list. */
void tagpu_nodraw_levers(unsigned turbo_k);
/* 1 when the lever passed and every byte the skip rule and the repose rest on matches, so the
   replay writes the skippable frame stub; else 0, with a line when the lever had passed */
int  tagpu_nodraw_site_ok(void);
/* once the replay wrote the frame stub, skippable or not: no-draw's rows and the composite's gate at
   0x45AE73, both or neither, and the shadow's lists and recorders, all or none, each with a line on
   failure; nothing without its lever */
void tagpu_nodraw_site_in(void);
/* the pacing guard at 0x49692D is in (`in`) or not: no-draw is on when both its sites are, with
   its line */
void tagpu_nodraw_guard(int in);
/* the skippable frame stub's flag (inc/tagpu_skipstub.h) and the function it jumps to in
   DrawGameScreen's place, stdcall with the draw's two arguments */
volatile unsigned char* tagpu_nodraw_skip(void);
void __stdcall tagpu_nodraw_frame(unsigned draw_units, unsigned blit);

/* THE MARK, in the pacing guard after every bound: this frame's batch is the turbo's when
   `turbo_batch` (the turbo live, no rewind), the lag scale clear and no puppet level */
void tagpu_nodraw_mark(char* ta, int turbo_batch);
/* THE POSE CHECK, first in the frame visit, at its GameTime `g` */
void tagpu_nodraw_posecheck(char* ta, unsigned g);
/* THE GATE, the frame visit's last act: `g0` the GameTime and `quiet` no seek, switch or rewind
   pending, both taken before the visit */
void tagpu_nodraw_gate(char* ta, unsigned g0, int quiet);
/* THE SHADOW's two halves, from the frame packet's observers on an in-play DrawGameScreen
   (tagpu_packet_pub.c): the selection right after the draw's commands, the comparison right after
   the draw returns */
void tagpu_nodraw_shadow_select(void);
void tagpu_nodraw_shadow_settle(void);
/* the level's account lines, at GameTime `g1`, and the level's state cleared */
void tagpu_nodraw_level_end(unsigned g1);

#endif
