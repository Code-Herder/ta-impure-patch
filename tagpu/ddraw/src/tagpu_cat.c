/* tagpu_cat.c — on-demand dump of the live unit and feature definition tables.
   Contract: inc/tagpu_cat.h. Why it exists: research/notes/scenario-format.md. */

#include <windows.h>
#include <stdio.h>
#include "tagpu_cat.h"
#include "tagpu_log.h"
#include "tagpu_packet_pub.h"

#define UNITS_TRIGGER "tagpu_units.trigger"
#define UNITS_OUT     "tagpu_units.json"
#define UNITS_TMP     "tagpu_units.json.tmp"
#define FEATS_TRIGGER "tagpu_features.trigger"
#define FEATS_OUT     "tagpu_features.json"
#define FEATS_TMP     "tagpu_features.json.tmp"

#define TA_MAINPP    0x00511DE8u  /* TAdynmemStruct** */

#define OFF_UCOUNT   0x1438F      /* unsigned UNITINFOCount        */
#define OFF_UDEFS    0x1439B      /* UnitDefStruct*                */
#define UD_STRIDE    0x249
#define UD_DISPLAY   0x00         /* char Name[0x20]        "Peewee"        */
#define UD_NAME      0x20         /* char UnitName[0x20]    "ARMPW"         */
#define UD_DESC      0x40         /* char UnitDescription[0x40]            */
#define UD_SIDE      0xA0         /* char Side[8]           "ARM"          */
#define UD_FOOTX     0x14A        /* short, in map squares                 */
#define UD_FOOTY     0x14C
#define UD_BITS2     0x245        /* u32 of FBI boolean bits                  */
#define UD_COMMANDER 0x40000u     /* `commander`: 0x42CB7D reads it, shl 0x12, 0x42CBAB stores
                                     it (DISASSEMBLED 2026-09-28)                  */

#define OFF_FCOUNT   0x14253      /* int NumFeatureDefs            */
#define OFF_FDEFS    0x1426F      /* FeatureDefStruct*             */
#define FD_STRIDE    0x100
#define FD_NAME      0x00         /* char Name[0x20]                       */
#define FD_DESC      0x80         /* char Description[20]                  */
#define FD_FOOTX     0x94         /* short, in map squares                 */
#define FD_FOOTZ     0x96
#define FD_ENERGY    0xEC         /* float                                 */
#define FD_METAL     0xF0         /* float                                 */
#define FD_MASK      0xFE         /* u16 FeatureMask                       */

/* The cross-check that proves the unit table's base and stride (see the header). */
#define OFF_BEGIN    0x14357      /* UnitStruct* BeginUnitsArray_p */
#define OFF_END      0x1435B      /* UnitStruct* EndOfUnitsArray_p */
#define UNIT_STRIDE  0x118
#define U_TYPE       0x92         /* UnitDefStruct*                */
#define U_TYPEIDX    0xA6         /* short, index into UnitDef[]   */
#define U_STATE      0x110        /* alive bit 0x10000000          */
#define U_OWNER      0xFF         /* u8 player index               */
#define OFF_USLOTS   0x14351      /* u16 the array's slot count, stored at 0x4854EF */
/* the ten seats, inline in main: a seat is in the game when its first dword is nonzero and
   its controller byte +0x73 is 1 (local human), 2 (local AI), 3 (remote human) or 4 (remote
   AI). DISASSEMBLED: 0x48664B tests the dword and then 1 and 2 for a local player, 0x489C99
   compares 3 and 0x428446 compares 4 (tadr-port/sim-fixes-evidence.md) */
#define OFF_PLAYERS  0x1B63
#define PLAYER_STRIDE 0x14B
#define PL_TYPE      0x73

/* Stock TA declares 512 unit types and a few hundred features; TADR-class mods
   raise the ceiling to 16000. Emit generously but never unboundedly — a garbage
   count must cost a truncated file, not a walk off the end of the world. */
#define MAX_DEFS     16384

static void catlog(const char* s)
{
    tagpu_log(s);
}

/* Committed, not guard/no-access. A wrong pointer must cost a log line, never
   the process — same rule as tagpu_peek and tagpu_ui. */
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

/* TA strings are fixed-size, NUL-padded and CP437-ish. Emit strict JSON: escape
   the two mandatory characters and \u-escape anything outside printable ASCII,
   so a stray high byte cannot produce invalid UTF-8 in the output file. */
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

/* A definition name has to look like one before it is worth emitting: the table
   is allocated at its full ceiling and only the loaded entries are filled in, so
   the tail is zeros or leftovers. Printable ASCII, non-empty. */
static BOOL named(const char* s, size_t cap)
{
    size_t i;

    if (!s || !s[0])
        return FALSE;

    for (i = 0; i < cap && s[i]; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] >= 0x7F)
            return FALSE;

    return TRUE;
}

static FILE* begin(const char* tmp, const char* what)
{
    FILE* f = fopen(tmp, "wb");

    if (!f)
    {
        catlog("cat: cannot open the temp file");
        return 0;
    }

    fprintf(f, "{\"ok\":1,\"kind\":\"%s\"", what);
    return f;
}

