#ifndef TAGPU_ICONSET_H
#define TAGPU_ICONSET_H
/* tagpu_iconset.h -- a mod's own strategic-icon set, the PURE half: bytes in,
   structs out.

   Several mods ship an icon set for TADR's megamap: an `iconcfg.ini` that
   TADR's own ini names (`[Preferences] MegaMapConfig`), and a folder of small
   8-bit PCX pictures in the TA palette. Impure draws a type with that set's
   picture where the set has one for it (research/notes/mega-map-plan.md,
   landing 4). TADR's code never runs for it: its files are read from the
   player's install, at run time, and parsed here.

   Nothing in this file reads a file, names engine memory or calls Win32, so
   tests/iconsettest.c runs every line of it on the host, malformed input
   included. The game-thread half -- which files, the engine's category map,
   which type takes which picture -- is tagpu_packet_pub.c's.

   EVERY INPUT HERE IS DATA. A PCX's dimensions, its run lengths and its row
   stride, an ini's line lengths and every value are bounded against the
   buffer they are written to before a byte is written; anything that does not
   fit is refused, never cut. */
#include <stddef.h>
#include "tagpu_icontype.h"   /* TAGPU_ICONRECT, TAGPU_ICONART_SETS */

/* ---- PCX ------------------------------------------------------------------
   TADR's icon is at most 22 x 22 (ICONMAXWIDTH/HEIGHT, UnitDrawer.h); the
   row stride a real file declares is its width, and 1024 is far past any. */
#define TAGPU_PCX_MAXW   22
#define TAGPU_PCX_MAXH   22
#define TAGPU_PCX_MAXBPL 1024

/* Decode an 8-bpp, one-plane, RLE PCX of at most TAGPU_PCX_MAXW x _MAXH into
   `out` (w x h palette indices, rows top-down), at most `cap` bytes. The
   texels are TA palette indices as they are (TADR's reading); the trailing
   palette is ignored. The stream is `h x bytes_per_line` bytes, a run may
   cross a row, and only the first `w` of each row are kept -- unlike TADR,
   which decodes `w x h` and so misreads a file whose stride is wider. 1 on
   success with *w, *h set; 0, with both 0, for a header that is not such a
   PCX, dimensions out of range, an image larger than `cap`, or a stream that
   ends first. */
int tagpu_pcx_decode(const unsigned char* b, size_t n, unsigned char* out, int cap, int* w, int* h);

/* ---- the profile files ------------------------------------------------------
   Read the way the Win32 profile API reads them: `[section]` and keys compared
   ASCII case-insensitively, whitespace trimmed around the section name, the key
   and the value, a line whose first non-blank character is `;` skipped, a value
   cut at its first `;` and trimmed again, lines ending in LF or CRLF. Only the
   FIRST section of a name is read, as the profile API finds it; a key read is
   its first occurrence there. A section line's name runs to its first `]`, so
   a comment after it (`[Option] ; General Settings`, which every set ships) is
   not part of it. */
#define TAGPU_INI_KEYMAX 64      /* a key's bytes, NUL included             */
#define TAGPU_INI_VALMAX 260     /* a value's, NUL included: MAX_PATH       */

/* The value of `key` in `section`, into `out` (`cap` bytes, always
   NUL-terminated): 1 found, 0 absent, -1 present but longer than `out` holds
   (refused, `out` empty). */
int tagpu_ini_get(const char* text, size_t n, const char* section, const char* key, char* out, int cap);

/* Every `key=value` line of `section`, in file order, to `fn` (0 from it stops
   the walk). A line with no `=`, or whose key or value does not fit
   TAGPU_INI_KEYMAX / _VALMAX, is not passed; `*refused` (may be NULL) counts
   the latter. Returns the lines passed. */
typedef int (*TAGPU_INI_LINE)(const char* key, const char* value, void* ctx);
int tagpu_ini_section(const char* text, size_t n, const char* section, TAGPU_INI_LINE fn, void* ctx,
                      int* refused);

/* TADR's readings of a value: a boolean is TRUE when the lower-cased value
   CONTAINS `true`; a colour is its leading decimal digits when they make
   0..255, else `dflt`. */
int tagpu_ini_bool(const char* value);
int tagpu_ini_byte(const char* value, int dflt);

/* ---- names ----------------------------------------------------------------- */
/* The TADR ini's file name, the string at 0x5098A3 the Community Patch
   Loader rewrites: 1..32 characters of [A-Za-z0-9_.-] and a NUL within `cap`
   bytes of `s`. Reads at most `cap` bytes. */
int tagpu_iconset_ini_name_ok(const char* s, size_t cap);
/* A path the set may name, relative to the folder it is resolved against
   and inside it: non-empty, no drive (`:`), not rooted (a leading `\` or
   `/`), and no segment that climbs -- `..`, or, since Win32's path parser
   trims a segment's trailing dots and blanks, any segment of only dots and
   blanks with two dots or more. `.` stays allowed (`.\Icon\iconcfg.ini`). */
int tagpu_iconset_relpath_ok(const char* s);
/* A side's name as a file name: 1..`cap`-1 printable characters, none of
   `\ / : * ? " < > |`, and a NUL within `cap` bytes. Reads at most `cap`. */
int tagpu_iconset_side_ok(const char* s, size_t cap);

