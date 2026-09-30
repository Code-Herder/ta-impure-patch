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
#define EV_KINDS (RK_EV_SPRAY - RK_EV_CREATE + 1)
static struct {
    unsigned steps, late, made, refused, missing, lingering, other, bad_ids, links, unlinked;
    unsigned obuilt, ofreed, orefused, yards, yrefused;
    unsigned played[EV_KINDS], skipped[EV_KINDS];   /* each event kind's, from RK_EV_CREATE    */
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
   before them -- through the create, as the loader made them, writes image T before the events
   of T, and seeds the wind from image T (wind_seed).

   AN IMAGE goes in in this order: the carry links (links_put), which the placement reads; each
   unit (units_put); the recorded yard changes (yards_put); +0x110 once more for every unit
   (states_put), since the placement's and the yards' claims mark the incumbents of the cells they
   contest; the players (players_put); and the order lists (orders_put).

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
   (0x48B090's); the yard's bit, +0x10F bit 2 (YARD_OPEN's); +0x110 bit 14, which the reaper acts
   on, and the create's bits 28-31; and the contact bits 8-10 and 12, which the host's radar pass
   computes for its own view (D1). The harness compares them all but the contact bits
   (tools/replay/tarec.py compare --puppet).

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
#define W_FLAGS        0x111        /* a weapon's flags (0x49D295)                                 */
#define U_COB          0x9A
#define U_CARRIER      0x86         /* the carrier                                                 */
#define U_CARGO        0x8A         /* the first cargo                                             */
#define U_NEXT         0x8E         /* the next cargo while +0x86 is set, else the bucket's next   */
#define U_BUCKET       0x82
#define U_PIECE        0xF9         /* the attach piece; PIECE_NONE for none                       */
#define PIECE_NONE     0xFF
#define U_NOMOVER      0x20000000u  /* +0x110: the type has no mover                               */
#define MV_LAYER       0x2E         /* the mover's layer, bits 0-1                                 */
#define U_DEF          0x92         /* the type's definition, *(main+0x1439B) + 0x249 the index    */
#define U_FOOTXZ       0x76         /* the footprint's cell x and z, i16 each (0x47D98E)           */
#define U_FOOTWH       0x7E         /* its width and height, i16 each: the def's +0x14A (0x485AAA) */
#define U_YARD         0x10F        /* bit 2: the yard is open (0x47DAF1)                          */
#define U_RECLAIM      0x08000000u  /* +0x110 bit 27: the re-claim is owed (0x47DAEC)              */
#define OFF_UDEFS      0x1439B
#define DEF_STRIDE     0x249
#define DEF_FOOTWH     0x14A
#define DEF_YARDMAP    0x14E        /* the yardmap, a byte a footprint cell (0x47DA3D..0x47DA51)    */
#define OFF_GRIDW      0x14233      /* the footprint grid's width and height in cells              */
#define OFF_GRIDH      0x14237
#define U_ORDERS       0x5C         /* the order lists' heads: the main list and the sub list      */
#define U_SUBORDERS    0x60
#define ORD_VTABLE     0x004FD2C8u  /* every constructed node's (0x43A131)                         */
#define ORD_ACTION     0x04         /* the descriptor's rank                                       */
#define ORD_REF        0x12         /* the unit reference, its target at +0x16                     */
#define ORD_TARGET     0x16
#define ORD_P1         0x36
#define ORD_FLAGS      0x42
#define ORD_NEXT       0x4A
#define ORD_OWNED      0x52
#define OF_COUNT       0x100u       /* a count node: +0x36 is a type (0x439D80)                    */
#define OF_SUBLIST     0x40000u     /* the node belongs on the sub list (0x43AD19)                 */
#define OF_SEEN        0x200000u    /* +0x32/+0x34 hold the target's last-seen place (0x4397C5)    */
#define DESC_BEGIN     0x00512344u  /* the order descriptors, 0x19 bytes each, to *(0x512348)      */
#define DESC_END       0x00512348u
#define DESC_STRIDE    0x19
#define DESC_MARKS     0x0C         /* the marker mask; bit 0 draws the build site (0x439B87)      */
#define REPLY_KINDS    24           /* the notice table 0x5086D8                                   */
#define OFF_FXLAYERS   0x38D77      /* the particle layer table, FX_LAYERS of 16 bytes (0x471D90)  */
#define FX_LAYERS      10
/* +0x110 bits no raw write touches: the layer and the walk class (0-3), the contact bits (8-10,
   12), dying (14), the fix-up's bit (16, step 3), carried (17), and the create's 28-31 */
#define U_ENGINE_BITS  0xF003570Fu
#define NOTES_MAX      32           /* the log lines a level's anomalies may take                 */

/* 0x485F50(owner, type, x, altitude, z, finished, layer, slot): the create (stdcall, `ret 0x20`);
   a nonzero slot is taken exactly, and an occupied one refused (0x486002..0x486043) */
typedef char* (__stdcall* PFN_Create)(unsigned owner, unsigned type, int32_t x, int32_t alt,
                                      int32_t z, uint32_t finished, unsigned layer, unsigned slot);
#define E_Create  ((PFN_Create)0x00485F50u)
/* 0x48AB70(rec): the 0x0A apply, stdcall, `ret 4`: rec is 0x0A, the child's u16 slot, the
   parent's (0: detach), the piece, the layer (0x48AB7D..0x48ABC4, 0x48AC63, 0x48ACE3) */
typedef void (__stdcall* PFN_Link)(const unsigned char* rec);
#define E_Link    ((PFN_Link)0x0048AB70u)
/* The order node's functions (demo-recorder-s2-orders.md §1.1-§1.6): MEM_Alloc 0x4B4F10 and
   MEM_Free 0x4B4F20, cdecl; the constructor 0x43A0C0(action, target, pos*, p1, p2, p3), thiscall
   on the block, `ret 0x18`; the destructor 0x43A1F0, thiscall; the tail linker 0x43AD10(unit,
   node), stdcall, `ret 8`, into the list the node's +0x42 & 0x40000 names; the reference's
   setter 0x489690(target), thiscall on +0x12, `ret 4`, which unlinks the old target and links a
   new one only when it is non-NULL with +0xA6 set */
typedef void* (__cdecl* PFN_MemAlloc)(unsigned size);
typedef void (__cdecl* PFN_MemFree)(void* p);
typedef char* (__thiscall* PFN_OrdMake)(char* block, unsigned action, char* target,
                                        const int32_t* pos, uint32_t p1, uint32_t p2, uint32_t p3);
typedef void (__thiscall* PFN_OrdFree)(char* node);
typedef void (__stdcall* PFN_OrdLink)(char* unit, char* node);
typedef void (__thiscall* PFN_RefSet)(char* ref, char* target);
#define E_MemAlloc ((PFN_MemAlloc)0x004B4F10u)
#define E_MemFree  ((PFN_MemFree)0x004B4F20u)
#define E_OrdMake  ((PFN_OrdMake)0x0043A0C0u)
#define E_OrdFree  ((PFN_OrdFree)0x0043A1F0u)
#define E_OrdLink  ((PFN_OrdLink)0x0043AD10u)
#define E_RefSet   ((PFN_RefSet)0x00489690u)
/* 0x47C790(unit): the re-claim, stdcall, `ret 4`; 0x440A40(xz, wh): the passability refresh over
   the footprint grown by a cell, stdcall, `ret 8` (demo-recorder-s2-host.md §5.1) */
typedef void (__stdcall* PFN_Reclaim)(char* unit);
typedef void (__stdcall* PFN_Passable)(uint32_t xz, uint32_t wh);
#define E_Reclaim  ((PFN_Reclaim)0x0047C790u)
#define E_Passable ((PFN_Passable)0x00440A40u)
/* 0x48A9F0(unit, x, altitude, z, layer): the set-position (stdcall, `ret 0x14`) */
typedef void (__stdcall* PFN_SetPos)(char* unit, int32_t x, int32_t alt, int32_t z,
                                     unsigned layer);
#define E_SetPos  ((PFN_SetPos)0x0048A9F0u)
/* 0x43DA70 the walk class, 0x43DB50 the water state: thiscall on the mover (unit), `ret 4` */
typedef void (__thiscall* PFN_Part)(char* mover, char* unit);
#define E_Walk    ((PFN_Part)0x0043DA70u)
#define E_Water   ((PFN_Part)0x0043DB50u)
/* 0x48B090(mask, on): the state byte's setter, thiscall on the unit, `ret 8` */
typedef void (__thiscall* PFN_SetState)(char* unit, unsigned mask, unsigned on);
#define E_SetState ((PFN_SetState)0x0048B090u)
/* 0x41B8D0(builder, built): build done, stdcall, `ret 8` */
typedef void (__stdcall* PFN_Built)(char* builder, char* built);
#define E_Built   ((PFN_Built)0x0041B8D0u)
/* 0x4B0B00(idx, cb, now, argc, a0..a3) and 0x4B0A70(name, cb, now, argc, a0..a3), thiscall on
   the COB object, `ret 0x20`; 0x4B0940(name, cb, now), `ret 0xC` */
typedef int (__thiscall* PFN_StartIdx)(void* cob, int idx, void* cb, int now, int argc, int a0,
                                       int a1, int a2, int a3);
typedef int (__thiscall* PFN_StartName)(void* cob, const char* name, void* cb, int now, int argc,
                                        int a0, int a1, int a2, int a3);
typedef int (__thiscall* PFN_StartName0)(void* cob, const char* name, void* cb, int now);
#define E_StartIdx   ((PFN_StartIdx)0x004B0B00u)
#define E_StartName  ((PFN_StartName)0x004B0A70u)
#define E_StartName0 ((PFN_StartName0)0x004B0940u)
/* 0x49D270(from, msg): the 0x0D receiver, stdcall, `ret 8`; it writes the message's slot byte */
typedef void (__stdcall* PFN_Shot)(void* from, unsigned char* msg);
#define E_Shot    ((PFN_Shot)0x0049D270u)
/* 0x47F780 and 0x47F850(unit, kind, text): the notices, stdcall, `ret 0xC` */
typedef void (__stdcall* PFN_Reply)(char* unit, unsigned kind, const char* text);
#define E_Reply0  ((PFN_Reply)0x0047F780u)
#define E_Reply1  ((PFN_Reply)0x0047F850u)
/* 0x4720D0(point, box, layer) and 0x472200(box, point, layer): the nano emitters, `ret 0xC` */
typedef void (__stdcall* PFN_Spray)(const void* a, const void* b, int32_t layer);
#define E_Spray0  ((PFN_Spray)0x004720D0u)
#define E_Spray1  ((PFN_Spray)0x00472200u)

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
static uint32_t s_state[REC_UNIT_DWORDS];         /* +0x110 as the last write takes it        */
static uint32_t s_oraw[REC_ORDER_IMAGE / 4];      /* an order node's values                   */
static uint32_t s_oclr[REC_ORDER_IMAGE / 4], s_oset[REC_ORDER_IMAGE / 4];   /* its puppet rows */
static int      s_masks;
static unsigned short s_chain[0x10000];   /* a recorded chain, head first: the slots' count is a
                                             u16 (OFF_NSLOTS), so no chain has more members     */

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
    m_bytes(s_oraw, 0, REC_ORDER_BYTES, 1);
    for (i = 0; i < sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0]; i++) {
        const ReplayLayoutRow* r = &tagpu_replay_layout[i];
        if (r->record == RL_UNIT) m_field(s_raw1, r->offset, r->size, r->mask, 0);
        if (r->record == RL_MOVER) m_field(s_raw1, REC_MOVER_AT + r->offset, r->size, r->mask, 0);
        if (r->record == RL_PLAYER) m_field(s_praw, r->offset, r->size, r->mask, 0);
        if (r->record == RL_INFO) m_field(s_praw, REC_INFO_AT + r->offset, r->size, r->mask, 0);
        if (r->record == RL_ORDER) {
            m_field(s_oraw, r->offset, r->size, r->mask, 0);
            if (r->target == RL_T_NONE && r->cls == RL_PUPPET0)
                m_field(s_oclr, r->offset, r->size, r->mask, 1);
            if (r->target == RL_T_NONE && r->cls == RL_PUPPET1)
                m_field(s_oset, r->offset, r->size, r->mask, 1);
        }
    }
    m_bytes(s_raw1, 0x6A, 12, 0);           /* x, altitude, z                                  */
    m_bytes(s_raw1, 0x76, 8, 0);            /* the cell, the sight stamp's +0x7A/+0x7C         */
    m_bytes(s_raw1, U_TYPEIX, 4, 0);        /* the type's index, the slot +0xA8                */
    m_bytes(s_raw1, 0xF8, 2, 0);            /* the sight stamp's +0xF8, the attach piece       */
    m_bytes(s_raw1, U_OWNERIX, 1, 0);
    m_bytes(s_raw1, 0x10A, 1, 0);           /* the water state                                 */
    m_bytes(s_raw1, 0x10E, 1, 0);           /* the state byte                                  */
    m_field(s_raw1, U_YARD, 1, 0x04, 0);    /* the yard, YARD_OPEN's                           */
    m_field(s_raw1, U_STATE, 4, U_ENGINE_BITS, 0);
    m_bytes(s_raw1, REC_MOVER_AT + 0x26, 8, 0);
    m_bytes(s_raw2, 0x64, 6, 1);            /* the turn words                                  */
    m_bytes(s_raw2, U_ALT, 4, 1);
    m_field(s_raw2, U_STATE, 4, 0x10000u, 1);
    m_bytes(s_raw2, REC_MOVER_AT + 0x26, 8, 1);
    m_field(s_state, U_STATE, 4, ~U_ENGINE_BITS, 1);
    m_bytes(s_oraw, 0, 4, 0);               /* the vtables, the constructor's                  */
    m_bytes(s_oraw, ORD_REF, 4, 0);
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

/* THE CARRY LINKS (demo-recorder-s2-movers.md §3.8), each step before the placement and after the
   step's deaths and creates, whose own detaches (0x4867CB, 0x48681D) are then done: every chain is
   made the recorded one through the attach 0x48AB70 alone. It is the one function that keeps a
   chain consistent -- its unlink walks the carrier's chain, or the child's bucket (0x47CB00), to
   the child and faults on one that does not hold it -- so the links, the piece and +0x110 bit 17
   are never written raw. A detach unlinks in place and an attach pushes at the front (0x48AC82):
   a chain whose host members are the recorded chain's tail gets the rest attached from the tail
   to the head, and any other is emptied and rebuilt so, which leaves the recorded order whatever
   happened within the step. Only what differs is called, and each call deselects its child
   (0x48C9B0), as the recording machine's did. The order purge 0x4384A0 is for a type-1/2
   owner's child (0x48ACF3..0x48AD1F) and never runs here.

   Each call is bounded first as 0x48AB70 bounds it (0x48ABC7..0x48AC1D), plus the two pointers it
   dereferences untested: the child's mover (0x48ACE1) and, for a child in no chain, its bucket.
   A host pointer is DATA until it is a slot of the array; a recorded chain is walked only through
   units the recording has live and carried by its carrier, and no further than the slots there
   are, so a repeat ends it. What fails a bound is counted and named, never forced. */

/* the slot a host unit pointer names; 0 for none and for a pointer that is no slot of the array */
static unsigned slot_of(const Lv* L, const char* p)
{
    size_t d;
    if (p <= L->begin) return 0;
    d = (size_t)(p - L->begin);
    if (d % REC_UNIT_BYTES || d / REC_UNIT_BYTES >= L->nslots) return 0;
    return (unsigned)(d / REC_UNIT_BYTES);
}

static const char* img_at(const RecWorld* W, unsigned slot)
{
    return (const char*)(W->unit + (size_t)slot * REC_UNIT_DWORDS);
}

/* a unit ID of `slot`'s image as a slot: 0 for none, and for an ID past the array (counted) */
static unsigned img_slot(const Lv* L, const RecWorld* W, unsigned slot, unsigned off)
{
    uint32_t id;
    memcpy(&id, img_at(W, slot) + off, 4);
    if (!id) return 0;
    if (id - 1 < 1 || id - 1 >= L->nslots) {
        s_st.bad_ids++;
        note("replay: GameTime %u: unit %u's +0x%X names ID %u, past the unit array", L->now,
             slot, off, id);
        return 0;
    }
    return id - 1;
}

/* the recorded carrier of the unit the host holds in `slot`: 0 when it holds none of the image */
static unsigned rec_carrier(const Lv* L, const RecWorld* W, unsigned slot)
{
    const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
    return W->live[slot] && holds(L, slot, img) ? img_slot(L, W, slot, U_CARRIER) : 0;
}

/* one call of the attach: `child` onto `parent` with `piece` and `layer`, or off its carrier
   when `parent` is 0; 1 made. Both are slots below the count. */
static int link(const Lv* L, unsigned child, unsigned parent, unsigned piece, unsigned layer)
{
    const char* c = unit_at(L, child);
    const char* p = parent ? unit_at(L, parent) : NULL;
    const char *mover, *cargo, *carrier, *bucket, *pc = NULL;
    uint32_t cs, ps = U_ALIVE;
    unsigned char rec[7];
    memcpy(&cs, c + U_STATE, 4);
    memcpy(&mover, c, 4);
    memcpy(&cargo, c + U_CARGO, 4);
    memcpy(&carrier, c + U_CARRIER, 4);
    memcpy(&bucket, c + U_BUCKET, 4);
    if (p) {
        memcpy(&ps, p + U_STATE, 4);
        memcpy(&pc, p + U_CARRIER, 4);
    }
    if (!(cs & U_ALIVE) || (cs & U_NOMOVER) || !mover || cargo || (!carrier && !bucket) ||
        !(ps & U_ALIVE) || parent == child || pc) {
        s_st.unlinked++;
        note("replay: GameTime %u: unit %u cannot be %s %u", L->now, child,
             parent ? "attached to" : "detached, carried by", parent ? parent : slot_of(L, carrier));
        return 0;
    }
    rec[0] = 0x0A;
    rec[1] = (unsigned char)child;
    rec[2] = (unsigned char)(child >> 8);
    rec[3] = (unsigned char)parent;
    rec[4] = (unsigned char)(parent >> 8);
    rec[5] = (unsigned char)piece;
    rec[6] = (unsigned char)layer;
    E_Link(rec);
    s_st.links++;
    return 1;
}

/* the layer a link call gives `slot`'s mover: the recorded one when the host holds the image's
   unit, else the mover's own */
static unsigned link_layer(const Lv* L, const RecWorld* W, unsigned slot)
{
    const char* mover;
    if (W->live[slot] && holds(L, slot, W->unit + (size_t)slot * REC_UNIT_DWORDS))
        return (unsigned char)img_at(W, slot)[REC_MOVER_AT + MV_LAYER] & 3u;
    memcpy(&mover, unit_at(L, slot), 4);
    return mover ? (unsigned char)mover[MV_LAYER] & 3u : 0;
}

/* the recorded chain of carrier `c` into s_chain, head first; its length, or -1 when it is not
   a chain of `c`'s cargo */
static int rec_chain(const Lv* L, const RecWorld* W, unsigned c)
{
    unsigned n = 0, s = img_slot(L, W, c, U_CARGO);
    while (s) {
        if (!W->live[s] || img_slot(L, W, s, U_CARRIER) != c || n == L->nslots - 1) return -1;
        s_chain[n++] = (unsigned short)s;
        s = img_slot(L, W, s, U_NEXT);
    }
    return (int)n;
}

/* the host's chain of carrier `c`: its length into *n, and 1 when every member is a slot */
static int host_chain(const Lv* L, unsigned c, unsigned* n)
{
    const char* p;
    unsigned s;
    *n = 0;
    memcpy(&p, unit_at(L, c) + U_CARGO, 4);
    while (p) {
        if (!(s = slot_of(L, p)) || *n == L->nslots - 1) return 0;
        ++*n;
        memcpy(&p, unit_at(L, s) + U_NEXT, 4);
    }
    return 1;
}

static void links_put(const Lv* L, const RecWorld* W)
{
    unsigned slot;
    /* the detach pass: every host cargo whose recorded carrier or piece differs */
    for (slot = 1; slot < L->nslots; slot++) {
        const char* u = unit_at(L, slot);
        const char* hp;
        unsigned hc;
        if (!host_live(L, slot)) continue;
        memcpy(&hp, u + U_CARRIER, 4);
        if (!hp) continue;
        if (!(hc = slot_of(L, hp))) {
            s_st.unlinked++;
            note("replay: GameTime %u: unit %u's carrier is no unit here", L->now, slot);
            continue;
        }
        if (rec_carrier(L, W, slot) == hc &&
            (unsigned char)u[U_PIECE] == (unsigned char)img_at(W, slot)[U_PIECE])
            continue;
        link(L, slot, 0, PIECE_NONE, link_layer(L, W, slot));
    }
    /* the attach pass: every recorded chain the host's differs from */
    for (slot = 1; slot < L->nslots; slot++) {
        const char* hp;
        uint32_t rp;
        unsigned h, i;
        int n;
        if (!W->live[slot] || !holds(L, slot, W->unit + (size_t)slot * REC_UNIT_DWORDS))
            continue;
        memcpy(&hp, unit_at(L, slot) + U_CARGO, 4);
        memcpy(&rp, img_at(W, slot) + U_CARGO, 4);
        if (!hp && !rp) continue;
        if ((n = rec_chain(L, W, slot)) < 0) {
            s_st.unlinked++;
            note("replay: GameTime %u: unit %u's recorded cargo is no chain", L->now, slot);
            continue;
        }
        if (!host_chain(L, slot, &h)) {
            s_st.unlinked++;
            note("replay: GameTime %u: unit %u's cargo here is no chain", L->now, slot);
            continue;
        }
        /* is the host's chain the recorded chain's tail? */
        i = 0;
        if (h <= (unsigned)n) {
            const char* p = hp;
            for (; i < h; i++) {
                if (slot_of(L, p) != s_chain[(unsigned)n - h + i]) break;
                memcpy(&p, p + U_NEXT, 4);
            }
        }
        if (i == h && h == (unsigned)n) continue;
        if (i != h || h > (unsigned)n) {
            /* no: empty it, head first, one detach for each of its h members. A refused detach
               (named) ends this, and the attaches below still run: each unlinks its child
               wherever it is (0x48AC23..0x48AC53), so the chain is the recorded one followed by
               what stayed on it, and the compare names the rest. */
            for (i = 0; i < h; i++) {
                unsigned s;
                memcpy(&hp, unit_at(L, slot) + U_CARGO, 4);
                if (!hp || !(s = slot_of(L, hp)) || !link(L, s, 0, PIECE_NONE, link_layer(L, W, s)))
                    break;
            }
            h = 0;
        }
        while (h < (unsigned)n) {
            unsigned s = s_chain[(unsigned)n - h - 1];
            link(L, s, slot, (unsigned char)img_at(W, s)[U_PIECE], link_layer(L, W, s));
            h++;
        }
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

/* A RECORDED YARD_OPEN (demo-recorder-s2-host.md §5.2), after the placement: for each unit whose
   recorded +0x10F bit 2 is not its own, what 0x47DAC0 does once its refusal 0x47D970 has passed --
   the bit, +0x110 bit 27, the re-claim 0x47C790 and the passability refresh 0x440A40 -- less the
   refusal's occupancy half, which the owner's machine passed mid-step against a grid the host does
   not hold at the boundary. The bounds are the refusal's and what its walk reads unbounded: a
   structure (bit 29, the yardmap path), whose definition is its type's, with a yardmap and the
   footprint the yardmap is laid out for, and the footprint strictly inside the grid, as 0x47D970
   tests it (0x47D991..0x47D9D8, 16-bit sums compared signed). The re-claim has no bound of its own
   and walks those cells. The contest it runs marks this unit and the incumbents with +0x110 bits
   26 and 27, which states_put then writes as recorded. */
static void yards_put(const Lv* L, const RecWorld* W)
{
    const char* defs = *(const char* const*)(L->ta + OFF_UDEFS);
    int gw = *(const int*)(L->ta + OFF_GRIDW), gh = *(const int*)(L->ta + OFF_GRIDH);
    unsigned slot;
    for (slot = 1; slot < L->nslots; slot++) {
        const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
        unsigned char want = (unsigned char)img_at(W, slot)[U_YARD] & 4u;
        char* u = unit_at(L, slot);
        const char* def;
        const void* yard;
        uint32_t st, wh, dwh;
        int16_t x, z, fw, fh;
        unsigned ix;
        if (!W->live[slot] || !holds(L, slot, img) || ((unsigned char)u[U_YARD] & 4u) == want)
            continue;
        ix = *(const unsigned short*)(u + U_TYPEIX);
        memcpy(&def, u + U_DEF, 4);
        memcpy(&st, u + U_STATE, 4);
        memcpy(&wh, u + U_FOOTWH, 4);
        memcpy(&x, u + U_FOOTXZ, 2);
        memcpy(&z, u + U_FOOTXZ + 2, 2);
        memcpy(&fw, u + U_FOOTWH, 2);
        memcpy(&fh, u + U_FOOTWH + 2, 2);
        yard = NULL;
        dwh = ~wh;
        if ((st & U_NOMOVER) && defs && ix < L->ndefs && def == defs + (size_t)ix * DEF_STRIDE) {
            memcpy(&yard, def + DEF_YARDMAP, 4);
            memcpy(&dwh, def + DEF_FOOTWH, 4);
        }
        if (!yard || dwh != wh || x < 1 || z < 1 || (int16_t)(x + fw) >= gw ||
            (int16_t)(z + fh) >= gh) {
            s_st.yrefused++;
            note("replay: GameTime %u: unit %u's yard cannot be %s here", L->now, slot,
                 want ? "opened" : "closed");
            continue;
        }
        u[U_YARD] = (char)(((unsigned char)u[U_YARD] & ~4u) | want);
        st |= U_RECLAIM;
        memcpy(u + U_STATE, &st, 4);
        E_Reclaim(u);
        E_Passable(*(const uint32_t*)(u + U_FOOTXZ), *(const uint32_t*)(u + U_FOOTWH));
        s_st.yards++;
    }
}

/* +0x110 as recorded, less the bits the engine functions own, for every unit once the placement
   and the yards are done: their contests mark bits 26 and 27 on incumbents too (0x47C828..0x47C89C) */
static void states_put(const Lv* L, const RecWorld* W)
{
    unsigned slot;
    for (slot = 1; slot < L->nslots; slot++) {
        const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
        if (W->live[slot] && holds(L, slot, img))
            put_masked(unit_at(L, slot), img, s_state, 0, REC_UNIT_BYTES);
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

/* THE ORDER LISTS (demo-recorder-s2-orders.md §3.6), after the images: each live unit's two
   lists are made its recorded ones, position by position -- a node at a position both have is
   patched in place, the host's extra nodes are torn down, and the missing ones are built and
   linked at the tail. Nothing is assumed of the host's lists, so whatever engine code did to them
   in the step (a death's purge, a target's death clearing references) is reconciled away, and no
   node pointer is kept from one step to the next: every walk starts from the unit's heads.

   EVERY NODE ON A PUPPET IS INERT (§2.4): its puppet rows hold their puppet values -- the
   destructor's handler call (+6 bit 1), its StopBuilding (+0x42 bit 22) and its owned object
   (+0x52) off, its weapon reset held off (+0x42 bit 16) -- so tearing one down, here, in a death
   or at the level's end, runs only the destructor's unlinks, and no handler runs for a type-3
   owner in any case (0x48AEB5..0x48AECD). The action indexes the descriptors unbounded wherever
   it is read, so it is below their count ((*(0x512348) - *(0x512344)) / 0x19, fixed for the
   process); a node whose descriptor draws a build site (mask bit 0), and a count node, index the
   unit types by (u16)+0x36 (0x438C00), so that is below UNITINFOCount or 0; a target becomes a
   pointer only through the engine's constructor or 0x489690, which refuse a slot with no unit
   (+0xA6 = 0), and only for a slot the host holds live. A host node is touched only while its
   vtable is the node's. What fails is counted and named. */

static unsigned desc_count(void)
{
    const char* b = *(const char* const*)(size_t)DESC_BEGIN;
    const char* e = *(const char* const*)(size_t)DESC_END;
    return b && e > b ? (unsigned)((size_t)(e - b) / DESC_STRIDE) : 0;
}

/* the node image's action is a descriptor's: 0 refuses the node */
static int ord_bounded(const unsigned char* img, unsigned ndesc)
{
    return img[ORD_ACTION] < ndesc;
}

/* a recorded unit ID as a target: the live unit the host holds there, else NULL */
static char* ord_target(const Lv* L, uint32_t id)
{
    if (!id) return NULL;
    if (id - 1 < 1 || id - 1 >= L->nslots || !host_live(L, id - 1)) {
        s_st.orefused++;
        note("replay: GameTime %u: an order's target, ID %u, is no live unit here", L->now, id);
        return NULL;
    }
    return unit_at(L, id - 1);
}

/* the node's values from its image, then its puppet rows held and the type bounded */
static void ord_values(const Lv* L, char* n, const unsigned char* img, unsigned slot)
{
    const char* desc = *(const char* const*)(size_t)DESC_BEGIN +
                       (size_t)img[ORD_ACTION] * DESC_STRIDE;
    uint32_t marks, flags, p1;
    unsigned b;
    put_masked(n, (const uint32_t*)img, s_oraw, 0, REC_ORDER_BYTES);
    for (b = 0; b < REC_ORDER_BYTES; b++) {
        unsigned char c = (unsigned char)(s_oclr[b >> 2] >> (8 * (b & 3)));
        unsigned char v = (unsigned char)(s_oset[b >> 2] >> (8 * (b & 3)));
        if (c | v) n[b] = (char)(((unsigned char)n[b] & ~c) | v);
    }
    memcpy(&marks, desc + DESC_MARKS, 4);
    memcpy(&flags, n + ORD_FLAGS, 4);
    memcpy(&p1, n + ORD_P1, 4);
    if (((marks & 1) || (flags & OF_COUNT)) && (p1 & 0xFFFFu) >= L->ndefs) {
        s_st.orefused++;
        note("replay: GameTime %u: unit %u's order names type %u, past the types", L->now, slot,
             p1 & 0xFFFFu);
        p1 = 0;
        memcpy(n + ORD_P1, &p1, 4);
    }
}

static void ord_patch(const Lv* L, char* n, const unsigned char* img, unsigned slot)
{
    uint32_t rid, flags;
    const char* ht;
    char* owned;
    unsigned hs, hid;
    memcpy(&rid, img + ORD_TARGET, 4);
    memcpy(&ht, n + ORD_TARGET, 4);
    hs = ht ? slot_of(L, ht) : 0;
    hid = hs ? hs + 1 : 0;
    memcpy(&flags, n + ORD_FLAGS, 4);
    if (hid != rid || (unsigned char)n[ORD_ACTION] != img[ORD_ACTION]) {
        /* a new target or action: the last-seen place is the old one's, as a fresh node has none */
        flags &= ~OF_SEEN;
        memcpy(n + ORD_FLAGS, &flags, 4);
    }
    if (hid != rid) {
        if (ht && !hs) {
            /* a target that is no slot: 0x489690's unlink would walk a chain it is not on */
            s_st.orefused++;
            note("replay: GameTime %u: unit %u's order holds a target that is no unit", L->now,
                 slot);
        } else {
            E_RefSet(n + ORD_REF, ord_target(L, rid));
        }
    }
    ord_values(L, n, img, slot);
    memcpy(&owned, n + ORD_OWNED, 4);
    if (owned) {
        s_st.orefused++;
        note("replay: GameTime %u: unit %u's order owns an object here", L->now, slot);
    }
}

/* a node built from its image and linked at the tail of `list`; 0 when no block */
static int ord_build(const Lv* L, unsigned slot, unsigned list, const unsigned char* img)
{
    char* unit = unit_at(L, slot);
    char* n = (char*)E_MemAlloc(REC_ORDER_BYTES);
    uint32_t rid, flags, want;
    int32_t pos[3];
    if (!n) return 0;
    memcpy(&rid, img + ORD_TARGET, 4);
    memcpy(pos, img + 0x22, 12);
    E_OrdMake(n, img[ORD_ACTION], ord_target(L, rid), pos, 0, 0, 0);
    ord_values(L, n, img, slot);
    /* the linker picks the list by the node's flag: the recorded flag when it agrees with the
       list the recording holds the node on, which the engine's linkers keep; else it is set for
       the link and the recorded one written back (the recorder counts such nodes) */
    memcpy(&flags, n + ORD_FLAGS, 4);
    want = list ? (flags | OF_SUBLIST) : (flags & ~OF_SUBLIST);
    memcpy(n + ORD_FLAGS, &want, 4);
    E_OrdLink(unit, n);
    memcpy(n + ORD_FLAGS, &flags, 4);
    s_st.obuilt++;
    return 1;
}

static void ord_teardown(char* n)
{
    E_OrdFree(n);
    E_MemFree(n);
    s_st.ofreed++;
}

static void orders_put(const Lv* L, const RecWorld* W)
{
    unsigned slot, list, ndesc = desc_count();
    for (slot = 1; slot < L->nslots; slot++) {
        if (!W->live[slot] || !holds(L, slot, W->unit + (size_t)slot * REC_UNIT_DWORDS)) continue;
        for (list = 0; list < 2; list++) {
            const RecList* rl = &W->list[(size_t)slot * 2 + list];
            char** prev = (char**)(unit_at(L, slot) + (list ? U_SUBORDERS : U_ORDERS));
            char* n = *prev;
            unsigned i = 0, want = rl->n, k;
            for (k = 0; k < want; k++) {
                if (!ord_bounded(rl->img + (size_t)k * REC_ORDER_IMAGE, ndesc)) {
                    s_st.orefused++;
                    note("replay: GameTime %u: unit %u's %s list holds action %u at %u, past the "
                         "%u orders", L->now, slot, list ? "sub" : "main",
                         rl->img[(size_t)k * REC_ORDER_IMAGE + ORD_ACTION], k, ndesc);
                    want = k;
                    break;
                }
            }
            while (n && i < want) {
                if (*(const uint32_t*)n != ORD_VTABLE) break;
                ord_patch(L, n, rl->img + (size_t)i * REC_ORDER_IMAGE, slot);
                prev = (char**)(n + ORD_NEXT);
                n = *prev;
                i++;
            }
            if (n && *(const uint32_t*)n != ORD_VTABLE) {
                s_st.orefused++;
                note("replay: GameTime %u: unit %u's %s list reaches a block that is no order",
                     L->now, slot, list ? "sub" : "main");
                continue;
            }
            while (n) {
                char* next;
                if (*(const uint32_t*)n != ORD_VTABLE) {
                    s_st.orefused++;
                    note("replay: GameTime %u: unit %u's %s list reaches a block that is no order",
                         L->now, slot, list ? "sub" : "main");
                    break;
                }
                next = *(char**)(n + ORD_NEXT);
                *prev = next;
                ord_teardown(n);
                n = next;
            }
            for (; i < want; i++) {
                if (!ord_build(L, slot, list, rl->img + (size_t)i * REC_ORDER_IMAGE)) {
                    s_st.orefused++;
                    note("replay: GameTime %u: no memory for unit %u's order", L->now, slot);
                    break;
                }
            }
        }
    }
}

/* RK_EV_CREATE: the create with the recorded arguments, into the recorded slot */
static int ev_create(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id, finished;
    unsigned short type;
    int32_t pos[3];
    if (len != 24) return 0;
    memcpy(&id, b, 4);
    memcpy(&type, b + 4, 2);
    memcpy(&finished, b + 8, 4);
    memcpy(pos, b + 12, 12);
    return make(L, id - 1, b[6], type, pos, finished, b[7] & 3u);
}

/* RK_EV_DEATH: the victim's fields as the destructor found them (s2-events §1.9), then the
   destructor through C4's wrap, as a remote's death is applied: mode 0, the killer's player by
   the host's netid for it (every seat's is its index, 0x464290), and C4's carried decision armed
   from the recording */
static int ev_death(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id, kid, netid = 0xFFFFFFFFu;
    unsigned slot, ks, kp = b[8];
    unsigned char rec[16];
    uint32_t img[REC_UNIT_DWORDS];
    if (len != 12 + REC_UNIT_IMAGE) return 0;
    memcpy(&id, b, 4);
    memcpy(&kid, b + 4, 4);
    memcpy(img, b + 12, REC_UNIT_IMAGE);
    slot = id - 1;
    if (slot < 1 || slot >= L->nslots || !holds(L, slot, img)) {
        s_st.missing++;
        note("replay: GameTime %u: the death of slot %u finds no unit of its type here", L->now,
             slot);
        return 0;
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
    if (tagpu_tx_replay_death(rec, b[11])) return 1;
    s_st.refused++;
    return 0;
}

/* THE OTHER EVENTS, each through the function the engine's own receiver or caller uses
   (demo-recorder-s2-events.md §1.3-§1.8). Each names its unit by ID, and one whose unit is not
   live here is not played. Two kinds of event meet that by design, and counts_log's played and
   not-played counts are how a run shows no other does:
   - those a create makes before its exit records it: its state byte, set through 0x48B090
     (0x486148), and the notice that setter makes (kind 3): the replayed create makes both again;
   - those a death makes after its record: the destructor's order purge (0x4867A1 -> 0x439EB0)
     tears the dying unit's nodes down, and a node's destructor starts its TargetCleared unless
     +0x42 bit 16 is set (0x43A2AB..0x43A2B9) and broadcasts its StopBuilding when bit 22 is
     (0x43A21E..0x43A25D). A puppet's nodes hold bit 16 set and bit 22 clear (orders_put), so its
     replayed death makes neither, on a unit that is no longer live. */

/* the live unit an event's ID names, or NULL */
static char* live_unit(const Lv* L, uint32_t id)
{
    uint32_t slot = id - 1;
    return slot >= 1 && slot < L->nslots && host_live(L, slot) ? unit_at(L, slot) : NULL;
}

/* RK_EV_STATE: the byte through 0x48B090 as the 0x11 receiver sets it (0x4555A1, 0x4555B0), its
   bits then cleared that it lacks -- Activate, StartBuilding and their opposites start as there */
static int ev_state(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id;
    char* u;
    if (len != 8) return 0;
    memcpy(&id, b, 4);
    if (!(u = live_unit(L, id))) return 0;
    E_SetState(u, b[4], 1);
    E_SetState(u, (unsigned char)~b[4], 0);
    return 1;
}

/* RK_EV_BUILT: 0x41B8D0(builder, built), which tests the builder's build list and both units'
   lives itself (0x41B8D6..0x41B917); only the frame's nanoframe flag, [+0x9E]+0x10, has no
   other writer here */
static int ev_built(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t bid, uid;
    char *builder, *built;
    if (len != 8) return 0;
    memcpy(&bid, b, 4);
    memcpy(&uid, b + 4, 4);
    if (!(builder = live_unit(L, bid)) || !(built = live_unit(L, uid))) return 0;
    E_Built(builder, built);
    return 1;
}

/* the unit's COB object, NULL for a type with no script (0x485DFF) */
static void* cob_of(const char* u)
{
    void* cob;
    memcpy(&cob, u + U_COB, 4);
    return cob;
}

/* RK_EV_START: the 0x10 case's call (0x455528..0x455551), made here rather than through the case,
   which scales the unit's u16 unbounded. 0x4B0B00 bounds the index itself (0x4B08C5..0x4B08D0)
   but not argc: its thread's stack pointer becomes argc - 1 (0x4B0B76), so argc is at most the
   four arguments there are. */
static int ev_start(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id;
    int32_t idx, argc, a[4];
    char* u;
    void* cob;
    if (len != 28) return 0;
    memcpy(&id, b, 4);
    memcpy(&idx, b + 4, 4);
    memcpy(&argc, b + 8, 4);
    memcpy(a, b + 12, 16);
    if (!(u = live_unit(L, id)) || argc < 0 || argc > 4 || !(cob = cob_of(u))) return 0;
    E_StartIdx(cob, idx, NULL, 0, argc, a[0], a[1], a[2], a[3]);
    return 1;
}

/* RK_EV_START_NAMED: the owner's start by name, 0x4B0A70 with its arguments, or 0x4B0940 for the
   sites that pass none -- every recorded site of 0x4B0A70 passes at least one (tagpu_recorder.c,
   START_SITES) -- with the recorded third argument, which runs the script at once */
static int ev_start_named(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id;
    int32_t a[4];
    char name[REC_NAME_MAX + 1];
    char* u;
    void* cob;
    unsigned argc = b[4];
    if (len < 24 || len > 24 + REC_NAME_MAX) return 0;
    memcpy(&id, b, 4);
    memcpy(a, b + 8, 16);
    memcpy(name, b + 24, len - 24);
    name[len - 24] = 0;
    if (!(u = live_unit(L, id)) || argc > 4 || !(cob = cob_of(u))) return 0;
    if (argc) E_StartName(cob, name, NULL, b[5], (int)argc, a[0], a[1], a[2], a[3]);
    else E_StartName0(cob, name, NULL, b[5]);
    return 1;
}

/* RK_EV_SHOT: the 0x0D through its receiver 0x49D270, as a live remote gets it, with the IDs made
   slots again. The receiver's own bounds apply (wpn_rx_fired at 0x49D280: the weapon, the units
   against the array, B3's diverged shooter); the replay adds the two its path lacks: a unit's
   shot from a unit that is live here, and a dropped weapon's (flag bit 8) from a unit with a
   mover, whose speed it reads (0x49D50F). A meteor names no unit (flag bit 5). */
static int ev_shot(const Lv* L, const unsigned char* b, unsigned len)
{
    unsigned char m[36];
    unsigned id, flags;
    unsigned short s, t;
    if (len != 36) return 0;
    memcpy(m, b, sizeof m);
    id = m[0x19];
    if (TAGPU_LIM_WEAPONS > 256) id |= (unsigned)(m[0x23] >> 4) << 8;
    if (!L->weapon0 || id >= L->nweapons) {
        s_st.bad_ids++;
        note("replay: GameTime %u: a shot of weapon %u, past the weapon array", L->now, id);
        return 0;
    }
    memcpy(&flags, L->weapon0 + (size_t)id * WPN_STRIDE + W_FLAGS, 4);
    if (!(flags & 0x20)) {
        memcpy(&s, m + 0x21, 2);
        memcpy(&t, m + 0x1F, 2);
        if (!s || s == 0xFFFF || t == 0xFFFF || s > L->nslots || t > L->nslots) {
            s_st.bad_ids++;
            note("replay: GameTime %u: a shot names units %u and %u, past the unit array",
                 L->now, s, t);
            return 0;
        }
        s--;
        if (t) t--;
        if (!host_live(L, s) || ((flags & 0x100) && !*(char* const*)unit_at(L, s))) return 0;
        memcpy(m + 0x21, &s, 2);
        memcpy(m + 0x1F, &t, 2);
    }
    E_Shot(NULL, m);
    return 1;
}

/* RK_EV_INTERCEPT: the projectile with the recorded target point and weapon detonates, as the
   0x0E receiver 0x49AF90 matches it (tagpu_wpn_replay_intercept) */
static int ev_intercept(const Lv* L, const unsigned char* b, unsigned len)
{
    unsigned short w;
    (void)L;
    if (len != 16) return 0;
    memcpy(&w, b + 12, 2);
    return tagpu_wpn_replay_intercept(b, w);
}

/* RK_EV_REPLY: the notice through the function that made it; its own viewer test (unit +0xFF is
   main+0x2A43, 0x47F780) picks what the viewed player hears. The kind indexes the 24-entry table
   0x5086D8 unbounded (0x47FAD0). */
static int ev_reply(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id, kind;
    char text[REC_TEXT_MAX + 1];
    char* u;
    if (len < 12 || len > 12 + REC_TEXT_MAX) return 0;
    memcpy(&id, b, 4);
    memcpy(&kind, b + 4, 4);
    if (!(u = live_unit(L, id)) || kind >= REPLY_KINDS || b[8] > 1) return 0;
    memcpy(text, b + 12, len - 12);
    text[len - 12] = 0;
    (b[8] ? E_Reply1 : E_Reply0)(u, kind, b[9] ? text : NULL);
    return 1;
}

/* RK_EV_SPRAY: the emitter the order handler called, with its point, box and layer; the layer's
   word indexes the ten particle layers of [main+0x38D77] unbounded (0x47217C..0x472188) */
static int ev_spray(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t reverse;
    int32_t point[3], box[6], layer;
    if (len != 48) return 0;
    memcpy(&reverse, b + 4, 4);
    memcpy(point, b + 8, 12);
    memcpy(box, b + 20, 24);
    memcpy(&layer, b + 44, 4);
    if (reverse > 1 || (short)layer < 0 || (short)layer >= FX_LAYERS ||
        !*(void* const*)(L->ta + OFF_FXLAYERS))
        return 0;
    if (reverse) E_Spray1(box, point, layer);
    else E_Spray0(point, box, layer);
    return 1;
}

static void events_put(const Lv* L, const RecWorld* W)
{
    unsigned i;
    for (i = 0; i < W->nev; i++) {
        const unsigned char* b = W->ev[i].b;
        unsigned n = W->ev[i].len, k = W->ev[i].kind;
        int done;
        switch (k) {
        case RK_EV_CREATE:      done = ev_create(L, b, n); break;
        case RK_EV_DEATH:       done = ev_death(L, b, n); break;
        case RK_EV_SHOT:        done = ev_shot(L, b, n); break;
        case RK_EV_INTERCEPT:   done = ev_intercept(L, b, n); break;
        case RK_EV_START:       done = ev_start(L, b, n); break;
        case RK_EV_START_NAMED: done = ev_start_named(L, b, n); break;
        case RK_EV_BUILT:       done = ev_built(L, b, n); break;
        case RK_EV_STATE:       done = ev_state(L, b, n); break;
        case RK_EV_REPLY:       done = ev_reply(L, b, n); break;
        case RK_EV_SPRAY:       done = ev_spray(L, b, n); break;
        default:                continue;
        }
        if (done) s_st.played[k - RK_EV_CREATE]++;
        else s_st.skipped[k - RK_EV_CREATE]++;
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
    links_put(L, W);
    units_put(L, W);
    yards_put(L, W);
    states_put(L, W);
    players_put(L, W);
    orders_put(L, W);
}

/* The wind's generator state from the recording's first image (tagpu_wind_state_set), for the
   host to draw the recording's wind from: the table the header names as the host declares it */
static void wind_seed(const RecWorld* W)
{
    const RecHeader* h = recr_header(s_rd);
    const char* name = tagpu_wind_state_name();
    unsigned long long st;
    unsigned i;
    for (i = 0; i < h->ntab && i < W->ntab; i++) {
        if (strcmp(h->tab[i].name, name) || h->tab[i].stride != sizeof st) continue;
        if (W->tab[i].size < sizeof st) break;
        memcpy(&st, W->tab[i].img, sizeof st);
        tagpu_wind_state_set(st);
        return;
    }
    tagpu_logf("replay: %s carries no wind state: the level draws a wind of its own", s_name);
}

/* once a level: at the recording's end, or at the level's when that comes first */
static void counts_log(const char* when, unsigned now)
{
    static const char* const name[EV_KINDS] = {
        "create", "death", "shot", "intercept", "start", "named start", "built", "state", "reply",
        "spray"
    };
    char ev[400];
    unsigned k, n = 0;
    ev[0] = 0;
    for (k = 0; k < EV_KINDS && n < sizeof ev; k++)
        n += (unsigned)snprintf(ev + n, sizeof ev - n, "%s%s %u/%u", k ? ", " : "", name[k],
                                s_st.played[k], s_st.skipped[k]);
    tagpu_logf("replay: %s at %s, GameTime %u: %u iterations applied (%u late); units made %u at "
               "the start; %u refused, %u missing here, %u here only, %u of another type; %u IDs "
               "past their array; %u carry links made, %u refused; order nodes %u built, %u torn "
               "down, %u refused; %u yards changed, %u refused", s_name, when, now, s_st.steps,
               s_st.late, s_st.made, s_st.refused, s_st.missing, s_st.lingering, s_st.other,
               s_st.bad_ids, s_st.links, s_st.unlinked, s_st.obuilt, s_st.ofreed, s_st.orefused,
               s_st.yards, s_st.yrefused);
    tagpu_logf("replay: %s: events played/not played: %s", s_name, ev);
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
        wind_seed(W);
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