static void finish(FILE* f, const char* tmp, const char* out)
{
    fprintf(f, "}\n");
    fclose(f);

    if (!MoveFileExA(tmp, out, MOVEFILE_REPLACE_EXISTING))
        catlog("cat: could not replace the catalogue file");
}

/* Prove the unit table before trusting it: the first living unit's
   UnitDefStruct* (unit+0x92) must be exactly defs + UnitID*0x249. If it is, the
   base and the stride are both right; if there is no unit yet (the menu), say
   so rather than claiming a proof nobody ran. */
static const char* verify_stride(const char* ta, const char* defs)
{
    const char* begin_u;
    unsigned slots, i;

    if (!tagpu_packet_pub_level_open())
        return "no units to check against";
    begin_u = *(const char* const*)(ta + OFF_BEGIN);
    slots   = *(const unsigned short*)(ta + OFF_USLOTS);
    if (!readable(begin_u, UNIT_STRIDE))
        return "no units to check against";

    for (i = 0; i < slots; i++)
    {
        const char* u = begin_u + (size_t)i * UNIT_STRIDE;
        const char* type;
        int idx;

        if (!readable(u, UNIT_STRIDE) || !(*(const unsigned*)(u + U_STATE) & 0x10000000u))
            continue;

        type = *(const char* const*)(u + U_TYPE);
        idx  = *(const short*)(u + U_TYPEIDX);

        if (idx < 0 || !readable(type, UD_STRIDE))
            continue;

        return (type == defs + (size_t)idx * UD_STRIDE) ? "ok" : "MISMATCH";
    }

    return "no units to check against";
}

/* Every living commander, as the engine holds it -- not as Impure draws it: the frame packet
   leaves out what the fog hides, and the other player's commander starts under it -- and every
   seat in the game, so the compat suite can ask "did every player get a commander".
   THE ORDERING the walk stands on: it runs only while a level is in play
   (tagpu_packet_pub_level_open, set on the game thread at the level's first in-play draw, after
   the loader thread's last store, and cleared by the teardown's post hook). Outside that window
   the loader thread can be building the array, and `begin` and `end` are then an unsynchronised
   pair (cross-thread-engine-reads.md §5). The teardown frees the array and nulls `begin` on the
   game thread, which is this one (tagpu_triggers_frame), and its post hook clears the latch on
   the same call stack, so no walk runs between the free and the clear. The one gap: a
   teardown entered on another thread would leave the latch set into the next load, because
   the level end declines to act off the game thread; none has been observed, and every run
   counts them (`tdforeign=`, tagpu_packet_pub.c).
   THE BOUNDS, each on a value read from engine memory: the walk runs to the array's own slot
   count (u16 +0x14351, what 0x4854A0 allocated for), never to `end`; a unit's type is its index
   (+0xA6) checked against the def count the table walk above used, never its +0x92 pointer;
   the owner (+0xFF) is used only below 10. */
static void write_commanders(FILE* out, const char* ta, const char* defs, unsigned count)
{
    const char* begin_u;
    unsigned slots, i, found = 0, owners = 0, k;
    const int in_play = tagpu_packet_pub_level_open();

    fprintf(out, ",\"in_play\":%s,\"commanders\":[", in_play ? "true" : "false");
    begin_u = *(const char* const*)(ta + OFF_BEGIN);
    slots   = *(const unsigned short*)(ta + OFF_USLOTS);
    if (in_play && defs && readable(begin_u, UNIT_STRIDE))
    {
        for (i = 0; i < slots; i++)
        {
            const char* u = begin_u + (size_t)i * UNIT_STRIDE;
            const char* rec;
            int idx;
            unsigned owner;

            if (!readable(u, UNIT_STRIDE) || !(*(const unsigned*)(u + U_STATE) & 0x10000000u))
                continue;
            owner = *(const unsigned char*)(u + U_OWNER);
            if (owner >= 10)
                continue;
            owners |= 1u << owner;
            idx = *(const short*)(u + U_TYPEIDX);
            if (idx < 0 || (unsigned)idx >= count)
                continue;
            rec = defs + (size_t)idx * UD_STRIDE;
            if (!readable(rec, UD_STRIDE) || !(*(const unsigned*)(rec + UD_BITS2) & UD_COMMANDER))
                continue;
            fprintf(out, "%s{\"owner\":%u", found ? "," : "", owner);
            jfield_str(out, "type", rec + UD_NAME, 0x20);
            fprintf(out, "}");
            found++;
        }
    }
    /* every owner with a living unit of any type */
    fprintf(out, "],\"unit_owners\":[");
    for (k = 0, found = 0; k < 10; k++)
        if (owners & (1u << k))
            fprintf(out, "%s%u", found++ ? "," : "", k);
    /* every seat in the game, units or none: a player the game gave nothing is here and not
       above. The records are inline in main, a fixed array. */
    fprintf(out, "],\"players\":[");
    for (k = 0, found = 0; in_play && k < 10; k++)
    {
        const char* pl = ta + OFF_PLAYERS + (size_t)k * PLAYER_STRIDE;
        const unsigned char type = *(const unsigned char*)(pl + PL_TYPE);
        if (*(const unsigned*)pl && type >= 1 && type <= 4)
            fprintf(out, "%s%u", found++ ? "," : "", k);
    }
    fprintf(out, "]");
}

