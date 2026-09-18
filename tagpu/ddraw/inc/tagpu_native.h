#ifndef TAGPU_NATIVE_H
#define TAGPU_NATIVE_H
#include "tagpu.h"
/* G12b native unit pass. Armed by tagpu_native.on (token = type, default
   armcom). Owned units leave the composite path: writeback must skip+wipe
   them, the owndraw stub wipes instead of restoring. */
void tagpu_native_frame(const TAGPU_FRAME* f);
/* THE UNIT FRAGMENT SHADER, so the posed program (G16 step 5,
   tagpu_posedraw.c) is a twin of this pass rather than a copy of it: the
   vertex stage is what step 5 replaces, and sharing the fragment stage is what
   stops the two drifting in the half it does not touch. */
const char* tagpu_native_unit_fs(void);
/* Is this unit natively owned right now? GAME THREAD ONLY since the frame
   packet's landing 3: it reads the unit record and its UnitDef, and the
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

/* The 256-byte fog shade table this pass last uploaded to its fog LUT texture
   -- the packet's `fogshade`, or the identity when the packet carries none,
   which is the same fallback the upload applies. NULL before the first upload.
   Render thread only; the buffer is ours for the process's life.
   [Phase G / G19e: the Vulkan editions of the world passes sample this table
   and cannot read a GL texture, so the BYTES are published rather than the
   texture name -- and they are the bytes that were uploaded, not a second
   construction of them.] */
const unsigned char* tagpu_native_foglut(void);
/* the grid that went with it, for a pass that has to copy it -- see the
   implementation for the bound and the lifetime */
const unsigned short* tagpu_native_foggrid(int* cols, int* rows, int* cells);

/* 1 while this frame's world-FBO passes are clipped to the engine's viewport
   rect (glScissor), 0 when glScissor could not be resolved and they are not.
   The world passes' Vulkan editions must clip exactly as their GL twins do, and
   "there is a rect" is not the same fact as "the clip is on". Render thread. */
int tagpu_native_scissor_on(void);

/* WHERE THE WORLD IS DRAWN AND WHERE THE FINISHED BLOCK LANDS, decided once a
   frame and published for whoever draws it.

   THIS IS THE ONE DECISION, NOT A SECOND COPY OF IT. `ss` is settled in
   `tagpu_native_frame` above the effects gather, for the reason stated there:
   every pass that draws into this frame has to agree how many samples a game
   pixel is, and the first cut of `devres` raised `ss` after `fv.ss` was set and
   put the scaffold test 1.5x out. The Vulkan backend needs the same number for
   the same reason -- it sizes the offscreen world target with it -- so it reads
   what was decided rather than recomputing `s_ss ? 2 : 1` on its own side.

   IT IS PUBLISHED FROM THE GATHER, ABOVE THE `!gl_draws` RETURN, so it is this
   frame's on both lanes. Everything in it is arithmetic on the frame packet and
   the levers; nothing here touches GL, which is what makes that placement legal.

   `serial` moves when any field moves, so a consumer holding device resources
   sized by this can tell "the same target" from "a new one" without comparing
   the fields itself. `frame` is the render-thread frame it was decided on, and
   a consumer must refuse a hand-over that is not its own frame's -- the rule
   every hand-over in this tree carries.

   Render thread only. [The vulkan-only plan, landing 4c-2.] */
typedef struct {
    int      gw, gh;      /* the GAME's own resolution -- not the window's     */
    int      ss;          /* samples per game pixel: the target is gw*ss,gh*ss */
    int      devres;      /* the 1x resolve is skipped; read the ss buffer     */
    int      vx, vy, vw, vh;  /* where the block lands, HUD shift applied      */
    unsigned serial;
    unsigned frame;
} TAGPU_WORLDTGT;
int tagpu_native_worldtgt(TAGPU_WORLDTGT* out);
#endif
