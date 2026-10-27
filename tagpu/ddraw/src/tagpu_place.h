#ifndef TAGPU_PLACE_H
#define TAGPU_PLACE_H
/* tagpu_place.c -- several buildings in one gesture: with a building armed, Shift + drag lays a
   line (Ctrl: held to its axis), Shift + Alt a filled rectangle (Ctrl: hollow), Shift + Ctrl
   over a unit or a queued build a ring round it (tagpu_placegeo.h for the shapes). Every
   function here runs on the GAME thread; the render thread sees the sites only as the packet's
   copied table. */
#include <stdint.h>

/* A previewed site, as the packet carries it: the build rectangle in 0x4197D0's layout
   (x0, altitude, z0, x1, altitude, z1, world pixels), the type it previews and its colour, an
   index into the GUI colour table at main+0xDCB (PLACE_OK, PLACE_BLOCKED, PLACE_TOGGLE). */
typedef struct {
    int32_t  rect[6];
    uint16_t type;
    uint8_t  colour;
    uint8_t  pad;
} TagpuPlaceSite;

#define PLACE_OK      0x0A   /* placed: the engine's green */
#define PLACE_BLOCKED 0x04   /* refused as a single click would be: skipped */
#define PLACE_TOGGLE  0x0E   /* removes a selected builder's queued build instead (the Shift toggle) */

void tagpu_place_init(void);          /* after tagpu_resource_init: chains on its observer;
                                         after tagpu_route_init: needs its stub and entry */
void tagpu_place_level_end(void);

/* The click route's verdict for a mode-14 record `rec` (the handler's stack copy) with message
   `msg`: 0 stock, 1 consumed, 2 run stock's click on this record (tagpu_route.c). */
int tagpu_place_button(const int* rec, unsigned msg);
/* The in-play handler's entry, before the record is handled (tagpu_route.c). */
void tagpu_place_entry(void);

/* Alt+Z / Alt+X with a building armed: one more / one less empty cell between the armed type's
   neighbouring sites (0..16, kept per type in impure.cfg). 1 when the key was taken -- the
   caller then drops it, which stock does nothing with (TA's key table has no Alt+Z or Alt+X).
   `alt_known`: the message itself says Alt is down (WM_SYSKEYDOWN). From the window procedure,
   on the game thread. */
int tagpu_place_key(unsigned vk, int alt_known);

/* The sites that replace the cursor's single square this frame, at most `max`; 0 when the stock
   square stands (no gesture and no ring under the cursor). For the packet's fill. */
unsigned tagpu_place_sites(TagpuPlaceSite* out, unsigned max);

/* GDI: the rectangles the engine's square call (0x4BF8C0 at 0x469EC5 / 0x469F1E) should draw
   instead of `engine` (l, t, r, b as passed), each the engine's own rectangle moved by the
   site's offset from the cursor site, with its palette byte from the GUI table; 0 when the
   engine's one call stands. */
unsigned tagpu_place_gdi_rects(const int engine[4], int out[][4], unsigned char* colour,
                               unsigned max);

#endif
