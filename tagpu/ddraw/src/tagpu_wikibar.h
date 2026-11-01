#ifndef TAGPU_WIKIBAR_H
#define TAGPU_WIKIBAR_H
/* tagpu_wikibar -- the game frame's two bars in the running mod's own art, for the in-game wiki's
   stage: the resource bar along the top (DrawGameScreen 0x468E3F..0x46960B) and the strip along
   the bottom (0x46A860), composed as the engine composes them, in palette indices, then through
   the mod's palette into RGBA. The mockup's tools/wiki/mockup/bar.js `compose` and `rgba` are the
   specification, line for line, and tools/wiki/barcheck.mjs compares the two byte for byte over
   every side pack; bar.js is held pixel-exact against a capture of the game's frame there. The
   engine's rules are DISASSEMBLED (research/notes/ingame-wiki-replay-plan.md, *What R0 found*),
   each cited in tagpu_wikibar.c where it is applied.

   THE PICTURE is WIKIBAR_W x WIKIBAR_H indices, WIKIBAR_W a row: rows 0..31 the top bar, rows
   32..63 the bottom bar, each w = Wg - 128 columns wide, column x being the frame's column 128 + x.
   Column 0 is the frame's column 128, which PANELSIDE covers where it is 129 columns wide
   (retail's): the bars themselves start at the frame's column 129.

   PURE AND SAFE ON ANY THREAD. No global, no engine address, no allocation: it reads the sidebar's
   art (WIKISIDE, one block immutable once published and never freed) and the numbers by value,
   and writes only `idx` or `rgba`. Every value read out of the block is bounded before it is used
   as an index: the side against the layout, the badge's colour against its frames (THE ENGINE
   HOLDS NONE), a frame against the sheet, a glyph against its font, and every write against the
   picture.

   Pure C99 and the C library, so barcheck.mjs compiles it natively. Its arithmetic is bar.js's: in
   doubles rounded once per operation (the Makefile builds it for SSE2, as the wiki's other ports),
   `ftol` the engine's 0x4E43A0 under RC=11, truncation. */
#include <stdint.h>
#include "tagpu_huddraw.h"

#define WIKIBAR_W HUD_BAR_W     /* the widest bar: frame x 128 .. 128 + 16 x WIKISTAGE_MAP - 1,
                                   the HUD pass's bar texture, which HOP_BAR samples          */
#define WIKIBAR_H HUD_BAR_H     /* the top bar's 32 rows, then the bottom bar's 32             */

struct WIKISIDE;

/* The numbers the top bar shows, the viewed record's (research/notes/ingame-wiki-replay-plan.md,
   *What R0 found*): the eased stored energy and metal, the two storages, the four rates (income,
   expense), and for the ticks the raw stored values and the share levels. Any double is safe; the
   metal rates `mi` and `mo` print as bar.js prints them while they are finite and below 9e14 in
   magnitude (tagpu_wikibar.c fixed1), every other value whatever it is. */
typedef struct {
    double e, m, emax, mmax, ei, eo, mi, mo, raw_e, raw_m, lev_e, lev_m;
} WIKIBARNUM;

/* THE BARS of layout side `side` with badge colour `colour`, in a frame Wg x Hg game pixels,
   showing `n`, the top bar in layout side `top`'s art, into `idx` (WIKIBAR_W x WIKIBAR_H, cleared
   first). THE TWO SIDES (decision 15 as M0-T #2 MEASURED it: a CORE seat taken over on an ARM
   load): the top bar -- its PANELTOP and the PANELBOT tiles after it (0x469050..0x46909D), the
   RaceSideData rects, the bars' colours and the font -- is `top`'s, and so is the badge's place,
   while the badge's frame is the colour's (0x467C00: by colour, not by side); the bottom bar
   (0x46A860's PANELBOT tiles) and PANELSIDE's column 128 on both bars' rows are `side`'s, the
   load's. `top` is `side` but in a takeover's live game. Returns the bars' width w = Wg - 128, or
   -1 when there is nothing to compose -- either side has no bar art, or the pack no GUI table (the
   stage draws the whitebox strips), or the frame is outside 129..128 + WIKIBAR_W columns or under
   64 rows. A colour that is not one of the badge's frames draws no badge. */
int  tagpu_wikibar_compose(const struct WIKISIDE* sd, int side, int top, int colour, int Wg, int Hg,
                           const WIKIBARNUM* n, uint8_t* idx);

/* The composed bars as RGBA through the pack's palette, alpha 255, WIKIBAR_W a row; the columns
   past `w` are transparent (all 0). `rgba` is WIKIBAR_W x WIKIBAR_H x 4 bytes. */
void tagpu_wikibar_rgba(const struct WIKISIDE* sd, const uint8_t* idx, int w, uint8_t* rgba);

#endif
