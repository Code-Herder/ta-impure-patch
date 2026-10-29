#ifndef TAGPU_WIKILAY_H
#define TAGPU_WIKILAY_H
/* tagpu_wikilay -- the in-game wiki's window: the browser mockup (tools/wiki/mockup/ index.html,
   wiki.css, wiki.js) in C, over the same art/wiki/layout.json and art/wiki/kit/kit.json, its text
   in art/hud/layout.json's styles and the replay HUD's faces. research/notes/ingame-wiki.md is the
   specification; tools/wiki/wikilaycheck.py pairs this file's boxes, lines and baselines with the
   mockup's.

   A PURE FUNCTION OF ITS INPUTS, like tagpu_hudlay.c: tagpu_wikilay_build reads the loaded files,
   the wiki's data, the faces and the state it is given, and nothing else -- no engine memory, no
   global, no clock. It is called once a frame on the render thread, and the frame's HOP_HOT ops
   are the hit map the game thread tests a click against, so a click lands on what the player saw.
   A WIKILAY is immutable once loaded and its lights set, and is never freed while the wiki can
   draw.

   THE TEXT IS FILLED HERE, AS IT IS READ: an entry's title (in the window and as its tree leaf),
   subtitle, paragraphs and demo titles go through tagpu_wikitext_fill with the words of that
   entry under the page's casting and interface type (WIKIPAGE.casting, .iface), as wiki.js
   `fill` does; the caption comes filled. It is a pure fill over immutable blocks -- the wiki's
   data and the published WIKISIDE's casting, neither ever freed -- so the render thread reads
   nothing the game thread writes, and the record carries two small values rather than every
   text of the window filled.

   Pure C99 and the C library, so tools/wiki/ compiles it natively (wikilaycheck.c).

   The screen is given in pixels; every coordinate out is in wiki units, the screen's height / 720,
   from the screen's top left -- the mockup's unit, and the HUD's at a scale of 100. */
#include <stddef.h>
#include <stdint.h>
#include "stb_truetype.h"
#include "tagpu_hudlay.h"
#include "tagpu_wikidata.h"

#define WIKI_MAXHOT    160      /* the hit map: TAGPU_CMD_WIKI's hits (bound: tagpu_wikilay.c)   */
#define WIKI_MAXNODE   1024     /* the tree's nodes the loader keeps: one fold bit each           */
#define WIKI_CAPTION   WIKI_CAPTION_MAX /* the caption's bytes, with its terminator: every loaded
                                           caption's fill fits                                  */

/* the three panels that scroll, the order of WIKIPAGE.scroll and the keys scroll:tree,
   scroll:demos and scroll:text */
enum { WIKI_SCROLL_TREE, WIKI_SCROLL_DEMOS, WIKI_SCROLL_TEXT, WIKI_NSCROLL };

/* The page as the player has it: what wiki.js keeps in S, and the demo's view (caption, keys). */
typedef struct {
    float    W, H;              /* the screen, pixels: the unit is H / 720                      */
    float    bar;               /* the top bar's height in units: the wiki button sits in its own
                                   pixels, bar / 32 units each, as tagpu_hudlay.c `live` places the
                                   rewind button                                                */
    int      rewind;            /* this frame draws the live HUD's rewind button: the wiki button
                                   stands left of it, else in its slot                          */
    int      open;              /* the window is drawn; closed, only the button is              */
    int      entry;             /* the selected entry, an index of WIKIDATA.entry; -1 none      */
    int      demo;              /* the selected demo of that entry                              */
    uint32_t fold[WIKI_MAXNODE / 32];   /* bit n: tree node n (WIKIDATA.node) is folded         */
    float    scroll[WIKI_NSCROLL];      /* each panel's offset, units; the build holds it to the
                                   panel's limit (WIKIFRAME)                                    */
    int      playing;           /* the demo plays: the play button shows pause                  */
    int      done;              /* the demo has ended and holds its last frame: play shows play  */
    int      speed;             /* 1 or 2: 1x or 2x is down                                     */
    int      try_it;            /* Try it yourself has the stage                                */
    int      mod;               /* MOD_* (tagpu_mod.h): the mod a lock is read for              */
    int      sim_locked;        /* the demo stopped at a build or a RECLAIM this mod's sidebar
                                   cannot reach (the view's `locked`): the badge shows, as for an entry the
                                   data locks (wiki.js `off`)                                   */
    char     held[HUD_KEYLEN];  /* the key of the control held down; "" none                    */
    char     caption[WIKI_CAPTION]; /* the demo's caption, UTF-8, filled; "" none               */
    const WIKICASTING* casting; /* the casting the text names units by, NULL: each entry's own
                                   names. LIFETIME: it lies in the published WIKISIDE block,
                                   immutable and never freed (tagpu_wikiasset.h)                */
    int      iface;             /* the Interface Type the text names buttons by: 1 exactly when
                                   1, as tagpu_wikitext_words takes it                          */
    unsigned keys;              /* bit WIKI_KEY_* (tagpu_wikidata.h): held in the demo's view   */
} WIKIPAGE;

