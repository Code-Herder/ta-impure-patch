#ifndef TAGPU_OWNDRAW_H
#define TAGPU_OWNDRAW_H
/* tagpu_owndraw -- on a Vulkan launch the engine's unit draw, DrawUnit's call of
   the composite at 0x45AE73, is removed at attach; every unit on the screen is
   the unit pass's (src/tagpu_owndraw.c says why nothing else is lost). Under
   tagpu_gdi.on nothing is written. DllMain, after cfg_load. */
void tagpu_owndraw_init(void);
#endif
