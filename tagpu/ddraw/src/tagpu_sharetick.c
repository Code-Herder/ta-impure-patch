/* tagpu_sharetick -- see tagpu_sharetick.h and research/notes/modules/share.md.

   The share level of a resource is what the player keeps: above it, the engine's auto-share
   0x457D30 gives to an ally every 60 ticks, in a network game, while the resource's share bit is
   set. Stock sets the two through console commands. Here the tick on the resource bar is the
   control: dragged, or set by a press anywhere on the bar's strip. The far right end is sharing
   off (the bit clear); anywhere else is sharing on (the bit set) at the level under the pointer.

   WHAT IS WRITTEN, all on the controlled record, all on the game thread:
   - the level, player +0xE4 (metal) or +0xE8 (energy), as SetShareMetal 0x419340 and
     SetShareEnergy 0x419400 write it: a float, at most the storage. Never sent to the other peers
     by the engine (DISASSEMBLED 2026-11-14: neither handler sends, and no message carries the
     field). Sharing off writes 0. The engine draws its own tick while the level is not 0 and the
     stored value is above it, whatever the bit (0x469133..0x4691B2 and 0x469391..0x469492), so a
     level set while the bit is clear (the console's SetShareMetal) is drawn here grey at that
     column, over the engine's.
   - at the first in-play draw of each level, the bits cleared, then the broadcast if one was set,
     so every level starts with sharing off whatever path led to it. No field write of the engine
     clears bits 2 and 4 (each one outside the console commands touches bit 0 alone: 0x4280F3,
     0x451334, 0x45193A, 0x46460F, DISASSEMBLED 2026-11-14); a surrender, the main menu and a new
     battle room did leave them clear (MEASURED 2026-11-14), by a write not mapped.
   - the bit, PlayerInfo +0x97 & 2 (metal) or & 4 (energy), as ShareMetal 0x418CD0 and ShareEnergy
     0x418D90 toggle it, followed as there by 0x450F90, which broadcasts every local record's
     PlayerInfo to the other peers as message 0x20. Only on a change of the bit: a drag that stays
     on writes the level alone. The handlers are not called because each prints a chat line, and a
     drag would print one a frame.

   A TICK MOVES ONLY OVER A FRESH BAR. DrawGameScreen repaints its resource block only when a
   33-byte memo of what the block shows, main+0x37E3F, differs from the live values (repz cmpsb at
   0x468FD9, je 0x469610); a full, steady bar is never repainted, so a tick drawn at a new column
   would leave the old one on screen beside it (MEASURED 2026-11-14: two ticks, the old at 750, on a
   full metal bar). When the column changes, the tick stays where it is drawn for one frame and the
   memo's byte 0 is spoiled, as tagpu_gui_hook.c's hud_invalidate spoils it: the block writes that
   byte with the viewed record's +0x146 (0x468E7F) and never reads it back, every writer of that
   field stores 0..10, so the record's byte ^ 0xFF compares unequal and fabricates nothing shown.
   ORDERING: DrawGameScreen has no jump past the block and no return between its entry 0x468CF0
   and the block's 0x468E3F (DISASSEMBLED 2026-11-14), and our draw runs later in the same call, at
   0x46A308; so the next draw that runs follows a compare that saw the spoiled byte and repainted.
   The tick moves when the byte no longer reads as spoiled, which is that repaint having run. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "dd.h"
#include "tagpu_log.h"
#include "tagpu_hud.h"
#include "tagpu_packet_pub.h"
#include "tagpu_reclaim.h"
#include "tagpu_wiki.h"
#include "tagpu_replay.h"
#include "tagpu_sharetick.h"

#define TA_MAIN         0x00511DE8u
#define VA_DRAWBAR      0x004BF6F0u     /* stdcall(ctx, RECT*, colour), ret 0xC; ctx NULL: the back
                                           buffer (0x4BF70E). Both ends inclusive (0x4CCDEA) */
#define VA_TEXT         0x004C14F0u     /* DrawTextCustomFont stdcall(ctx, str, x, y, maxW), ret 0x14 */
#define VA_SETFONT      0x004C1420u     /* stdcall(font), NULL ignored                          */
#define VA_GETFONT      0x004C1440u
#define VA_SETCOLOURS   0x004C13A0u     /* stdcall(fg, bg), -1 leaves one alone                 */
#define VA_GETFG        0x004C1400u
#define VA_GETBG        0x004C1410u
#define VA_GETCLEAR     0x004C13F0u     /* the text's transparent index                         */
#define VA_SENDINFO     0x00450F90u     /* void(void): every local record's PlayerInfo as 0x20  */

