/* tagpu_replay.c -- playing a demo recording back on puppets (tagpu_replay.h;
   research/notes/tadr-port/demo-recorder-s2-host.md, which this file cites by section).

   OPENING (§6). The request file names a recording, and the SKIRMISH Start reads it. Four call
   sites turn the level that Start loads into a puppet level, each inert unless the request was
   accepted:
   - S-a, the Start's call of the seat fill (0x47B092). The stock fill runs first. Then the
     request is validated before anything is written: the recording's format and exclusion file,
     a skirmish, no network, the map the Start has just read, the seats and the unit limit. On
     success every recorded seat becomes a type-3 record with its recorded side, logo,
     name and alliance row, the viewer's own seat V is seated beside them (§1.1), the objective's
     victory and defeat tests are disarmed (0x4904B0), and the puppet table is built. On any
     failure the log says why, and the Start goes on as the skirmish its settings describe.
   - S-b, the Start's call of the settings persist 0x430F00 (0x47B09C). After the persist has
     written the player's own settings to the registry, the recording's options go into the
     settings the loader copies from. The teardown puts the player's back.
   - S-c, the loader's two calls of the seat setup 0x496EE0 (0x497963, 0x497ACD), which create
     the start units: skipped on a puppet level.
   - S-d, the first step: what needs the loaded level is checked -- its counts, its map, the unit
     and weapon data, its first GameTime. On a mismatch nothing is ever applied and the level
     stays empty, which the viewer sees and the log explains.
   The teardown (S-e) ends the replay, and the recorder refuses a puppet level (S-f).

   WHAT IT READS AND WRITES. Everything here runs on the game thread: the Start handler and the
   sim step are the engine's own game-thread code, and the teardown is entered only from the
   engine's state handlers there. The player records and their PlayerInfo blocks live in the main
   block for the process (tagpu_recorder.c says why); a seat index is below 10 before it
   addresses one (the reader refuses anything else, and V is chosen below 10). The skirmish
   settings [main+0x29A0], GameingState [main+0x391E9] and the objective [main+0x391ED] are the
   ones the Start has just used: the settings for its fill, GameingState for its mode, and the
   objective armed by the .ota read at 0x47AF77. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_replay.h"
#include "tagpu_recreader.h"
#include "tagpu_recfmt.h"
#include "tagpu_recorder.h"
#include "tagpu_replay_layout.h"
#include "tagpu_puppet.h"
#include "tagpu_limits.h"
#include "tagpu_patches.h"
#include "tagpu_simreg.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "git.h"

#define TA_MAINPP      0x00511DE8u
#define OFF_PLAYERS    0x1B63       /* the eleven player records, 0x14B apart                     */
#define PL_STRIDE      0x14B
#define PL_INFO        0x27         /* PlayerInfo*                                                */
#define PL_NAME        0x2B         /* the name, 0x1E bytes; the seat activation copies it to +0x49 */
#define PL_NAME2       0x49
#define PL_NAMELEN     0x1E
#define PL_CONTROLLER  0x73
#define PL_ALLY        0x108        /* the alliance row, 11 bytes                                 */
#define INFO_SIDE      0x95
#define INFO_LOGO      0x96
#define INFO_FLAGS     0x9B
#define INFO_WATCHER   0x40         /* the watcher bit, as the network watcher switch sets it
                                       (0x46569D)                                                */
#define OFF_NPLAYERS   0x2A3C       /* u16: the player count                                      */
#define OFF_CONTROLLED 0x2A42       /* u8: the player the local game commands                     */
#define OFF_VIEWED     0x2A43       /* u8: the player whose view it draws                        */
#define OFF_NETWORK    0x2A44       /* bit 0: a network game (the pump's test, 0x453D4B)          */
#define OFF_SETTINGS   0x29A0       /* the skirmish settings: +0x108..+0x118 the options, +0x11C
                                       the map's name                                            */
#define SET_OPTIONS    0x108        /* ActiveCommanderDeath, then Mapping, LineOfSight, LOSType
                                       (bit 0 each: LosType bits 0, 1, 2, 0x4973E0..0x497450),
                                       then the fixed start positions (0x49790E), a dword each   */
#define SET_MAP        0x11C
#define OFF_NSEATS     0x38D81      /* u32: the settings entries the fill walks                   */
#define OFF_GSTATE     0x391E9      /* GameingState*: +0 the mode (0x435100)                      */
#define OFF_OBJECTIVE  0x391ED      /* the objective, armed by the .ota read (0x435DE9)           */
#define OFF_EACH       0x37EE6      /* u16: the units a player may have in this level              */
#define OFF_EACH_MENU  0x37EEC      /* u16: the same, as the menu set it; the loader copies it
                                       (0x4973C1..0x4973CD)                                       */
#define OFF_DIFFICULTY 0x37EEE      /* u32 (0x401441)                                             */
#define OFF_NSLOTS     0x14351      /* u16: the unit array's slots                                */
#define OFF_GAMETIME   0x38A47
#define MODE_SKIRMISH  2

/* 0x464290(seat, type): the seat activation (stdcall, `ret 8`; seat and type are read as bytes) */
typedef void (__stdcall* PFN_ActivateSeat)(unsigned seat, unsigned type);
#define E_ActivateSeat ((PFN_ActivateSeat)0x00464290u)
/* 0x4904B0: the objective's disarm, `mov dword [ecx+0x88],0; ret` (thiscall) */
typedef void (__thiscall* PFN_Disarm)(void* objective);
#define E_Disarm ((PFN_Disarm)0x004904B0u)

#define REQUEST   SIM_TOOL_FILE("tagpu_replay.play")
#define NAME_MAX_ 64                /* a request's name, NUL included                            */