/* A text style's face (art/hud/layout.json `styles`, by index): tagpu_hudart.c's HUDFACE. */
typedef struct {
    const stbtt_fontinfo* info;     /* NULL: the style's text measures nothing                  */
    float                 ascent;   /* em, descent positive, as the browser lays a line out by  */
    float                 descent;
} WIKIFACE;

/* The faces, by style index, and two answers the browser gives otherwise than the faces do. Both
   are NULL in the game, which sets every text in its own faces at its own size; the check gives
   the browser's, so the layout's rules are compared on what the mockup sets, and the two gaps are
   named (research/notes/ingame-wiki.md, "Gaps"):
     missing  the advance of a code point no face has, which the mockup sets in the browser's
              fallback face and the DLL does not carry; NULL measures it as its face's .notdef, as
              the game draws it. The check fails on any text that needs it: the control glyphs and
              the tree's marks, which no face has, are kit icons.
     size     the size a face is set at for a font size, both in pixels. Chrome sets a face at a
              size of whole sixty-fourths of a pixel near the CSS one, by a rule not identified
              (MEASURED 2026-10-25: 9.0667 px is set at 579/64, 9.07 at 580/64, 10.6667 at
              682/64, 12.75 at 816/64), so at a font size that is no whole number of
              sixty-fourths its lines are up to 0.2 % narrower than the faces'; NULL sets it at
              the size. */
typedef struct {
    WIKIFACE face[24];
    double (*missing)(void* ctx, int style, double size, uint32_t cp);   /* units at `size` units */
    double (*size)(void* ctx, int style, double px);
    void*    ctx;
} WIKIFACES;

/* What one build answers besides its ops: each panel's scroll as drawn and its limit, its
   scrollbar as drawn (the track, h 0 when the panel shows none, and the thumb's top and length on
   it, so a press is read against the thumb the player saw), the stage's rect inside its bezel (the
   stage draws there and a click on it is read there), the window, and whether the caption's lines
   passed its strip, whose overflow is hidden. A closed window answers zeros. */
typedef struct {
    float   scroll[WIKI_NSCROLL];
    float   scroll_max[WIKI_NSCROLL];
    HUDRECT track[WIKI_NSCROLL];
    float   thumb_y[WIKI_NSCROLL], thumb_h[WIKI_NSCROLL];
    HUDRECT stage;
    HUDRECT window;
    int     caption_cut;        /* the strip shows only the caption's first lines */
} WIKIFRAME;

typedef struct WIKILAY WIKILAY;

/* layout.json and kit.json, parsed and resolved against the replay HUD's layout `hud`, whose text
   styles and measure the kit then carries (tagpu_hudlay_load_kit), so the draw sets the wiki's text
   in the HUD's faces by the same rules: every number the rules read, every part they draw, every
   role's style. NULL with the reason in `err`. `png_size` is tagpu_hudlay_load's. `hud` must
   outlive the layout. */
