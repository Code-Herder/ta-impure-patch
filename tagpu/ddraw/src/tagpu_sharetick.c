/* tagpu_sharetick -- see tagpu_sharetick.h and research/notes/modules/share.md.

   The share level of a resource is what the player keeps: above it, the engine's auto-share
   0x457D30 gives to an ally every 60 ticks while the resource's share bit is set, to an AI ally as
   to a human one and in a skirmish as in a network game (tagpu_patches.c,
   ai_autoshare). Stock sets the two through console commands. Here the tick on the resource bar is the
   control: dragged, or set by a press anywhere on the bar's strip. The far right end is sharing
   off (the bit clear); anywhere else is sharing on (the bit set) at the level under the pointer.

   WHAT IS WRITTEN, all on the game thread, on the controlled record but for the give's credit:
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
   - a give asked by the HUD's give panel (tagpu_hudinput.c), through the engine's own transfers
     0x464C60 (metal) and 0x464B30 (energy) with send = 1, as the SHARE dialog's OK calls them:
     the giver's stored value and its +0xEC record debited, its own copy of the recipient's +0xEC
     record credited, and the 0x16 sent that credits the recipient on its peer. Bounded in
     give_run.

   A TICK MOVES ONLY OVER A FRESH BAR. DrawGameScreen repaints its resource block only when a
   33-byte memo of what the block shows, main+0x37E3F, differs from the live values; a full, steady
   bar is never repainted, so a tick drawn at a new column would leave the old one on screen beside
   it (MEASURED 2026-11-14: two ticks, the old at 750, on a full metal bar). When the column
   changes, the tick stays where it is drawn and a repaint is asked (tagpu_topbar_repaint); it moves
   in the frame whose block has repainted (tagpu_topbar_fresh). Why that frame is known:
   tagpu_topbar.c. */

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
#include "tagpu_hudinput.h"
#include "tagpu_menu.h"
#include "tagpu_recorder.h"
#include "tagpu_puppet.h"
#include "tagpu_sharetick.h"
#include "tagpu_topbar.h"

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
#define VA_GIVE_METAL   0x00464C60u     /* stdcall(from, to, float amount, send), ret 0x10      */
#define VA_GIVE_ENERGY  0x00464B30u     /* the same, energy                                     */

#define OFF_CONTROLLED  0x2A42
#define OFF_VIEWED      0x2A43          /* the record the top bar shows (0x468E4B)              */
#define OFF_PLAYERS     0x1B63
#define PL_STRIDE       0x14B
#define PLAYERS         10
#define OFF_GUI         0xDCB           /* the GUI colour table, one byte an entry              */
#define GUI_EXPENSE     0xC             /* the engine's tick and expense colour                 */
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
#define PL_CONTROLLER   0x73            /* as this peer sees the record: 1 the local human, 2 a local
                                           AI, 3 remote                                         */
#define LOCAL_AI        2
#define REMOTE          3
#define PL_ALLY         0x108           /* u8[10]: 1 where this player allies player j          */
#define PL_ALLIED_BY    0x113           /* u8[10]: this peer's mirror, 1 where player j allies this one */
#define PL_INCOME_E     0x90            /* the economy pass's production and expense, per pass   */
#define PL_SPEND_E      0x94
#define PL_INCOME_M     0x9C
#define PL_SPEND_M      0xA0
#define PL_STORED_E     0x8C            /* raw stored values, the engine tick's test            */
#define PL_STORED_M     0x98
#define PL_STORAGE_E    0xA4
#define PL_STORAGE_M    0xA8
#define PL_NETID        0x04            /* the DirectPlay ID: the contribution channel's origin  */
#define PL_MADE         0x140           /* u32: units created                                   */
#define PL_UNITS        0x144           /* u16: units alive (0x486187, 0x486DFC)                */
#define PL_SELF         0x146           /* the record's own index; 10 for none                  */
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
typedef void (__stdcall *give_fn)(unsigned from, unsigned to, float amount, int send);

enum { METAL, ENERGY };
static const char* const NAME[2] = { "metal", "energy" };
static const unsigned BIT[2] = { 2, 4 };