static RecReader* s_rd;             /* the recording this level plays                             */
static char       s_name[NAME_MAX_];
static unsigned   s_viewer;         /* V                                                          */
static int        s_first;          /* the first step's checks are still to come                  */
static int        s_dead;           /* they failed: nothing is applied this level                 */
static int        s_over;           /* the recording has nothing more to apply                    */
static unsigned   s_notes;          /* the level's anomalies logged                               */
static struct {
    unsigned steps, late, made, created, died, refused, missing, lingering, other, bad_ids;
} s_st;                             /* the level's counts, logged at its end                      */
static struct {
    int            saved;
    unsigned char  set[20];         /* the settings' +0x108..+0x11B, as the player had them       */
    unsigned       difficulty;
    unsigned short each;
} s_opt;

static char* ta_main(void) { return *(char**)(size_t)TA_MAINPP; }

static char* player_rec(char* ta, unsigned i) { return ta + OFF_PLAYERS + (size_t)i * PL_STRIDE; }

static void replay_close(void)
{
    recr_close(s_rd);
    s_rd = NULL;
    s_first = s_dead = s_over = 0;
    s_notes = 0;
    memset(&s_st, 0, sizeof s_st);
}

/* ---- S-a: the request and the seat fill ----------------------------------------------------- */

/* The request's name: 1 read, 0 no request, -1 not understood (`why`). The file is gone
   afterwards whatever it held: one reader, one time (§6.3). */
static int request_read(char* name, unsigned cap, char* why, unsigned whycap)
{
    char line[128], got[NAME_MAX_];
    FILE* f;
    size_t n = 0;
    unsigned i;
    if (GetFileAttributesA(REQUEST) == INVALID_FILE_ATTRIBUTES) return 0;
    f = fopen(REQUEST, "rb");
    if (f) {
        n = fread(line, 1, sizeof line - 1, f);
        fclose(f);
    }
    line[n] = 0;
    DeleteFileA(REQUEST);
    if (sscanf(line, "play %63s", got) != 1) {
        snprintf(why, whycap, "%s is not `play <name>`", REQUEST);
        return -1;
    }
    for (i = 0; got[i]; i++)
        if (!((got[i] >= 'a' && got[i] <= 'z') || (got[i] >= 'A' && got[i] <= 'Z') ||
              (got[i] >= '0' && got[i] <= '9') || got[i] == '-' || got[i] == '_')) {
            snprintf(why, whycap, "the name %s is not letters, digits, - and _", got);
            return -1;
        }
    snprintf(name, cap, "%s", got);
    return 1;
}

/* The unit types and the weapons against the header's (D8). Only at the first step: the level's
   loader reloads the unit data, so at the Start the types are the last level's, and a check there
   compared stale names (measured 2026-09-30: 279 types on both sides, the names' hashes apart). */
static int data_match(const char* ta, const RecHeader* h, char* why, unsigned cap)
{
    unsigned n, hash;
    tagpu_rec_defs_print(ta, &n, &hash);
    if (!h->has_unitdefs || n != h->ndefs || hash != h->defs_hash) {
        snprintf(why, cap, "the unit types differ from the recording's (%u here, %u there)", n,
                 h->ndefs);
        return 0;
    }
    tagpu_rec_weapons_print(&n, &hash);
    if (n && (!h->has_weapons || n != h->nweapons || hash != h->weapons_hash)) {
        snprintf(why, cap, "the weapons differ from the recording's (%u slots here, %u there)", n,
                 h->nweapons);
        return 0;
    }
    return 1;
}

/* Everything the seating rests on, before anything is written (§6.4). 1 with V in `viewer`. */
static int validate(const char* ta, const RecHeader* h, char* why, unsigned cap,
                    unsigned* viewer)
{
    const char* set = *(const char* const*)(ta + OFF_SETTINGS);
    const char* gs = *(const char* const*)(ta + OFF_GSTATE);
    unsigned char seated[REC_PLAYERS] = { 0 };
    char map[RECR_TEXT];
    unsigned i;
    if (strcmp(h->layout, TAGPU_REPLAY_LAYOUT_SHA256)) {
        snprintf(why, cap, "it was recorded with another exclusion file (replay-layout.tsv)");
        return 0;
    }
    if (h->mode != MODE_SKIRMISH) {
        snprintf(why, cap, "it is not a skirmish (mode %u)", h->mode);
        return 0;
    }
    if (ta[OFF_NETWORK] & 1) {
        snprintf(why, cap, "this is a network game");
        return 0;
    }
    if (!gs || *(const int*)gs != MODE_SKIRMISH) {
        snprintf(why, cap, "this Start is not a skirmish's");
        return 0;
    }
    if (!set || !h->has_settings_map || !h->has_options) {
        snprintf(why, cap, "the recording or the settings lack the map or the options");
        return 0;
    }
    for (i = 0; i + 1 < sizeof map && set[SET_MAP + i]; i++) map[i] = set[SET_MAP + i];
    map[i] = 0;
    if (_stricmp(map, h->settings_map)) {
        snprintf(why, cap, "the Start read the map %s, the recording's is %s", map,
                 h->settings_map);
        return 0;
    }
    if (h->nseats < 2) {
        snprintf(why, cap, "it seats %u players", h->nseats);
        return 0;
    }
    if (h->nseats >= REC_PLAYERS) {
        snprintf(why, cap, "it seats all ten records, which leaves the viewer none");
        return 0;
    }
    for (i = 0; i < h->nseats; i++) {
        if (h->seats[i].controller != 1 && h->seats[i].controller != 2) {
            snprintf(why, cap, "seat %u's controller is %u", h->seats[i].index,
                     h->seats[i].controller);
            return 0;
        }
        seated[h->seats[i].index] = 1;
    }
    if (*(const unsigned*)(ta + OFF_NSEATS) > REC_PLAYERS) {
        snprintf(why, cap, "the settings walk %u entries", *(const unsigned*)(ta + OFF_NSEATS));
        return 0;
    }
    if (h->units_each < 1 || h->units_each > TAGPU_LIM_UNITS) {
        snprintf(why, cap, "it allowed %u units a player, this build at most %u", h->units_each,
                 TAGPU_LIM_UNITS);
        return 0;
    }
    for (i = 0; i < REC_PLAYERS && seated[i]; i++) {}
    *viewer = i;    /* below 10: at most nine are seated */
    if (strcmp(h->build, GIT_COMMIT))
        tagpu_logf("replay: recorded by the build %s, played by %s: a puppet computes nothing a "
                   "build changes (D8)", h->build, GIT_COMMIT);
    return 1;
}