#define OFF_NETWORK     0x2A44          /* bit 0: a network game (the share handlers' own test)  */
#define OFF_CONTROLLED  0x2A42
#define OFF_VIEWED      0x2A43          /* the record the top bar shows (0x468E4B)              */
#define OFF_PLAYERS     0x1B63
#define PL_STRIDE       0x14B
#define PLAYERS         10
#define OFF_GUI         0xDCB           /* the GUI colour table, one byte an entry              */
#define GUI_EXPENSE     0xC             /* the engine's tick and expense colour                 */
#define OFF_MEMO        0x37E3F         /* the resource block's memo; byte 0 never read back    */
#define PL_PANELINDEX   0x146           /* what the block writes to the memo's byte 0 (0x468E6C)  */
#define OFF_SIDES       0x37F3D         /* RaceSideData, 0x232 a side, five slots (0x429870)    */
#define SIDE_STRIDE     0x232
#define SIDES           5
#define SD_ENERGYBAR    0x52            /* RECT {x1, y1, x2, y2}, the engine's fill's own rect  */
#define SD_METALBAR     0x72
#define SD_FONT         0x22E           /* the side's font, SetFont at 0x46901D                 */

#define PL_ACTIVE       0x00
#define PL_INFO         0x27
#define PL_NAME         0x2B            /* the name, 0x1E bytes                                 */
#define NAME_LEN        0x1E
#define PL_CONTROLLER   0x73            /* as this peer sees the record: 3 remote               */
#define PL_ALLY         0x108           /* u8[10]: 1 where this player allies player j          */
#define PL_STORED_E     0x8C            /* raw stored values, the engine tick's test            */
#define PL_STORED_M     0x98
#define PL_STORAGE_E    0xA4
#define PL_STORAGE_M    0xA8
#define PL_LEVEL_M      0xE4
#define PL_LEVEL_E      0xE8
#define PI_SIDE         0x95
#define PI_LOGO         0x96            /* the colour: a frame of the engine's colour sequence  */
#define PI_SHARE        0x97            /* u16, as the handlers access it                       */
#define PI_WATCH        0x9B            /* bit 0x40: a watcher                                  */

#define BAR_ROWS        32              /* the top bar, engine rows 0..31                       */
#define TOPBAR_X0       129             /* the top bar's first column, 0x469050                */
#define REACH_X         4               /* the strip past each end of a bar                     */
#define REACH_Y         9               /* and above and below it                               */
#define STEP            50.0f           /* a level snaps to this                                */
/* PALETTE.PAL's own entries, MEASURED on the stock palette: 0 is (0,0,0), 24 is (111,51,59) and
   89 is (107,107,107) */
#define C_EDGE          0
#define C_BELOW         24              /* sharing, holding no more than the level              */
#define C_OFF           89              /* not sharing                                          */

typedef void (__stdcall *drawbar_fn)(void* ctx, const RECT* r, int colour);
typedef void (__stdcall *text_fn)(void* ctx, const char* s, int x, int y, int maxw);
typedef void (__stdcall *setfont_fn)(void* font);
typedef void* (__stdcall *getptr_fn)(void);
typedef int  (__stdcall *getint_fn)(void);
typedef void (__stdcall *setcolours_fn)(int fg, int bg);
typedef void (__cdecl *sendinfo_fn)(void);

enum { METAL, ENERGY };
static const char* const NAME[2] = { "metal", "energy" };
static const unsigned BIT[2] = { 2, 4 };

/* The controlled player, when it may set its levels from this bar: a network game, the bar
   showing that player, an active record with its PlayerInfo, not a watcher, a side the five slots
   hold. */
typedef struct {
    char* ta;
    char* rec;
    unsigned char* info;
    const char* side;
} Me;

/* One bar: its rect on the engine's surface, its storage, stored value and level, and the bit. */
typedef struct {
    RECT r;
    float storage, stored, level;
    int on;
} Bar;

static int s_drag = -1;                 /* the bar a held press is setting, -1 none            */
static int s_shown[2] = { -1, -1 };     /* the column each tick is on screen at, -1 none        */
static int s_spoil = -1;                /* the memo byte we wrote and the block has not replaced */
static unsigned s_level = ~0u;          /* the level generation whose bits have been cleared   */

/* The controlled player in play: a network game, a level the engine has drawn at least once and
   is not tearing down, an active record with its PlayerInfo, not a watcher. THE LIFETIME: the
   network bit is set from the front end on (0x426D34 and its siblings), and during the load the
   loader thread writes the same record and PlayerInfo (0x46478F, 0x497BFF, 0x497C06) while the
   game thread pumps the window procedure; the level gates are the game thread's own, set at the
   level's first in-play draw and raised before the teardown frees (tagpu_packet_pub.h,
   tagpu_reclaim.h), so nothing here runs in either window. Without the teardown wrap there is no
   ordering, and the tick is not offered. */
