#ifndef TAGPU_MODSLAY_H
#define TAGPU_MODSLAY_H
/* tagpu_modslay -- the Mods screen's window: the browser mockup (tools/mods/mockup/) in C, over the same
   art/mods/layout.json and the wiki's kit, its text in art/hud/layout.json's styles and the replay HUD's faces.
   tools/mods/modslaycheck.py pairs this file's boxes, lines and baselines with the mockup's.

   A PURE FUNCTION OF ITS INPUTS, like tagpu_wikilay.c: tagpu_modslay_build reads the loaded files, the faces and the
   page it is given, and nothing else. It runs once a frame on the render thread, and the frame's HOP_HOT ops are the hit
   map the game thread tests a click against, so a click lands on what the player saw. Pure C99 and the C library.

   The screen is given in pixels; every coordinate out is in units, the screen's height / 720, from its top left.

   THE CONTROLS' KEYS, what the game thread reads a click by (tagpu_modsui.c):
     tab:0 tab:1 tab:2     Manage, Explore, Updates
     row:<i>               row i of the list: selects it
     act:<i>               row i's action button (Install, Update, or its radio on Manage)
     close back confirm updateall
     link:<n>              a showcase's link: 0 its website, n its n-th link block (MODSSHOW), which opens the player's browser
     scroll:list scroll:detail   a scrollbar's track, as the wiki's */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_hudlay.h"
#include "tagpu_json.h"
#include "tagpu_uikit.h"

#define MODS_MAXHOT   128
#define MODS_KEY      16u
#define MODS_MAXROW   16        /* "This folder", the catalog's mods; a mutator lists here too */
#define MODS_MAXOPT   8

#define MODS_MAXBLOCK 64        /* a showcase's blocks, a list's items each one: tools/modpack/modpack.py SHOW_BLOCKS */
#define MODS_SHOWTEXT 6144      /* a showcase's strings, each with its NUL: modpack.py SHOW_TEXT */
#define MODS_MAXCOLS  4         /* a list's columns: modpack.py SHOW_COLS */
#define MODS_SHOWLINE 2048      /* a block's text with its link, in bytes: no more code points than the widgets set in one
                                   text (UI_MAXGLYPH); modpack.py SHOW_LINE */

enum { MODS_TAB_MANAGE, MODS_TAB_EXPLORE, MODS_TAB_UPDATES, MODS_NTAB };
enum { MODS_SCROLL_LIST, MODS_SCROLL_DETAIL, MODS_NSCROLL };

/* A row's badge. */
enum { MODS_BADGE_NONE, MODS_BADGE_ACTIVE, MODS_BADGE_UPDATE, MODS_BADGE_GREY, MODS_BADGE_CONFLICT };
/* A row's control at its right. */
enum { MODS_ACT_NONE, MODS_ACT_RADIO, MODS_ACT_RADIO_ON, MODS_ACT_INSTALL, MODS_ACT_UPDATE, MODS_ACT_BUSY, MODS_ACT_CHECK, MODS_ACT_CHECK_ON };
typedef struct {
    char    name[48];           /* UTF-8, as every text here; a NUL inside its array bounds it */
    char    detail[48];         /* under the name: a version, a state */
    uint8_t badge;              /* MODS_BADGE_* */
    uint8_t act;                /* MODS_ACT_* */
    int16_t progress;           /* MODS_ACT_BUSY's bar, 0..1000; -1 no bar */
    char    badge_text[16];
} MODSROW;

typedef struct {
    char    label[48];
    char    value[16];          /* the value in force, a number or "On" and "Off": shown, never set here */
    char    reason[120];        /* a locked option's: why it is off */
    uint8_t locked;             /* greyed, a lock disc */
} MODSOPT;

/* A showcase's block: a paragraph, a section's opening line (one blank line above it), a list's item, or a paragraph ending
   in a link. A list is a run of items, the first marked. */
enum { MODS_BLOCK_PARA, MODS_BLOCK_SECTION, MODS_BLOCK_ITEM, MODS_BLOCK_LINK };
typedef struct {
    uint8_t  kind;              /* MODS_BLOCK_* */
    uint8_t  cols;              /* an item: its list's columns, 1..MODS_MAXCOLS, filled down each */
    uint8_t  first;             /* an item: the first of its list */
    uint16_t text;              /* its text's offset in MODSSHOW.text */
    uint16_t link;              /* a link: its address's offset there, an https address */
} MODSBLOCK;