/* a recorded name into one of the record's 0x1E-byte name fields, NUL-ended */
static void name_put(char* dst, const char* name)
{
    size_t n = strlen(name);
    if (n > PL_NAMELEN - 1) n = PL_NAMELEN - 1;
    memset(dst, 0, PL_NAMELEN);
    memcpy(dst, name, n);
}

/* §6.4's seating, after validate */
static void seat_level(char* ta, const RecHeader* h, unsigned v)
{
    unsigned char seatv[REC_PLAYERS] = { 0 }, localv[REC_PLAYERS] = { 0 };
    unsigned char* info;
    void* objective;
    unsigned i;
    for (i = 0; i < h->nseats; i++) {
        const RecSeat* s = &h->seats[i];
        char* rec = player_rec(ta, s->index);
        E_ActivateSeat(s->index, 3);
        info = *(unsigned char**)(rec + PL_INFO);
        if (info) {
            info[INFO_SIDE] = s->side;
            info[INFO_LOGO] = s->logo;
        }
        name_put(rec + PL_NAME, s->name);
        name_put(rec + PL_NAME2, s->name);
        memcpy(rec + PL_ALLY, s->ally, 11);     /* the activation zeroes the row */
        seatv[s->index] = 1;
        localv[s->index] = 1;                   /* validate: every controller is 1 or 2 */
    }
    /* V shows the recording viewer's side: the interface the game draws is the controlled
       player's */
    E_ActivateSeat(v, 3);
    info = *(unsigned char**)(player_rec(ta, v) + PL_INFO);
    if (info) {
        info[INFO_FLAGS] |= INFO_WATCHER;
        for (i = 0; i < h->nseats; i++)
            if (h->seats[i].index == h->controlled) info[INFO_SIDE] = h->seats[i].side;
    }
    seatv[v] = 1;
    /* a record the stock fill activated that the recording does not seat */
    for (i = 0; i < REC_PLAYERS; i++) {
        char c = player_rec(ta, i)[PL_CONTROLLER];
        if (!seatv[i] && (c == 1 || c == 2)) E_ActivateSeat(i, 0);
    }
    ta[OFF_CONTROLLED] = (char)v;
    ta[OFF_VIEWED] = (char)v;
    *(unsigned short*)(ta + OFF_NPLAYERS) = (unsigned short)h->nplayers;
    objective = *(void**)(ta + OFF_OBJECTIVE);
    if (objective) E_Disarm(objective);
    tagpu_puppet_begin(seatv, localv);
}

static void __cdecl replay_fill(void)
{
    char* ta = ta_main();
    char name[NAME_MAX_], path[MAX_PATH], why[200];
    unsigned v = 0;
    int k;
    replay_close();
    tagpu_puppet_end();
    k = request_read(name, sizeof name, why, sizeof why);
    if (!k) return;
    if (k < 0) {
        tagpu_logf("replay: not playing: %s", why);
        return;
    }
    snprintf(path, sizeof path, "replays\\%s.imprec", name);
    s_rd = recr_open(path, why, sizeof why);
    if (!s_rd) {
        tagpu_logf("replay: not playing %s: %s", name, why);
        return;
    }
    if (!ta || !validate(ta, recr_header(s_rd), why, sizeof why, &v)) {
        tagpu_logf("replay: not playing %s: %s", name, ta ? why : "no main block");
        replay_close();
        return;
    }
    seat_level(ta, recr_header(s_rd), v);
    snprintf(s_name, sizeof s_name, "%s", name);
    s_viewer = v;
    s_first = 1;
    tagpu_logf("replay: %s seated: %u players on %s, the viewer at record %u", name,
               recr_header(s_rd)->nseats, recr_header(s_rd)->settings_map, v);
}

/* ---- S-b: the recording's options ----------------------------------------------------------- */

static void __cdecl replay_options(void)
{
    char* ta = ta_main();
    char* set;
    const RecHeader* h;
    unsigned b;
    if (!tagpu_puppet_on() || !s_rd || !ta) return;
    set = *(char**)(ta + OFF_SETTINGS);
    if (!set) return;
    h = recr_header(s_rd);
    memcpy(s_opt.set, set + SET_OPTIONS, sizeof s_opt.set);
    s_opt.difficulty = *(unsigned*)(ta + OFF_DIFFICULTY);
    s_opt.each = *(unsigned short*)(ta + OFF_EACH_MENU);
    s_opt.saved = 1;
    memcpy(set + SET_OPTIONS, &h->cmdrdeath, 4);
    for (b = 0; b < 3; b++) {
        char* f = set + SET_OPTIONS + 4 + 4 * b;
        *f = (char)((*f & ~1) | (h->lostype >> b & 1));
    }
    memcpy(set + SET_OPTIONS + 16, &h->fixed_starts, 4);
    *(unsigned*)(ta + OFF_DIFFICULTY) = h->difficulty;
    *(unsigned short*)(ta + OFF_EACH_MENU) = (unsigned short)h->units_each;
}

static void options_restore(char* ta)
{
    char* set;
    if (!s_opt.saved) return;
    s_opt.saved = 0;
    if (!ta) return;
    set = *(char**)(ta + OFF_SETTINGS);
    if (set) memcpy(set + SET_OPTIONS, s_opt.set, sizeof s_opt.set);
    *(unsigned*)(ta + OFF_DIFFICULTY) = s_opt.difficulty;
    *(unsigned short*)(ta + OFF_EACH_MENU) = s_opt.each;
}

/* ---- S-d: the step -------------------------------------------------------------------------- */