static int player(Me* m)
{
    char* ta = *(char**)(uintptr_t)TA_MAIN;
    unsigned c;
    if (!tagpu_reclaim_level_tracked() || tagpu_reclaim_level_closing() ||
        !tagpu_packet_pub_level_open()) return 0;
    if (!ta || !((unsigned char)ta[OFF_NETWORK] & 1)) return 0;
    c = (unsigned char)ta[OFF_CONTROLLED];
    if (c >= PLAYERS) return 0;
    m->ta = ta;
    m->rec = ta + OFF_PLAYERS + c * PL_STRIDE;
    if (!*(const unsigned*)(m->rec + PL_ACTIVE)) return 0;
    m->info = *(unsigned char**)(m->rec + PL_INFO);
    return m->info && !(m->info[PI_WATCH] & 0x40);
}

/* THE ALLIES: every other record that is active, remote, has its PlayerInfo, is not a watcher and
   is allied BOTH WAYS -- the controlled player's row marks it and its row marks the controlled
   player. The rows are one-way, and a peer learns another's row toward it from that peer's 0x23
   (research/notes/shared-resources-plan.md, *The engine*), so the two rows read here are the two
   this peer is told of. Read under player()'s lifetime; every index is bounded by the ten
   records, and a value that is not finite is shown as 0. */
int tagpu_share_allies(TAGPU_SHARE_ALLY* out, int cap)
{
    Me m;
    unsigned c, j;
    int n = 0;
    if (!player(&m)) return 0;
    c = (unsigned char)m.ta[OFF_CONTROLLED];
    for (j = 0; j < PLAYERS; j++) {
        const char* rec = m.ta + OFF_PLAYERS + j * PL_STRIDE;
        const unsigned char* info;
        if (j == c || !*(const unsigned*)(rec + PL_ACTIVE) || (unsigned char)rec[PL_CONTROLLER] != 3) continue;
        if (m.rec[PL_ALLY + j] != 1 || rec[PL_ALLY + c] != 1) continue;
        info = *(const unsigned char* const*)(rec + PL_INFO);
        if (!info || (info[PI_WATCH] & 0x40)) continue;
        if (out && n < cap) {
            TAGPU_SHARE_ALLY* a = &out[n];
            float v[4];
            int k;
            a->index = j;
            a->colour = tagpu_replay_logo_colour(info[PI_LOGO]);
            memcpy(a->name, rec + PL_NAME, NAME_LEN);
            a->name[NAME_LEN] = 0;
            memcpy(&v[0], rec + PL_STORED_M, 4);
            memcpy(&v[1], rec + PL_STORAGE_M, 4);
            memcpy(&v[2], rec + PL_STORED_E, 4);
            memcpy(&v[3], rec + PL_STORAGE_E, 4);
            for (k = 0; k < 4; k++) if (!isfinite(v[k])) v[k] = 0.0f;
            a->metal = v[0]; a->metal_max = v[1]; a->energy = v[2]; a->energy_max = v[3];
        }
        n++;
    }
    return n;
}

static int me(Me* m)
{
    unsigned s;
    if (!player(m)) return 0;
    if ((unsigned char)m->ta[OFF_CONTROLLED] != (unsigned char)m->ta[OFF_VIEWED]) return 0;
    s = m->info[PI_SIDE];
    if (s >= SIDES) return 0;
    m->side = m->ta + OFF_SIDES + s * SIDE_STRIDE;
    return 1;
}

/* A bar the tick can sit on: a rect inside the top bar with some length, and a storage the
   engine divides by (its own fill skips a storage of 0, 0x4690BD and 0x4690ED). */
static int bar(const Me* m, int k, Bar* b)
{
    memcpy(&b->r, m->side + (k == METAL ? SD_METALBAR : SD_ENERGYBAR), sizeof b->r);
    if (b->r.left < 0 || b->r.right - b->r.left < 2 || b->r.right > 4096 ||
        b->r.top < 0 || b->r.bottom < b->r.top || b->r.bottom >= BAR_ROWS) return 0;
    memcpy(&b->storage, m->rec + (k == METAL ? PL_STORAGE_M : PL_STORAGE_E), sizeof b->storage);
    memcpy(&b->stored, m->rec + (k == METAL ? PL_STORED_M : PL_STORED_E), sizeof b->stored);
    memcpy(&b->level, m->rec + (k == METAL ? PL_LEVEL_M : PL_LEVEL_E), sizeof b->level);
    if (!isfinite(b->storage) || b->storage <= 0.0f || !isfinite(b->level)) return 0;
    b->on = (*(const unsigned short*)(m->info + PI_SHARE) & BIT[k]) != 0;
    return 1;
}