/* A mod's showcase, from its catalog entry's `show` (tagpu_modslay_show), which the pane draws in place of the title, the
   meta and the summary. Its pictures are the kit's whole parts logo_<id>, banner_<id> and backdrop_<id>, each drawn when the
   kit has it (tools/mods/genparts.py); a mod with no logo shows its name. */
typedef struct {
    char      id[32];           /* "" no showcase */
    char      name[48];
    char      version[32];      /* "Version 4.8" */
    char      by[96];           /* "By ..." */
    char      site[160];        /* an https address: link:0 */
    int       nblock;
    MODSBLOCK block[MODS_MAXBLOCK];
    char      text[MODS_SHOWTEXT];
} MODSSHOW;

/* The page as the player has it. Values only, read on a copy: every count is bounded in the build. */
typedef struct {
    float      W, H;            /* the screen, pixels */
    int        tab;
    int        updates;         /* the count on the Updates tab; 0 shows none */
    int        nrow;
    int        selected;        /* the row the pane shows; -1 none */
    MODSROW    row[MODS_MAXROW];
    char       empty[120];      /* the list's text when nrow is 0 */
    char       title[48];       /* the pane */
    char       meta[120];       /* author and version */
    char       summary[400];
    char       note[200];       /* under the summary: a refusal, an install's failure, what Confirm will do */
    MODSSHOW   show;            /* show.id "": the title, the meta and the summary are the pane's head */
    int        nopt;
    MODSOPT    opt[MODS_MAXOPT];
    float      scroll[MODS_NSCROLL];
    unsigned   gen;             /* the page's shape generation, handed back with the frame's hit map */
    char       held[MODS_KEY];  /* the key of the control held down; "" none */
    char       confirm[48];     /* Confirm's label; "" shows it greyed with the word Confirm */
    int        update_all;      /* the Updates tab's button shows */
} MODSPAGE;

/* What one build answers besides its ops: each panel's scroll as drawn and its limit, its scrollbar as drawn (the track, h 0
   when it shows none, the thumb's top and length on it), as WIKIFRAME does. */
typedef struct {
    float   scroll[MODS_NSCROLL];
    float   scroll_max[MODS_NSCROLL];
    HUDRECT track[MODS_NSCROLL];
    float   thumb_y[MODS_NSCROLL], thumb_h[MODS_NSCROLL];
    HUDRECT window;
} MODSFRAME;

typedef struct MODSLAY MODSLAY;

/* layout.json parsed and resolved against the wiki's kit `kit` (tagpu_wikilay_kit), which must outlive the layout. NULL with the
   reason in `err`. */
MODSLAY* tagpu_modslay_load(const char* layout, size_t nlayout, const HUDLAY* kit, char* err, size_t errn);
void     tagpu_modslay_free(MODSLAY* l);
/* Set part `part`'s lightness (ui_face_light of its decoded image), once, before the layout is published: a kit does not carry it,
   the wiki's layout keeping its own table the same way (tagpu_wikilay_light). A part never set is dark. */
void     tagpu_modslay_light(MODSLAY* l, int part, int light);

/* The ops for `st` into `out` (never on a stack) and what the frame answers. 0 with the reason in `err` for a missing argument or
   no memory; `out->overflow` is set when an op, a text or a hit did not fit and was dropped. */
int      tagpu_modslay_build(const MODSLAY* l, const UIFACES* f, const MODSPAGE* st, HUDOPS* out, MODSFRAME* fr, char* err, size_t errn);

/* A catalog entry's `show` (tools/modpack/modpack.py check_show) into `out`, for the mod `id` named `name` at `version`. 0, and
   out->id "", when there is none or any of it does not fit MODSSHOW whole -- a string its array would cut, a block past
   MODS_SHOWLINE: a showcase is drawn whole or not at all. */
int      tagpu_modslay_show(const TJV* show, const char* id, const char* name, const char* version, MODSSHOW* out);

/* At the load: 1 when the fullest page there can be builds whole, with nothing dropped and every row's controls within the row, on
   1024 x 768, 1280 x 720 and 3840 x 2160, at every scroll. `scratch` is a HUDOPS the caller owns. 0 with the reason in `err`. */
int      tagpu_modslay_check(const MODSLAY* l, const UIFACES* f, HUDOPS* scratch, char* err, size_t errn);

#endif