/* The controlled player, when it may set its levels from this bar: the bar showing that player,
   an active record with its PlayerInfo, not a watcher, a side the five slots hold. */
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
static unsigned s_level = ~0u;          /* the level generation whose bits have been cleared   */
static int s_giveTo = -1;               /* the give asked and not yet run: a record index      */
static unsigned s_giveGen;              /* the level generation it was asked in                */
static float s_giveAmt[2];

/* The controlled player in play, in a skirmish or a network game: a level the engine has drawn
   at least once and is not tearing down, an active record with its PlayerInfo, not a watcher. A
   playback's viewer sits in record 10 (tagpu_replay.c, THE VIEWER'S SEAT), which the bound below
   refuses. THE LIFETIME: during the load the loader thread writes the records and PlayerInfo
   (0x46478F, 0x497BFF, 0x497C06) while the game thread pumps the window procedure; the level
   gates are the game thread's own, set at the level's first in-play draw and raised before the
   teardown frees (tagpu_packet_pub.h, tagpu_reclaim.h), so nothing here runs in either window.
   Without the teardown wrap there is no ordering, and the tick is not offered. */
static int player(Me* m)
{
    char* ta = *(char**)(uintptr_t)TA_MAIN;
    unsigned c;
    if (!tagpu_reclaim_level_tracked() || tagpu_reclaim_level_closing() ||
        !tagpu_packet_pub_level_open()) return 0;
    if (!ta) return 0;
    c = (unsigned char)ta[OFF_CONTROLLED];
    if (c >= PLAYERS) return 0;
    m->ta = ta;
    m->rec = ta + OFF_PLAYERS + c * PL_STRIDE;
    if (!*(const unsigned*)(m->rec + PL_ACTIVE)) return 0;
    m->info = *(unsigned char**)(m->rec + PL_INFO);
    return m->info && !(m->info[PI_WATCH] & 0x40);
}

/* The transfer's own send gate, 0x457050 (metal) and 0x456EE0 (energy), which tests both records
   only after the transfer has debited the giver and credited its own copy of the recipient: a
   record whose own index +0x146 is 10, or that has created units (+0x140) and has none alive
   (+0x144), sends nothing (energy 0x456F43..0x456FAB, metal 0x4570B3..0x457119, DISASSEMBLED). */
static int sendable(const char* rec)
{
    unsigned made;
    memcpy(&made, rec + PL_MADE, 4);
    return (unsigned char)rec[PL_SELF] != PLAYERS && !(*(const unsigned short*)(rec + PL_UNITS) == 0 && made != 0);
}

/* A local AI's economy, its own record's: the economy pass runs for it on this peer (type 2,
   0x465539), so its stored values, storages, rates and share levels are exact here. The same
   bound as the channel's: a value that is not finite or is negative leaves no rates. */
static void local_economy(const char* rec, TAGPU_SHARE_ALLY* a)
{
    const unsigned char* info = *(const unsigned char* const*)(rec + PL_INFO);
    float v[4], lv[2];
    unsigned short bits = 0;
    int k;
    a->rates = 0;
    a->metal_in = a->metal_out = a->energy_in = a->energy_out = 0;
    a->share[0] = a->share[1] = -1;
    memcpy(&v[0], rec + PL_INCOME_M, 4);
    memcpy(&v[1], rec + PL_SPEND_M, 4);
    memcpy(&v[2], rec + PL_INCOME_E, 4);
    memcpy(&v[3], rec + PL_SPEND_E, 4);
    for (k = 0; k < 4; k++) if (!(isfinite(v[k]) && v[k] >= 0)) return;
    a->rates = 1;
    a->metal_in = v[0]; a->metal_out = v[1]; a->energy_in = v[2]; a->energy_out = v[3];
    memcpy(&lv[0], rec + PL_LEVEL_M, 4);
    memcpy(&lv[1], rec + PL_LEVEL_E, 4);
    if (info) memcpy(&bits, info + PI_SHARE, 2);
    if ((bits & BIT[METAL]) && isfinite(lv[0]) && lv[0] >= 0 && lv[0] <= a->metal_max) a->share[METAL] = lv[0];
    if ((bits & BIT[ENERGY]) && isfinite(lv[1]) && lv[1] >= 0 && lv[1] <= a->energy_max) a->share[ENERGY] = lv[1];
}

