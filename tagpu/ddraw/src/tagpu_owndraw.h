#ifndef TAGPU_OWNDRAW_H
#define TAGPU_OWNDRAW_H
/* Phase B own-the-draw: skip TA's software rasterisation of the per-unit
   composite for chosen unit types, leaving all builder bookkeeping (AABB,
   alloc, hotspots, blit) to the engine while the GPU thread supplies the
   pixels. Armed by tagpu_owndraw.on (first token = type, or "all"). */
void tagpu_owndraw_init(void);
void tagpu_owndraw_flush(unsigned int frame_counter);
/* THE STRUCTURE-SHADOW GATE (the vulkan-only plan, landing 10b).

   `set` is the publish: the native pass says, once per frame from the render
   thread, whether it will paint structures' cached slant shadows. While it
   says yes the blit's two branches (detoured at 0x4592BF / 0x459522) take the
   engine's "no cached shadow" path and the native pass owes every structure
   one; while it says no -- and until it has ever said anything, which is the
   whole of a lane that never runs -- the engine draws its own, as stock.

   `ours` is what the ENGINE will do, read back so the pass's geometry agrees
   with the branch. It is the GATE, not the install: the hooks can be in and
   this still 0. */
void tagpu_owndraw_set_structshadow(int ours);
int  tagpu_owndraw_structshadow_ours(void);
#endif
