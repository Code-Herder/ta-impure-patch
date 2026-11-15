/* tagpu_topbar -- see tagpu_topbar.h and research/notes/modules/topbar.md.

   THE REPAINT. DrawGameScreen repaints its resource block, the top bar's tiles across the whole
   screen width included (the tile loop 0x469055..0x46909D runs to main+0x37E1F), only when a
   33-byte memo of what the block shows, main+0x37E3F, differs from the live values (repz cmpsb at
   0x468FD9, je 0x469610); otherwise every pixel drawn on the bar stays there. Text drawn on it
   that changes would therefore print over its old self. A repaint is asked by writing the memo's
   byte 0: the block writes that byte with the viewed record's +0x146 (0x468E6C, 0x468E7F) and never
   reads it back, every writer of that field stores 0..10, so the record's byte ^ 0xFF compares
   unequal, the block repaints and copies its fresh state back over the byte (0x468FEC), and nothing
   it shows is fabricated (engine/render.md, *The resource block*).
   WHY A REPAINT IS KNOWN, BY CONSTRUCTION: after our write, the byte changes only by the block's
   copy-back, which happens only when it repaints, or by another spoil of the same lever
   (tagpu_gui_hook.c's after a reset, between flips), which differs from the live byte and so is
   itself followed by a repaint before the next post-GUI observer: DrawGameScreen has no exit
   between its entry 0x468CF0 and the block's compare, and the observer 0x46A308 runs after it in
   the same call (DISASSEMBLED 2026-11-14). The recorder leaves the memo alone (replay-globals.tsv,
   `resmemo`, leave), and 0x4679A0 zeroes four of its dwords, never byte 0. So a byte that no longer
   reads as ours, seen from the observer, is a repaint that has happened. A view change between the
   write and the block only makes the block see a difference sooner.
   One coordinator for every drawer on the bar: two of them testing their own writes of the same
   byte would each take the other's later write as "no repaint yet" or, written for another record,
   as one that happened.

   THE WEATHER REPORT draws three lines as TADR's BlitWeatherReport places them (cinomce.cpp:867,
   tagged dcff5dd), in the viewed side's font, GUI colour 7 for the words and 10 for the values:
       Wind : +17 (6-30)
                            Game Time : 00:00:24
      Tidal : +25
   The values come from the engine's own economy (engine/weapons.md, the generator branch
   0x401550..0x4016C3): a unit def with windgenerator +0x1D2 above 0 makes windgenerator x the
   wind ratio main+0x37EDE (speed +0x37EDA / limit +0x37EC8, capped at 1.0, 0x490D3B..0x490D7B);
   else one with tidalgenerator +0x1D6 above 0 makes tidalgenerator x TidalStrength main+0x14267.
   The generator a line describes is the mod's basic one: the smallest windgenerator among unit defs
   with no tidalgenerator (Escalation's tidal generators carry windgenerator 10 beside tidal 1), and
   the smallest tidalgenerator. In OTA, ProTA, Total Mayhem, Twilight and Escalation that is
   ARMWIN's 30 and ARMTIDE's 1, TADR's numbers (MEASURED 2026-11-15 from the archives' FBIs). A mod
   with no such unit shows no such line: TA Zero shows the clock alone.

   THE BOUND: the unit defs are read only below the engine's own count, UNITINFOCount
   main+0x1438F, the count the def array main+0x1439B was allocated for; the viewed record's index
   below the ten records, its side below the five side slots; every float taken as data, finite
   and in range, before it is shown. THE LIFETIME: everything runs on the game thread inside
   DrawGameScreen, while the level is open and not closing (tagpu_packet_pub_level_open,
   tagpu_reclaim_level_closing), when the game thread owns the records, side data and unit defs. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tagpu_log.h"
#include "tagpu_packet_pub.h"
#include "tagpu_reclaim.h"
#include "tagpu_topbar.h"

#define TA_MAIN         0x00511DE8u
#define VA_TEXT         0x004C14F0u     /* DrawTextCustomFont stdcall(ctx, str, x, y, maxW), ret 0x14 */
#define VA_SETFONT      0x004C1420u     /* stdcall(font), NULL ignored                          */
#define VA_GETFONT      0x004C1440u
#define VA_WIDTH        0x004C1480u     /* stdcall(font, str), ret 8: the sum of glyph widths    */
#define VA_SETCOLOURS   0x004C13A0u     /* stdcall(fg, bg), -1 leaves one alone                 */
#define VA_GETFG        0x004C1400u
#define VA_GETBG        0x004C1410u
#define VA_GETCLEAR     0x004C13F0u     /* the text's transparent index                         */

