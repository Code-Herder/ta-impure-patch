#ifndef TAGPU_PLACEGEO_H
#define TAGPU_PLACEGEO_H
/* The shapes of a multi-site placement gesture and Build Split's shares, in TA build cells.
   Engine-free and thread-free: no engine header, no Win32, no state. Recoil's
   CGuiHandler::GetBuildPositions and BAR's api_build_orders.lua, in exact integers
   (research/notes/tadr-port/quality-of-life.md, "Queue editing and line building"). */
#include <stdint.h>

#define PLACE_MAX_SITES   256
#define PLACE_SPACING_MAX 16

enum { PLACE_NONE, PLACE_LINE, PLACE_AXIS, PLACE_RECT, PLACE_HOLLOW, PLACE_RING };

/* A site is its footprint's first cell, as 0x4197D0 snaps the cursor: the cell a footprint of
   fx cells centred on world x starts at is floor((x - 8fx + 8) / 16), i.e. half rounds up. */
typedef struct { int16_t cx, cz; } PlaceCell;

/* Sites of a dragged shape from `from` to `to` (both first cells of an fx x fz footprint),
   `spacing` empty cells between neighbours (0..PLACE_SPACING_MAX). `kind` is PLACE_LINE,
   PLACE_AXIS (the line held to its dominant axis), PLACE_RECT (filled, serpentine) or
   PLACE_HOLLOW. Writes at most `cap` sites in placement order and returns the count. */
unsigned place_shape(int kind, int fx, int fz, int spacing, PlaceCell from, PlaceCell to,
                     PlaceCell* out, unsigned cap);

/* A ring of fx x fz sites around the cells [tx, tx+tw) x [tz, tz+th): right side downwards,
   bottom leftwards, left upwards, top rightwards, each side starting at a corner and the
   corners shared out one per side (Recoil's pinwheel, unrotated: TA has no build facing). */
unsigned place_ring(int fx, int fz, int tx, int tz, int tw, int th, PlaceCell* out, unsigned cap);

/* Build Split over n equal-cost sites for k participants already sorted by build power,
   strongest first. Participant i owns sites [first, first+count) and continues, after them,
   from `follow` round the list (place_split_order). */
typedef struct { uint16_t first, count, follow; } PlaceChunk;
void place_split(const uint32_t* power, unsigned k, unsigned n, PlaceChunk* out);

/* One participant's order of the n sites: its own chunk, then every other site from
   `follow`, wrapping. Writes n indices. */
void place_split_order(const PlaceChunk* c, unsigned n, uint16_t* out);

#endif