/* ---- the iconcfg ------------------------------------------------------------ */
#define TAGPU_ICONCFG_LINES TAGPU_ICONART_SETS
typedef struct TAGPU_ICONCFG_LINE {
    char key[TAGPU_INI_KEYMAX];          /* the category, or nothing/unknow/nukeicon */
    char file[TAGPU_INI_VALMAX];         /* relative to the iconcfg's folder         */
} TAGPU_ICONCFG_LINE;
typedef struct TAGPU_ICONCFG {
    int fill, transparent, selected;     /* [Option]'s colours; TADR's defaults
                                            0, 9, 89 when absent or not a byte  */
    int custom;                          /* UseDefaultIcon is not TRUE: [Icon]
                                            names the set, else the fixed list   */
    int n;                               /* [Icon] lines kept, in file order     */
    int refused;                         /* [Icon] lines that did not fit, named
                                            no file, or were past the cap        */
    TAGPU_ICONCFG_LINE line[TAGPU_ICONCFG_LINES];
} TAGPU_ICONCFG;
void tagpu_iconcfg_parse(const char* text, size_t n, TAGPU_ICONCFG* c);

/* ---- the load list -----------------------------------------------------------
   Which pictures a set has, in the order TADR loads them, which is the order a
   type's picture is chosen in (the first whose types hold it):
     DEFAULT MODE  `<side>.PCX` for each side, then COMMANDER, AIRCRAFTCOMBAT,
                   AIRCONS, CONS, MOBILECOMBAT, FACTORY, BUILDING, NONE,
                   UNKNOWN, NUKEICON (`.PCX`);
     CUSTOM MODE   `<side>.PCX` for each side, then every [Icon] line in file
                   order: `nothing` is NONE, `unknow` UNKNOWN, `nukeicon`
                   NUKEICON, any other key the category it names.
   What each kind holds is decided per level from the UnitDefs and the
   engine's categories (tagpu_packet_pub.c, TADR's ExternQuickKey masks). */
enum { TAGPU_ISE_SIDE,           /* that side's commanders; arg = the side   */
       TAGPU_ISE_DEFAULT,        /* a default-mode class; arg = TAGPU_ISD_*  */
       TAGPU_ISE_CATEGORY,       /* the types of category `name`             */
       TAGPU_ISE_NONE,           /* a radar contact: no type                 */
       TAGPU_ISE_UNKNOWN,        /* a type no other picture holds            */
       TAGPU_ISE_NUKE };         /* a nuke in flight: loaded, not drawn      */
enum { TAGPU_ISD_COMMANDER, TAGPU_ISD_AIRWEAPON, TAGPU_ISD_AIRCON, TAGPU_ISD_CONS,
       TAGPU_ISD_MOBILEWEAPON, TAGPU_ISD_FACTORY, TAGPU_ISD_BUILDING, TAGPU_ISD_N };
#define TAGPU_ICONSET_SIDES 5    /* the engine's side records               */
#define TAGPU_ICONSET_PLAN  (TAGPU_ICONSET_SIDES + TAGPU_ICONCFG_LINES)
typedef struct TAGPU_ISENTRY {
    int  kind, arg;
    char name[TAGPU_INI_KEYMAX];         /* the category, or the side's name     */
    char file[TAGPU_INI_VALMAX];         /* relative to the iconcfg's folder     */
} TAGPU_ISENTRY;
/* The load list for `c` and the game's `nsides` side names (each checked by
   tagpu_iconset_side_ok; a side that is not is left out), into `out` (`cap`
   entries). Returns the entries written; past `cap` they are dropped. */
int tagpu_iconset_plan(const TAGPU_ICONCFG* c, const char* const* sides, int nsides, TAGPU_ISENTRY* out,
                       int cap);

/* ---- the atlas ---------------------------------------------------------------
   What a texel of a picture is, before it goes into the atlas: nothing (the
   set's TransparentColor, or a logo frame's key), the set's FillColor -- the
   team colour, or white when selected --, its SelectedColor -- shown only
   when selected --, or drawn as its own palette index. */
enum { TAGPU_ITX_KEY, TAGPU_ITX_FILL, TAGPU_ITX_SEL, TAGPU_ITX_DRAWN };
/* A set picture's texel classes, in TADR's precedence: Transparent, then
   Fill, then Selected, so a set naming one index twice is read as TADR
   reads it. */
void tagpu_iconset_classify(const unsigned char* pix, int n, int transparent, int fill, int selected,
                            unsigned char* cls);

typedef struct TAGPU_ICONSRC {
    const unsigned char* pix;            /* w x h palette indices               */
    const unsigned char* cls;            /* w x h TAGPU_ITX_*                   */
    int w, h;
} TAGPU_ICONSRC;
/* Pack `n` pictures into `atlas` (`aw` x `ah`), each a 1-texel gutter from
   the next, in order, row by row. marks[0..2] = key, fill, sel: the three
   smallest palette indices no DRAWN texel of any picture holds, so a marker
   is never a colour of the art; KEY, FILL and SEL texels and every texel no
   picture covers are written as them. rect[i] gets each picture's place
   (hotspot 0). All or nothing: 0 -- with the atlas's contents unspecified --
   for a picture with no pixels, larger than the atlas, more than fit, or
   fewer than three free indices. */
int tagpu_iconart_pack(const TAGPU_ICONSRC* src, int n, unsigned char* atlas, int aw, int ah,
                       TAGPU_ICONRECT* rect, int marks[3]);

#endif
