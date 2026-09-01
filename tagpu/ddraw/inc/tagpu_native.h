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
#endif