#define OFF_VIEWED      0x2A43          /* the record the top bar shows (0x468E4B)              */
#define OFF_PLAYERS     0x1B63
#define PL_STRIDE       0x14B
#define PLAYERS         10
#define PL_INFO         0x27
#define PL_PANELINDEX   0x146           /* what the block writes to the memo's byte 0           */
#define PI_SIDE         0x95
#define OFF_MEMO        0x37E3F
#define OFF_GUI         0xDCB           /* the GUI colour table, one byte an entry              */
#define GUI_WORDS       7               /* TADR's grey: #ABABAB on the stock palette            */
#define GUI_VALUES      10              /* TADR's green: #53DF4F on the stock palette           */
#define OFF_SCREEN_W    0x37E1F
#define OFF_GAMETIME    0x38A47         /* i32, 30 a second                                     */
#define OFF_WIND_LIMIT  0x37EC8         /* u32, the speed a ratio of 1.0 is                    */
#define OFF_WIND_RATIO  0x37EDE         /* float, speed / limit capped at 1.0                   */
#define OFF_WIND_MIN    0x1425B         /* i32, the map's least speed                           */
#define OFF_WIND_MAX    0x1425F         /* i32, the map's greatest                              */
#define OFF_TIDAL       0x14267         /* float, TidalStrength                                 */
#define OFF_UDEFCOUNT   0x1438F         /* u32 UNITINFOCount                                    */
#define OFF_UDEFS       0x1439B         /* UnitDefStruct*, 0x249 a def                          */
#define UDEF_STRIDE     0x249
#define UD_WINDGEN      0x1D2           /* float windgenerator                                  */
#define UD_TIDALGEN     0x1D6           /* float tidalgenerator                                 */
#define OFF_SIDES       0x37F3D         /* RaceSideData, 0x232 a side, five slots (0x429870)    */
#define SIDE_STRIDE     0x232
#define SIDES           5
#define SD_ENERGYBAR    0x52            /* RECT {x1, y1, x2, y2}                                */
#define SD_METALBAR     0x72
#define SD_EPRODUCED    0xF2            /* the energy income's text rect                        */
#define SD_ECONSUMED    0x102           /* the energy expense's                                 */
#define SD_FONT         0x22E

#define BAR_ROWS        32              /* the top bar, engine rows 0..31                       */
#define GEN_MAX         1.0e6f          /* a generator value past this is not data              */

typedef void (__stdcall *text_fn)(void* ctx, const char* s, int x, int y, int maxw);
typedef void (__stdcall *setfont_fn)(void* font);
typedef void* (__stdcall *getptr_fn)(void);
typedef int  (__stdcall *getint_fn)(void);
typedef int  (__stdcall *width_fn)(const void* font, const char* s);
typedef void (__stdcall *setcolours_fn)(int fg, int bg);

/* ---- the repaint ---------------------------------------------------------------------------- */

static int s_poison = -1;               /* the byte we wrote and no repaint has replaced, or -1  */
static int s_fresh;                     /* this DrawGameScreen repainted after our ask          */

static char* main_ptr(void) { return *(char**)(uintptr_t)TA_MAIN; }

void tagpu_topbar_frame(void)
{
    char* ta = main_ptr();
    s_fresh = 0;
    if (s_poison < 0 || !ta) return;
    if ((unsigned char)ta[OFF_MEMO] != (unsigned char)s_poison) { s_poison = -1; s_fresh = 1; }
}

int tagpu_topbar_fresh(void) { return s_fresh; }

/* Only while the level is open and not closing, when the game thread owns the viewed record. */
void tagpu_topbar_repaint(void)
{
    char* ta = main_ptr();
    unsigned p;
    if (s_poison >= 0 || !ta || !tagpu_reclaim_level_tracked() || tagpu_reclaim_level_closing() ||
        !tagpu_packet_pub_level_open()) return;
    p = (unsigned char)ta[OFF_VIEWED];
    if (p >= PLAYERS) return;           /* the block forms no record either: nothing to repaint */
    s_poison = (unsigned char)(ta[OFF_PLAYERS + p * PL_STRIDE + PL_PANELINDEX] ^ 0xFF);
    ta[OFF_MEMO] = (char)s_poison;
}

/* ---- the weather report --------------------------------------------------------------------- */

/* What the report puts on the bar: three strings at their places, in one font and two colours.
   Compared whole, so a change of any part asks a repaint before the new one is drawn. */
typedef struct {
    void* font;
    int words, values;                  /* palette indexes                                      */
    int wind, tidal;                    /* the lines shown                                      */
    int xw, yw, xt, yt, xc, yc;         /* each line's start: wind, tidal, clock                */
    int xwv, xtv;                       /* where each value starts                              */
    char windText[40], windValue[12], tidalText[24], tidalValue[12], clock[32];
} Report;

static Report s_onBar;                  /* what is on the bar now, as far as we drew it          */
static int s_have;                      /* s_onBar holds something we drew                      */
static unsigned s_drawLevel = ~0u;      /* the level s_onBar was drawn in                       */
static int s_stale;                     /* the bar may hold an earlier level's report           */