/* An ally's economy from the contribution channel, when it runs: its stored values, storages,
   rates and share levels in place of the record's. THE BOUND: the image's numbers are DATA from
   another peer, shown and never fed to the simulation. They are taken only when every stored
   value, storage and rate is finite and not negative; a share level is drawn only while its bit
   is set and it lies in 0..that image's storage. Anything else leaves the record's stored values
   and no rates. */
static void channel_economy(const char* rec, TAGPU_SHARE_ALLY* a)
{
    TAGPU_REC_ECON e;
    unsigned native;
    int k;
    a->rates = 0;
    a->metal_in = a->metal_out = a->energy_in = a->energy_out = 0;
    a->share[0] = a->share[1] = -1;
    memcpy(&native, rec + PL_NETID, 4);
    if (!tagpu_rec_mp_economy(native, &e)) return;
    for (k = 0; k < 2; k++)
        if (!(isfinite(e.stored[k]) && e.stored[k] >= 0 && isfinite(e.storage[k]) && e.storage[k] >= 0 &&
              isfinite(e.in[k]) && e.in[k] >= 0 && isfinite(e.out[k]) && e.out[k] >= 0)) return;
    a->rates = 1;
    a->metal = e.stored[0]; a->metal_max = e.storage[0]; a->energy = e.stored[1]; a->energy_max = e.storage[1];
    a->metal_in = e.in[0]; a->metal_out = e.out[0]; a->energy_in = e.in[1]; a->energy_out = e.out[1];
    for (k = 0; k < 2; k++)
        if (e.share[k] && isfinite(e.level[k]) && e.level[k] >= 0 && e.level[k] <= e.storage[k]) a->share[k] = e.level[k];
}

/* THE ALLIES: every other record that is active, a local AI or remote (a human or an AI another
   peer hosts), has its PlayerInfo, is not a watcher and is allied BOTH WAYS: the controlled
   player's row marks it, and it marks the controlled player -- in this peer's mirror +0x113, the
   engine's own "j allies me" (the victory test 0x490080 reads the row and the mirror), or in its
   own row. The mirror is what a network game keeps: a remote player's 0x23 sets it (0x454B64), and
   allying an AI sets it at once, the engine taking an AI to reciprocate (0x452A3A..0x452A75); a
   remote record's own row here is written only by the battle room's team fill. A skirmish's seat
   fill writes the rows alone (0x47A82F..0x47A890), both ways, and the mirror only for a record
   itself (0x464342). Read under player()'s lifetime; every index is bounded by the ten records,
   and a value that is not finite is shown as 0. */
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
        unsigned char kind = (unsigned char)rec[PL_CONTROLLER];
        if (j == c || !*(const unsigned*)(rec + PL_ACTIVE) || (kind != LOCAL_AI && kind != REMOTE)) continue;
        if (m.rec[PL_ALLY + j] != 1 || (m.rec[PL_ALLIED_BY + j] != 1 && rec[PL_ALLY + c] != 1)) continue;
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
            a->units = *(const unsigned short*)(rec + PL_UNITS);
            a->out = !sendable(rec);
            if (kind == LOCAL_AI) local_economy(rec, a);
            else channel_economy(rec, a);
        }
        n++;
    }
    return n;
}

int tagpu_share_stored(float out[2])
{
    Me m;
    int k;
    if (!player(&m)) return 0;
    memcpy(&out[METAL], m.rec + PL_STORED_M, 4);
    memcpy(&out[ENERGY], m.rec + PL_STORED_E, 4);
    for (k = METAL; k <= ENERGY; k++) if (!isfinite(out[k]) || out[k] < 0.0f) out[k] = 0.0f;
    return 1;
}

int tagpu_share_give(unsigned index, float metal, float energy)
{
    if (index >= PLAYERS || !tagpu_menu_postgui_armed()) return 0;
    s_giveTo = (int)index;
    s_giveGen = tagpu_reclaim_level_gen();
    s_giveAmt[METAL] = metal;
    s_giveAmt[ENERGY] = energy;
    return 1;
}

