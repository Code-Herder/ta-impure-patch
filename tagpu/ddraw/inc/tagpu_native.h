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
int  tagpu_native_owns_unit(const char* unit);
int  tagpu_native_owns_obj(unsigned int obj3do);
int  tagpu_native_wrecks_armed(void);
/* 1 while the last frame drew a selection rect for EVERY unit it owed one to.
   tagpu_markown.c suppresses the engine's per unit, and must hand them all back
   when this is 0 or a selected unit ends up with no box at all. */
int  tagpu_native_selbox_complete(void);
/* One unit's world position for a marker anchored to it: the sub-pixel
   interpolated sample when this frame's unit gather produced one for that
   slot, the raw 16.16 otherwise. x = world x, y = ALTITUDE, z = map depth
   (the engine's own order at unit+0x6A). 0 = no position; do not use the
   outputs. Read-only, render thread. */
int  tagpu_native_unit_pos(const char* unit, float* x, float* y, float* z);
#endif