/* What the loaded level must agree on with the recording (§6.4, S-d). */
static int first_checks(const char* ta, const RecHeader* h, char* why, unsigned cap)
{
    char stem[RECR_TEXT];
    unsigned each = *(const unsigned short*)(ta + OFF_EACH);
    unsigned slots = *(const unsigned short*)(ta + OFF_NSLOTS);
    unsigned now = *(const unsigned*)(ta + OFF_GAMETIME);
    if (each != h->units_each || slots != h->slots) {
        snprintf(why, cap, "the level allows %u units a player in %u slots, the recording %u in %u",
                 each, slots, h->units_each, h->slots);
        return 0;
    }
    tagpu_rec_map_stem(ta, stem, sizeof stem);
    if (strcmp(stem, h->map)) {
        snprintf(why, cap, "the level's map is %s, the recording's %s", stem, h->map);
        return 0;
    }
    if (!data_match(ta, h, why, cap)) return 0;
    if (now != h->start) {
        snprintf(why, cap, "the level's first step is GameTime %u, the recording's %u", now,
                 h->start);
        return 0;
    }
    return 1;
}

/* ---- the applier ---------------------------------------------------------------------------- */

/* THE APPLY, at each step T, when the host's unit tick has returned (B2's stub at 0x4954ED): first
   the events of step T, in recorded order -- the recording machine's unit tick T made them, from
   the world of image T, which the host holds -- and then the image of T+1, the world that unit
   tick and the rest of iteration T left there (demo-recorder-s2-events.md §1.0). The rest of the
   host's iteration then reads what it read on the recording machine: the players phase re-stamps
   every unit's sight from the positions the unit tick left (0x4827B0, 0x465046..0x465063), and the
   30-tick passes see the values they saw. At the next step's capture point the host holds image
   T+1, which is where a dump (`dump <name>` in tagpu_replay.trigger) is taken and compared.
   The first step makes the units the level started with -- recorded as images, with no create
   before them -- through the create, as the loader made them, and writes image T before the
   events of T.

   A UNIT'S IMAGE goes in by the movers note's recipe (demo-recorder-s2-movers.md §2.3), in
   ascending slot order as the unit tick runs:
   1. the values, raw (s_raw1): every byte the exclusion file keeps as a value except those below,
      the mover's inputs and the turn words included;
   2. for a unit with a mover, the placement: the set-position 0x48A9F0 with the recorded x, z and
      layer and the altitude the movement pass had before its fix-up (a_pre), then the walk class
      0x43DA70 and the water state 0x43DB50, which start the scripts a change starts;
   3. what the fix-up and the relink leave, raw (s_raw2): the altitude and turn words, +0x110 bit
      16, the mover's claim stamp +0x26 and moved-at +0x2A;
   4. the ID rows that name a weapon or the last attacker, each turned back into its pointer.
   Never written: the position, cell, layer, sight stamp, walk class and water state, which the
   engine functions above write; what the create and the level init wrote (the type and its index,
   the slot, the owner); the carry links and the attach piece (0x48AB70's); the state byte
   (0x48B090's); +0x110 bit 14, which the reaper acts on, and the create's bits 28-31; and the
   contact bits 8-10 and 12, which the host's radar pass computes for its own view (D1). The
   harness compares them all but the contact bits (tools/replay/tarec.py compare --puppet).

   A PLAYER'S IMAGE is written raw under the same exclusions, every record's but V's. Each step
   every seat's +0x18 is GameTime: the pacing gate 0x495230 runs ticks only as far as the
   smallest +0x18 of a type-3 record with a live unit.

   EVERY VALUE FROM THE RECORDING IS DATA until bounded: a slot below the array's count, which the
   first step checked equal to the recording's; a slot the host holds a unit of the image's type in;
   a weapon or unit ID within its array; a create's owner a seat whose block holds the slot, and
   its type below UNITINFOCount. What fails a bound is not applied; it is counted and the first few
   are logged. A unit the recording has and the host does not, or the reverse, is named, never
   made or destroyed here: its create or death is what was missed. */

#define OFF_BEGIN      0x14357      /* the unit array                                              */
#define OFF_LAST       0x1435B      /* its last slot                                               */
#define OFF_UCOUNT     0x1438F      /* UNITINFOCount                                               */
#define PL_NETID       0x04         /* the seat activation sets it to the seat (0x464290)          */
#define PL_PACE        0x18         /* the pacing gate's stamp (0x4952CF..0x495303)                */
#define PL_BLOCKFIRST  0x67         /* the record's first and last unit, inclusive                 */
#define PL_BLOCKLAST   0x6B
#define U_ALT          0x6E
#define U_TYPEIX       0xA6         /* u16, the type's index; 0 for a free slot (0x486DC7)          */
#define U_ATTACKER     0xF0
#define U_OWNERIX      0xFF
#define U_BUILT        0x104        /* the build left, a float: 0.0 when finished                  */
#define U_STATE        0x110
#define U_ALIVE        0x10000000u
#define MV_VALT        0x0C         /* the mover's vertical velocity                              */
#define WPN_STRIDE     0x115
/* +0x110 bits no raw write touches: the layer and the walk class (0-3), the contact bits (8-10,
   12), dying (14), the fix-up's bit (16, step 3), carried (17), and the create's 28-31 */
#define U_ENGINE_BITS  0xF003570Fu
#define NOTES_MAX      32           /* the log lines a level's anomalies may take                 */

/* 0x485F50(owner, type, x, altitude, z, finished, layer, slot): the create (stdcall, `ret 0x20`);
   a nonzero slot is taken exactly, and an occupied one refused (0x486002..0x486043) */
typedef char* (__stdcall* PFN_Create)(unsigned owner, unsigned type, int32_t x, int32_t alt,
                                      int32_t z, uint32_t finished, unsigned layer, unsigned slot);
#define E_Create  ((PFN_Create)0x00485F50u)
/* 0x48A9F0(unit, x, altitude, z, layer): the set-position (stdcall, `ret 0x14`) */
typedef void (__stdcall* PFN_SetPos)(char* unit, int32_t x, int32_t alt, int32_t z,
                                     unsigned layer);
#define E_SetPos  ((PFN_SetPos)0x0048A9F0u)
/* 0x43DA70 the walk class, 0x43DB50 the water state: thiscall on the mover (unit), `ret 4` */
typedef void (__thiscall* PFN_Part)(char* mover, char* unit);
#define E_Walk    ((PFN_Part)0x0043DA70u)
#define E_Water   ((PFN_Part)0x0043DB50u)

