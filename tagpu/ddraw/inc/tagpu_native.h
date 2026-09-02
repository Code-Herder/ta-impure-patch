#ifndef TAGPU_NATIVE_H
#define TAGPU_NATIVE_H
#include "tagpu.h"
/* G12b native unit pass. Armed by tagpu_native.on (token = type, default
   armcom). Owned units leave the composite path: writeback must skip+wipe
   them, the owndraw stub wipes instead of restoring. */
void tagpu_native_frame(const TAGPU_FRAME* f);
int  tagpu_native_owns_unit(const char* unit);
int  tagpu_native_owns_obj(unsigned int obj3do);
int  tagpu_native_wrecks_armed(void);
/* 1 while the last frame drew a selection rect for EVERY unit it owed one to.
   tagpu_markown.c suppresses the engine's per unit, and must hand them all back
   when this is 0 or a selected unit ends up with no box at all. */
int  tagpu_native_selbox_complete(void);
#endif