static void write_units(const TAGPU_FRAME* f)
{
    char* ta = *(char**)TA_MAINPP;
    unsigned count = 0, emitted = 0, i;
    char* defs = 0;
    FILE* out = begin(UNITS_TMP, "units");

    if (!out)
        return;

    fprintf(out, ",\"frame\":%u", f ? f->frame_counter : 0);

    if (!readable(ta, OFF_UDEFS + 4))
    {
        fprintf(out, ",\"count\":0,\"units\":[],\"note\":\"no game structure yet\"");
        finish(out, UNITS_TMP, UNITS_OUT);
        return;
    }

    count = *(unsigned*)(ta + OFF_UCOUNT);
    defs  = *(char**)(ta + OFF_UDEFS);

    if (count > MAX_DEFS)
    {
        fprintf(out, ",\"truncated\":%u", count);
        count = MAX_DEFS;
    }

    fprintf(out, ",\"declared\":%u", count);
    fprintf(out, ",\"stride_check\":\"%s\"", verify_stride(ta, defs));
    fprintf(out, ",\"units\":[");

    for (i = 0; i < count; i++)
    {
        const char* rec = defs + (size_t)i * UD_STRIDE;

        if (!readable(rec, UD_STRIDE) || !named(rec + UD_NAME, 0x20))
            continue;   /* the table is allocated at its ceiling; skip the gaps */

        fprintf(out, "%s{\"id\":%u", emitted ? "," : "", i);
        jfield_str(out, "name", rec + UD_NAME, 0x20);
        jfield_str(out, "display", rec + UD_DISPLAY, 0x20);
        jfield_str(out, "desc", rec + UD_DESC, 0x40);
        jfield_str(out, "side", rec + UD_SIDE, 8);
        fprintf(out, ",\"footprint\":[%d,%d]}",
                *(short*)(rec + UD_FOOTX), *(short*)(rec + UD_FOOTY));
        emitted++;
    }

    fprintf(out, "],\"count\":%u", emitted);
    write_commanders(out, ta, defs, count);
    finish(out, UNITS_TMP, UNITS_OUT);
}

static void write_features(const TAGPU_FRAME* f)
{
    char* ta = *(char**)TA_MAINPP;
    int count = 0;
    unsigned emitted = 0;
    int i;
    char* defs = 0;
    FILE* out = begin(FEATS_TMP, "features");

    if (!out)
        return;

    fprintf(out, ",\"frame\":%u", f ? f->frame_counter : 0);

    if (!readable(ta, OFF_FDEFS + 4))
    {
        fprintf(out, ",\"count\":0,\"features\":[],\"note\":\"no game structure yet\"");
        finish(out, FEATS_TMP, FEATS_OUT);
        return;
    }

    count = *(int*)(ta + OFF_FCOUNT);
    defs  = *(char**)(ta + OFF_FDEFS);

    if (count < 0)
        count = 0;

    if (count > MAX_DEFS)
    {
        fprintf(out, ",\"truncated\":%d", count);
        count = MAX_DEFS;
    }

    fprintf(out, ",\"declared\":%d,\"features\":[", count);

    for (i = 0; i < count; i++)
    {
        const char* rec = defs + (size_t)i * FD_STRIDE;

        if (!readable(rec, FD_STRIDE) || !named(rec + FD_NAME, 0x20))
            continue;

        fprintf(out, "%s{\"id\":%d", emitted ? "," : "", i);
        jfield_str(out, "name", rec + FD_NAME, 0x20);
        jfield_str(out, "desc", rec + FD_DESC, 20);
        fprintf(out, ",\"footprint\":[%d,%d]",
                *(short*)(rec + FD_FOOTX), *(short*)(rec + FD_FOOTZ));
        fprintf(out, ",\"metal\":%g,\"energy\":%g",
                *(float*)(rec + FD_METAL), *(float*)(rec + FD_ENERGY));
        fprintf(out, ",\"mask\":%u}", *(unsigned short*)(rec + FD_MASK));
        emitted++;
    }

    fprintf(out, "],\"count\":%u", emitted);
    finish(out, FEATS_TMP, FEATS_OUT);
}

void tagpu_cat_frame(const TAGPU_FRAME* f)
{
    /* Same cadence as the GUI snapshot: the CLI polls the result in an auto-wait
       loop, so a five-frame worst case is what it feels like. */
    if (f && (f->frame_counter % 5))
        return;

    if (GetFileAttributesA(UNITS_TRIGGER) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileA(UNITS_TRIGGER);
        write_units(f);
    }

    if (GetFileAttributesA(FEATS_TRIGGER) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileA(FEATS_TRIGGER);
        write_features(f);
    }
}
