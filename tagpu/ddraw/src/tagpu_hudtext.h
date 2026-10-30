/* tagpu_hudtext -- one line of HUD text as the browser sets it: the code points of its UTF-8,
   upper-cased as CSS does, and the pen before each glyph. One rule for the drawing
   (tagpu_huddraw.c) and for the layout's measure (tagpu_hudart.c, tools/hudart/laycheck.c), so a
   box sized to its text is sized to the text that is drawn. Pure: any thread. */
#ifndef TAGPU_HUDTEXT_H
#define TAGPU_HUDTEXT_H
#include <stddef.h>
#include <stdint.h>
#include "stb_truetype.h"

/* the glyphs one line sets at most; the rest of a longer string is not drawn. The in-game wiki's
   text sets a line as long as its panel is wide -- some 200 glyphs at 16:9, twice that on a 32:9
   screen -- so the cap is the most of one wiki text the layout sets (tagpu_wikilay.c MAXGLYPH,
   which breaks at most that many code points of a text and leaves the rest of it unset, and does
   not compile above this cap): no line the layout sets is cut here. The draw keeps four arrays of
   it on its stack, 32 KB */
#define TAGPU_HUDTEXT_MAX 2048

/* the next code point of UTF-8, or U+FFFD for a bad sequence */
uint32_t tagpu_hudtext_utf8(const unsigned char** s, const unsigned char* e);

/* CSS `text-transform: uppercase` for what a HUD string can hold: ASCII and Latin-1 */
uint32_t tagpu_hudtext_upper(uint32_t c);

/* The pen before each of the `n` glyphs `gi` (into `pen`, unless NULL) and the line's advance:
   each glyph's advance and the kerning before it at the stb_truetype scale `sc`, and `sp` of
   letter-spacing after every glyph, the last included, as CSS `letter-spacing` does. */
float tagpu_hudtext_pens(const stbtt_fontinfo* f, float sc, float sp, const int* gi, int n, float* pen);

/* The advance of the UTF-8 `s` (`len` bytes) in face `f` at `size` -- in whatever unit size is
   in -- with `spacing` em of letter-spacing, upper-cased when `upper`, its first
   TAGPU_HUDTEXT_MAX glyphs: the width tagpu_huddraw.c draws it with. */
float tagpu_hudtext_width(const stbtt_fontinfo* f, float size, float spacing, int upper,
                          const char* s, size_t len);

#endif
