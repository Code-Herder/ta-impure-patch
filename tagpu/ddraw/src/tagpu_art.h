#ifndef TAGPU_ART_H
#define TAGPU_ART_H
/* tagpu_art.h — Impure's own UI art, baked into the DLL from art/ui/.

   The art is ours: painted for Impure in each style's look, committed as PNGs
   under art/ui/<style>/, and baked by tools/artbake.py into palette indices at
   build time (the generated inc/tagpu_art_gen.h). Nothing here comes from the
   game; the design is research/notes/tadr-port/demo-recorder-s5-ui.md, *The art*.

   A STYLE is a family of mods that share a look: "retail", "escalation",
   "twilight". An entry a style does not carry is taken from "retail", so a
   style that is still being drawn shows the retail art rather than nothing.

   LIFETIME: tagpu_art_init inflates the art once, into memory that is never
   freed or written again, so every pointer it hands out is valid for the life
   of the process and may be read from any thread. Call it before any reader;
   it returns 0, and every lookup NULL, when the art cannot be inflated. */

typedef struct TAGPU_ART_FRAME {
    unsigned short       code;      /* a font frame's character, else its position */
    unsigned short       w, h;
    short                hotx, hoty;/* a font frame's baseline is its hoty          */
    unsigned char        key;       /* the transparent index                        */
    const unsigned char* pix;       /* w*h palette indices, rows top to bottom      */
} TAGPU_ART_FRAME;

typedef struct TAGPU_ART_ENTRY {
    const char*            style;
    const char*            name;
    int                    nframes;
    const TAGPU_ART_FRAME* frames;
} TAGPU_ART_ENTRY;

int tagpu_art_init(void);

/* the entry `name` of `style`, else of "retail"; NULL when neither has it */
const TAGPU_ART_ENTRY* tagpu_art_entry(const char* style, const char* name);

/* the frame of `e` whose code is `code` (a font's character); NULL if none */
const TAGPU_ART_FRAME* tagpu_art_char(const TAGPU_ART_ENTRY* e, int code);

/* every entry, for the self-test: `i` from 0 while it returns non-NULL */
const TAGPU_ART_ENTRY* tagpu_art_at(int i);

#endif
