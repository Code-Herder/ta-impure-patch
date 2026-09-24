#ifndef TAGPU_LIMITS_H
#define TAGPU_LIMITS_H
/* tagpu_limits.h — the engine limits this DLL raises, and where the moved pools live.

   The patches are in tagpu_patches.c (tagpu_limits_install); the plan and the evidence
   are research/notes/tadr-port/raised-limits.md and limits-evidence.md. TADR's
   EngineLimits.cpp is the prior art for the four effect pools, its LimitCrack.cpp for the
   unit limit, the pathfinding budget, the particles and the composite frame.

   THE RULE IS ALL OR NOTHING, AND A FAILURE ENDS THE PROCESS. Every site is compared with
   the stock 3.1 bytes before any is written; one mismatch writes none, and
   tagpu_limits_report — at the first DirectDraw call, outside the loader lock — shows why
   and exits. Every player of a network game must run the same limits (the port's
   same-build contract), so a process that silently kept stock limits would desync the
   first time a raised one was exceeded.

   THE STOCK BUILD. `make LIMITS=stock` defines TAGPU_LIMITS_STOCK: the counts below are
   stock's, nothing is patched, and the accessors return the engine's own arrays. It exists
   for comparisons with stock; there is no runtime switch, because a player must not be
   able to diverge. BELOW THE STOCK CAPS THE TWO ARE NOT IDENTICAL in one respect: the
   flying pieces' ring allocator (0x437A30) is ten times larger, so a piece stock would
   have evicted to make room lives on, lands, and adds its explosion. That changes the
   explosion count and the C-runtime rand() stream, never the simulation's own
   generator unless the explosion pool fills (research/notes/tadr-port/raised-limits.md). */

#ifdef TAGPU_LIMITS_STOCK
#define TAGPU_LIM_PROJ   300     /* live projectiles                              */
#define TAGPU_LIM_EXPL   300     /* live explosion records                        */
#define TAGPU_LIM_PSYS   100     /* flying-piece particle systems                 */
#define TAGPU_LIM_AUX    300     /* debris records an explosion carries           */
#define TAGPU_LIM_UNITS  500     /* units a player: the ceiling (the default is 250) */
#define TAGPU_LIM_PATH   1333    /* the pathfinder's search budget                */
#define TAGPU_LIM_SFX    400     /* particle objects a layer (the emitters keep one more) */
#define TAGPU_LIM_SFXPOOL 1000   /* particle objects in all ten layers together   */
#define TAGPU_LIM_COMPOSITE 600  /* the composite scratch frame, a side          */
#define TAGPU_LIM_WRECKS 2048    /* wreck records: 3DO wrecks and feature animations */
#else
#define TAGPU_LIM_PROJ   3000
#define TAGPU_LIM_EXPL   3000
#define TAGPU_LIM_PSYS   1000
#define TAGPU_LIM_AUX    3000
#define TAGPU_LIM_UNITS  1500    /* the ceiling AND the default                   */
#define TAGPU_LIM_PATH   66650
#define TAGPU_LIM_SFX    20480
#define TAGPU_LIM_SFXPOOL (10 * TAGPU_LIM_SFX)
#define TAGPU_LIM_COMPOSITE 1280
/* Not a TADR value: TADR leaves the pool at 2048. 8192 is what the frame packet's wreck
   table holds inside its existing reserve (tagpu_packet.c), so the raise costs no address
   space; the engine's own ceiling is 0x7FFF, its list links being signed 16-bit. */
#define TAGPU_LIM_WRECKS 8192
#endif
/* the wreck pool's list links are signed 16-bit words (0x4232F0 reads them with movsx), so
   every record index has to stay below 0x8000 */
typedef char tagpu_lim_wrecks_fit[(TAGPU_LIM_WRECKS <= 0x7FFF) ? 1 : -1];
/* the engine's own floor for a player's units (0x491678: `cmp eax,0x14`) */
#define TAGPU_LIM_UNITS_MIN 20

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
