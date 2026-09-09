/* tagpu_gui_snap.c — on-demand snapshot of the live GUI gadget tree (was tagpu_ui.c;
   the read half of `tacli ui`, now part of the tagpu_gui_* family, gui-renderer.md 4).
   Contract: inc/tagpu_ui.h. Offsets and evidence: research/notes/gui-gadgets.md. */

#include <windows.h>
#include <stdio.h>
#include "tagpu_ui.h"

#define UI_TRIGGER  "tagpu_ui.trigger"
#define UI_OUT      "tagpu_ui.json"
#define UI_TMP      "tagpu_ui.json.tmp"

#define TA_MAINPP   0x00511DE8u   /* TAdynmemStruct** */
#define TA_GFXP     0x0051FBA4u   /* the draw context the GUI renders through */
#define GFX_FONTSET 0x14          /* -> font set                             */
#define FS_FONT     0x0C          /* -> the font the listbox measures with    */
#define FONT_COUNT  0x00          /* u16 glyph count                          */
#define FONT_GLYPHS 0x28          /* glyph[i] = *(void**)(font + i*8 + 0x28)  */
#define GLYPH_H     0x02          /* i16                                      */

#define OFF_GUIINFO 0x519         /* GUIInfo, inline in main            */
#define GI_ACTIVE   0x18          /* GUIMEMSTRUCT* TheActive_GUIMEM     */
#define GI_UICHANGE 0x60          /* int, last actuated gadget index    */

#define GM_PREV     0x00          /* GUIMEMSTRUCT* per_active           */
#define GM_CTRLS    0x04          /* GUI0IDControl* ControlsAry         */

#define STRIDE      0x15B         /* gadget record, pack(1)             */

/* common header */
#define G_ID        0x00
#define G_ASSOC     0x01
#define G_NAME      0x02          /* char[16] */
#define G_XPOS      0x13          /* i16 */
#define G_YPOS      0x15
#define G_WIDTH     0x17
#define G_HEIGHT    0x19
#define G_ATTRIBS   0x1B          /* i32 */
#define G_ACTIVE    0x29          /* u8; zero => engine skips the gadget */
#define G_HELP      0x33          /* char[128] */

/* panel (id 0) */
#define P_TOTAL     0xB6          /* i16 totalgadgets */
#define P_CRDEF     0xCC          /* char[16] gadget Enter activates   */
#define P_ESCDEF    0xDC          /* char[16] gadget Escape activates  */
#define P_FOCUS     0xEC          /* char[16] default focus            */

/* button (id 1, id 12) */
#define B_TEXT      0xB6          /* char[128] */
#define B_STAGES    0x136         /* u8  */
#define B_STATUS    0x137         /* u8  current stage */
#define B_QUICKKEY  0x13A         /* i16 */
#define B_GRAYED    0x13C         /* i32 */

/* listbox (id 2) */
#define L_SELECTED  0xBA          /* i16 selected item                        */
#define L_TOP       0xBC          /* i16 first visible item (scroll position) */
#define L_MAXTOP    0xBE          /* i16 largest legal top = count - visible   */
#define L_COUNT     0xC0          /* i16 item count                            */
#define L_TEXT      0xC2          /* char* flat item blob, \0/\n separated     */
#define L_ITEMH     0xDA          /* i16 */
#define L_FLAGS     0xD6          /* u8* one byte per item, bit0 = enabled    */
#define L_ATTR_TEXT 0x10          /* attribs bit: items are the +0xC2 blob    */
#define L_ATTR_ICON 0xA0          /* attribs bits: items are pictures, no text */
#define L_ATTR_FLAG 0x800         /* attribs bit: +0xD6 is a real flag array  */

/* textfield (id 3) */
#define F_MAXCHARS  0x138         /* i16 */

/* slider (id 4) */
#define S_RANGE     0x136         /* i16 pixel span the knob travels          */
#define S_THICK     0x13C         /* i32 scale the value is expressed in      */
#define S_KNOBPOS   0x140         /* i16 knob offset in pixels                */
#define S_KNOBSIZE  0x142         /* i16 */
#define S_ALTPROC   0x157         /* non-zero => the mouse handler bails out  */
#define S_ATTR_HORZ 0x01          /* attribs bit: knob travels along x        */
#define S_ATTR_DEAF 0x10          /* attribs bit: ignore the mouse entirely   */

#define MAX_GADGETS 512
#define MAX_STACK   16
#define MAX_ITEMS   256           /* per listbox                              */
#define MAX_ITEMLEN 128
#define MAX_BLOB    0x8000        /* how far into the item blob we will walk  */

