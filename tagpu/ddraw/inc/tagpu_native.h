#ifndef TAGPU_NATIVE_H
#define TAGPU_NATIVE_H
#include "tagpu.h"
/* The native unit pass. Armed by tagpu_native.on (token = type, default
   armcom). Owned units leave the composite path: writeback must skip+wipe
   them, the owndraw stub wipes instead of restoring. */
void tagpu_native_frame(const TAGPU_FRAME* f);
/* Render thread, from render_vk.c, when tagpu_vk_frame presented the frame
   gathered under `frame_counter`: the fog witnesses (`bare=`, `out=`,
   `nopieces=`) count that frame. A frame never presented counts nothing. */
void tagpu_native_presented(unsigned int frame_counter);
/* Is this unit natively owned right now? GAME THREAD ONLY: it reads the unit record and its UnitDef, and the
   publisher is what calls it per unit per frame — the answer travels to the
   render thread as TAGPU_PK_U_NATIVE in the packet, so the marker pass, the
   composite wipe and the owndraw classifier all act on one answer instead of
   three threads' reads of the same bytes. */
int  tagpu_native_owns_unit(const char* unit);
int  tagpu_native_owns_obj(unsigned int obj3do);
int  tagpu_native_wrecks_armed(void);

/* the build ghost's standing request for the packet's builds table: set on the
   render thread from the ghost's own 30-frame poll, read by the publisher on
   the game thread so a session with no ghost does not pay for the walk */
void tagpu_native_set_want_builds(int want, unsigned int frame_counter);
void tagpu_native_flush_want(unsigned int frame_counter);   /* its watchdog */
int  tagpu_native_want_builds(void);
/* 1 while the last frame drew a selection rect for EVERY unit it owed one to.
   tagpu_markown.c suppresses the engine's per unit, and must hand them all back
   when this is 0 or a selected unit ends up with no box at all. */
int  tagpu_native_selbox_complete(void);
/* One unit's world position for a marker anchored to it: the sub-pixel
   interpolated sample when this frame's unit gather produced one for that
   slot, the packet's own raw 16.16 otherwise. x = world x, y = ALTITUDE,
   z = map depth (the engine's own order at unit+0x6A). 0 = no position; do
   not use the outputs. Read-only, render thread. */
struct TAGPU_PK_UNIT;
int  tagpu_native_unit_pos(const struct TAGPU_PK_UNIT* u, float* x, float* y, float* z);

/* This frame's fog grid, for a pass that has to copy it -- see the
   implementation for the bound and the lifetime */
const unsigned short* tagpu_native_foggrid(int* cols, int* rows, int* cells);

/* 1 while this frame's world passes are clipped to the engine's viewport
   rect. The native pass says so at its hand-over (tagpu_native.c, `s_scissorOn`)
   and the Vulkan world passes clip on this rather than on the rect, because
   "there is a rect" is not the same fact as "the clip is on". 0 until the
   first frame reaches that hand-over. Render thread. */
int tagpu_native_scissor_on(void);

/* WHERE THE WORLD IS DRAWN AND WHERE THE FINISHED BLOCK LANDS, decided once a
   frame and published for whoever draws it.

   THIS IS THE ONE DECISION, NOT A SECOND COPY OF IT. `ss` is settled in
   `tagpu_native_frame` above the effects gather, for the reason stated there:
   every pass that draws into this frame has to agree how many samples a game
   pixel is, and raising `ss` after `fv.ss` is set puts the scaffold test 1.5x
   out. The Vulkan backend needs the same number for
   the same reason -- it sizes the offscreen world target with it -- so it reads
   what was decided rather than recomputing `s_ss ? 2 : 1` on its own side.

   IT IS PUBLISHED FROM THE GATHER, one line below the decision. Everything in
   it is arithmetic on the frame packet and the levers.

   `frame` is the render-thread frame it was decided on, and a consumer must
   refuse a hand-over that is not its own frame's -- the rule every hand-over in
   this tree carries.

   THERE IS NO `serial` HERE: a consumer that needs to tell "the same target"
   from "a new one" compares the extent it is about to build, as
   tagpu_vk_world.c's `slot_size` does.

   Render thread only. */
typedef struct {
    int      gw, gh;      /* the GAME's own resolution -- not the window's     */
    int      ss;          /* samples per game pixel: the target is gw*ss,gh*ss */
    int      devres;      /* the 1x resolve is skipped; read the ss buffer     */
    int      vx, vy, vw, vh;  /* where the block lands, HUD shift applied      */
    unsigned frame;
} TAGPU_WORLDTGT;
int tagpu_native_worldtgt(TAGPU_WORLDTGT* out);
#endif