typedef struct Lv {
    char*    ta;
    char*    begin;
    unsigned nslots;
    char*    weapon0;
    unsigned nweapons;
    unsigned ndefs;
    unsigned now;
} Lv;

static uint32_t s_raw1[REC_UNIT_DWORDS], s_raw2[REC_UNIT_DWORDS], s_praw[REC_PLAYER_DWORDS];
static int      s_masks;

static void m_bytes(uint32_t* m, unsigned off, unsigned n, int on)
{
    unsigned i;
    for (i = off; i < off + n; i++) {
        uint32_t b = 0xFFu << (8 * (i & 3));
        if (on) m[i >> 2] |= b;
        else m[i >> 2] &= ~b;
    }
}

/* a field of `size` bytes at `off`, or the bits `mask` of it as a little-endian integer */
static void m_field(uint32_t* m, unsigned off, unsigned size, uint32_t mask, int on)
{
    unsigned i;
    if (!mask) { m_bytes(m, off, size, on); return; }
    for (i = 0; i < size && i < 4; i++) {
        uint32_t b = (mask >> (8 * i) & 0xFFu) << (8 * ((off + i) & 3));
        if (on) m[(off + i) >> 2] |= b;
        else m[(off + i) >> 2] &= ~b;
    }
}

static void masks_init(void)
{
    unsigned i;
    if (s_masks) return;
    m_bytes(s_raw1, 0, REC_UNIT_BYTES + REC_MOVER_BYTES, 1);
    m_bytes(s_praw, 0, REC_PLAYER_BYTES, 1);
    m_bytes(s_praw, REC_INFO_AT, REC_INFO_BYTES, 1);
    for (i = 0; i < sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0]; i++) {
        const ReplayLayoutRow* r = &tagpu_replay_layout[i];
        if (r->record == RL_UNIT) m_field(s_raw1, r->offset, r->size, r->mask, 0);
        if (r->record == RL_MOVER) m_field(s_raw1, REC_MOVER_AT + r->offset, r->size, r->mask, 0);
        if (r->record == RL_PLAYER) m_field(s_praw, r->offset, r->size, r->mask, 0);
        if (r->record == RL_INFO) m_field(s_praw, REC_INFO_AT + r->offset, r->size, r->mask, 0);
    }
    m_bytes(s_raw1, 0x6A, 12, 0);           /* x, altitude, z                                  */
    m_bytes(s_raw1, 0x76, 8, 0);            /* the cell, the sight stamp's +0x7A/+0x7C         */
    m_bytes(s_raw1, U_TYPEIX, 4, 0);        /* the type's index, the slot +0xA8                */
    m_bytes(s_raw1, 0xF8, 2, 0);            /* the sight stamp's +0xF8, the attach piece       */
    m_bytes(s_raw1, U_OWNERIX, 1, 0);
    m_bytes(s_raw1, 0x10A, 1, 0);           /* the water state                                 */
    m_bytes(s_raw1, 0x10E, 1, 0);           /* the state byte                                  */
    m_field(s_raw1, U_STATE, 4, U_ENGINE_BITS, 0);
    m_bytes(s_raw1, REC_MOVER_AT + 0x26, 8, 0);
    m_bytes(s_raw2, 0x64, 6, 1);            /* the turn words                                  */
    m_bytes(s_raw2, U_ALT, 4, 1);
    m_field(s_raw2, U_STATE, 4, 0x10000u, 1);
    m_bytes(s_raw2, REC_MOVER_AT + 0x26, 8, 1);
    s_masks = 1;
}

/* the level as the apply needs it; 0 when the unit array is not the one the first step checked */
static int level_of(char* ta, Lv* L)
{
    unsigned n = *(const unsigned short*)(ta + OFF_NSLOTS);
    char* b = *(char**)(ta + OFF_BEGIN);
    char* last = *(char**)(ta + OFF_LAST);
    if (!b || n < 2 || n != recr_world(s_rd)->slots ||
        last != b + (size_t)(n - 1) * REC_UNIT_BYTES)
        return 0;
    L->ta = ta;
    L->begin = b;
    L->nslots = n;
    L->weapon0 = tagpu_limits_weapon0();
    L->nweapons = L->weapon0 ? (unsigned)tagpu_limits_weapon_slots() : 0;
    L->ndefs = *(const unsigned*)(ta + OFF_UCOUNT);
    L->now = *(const unsigned*)(ta + OFF_GAMETIME);
    return 1;
}

static char* unit_at(const Lv* L, unsigned slot) { return L->begin + (size_t)slot * REC_UNIT_BYTES; }

static int host_live(const Lv* L, unsigned slot)
{
    return (*(const uint32_t*)(unit_at(L, slot) + U_STATE) & U_ALIVE) != 0;
}

static unsigned img_type(const uint32_t* img)
{
    return *(const unsigned short*)((const char*)img + U_TYPEIX);
}

/* the slot holds a live unit of the image's type: the image describes the unit there */
static int holds(const Lv* L, unsigned slot, const uint32_t* img)
{
    return host_live(L, slot) &&
           *(const unsigned short*)(unit_at(L, slot) + U_TYPEIX) == img_type(img);
}

static void note(const char* fmt, ...)
{
    char line[256];
    va_list ap;
    if (s_notes >= NOTES_MAX) return;
    if (++s_notes == NOTES_MAX) tagpu_log("replay: (the last line of this level's notes)");
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    tagpu_log(line);
}

/* `n` bytes at `dst` from the image's bytes at `at`, under the mask: a dword at a time where the
   image's dword lies inside the `n` bytes, a byte at a time at an end, so nothing past the
   object is written */
