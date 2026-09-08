/* tagpu_scenario.c — the applier: a compiled wire file becomes a live situation.
   Contract: inc/tagpu_scenario.h. Why it exists: research/notes/scenario-format.md.

   Two halves, on two threads, deliberately:

     present path (tagpu_overlay_draw)   trigger -> parse -> arena;  arena -> result JSON
     Game_MainLoopTick detour @0x4969D2  arena  -> resolve -> create -> orders

   Nothing between them but a one-way state machine and an interlocked hand-off, so
   neither half can see the other's half-built work. */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_scenario.h"

#define SCN_TRIGGER   "tagpu_scenario.trigger"
#define SCN_OUT       "tagpu_scenario.json"
#define SCN_TMP       "tagpu_scenario.json.tmp"
#define SW_TRIGGER    "tagpu_switches.trigger"
#define SW_OUT        "tagpu_switches.json"
#define SW_TMP        "tagpu_switches.json.tmp"

/* ---------------------------------------------------------------- engine layout */

#define TA_MAINPP     0x00511DE8u  /* TAdynmemStruct**                            */

#define OFF_PLAYERS   0x1B63       /* PlayerStruct Players[10], stride 0x14B      */
#define PL_STRIDE     0x14B
#define PL_ACTIVE     0x00         /* int  PlayerActive                           */
#define PL_TYPE       0x73         /* char My_PlayerType (0 = empty slot)         */
#define PL_ENERGY     0x8C         /* float fCurrentEnergy  (PlayerRes + 0x00)    */
#define PL_METAL      0x98         /* float fCurrentMetal   (PlayerRes + 0x0C)    */
#define PL_MAXENERGY  0xA4         /* float fMaxEnergyStorage                     */
#define PL_MAXMETAL   0xA8         /* float fMaxMetalStorage                      */
#define PL_FIRSTUNIT  0x67         /* UnitStruct* first/last of this player's     */
#define PL_LASTUNIT   0x6B         /* units, INCLUSIVE, stepping UNIT_STRIDE      */

#define OFF_MAPPXW    0x14223      /* int MapWidth  (world units)                 */
#define OFF_MAPPXH    0x14227      /* int MapHeight                               */
#define OFF_FMAPX     0x14233      /* int FeatureMapSizeX (tiles, 16 units each)  */
#define OFF_FMAPY     0x14237
#define OFF_FMAP      0x14287      /* FeatureStruct* FeatureMap, stride 0x0D      */
#define FM_STRIDE     0x0D
#define FM_HEIGHT     0x04         /* unsigned char height                        */

#define OFF_EYEX      0x1431F      /* int EyeBallMapXPos                          */
#define OFF_EYEY      0x14323
#define OFF_SCRTX     0x14327      /* MapXScrollingTo — write or the engine eases */
#define OFF_SCRTY     0x1432B      /* the eye back                                */
#define OFF_VIEW_W    0x37E37
#define OFF_VIEW_H    0x37E3B

#define OFF_WATCHED   0x2A42       /* u8 watched player id — whose selection it is */

#define OFF_BEGIN     0x14357      /* UnitStruct* BeginUnitsArray_p               */
#define OFF_END       0x1435B      /* UnitStruct* EndOfUnitsArray_p               */
#define UNIT_STRIDE   0x118

#define OFF_UCOUNT    0x1438F      /* unsigned UNITINFOCount                      */
#define OFF_UDEFS     0x1439B      /* UnitDefStruct*, stride 0x249                */
#define UD_STRIDE     0x249
#define UD_NAME       0x20         /* char UnitName[0x20]  "ARMPW"                */

#define OFF_LIMIT     0x37EEA      /* u16 ActualUnitLimit                         */
#define OFF_PERPLAYER 0x37EEC      /* u16 MaxUnitNumberPerPlayer                  */
#define OFF_CMDRDEATH 0x37EF6      /* u32 ActiveCommanderDeath (0 = game goes on) */
#define OFF_SWITCHES  0x37F2F      /* u16 SoftwareDebugMode                       */
#define OFF_GAMETIME  0x38A47      /* int GameTime                                */

#define U_TURN        0x64         /* Volume_Word{bank, heading, pitch}           */
#define U_HEADING     0x66         /* full circle 0x10000, TA's build default is  */
#define U_XPOS        0x6C         /*   0x8000 and it increases CCW from above    */
#define U_ZPOS        0x70         /* altitude, whole world units                 */
#define U_YPOS        0x74         /* map depth, whole world units                */
#define U_ORDERS      0x5C         /* UnitOrdersStruct* UnitOrders                */
#define U_INGAMEIDX   0xA8         /* short UnitInGameIndex — recycled on death   */
#define U_UDEF        0x92         /* UnitDefStruct* — a live unit's own type     */
#define U_HEALTHPCTA  0xF6         /* char HealthPerA                             */
#define U_HEALTHPCTB  0xF7
#define U_OWNER       0xFF         /* unsigned char cOwnerID                      */
#define U_NANOFRAME   0x104        /* float, fraction of the build REMAINING      */
#define U_HEALTH      0x108        /* short                                       */
#define U_STATE       0x110        /* alive 0x10000000; 0x20 is written below for  */
                                   /* a nanoframe but the DRAW path never reads it */
                                   /* — under construction is +0x104 alone, and    */
                                   /* 0x20000000 is the STRUCTURE bit (G13l)       */
                                   /* stance (0xC0000)>>18: 0 hold 1 manv 2 roam  */
#define UO_POS        0x22         /* Position_Dword Pos in UnitOrdersStruct      */

/* Engine calls. Every address is from the merged community corpus
   (tools/ta_symbols.txt) and every signature from the vendored TADR corpus; the
   recipes below are TADR's own, which is the prior art that settles feasibility.

   UNITS_CreateUnit and ORDERS_NewMainOrder2Unit speak the SAME language: 16.16
   fixed-point positions in the 3-D convention (x, altitude, depth). There is no
   asymmetry — the earlier claim here (whole world units, {x, depth, altitude},
   read off TADR's ConstructionKickout) was wrong, and the read-back probe below
   could not catch it because the constructor 0x43A0C0 copies the caller's three
   dwords verbatim into UnitOrders->Pos (0x43A164..), so anything written reads
   back unchanged. What settles the scale is the duplicate-order test inside
   0x43AFC0: `sub ebp,[esi+0x22]; add ebp,0x100000; cmp ebp,0x200000` at
   0x43B006 -- a tolerance of +/-0x100000, which is +/-16.0 in 16.16, one map
   cell. It compares components 0 and 2 (0x22 and 0x2A) and never component 1,
   i.e. the ground plane and not the altitude. Measured 2026-09-04: with whole
   units every ordered unit walked to the map origin (1900 >> 16 == 0), aircraft
   included; with the shift they go where the file says. */
typedef void* (__stdcall* PFN_CREATE)(int owner, int typeIdx, int x, int alt, int depth,
                                      int fullHp, unsigned stateMask, int unitNumber);
typedef int   (__stdcall* PFN_KILL)(void* unit, unsigned mode);
typedef short (__stdcall* PFN_FEATNAME2ID)(const char* name);
typedef short (__stdcall* PFN_LOADFEATURE)(const char* name);
typedef void* (__stdcall* PFN_GRIDPLOT)(int gridX, int gridZ);
typedef void* (__stdcall* PFN_SPAWNFEAT)(void* gridPlot, short defIdx, const void* pos,
                                         const void* turn, unsigned char playerId);
typedef char* (__stdcall* PFN_TYPE2INDEX)(int* rtnIndex, unsigned actionId, void* unit,
                                          void* target, const void* pos);
typedef void  (__stdcall* PFN_ORDER)(int actionIndex, int shift, void* unit, void* target,
                                     const void* pos, int p1, int p2);

#define UNITS_CreateUnit ((PFN_CREATE)      0x00485F50u)
#define UNITS_KillUnit   ((PFN_KILL)        0x004864B0u)
#define FeatureName2ID   ((PFN_FEATNAME2ID) 0x00422DD0u)
#define LoadFeature      ((PFN_LOADFEATURE) 0x004224B0u)
#define GetGridPosPLOT   ((PFN_GRIDPLOT)    0x00481550u)
#define SpawnFeatureOnMap ((PFN_SPAWNFEAT)  0x00423C50u)
#define ScriptAction_Type2Index ((PFN_TYPE2INDEX) 0x0043F0E0u)
#define ORDERS_NewMainOrder2Unit ((PFN_ORDER) 0x0043AFC0u)

/* The tick site: `mov eax, ds:0x511DE8`, five position-independent bytes inside
   the straight-line block that 0x4969CB (TADR's GameTickHook address) enters. */
#define TICK_SITE     0x004969D2u
static const unsigned char TICK_STOLEN[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };

/* ------------------------------------------------------------------ the arena */

#define SCN_MAX_UNITS   4096
#define SCN_MAX_FEATS   1024
#define SCN_MAX_ORDERS  4096
#define SCN_MAX_ERRORS  24
#define SCN_MAX_CLEAR   4096
#define SCN_ERRLEN      168
#define SCN_NAMELEN     32
#define SCN_FILEMAX     (4u * 1024u * 1024u)
#define SCN_UNSET       (-2147483647 - 1)      /* the wire's `-` column          */

/* order target kinds */
#define TGT_NONE  0
#define TGT_POS   1
#define TGT_UNIT  2
#define TGT_FEAT  3

/* camera target kinds share TGT_*; 0 means "no camera line" */