static unsigned s_genLevel = ~0u;       /* the level the generators were found for              */
static float s_windGen, s_tidalGen;     /* the basic generators', 0 for none                   */

/* The mod's basic wind and tidal generators, once a level from its unit defs. */
static void find_generators(const char* ta)
{
    unsigned gen = tagpu_reclaim_level_gen(), n, i;
    const char* defs;
    if (gen == s_genLevel) return;
    s_genLevel = gen;
    s_windGen = s_tidalGen = 0.0f;
    memcpy(&n, ta + OFF_UDEFCOUNT, 4);
    memcpy(&defs, ta + OFF_UDEFS, sizeof defs);
    if (!defs) n = 0;
    for (i = 0; i < n; i++) {
        float w, t;
        memcpy(&w, defs + (size_t)i * UDEF_STRIDE + UD_WINDGEN, 4);
        memcpy(&t, defs + (size_t)i * UDEF_STRIDE + UD_TIDALGEN, 4);
        if (!(t > 0.0f) && w > 0.0f && w < GEN_MAX && (s_windGen == 0.0f || w < s_windGen)) s_windGen = w;
        if (t > 0.0f && t < GEN_MAX && (s_tidalGen == 0.0f || t < s_tidalGen)) s_tidalGen = t;
    }
    tagpu_logf("topbar: %u unit defs, wind generator %g, tidal generator %g", n, s_windGen, s_tidalGen);
}

/* A wind speed as the energy the basic generator makes at it, as 0x490D3B forms the ratio. */
static int wind_energy(int speed, unsigned limit)
{
    float r = limit ? (float)speed / (float)limit : (speed > 0 ? 1.0f : 0.0f);
    if (!(r > 0.0f)) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return (int)floorf(s_windGen * r + 0.5f);
}

static RECT side_rect(const char* side, int off)
{
    RECT r;
    memcpy(&r, side + off, sizeof r);
    return r;
}