/* The tick's left column: the engine's own formula, ftol(x1 + (x2 - x1) * level / storage)
   (0x46916D..0x46918E), held inside the bar; the right end while sharing is off with no level. */
static int tick_x(const Bar* b)
{
    float f;
    if (!b->on && b->level == 0.0f) return b->r.right;
    f = (float)b->r.left + (float)(b->r.right - b->r.left) * b->level / b->storage;
    if (!(f >= (float)b->r.left)) return b->r.left;
    if (f > (float)b->r.right) return b->r.right;
    return (int)f;
}

static int on_strip(const Bar* b, int gx, int gy)
{
    int top = b->r.top - REACH_Y, bottom = b->r.bottom + REACH_Y;
    if (top < 0) top = 0;
    if (bottom > BAR_ROWS - 1) bottom = BAR_ROWS - 1;
    return gx >= b->r.left - REACH_X && gx <= b->r.right + REACH_X && gy >= top && gy <= bottom;
}

/* The level the pointer's column names, snapped to STEP; past the last step short of the storage,
   sharing off. */
static void set_from_x(const Me* m, int k, const Bar* b, int gx)
{
    float f = (float)(gx - b->r.left) / (float)(b->r.right - b->r.left), level;
    unsigned short* bits = (unsigned short*)(m->info + PI_SHARE);
    int on;
    if (f < 0.0f) f = 0.0f;
    level = floorf(f * b->storage / STEP + 0.5f) * STEP;
    on = gx < b->r.right && level < b->storage;
    if (!on) level = 0.0f;
    memcpy(m->rec + (k == METAL ? PL_LEVEL_M : PL_LEVEL_E), &level, sizeof level);
    if (on != ((*bits & BIT[k]) != 0)) {
        *bits ^= (unsigned short)BIT[k];
        ((sendinfo_fn)(uintptr_t)VA_SENDINFO)();
        tagpu_logf("sharetick: %s sharing %s, level %.0f", NAME[k], on ? "on" : "off", level);
    }
}

static void fill(int x1, int y1, int x2, int y2, int colour)
{
    RECT r = { x1, y1, x2, y2 };
    ((drawbar_fn)(uintptr_t)VA_DRAWBAR)(NULL, &r, colour);
}

static void draw_tick(const Me* m, const Bar* b, int x)
{
    int y1 = b->r.top - 2, y2 = b->r.bottom + 2, c;
    if (y1 < 0) y1 = 0;
    if (y2 > BAR_ROWS - 1) y2 = BAR_ROWS - 1;
    c = !b->on ? C_OFF
      : b->stored > b->level ? (unsigned char)m->ta[OFF_GUI + GUI_EXPENSE]
      : C_BELOW;
    fill(x - 1, y1, x - 1, y2, C_EDGE);
    fill(x + 3, y1, x + 3, y2, C_EDGE);
    fill(x, y1, x + 2, y2, c);
}

/* The levels, under the bar, in the side's font and the engine's tick colour. The world region
   is not magnified by HUD scale but moved (tagpu_hud.c), so the line is placed where the
   magnified bar's left end lands on screen and moved back along that vector, as the Config
   panel is (tagpu_menu.c, panel_rect). */
static void draw_levels(const Me* m, const Bar* bars, const int* have)
{
    int q8 = 256, bh = BAR_ROWS, dx = 0, dy = 0, k, x, y;
    void* oldfont = ((getptr_fn)(uintptr_t)VA_GETFONT)();
    void* font = *(void**)(m->side + SD_FONT);
    int fg = ((getint_fn)(uintptr_t)VA_GETFG)(), bg = ((getint_fn)(uintptr_t)VA_GETBG)();
    char s[64];
    tagpu_hud_live(NULL, &bh, &q8);
    tagpu_hud_shift(&dx, &dy);
    ((setfont_fn)(uintptr_t)VA_SETFONT)(font);
    ((setcolours_fn)(uintptr_t)VA_SETCOLOURS)((unsigned char)m->ta[OFF_GUI + GUI_EXPENSE],
                                               ((getint_fn)(uintptr_t)VA_GETCLEAR)());
    for (k = METAL; k <= ENERGY; k++) {
        const Bar* b = &bars[k];
        if (!have[k]) continue;
        if (!b->on) snprintf(s, sizeof s, "shares no %s", NAME[k]);
        else if (b->level <= 0.0f) snprintf(s, sizeof s, "shares all %s", NAME[k]);
        else snprintf(s, sizeof s, "shares %s above %.0f", NAME[k], b->level);
        x = b->r.left * q8 / 256 - dx;
        y = bh - dy + 3;
        if (x < 0) x = 0;
        ((text_fn)(uintptr_t)VA_TEXT)(NULL, s, x, y, -1);
    }
    ((setfont_fn)(uintptr_t)VA_SETFONT)(oldfont);
    ((setcolours_fn)(uintptr_t)VA_SETCOLOURS)(fg, bg);
}

