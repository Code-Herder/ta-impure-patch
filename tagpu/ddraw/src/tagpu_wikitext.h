#ifndef TAGPU_WIKITEXT_H
#define TAGPU_WIKITEXT_H
/* tagpu_wikitext -- the wiki's shown text, filled in: the mockup's tools/wiki/mockup/text.js, which
   is its specification, rule for rule. An entry's title, subtitle and paragraphs, its demos' titles
   and their captions name units by cast ({builder}) and mouse buttons by action ({order}); the fill
   puts in the display names of the side the tutorial plays and the words of the player's interface
   type (research/notes/ingame-wiki.md, the owner's decisions 16 and 18). Filling is presentation
   only: the simulation and its hash keep the raw text.

   THE TOKEN RULE, text.js's: a token is a "{" and everything up to the next "}"; a "}" on its own,
   and a "{" with no "}" after it, are plain text. A token whose name has a word is replaced by it;
   any other is copied as written, so a gap shows on the page instead of reading as a sentence.
   tagpu_wikidata.c's loader refuses every entry holding such a token (text_check), so no loaded
   entry's text does.

   PURE AND ALLOCATION-FREE: it reads its arguments and writes the caller's buffer, and nothing else,
   so any thread may call it and tools/wiki/textparity.mjs compiles it natively; that check fills
   every entry's text with every casting at four interface values through text.js and through this
   file and compares the bytes. */
#include <stddef.h>
#include "tagpu_wikidata.h"

/* The words a text is filled with: each cast's display name, NULL when it has none, and the
   interface type, 1 or 0. The names point into the casting and the entry they were taken from
   (tagpu_wikitext_words), so a WIKIWORDS is good exactly as long as both are. */
typedef struct {
    const char* cast[WIKI_NCAST];
    int         iface;
} WIKIWORDS;

/* text.js's words(): cast c's name is the casting's title when the casting binds c (its type when
   the title is empty); else the name of the first of the entry's types in file order whose cast is
   c, its `name` when the entry gives a non-empty one, else its key; else NULL. `casting` NULL
   casts nothing, as text.js's null casting: every name then comes from the entry. `iface` is 1 only
   when it is exactly 1: the engine's left-button readers test main+0x37EFA `== 1` (0x43E505,
   0x43F9EF, 0x499046, 0x499567, DISASSEMBLED), so any other value plays the left-click interface.
   `e` NULL gives no unit names. */
void tagpu_wikitext_words(WIKIWORDS* w, const WIKIENTRY* e, const WIKICASTING* casting, int iface);

/* text.js's fillText(): `raw` (NULL reads "") with every token replaced by its word, into `out`.
   THE BOUND is `n`, the bytes of `out` with its terminator: a fill longer than n - 1 bytes is cut
   at the last UTF-8 character that ends within them, as tagpu_wikistage.c's copy cuts, and the
   result is always terminated. 1 when it was cut (n == 0 writes nothing and answers 1), else 0. */
int tagpu_wikitext_fill(const char* raw, const WIKIWORDS* w, char* out, size_t n);

#endif