static void put_masked(char* dst, const uint32_t* img, const uint32_t* m, unsigned at, unsigned n)
{
    unsigned i = 0;
    while (i < n) {
        unsigned b = at + i;
        if (!(b & 3) && i + 4 <= n) {
            uint32_t mk = m[b >> 2], v;
            if (mk) {
                memcpy(&v, dst + i, 4);
                v = (v & ~mk) | (img[b >> 2] & mk);
                memcpy(dst + i, &v, 4);
            }
            i += 4;
        } else {
            unsigned char mk = (unsigned char)(m[b >> 2] >> (8 * (b & 3)));
            unsigned char iv = (unsigned char)(img[b >> 2] >> (8 * (b & 3)));
            dst[i] = (char)(((unsigned char)dst[i] & ~mk) | (iv & mk));
            i++;
        }
    }
}

/* an ID of the weapon array or the unit array as its pointer: 0 when it is past the array */
static int weapon_ptr(const Lv* L, uint32_t id, char** p)
{
    if (!id) { *p = NULL; return 1; }
    if (!L->weapon0 || id - 1 >= L->nweapons) return 0;
    *p = L->weapon0 + (size_t)(id - 1) * WPN_STRIDE;
    return 1;
}

static int unit_ptr(const Lv* L, uint32_t id, char** p)
{
    if (!id) { *p = NULL; return 1; }
    if (id - 1 >= L->nslots) return 0;
    *p = unit_at(L, id - 1);
    return 1;
}

static void ids_put(const Lv* L, char* u, const uint32_t* img, unsigned slot)
{
    static const unsigned short weapon[3] = { 0x10, 0x2C, 0x48 };
    unsigned k;
    char* p;
    for (k = 0; k < 3; k++) {
        if (weapon_ptr(L, img[weapon[k] / 4], &p)) {
            memcpy(u + weapon[k], &p, 4);
        } else {
            s_st.bad_ids++;
            note("replay: unit %u's weapon %u names ID %u, past the weapon array", slot, k + 1,
                 img[weapon[k] / 4]);
        }
    }
    if (unit_ptr(L, img[U_ATTACKER / 4], &p)) {
        memcpy(u + U_ATTACKER, &p, 4);
    } else {
        s_st.bad_ids++;
        note("replay: unit %u's last attacker names ID %u, past the unit array", slot,
             img[U_ATTACKER / 4]);
    }
}

/* one unit's image, by the recipe above; the slot holds() it */
static void unit_put(const Lv* L, unsigned slot, const uint32_t* img)
{
    char* u = unit_at(L, slot);
    char* mover = *(char**)u;           /* the create made it for a type with one (def +0x22F) */
    put_masked(u, img, s_raw1, 0, REC_UNIT_BYTES);
    if (mover) {
        const char* ib = (const char*)img;
        int32_t x, z, alt, a_pre, vy;
        uint32_t carrier;
        put_masked(mover, img, s_raw1, REC_MOVER_AT, REC_MOVER_BYTES);
        memcpy(&x, ib + 0x6A, 4);
        memcpy(&alt, ib + U_ALT, 4);
        memcpy(&z, ib + 0x72, 4);
        memcpy(&carrier, ib + 0x86, 4);
        /* the movement pass's altitude before the fix-up: the carried branch places the unit at
           its carrier's point, and otherwise adds the vertical velocity (0x43D80D..0x43D813) */
        if (carrier) {
            a_pre = alt;
        } else {
            memcpy(&a_pre, u + U_ALT, 4);
            memcpy(&vy, ib + REC_MOVER_AT + MV_VALT, 4);
            a_pre += vy;
        }
        E_SetPos(u, x, a_pre, z, img[U_STATE / 4] & 3u);
        E_Walk(mover, u);
        E_Water(mover, u);
        put_masked(u, img, s_raw2, 0, REC_UNIT_BYTES);
        put_masked(mover, img, s_raw2, REC_MOVER_AT, REC_MOVER_BYTES);
    }
    ids_put(L, u, img, slot);
}

/* `slot` is in the unit block of `owner`, a seat other than V: the create scales the owner's
   record unbounded (0x485F5C..0x485F78), and a slot outside its block would be another seat's */
static int owner_holds(const Lv* L, unsigned owner, unsigned slot)
{
    const char* rec;
    const char *first, *last, *u;
    if (owner >= REC_PLAYERS || owner == s_viewer || !tagpu_puppet_seat(owner)) return 0;
    if (slot < 1 || slot >= L->nslots) return 0;
    rec = player_rec(L->ta, owner);
    memcpy(&first, rec + PL_BLOCKFIRST, 4);
    memcpy(&last, rec + PL_BLOCKLAST, 4);
    u = unit_at(L, slot);
    return first && last && u >= first && u <= last;
}

/* the create of a recorded unit in its recorded slot; 1 made */
static int make(const Lv* L, unsigned slot, unsigned owner, unsigned type, const int32_t pos[3],
                uint32_t finished, unsigned layer)
{
    char* u;
    if (!owner_holds(L, owner, slot) || !type || type >= L->ndefs) {
        s_st.refused++;
        note("replay: the create of slot %u refused: owner %u, type %u", slot, owner, type);
        return 0;
    }
    if (*(const unsigned short*)(unit_at(L, slot) + U_TYPEIX)) {
        s_st.refused++;
        note("replay: the create of slot %u at GameTime %u refused: the slot holds type %u", slot,
             L->now, *(const unsigned short*)(unit_at(L, slot) + U_TYPEIX));
        return 0;
    }
    u = E_Create(owner, type, pos[0], pos[1], pos[2], finished, layer, slot);
    if (u != unit_at(L, slot)) {
        s_st.refused++;
        note("replay: the create of slot %u gave %s %u", slot, u ? "slot" : "nothing,",
             u ? (unsigned)((u - L->begin) / REC_UNIT_BYTES) : 0);
        return 0;
    }
    return 1;
}

/* the first step: every unit of the recording's first image, made as the loader made it */
static void level_units(const Lv* L, const RecWorld* W)
{
    unsigned slot;
    for (slot = 1; slot < L->nslots; slot++) {
        const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
        const char* ib = (const char*)img;
        int32_t pos[3];
        float built;
        if (!W->live[slot]) continue;
        memcpy(pos, ib + 0x6A, 12);
        memcpy(&built, ib + U_BUILT, 4);
        if (make(L, slot, (unsigned char)ib[U_OWNERIX], img_type(img), pos, built == 0.0f,
                 img[U_STATE / 4] & 3u))
            s_st.made++;
    }
}