typedef struct {
    char  type[SCN_NAMELEN];
    int   owner, x, y;
    int   height, facing, health, stance, nano;   /* SCN_UNSET = engine default  */
    int   defIdx;                                 /* resolved UNITINFO index     */
    void* unit;                                   /* the created UnitStruct*     */
    int   ax, ay, ah;                             /* where it actually landed    */
    int   engineIdx;
    int   ok;
} scn_unit;

typedef struct {
    char  type[SCN_NAMELEN];
    int   x, y, height, facing;
    int   defIdx;
    void* feat;
    int   ax, ay, ah;
    int   ok;
} scn_feat;

/* WHO the order is for. An ordinal names a unit this same wire file created and
   is the only identity `scenario-format.md` calls public, because
   UnitInGameIndex is recycled when a unit dies. The other two exist so a caller
   can order units that were already there — `tacli order` — and both carry
   their own guard against that recycling: SUBJ_LIVE checks the caller's claim
   about the slot's type before it orders, and SUBJ_SEL never names a slot at
   all, it asks the engine what the player currently has selected. */
#define SUBJ_ORD  0                               /* an ordinal in this wire     */
#define SUBJ_LIVE 1                               /* a live UnitInGameIndex      */
#define SUBJ_SEL  2                               /* whatever is selected now    */

typedef struct {
    int  subj;                                    /* SUBJ_*                      */
    int  uord;                                    /* ordinal, or engine index    */
    char sexp[SCN_NAMELEN];                       /* "" = no type guard          */
    int  cmd;                                     /* ORDERTYPE constant          */
    int  kind;                                    /* TGT_*                       */
    int  a, b;                                    /* pos x,y  or  ordinal in a   */
    int  tlive;                                   /* target `a` is an engine idx */
    char texp[SCN_NAMELEN];
} scn_order;

static scn_unit  g_units[SCN_MAX_UNITS];
static scn_feat  g_feats[SCN_MAX_FEATS];
static scn_order g_orders[SCN_MAX_ORDERS];
static char      g_errs[SCN_MAX_ERRORS][SCN_ERRLEN];
static char*     g_clearlist[SCN_MAX_CLEAR];
static int       g_nclear;

static int g_nunits, g_nfeats, g_norders, g_nerrs, g_errdrop;

/* setup carried by the wire */
static int      g_seed, g_clear, g_abort, g_limit;
static char     g_map[128];
static unsigned g_sw_set, g_sw_clr;               /* SoftwareDebugMode bit masks */
static int      g_pl_slot[10], g_pl_metal[10], g_pl_energy[10], g_npl;
/* what the fields read back the instant after the write, still on the same tick */
static int      g_pl_got_metal[10], g_pl_got_energy[10];
static int      g_cam_kind, g_cam_a, g_cam_b, g_cam_pin;

/* results filled by the apply pass */
static int      g_applied, g_failed, g_cleared, g_ord_ok, g_ord_fail;
static unsigned g_sw_before, g_sw_after;
static int      g_gametime, g_cap, g_limit_live, g_perplayer_live;
static int      g_cam_x, g_cam_y, g_cam_h, g_cam_have, g_eye_x, g_eye_y, g_eye_have;
static int      g_probe_have, g_probe_pass[3], g_probe_stored[3];

/* live map extents, reloaded every apply */
static int   map_w, map_h, fmap_x, fmap_y;
static char* fmap;

/* IDLE -> ARMED -> APPLYING -> DONE -> IDLE. The present path owns every
   transition but ARMED->APPLYING and APPLYING->DONE, which the tick owns. */
#define ST_IDLE     0
#define ST_ARMED    1
#define ST_APPLYING 2
#define ST_DONE     3
static volatile LONG g_state = ST_IDLE;

static int g_hooked;                              /* the detour is installed once */
static unsigned g_frame;                          /* stamped by the present path  */
static unsigned g_armed_at;                       /* frame the arena went ARMED   */

/* How long the present path waits for Game_MainLoopTick to reach the detour.
   It never does at the menus, on the mission-end screen, or while the game is
   paused — and a silent CLI timeout tells nobody which of those it was. */
#define SCN_ARM_FRAMES 600u

/* ------------------------------------------------------------------ utilities */

static void scnlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static void scnlogf(const char* fmt, ...)
{
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b - 1, fmt, ap);
    b[sizeof b - 1] = 0;
    va_end(ap);
    scnlog(b);
}

/* Committed, not guard/no-access. A wrong pointer must cost a log line, never
   the process — same rule as tagpu_peek, tagpu_ui and tagpu_cat. */
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
   the two mandatory characters and \u-escape anything outside printable ASCII. */
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

/* Errors are the product here: a scenario that creates nothing must say which
   line was wrong. Keep the first few verbatim and count the rest. */
static void scn_err(const char* fmt, ...)
{
    va_list ap;

    if (g_nerrs >= SCN_MAX_ERRORS)
    {
        g_errdrop++;
        return;
    }

    va_start(ap, fmt);
    _vsnprintf(g_errs[g_nerrs], SCN_ERRLEN - 1, fmt, ap);
    va_end(ap);
    g_errs[g_nerrs][SCN_ERRLEN - 1] = 0;
    g_nerrs++;
}

static char* ta_base(void)
{
    char* ta = *(char**)TA_MAINPP;
    return readable(ta, OFF_GAMETIME + 4) ? ta : 0;
}

/* ------------------------------------------------------------- the wire parser
   Fixed vocabulary, positional columns, `-` for unset. No user hand-writes this
   file and nothing promises it is stable; it changes whenever the DLL does. */

static char* tok(char** p)
{
    char* s = *p;
    char* b;

    while (*s == ' ' || *s == '\t')
        s++;

    if (!*s)
    {
        *p = s;
        return 0;
    }

    b = s;

    while (*s && *s != ' ' && *s != '\t')
        s++;

    if (*s)
        *s++ = 0;

    *p = s;
    return b;
}

/* One integer column. `-` means "unset — use the engine's own default". */
static int num(const char* t, int* out)
{
    char* end;
    long v;

    if (!t)
        return 0;

    if (t[0] == '-' && !t[1])
    {
        *out = SCN_UNSET;
        return 1;
    }

    v = strtol(t, &end, 10);

    if (end == t || *end)
        return 0;

    *out = (int)v;
    return 1;
}

static int req(const char* t, int* out)
{
    return num(t, out) && *out != SCN_UNSET;
}

/* `name=value` in the sw / player / cam lines. */
static int kv(char* t, char** key, int* value)
{
    char* eq;

    if (!t)
        return 0;

    eq = strchr(t, '=');

    if (!eq)
        return 0;

    *eq = 0;
    *key = t;
    return num(eq + 1, value);
}

static const struct { const char* name; unsigned bit; } SCN_SWITCHES[] = {
    { "drop", 0x1 }, { "cheats", 0x2 }, { "selboxes", 0x4 }, { "invulnfeatures", 0x8 },
    { "noshake", 0x10 }, { "clock", 0x40 }, { "doubleshot", 0x80 }, { "halfshot", 0x100 },
    { "radar", 0x200 }, { "shootall", 0x400 }, { 0, 0 }
};

/* TA's own order table [TA_MemoryStructures.pas:6-21]. There is no attack-move;
   the CLI rejects the word and offers `attack` at a coordinate instead. */
static const struct { const char* name; int id; } SCN_ORDERTYPES[] = {
    { "stop", 1 }, { "move", 2 }, { "attack", 3 }, { "blast", 4 }, { "unload", 5 },
    { "load", 6 }, { "defend", 7 }, { "repair", 8 }, { "patrol", 9 }, { "reclaim", 12 },
    { "capture", 13 }, { "mobilebuild", 14 }, { 0, 0 }
};

/* The stance column is a token, never a raw mask: (UnitStateMask & 0xC0000) >> 18
   is 0 hold / 1 manoeuvre / 2 roam [VERIFIED, TADR dialog.cpp:409-417]. */
static const char* SCN_STANCES[] = { "hold", "manoeuvre", "roam", 0 };

static int stance(const char* t, int* out)
{
    int i;

    if (!t)
        return 0;

    if (t[0] == '-' && !t[1])
    {
        *out = SCN_UNSET;
        return 1;
    }

    for (i = 0; SCN_STANCES[i]; i++)
        if (!strcmp(t, SCN_STANCES[i]))
        {
            *out = i;
            return 1;
        }

    return 0;
}

static void arena_reset(void)
{
    g_nunits = g_nfeats = g_norders = g_nerrs = g_errdrop = 0;
    g_seed = 0; g_clear = 1; g_abort = 1; g_limit = SCN_UNSET;
    g_map[0] = 0;
    g_sw_set = g_sw_clr = 0;
    g_npl = 0;
    g_cam_kind = TGT_NONE; g_cam_a = g_cam_b = 0; g_cam_pin = 0;
    g_applied = g_failed = g_cleared = g_ord_ok = g_ord_fail = 0;
    g_nclear = 0;
    g_sw_before = g_sw_after = 0;
    g_gametime = 0; g_cap = 0; g_limit_live = 0; g_perplayer_live = 0;
    g_cam_x = g_cam_y = g_cam_h = g_cam_have = 0;
    g_eye_x = g_eye_y = g_eye_have = 0;
    /* the map statics too: a parse failure reports before any apply reads them,
       and a result carrying the previous game's extents would be a lie */
    map_w = map_h = fmap_x = fmap_y = 0;
    fmap = 0;
    g_probe_have = 0;
}

static void copy_name(char* dst, const char* src)
{
    int i;

    for (i = 0; i < SCN_NAMELEN - 1 && src[i]; i++)
        dst[i] = src[i];

    dst[i] = 0;
}

