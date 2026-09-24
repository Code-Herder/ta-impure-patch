#ifndef TAGPU_LIMITS_H
#define TAGPU_LIMITS_H
/* tagpu_limits.h — the engine limits this DLL raises, and where the moved pools live.

   The patches are in tagpu_patches.c (tagpu_limits_install); the plan and the evidence
   are research/notes/tadr-port/raised-limits.md and limits-evidence.md. TADR's
   EngineLimits.cpp is the prior art for the four effect pools.

   THE RULE IS ALL OR NOTHING, AND A FAILURE ENDS THE PROCESS. Every site is compared with
   the stock 3.1 bytes before any is written; one mismatch writes none, and
   tagpu_limits_report — at the first DirectDraw call, outside the loader lock — shows why
   and exits. Every player of a network game must run the same limits (the port's
   same-build contract), so a process that silently kept stock limits would desync the
   first time a raised one was exceeded.

   THE STOCK BUILD. `make LIMITS=stock` defines TAGPU_LIMITS_STOCK: the counts below are
   stock's, nothing is patched, and the accessors return the engine's own arrays. It exists
   for the comparisons that prove the raised build equal to stock below the stock caps;
   there is no runtime switch, because a player must not be able to diverge. */

#ifdef TAGPU_LIMITS_STOCK
#define TAGPU_LIM_PROJ   300     /* live projectiles                              */
#define TAGPU_LIM_EXPL   300     /* live explosion records                        */
#define TAGPU_LIM_PSYS   100     /* flying-piece particle systems                 */
#define TAGPU_LIM_AUX    300     /* debris records an explosion carries           */
#else
#define TAGPU_LIM_PROJ   3000
#define TAGPU_LIM_EXPL   3000
#define TAGPU_LIM_PSYS   1000
#define TAGPU_LIM_AUX    3000
#endif

/* DllMain, DLL_PROCESS_ATTACH: check every site, then write all or none. Returns 1 when
   the raised limits are installed (always 0 in the stock build). */
int tagpu_limits_install(void);

/* The first DirectDraw call: if the install failed, show the report, write it to
   log\startup-failure.txt and exit the process. Otherwise nothing. */
void tagpu_limits_report(void);

/* Where the explosion pool is: {i32 count; records of 0x54 bytes}. `ta` is the engine's
   main block, which holds the pool in the stock build. GAME THREAD. */
const char* tagpu_limits_expl_pool(const char* ta);

/* The flying-piece slots, [begin, end): pointers to particle systems, NULL = free. */
const void* const* tagpu_limits_psys_begin(void);
const void* const* tagpu_limits_psys_end(void);

#endif