static void units_put(const Lv* L, const RecWorld* W)
{
    unsigned slot;
    for (slot = 1; slot < L->nslots; slot++) {
        const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
        if (!W->live[slot]) {
            if (host_live(L, slot)) {
                s_st.lingering++;
                note("replay: GameTime %u: slot %u is live here, not in the recording", L->now,
                     slot);
            }
            continue;
        }
        if (!host_live(L, slot)) {
            s_st.missing++;
            note("replay: GameTime %u: slot %u is live in the recording, not here", L->now,
                 slot);
            continue;
        }
        if (!holds(L, slot, img)) {
            s_st.other++;
            note("replay: GameTime %u: slot %u holds type %u here", L->now, slot,
                 *(const unsigned short*)(unit_at(L, slot) + U_TYPEIX));
            continue;
        }
        unit_put(L, slot, img);
    }
}

/* Every record but V's: the seats, and the records neither the recording nor the host seats, which
   hold what the level start 0x465E30 gave all ten from the skirmish menu's entries (their energy
   and metal, 0x465EF5..0x465F1A), and which no loop over the players reads while inactive. */
static void players_put(const Lv* L, const RecWorld* W)
{
    unsigned p;
    for (p = 0; p < REC_PLAYERS; p++) {
        char* rec;
        char* info;
        if (p == s_viewer) continue;
        rec = player_rec(L->ta, p);
        put_masked(rec, W->player[p], s_praw, 0, REC_PLAYER_BYTES);
        memcpy(&info, rec + PL_INFO, 4);
        if (info) put_masked(info, W->player[p], s_praw, REC_INFO_AT, REC_INFO_BYTES);
        if (tagpu_puppet_seat(p)) tagpu_puppet_set_local(p, W->ctl[p] == 1 || W->ctl[p] == 2);
    }
}

/* RK_EV_CREATE: the create with the recorded arguments, into the recorded slot */
static void ev_create(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id, finished;
    unsigned short type;
    int32_t pos[3];
    if (len != 24) return;
    memcpy(&id, b, 4);
    memcpy(&type, b + 4, 2);
    memcpy(&finished, b + 8, 4);
    memcpy(pos, b + 12, 12);
    if (make(L, id - 1, b[6], type, pos, finished, b[7] & 3u)) s_st.created++;
}

/* RK_EV_DEATH: the victim's fields as the destructor found them (s2-events §1.9), then the
   destructor through C4's wrap, as a remote's death is applied: mode 0, the killer's player by
   the host's netid for it (every seat's is its index, 0x464290), and C4's carried decision armed
   from the recording */
static void ev_death(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id, kid, netid = 0xFFFFFFFFu;
    unsigned slot, ks, kp = b[8];
    unsigned char rec[16];
    uint32_t img[REC_UNIT_DWORDS];
    if (len != 12 + REC_UNIT_IMAGE) return;
    memcpy(&id, b, 4);
    memcpy(&kid, b + 4, 4);
    memcpy(img, b + 12, REC_UNIT_IMAGE);
    slot = id - 1;
    if (slot < 1 || slot >= L->nslots || !holds(L, slot, img)) {
        s_st.missing++;
        note("replay: GameTime %u: the death of slot %u finds no unit of its type here", L->now,
             slot);
        return;
    }
    unit_put(L, slot, img);
    if (kp < REC_PLAYERS && kp != s_viewer && tagpu_puppet_seat(kp))
        memcpy(&netid, player_rec(L->ta, kp) + PL_NETID, 4);
    ks = kid && kid - 1 < L->nslots ? kid - 1 : 0;
    memset(rec, 0, sizeof rec);
    rec[0] = 0x0C;
    rec[1] = (unsigned char)slot;
    rec[2] = (unsigned char)(slot >> 8);
    memcpy(rec + 3, &netid, 4);
    rec[7] = (unsigned char)ks;
    rec[8] = (unsigned char)(ks >> 8);
    rec[9] = b[9];
    rec[10] = b[10];
    if (tagpu_tx_replay_death(rec, b[11])) s_st.died++;
    else s_st.refused++;
}

static void events_put(const Lv* L, const RecWorld* W)
{
    unsigned i;
    for (i = 0; i < W->nev; i++) {
        if (W->ev[i].kind == RK_EV_CREATE) ev_create(L, W->ev[i].b, W->ev[i].len);
        else if (W->ev[i].kind == RK_EV_DEATH) ev_death(L, W->ev[i].b, W->ev[i].len);
    }
}

/* every seat's +0x18 = GameTime, each step while the level plays or has played a recording: the
   pacing gate would otherwise slow the level's ticks to a hundredth as the stamps age */
static void pace(char* ta, unsigned now)
{
    unsigned p;
    for (p = 0; p < REC_PLAYERS; p++)
        if (p != s_viewer && tagpu_puppet_seat(p)) memcpy(player_rec(ta, p) + PL_PACE, &now, 4);
}

static void images_put(const Lv* L, const RecWorld* W)
{
    units_put(L, W);
    players_put(L, W);
}

/* once a level: at the recording's end, or at the level's when that comes first */
static void counts_log(const char* when, unsigned now)
{
    tagpu_logf("replay: %s at %s, GameTime %u: %u iterations applied (%u late); units made %u at "
               "the start, %u created, %u destroyed; %u refused, %u missing here, %u here only, "
               "%u of another type; %u IDs past their array", s_name, when, now, s_st.steps,
               s_st.late, s_st.made, s_st.created, s_st.died, s_st.refused, s_st.missing,
               s_st.lingering, s_st.other, s_st.bad_ids);
}