/* Returns 1 when the whole file parsed and ended with `end`. A truncated file is
   visible rather than half-applied — that is what the marker is for. */
/* `@<index>` or `@<index>:<TYPE>` — a live unit and the caller's claim about
   what should be in that slot. Returns 0 on anything malformed; `expect` is
   left empty when no type was named. */
static int live_ref(const char* t, int* idx, char* expect)
{
    char  num[16];
    int   i = 0;
    const char* p;

    expect[0] = 0;

    if (!t || t[0] != '@')
        return 0;

    for (p = t + 1; *p && *p != ':'; p++)
    {
        if (*p < '0' || *p > '9' || i >= (int)sizeof num - 1)
            return 0;
        num[i++] = *p;
    }

    if (!i)
        return 0;

    num[i] = 0;

    if (!req(num, idx))
        return 0;

    if (*p == ':')
    {
        if (!p[1])
            return 0;
        copy_name(expect, p + 1);
    }

    return 1;
}

/* The first column of an `order` line: an ordinal, `sel`, or a live reference. */
static int order_subject(char* t, scn_order* o, int line)
{
    if (!t)
    {
        scn_err("line %d: order wants a subject (an ordinal, sel, or @index)", line);
        return 0;
    }

    if (!strcmp(t, "sel"))
    {
        o->subj = SUBJ_SEL;
        return 1;
    }

    if (t[0] == '@')
    {
        if (!live_ref(t, &o->uord, o->sexp))
        {
            scn_err("line %d: order subject `%.24s` is not @<index> or "
                    "@<index>:<TYPE>", line, t);
            return 0;
        }
        o->subj = SUBJ_LIVE;
        return 1;
    }

    if (!req(t, &o->uord))
    {
        scn_err("line %d: order wants a unit ordinal", line);
        return 0;
    }

    o->subj = SUBJ_ORD;
    return 1;
}

static int parse_wire(char* text)
{
    char* p = text;
    int   sawv = 0, sawend = 0;
    int   want_units = -1, want_feats = -1;
    int   line = 0;

    arena_reset();

    while (*p)
    {
        char* eol = strchr(p, '\n');
        char* cur = p;
        char* verb;

        if (eol)
        {
            *eol = 0;
            p = eol + 1;
        }
        else
        {
            p = cur + strlen(cur);
        }

        line++;

        {   /* strip a trailing CR from a file that crossed a Windows share */
            size_t n = strlen(cur);
            if (n && cur[n - 1] == '\r')
                cur[n - 1] = 0;
        }

        verb = tok(&cur);

        if (!verb || verb[0] == '#')
            continue;

        if (sawend)
        {
            scn_err("line %d: %.32s after the `end` marker", line, verb);
            return 0;
        }

        if (!sawv && strcmp(verb, "v1") != 0)
        {
            scn_err("line %d: expected the version line `v1 ...`, got %.32s", line, verb);
            return 0;
        }

        if (!strcmp(verb, "v1"))
        {
            char* t;
            sawv = 1;
            while ((t = tok(&cur)) != 0)
            {
                char* k;
                int   v;
                if (!strncmp(t, "onerror=", 8))
                {
                    g_abort = strcmp(t + 8, "skip") != 0;
                    continue;
                }
                if (!kv(t, &k, &v))
                {
                    scn_err("line %d: `%.32s` is not name=value", line, t);
                    return 0;
                }
                if (!strcmp(k, "seed"))       g_seed = v;
                else if (!strcmp(k, "clear")) g_clear = v != 0;
                else if (!strcmp(k, "units")) want_units = v;
                else if (!strcmp(k, "feats")) want_feats = v;
                else
                {
                    scn_err("line %d: unknown v1 key `%.32s`", line, k);
                    return 0;
                }
            }
            continue;
        }

        if (!strcmp(verb, "map"))
        {
            while (*cur == ' ' || *cur == '\t')
                cur++;
            _snprintf(g_map, sizeof g_map - 1, "%s", cur);
            g_map[sizeof g_map - 1] = 0;
            continue;
        }

        if (!strcmp(verb, "limit"))
        {
            if (!req(tok(&cur), &g_limit))
            {
                scn_err("line %d: limit wants one number", line);
                return 0;
            }
            continue;
        }

        if (!strcmp(verb, "sw"))
        {
            char* t;
            while ((t = tok(&cur)) != 0)
            {
                char* k;
                int   v, i, found = 0;
                if (!kv(t, &k, &v))
                {
                    scn_err("line %d: `%.32s` is not name=0|1", line, t);
                    return 0;
                }
                for (i = 0; SCN_SWITCHES[i].name; i++)
                    if (!strcmp(k, SCN_SWITCHES[i].name))
                    {
                        if (v) g_sw_set |= SCN_SWITCHES[i].bit;
                        else   g_sw_clr |= SCN_SWITCHES[i].bit;
                        found = 1;
                        break;
                    }
                if (!found)
                {
                    scn_err("line %d: no engine switch named `%.24s`", line, k);
                    return 0;
                }
            }
            continue;
        }

        if (!strcmp(verb, "player"))
        {
            char* t;
            int   slot;
            if (!req(tok(&cur), &slot) || slot < 0 || slot > 9)
            {
                scn_err("line %d: player wants a slot 0..9", line);
                return 0;
            }
            if (g_npl >= 10)
            {
                scn_err("line %d: more than ten player lines", line);
                return 0;
            }
            g_pl_slot[g_npl] = slot;
            g_pl_metal[g_npl] = SCN_UNSET;
            g_pl_energy[g_npl] = SCN_UNSET;
            while ((t = tok(&cur)) != 0)
            {
                char* k;
                int   v;
                if (!kv(t, &k, &v))
                {
                    scn_err("line %d: `%.32s` is not name=value", line, t);
                    return 0;
                }
                if (!strcmp(k, "metal"))       g_pl_metal[g_npl] = v;
                else if (!strcmp(k, "energy")) g_pl_energy[g_npl] = v;
                else
                {
                    scn_err("line %d: unknown player key `%.24s`", line, k);
                    return 0;
                }
            }
            g_npl++;
            continue;
        }

        if (!strcmp(verb, "unit"))
        {
            scn_unit* u;
            char*     name;
            int       ord;
            if (g_nunits >= SCN_MAX_UNITS)
            {
                scn_err("more than %d units -- the fork's own ceiling", SCN_MAX_UNITS);
                return 0;
            }
            if (!req(tok(&cur), &ord) || ord != g_nunits)
            {
                scn_err("line %d: unit ordinals must run 0,1,2... (expected %d)",
                        line, g_nunits);
                return 0;
            }
            u = &g_units[g_nunits];
            memset(u, 0, sizeof *u);
            name = tok(&cur);
            if (!name)
            {
                scn_err("line %d: unit %d has no type", line, ord);
                return 0;
            }
            copy_name(u->type, name);
            if (!req(tok(&cur), &u->owner) ||
                !req(tok(&cur), &u->x) || !req(tok(&cur), &u->y) ||
                !num(tok(&cur), &u->height) || !num(tok(&cur), &u->facing) ||
                !num(tok(&cur), &u->health) || !stance(tok(&cur), &u->stance) ||
                !num(tok(&cur), &u->nano))
            {
                scn_err("line %d: unit %d has a malformed column", line, ord);
                return 0;
            }
            u->defIdx = -1;
            g_nunits++;
            continue;
        }

        if (!strcmp(verb, "feat"))
        {
            scn_feat* fe;
            char*     name;
            int       ord;
            if (g_nfeats >= SCN_MAX_FEATS)
            {
                scn_err("more than %d features -- the fork's own ceiling", SCN_MAX_FEATS);
                return 0;
            }
            if (!req(tok(&cur), &ord) || ord != g_nfeats)
            {
                scn_err("line %d: feature ordinals must run 0,1,2... (expected %d)",
                        line, g_nfeats);
                return 0;
            }
            fe = &g_feats[g_nfeats];
            memset(fe, 0, sizeof *fe);
            name = tok(&cur);
            if (!name)
            {
                scn_err("line %d: feature %d has no type", line, ord);
                return 0;
            }
            copy_name(fe->type, name);
            if (!req(tok(&cur), &fe->x) || !req(tok(&cur), &fe->y) ||
                !num(tok(&cur), &fe->height) || !num(tok(&cur), &fe->facing))
            {
                scn_err("line %d: feature %d has a malformed column", line, ord);
                return 0;
            }
            fe->defIdx = -1;
            g_nfeats++;
            continue;
        }

        if (!strcmp(verb, "order"))
        {
            scn_order* o;
            char*      cmd;
            char*      kind;
            int        i, found = 0;
            if (g_norders >= SCN_MAX_ORDERS)
            {
                scn_err("more than %d orders -- the fork's own ceiling", SCN_MAX_ORDERS);
                return 0;
            }
            o = &g_orders[g_norders];
            memset(o, 0, sizeof *o);
            if (!order_subject(tok(&cur), o, line))
                return 0;
            cmd = tok(&cur);
            if (!cmd)
            {
                scn_err("line %d: order %d has no command", line, o->uord);
                return 0;
            }
            for (i = 0; SCN_ORDERTYPES[i].name; i++)
                if (!strcmp(cmd, SCN_ORDERTYPES[i].name))
                {
                    o->cmd = SCN_ORDERTYPES[i].id;
                    found = 1;
                    break;
                }
            if (!found)
            {
                scn_err("line %d: no engine order named `%.24s`", line, cmd);
                return 0;
            }
            kind = tok(&cur);
            if (!kind)
            {
                o->kind = TGT_NONE;
            }
            else if (!strcmp(kind, "pos"))
            {
                o->kind = TGT_POS;
                if (!req(tok(&cur), &o->a) || !req(tok(&cur), &o->b))
                {
                    scn_err("line %d: order %d pos wants x and y", line, o->uord);
                    return 0;
                }
            }
            else if (!strcmp(kind, "unit") || !strcmp(kind, "feat"))
            {
                char* t = tok(&cur);
                o->kind = kind[0] == 'u' ? TGT_UNIT : TGT_FEAT;
                if (o->kind == TGT_UNIT && t && t[0] == '@')
                {
                    if (!live_ref(t, &o->a, o->texp))
                    {
                        scn_err("line %d: order %d target `%.24s` is not "
                                "@<index> or @<index>:<TYPE>", line, o->uord, t);
                        return 0;
                    }
                    o->tlive = 1;
                }
                else if (!req(t, &o->a))
                {
                    scn_err("line %d: order %d %s wants an ordinal", line, o->uord, kind);
                    return 0;
                }
            }
            else
            {
                scn_err("line %d: order target must be pos/unit/feat, got `%.16s`",
                        line, kind);
                return 0;
            }
            g_norders++;
            continue;
        }

        if (!strcmp(verb, "cam"))
        {
            char* kind = tok(&cur);
            char* t;
            if (!kind)
            {
                scn_err("line %d: cam wants pos/unit/feat", line);
                return 0;
            }
            if (!strcmp(kind, "pos"))
            {
                g_cam_kind = TGT_POS;
                if (!req(tok(&cur), &g_cam_a) || !req(tok(&cur), &g_cam_b))
                {
                    scn_err("line %d: cam pos wants x and y", line);
                    return 0;
                }
            }
            else if (!strcmp(kind, "unit") || !strcmp(kind, "feat"))
            {
                g_cam_kind = kind[0] == 'u' ? TGT_UNIT : TGT_FEAT;
                if (!req(tok(&cur), &g_cam_a))
                {
                    scn_err("line %d: cam %s wants an ordinal", line, kind);
                    return 0;
                }
            }
            else
            {
                scn_err("line %d: cam target must be pos/unit/feat", line);
                return 0;
            }
            while ((t = tok(&cur)) != 0)
            {
                char* k;
                int   v;
                if (kv(t, &k, &v) && !strcmp(k, "pin"))
                    g_cam_pin = v != 0;
            }
            continue;
        }

        if (!strcmp(verb, "end"))
        {
            sawend = 1;
            continue;
        }

        scn_err("line %d: unknown wire verb `%.24s`", line, verb);
        return 0;
    }

    if (!sawend)
    {
        scn_err("the file has no `end` marker -- it is truncated, nothing was applied");
        return 0;
    }

    if (want_units >= 0 && want_units != g_nunits)
    {
        scn_err("the header promised %d units and the file carries %d",
                want_units, g_nunits);
        return 0;
    }

    if (want_feats >= 0 && want_feats != g_nfeats)
    {
        scn_err("the header promised %d features and the file carries %d",
                want_feats, g_nfeats);
        return 0;
    }

    return 1;
}

