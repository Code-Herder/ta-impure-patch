/* tagpu_ui.c — on-demand snapshot of the live GUI gadget tree.
   Contract: inc/tagpu_ui.h. Offsets and evidence: research/notes/gui-gadgets.md. */

#include <windows.h>
#include <stdio.h>
#include "tagpu_ui.h"

#define UI_TRIGGER  "tagpu_ui.trigger"
#define UI_OUT      "tagpu_ui.json"
#define UI_TMP      "tagpu_ui.json.tmp"

#define TA_MAINPP   0x00511DE8u   /* TAdynmemStruct** */

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
#define L_SELECTED  0xBA          /* i16 */
#define L_ITEMH     0xDA          /* i16 */

#define MAX_GADGETS 512
#define MAX_STACK   16

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
            /* items stay null until 0x4B6AF0's node layout is identified —
               "not implemented", never "empty". gui-gadgets.md §7. */
            fprintf(out, ",\"selected\":%d",   *(short*)(g + L_SELECTED));
            fprintf(out, ",\"itemheight\":%d", *(short*)(g + L_ITEMH));
            fprintf(out, ",\"items\":null");
        }
        else if (id == 3 || id == 5)
        {
            jfield_str(out, "text", g + B_TEXT, 128);
        }
        else if (id == 4)
        {
            fprintf(out, ",\"value\":null");   /* position field unidentified */
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