/* The next iteration into the world; 0 when there is none, and the playing stops. */
static int iteration_read(const Lv* L)
{
    char why[200];
    int k = recr_tick(s_rd, why, sizeof why);
    if (k > 0) return 1;
    s_over = 1;
    if (k == 0) tagpu_logf("replay: %s played to its end at GameTime %u: %s", s_name, L->now, why);
    else tagpu_logf("replay: %s cannot be read on from GameTime %u: %s; the units stop here",
                    s_name, L->now, why);
    counts_log("the recording's end", L->now);
    return 0;
}

/* the step's events and the next image, by THE APPLY above */
static void play(const Lv* L, int first)
{
    RecWorld* W = recr_world(s_rd);
    masks_init();
    if (first) {
        if (!iteration_read(L)) return;
        if (W->tick != L->now) {
            s_over = 1;
            tagpu_logf("replay: not playing %s: its first iteration is GameTime %u, the level's "
                       "first step %u", s_name, W->tick, L->now);
            return;
        }
        level_units(L, W);
        images_put(L, W);
    }
    /* The world holds iteration k: its images went in at the step before k, its events are
       step k's. An iteration of an earlier GameTime is played late rather than dropped, since its
       events are the only record of what they made and destroyed. */
    while (W->tick <= L->now) {
        if (W->tick < L->now) {
            s_st.late++;
            note("replay: the events of GameTime %u played at GameTime %u", W->tick, L->now);
        }
        events_put(L, W);
        s_st.steps++;
        if (!iteration_read(L)) break;
        images_put(L, W);
    }
}

void __cdecl tagpu_replay_step(void)
{
    char* ta;
    char why[200];
    Lv L;
    int first = 0;
    if (!tagpu_puppet_on() || !s_rd || s_dead) return;
    ta = ta_main();
    if (!ta) return;
    if (s_first) {
        s_first = 0;
        if (!first_checks(ta, recr_header(s_rd), why, sizeof why)) {
            s_dead = 1;
            tagpu_logf("replay: not playing %s: %s; the level stays empty", s_name, why);
            return;
        }
        tagpu_logf("replay: playing %s: %u seats, the viewer at record %u, from GameTime %u",
                   s_name, recr_header(s_rd)->nseats, s_viewer, recr_header(s_rd)->start);
        first = 1;
    }
    if (!s_over && !level_of(ta, &L)) {
        s_over = 1;
        tagpu_logf("replay: %s stops at GameTime %u: the unit array is not the one the level began "
                   "with", s_name, *(const unsigned*)(ta + OFF_GAMETIME));
        counts_log("the stop", *(const unsigned*)(ta + OFF_GAMETIME));
    }
    if (!s_over) play(&L, first);
    pace(ta, *(const unsigned*)(ta + OFF_GAMETIME));
}

/* ---- S-e: the teardown ---------------------------------------------------------------------- */

void tagpu_replay_level_end(void)
{
    char* ta = ta_main();
    options_restore(ta);
    if (s_rd && !s_dead && !s_over)
        counts_log("its level's end", ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0);
    replay_close();
    tagpu_puppet_end();
}

/* ---- the sites ------------------------------------------------------------------------------ */

/* a call at `site` whose target is `target` (E8 rel32), not taken by another module */
static int call_is(unsigned site, unsigned target)
{
    const unsigned char* p = (const unsigned char*)(size_t)site;
    int rel = (int)(target - (site + 5));
    return !tagpu_detour_landed(site, NULL, NULL) && p[0] == 0xE8 && !memcmp(p + 1, &rel, 4);
}

static void call_write(unsigned site, const unsigned char* stub, int* ok)
{
    unsigned char c[5];
    int rel = (int)((unsigned)(size_t)stub - (site + 5));
    c[0] = 0xE8;
    memcpy(c + 1, &rel, 4);
    if (*ok && !tagpu_detour_write(site, c, 5)) *ok = 0;
}

/* call `engine`, then `fn` with every register and the flags kept; the site's return follows */
static unsigned char* after_call(unsigned engine, void (__cdecl* fn)(void))
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0xE8; tagpu_detour_rel(p, engine); p += 4;
    *p++ = 0x9C;                                        /* pushfd */
    *p++ = 0x60;                                        /* pushad */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)fn); p += 4;
    *p++ = 0x61;                                        /* popad  */
    *p++ = 0x9D;                                        /* popfd  */
    *p++ = 0xC3;                                        /* ret    */
    return s;
}

void tagpu_replay_init(void)
{
    enum { FILL = 0x0047B092u, PERSIST = 0x0047B09Cu, START1 = 0x00497963u, START2 = 0x00497ACDu };
    unsigned char *fill, *persist, *skip, *p;
    int ok = 1;
    if (!call_is(FILL, 0x0047A760u) || !call_is(PERSIST, 0x00430F00u) ||
        !call_is(START1, 0x00496EE0u) || !call_is(START2, 0x00496EE0u)) {
        tagpu_logf("replay: the Start's or the loader's calls hold other bytes: no replays");
        return;
    }
    fill = after_call(0x0047A760u, replay_fill);
    persist = after_call(0x00430F00u, replay_options);
    /* the seat setup 0x496EE0(seat, start) is stdcall `ret 8`: on a puppet level the stub returns
       as it would, with the two arguments popped; otherwise it jumps to it with the site's
       return address on top */
    skip = p = tagpu_detour_stub();
    if (!fill || !persist || !skip) {
        tagpu_detour_stub_free(fill);
        tagpu_detour_stub_free(persist);
        tagpu_detour_stub_free(skip);
        tagpu_logf("replay: no memory for the sites: no replays");
        return;
    }
    p = tagpu_detour_cmp_flag(p, (volatile unsigned char*)tagpu_puppet_flag());
    *p++ = 0x75; *p++ = 0x05;                           /* jne over the jump   */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00496EE0u); p += 4;
    *p++ = 0xC2; *p++ = 0x08; *p++ = 0x00;              /* ret 8               */

    call_write(START1, skip, &ok);
    call_write(START2, skip, &ok);
    call_write(PERSIST, persist, &ok);
    call_write(FILL, fill, &ok);
    if (!ok) {
        tagpu_logf("replay: FAILED a site could not be written: no request is read");
        return;
    }
    tagpu_logf("replay: 4 sites in");
}