/* ----------------------------------------------------------------- the applier
   Everything below runs on the Game_MainLoopTick detour. No file I/O, no CRT
   allocation: this is the sim's own thread and the whole point is one visit. */

static void load_map_extents(char* ta)
{
    map_w  = *(int*)(ta + OFF_MAPPXW);
    map_h  = *(int*)(ta + OFF_MAPPXH);
    fmap_x = *(int*)(ta + OFF_FMAPX);
    fmap_y = *(int*)(ta + OFF_FMAPY);
    fmap   = *(char**)(ta + OFF_FMAP);
}

/* TADR's own height rule: feature cells are 16 world units, and the cell's
   `height` byte is the ground the engine would snap a unit to. */
static int ground_at(int x, int y)
{
    int gx = x / 16, gy = y / 16;
    long idx;

    if (!fmap || fmap_x <= 0 || fmap_y <= 0)
        return 0;

    if (gx < 0 || gy < 0 || gx >= fmap_x || gy >= fmap_y)
        return 0;

    idx = (long)gx + (long)gy * fmap_x;

    if (!readable(fmap + idx * FM_STRIDE, FM_STRIDE))
        return 0;

    return *(unsigned char*)(fmap + idx * FM_STRIDE + FM_HEIGHT);
}

static int in_map(int x, int y)
{
    return x >= 0 && y >= 0 && x < map_w && y < map_h;
}

static char* player_at(char* ta, int slot)
{
    char* pl;

    if (slot < 0 || slot > 9)
        return 0;

    pl = ta + OFF_PLAYERS + (size_t)slot * PL_STRIDE;
    return readable(pl, PL_STRIDE) ? pl : 0;
}

/* Degrees, 0 = TA's own default build facing (heading word 0x8000), increasing
   counter-clockwise from above over a full circle of 0x10000. */
static unsigned heading_of(int degrees)
{
    long h = 0x8000L + (long)degrees * 0x10000L / 360L;
    return (unsigned)(h & 0xFFFF);
}

static int unit_type_index(char* ta, const char* name)
{
    unsigned count = *(unsigned*)(ta + OFF_UCOUNT);
    char*    defs  = *(char**)(ta + OFF_UDEFS);
    unsigned i;

    if (!defs || count > 16384u)
        return -1;

    for (i = 0; i < count; i++)
    {
        const char* rec = defs + (size_t)i * UD_STRIDE;

        if (!readable(rec, UD_STRIDE))
            return -1;

        if (!lstrcmpiA(rec + UD_NAME, name))
            return (int)i;
    }

    return -1;
}

/* Layer 3: resolve everything against the LIVE game before creating anything.
   The CLI already checked these names against a catalogue, but the file is on
   disk and can be stale — a mod reload, a different game, an edited wire. */
static int resolve_all(char* ta)
{
    int bad = 0, i;

    for (i = 0; i < g_nunits; i++)
    {
        scn_unit* u = &g_units[i];

        u->defIdx = unit_type_index(ta, u->type);

        if (u->defIdx < 0)
        {
            scn_err("unit %d: no unit type named '%.20s' in this game", i, u->type);
            u->ok = 0;
            bad++;
            continue;
        }

        if (!in_map(u->x, u->y))
        {
            scn_err("unit %d (%.20s): %d,%d is outside the map (%dx%d)",
                    i, u->type, u->x, u->y, map_w, map_h);
            bad++;
            continue;
        }

        if (!player_at(ta, u->owner))
        {
            scn_err("unit %d (%.20s): player slot %d does not exist",
                    i, u->type, u->owner);
            bad++;
            continue;
        }

        if (!*(int*)(player_at(ta, u->owner) + PL_ACTIVE))
        {
            scn_err("unit %d (%.20s): player slot %d is not in this game",
                    i, u->type, u->owner);
            bad++;
            continue;
        }

        u->ok = 1;
    }

    for (i = 0; i < g_nfeats; i++)
    {
        scn_feat* fe = &g_feats[i];

        fe->defIdx = FeatureName2ID(fe->type);

        if (fe->defIdx < 0)
            fe->defIdx = LoadFeature(fe->type);

        if (fe->defIdx < 0)
        {
            scn_err("feature %d: no feature named '%.20s' in this game", i, fe->type);
            bad++;
            continue;
        }

        if (!in_map(fe->x, fe->y))
        {
            scn_err("feature %d (%.20s): %d,%d is outside the map (%dx%d)",
                    i, fe->type, fe->x, fe->y, map_w, map_h);
            bad++;
            continue;
        }

        fe->ok = 1;
    }

    /* The engine's per-player cap refuses silently, one unit at a time, so 200
       units over the line means 200 identical error lines and no diagnosis. Do
       the arithmetic here instead: what each player already has (minus what the
       clear is about to remove) plus what the file asks for. */
    {
        int want[10], have[10], cap = *(unsigned short*)(ta + OFF_PERPLAYER);
        char* beg = *(char**)(ta + OFF_BEGIN);
        char* end = *(char**)(ta + OFF_END);

        for (i = 0; i < 10; i++)
            want[i] = have[i] = 0;

        for (i = 0; i < g_nunits; i++)
            if (g_units[i].ok && g_units[i].owner >= 0 && g_units[i].owner < 10)
                want[g_units[i].owner]++;

        if (!g_clear && readable(beg, UNIT_STRIDE) && end > beg &&
            (size_t)(end - beg) <= (size_t)UNIT_STRIDE * 20000)
        {
            char* u;
            for (u = beg; u + UNIT_STRIDE <= end; u += UNIT_STRIDE)
            {
                int owner;
                if (!readable(u, UNIT_STRIDE))
                    break;
                if (!(*(unsigned*)(u + U_STATE) & 0x10000000u))
                    continue;
                owner = *(unsigned char*)(u + U_OWNER);
                if (owner >= 0 && owner < 10)
                    have[owner]++;
            }
        }

        if (cap > 0)
            for (i = 0; i < 10; i++)
                if (want[i] && want[i] + have[i] > cap)
                {
                    scn_err("player %d would end up with %d units and this game's "
                            "cap is %d -- raise UnitLimit before the game starts",
                            i, want[i] + have[i], cap);
                    bad++;
                }
    }

    for (i = 0; i < g_norders; i++)
    {
        scn_order* o = &g_orders[i];

        /* Only an ordinal can be checked here. A live subject names a slot in
           the running game, so it is resolved — and its type guard tested — at
           the apply point, on the game thread, where the answer cannot go stale
           between the check and the order. */
        if (o->subj == SUBJ_ORD && (o->uord < 0 || o->uord >= g_nunits))
        {
            scn_err("order %d: unit ordinal %d does not exist", i, o->uord);
            bad++;
            continue;
        }

        if (o->kind == TGT_UNIT && !o->tlive && (o->a < 0 || o->a >= g_nunits))
        {
            scn_err("order %d: target unit ordinal %d does not exist", i, o->a);
            bad++;
        }
        else if (o->kind == TGT_FEAT && (o->a < 0 || o->a >= g_nfeats))
        {
            scn_err("order %d: target feature ordinal %d does not exist", i, o->a);
            bad++;
        }
        else if (o->kind == TGT_POS && !in_map(o->a, o->b))
        {
            scn_err("order %d: target %d,%d is outside the map (%dx%d)",
                    i, o->a, o->b, map_w, map_h);
            bad++;
        }
    }

    if (g_cam_kind == TGT_UNIT && (g_cam_a < 0 || g_cam_a >= g_nunits))
    {
        scn_err("camera: unit ordinal %d does not exist", g_cam_a);
        bad++;
    }
    else if (g_cam_kind == TGT_FEAT && (g_cam_a < 0 || g_cam_a >= g_nfeats))
    {
        scn_err("camera: feature ordinal %d does not exist", g_cam_a);
        bad++;
    }

    return bad;
}