/* Sharing starts off in every level: the first in-play draw of a level clears both bits. */
static void new_level(void)
{
    Me m;
    unsigned gen = tagpu_reclaim_level_gen();
    unsigned short* bits;
    if (gen == s_level || !player(&m)) return;
    s_level = gen;
    bits = (unsigned short*)(m.info + PI_SHARE);
    if (!(*bits & (BIT[METAL] | BIT[ENERGY]))) return;
    *bits &= (unsigned short)~(BIT[METAL] | BIT[ENERGY]);
    ((sendinfo_fn)(uintptr_t)VA_SENDINFO)();
    tagpu_logf("sharetick: a new level, sharing off");
}

/* Spoils the memo's byte 0 so the next DrawGameScreen repaints the resource block. */
static void repaint_bars(const Me* m)
{
    unsigned char* memo = (unsigned char*)m->ta + OFF_MEMO;
    s_spoil = (unsigned char)(m->rec[PL_PANELINDEX] ^ 0xFF);
    *memo = (unsigned char)s_spoil;
}

void tagpu_sharetick_draw(void)
{
    Me m;
    Bar bars[2];
    int have[2], x[2], k, cx, cy, moved = 0, fresh = 0;
    new_level();
    /* the wiki takes every input message while it is open, a release included */
    if (tagpu_wiki_holds_input()) s_drag = -1;
    if (!me(&m)) { s_drag = -1; s_shown[METAL] = s_shown[ENERGY] = -1; return; }
    if (s_spoil >= 0 && (unsigned char)m.ta[OFF_MEMO] != (unsigned char)s_spoil) {
        s_spoil = -1;                   /* this frame's block has repainted the bars */
        fresh = 1;
    }
    cx = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
    cy = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);
    for (k = METAL; k <= ENERGY; k++) have[k] = bar(&m, k, &bars[k]);
    if (s_drag >= 0 && have[s_drag]) {
        set_from_x(&m, s_drag, &bars[s_drag], cx);
        have[s_drag] = bar(&m, s_drag, &bars[s_drag]);
    }
    for (k = METAL; k <= ENERGY; k++) {
        if (!have[k]) { s_shown[k] = -1; continue; }
        x[k] = tick_x(&bars[k]);
        if (s_shown[k] >= 0 && x[k] != s_shown[k]) moved = 1;
    }
    if (moved && !fresh && s_spoil < 0) repaint_bars(&m);
    for (k = METAL; k <= ENERGY; k++) {
        if (!have[k]) continue;
        if (fresh || s_shown[k] < 0) s_shown[k] = x[k];
        draw_tick(&m, &bars[k], s_shown[k]);
    }
    if (s_drag >= 0 || (cy >= 0 && cy < BAR_ROWS && cx >= TOPBAR_X0)) draw_levels(&m, bars, have);
}

int tagpu_sharetick_owns(int gx, int gy)
{
    Me m;
    Bar b;
    int k;
    if (!me(&m)) return 0;
    for (k = METAL; k <= ENERGY; k++) if (bar(&m, k, &b) && on_strip(&b, gx, gy)) return 1;
    return 0;
}

int tagpu_sharetick_click(int gx, int gy, int down)
{
    Me m;
    Bar b;
    int k;
    if (!down) {
        int was = s_drag >= 0;
        s_drag = -1;
        return was;
    }
    /* a press decides afresh: one whose release never came must not keep a drag going */
    s_drag = -1;
    if (!me(&m)) return 0;
    for (k = METAL; k <= ENERGY; k++) {
        if (!bar(&m, k, &b) || !on_strip(&b, gx, gy)) continue;
        s_drag = k;
        set_from_x(&m, k, &b, gx);
        return 1;
    }
    return 0;
}

int tagpu_sharetick_dragging(void)
{
    return s_drag >= 0;
}

void tagpu_sharetick_cancel(void)
{
    s_drag = -1;
}
