#ifndef TAGPU_R3DCACHE_H
#define TAGPU_R3DCACHE_H
/* Per-unit cache of GPU-rendered composite planes (colour+depth), and the
   composite wipe the owndraw rasterise-skip detour uses (GAME thread).
   NOTHING CALLS tagpu_r3dcache_store, so the cache is always empty and
   tagpu_r3dcache_restore -- which the skip calls for a unit the native pass
   does not own -- always misses and leaves the composite as the engine built
   it. The live entry point is tagpu_r3dcache_wipe, for the units the native
   pass does own. Lock-free by design: store never frees a buffer another
   thread might be copying (delayed-free ring). */
void tagpu_r3dcache_store(const void* obj3do, const unsigned char* col,
                          const unsigned char* dep, int w, int h, int hx, int hy);
int  tagpu_r3dcache_restore(unsigned int obj3do, unsigned int frame); /* 1=repainted */
void tagpu_r3dcache_stats(unsigned* restored, unsigned* missed);      /* reads+clears */
void tagpu_r3dcache_wipe(unsigned int frame);
#endif