/* `setup.clear_existing` defaults true: a scenario contains exactly what the file
   says. UNITS_KillUnit mode 0 is the silent path — no explosion, no wreck.

   Two guards, both earned live on 2026-09-01:

   * The sweep runs over a SNAPSHOT of the array, never over the live array, so
     it kills exactly what was there when the scenario arrived and can never
     reach a unit the same tick created. It runs BEFORE the create pass: the
     engine's per-player cap (MaxUnitNumberPerPlayer, 250 in stock TA) counts
     units that are about to die, and creating first cost 302 of a 401-unit
     scenario. Clearing first is safe because the whole apply is one visit to a
     hook OUTSIDE the simulation loop — the sim never sees the empty world.
   * ActiveCommanderDeath is parked at zero for the kills. 0x486688 reads it and
     only calls UNITS_KillAllForPlayer when it is non-zero, so clearing a
     skirmish's starting commanders would otherwise take the whole side with it.

   What this cannot prevent: a scenario that leaves a player with no units at all
   is a defeat, and TA goes to ENDMSN.GUI where Game_MainLoopTick stops. That is
   the engine being right; the applier's job is to say so, which the arm watchdog
   in tagpu_scenario_frame does on the next attempt. */
static void snapshot_existing(char* ta)
{
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    char* u;

    g_nclear = 0;

    if (!readable(beg, UNIT_STRIDE) || end <= beg)
        return;

    if ((size_t)(end - beg) > (size_t)UNIT_STRIDE * 20000)
        return;

    for (u = beg; u + UNIT_STRIDE <= end; u += UNIT_STRIDE)
    {
        if (!readable(u, UNIT_STRIDE))
            break;

        if (!(*(unsigned*)(u + U_STATE) & 0x10000000u))
            continue;

        if (g_nclear >= SCN_MAX_CLEAR)
        {
            scn_err("more than %d units were already alive; the rest stay",
                    SCN_MAX_CLEAR);
            return;
        }

        g_clearlist[g_nclear++] = u;
    }
}

static void clear_snapshot(char* ta)
{
    unsigned saved = *(unsigned*)(ta + OFF_CMDRDEATH);
    int      i;

    *(unsigned*)(ta + OFF_CMDRDEATH) = 0;

    for (i = 0; i < g_nclear; i++)
    {
        char* u = g_clearlist[i];

        if (!readable(u, UNIT_STRIDE) || !(*(unsigned*)(u + U_STATE) & 0x10000000u))
            continue;

        UNITS_KillUnit(u, 0);
        g_cleared++;
    }

    *(unsigned*)(ta + OFF_CMDRDEATH) = saved;
}

static void apply_switches(char* ta)
{
    unsigned short* sw = (unsigned short*)(ta + OFF_SWITCHES);

    g_sw_before = *sw;
    *sw = (unsigned short)((g_sw_before | g_sw_set) & ~g_sw_clr);
    g_sw_after = *sw;
}

/* Player resources are the one thing in the schema the apply point cannot make
   stick. The write lands — the offsets are confirmed against a live read — but
   TA recomputes storage from the units a player owns and refills current from
   production every simulation tick, so both fields are back to the engine's own
   numbers before the next frame. Measured 2026-09-01: asking for 4321 metal
   against 50 storage, and even for 12 against the same 50, both read 50 again.

   So the write stays (it is free, and correct where the engine has no opinion),
   and the result reports the read-back from the SAME tick beside a second read
   taken when the file is written. Requested, wrote, and what survived: an agent
   sees the clamp instead of believing a number. Setting resources for real is a
   launch-time problem and belongs to `scenario load`. */
static void apply_players(char* ta)
{
    int i;

    for (i = 0; i < 10; i++)
        g_pl_got_metal[i] = g_pl_got_energy[i] = SCN_UNSET;

    for (i = 0; i < g_npl; i++)
    {
        char* pl = player_at(ta, g_pl_slot[i]);

        if (!pl)
            continue;

        if (g_pl_metal[i] != SCN_UNSET)
        {
            float want = (float)g_pl_metal[i];
            if (*(float*)(pl + PL_MAXMETAL) < want)
                *(float*)(pl + PL_MAXMETAL) = want;
            *(float*)(pl + PL_METAL) = want;
            g_pl_got_metal[i] = (int)*(float*)(pl + PL_METAL);
        }

        if (g_pl_energy[i] != SCN_UNSET)
        {
            float want = (float)g_pl_energy[i];
            if (*(float*)(pl + PL_MAXENERGY) < want)
                *(float*)(pl + PL_MAXENERGY) = want;
            *(float*)(pl + PL_ENERGY) = want;
            g_pl_got_energy[i] = (int)*(float*)(pl + PL_ENERGY);
        }
    }
}

static void dress_unit(scn_unit* u)
{
    char*    p = (char*)u->unit;
    unsigned state = *(unsigned*)(p + U_STATE);
    int      maxhp = *(short*)(p + U_HEALTH);   /* created fullHp: this is the max */

    if (u->facing != SCN_UNSET)
        *(unsigned short*)(p + U_HEADING) = (unsigned short)heading_of(u->facing);

    if (u->stance != SCN_UNSET && u->stance >= 0 && u->stance <= 2)
        state = (state & ~0x000C0000u) | ((unsigned)u->stance << 18);

    if (u->nano != SCN_UNSET)
    {
        /* `nanoframe` is the percentage BUILT; the engine's field is the fraction
           REMAINING (research/notes/build-state.md), 0.0 = finished. */
        int pct = u->nano < 0 ? 0 : (u->nano > 100 ? 100 : u->nano);
        *(float*)(p + U_NANOFRAME) = (float)(100 - pct) / 100.0f;
        *(short*)(p + U_HEALTH) = (short)(maxhp * pct / 100);
        *(unsigned char*)(p + U_HEALTHPCTA) = (unsigned char)pct;
        *(unsigned char*)(p + U_HEALTHPCTB) = (unsigned char)pct;
        if (pct < 100)
            state |= 0x20u;
        else
            state &= ~0x20u;
    }

    if (u->health != SCN_UNSET)
    {
        int pct = u->health < 1 ? 1 : (u->health > 100 ? 100 : u->health);
        *(short*)(p + U_HEALTH) = (short)(maxhp * pct / 100);
        *(unsigned char*)(p + U_HEALTHPCTA) = (unsigned char)pct;
        *(unsigned char*)(p + U_HEALTHPCTB) = (unsigned char)pct;
    }

    *(unsigned*)(p + U_STATE) = state;

    u->ax = *(unsigned short*)(p + U_XPOS);
    u->ay = *(unsigned short*)(p + U_YPOS);
    u->ah = *(unsigned short*)(p + U_ZPOS);
    u->engineIdx = *(short*)(p + U_INGAMEIDX);
}

static void create_units(void)
{
    int i;

    for (i = 0; i < g_nunits; i++)
    {
        scn_unit* u = &g_units[i];
        int       alt;

        if (!u->ok)
        {
            g_failed++;
            continue;
        }

        alt = u->height != SCN_UNSET ? u->height : ground_at(u->x, u->y);

        u->unit = UNITS_CreateUnit(u->owner, u->defIdx,
                                   u->x << 16, alt << 16, u->y << 16, 1, 1, 0);

        if (!u->unit || !readable(u->unit, UNIT_STRIDE))
        {
            u->unit = 0;
            u->ok = 0;
            g_failed++;
            scn_err("unit %d (%.20s): the engine refused to create it "
                    "(unit limit, or no room at %d,%d)", i, u->type, u->x, u->y);
            continue;
        }

        dress_unit(u);
        g_applied++;
    }
}

static void create_features(void)
{
    int i;

    for (i = 0; i < g_nfeats; i++)
    {
        scn_feat* fe = &g_feats[i];
        int       pos[3], alt;
        unsigned short turn[3];
        void*     plot;

        if (!fe->ok)
        {
            g_failed++;
            continue;
        }

        alt = fe->height != SCN_UNSET ? fe->height : ground_at(fe->x, fe->y);

        /* TPosition is the 3-D convention {x, altitude, depth}, 16.16 fixed point;
           TTurn is {bank, pitch, heading} as words [TA_MemoryStructures.pas]. */
        pos[0] = fe->x << 16;
        pos[1] = alt << 16;
        pos[2] = fe->y << 16;
        turn[0] = 0;
        turn[1] = 0;
        turn[2] = (unsigned short)heading_of(fe->facing != SCN_UNSET ? fe->facing : 0);

        plot = GetGridPosPLOT(fe->x / 16, fe->y / 16);

        if (!plot)
        {
            fe->ok = 0;
            g_failed++;
            scn_err("feature %d (%.20s): no map cell at %d,%d", i, fe->type, fe->x, fe->y);
            continue;
        }

        fe->feat = SpawnFeatureOnMap(plot, (short)fe->defIdx, pos, turn, 10);

        if (!fe->feat)
        {
            fe->ok = 0;
            g_failed++;
            scn_err("feature %d (%.20s): the engine refused to place it at %d,%d",
                    i, fe->type, fe->x, fe->y);
            continue;
        }

        fe->ax = fe->x;
        fe->ay = fe->y;
        fe->ah = alt;
        g_applied++;
    }
}

