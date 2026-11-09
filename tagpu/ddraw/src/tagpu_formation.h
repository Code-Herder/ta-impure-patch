#ifndef TAGPU_FORMATION_H
#define TAGPU_FORMATION_H
/* tagpu_formation.c -- formation move: drag the button that gives orders and the selected units
   spread evenly along the dragged path instead of all heading for one point. Interface Type 1:
   a right drag with units selected; either type: a left drag with Move armed. Shift appends each
   unit's slot to its own queue. A drag shorter than the threshold is stock's click. Every
   function here runs on the GAME thread; the render thread sees the dots only as the packet's
   copied table. Toggled by `formation=` in impure.cfg (tagpu_formation.on / .off). */
#include <stdint.h>

/* One preview dot: a slot the held gesture would order, world pixels (x, altitude, z). */
typedef struct { int32_t x, alt, z; } TagpuFormDot;

void tagpu_formation_init(void);      /* after tagpu_route_init (needs its three stubs) and after
                                         tagpu_rec_init (decodes the clicks' call sites) */
void tagpu_formation_level_end(void);

/* The right-press branch's verdict for every record that reaches 0x49939B: 0 stock, 1 consumed
   (tagpu_route.c). */
int  tagpu_formation_right(const int* rec, unsigned msg);
/* The click route's verdict for a mode-2 (Move armed) record: 0 stock, 1 consumed. */
int  tagpu_formation_button(const int* rec, unsigned msg);
/* The in-play handler's entry, before the record is handled (tagpu_route.c). */
void tagpu_formation_entry(void);

/* The dots of the held gesture, at most `max`; *cut set when there were more. 0 when no gesture
   shows dots. Values only, for the packet's fill. */
unsigned tagpu_formation_dots(TagpuFormDot* out, unsigned max, int* cut);

/* GDI: blits the move waypoint sprite at each dot, as the engine's order markers do
   (0x439740). From the marker block's hook 9 (tagpu_markown.c) with the frame's context. */
void tagpu_formation_gdi(void* ctx);

#endif
