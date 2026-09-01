#ifndef TAGPU_OWNDRAW_H
#define TAGPU_OWNDRAW_H
/* Phase B own-the-draw: skip TA's software rasterisation of the per-unit
   composite for chosen unit types, leaving all builder bookkeeping (AABB,
   alloc, hotspots, blit) to the engine while the GPU thread supplies the
   pixels. Armed by tagpu_owndraw.on (first token = type, or "all"). */
void tagpu_owndraw_init(void);
void tagpu_owndraw_flush(unsigned int frame_counter);
#endif