/* TADR's SendOrder recipe, minus its one dead call: ScriptAction_Index2Handler
   (0x438830) is a pure `base + *ecx * 25` address computation whose result TADR
   discards, so it is not on this path. */
/* ---- naming a unit the wire did not create ---------------------------------

   Both resolvers run at the apply point, on the game thread, so what they
   return cannot be stale by the time the order is issued.

   `live_by_index` is the engine's own index idiom: UnitInGameIndex is the slot
   in the unit array, which is how tagpu_order.c already resolves the tracked
   and hovered units. The index is recycled on death, and that is exactly what
   `expect` is for — the caller says what it believes is in the slot (the type
   name the roster reported) and a mismatch is an error instead of an order to
   whatever moved in. */
static char* live_by_index(char* ta, int idx, const char* expect, const char** why)
{
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    char* u;
    char* def;

    *why = 0;

    if (!readable(beg, UNIT_STRIDE) || end <= beg)
    {
        *why = "the game has no unit array yet";
        return 0;
    }

    if (idx < 0 || (size_t)(idx + 1) * UNIT_STRIDE > (size_t)(end - beg))
    {
        *why = "index is outside the unit array";
        return 0;
    }

    u = beg + (size_t)idx * UNIT_STRIDE;

    if (!readable(u, UNIT_STRIDE))
    {
        *why = "that slot is not readable";
        return 0;
    }

    if (!(*(unsigned*)(u + U_STATE) & 0x10000000u))
    {
        *why = "nothing alive in that slot";
        return 0;
    }

    if (expect && expect[0])
    {
        def = *(char**)(u + U_UDEF);

        if (!readable(def, UD_NAME + SCN_NAMELEN))
        {
            *why = "that unit has no readable definition";
            return 0;
        }

        if (lstrcmpiA(def + UD_NAME, expect) != 0)
        {
            *why = "the slot holds a different unit than expected "
                   "(the index was recycled)";
            return 0;
        }
    }

    return u;
}

/* The watched player's current selection, the engine's own walk: the player's
   unit range at PlayerStruct+0x67..+0x6B, inclusive, stepping the unit stride,
   keeping the alive and unexcluded ones whose state carries the selected bit.
   Same three tests CorretCursor_InGame and our own order overlay make. */
static int live_selected(char* ta, char** out, int max)
{
    unsigned char watched = *(unsigned char*)(ta + OFF_WATCHED);
    char* player = ta + OFF_PLAYERS + (size_t)watched * PL_STRIDE;
    char* first;
    char* last;
    char* u;
    int   n = 0;

    if (!readable(player, PL_STRIDE))
        return 0;

    first = *(char**)(player + PL_FIRSTUNIT);
    last  = *(char**)(player + PL_LASTUNIT);

    if (!readable(first, UNIT_STRIDE) || last < first)
        return 0;

    if ((size_t)(last - first) > (size_t)UNIT_STRIDE * 20000)
        return 0;

    for (u = first; u <= last && n < max; u += UNIT_STRIDE)
    {
        unsigned st;

        if (!readable(u, UNIT_STRIDE))
            break;

        st = *(unsigned*)(u + U_STATE);

        if (!(st & 0x10000000u) || (st & 0x4000u))
            continue;

        if (st & 0x10u)
            out[n++] = u;
    }

    return n;
}

/* WHERE a live unit is, in the whole world units the order path wants. */
static void live_pos(char* u, int* x, int* alt, int* y)
{
    *x   = *(unsigned short*)(u + U_XPOS);
    *alt = *(unsigned short*)(u + U_ZPOS);
    *y   = *(unsigned short*)(u + U_YPOS);
}

#define SCN_MAX_SEL 512

static void issue_orders(void)
{
    char* ta = *(char**)TA_MAINPP;
    int i;

    for (i = 0; i < g_norders; i++)
    {
        scn_order*  o = &g_orders[i];
        char*       subj[SCN_MAX_SEL];
        const char* sname = "";
        const char* why = 0;
        int         nsubj = 0, s;
        void*       target = 0;
        int         pos[3];
        int         havepos = 0;

        /* WHO ------------------------------------------------------------- */
        if (o->subj == SUBJ_ORD)
        {
            scn_unit* u = &g_units[o->uord];
            sname = u->type;
            if (u->unit)
                subj[nsubj++] = (char*)u->unit;
        }
        else if (o->subj == SUBJ_LIVE)
        {
            char* u = live_by_index(ta, o->uord, o->sexp, &why);
            sname = o->sexp[0] ? o->sexp : "that unit";
            if (u)
                subj[nsubj++] = u;
            else
                scn_err("order %d: unit @%d -- %s", i, o->uord, why);
        }
        else
        {
            nsubj = live_selected(ta, subj, SCN_MAX_SEL);
            sname = "the selection";
            if (!nsubj)
                scn_err("order %d: nothing is selected", i);
            else if (nsubj == SCN_MAX_SEL)
                /* A cap that swallows the overflow quietly would report a full
                   success while ordering part of the selection. Say it. */
                scn_err("order %d: more than %d units are selected; only the "
                        "first %d were ordered", i, SCN_MAX_SEL, SCN_MAX_SEL);
        }

        if (!nsubj)
        {
            g_ord_fail++;
            continue;
        }

        /* WHERE / WHAT ----------------------------------------------------- */
        memset(pos, 0, sizeof pos);

        if (o->kind == TGT_POS)
        {
            /* 16.16 fixed point, 3-D convention {x, altitude, depth} — the same
               language UNITS_CreateUnit speaks, not the asymmetry this file used
               to claim (see the header comment). */
            pos[0] = o->a << 16;
            pos[1] = ground_at(o->a, o->b) << 16;
            pos[2] = o->b << 16;
            havepos = 1;
        }
        else if (o->kind == TGT_UNIT && o->tlive)
        {
            int x, alt, y;
            char* t = live_by_index(ta, o->a, o->texp, &why);
            if (!t)
            {
                g_ord_fail++;
                scn_err("order %d: target unit @%d -- %s", i, o->a, why);
                continue;
            }
            target = t;
            live_pos(t, &x, &alt, &y);
            pos[0] = x << 16;
            pos[1] = alt << 16;
            pos[2] = y << 16;
            havepos = 1;
        }
        else if (o->kind == TGT_UNIT)
        {
            target = g_units[o->a].unit;
            if (!target)
            {
                g_ord_fail++;
                scn_err("order %d: its target unit %d was never created", i, o->a);
                continue;
            }
            pos[0] = g_units[o->a].ax << 16;
            pos[1] = g_units[o->a].ah << 16;
            pos[2] = g_units[o->a].ay << 16;
            havepos = 1;
        }
        else if (o->kind == TGT_FEAT)
        {
            /* A feature is not a UnitStruct: TA reclaims and attacks it through the
               map cell, so the order carries the position and no target pointer.
               A feature the wire did not create is named by its position instead —
               `pos` reaches the same code, which is why there is no @ form here. */
            pos[0] = g_feats[o->a].ax << 16;
            pos[1] = g_feats[o->a].ah << 16;
            pos[2] = g_feats[o->a].ay << 16;
            havepos = 1;
        }

        /* ISSUE ------------------------------------------------------------ */
        for (s = 0; s < nsubj; s++)
        {
            int   index = -1;
            char* resolved = ScriptAction_Type2Index(&index, (unsigned)o->cmd,
                                                     subj[s], target, pos);

            if (!resolved)
            {
                g_ord_fail++;
                scn_err("order %d: this unit cannot take that order (%.20s, cmd %d)",
                        i, sname, o->cmd);
                continue;
            }

            ORDERS_NewMainOrder2Unit((int)(unsigned char)*resolved, 0, subj[s],
                                     target, pos, 0, 0);
            g_ord_ok++;

            /* Read the stored order position back once, in whole world units, so the
               result file shows where the order actually points. It cannot prove the
               scale (the constructor copies the dwords verbatim), but it does catch
               the engine relocating or clamping a target. */
            if (!g_probe_have && havepos)
            {
                char* ord = *(char**)(subj[s] + U_ORDERS);
                g_probe_pass[0] = pos[0] >> 16;
                g_probe_pass[1] = pos[1] >> 16;
                g_probe_pass[2] = pos[2] >> 16;
                if (readable(ord, UO_POS + 12))
                {
                    g_probe_stored[0] = *(int*)(ord + UO_POS)       >> 16;
                    g_probe_stored[1] = *(int*)(ord + UO_POS + 4)   >> 16;
                    g_probe_stored[2] = *(int*)(ord + UO_POS + 8)   >> 16;
                    g_probe_have = 1;
                }
            }
        }
    }
}

