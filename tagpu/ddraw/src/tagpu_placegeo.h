#ifndef TAGPU_PLACEGEO_H
#define TAGPU_PLACEGEO_H
/* The shapes of a multi-site placement gesture and Build Split's shares, in TA build cells, and
   the take-off rule that decides which queued orders a gesture's sites take off. Engine-free
   and thread-free: no engine header, no Win32, no state. Recoil's
   CGuiHandler::GetBuildPositions and BAR's api_build_orders.lua, in exact integers
   (research/notes/tadr-port/quality-of-life.md, "Queue editing and line building"). The game
   (tagpu_place.c) and the in-game wiki's simulation (tagpu_wikisim.c) both call these. */
#include <stdint.h>

#define PLACE_MAX_SITES   256
#define PLACE_SPACING_MAX 16
/* A builder's list stops growing at this many orders: below the 4096 nodes a recorded list
   holds (REC_ORDERS_MAX, tagpu_recfmt.h), so no gesture makes a list the recorder would cut. */
#define PLACE_LIST_CAP    1024
#define PLACE_NO_SITE     0xFFFFu   /* place_take_offs_one's `by` for an order no site takes */

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

/* Positions are order positions: world units in 16.16 fixed point, as 0x419670 writes them at
   an order node's +0x22/+0x2A -- a footprint of fx cells at first cell cx is at
   (2cx + fx) << 19. */

/* One queued order of a builder: its position, and `match` when it is of the builder's own
   build action (the only orders a Shift-click can take off). */
typedef struct { int32_t x, z; uint8_t match; } PlaceOrder;

/* One site of a gesture: its order position, and `refused` when the site test refuses it. */
typedef struct { int32_t x, z; uint8_t refused; } PlaceSitePos;

/* 0x43AFC0's predicate, DISASSEMBLED 0x43B004..0x43B029: the position within 16 world units of
   the node on both axes, inclusive, as an unsigned compare of the wrapped difference. */
int place_within(int32_t px, int32_t pz, int32_t nx, int32_t nz);

/* THE TAKE-OFF RULE for one builder, TA's Shift-click per site against the builder's queue as
   it stands, each queued order taken off by at most one site: every site not refused first
   takes the `match` order at its own position, then each site that took none takes the first
   `match` order within 16 world units (place_within) that no site has taken -- both in site
   order, each in queue order. Own positions go first over the whole shape because neighbouring
   1 x 1 sites are 16 world units apart: a site beside a queued row would otherwise take its
   neighbour's order, and each later site the next one's, leaving the row's last site ordered
   afresh -- whichever end a drag starts from, every order of the row is taken off by the site
   that stands on it, and a site that stands beside it is placed.

   It reads only its arguments. It writes `by[j]`, the site that takes order j off
   (PLACE_NO_SITE for none), for the `nq` orders, and `took[i]`, 1 when site i took one, for the
   `n` sites, and returns how many sites took one. The preview's colours and the commit's
   removals both read what it leaves, so for the same queue the preview shows what the release
   does. A builder's answer reads only its own queue and which sites are refused, so a caller
   may change a builder's queue once it has used that builder's answer. */
int place_take_offs_one(const PlaceOrder* q, unsigned nq, const PlaceSitePos* s, unsigned n,
                        uint16_t* by, uint8_t* took);

#endif
