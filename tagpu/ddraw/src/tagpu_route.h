#ifndef TAGPU_ROUTE_H
#define TAGPU_ROUTE_H
/* tagpu_route.c -- the in-play mouse handler's shared hooks and the bounded world view their
   clients use. The handler 0x499200 is the engine's per-mode call for a mouse record in play;
   two clients act inside it, each in its own cursor mode: tagpu_place.c (14, a building armed)
   and tagpu_sweep.c (12, RECLAIM armed). Everything here runs on the GAME thread. */
#include <stdint.h>

/* The sites, for tagpu_route_ok: the click routing's stub at 0x4993BF, the before-observer on
   the handler's entry 0x499200, and the two calls of the cursor chooser 0x48D220 (0x499297 inside
   the handler, 0x491D36 outside it). */
#define TAGPU_ROUTE_STUB   1u
#define TAGPU_ROUTE_ENTRY  2u
#define TAGPU_ROUTE_CURSOR 4u

/* DLL attach, before tagpu_place_init and tagpu_sweep_init: byte-matches and writes the sites.
   Each client asks tagpu_route_ok for the ones it needs and arms only on all of them. */
void tagpu_route_init(void);
/* 1 when every site in `what` is installed */
int  tagpu_route_ok(unsigned what);

/* One record's view of the world: the main block, the unit defs and their count, the unit array
   and its slot count, and the controlled player's block of slots, every one bounded against the
   engine's own counts. */
typedef struct {
    char *ta, *defs, *units;
    unsigned ndefs, nslots, first, last;
    int player;
} TagpuWorld;

/* 1 when the view holds: in a level, the controlled player (main+0x2A42) below 10 and not a
   puppet seat, the array's two ends and the player's block consistent with its slot count. */
int      tagpu_route_world(TagpuWorld* w);
/* an alive, not excluded unit at `slot`, or NULL */
char*    tagpu_route_unit(const TagpuWorld* w, unsigned slot);
/* the unit's def index, bounded by UNITINFOCount; 0 when its def pointer is not a def */
unsigned tagpu_route_def_id(const TagpuWorld* w, const char* u);
/* the mouse object's current move record, six dwords */
void     tagpu_route_stale_move(int out[6]);
/* the two move records are one move, whatever their x and y say */
int      tagpu_route_same_move(const int a[6], const int b[6]);

#endif
