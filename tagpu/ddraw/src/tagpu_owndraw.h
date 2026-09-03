#ifndef TAGPU_OWNDRAW_H
#define TAGPU_OWNDRAW_H
/* Phase B own-the-draw: skip TA's software rasterisation of the per-unit
   composite for chosen unit types, leaving all builder bookkeeping (AABB,
   alloc, hotspots, blit) to the engine while the GPU thread supplies the
   pixels. Armed by tagpu_owndraw.on (first token = type, or "all"). */
void tagpu_owndraw_init(void);
void tagpu_owndraw_flush(unsigned int frame_counter);
/* 1 while target "all" has redirected the blit's structure-shadow branches
   (0x4592C6 / 0x45952C je->jmp): the engine then draws NO cached slant shadow
   and the native pass owes every structure one (tagpu_native.c). */
int  tagpu_owndraw_structshadow_ours(void);
#endif