/* THE GIVE, asked by the HUD's Send (tagpu_hudinput.c) and run here, in the post-GUI observer on
   the game thread, where the SHARE dialog's OK, a GUI handler, makes the same calls. THE BOUND:
   the recipient is, at this moment, one of the allies the ribbon offers (tagpu_share_allies:
   active, a local AI or remote, not a watcher, allied both ways) and not out of the game; anything else is
   refused,
   since the transfer debits the giver before it tests its recipient (network.md, 0x457050) --
   and both records pass the transfer's send gate (sendable), which it tests only after the
   debit. An ask belongs to the level it was made in: one a teardown came before is dropped. Each
   amount is held to 0..the giver's stored value now, which the transfer would cap at too; a
   negative one, which the console's Give lets through, never reaches it. send = 1: the giver's
   peer debits, credits its own copy of the recipient's economy record, and, for a remote
   recipient, sends the 0x16 that credits it on its own peer; a local AI's record is the one
   credited, and nothing is sent (0x451BC0 needs a network game and a type-3 destination). */
static void give_run(void)
{
    TAGPU_SHARE_ALLY a[PLAYERS];
    Me m;
    float stored[2] = { 0, 0 };
    int to = s_giveTo, n, i, k, found = -1;
    s_giveTo = -1;
    if (to < 0 || s_giveGen != tagpu_reclaim_level_gen() || !player(&m) || !tagpu_share_stored(stored)) return;
    n = tagpu_share_allies(a, PLAYERS);
    for (i = 0; i < n && i < PLAYERS; i++) if ((int)a[i].index == to) found = i;
    if (found < 0 || a[found].out || !sendable(m.rec) ||
        !sendable(m.ta + OFF_PLAYERS + to * PL_STRIDE)) {
        tagpu_logf("share: a give to record %d refused: it is no longer an ally in the game", to);
        tagpu_hudinput_toast("Give refused: that player is out of the game");
        return;
    }
    for (k = METAL; k <= ENERGY; k++) {
        float v = s_giveAmt[k];
        if (!(v > 0.0f)) continue;
        if (v > stored[k]) v = stored[k];
        if (!(v > 0.0f)) continue;
        ((give_fn)(uintptr_t)(k == METAL ? VA_GIVE_METAL : VA_GIVE_ENERGY))(
            (unsigned char)m.ta[OFF_CONTROLLED], (unsigned)to, v, 1);
        tagpu_logf("share: gave %.0f %s to record %d (%s)", v, NAME[k], to, a[found].name);
    }
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

/* Sharing starts off in every level: the first in-play draw of a level clears both bits. A level
   that plays a recording is marked seen at its first draw and never cleared: a takeover hands the
   controlled seat to a recorded player without a new level, and that seat's recorded share bits
   are the recording's, which the taken-over game plays on (the auto-share runs in a skirmish). */
static void new_level(void)
{
    Me m;
    unsigned gen = tagpu_reclaim_level_gen();
    unsigned short* bits;
    if (gen == s_level) return;
    if (tagpu_puppet_on()) { s_level = gen; return; }
    if (!player(&m)) return;
    s_level = gen;
    bits = (unsigned short*)(m.info + PI_SHARE);
    if (!(*bits & (BIT[METAL] | BIT[ENERGY]))) return;
    *bits &= (unsigned short)~(BIT[METAL] | BIT[ENERGY]);
    ((sendinfo_fn)(uintptr_t)VA_SENDINFO)();
    tagpu_logf("sharetick: a new level, sharing off");
}

void tagpu_sharetick_draw(void)
{
    Me m;
    Bar bars[2];
    int have[2], x[2], k, cx, cy, moved = 0, fresh = 0;
    new_level();
    /* the wiki takes every input message while it is open, a release included */
    if (tagpu_wiki_holds_input()) s_drag = -1;
    give_run();
    if (!me(&m)) { s_drag = -1; s_shown[METAL] = s_shown[ENERGY] = -1; return; }
    fresh = tagpu_topbar_fresh();       /* this frame's block has repainted the bars */
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
    if (moved && !fresh) tagpu_topbar_repaint();
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