static void resolve_camera(void)
{
    if (g_cam_kind == TGT_POS)
    {
        g_cam_x = g_cam_a;
        g_cam_y = g_cam_b;
        g_cam_h = ground_at(g_cam_a, g_cam_b);
        g_cam_have = 1;
    }
    else if (g_cam_kind == TGT_UNIT && g_units[g_cam_a].unit)
    {
        /* an entity handle resolves to where the unit ACTUALLY landed */
        g_cam_x = g_units[g_cam_a].ax;
        g_cam_y = g_units[g_cam_a].ay;
        g_cam_h = g_units[g_cam_a].ah;
        g_cam_have = 1;
    }
    else if (g_cam_kind == TGT_FEAT && g_feats[g_cam_a].ok)
    {
        g_cam_x = g_feats[g_cam_a].ax;
        g_cam_y = g_feats[g_cam_a].ay;
        g_cam_h = g_feats[g_cam_a].ah;
        g_cam_have = 1;
    }
}

/* The one visit. Everything the scenario asks for lands here, in this order, so
   the situation is reproducible: switches first (units must spawn under the
   rules the file asked for), then the clear, then entities, then orders. */
static void apply_now(void)
{
    char* ta = ta_base();
    int   bad;

    if (!ta)
    {
        scn_err("no game structure -- apply needs a running game, not the menu");
        return;
    }

    load_map_extents(ta);
    g_gametime = *(int*)(ta + OFF_GAMETIME);
    g_limit_live = *(unsigned short*)(ta + OFF_LIMIT);
    g_perplayer_live = *(unsigned short*)(ta + OFF_PERPLAYER);

    {
        char* beg = *(char**)(ta + OFF_BEGIN);
        char* end = *(char**)(ta + OFF_END);
        if (readable(beg, UNIT_STRIDE) && end > beg &&
            (size_t)(end - beg) <= (size_t)UNIT_STRIDE * 20000)
            g_cap = (int)((end - beg) / UNIT_STRIDE);
    }

    if (map_w <= 0 || map_h <= 0)
    {
        scn_err("the map has no extents yet -- is the game past its loading screen?");
        return;
    }

    bad = resolve_all(ta);

    if (bad && g_abort)
    {
        scn_err("%d entities did not resolve and on_error is abort: "
                "nothing was created", bad);
        return;
    }

    apply_switches(ta);
    apply_players(ta);

    if (g_clear)
    {
        snapshot_existing(ta);
        clear_snapshot(ta);
    }

    create_units();
    create_features();
    issue_orders();
    resolve_camera();
}

/* --------------------------------------------------------------- the detour
   Classic 5-byte E9 at a byte-matched site, jumping to a runtime-generated stub:
   pushad -> call C -> popad -> the stolen prologue -> jmp back. pushad/popad
   bracket the call, so the stolen `mov eax, ds:0x511DE8` re-executes exactly as
   the untouched code would, then control resumes past it.

   Incoming EFLAGS are dead at the resume point (0x4969D7 is lea/call/mov/mov,
   and the first flag consumer at 0x4969E9 writes them first), so they need no
   saving — the same argument tagpu_tracer.c makes for its two sites. */

static void __cdecl scn_tick(void)
{
    if (InterlockedCompareExchange(&g_state, ST_APPLYING, ST_ARMED) != ST_ARMED)
        return;

    apply_now();
    InterlockedExchange(&g_state, ST_DONE);
}