/* The report the viewed side would show now; 0 when none is shown. */
static int compose(Report* rp)
{
    char* ta = main_ptr();
    const char* rec;
    const unsigned char* info;
    const char* side;
    const unsigned char* font;
    unsigned p, s, limit;
    int screenW, fh, xRef, yE, yM, x1, x2, y3, clockW, labelW, wPrefix, tPrefix, t, wmin, wmax;
    float ratio, tidal;
    width_fn width = (width_fn)(uintptr_t)VA_WIDTH;

    if (!ta || !tagpu_reclaim_level_tracked() || tagpu_reclaim_level_closing() ||
        !tagpu_packet_pub_level_open()) return 0;
    p = (unsigned char)ta[OFF_VIEWED];
    if (p >= PLAYERS) return 0;
    rec = ta + OFF_PLAYERS + p * PL_STRIDE;
    info = *(const unsigned char* const*)(rec + PL_INFO);
    if (!info) return 0;
    s = info[PI_SIDE];
    if (s >= SIDES) return 0;
    side = ta + OFF_SIDES + s * SIDE_STRIDE;
    font = *(const unsigned char* const*)(side + SD_FONT);
    if (!font) return 0;
    memcpy(&screenW, ta + OFF_SCREEN_W, 4);
    if (screenW <= 0 || screenW > 0x4000) return 0;
    find_generators(ta);

    memset(rp, 0, sizeof *rp);
    rp->font = (void*)font;
    rp->words = (unsigned char)ta[OFF_GUI + GUI_WORDS];
    rp->values = (unsigned char)ta[OFF_GUI + GUI_VALUES];
    rp->wind = s_windGen > 0.0f;
    rp->tidal = s_tidalGen > 0.0f;

    /* The rows: the energy income's and expense's, as TADR takes them, falling back to the bars
       for a side that leaves those rects zeroed; the columns 110 right of the longer bar. */
    {
        RECT eb = side_rect(side, SD_ENERGYBAR), mb = side_rect(side, SD_METALBAR);
        RECT ep = side_rect(side, SD_EPRODUCED), ec = side_rect(side, SD_ECONSUMED);
        xRef = eb.right > mb.right ? eb.right : mb.right;
        if (xRef <= 0) xRef = ep.right;
        if (xRef <= 0) xRef = 256;
        yE = ep.top > 0 ? ep.top : eb.top;
        yM = ec.top > 0 ? ec.top : mb.top;
    }
    fh = font[0];
    if (yE < 0) yE = 0;
    if (yM <= yE) yM = yE + 16;
    if (yE > BAR_ROWS - fh) yE = BAR_ROWS - fh;
    if (yM > BAR_ROWS - fh) yM = BAR_ROWS - fh;
    if (yE < 0) yE = 0;
    if (yM < 0) yM = 0;
    y3 = (yE + yM) / 2;

    /* Held inside the screen, the clock first, as TADR holds them. */
    clockW = width(font, "Game Time : 00:00:00") + 4;
    labelW = width(font, "Wind : +99 (-99-+99)") + 4;
    wPrefix = width(font, "Wind : ");
    tPrefix = width(font, "Tidal : ");
    x1 = 110 + xRef;
    x2 = x1 + 117;
    if (x2 > screenW - clockW) x2 = screenW - clockW;
    if (x1 > x2 - labelW) x1 = x2 - labelW;
    if (x1 < 0) x1 = 0;
    if (x2 < x1 + 1) x2 = x1 + 1;

    rp->xw = x1 - wPrefix; rp->yw = yE; rp->xwv = x1;
    rp->xt = x1 - tPrefix; rp->yt = yM; rp->xtv = x1;
    rp->xc = x2;           rp->yc = y3;

    if (rp->wind) {
        memcpy(&limit, ta + OFF_WIND_LIMIT, 4);
        memcpy(&ratio, ta + OFF_WIND_RATIO, 4);
        memcpy(&wmin, ta + OFF_WIND_MIN, 4);
        memcpy(&wmax, ta + OFF_WIND_MAX, 4);
        if (!isfinite(ratio) || ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        snprintf(rp->windValue, sizeof rp->windValue, "+%d", (int)floorf(s_windGen * ratio + 0.5f));
        snprintf(rp->windText, sizeof rp->windText, "Wind : %s (%d-%d)", rp->windValue,
                 wind_energy(wmin, limit), wind_energy(wmax, limit));
    }
    if (rp->tidal) {
        memcpy(&tidal, ta + OFF_TIDAL, 4);
        if (!isfinite(tidal) || tidal < 0.0f || tidal > GEN_MAX) tidal = 0.0f;
        snprintf(rp->tidalValue, sizeof rp->tidalValue, "+%d", (int)(tidal * s_tidalGen));
        snprintf(rp->tidalText, sizeof rp->tidalText, "Tidal : ");
    }
    memcpy(&t, ta + OFF_GAMETIME, 4);
    if (t < 0) t = 0;
    t /= 30;
    snprintf(rp->clock, sizeof rp->clock, "Game Time : %02d:%02d:%02d", t / 3600 % 100, t / 60 % 60, t % 60);
    return 1;
}

static void draw(const Report* r)
{
    text_fn text = (text_fn)(uintptr_t)VA_TEXT;
    setcolours_fn colours = (setcolours_fn)(uintptr_t)VA_SETCOLOURS;
    void* oldfont = ((getptr_fn)(uintptr_t)VA_GETFONT)();
    int fg = ((getint_fn)(uintptr_t)VA_GETFG)(), bg = ((getint_fn)(uintptr_t)VA_GETBG)();
    int clear = ((getint_fn)(uintptr_t)VA_GETCLEAR)();
    ((setfont_fn)(uintptr_t)VA_SETFONT)(r->font);
    /* the whole line in the words' colour, then the value over its own glyphs */
    if (r->wind) {
        colours(r->words, clear);
        text(NULL, r->windText, r->xw, r->yw, -1);
        colours(r->values, clear);
        text(NULL, r->windValue, r->xwv, r->yw, -1);
    }
    if (r->tidal) {
        colours(r->words, clear);
        text(NULL, r->tidalText, r->xt, r->yt, -1);
        colours(r->values, clear);
        text(NULL, r->tidalValue, r->xtv, r->yt, -1);
    }
    colours(r->words, clear);
    text(NULL, r->clock, r->xc, r->yc, -1);
    ((setfont_fn)(uintptr_t)VA_SETFONT)(oldfont);
    colours(fg, bg);
}

/* Draws what is on the bar again each frame, which changes no pixel; a different report waits for
   a repaint, the old one drawn meanwhile, so no frame shows the two over each other. A new level's
   bar is not known to be clean of the last level's report (the level load's reset 0x4679A0, called
   at 0x491893, zeroes only four of the memo's values), so nothing is drawn in a level until a
   repaint asked in it has been seen. */
void tagpu_topbar_draw(void)
{
    Report want;
    int have;
    unsigned level = tagpu_reclaim_level_gen();
    if (level != s_drawLevel) { s_drawLevel = level; s_have = 0; s_stale = 1; }
    if (tagpu_topbar_fresh()) { s_have = 0; s_stale = 0; }
    if (s_stale) { tagpu_topbar_repaint(); return; }
    have = compose(&want);
    if (have && s_have && !memcmp(&want, &s_onBar, sizeof want)) { draw(&s_onBar); return; }
    if (!s_have) {
        if (!have) return;
        s_onBar = want;
        s_have = 1;
        draw(&s_onBar);
        return;
    }
    tagpu_topbar_repaint();
    if (have) draw(&s_onBar);
}