static void ulog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* Committed, not guard/no-access. A wrong pointer must cost a log line, never
   the process — same rule as tagpu_peek. */
static BOOL readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const char* c = (const char*)p;

    if (!p)
        return FALSE;

    while (n)
    {
        size_t span;

        if (!VirtualQuery(c, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return FALSE;

        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return FALSE;

        span = (size_t)((const char*)mbi.BaseAddress + mbi.RegionSize - c);

        if (span >= n)
            return TRUE;

        n -= span;
        c += span;
    }

    return TRUE;
}

/* How many bytes from p are readable, up to max. The item blob has no length
   field — the engine walks it until it has counted `count` separators — so a
   corrupt pointer must be bounded by the mapping, not by trust. */
static size_t readable_span(const char* p, size_t max)
{
    MEMORY_BASIC_INFORMATION mbi;
    size_t have = 0;

    if (!p)
        return 0;

    while (have < max)
    {
        const char* c = p + have;
        size_t span;

        if (!VirtualQuery(c, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            break;

        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            break;

        span = (size_t)((const char*)mbi.BaseAddress + mbi.RegionSize - c);
        have += span;
    }

    return have > max ? max : have;
}

static const char* type_name(int id)
{
    switch (id)
    {
        case 0:  return "panel";
        case 1:  return "button";
        case 2:  return "listbox";
        case 3:  return "field";
        case 4:  return "slider";
        case 5:  return "label";
        case 6:  return "surface";
        case 7:  return "resource";   /* a font or GAF another gadget indexes */
        case 11: return "picture";
        case 12: return "button2";
        case 13: return "timer";
        default: return "unknown";
    }
}

/* TA strings are fixed-size, NUL-padded, and CP437-ish. Emit strict JSON: escape
   the two mandatory characters and \u-escape anything outside printable ASCII, so
   a stray high byte cannot produce invalid UTF-8 in the output file. */
static void jstr(FILE* f, const char* src, size_t cap)
{
    size_t i;

    fputc('"', f);

    for (i = 0; i < cap && src[i]; i++)
    {
        unsigned char c = (unsigned char)src[i];

        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c >= 0x20 && c < 0x7F)
            fputc((char)c, f);
        else
            fprintf(f, "\\u%04X", c);
    }

    fputc('"', f);
}

static void jfield_str(FILE* f, const char* key, const char* src, size_t cap)
{
    fprintf(f, ",\"%s\":", key);
    jstr(f, src, cap);
}

/* The pitch of one listbox row, or 0 if it cannot be established.

   A row is not `height / count`: the engine lays rows out at a fixed pitch from
   the top, so selecting item k means clicking `rect.y + 2 + k*pitch`. The pitch
   is the gadget's own itemheight when it sets one — and across the whole stock
   .GUI corpus exactly one gadget does — otherwise the height of the glyph 'I'
   in the GUI font plus 3. That is what GUI_ListboxBuild computes at 0x4A1C0D,
   mirrored here rather than called: every step is a plain read. */
static int row_pitch(const char* g)
{
    char* gfx;
    char* fontset;
    char* font;
    char* glyph;
    int   ih = *(short*)(g + L_ITEMH);

    if (ih > 0)
        return ih;

    gfx = *(char**)TA_GFXP;

    if (!readable(gfx, GFX_FONTSET + 4))
        return 0;

    fontset = *(char**)(gfx + GFX_FONTSET);

    if (!readable(fontset, FS_FONT + 4))
        return 0;

    font = *(char**)(fontset + FS_FONT);

    if (!readable(font, FONT_GLYPHS + ('I' + 1) * 8))
        return 0;

    if ((unsigned short)*(short*)(font + FONT_COUNT) <= 'I')
        return 0;

    glyph = *(char**)(font + 'I' * 8 + FONT_GLYPHS);

    if (!readable(glyph, GLYPH_H + 2))
        return 0;

    return *(short*)(glyph + GLYPH_H) + 3;
}

/* Listbox items.

   TA keeps them as one flat char blob at +0xC2 whose entries are separated by
   '\0' or '\n' — that is exactly the rule the engine's own item locator
   (0x4B6AF0) walks — with the entry count in the i16 at +0xC0. The draw path
   only reads that blob when attribs bit 0x10 is set (0x4A1C5C); bits 0x20/0x80
   select the picture flavour instead, whose entries at +0xC6 are GAF frame
   headers holding no text at all. So "has items" is a property of attribs, not
   of the pointer, and the two cases must not be conflated: a picture list is
   reported as unknown, never as an empty text list.

   A leading "&G" marks a NON-SELECTABLE separator row. The engine strips the
   two characters before drawing (0x4A1E17) and refuses to leave the selection
   on such a row (0x4A39EA, 0x4A3BA3, 0x4A996A). Both halves matter to a caller,
   so the text is reported stripped — what is on screen — and the marker is
   reported separately. */
static const char* item_at(const char* blob, size_t avail, int k,
                           size_t* len, int* marked)
{
    size_t at = 0;
    int i;

    for (i = 0; ; i++)
    {
        const char* item = blob + at;
        size_t raw = 0;

        if (at >= avail)
            return NULL;

        while (at + raw < avail && item[raw] && item[raw] != '\n')
            raw++;

        if (i == k)
        {
            *marked = (raw >= 2 && item[0] == '&' && item[1] == 'G');
            *len = *marked ? raw - 2 : raw;

            if (*len > MAX_ITEMLEN)
                *len = MAX_ITEMLEN;

            return *marked ? item + 2 : item;
        }

        at += raw + 1;
    }
}

static int item_count(const char* g, const char** blob, size_t* avail)
{
    int count = *(short*)(g + L_COUNT);
    int attr  = *(int*)(g + G_ATTRIBS);

    if (!(attr & L_ATTR_TEXT) || count < 0)
        return -1;                       /* unknown: not a text list at all */

    if (count > MAX_ITEMS)
        count = MAX_ITEMS;

    *blob  = *(const char**)(g + L_TEXT);
    *avail = count ? readable_span(*blob, MAX_BLOB) : 0;

    return (count && !*avail) ? -1 : count;
}

static void write_items(FILE* out, const char* g)
{
    const char* blob;
    size_t avail;
    int count = item_count(g, &blob, &avail);
    int k;

    if (count < 0)
    {
        fprintf(out, ",\"items\":null,\"separator\":null");
        return;
    }

    fprintf(out, ",\"items\":[");

    for (k = 0; k < count; k++)
    {
        size_t len = 0;
        int marked = 0;
        const char* item = item_at(blob, avail, k, &len, &marked);

        if (!item)
            break;

        if (k)
            fputc(',', out);

        jstr(out, item, len);
    }

    fprintf(out, "],\"separator\":[");

    for (k = 0; k < count; k++)
    {
        size_t len = 0;
        int marked = 0;

        if (!item_at(blob, avail, k, &len, &marked))
            break;

        fprintf(out, "%s%d", k ? "," : "", marked);
    }

    fputc(']', out);
}

/* Per-item enable flags.

   GUIGADGET_SetListText takes a fifth argument the first reading missed — a u8
   array, one byte per item, stored at +0xD6 with attribs bit 0x800 set
   (0x4A33A8). The listbox draw path tests bit 0 of each byte to grey a row out
   (0x4A21F4). Only RESTRICT2.GUI uses it, and there it is what says which units
   are available, so a row's flag is the difference between a selection landing
   and quietly doing nothing. */
static void write_itemflags(FILE* out, const char* g)
{
    const unsigned char* flags = *(const unsigned char**)(g + L_FLAGS);
    int count = *(short*)(g + L_COUNT);
    int attr  = *(int*)(g + G_ATTRIBS);
    int k;

    if (!(attr & L_ATTR_FLAG) || count <= 0 || !readable(flags, (size_t)count))
    {
        fprintf(out, ",\"itemflags\":null");
        return;
    }

    if (count > MAX_ITEMS)
        count = MAX_ITEMS;

    fprintf(out, ",\"itemflags\":[");

    for (k = 0; k < count; k++)
        fprintf(out, "%s%d", k ? "," : "", flags[k] & 1);

    fputc(']', out);
}

static void write_snapshot(const TAGPU_FRAME* f)
{
    char*  main_p;
    char*  gi;
    char*  top;
    char*  ctrls;
    char*  level;
    FILE*  out;
    int    total, i, depth, px, py;

    out = fopen(UI_TMP, "wb");

    if (!out)
    {
        ulog("ui: cannot open tagpu_ui.json.tmp");
        return;
    }

    main_p = *(char**)TA_MAINPP;

    if (!readable(main_p, OFF_GUIINFO + 0x100))
    {
        fprintf(out, "{\"ok\":false,\"error\":\"no game struct\"}\n");
        goto done;
    }

    gi  = main_p + OFF_GUIINFO;
    top = *(char**)(gi + GI_ACTIVE);

    if (!readable(top, 0x20))
    {
        fprintf(out, "{\"ok\":false,\"error\":\"no active gui\"}\n");
        goto done;
    }

    ctrls = *(char**)(top + GM_CTRLS);

    if (!readable(ctrls, STRIDE))
    {
        fprintf(out, "{\"ok\":false,\"error\":\"no controls array\"}\n");
        goto done;
    }

    total = *(short*)(ctrls + P_TOTAL);

    if (total < 0)
        total = 0;

    if (total > MAX_GADGETS)
    {
        char b[96];
        _snprintf(b, sizeof b, "ui: totalgadgets=%d clamped to %d", total, MAX_GADGETS);
        b[sizeof b - 1] = 0;
        ulog(b);
        total = MAX_GADGETS;
    }

    fprintf(out, "{\"ok\":true,\"frame\":%u,\"surface\":[%d,%d]",
            f ? f->frame_counter : 0u,
            f ? f->game_width : 0,
            f ? f->game_height : 0);

    /* The letterboxed viewport in WINDOW pixels (G17b). `surface` above is the
       engine's logical screen and every gadget rect below is in it; this is
       where that screen is actually drawn, so the two together are what a
       harness needs to aim a DEVICE-space click at a gadget — which is the
       only kind of click that exercises the pointer unscale at all. */
    fprintf(out, ",\"viewport\":[%d,%d,%d,%d]",
            f ? f->vp_x : 0, f ? f->vp_y : 0,
            f ? f->vp_w : 0, f ? f->vp_h : 0);

    fprintf(out, ",\"gui\":");
    jstr(out, ctrls + G_NAME, 16);

    jfield_str(out, "enter", ctrls + P_CRDEF,  16);
    jfield_str(out, "esc",   ctrls + P_ESCDEF, 16);
    jfield_str(out, "focus", ctrls + P_FOCUS,  16);

    fprintf(out, ",\"uichange\":%d", *(int*)(gi + GI_UICHANGE));

    /* The panel record's own rect. Shell menus are full-screen panels at 0,0 so
       their gadget coords read as absolute; the in-game panel is not, and this is
       the candidate origin its gadgets are measured from. */
    fprintf(out, ",\"panel\":[%d,%d,%d,%d]",
            *(short*)(ctrls + G_XPOS), *(short*)(ctrls + G_YPOS),
            *(short*)(ctrls + G_WIDTH), *(short*)(ctrls + G_HEIGHT));

    /* breadcrumb: the screens this one covers, top-down. Only the top GUI is
       interactive, so they are named but their gadgets are deliberately not
       listed — see gui-gadgets.md §1.3. */
    fprintf(out, ",\"under\":[");
    level = *(char**)(top + GM_PREV);

    for (depth = 0; depth < MAX_STACK && readable(level, 0x20); depth++)
    {
        char* lc = *(char**)(level + GM_CTRLS);

        if (!readable(lc, STRIDE))
            break;

        if (depth)
            fputc(',', out);

        jstr(out, lc + G_NAME, 16);
        level = *(char**)(level + GM_PREV);
    }

    fprintf(out, "],\"gadgets\":[");

    /* Gadget coordinates are relative to the panel record's own origin. Shell
       menus are full-screen panels at (0,0) so the distinction is invisible
       there, but the in-game build panel sits at (0,128) — below the minimap —
       and its gadgets are measured from that corner. Reported rects and click
       points are therefore screen-space; "panel" above keeps the raw origin so
       nothing is lost. Measured live 2026-09-01: ARMSOLAR raw [0,27,64,64] +
       (0,128) lands on the solar icon in a `tacli shot`. */
    px = *(short*)(ctrls + G_XPOS);
    py = *(short*)(ctrls + G_YPOS);

    for (i = 1; i <= total; i++)
    {
        char* g = ctrls + (size_t)i * STRIDE;
        int   id, x, y, w, h;

        if (!readable(g, STRIDE))
            break;

        id = *(unsigned char*)(g + G_ID);
        x  = px + *(short*)(g + G_XPOS);
        y  = py + *(short*)(g + G_YPOS);
        w  = *(short*)(g + G_WIDTH);
        h  = *(short*)(g + G_HEIGHT);

        if (i > 1)
            fputc(',', out);

        fprintf(out, "{\"i\":%d,\"id\":%d,\"type\":\"%s\"", i, id, type_name(id));
        fprintf(out, ",\"name\":");
        jstr(out, g + G_NAME, 16);
        fprintf(out, ",\"assoc\":%d", *(unsigned char*)(g + G_ASSOC));
        fprintf(out, ",\"rect\":[%d,%d,%d,%d]", x, y, w, h);
        fprintf(out, ",\"click\":[%d,%d]", x + w / 2, y + h / 2);
        fprintf(out, ",\"active\":%d", *(unsigned char*)(g + G_ACTIVE));
        fprintf(out, ",\"attribs\":%d", *(int*)(g + G_ATTRIBS));
        /* Always empty in practice: GUI_ParseCommonFields reads the TDF's help
           text into +0x33 and then memsets the field before re-copying it
           through the (usually absent) translation table — 0x4AD49B. The offset
           is right, the engine wipes the value. gui-gadgets.md §7. */
        jfield_str(out, "help", g + G_HELP, 128);

        /* Past +0x33 the type structs are a union over the same bytes — +0xB6 is
           totalgadgets in a panel but text[128] in a button — so nothing below
           may be read without switching on id first. */
        if (id == 1 || id == 12)
        {
            jfield_str(out, "text", g + B_TEXT, 128);
            fprintf(out, ",\"stages\":%d",   *(unsigned char*)(g + B_STAGES));
            fprintf(out, ",\"stage\":%d",    *(unsigned char*)(g + B_STATUS));
            fprintf(out, ",\"quickkey\":%d", *(unsigned char*)(g + B_QUICKKEY));
            fprintf(out, ",\"grayed\":%d",   *(int*)(g + B_GRAYED) ? 1 : 0);
        }
        else if (id == 2)
        {
            fprintf(out, ",\"selected\":%d",   *(short*)(g + L_SELECTED));
            fprintf(out, ",\"top\":%d",        *(short*)(g + L_TOP));
            fprintf(out, ",\"maxtop\":%d",     *(short*)(g + L_MAXTOP));
            fprintf(out, ",\"count\":%d",      *(short*)(g + L_COUNT));
            fprintf(out, ",\"itemheight\":%d", *(short*)(g + L_ITEMH));
            fprintf(out, ",\"rowpitch\":%d", row_pitch(g));
            write_items(out, g);
            write_itemflags(out, g);
        }
        else if (id == 3)
        {
            jfield_str(out, "text", g + B_TEXT, 128);
            fprintf(out, ",\"maxchars\":%d", *(short*)(g + F_MAXCHARS));
        }
        else if (id == 5)
        {
            jfield_str(out, "text", g + B_TEXT, 128);
        }
        else if (id == 4)
        {
            /* Two different numbers, and conflating them is the whole trap.
               `pos` (knobpos) is a raw pixel offset of the knob along the track,
               bounded by range-1. `value` is what the engine actually acts on —
               GUI_SliderGetValue 0x45BA20 computes pos*thick/(range-1),
               truncating — and it is the number a caller means by "set the
               volume to 32". range/thick can both be rewritten at load time by
               the screen that owns the slider, so neither may be taken from the
               .GUI file. */
            int  rng   = *(short*)(g + S_RANGE);
            int  pos   = *(short*)(g + S_KNOBPOS);
            int  thick = *(int*)(g + S_THICK);
            int  attr  = *(int*)(g + G_ATTRIBS);

            fprintf(out, ",\"pos\":%d",      pos);
            fprintf(out, ",\"range\":%d",    rng);
            fprintf(out, ",\"knobsize\":%d", *(short*)(g + S_KNOBSIZE));
            fprintf(out, ",\"thick\":%d",    thick);
            fprintf(out, ",\"horizontal\":%d", (attr & S_ATTR_HORZ) ? 1 : 0);
            fprintf(out, ",\"nomouse\":%d",
                    ((attr & S_ATTR_DEAF) || *(unsigned char*)(g + S_ALTPROC)) ? 1 : 0);

            if (rng > 1)
                fprintf(out, ",\"value\":%d", (int)(((long long)pos * thick) / (rng - 1)));
            else
                fprintf(out, ",\"value\":null");
        }

        fputc('}', out);
    }

    fprintf(out, "]}\n");

done:
    fclose(out);

    if (!MoveFileExA(UI_TMP, UI_OUT, MOVEFILE_REPLACE_EXISTING))
        ulog("ui: could not replace tagpu_ui.json");
}

void tagpu_ui_frame(const TAGPU_FRAME* f)
{
    if (f && (f->frame_counter % 5))
        return;

    if (GetFileAttributesA(UI_TRIGGER) == INVALID_FILE_ATTRIBUTES)
        return;

    DeleteFileA(UI_TRIGGER);
    write_snapshot(f);
}
