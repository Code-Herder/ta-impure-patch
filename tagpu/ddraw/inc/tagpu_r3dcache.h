#ifndef TAGPU_R3DCACHE_H
#define TAGPU_R3DCACHE_H
/* Per-unit cache of the last GPU-rendered composite planes (colour+depth).
   Written by the render thread (tagpu_render3do after each readback), read by
   the GAME thread inside the owndraw rasterise-skip detour: when the engine
   rebuilds a composite (move/animate = fresh empty planes) the stub repaints
   it synchronously from here, killing the one-frame invisible window that
   made moving/building units flicker. Lock-free by design: store never frees
   a buffer another thread might be copying (delayed-free ring). */
void tagpu_r3dcache_store(const void* obj3do, const unsigned char* col,
                          const unsigned char* dep, int w, int h, int hx, int hy);
int  tagpu_r3dcache_restore(unsigned int obj3do, unsigned int frame); /* 1=repainted */
void tagpu_r3dcache_stats(unsigned* restored, unsigned* missed);      /* reads+clears */
void tagpu_r3dcache_wipe(unsigned int frame);
#endif