static int install_tick_hook(void)
{
    unsigned char* t = (unsigned char*)TICK_SITE;
    unsigned char* stub;
    unsigned char* p;
    DWORD          old;
    int            rel;

    if (memcmp(t, TICK_STOLEN, sizeof TICK_STOLEN) != 0)
    {
        scnlog("scenario: 0x4969D2 does not hold the expected bytes -- "
               "wrong TotalA.exe build; the applier stays disarmed");
        return 0;
    }

    stub = (unsigned char*)VirtualAlloc(NULL, 0x100, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);

    if (!stub)
    {
        scnlog("scenario: VirtualAlloc(stub) failed -- the applier stays disarmed");
        return 0;
    }

    p = stub;
    *p++ = 0x60;                                            /* pushad            */
    *p++ = 0xE8;                                            /* call rel32        */
    rel = (int)((unsigned)&scn_tick - ((unsigned)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x61;                                            /* popad             */
    memcpy(p, TICK_STOLEN, sizeof TICK_STOLEN);             /* stolen prologue   */
    p += sizeof TICK_STOLEN;
    *p++ = 0xE9;                                            /* jmp rel32 (resume)*/
    rel = (int)((TICK_SITE + sizeof TICK_STOLEN) - ((unsigned)p + 4));
    memcpy(p, &rel, 4);

    if (!VirtualProtect(t, sizeof TICK_STOLEN, PAGE_EXECUTE_READWRITE, &old))
    {
        scnlog("scenario: VirtualProtect failed -- the applier stays disarmed");
        return 0;
    }

    t[0] = 0xE9;
    rel = (int)((unsigned)stub - (TICK_SITE + 5));
    memcpy(t + 1, &rel, 4);
    VirtualProtect(t, sizeof TICK_STOLEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, sizeof TICK_STOLEN);

    scnlogf("scenario: applier armed, tick detour at 0x%08X -> stub %p",
            TICK_SITE, (void*)stub);
    return 1;
}

/* -------------------------------------------------------------- the reporting
   Back on the present path: the result is written tmp + rename so the CLI's poll
   can never read a half-built file, exactly like tagpu_ui and tagpu_cat. */

static void write_switch_bits(FILE* f, unsigned value)
{
    int i;

    fprintf(f, "{");

    for (i = 0; SCN_SWITCHES[i].name; i++)
        fprintf(f, "%s\"%s\":%s", i ? "," : "", SCN_SWITCHES[i].name,
                (value & SCN_SWITCHES[i].bit) ? "true" : "false");

    fprintf(f, "}");
}

static void write_result(void)
{
    FILE* f = fopen(SCN_TMP, "wb");
    int   i, ok, first;

    if (!f)
    {
        scnlog("scenario: cannot open the result temp file");
        return;
    }

    ok = (g_nerrs == 0 && g_errdrop == 0);

    fprintf(f, "{\"ok\":%d,\"frame\":%u,\"gametime\":%d", ok ? 1 : 0, g_frame, g_gametime);
    fprintf(f, ",\"seed\":%d", g_seed);
    fprintf(f, ",\"requested\":{\"units\":%d,\"features\":%d,\"orders\":%d}",
            g_nunits, g_nfeats, g_norders);
    fprintf(f, ",\"applied\":%d,\"failed\":%d,\"cleared\":%d", g_applied, g_failed,
            g_cleared);
    fprintf(f, ",\"orders\":{\"issued\":%d,\"failed\":%d}", g_ord_ok, g_ord_fail);
    fprintf(f, ",\"map\":{\"name\":");
    jstr(f, g_map, sizeof g_map);
    fprintf(f, ",\"world\":[%d,%d],\"tiles\":[%d,%d]}", map_w, map_h, fmap_x, fmap_y);
    fprintf(f, ",\"limit\":{\"requested\":%s,\"total\":%d,\"per_player\":%d,"
               "\"array_slots\":%d}",
            g_limit == SCN_UNSET ? "null" : "0", g_limit_live, g_perplayer_live, g_cap);
    if (g_limit != SCN_UNSET)
        fprintf(f, ",\"limit_requested\":%d", g_limit);

    fprintf(f, ",\"switches\":{\"before\":%u,\"after\":%u,\"bits\":",
            g_sw_before, g_sw_after);
    write_switch_bits(f, g_sw_after);
    fprintf(f, "}");

    fprintf(f, ",\"units\":{");
    first = 1;
    for (i = 0; i < g_nunits; i++)
    {
        scn_unit* u = &g_units[i];
        if (!u->unit)
            continue;
        fprintf(f, "%s\"%d\":{\"engine_index\":%d,\"owner\":%d,\"type\":",
                first ? "" : ",", i, u->engineIdx, u->owner);
        jstr(f, u->type, SCN_NAMELEN);
        fprintf(f, ",\"requested\":[%d,%d],\"actual\":[%d,%d,%d]}",
                u->x, u->y, u->ax, u->ay, u->ah);
        first = 0;
    }
    fprintf(f, "}");

    fprintf(f, ",\"features\":{");
    first = 1;
    for (i = 0; i < g_nfeats; i++)
    {
        scn_feat* fe = &g_feats[i];
        if (!fe->ok)
            continue;
        fprintf(f, "%s\"%d\":{\"def\":%d,\"type\":", first ? "" : ",", i, fe->defIdx);
        jstr(f, fe->type, SCN_NAMELEN);
        fprintf(f, ",\"requested\":[%d,%d],\"actual\":[%d,%d,%d]}",
                fe->x, fe->y, fe->ax, fe->ay, fe->ah);
        first = 0;
    }
    fprintf(f, "}");

    if (g_npl)
    {
        char* ta = ta_base();
        fprintf(f, ",\"players\":{");
        for (i = 0; i < g_npl; i++)
        {
            char* pl = ta ? player_at(ta, g_pl_slot[i]) : 0;
            fprintf(f, "%s\"%d\":{", i ? "," : "", g_pl_slot[i]);
            fprintf(f, "\"requested\":{\"metal\":%d,\"energy\":%d}",
                    g_pl_metal[i] == SCN_UNSET ? -1 : g_pl_metal[i],
                    g_pl_energy[i] == SCN_UNSET ? -1 : g_pl_energy[i]);
            fprintf(f, ",\"wrote\":{\"metal\":%d,\"energy\":%d}",
                    g_pl_got_metal[i] == SCN_UNSET ? -1 : g_pl_got_metal[i],
                    g_pl_got_energy[i] == SCN_UNSET ? -1 : g_pl_got_energy[i]);
            if (pl)
                fprintf(f, ",\"now\":{\"metal\":%d,\"energy\":%d}",
                        (int)*(float*)(pl + PL_METAL), (int)*(float*)(pl + PL_ENERGY));
            fprintf(f, "}");
        }
        fprintf(f, "}");
    }

    if (g_probe_have)
        fprintf(f, ",\"order_probe\":{\"passed\":[%d,%d,%d],\"stored\":[%d,%d,%d]}",
                g_probe_pass[0], g_probe_pass[1], g_probe_pass[2],
                g_probe_stored[0], g_probe_stored[1], g_probe_stored[2]);

    if (g_cam_have)
        fprintf(f, ",\"camera\":[%d,%d],\"pin\":%d", g_cam_x, g_cam_y, g_cam_pin);
    else
        fprintf(f, ",\"camera\":null,\"pin\":0");

    if (g_eye_have)
        fprintf(f, ",\"eye\":[%d,%d]", g_eye_x, g_eye_y);
    else
        fprintf(f, ",\"eye\":null");

    fprintf(f, ",\"errors\":[");
    for (i = 0; i < g_nerrs; i++)
    {
        if (i) fputc(',', f);
        jstr(f, g_errs[i], SCN_ERRLEN);
    }
    fprintf(f, "]");

    if (g_errdrop)
        fprintf(f, ",\"errors_dropped\":%d", g_errdrop);

    fprintf(f, "}\n");
    fclose(f);

    if (!MoveFileExA(SCN_TMP, SCN_OUT, MOVEFILE_REPLACE_EXISTING))
        scnlog("scenario: could not replace the result file");
}

/* The camera is display state, not sim state, so it is set here rather than on
   the tick — the same rule (and the same clamp) as tagpu_input.c's eye hold.
   `at` and `center_on` both mean the CENTRE of the window, so the eye is the
   centre minus half a view, using the projection the roster already reports. */
static void place_camera(const TAGPU_FRAME* f)
{
    char* ta = ta_base();
    int   gw, gh, mw, mh, vw, vh, ex, ey;

    if (!g_cam_have || !ta)
        return;

    gw = f && f->game_width  > 0 ? f->game_width  : 640;
    gh = f && f->game_height > 0 ? f->game_height : 480;
    mw = *(int*)(ta + OFF_MAPPXW);
    mh = *(int*)(ta + OFF_MAPPXH);
    vw = *(int*)(ta + OFF_VIEW_W);
    vh = *(int*)(ta + OFF_VIEW_H);

    if (mw <= 0 || mh <= 0 || vw <= 0 || vh <= 0)
        return;

    /* The overlay's own projection, inverted:
           sx = wx - eyeX + 128
           sy = wy - altitude/2 - eyeY + 32
       so the target's HEIGHT belongs in the answer — without it a unit on a hill
       sits half its altitude above the middle of the window. */
    ex = g_cam_x - gw / 2 + 128;
    ey = g_cam_y - g_cam_h / 2 - gh / 2 + 32;

    if (ex < 0) ex = 0; else if (ex > mw - vw) ex = mw - vw;
    if (ey < 0) ey = 0; else if (ey > mh - vh) ey = mh - vh;

    *(volatile int*)(ta + OFF_EYEX)  = ex;
    *(volatile int*)(ta + OFF_EYEY)  = ey;
    *(volatile int*)(ta + OFF_SCRTX) = ex;
    *(volatile int*)(ta + OFF_SCRTY) = ey;

    /* Reported so `pin` can hold exactly this eye through tagpu_eye.txt rather
       than re-deriving the projection in a second place. */
    g_eye_x = ex;
    g_eye_y = ey;
    g_eye_have = 1;
}

static char* read_trigger(void)
{
    HANDLE h = CreateFileA(SCN_TRIGGER, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  size, got = 0;
    char*  buf;

    if (h == INVALID_HANDLE_VALUE)
        return 0;

    size = GetFileSize(h, NULL);

    if (size == INVALID_FILE_SIZE || size > SCN_FILEMAX)
    {
        CloseHandle(h);
        return 0;
    }

    buf = (char*)malloc((size_t)size + 1);

    if (!buf)
    {
        CloseHandle(h);
        return 0;
    }

    if (!ReadFile(h, buf, size, &got, NULL))
        got = 0;

    CloseHandle(h);
    buf[got] = 0;
    return buf;
}

/* ------------------------------------------------------------------- switches
   `tacli switches` — the same SoftwareDebugMode word, on its own trigger, so a
   switch can be A/B'd on a live instance with no scenario at all. It works at
   the menus too: the word lives in the main struct, which exists before a game. */
static void do_switches(void)
{
    char*           ta = *(char**)TA_MAINPP;
    HANDLE          h;
    DWORD           got = 0;
    char            buf[512];
    unsigned short* sw;
    unsigned        before, after;
    FILE*           out;
    char*           p;
    int             bad = 0;
    char            badname[32];

    badname[0] = 0;

    h = CreateFileA(SW_TRIGGER, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (h != INVALID_HANDLE_VALUE)
    {
        if (!ReadFile(h, buf, sizeof buf - 1, &got, NULL))
            got = 0;
        CloseHandle(h);
    }

    buf[got] = 0;
    DeleteFileA(SW_TRIGGER);

    out = fopen(SW_TMP, "wb");

    if (!out)
        return;

    if (!readable(ta, OFF_SWITCHES + 2))
    {
        fprintf(out, "{\"ok\":0,\"note\":\"no game structure yet\"}\n");
        fclose(out);
        MoveFileExA(SW_TMP, SW_OUT, MOVEFILE_REPLACE_EXISTING);
        return;
    }

    sw = (unsigned short*)(ta + OFF_SWITCHES);
    before = *sw;
    after = before;

    for (p = buf; *p; )
    {
        char* t;
        char* eol = strchr(p, '\n');
        char* cur = p;

        if (eol) { *eol = 0; p = eol + 1; } else { p = cur + strlen(cur); }

        while ((t = tok(&cur)) != 0)
        {
            char* k;
            int   v, i, found = 0;

            if (t[0] == '#' || !kv(t, &k, &v))
                break;

            for (i = 0; SCN_SWITCHES[i].name; i++)
                if (!lstrcmpiA(k, SCN_SWITCHES[i].name))
                {
                    if (v) after |= SCN_SWITCHES[i].bit;
                    else   after &= ~SCN_SWITCHES[i].bit;
                    found = 1;
                    break;
                }

            if (!found)
            {
                bad = 1;
                _snprintf(badname, sizeof badname - 1, "%s", k);
                badname[sizeof badname - 1] = 0;
            }
        }
    }

    if (!bad)
        *sw = (unsigned short)after;

    fprintf(out, "{\"ok\":%d,\"before\":%u,\"after\":%u,\"bits\":", bad ? 0 : 1,
            before, *sw);
    write_switch_bits(out, *sw);

    if (bad)
    {
        fprintf(out, ",\"error\":");
        jstr(out, badname, sizeof badname);
    }

    fprintf(out, "}\n");
    fclose(out);

    if (!MoveFileExA(SW_TMP, SW_OUT, MOVEFILE_REPLACE_EXISTING))
        scnlog("scenario: could not replace the switches file");
}

/* --------------------------------------------------------------- the per-frame
   Same cadence as the other on-demand modules: the CLI polls the result in an
   auto-wait loop, so a five-frame worst case is what it feels like. */

void tagpu_scenario_frame(const TAGPU_FRAME* f)
{
    if (f && (f->frame_counter % 5))
        return;

    g_frame = f ? f->frame_counter : 0;

    if (GetFileAttributesA(SW_TRIGGER) != INVALID_FILE_ATTRIBUTES)
        do_switches();

    if (g_state == ST_DONE)
    {
        place_camera(f);
        write_result();
        scnlogf("scenario: applied=%d failed=%d cleared=%d orders=%d/%d errors=%d",
                g_applied, g_failed, g_cleared, g_ord_ok, g_ord_ok + g_ord_fail,
                g_nerrs + g_errdrop);
        InterlockedExchange(&g_state, ST_IDLE);
        return;
    }

    if (g_state == ST_ARMED && g_frame - g_armed_at > SCN_ARM_FRAMES)
    {
        /* The tick may claim the arena at any moment; only report the give-up if
           we won the race for it. */
        if (InterlockedCompareExchange(&g_state, ST_IDLE, ST_ARMED) == ST_ARMED)
        {
            scn_err("the game's main loop never reached the apply point in %u frames "
                    "-- apply needs a running game, not the menus, the mission-end "
                    "screen or a paused one", SCN_ARM_FRAMES);
            write_result();
        }
        return;
    }

    if (g_state != ST_IDLE)
        return;                              /* a tick still owns the arena */

    if (GetFileAttributesA(SCN_TRIGGER) == INVALID_FILE_ATTRIBUTES)
        return;

    {
        char* text = read_trigger();

        DeleteFileA(SCN_TRIGGER);

        if (!text)
        {
            arena_reset();
            scn_err("the trigger file could not be read (missing, empty or over %u bytes)",
                    SCN_FILEMAX);
            write_result();
            return;
        }

        if (!parse_wire(text))
        {
            free(text);
            write_result();                  /* the errors say which line was wrong */
            return;
        }

        free(text);

        if (!g_hooked)
            g_hooked = install_tick_hook();

        if (!g_hooked)
        {
            scn_err("the tick detour is not installed -- nothing was applied");
            write_result();
            return;
        }

        g_armed_at = g_frame;
        InterlockedExchange(&g_state, ST_ARMED);
    }
}