WIKILAY*      tagpu_wikilay_load(const char* layout, size_t nlayout, const char* kit, size_t nkit,
                                 HUDPNGSIZE png_size, void* ctx, const HUDLAY* hud,
                                 char* err, size_t errn);
void          tagpu_wikilay_free(WIKILAY* l);
/* The kit, for the draw: HOP_PART ops index its parts. */
const HUDLAY* tagpu_wikilay_kit(const WIKILAY* l);

/* THE FACE LIGHTNESS RULE (wiki.js faceLight): a part's face is light when its middle, inside its
   slices, scaled to 8 x 8 and its opaque pixels (alpha above 128) averaged at 0.3 R + 0.59 G +
   0.11 B, is above a grey of 120; text on a light face is drawn in dark ink with no shadow. Here
   each of the 64 cells is the box average of the source pixels it covers, the canvas's scaling
   approximated. `rgba` is w x h x 4 bytes, premultiplied by its alpha when `premultiplied`.
   Pure. */
int           tagpu_wikilay_face_light(const unsigned char* rgba, int w, int h, const float slice[4],
                                       int premultiplied);
/* Set part `part`'s lightness (tagpu_wikilay_face_light of its decoded image), once, before the
   layout is published. A part never set is dark. */
void          tagpu_wikilay_light(WIKILAY* l, int part, int light);

/* The data's name of a MOD_* (an entry's `lockedIn` names): "stock", "escalation", ... */
const char*   tagpu_wikilay_mod_name(int mod);

/* The ops for page `st` over the wiki `d`, into `out` (about 500 KB: never on a stack), and what
   the frame answers into `fr`. The state's indices are bounded against the data: an entry or a
   demo it does not have reads as none. Returns 0 with the reason in `err` only for a missing
   argument or no memory for the build's text buffers (about 51 KB); `out->overflow` is set when an
   op, a text or a hit did not fit and was dropped. */
int           tagpu_wikilay_build(const WIKILAY* l, const WIKIDATA* d, const WIKIFACES* f,
                                  const WIKIPAGE* st, HUDOPS* out, WIKIFRAME* fr, char* err, size_t errn);

/* The kit and the data checked at the load: 1 when the kit holds every icon the window and the
   stage draw and the stage's `toast` style, and the page builds whole -- nothing dropped, no
   `overflow` -- in each of these states on each of three screens, 1024 x 768, 1280 x 720 and
   3840 x 1080, every scroll at 0: the window closed with its button held, open with its close
   button held, and every entry's every demo playing at 1x and ended at 2x with Try it yourself
   held. Each state is filled at interface type 1, whose words are the longer, with each entry's
   own names, with each of the `ncasting` castings given, and with the two worst castings there
   can be (every display name of 31 characters, the engine's cut: 31 of three bytes, the
   WIKI_TITLE_MAX - 1 bytes, as one unbreakable word, and 16 of them broken by spaces into the
   most lines). With the entry's own names and each casting
   given, every caption of the demo is built in turn and must show whole in its strip
   (WIKIFRAME.caption_cut); with the worst casts, the demo's longest caption only, for the ops
   (the strip clips the rest of a caption's lines, which then make no op). Else 0 with the first
   failure in `err`. The build itself refuses only a missing argument or its memory, so the states
   test the op, run, text, hit and group bounds; another scroll, a held key or another screen is
   not built here, and a frame that overflows all the same is drawn with the ops that fit
   (tagpu_wikistate.c says so once). `scratch` takes the ops of the states tried. A load-time
   call: any thread, before the layout is published; the game's passes no casting, its side
   loading later, and tools/wiki/wikilaycheck.c passes every pack's. */
int           tagpu_wikilay_check(const WIKILAY* l, const WIKIDATA* d, const WIKIFACES* f,
                                  const WIKICASTING* castings, int ncasting, HUDOPS* scratch,
                                  char* err, size_t errn);

#endif
