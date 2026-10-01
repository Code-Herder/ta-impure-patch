/* tagpu_replay.c -- playing a demo recording back on puppets (tagpu_replay.h;
   research/notes/tadr-port/demo-recorder-s2-host.md, which this file cites by section).

   OPENING (§6). The request file, `play <name> [<record>] [at <T> [dump <name>]]`, names a
   recording, optionally the recorded player whose view the level shows -- without one it shows
   everything, V's full visibility (§2.9) -- and optionally a seek to run once the first step has
   played (THE SEEK). The SKIRMISH Start reads it. Four call sites turn the level that Start loads
   into a puppet level, each inert unless the request was accepted:
   - S-a, the Start's call of the seat fill (0x47B092). The stock fill runs first. Then the
     request is validated before anything is written: every puppet gate in (tagpu_puppet.c) and
     the teardown wrap in (tagpu_reclaim.c), which ends the puppet level before its units are
     destroyed; the recording's format and exclusion file, a skirmish, no network, the map the
     Start has just read, the seats, the unit limit and the view. On success every recorded seat
     becomes a type-3 record with its recorded side, logo, name and alliance row, the viewer's
     own seat V is seated beside them (§1.1) and +0x2A43 names the view, the objective's victory
     and defeat tests are disarmed (0x4904B0), and the puppet table is built. On any failure the
     log says why, and the Start goes on as the skirmish its settings describe.
   - S-b, the Start's call of the settings persist 0x430F00 (0x47B09C). After the persist has
     written the player's own settings to the registry, the recording's options go into the
     settings the loader copies from. From the level's first step, every settings save runs with
     the player's in their place, and the teardown puts the player's back (THE PLAYER'S
     SETTINGS).
   - S-c, the loader's two calls of the seat setup 0x496EE0 (0x497963, 0x497ACD), which create
     the start units: skipped on a puppet level.
   - S-d, the first step: what needs the loaded level is checked -- its counts, its map, the unit
     and weapon data, its first GameTime. On a mismatch nothing is ever applied and the level
     stays empty, which the viewer sees and the log explains. Otherwise the start units are made,
     the first image goes in, the wind is seeded and V's sight is filled (THE APPLY).
   Two more sites serve a level that plays: B2's stub calls tagpu_replay_capture first at each
   capture point, and the frame visit before the in-play draw call 0x4969CD reads the trigger,
   runs a seek (THE SEEK) and writes what the draw after a step shows (THE GLOBALS AND THE
   PROJECTILES).
   The teardown (S-e) ends the replay and gives V's PlayerInfo back once it has returned (THE
   VIEWER'S SEAT), and the recorder refuses a puppet level (S-f).

   WHAT IT READS AND WRITES. Everything here runs on the game thread: the Start handler, the
   sim step and the frame callback 0x496790 are the engine's own game-thread code, and the
   teardown is entered only from the engine's state handlers there. The player records and their
   PlayerInfo blocks live in the main block for the process (tagpu_recorder.c says why); a seat
   index is below 10 before it addresses one (the reader refuses anything else, and V is chosen
   below 10). The skirmish
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
#include "tagpu_reclaim.h"
#include "tagpu_limits.h"
#include "tagpu_patches.h"
#include "tagpu_weapons.h"
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
#define PL_SIGHT       0x7C         /* u8* the sight counters, one a cell ...                     */
#define PL_SIGHTLEN    0x88         /* ... and their bytes (0x4816A0's fill, 0x481734..0x481767)  */
#define PL_MAPBIT      0x146        /* u8: the record's bit in an explored-map entry (0x48194B)   */
#define PL_ENERGY      0x8C         /* float: the energy the level start gives (0x465F04) ...     */
#define PL_METAL       0x98         /* ... and the metal (0x465F1A)                               */
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
#define OFF_SIDES      0x37F39      /* u32: the sides this game loaded; the lobby clamps a side at
                                       or above it (0x447EC0..0x447ECE)                          */
#define OFF_COLOURS    0x148DB      /* the player colours' GAF sequence, loaded at start-up
                                       (0x431911): its u16 count at +0 bounds a colour (0x4B7F41),
                                       its frames at +0x28, 8 bytes each                         */
#define OFF_NSLOTS     0x14351      /* u16: the unit array's slots                                */
#define OFF_GAMETIME   0x38A47
#define OFF_MAPPED     0x14273      /* u16* the explored map, a bit a player (0x4845A9); its bytes
                                       are ([+0x14233]·[+0x14237]·2) >> 2 (0x483CF6, 0x4816B5)   */
#define OFF_LOSTYPE    0x14281      /* u16: bit 3 clear makes the draw rebuild the fog grid from
                                       the viewed player's sight (0x4848F2)                      */
#define OFF_MMFLAGS    0x142F1      /* u8: bit 2 asks 0x466C20 for the minimap's fog; bit 1 is
                                       DrawMinimap's redraw (0x466B11)                           */
#define MODE_SKIRMISH  2

/* 0x464290(seat, type): the seat activation (stdcall, `ret 8`; seat and type are read as bytes) */
typedef void (__stdcall* PFN_ActivateSeat)(unsigned seat, unsigned type);
#define E_ActivateSeat ((PFN_ActivateSeat)0x00464290u)
/* 0x4904B0: the objective's disarm, `mov dword [ecx+0x88],0; ret` (thiscall) */
typedef void (__thiscall* PFN_Disarm)(void* objective);
#define E_Disarm ((PFN_Disarm)0x004904B0u)
/* 0x430F00: the settings save, every preference to the registry (no argument, `ret` at 0x43173F) */
typedef void (__cdecl* PFN_SaveSettings)(void);
#define E_SaveSettings ((PFN_SaveSettings)0x00430F00u)

#define REQUEST   SIM_TOOL_FILE("tagpu_replay.play")
#define NAME_MAX_ 64                /* a request's name, NUL included                            */

static RecReader* s_rd;             /* the recording this level plays                             */
static char       s_name[NAME_MAX_];
static unsigned   s_viewer;         /* V                                                          */
static unsigned   s_view;           /* the viewed player: V for full visibility, or a seat         */
static int        s_first;          /* the first step's checks are still to come                  */
static int        s_dead;           /* they failed: nothing is applied this level                 */
static int        s_over;           /* the recording has nothing more to apply                    */
static unsigned   s_notes;          /* the level's anomalies logged                               */
#define EV_KINDS (RK_EV_SPRAY - RK_EV_CREATE + 1)
static struct {
    unsigned steps, late, made, refused, missing, lingering, other, bad_ids, links, unlinked;
    unsigned obuilt, ofreed, orefused, yards, yrefused, grefused;
    unsigned played[EV_KINDS], skipped[EV_KINDS];   /* each event kind's, from RK_EV_CREATE    */
} s_st;                             /* the level's counts, logged at its end                      */
static struct {
    unsigned roots, rrefused, rdiffer, deaths, dmissed, left, cells, cstruct, cbad, wrecks,
             wstruct, wbad, heads, cursor, tcells, tover;
} s_fst;                            /* ... and its features' (THE FEATURES)                       */
/* The runs the feature tick's input wrote since the last images (THE TICK'S INPUT). 4096 runs:
   MEASURED in the six fixtures of 2026-10-01, the runs of one step's input were at most 432
   (the battle; median 134, p99 270), at most 47 in every other. Past the cap the whole grid is
   written back, so the cap costs time, never a cell. */
#define TIN_RUNS 4096
static struct {
    unsigned n;
    int      overflow;
    uint16_t run[TIN_RUNS][3];      /* x, z, n: bounded by the reader (cells_ok)                  */
} s_tin;
static struct {
    unsigned puts, late, refused, reacts, dropped, unbounded, mputs, mentries, mrefused;
} s_gst;                            /* ... and its globals' and projectiles'                      */
static unsigned   s_frame_tick;     /* the GameTime the frame visit last wrote at                 */
static struct {
    int      pending;               /* a seek waits for the next frame visit after the first step */
    unsigned T;
    char     dump[40];              /* a dump to take at its capture point, or ""                 */
} s_seek;
static struct {
    int            saved;
    int            played;          /* the level's first step has run (THE PLAYER'S SETTINGS)     */
    int            stale;           /* a save ran before it, with the recording's options in      */
    unsigned char  set[20];         /* the settings' +0x108..+0x11B, as the player had them       */
    unsigned       difficulty;
    unsigned short each;
} s_opt;
static struct {
    unsigned char* at;              /* V's PlayerInfo, or NULL                                    */
    unsigned char  was[REC_INFO_BYTES];
} s_vinfo;                          /* as the Start's fill left it (THE VIEWER'S SEAT)            */

static char* ta_main(void) { return *(char**)(size_t)TA_MAINPP; }

static char* player_rec(char* ta, unsigned i) { return ta + OFF_PLAYERS + (size_t)i * PL_STRIDE; }

/* the colours a PlayerInfo +0x96 may name: the sequence's own count, as 0x4B7F30 bounds it */
static unsigned colour_count(const char* ta)
{
    const unsigned short* seq = *(const unsigned short* const*)(ta + OFF_COLOURS);
    return seq ? *seq : 0;
}

static void replay_close(void)
{
    recr_close(s_rd);
    s_rd = NULL;
    s_first = s_dead = s_over = 0;
    s_notes = 0;
    memset(&s_st, 0, sizeof s_st);
    memset(&s_fst, 0, sizeof s_fst);
    s_tin.n = 0;
    s_tin.overflow = 0;
    memset(&s_gst, 0, sizeof s_gst);
    memset(&s_seek, 0, sizeof s_seek);
    s_frame_tick = ~0u;
}

/* ---- S-a: the request and the seat fill ----------------------------------------------------- */

/* the name rule of a recording's and a dump's name: letters, digits, - and _ */
static int name_ok(const char* s)
{
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '-' || *s == '_'))
            return 0;
    return 1;
}

/* `at <T> [dump <name>]` from `p`, the rest of a request's or a trigger's line, into s_seek's
   form: 1 parsed (the whole rest), 0 not */
static int seek_words(const char* p, unsigned* T, char* dump, unsigned cap)
{
    char d[40];
    int end = 0;
    dump[0] = 0;
    if (sscanf(p, " %u%n", T, &end) != 1) return 0;
    p += end;
    while (*p == ' ') p++;
    if (!*p) return 1;
    end = 0;
    if (sscanf(p, "dump %39s%n", d, &end) != 1) return 0;
    p += end;
    while (*p == ' ') p++;
    if (*p || !name_ok(d)) return 0;
    snprintf(dump, cap, "%s", d);
    return 1;
}

/* The request, `play <name> [<seat>] [at <T> [dump <name>]]`: the recording's name, the seat
   whose view to show, or -1 for full visibility, and a seek to run at the first frame visit after
   the first step (THE SEEK), with the dump to take at its capture point. 1 read, 0 no request,
   -1 not understood (`why`). The file is gone afterwards whatever it held: one reader, one time
   (§6.3). */
static int request_read(char* name, unsigned cap, int* view, char* why, unsigned whycap)
{
    char line[160], got[NAME_MAX_];
    const char* p;
    FILE* f;
    size_t n = 0;
    unsigned seat;
    int k, end = 0;
    if (GetFileAttributesA(REQUEST) == INVALID_FILE_ATTRIBUTES) return 0;
    f = fopen(REQUEST, "rb");
    if (f) {
        n = fread(line, 1, sizeof line - 1, f);
        fclose(f);
    }
    line[n] = 0;
    DeleteFileA(REQUEST);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = 0;
    memset(&s_seek, 0, sizeof s_seek);
    if (sscanf(line, "play %63s%n", got, &end) != 1) {
        snprintf(why, whycap, "%s is not `play <name> [<seat>] [at <T> [dump <name>]]`", REQUEST);
        return -1;
    }
    p = line + end;
    *view = -1;
    k = 0;
    if (sscanf(p, " %u%n", &seat, &k) == 1) {
        *view = (int)(seat < REC_PLAYERS ? seat : REC_PLAYERS);
        p += k;
    }
    while (*p == ' ') p++;
    if (*p) {
        k = 0;
        if (strncmp(p, "at ", 3) || !seek_words(p + 3, &s_seek.T, s_seek.dump, sizeof s_seek.dump)) {
            snprintf(why, whycap, "%s is not `play <name> [<seat>] [at <T> [dump <name>]]`",
                     REQUEST);
            return -1;
        }
        s_seek.pending = 1;
    }
    if (!name_ok(got)) {
        memset(&s_seek, 0, sizeof s_seek);
        snprintf(why, whycap, "the name %s is not letters, digits, - and _", got);
        return -1;
    }
    snprintf(name, cap, "%s", got);
    return 1;
}

/* The unit types, the weapons and the features against the header's (D8). Only at the first step: the level's
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
    {
        unsigned gw, gh;
        tagpu_rec_features_print(ta, &gw, &gh, &n, &hash);
        if (gw != h->grid_w || gh != h->grid_h || TAGPU_LIM_WRECKS != h->wrecks ||
            n != h->nfdefs || hash != h->fdefs_hash) {
            snprintf(why, cap, "the features differ from the recording's (a %u x %u grid, %u "
                     "wreck records and %u defs here; %u x %u, %u and %u there)", gw, gh,
                     (unsigned)TAGPU_LIM_WRECKS, n, h->grid_w, h->grid_h, h->wrecks, h->nfdefs);
            return 0;
        }
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
    if (strcmp(h->globals, TAGPU_REPLAY_GLOBALS_SHA256)) {
        snprintf(why, cap, "it was recorded with another positive list (replay-globals.tsv)");
        return 0;
    }
    /* a recorded projectile ID is a record of a pool this size: the pool the limits patch
       allocates, as the wreck pool's size is checked (data_match) */
    if (h->proj_cap != TAGPU_LIM_PROJ) {
        snprintf(why, cap, "it was recorded with a projectile pool of %u records, this build's "
                 "holds %u", h->proj_cap, (unsigned)TAGPU_LIM_PROJ);
        return 0;
    }
    if (h->mode != MODE_SKIRMISH) {
        snprintf(why, cap, "it is not a skirmish (mode %u)", h->mode);
        return 0;
    }
    if (!tagpu_puppet_gates_in()) {
        snprintf(why, cap, "the puppet gates are not in (tagpu_puppet.c)");
        return 0;
    }
    if (!tagpu_reclaim_level_tracked()) {
        snprintf(why, cap, "the teardown wrap is not in (tagpu_reclaim.c), so nothing would end "
                 "the puppet level before its units are destroyed");
        return 0;
    }
    if (!tagpu_rec_hooks_in()) {
        snprintf(why, cap, "the recorder's event hooks are not in (tagpu_rec_init), where the "
                 "replay takes the feature draws, a death's explosion and the footprint writes");
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
    /* the engine sizes the share list by the count (0x4938B7..0x4938C7) and copies every active
       record that is neither local nor a watcher into it (0x4938FF..0x4939BA): every seat */
    if (h->nplayers != h->nseats) {
        snprintf(why, cap, "it counts %u players and seats %u", h->nplayers, h->nseats);
        return 0;
    }
    /* and indexes its side and colour tables with a seat's side and colour unbounded
       (0x48692A..0x486943, 0x467C18..0x467C24) */
    for (i = 0; i < h->nseats; i++) {
        unsigned sides = *(const unsigned*)(ta + OFF_SIDES), colours = colour_count(ta);
        if (h->seats[i].side >= sides || h->seats[i].logo >= colours) {
            snprintf(why, cap, "seat %u's side %u or colour %u is not one of this game's %u "
                     "sides and %u colours", h->seats[i].index, h->seats[i].side,
                     h->seats[i].logo, sides, colours);
            return 0;
        }
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

/* §6.4's seating, after validate; `view` the viewed player, V or a recorded seat (§2.9) */
static void seat_level(char* ta, const RecHeader* h, unsigned v, unsigned view)
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
       player's. What V's PlayerInfo held goes back after the teardown (THE VIEWER'S SEAT). */
    info = *(unsigned char**)(player_rec(ta, v) + PL_INFO);
    s_vinfo.at = info;
    if (info) memcpy(s_vinfo.was, info, REC_INFO_BYTES);
    E_ActivateSeat(v, 3);
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
    ta[OFF_VIEWED] = (char)view;
    *(unsigned short*)(ta + OFF_NPLAYERS) = (unsigned short)h->nplayers;
    objective = *(void**)(ta + OFF_OBJECTIVE);
    if (objective) E_Disarm(objective);
    tagpu_puppet_begin(seatv, localv);
}

static void __cdecl replay_fill(void)
{
    char* ta = ta_main();
    char name[NAME_MAX_], path[MAX_PATH], why[200];
    unsigned v = 0, i;
    int k, view = -1;
    replay_close();
    tagpu_puppet_end();
    k = request_read(name, sizeof name, &view, why, sizeof why);
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
    for (i = 0; view >= 0 && i < recr_header(s_rd)->nseats; i++)
        if (recr_header(s_rd)->seats[i].index == (unsigned)view) break;
    if (view >= 0 && i == recr_header(s_rd)->nseats) {
        tagpu_logf("replay: not playing %s: the request's view names record %d, which the "
                   "recording does not seat", name, view);
        replay_close();
        return;
    }
    s_view = view >= 0 ? (unsigned)view : v;
    seat_level(ta, recr_header(s_rd), v, s_view);
    snprintf(s_name, sizeof s_name, "%s", name);
    s_viewer = v;
    s_first = 1;
    tagpu_logf("replay: %s seated: %u players on %s, the viewer at record %u", name,
               recr_header(s_rd)->nseats, recr_header(s_rd)->settings_map, v);
    if (s_seek.pending)
        tagpu_logf("replay: %s: a seek to GameTime %u waits for the first step", name, s_seek.T);
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
    s_opt.played = s_opt.stale = 0;
    memcpy(set + SET_OPTIONS, &h->cmdrdeath, 4);
    for (b = 0; b < 3; b++) {
        char* f = set + SET_OPTIONS + 4 + 4 * b;
        *f = (char)((*f & ~1) | (h->lostype >> b & 1));
    }
    memcpy(set + SET_OPTIONS + 16, &h->fixed_starts, 4);
    *(unsigned*)(ta + OFF_DIFFICULTY) = h->difficulty;
    *(unsigned short*)(ta + OFF_EACH_MENU) = (unsigned short)h->units_each;
}

/* 1 when the recording's options were in and the player's are back */
static int options_restore(char* ta)
{
    char* set;
    if (!s_opt.saved) return 0;
    s_opt.saved = 0;
    if (!ta) return 0;
    set = *(char**)(ta + OFF_SETTINGS);
    if (set) memcpy(set + SET_OPTIONS, s_opt.set, sizeof s_opt.set);
    *(unsigned*)(ta + OFF_DIFFICULTY) = s_opt.difficulty;
    *(unsigned short*)(ta + OFF_EACH_MENU) = s_opt.each;
    return 1;
}

/* THE PLAYER'S SETTINGS. From S-b to the teardown the live settings hold the recording's options,
   and the settings save 0x430F00 writes their fields to the registry (the difficulty at 0x4311D1,
   the settings' +0x108..+0x118 at 0x430F80 and 0x4314F6..0x43157E). Of its 27 callers the in-game
   Options' OK
   (0x45FD5F) and the damage-bars key (0x496077) run during a playback; the loader's (0x497FF6) is
   skipped for a skirmish, whose mode is 2 (0x497FEC..0x497FF4). The level reads the difficulty
   throughout (0x41BBBD, 0x4654AF, 0x486CE6), so the recording's options stay in until the
   teardown, and every save the level makes runs with the player's exchanged in for its
   duration: an observer on 0x430F00 exchanges them on entry and again on return. The save itself
   runs and returns as it always does -- handlers hand its EAX back to their own callers
   (0x4462FC, 0x477C92, fourteen typed-command handlers), so it is never skipped.
   The exchange waits for the level's first step: until then the loader thread may be reading the
   settings (0x4973C6..0x49790E), and the loader's last act sets the load word's bit 1 (0x497C5F),
   which the game thread reads before the level runs (0x498342), so no loader read follows the
   first step. A save before it, which no known path makes, writes the recording's options, and
   the teardown then saves once more with the player's back. No call of the save is on the loader
   thread: the loader's own (0x497FF6) is in the game thread's load state 0x497F40, which starts
   the loader thread only after it (0x4982C5..0x4982CA). The save calls nothing that calls it
   again (0x4B6A20, 0x4B6A50, 0x4CF220, 0x4CFFF0, 0x4D0040 and imports), so one return address
   is in flight at a time. */
static void* s_saveRet;

/* the live options and s_opt's, each put in the other's place */
static void options_exchange(char* ta)
{
    char* set = *(char**)(ta + OFF_SETTINGS);
    unsigned char t[sizeof s_opt.set];
    unsigned d = *(unsigned*)(ta + OFF_DIFFICULTY);
    unsigned short e = *(unsigned short*)(ta + OFF_EACH_MENU);
    if (set) {
        memcpy(t, set + SET_OPTIONS, sizeof t);
        memcpy(set + SET_OPTIONS, s_opt.set, sizeof t);
        memcpy(s_opt.set, t, sizeof t);
    }
    *(unsigned*)(ta + OFF_DIFFICULTY) = s_opt.difficulty;
    s_opt.difficulty = d;
    *(unsigned short*)(ta + OFF_EACH_MENU) = s_opt.each;
    s_opt.each = e;
}

static int __cdecl save_before(void* entry_esp)
{
    char* ta = ta_main();
    if (!s_opt.saved || !ta) return 0;
    if (!s_opt.played) {
        s_opt.stale = 1;
        return 0;
    }
    s_saveRet = ((void**)entry_esp)[0];
    options_exchange(ta);
    return 1;
}

static void* __cdecl save_after(unsigned* regs)
{
    (void)regs;
    options_exchange(ta_main());
    return s_saveRet;
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
   every unit's sight from the positions the unit tick left (0x4827B0, 0x465046..0x465063), and a
   seat's 30-tick pass runs at the GameTime it ran at there (A PLAYER'S IMAGE). At the next step's
   capture point the host holds image T+1, which is where a dump (`dump <name>` in
   tagpu_replay.trigger) is taken and compared.
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
   4. the ID rows that name a weapon or the last attacker, each turned back into its pointer;
   5. the control group +0xAC, through the engine's setter (group_put).
   Never written: the position, cell, layer, sight stamp, walk class and water state, which the
   engine functions above write; what the create and the level init wrote (the type and its index,
   the slot, the owner); the carry links and the attach piece (0x48AB70's); the state byte
   (0x48B090's); the yard's bit, +0x10F bit 2 (YARD_OPEN's); the create's +0x110 bits 28-31; and
   the contact bits 8-10 and 12, which the host's radar pass computes for its own view (D1). The
   harness compares them all but the contact bits (tools/replay/tarec.py compare --puppet), and
   bits 8-10 too when the host shows the recording's own viewed player. +0x110 bit 14, dying, is
   a value like the rest: the reaper's Send_UnitDeath is refused for a puppet (tagpu_puppet.c),
   so a unit the recording marks dying lives until its recorded death, in the step it died in
   there.

   A PLAYER'S IMAGE is written raw under the same exclusions, every record's but V's, and but
   +0xF0, the players phase's 30-tick deadline (0x46507D..0x465092). Image T+1 carries it as the
   recording machine's phase of T left it, already advanced, so written it would keep the host's
   phase from ever finding a seat's pass due -- and the viewed record's radar pass 0x467440 and
   minimap fog 0x466C20 run only in that pass (0x46556D, 0x465572). Both machines' deadlines start
   at the load's GameTime (0x464700) and step by 30 whenever GameTime reaches them, and the host
   plays one recorded iteration a GameTime, so the host's own agrees; it is compared. Each step
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
#define PL_PASS        0xF0         /* the players phase's 30-tick deadline, the host's own        */
#define PL_BLOCKFIRST  0x67         /* the record's first and last unit, inclusive                 */
#define PL_BLOCKLAST   0x6B
#define U_ALT          0x6E
#define U_TYPEIX       0xA6         /* u16, the type's index; 0 for a free slot (0x486DC7)          */
#define U_GROUP        0xAC         /* the control group, or -1: the setter 0x480250's alone        */
#define GROUPS         10           /* the owner's group table: 10 entries of 0x20 (0x480190)     */
#define U_ATTACKER     0xF0
#define U_OWNERIX      0xFF
#define U_BUILT        0x104        /* the build left, a float: 0.0 when finished                  */
#define U_STATE        0x110
#define U_ALIVE        0x10000000u
#define U_WATER        0x10A        /* the water state, 0..4: 0x43DB50 its only writer and reader,
                                       a dword, read and stored whole (0x43DB6B, 0x43DBED)        */
#define WATER_MAX      4
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
   12), the fix-up's bit (16, step 3), carried (17), and the create's 28-31 */
#define U_ENGINE_BITS  0xF003170Fu
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
/* 0x480250(unit, g): the control-group setter, stdcall `ret 8` (DISASSEMBLED 0x480250..0x480455):
   it removes the unit from the vector its +0xAC names when it finds it there, appends it to
   vector g, and stores +0xAC = g on every path (0x4803A9, 0x48044C); g = -1 only removes */
typedef void (__stdcall* PFN_SetGroup)(char* unit, int g);
#define E_SetGroup ((PFN_SetGroup)0x00480250u)
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
    unsigned sides, colours;
    unsigned now;
    char*    grid;          /* the feature grid, gw x gh cells (THE FEATURES)                  */
    unsigned gw, gh;
    char*    pool;          /* the wreck pool, nrec records                                    */
    unsigned nrec;
    char*    fdefs;         /* the feature defs, nfdefs of them                                */
    unsigned nfdefs;
} Lv;

static uint32_t s_raw1[REC_UNIT_DWORDS], s_raw2[REC_UNIT_DWORDS], s_praw[REC_PLAYER_DWORDS];
static uint32_t s_state[REC_UNIT_DWORDS];         /* +0x110 as the last write takes it        */
static uint32_t s_oraw[REC_ORDER_IMAGE / 4];      /* an order node's values                   */
static uint32_t s_oclr[REC_ORDER_IMAGE / 4], s_oset[REC_ORDER_IMAGE / 4];   /* its puppet rows */
static uint32_t s_jval[REC_PROJ_DWORDS];          /* a projectile record's value bytes         */
static int      s_masks;
static void feat_masks_init(void);
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
    m_bytes(s_jval, 0, REC_PROJ_BYTES, 1);
    /* the state write takes +0x110 less the engine's bits and every row of the exclusion file:
       the selection, marks and build page are the viewer's own (drop) */
    m_field(s_state, U_STATE, 4, ~U_ENGINE_BITS, 1);
    for (i = 0; i < sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0]; i++) {
        const ReplayLayoutRow* r = &tagpu_replay_layout[i];
        if (r->record == RL_UNIT) {
            m_field(s_raw1, r->offset, r->size, r->mask, 0);
            m_field(s_state, r->offset, r->size, r->mask, 0);
        }
        if (r->record == RL_MOVER) m_field(s_raw1, REC_MOVER_AT + r->offset, r->size, r->mask, 0);
        if (r->record == RL_PLAYER) m_field(s_praw, r->offset, r->size, r->mask, 0);
        if (r->record == RL_INFO) m_field(s_praw, REC_INFO_AT + r->offset, r->size, r->mask, 0);
        if (r->record == RL_PROJ) m_field(s_jval, r->offset, r->size, r->mask, 0);
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
    m_bytes(s_raw1, U_GROUP, 4, 0);         /* the control group, group_put's              */
    m_bytes(s_raw1, 0x10A, 1, 0);           /* the water state                                 */
    m_bytes(s_raw1, 0x10E, 1, 0);           /* the state byte                                  */
    m_field(s_raw1, U_YARD, 1, 0x04, 0);    /* the yard, YARD_OPEN's                           */
    m_field(s_raw1, U_STATE, 4, U_ENGINE_BITS, 0);
    m_bytes(s_raw1, REC_MOVER_AT + 0x26, 8, 0);
    m_bytes(s_raw2, 0x64, 6, 1);            /* the turn words                                  */
    m_bytes(s_raw2, U_ALT, 4, 1);
    m_field(s_raw2, U_STATE, 4, 0x10000u, 1);
    m_bytes(s_raw2, REC_MOVER_AT + 0x26, 8, 1);
    m_bytes(s_praw, PL_PASS, 4, 0);
    m_bytes(s_oraw, 0, 4, 0);               /* the vtables, the constructor's                  */
    m_bytes(s_oraw, ORD_REF, 4, 0);
    feat_masks_init();
    s_masks = 1;
}

/* the level as the apply needs it; 0 when the unit array or the features are not the ones the
   first step checked */
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
    L->sides = *(const unsigned*)(ta + OFF_SIDES);
    L->colours = colour_count(ta);
    L->now = *(const unsigned*)(ta + OFF_GAMETIME);
    L->grid = *(char**)(ta + 0x14287);
    L->gw = *(const unsigned*)(ta + 0x14233);
    L->gh = *(const unsigned*)(ta + 0x14237);
    L->pool = *(char**)(ta + 0x1420B);
    L->nrec = TAGPU_LIM_WRECKS;
    L->fdefs = *(char**)(ta + 0x1426F);
    L->nfdefs = *(const unsigned*)(ta + 0x14253);
    /* the grid, the pool and the defs the first step checked against the recording's: a load
       makes them and only the teardown frees them (tagpu_recorder.c, *The features*) */
    return L->grid && L->pool && L->fdefs && L->gw == recr_header(s_rd)->grid_w &&
           L->gh == recr_header(s_rd)->grid_h && L->nrec == recr_header(s_rd)->wrecks &&
           L->nfdefs == recr_header(s_rd)->nfdefs;
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

/* A UNIT'S CONTROL GROUP +0xAC goes in through the engine's own setter, never raw. Two
   invariants rest on it:
   - the bound: the owner's group table is 10 entries of 0x20 bytes (0x480190 allocates 0x140,
     DISASSEMBLED 0x480197..0x4801DE), and the setter indexes it with g and with the unit's
     current +0xAC unchecked (0x48026F, 0x4802D7), so it is called only when both are -1 or below
     10; otherwise the host's group stays, counted;
   - the pairing: a unit's +0xAC names the vector that holds it, which the destructor's removal
     0x480250(unit, -1) relies on (0x4867AF). The create appends every unit to group 0 (0x485D29);
     a raw +0xAC = g would leave it there, and at its death the removal would search vector g,
     miss, and leave a pointer to the dead unit in vector 0. */
static void group_put(const Lv* L, char* u, const uint32_t* img, unsigned slot)
{
    int32_t want = (int32_t)img[U_GROUP / 4], have;
    memcpy(&have, u + U_GROUP, 4);
    if (want == have) return;
    if ((want != -1 && (uint32_t)want >= GROUPS) || (have != -1 && (uint32_t)have >= GROUPS)) {
        s_st.grefused++;
        note("replay: GameTime %u: unit %u's control group %d not set from %d", L->now, slot,
             want, have);
        return;
    }
    E_SetGroup(u, want);
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
    group_put(L, u, img, slot);
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
    tagpu_puppet_open(1);           /* what it nests is the recording's (THE OTHER EVENTS) */
    u = E_Create(owner, type, pos[0], pos[1], pos[2], finished, layer, slot);
    tagpu_puppet_open(0);
    if (u != unit_at(L, slot)) {
        s_st.refused++;
        note("replay: the create of slot %u gave %s %u", slot, u ? "slot" : "nothing,",
             u ? (unsigned)((u - L->begin) / REC_UNIT_BYTES) : 0);
        return 0;
    }
    return 1;
}

/* every unit of the world's image, made as the loader made it, at the first step and at a seek;
   how many were made */
static unsigned level_units(const Lv* L, const RecWorld* W)
{
    unsigned slot, made = 0;
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
            made++;
    }
    return made;
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
   and metal, 0x465EF5..0x465F1A), and which no loop over the players reads while inactive. An
   image goes in only with each name ended inside its field, since the engine copies a name to
   its end (0x49395F..0x493977, into a list of 0x1E bytes a player); and a seat's PlayerInfo image
   only with its side and colour within this game's counts, which the engine indexes its tables
   with unbounded (validate). The other records' go in as recorded: the engine itself leaves
   colours outside its count in the records it does not seat (0xFF from the battleroom, 0x45133E;
   the record's index from the reset, 0x46461E). */
static void players_put(const Lv* L, const RecWorld* W)
{
    unsigned p;
    for (p = 0; p < REC_PLAYERS; p++) {
        const unsigned char* rimg = (const unsigned char*)W->player[p];
        const unsigned char* img = rimg + REC_INFO_AT;
        char* rec;
        char* info;
        if (p == s_viewer) continue;
        if (!memchr(rimg + PL_NAME, 0, PL_NAMELEN) || !memchr(rimg + PL_NAME2, 0, PL_NAMELEN)) {
            s_st.bad_ids++;
            note("replay: GameTime %u: record %u's name runs past its field; its image is left",
                 L->now, p);
            continue;
        }
        rec = player_rec(L->ta, p);
        put_masked(rec, W->player[p], s_praw, 0, REC_PLAYER_BYTES);
        memcpy(&info, rec + PL_INFO, 4);
        if (!info) continue;
        if (tagpu_puppet_seat(p) && (img[INFO_SIDE] >= L->sides || img[INFO_LOGO] >= L->colours)) {
            s_st.bad_ids++;
            note("replay: GameTime %u: record %u's side %u or colour %u is not one of this game's; "
                 "its PlayerInfo is left", L->now, p, img[INFO_SIDE], img[INFO_LOGO]);
            continue;
        }
        put_masked(info, W->player[p], s_praw, REC_INFO_AT, REC_INFO_BYTES);
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

/* A node built from its image and linked at the tail of `list`; 0 when no block. Its target ends
   as ord_patch sets one: the constructor 0x43A0C0 sets the reference through 0x4895C0 (0x43A0E3)
   and, when the action's descriptor flags lack bit 9 (+0x42 & 0x200, copied at
   0x43A18F..0x43A196), clears it again with 0x489690(NULL) (0x43A1B6..0x43A1C1); the recording
   machine's handlers set such a node's target afterwards through 0x489690 (DISASSEMBLED; MEASURED
   2026-10-01, action 0x19's node in the stock fixture names the nanoframe 502 with bit 9 clear).
   So the target goes through the same setter, bounded by ord_target as every later step's is. */
static int ord_build(const Lv* L, unsigned slot, unsigned list, const unsigned char* img)
{
    char* unit = unit_at(L, slot);
    char* n = (char*)E_MemAlloc(REC_ORDER_BYTES);
    char* target;
    const char* ht;
    uint32_t rid, flags, want;
    int32_t pos[3];
    if (!n) return 0;
    memcpy(&rid, img + ORD_TARGET, 4);
    memcpy(pos, img + 0x22, 12);
    target = ord_target(L, rid);
    E_OrdMake(n, img[ORD_ACTION], target, pos, 0, 0, 0);
    ord_values(L, n, img, slot);
    /* the linker picks the list by the node's flag: the recorded flag when it agrees with the
       list the recording holds the node on, which the engine's linkers keep; else it is set for
       the link and the recorded one written back (the recorder counts such nodes) */
    memcpy(&flags, n + ORD_FLAGS, 4);
    want = list ? (flags | OF_SUBLIST) : (flags & ~OF_SUBLIST);
    memcpy(n + ORD_FLAGS, &want, 4);
    E_OrdLink(unit, n);
    memcpy(n + ORD_FLAGS, &flags, 4);
    memcpy(&ht, n + ORD_TARGET, 4);
    if (ht != target) E_RefSet(n + ORD_REF, target);
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

/* RK_EV_CREATE: the create with the recorded arguments, into the recorded slot, then the water
   state the recording machine's unit took from its slot. A create leaves the water state as its
   slot held it, and 0x43DB50 keeps a value while the unit stays in the band where no test fires
   (0x43DB78..0x43DBCC). A unit's water states are therefore the recording machine's only if it
   starts from the same value. Without this, a create played after a seek, in a slot dead at T,
   starts from the seek host's history. The value is bounded by the 0..4 that 0x43DB50 stores
   (0x43DB84..0x43DBC7). It is written whole, as 0x43DB50 reads and stores it, before the unit's
   first E_Water at the next images, so that call starts setSFXoccupy exactly where the recording
   machine's 0x43DB50 did. */
static int ev_create(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id, finished, water;
    unsigned short type;
    int32_t pos[3];
    if (len != 28) return 0;
    memcpy(&id, b, 4);
    memcpy(&type, b + 4, 2);
    memcpy(&finished, b + 8, 4);
    memcpy(pos, b + 12, 12);
    if (!make(L, id - 1, b[6], type, pos, finished, b[7] & 3u)) return 0;
    water = b[24];
    if (water <= WATER_MAX) {
        memcpy(unit_at(L, id - 1) + U_WATER, &water, 4);
    } else {
        s_st.bad_ids++;
        note("replay: GameTime %u: unit %u's create carries water state %u, past the 0..4 "
             "0x43DB50 stores: the host's stays", L->now, id - 1, water);
    }
    return 1;
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
     (0x486148), the SETs of its Create script's first slice, which 0x485D40 runs at once, and the
     replies they make: the replayed create makes them again, with the puppet gates open around
     it (make, tagpu_puppet_open);
   - those a death makes after its record: the destructor's order purge (0x4867A1 -> 0x439EB0)
     tears the dying unit's nodes down, and a node's destructor starts its TargetCleared unless
     +0x42 bit 16 is set (0x43A2AB..0x43A2B9) and broadcasts its StopBuilding when bit 22 is
     (0x43A21E..0x43A25D). A puppet's nodes hold bit 16 set and bit 22 clear (orders_put), so its
     replayed death makes neither, on a unit that is no longer live.
   A reply is played with the gates open too. Every other reply the host would make about a
   puppet's unit is refused (tagpu_puppet.c) -- the one inside a replayed state change above all,
   whose recorded reply follows it -- so each recorded reply is heard once. */

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
    tagpu_puppet_open(1);
    (b[8] ? E_Reply1 : E_Reply0)(u, kind, b[9] ? text : NULL);
    tagpu_puppet_open(0);
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

/* ---- the features --------------------------------------------------------------------------- */

/* THE FEATURES (the slice note's S3a). The recording carries the feature grid and the wreck pool
   as images, and every change to them as the call that made it: a feature root, RK_EV_FEATURE to
   its END, or a unit's death, RK_EV_DEATH to its END (tagpu_recfmt.h). From the level's first
   step the feature functions refuse every call the host would make by itself (tagpu_puppet.c),
   and the replay makes each change through the function that made it there, in recorded order:
   - A ROOT is called with its recorded arguments, each bounded first: a cell by the grid's columns
     and rows -- FeatureDie indexes the grid with its x and z and never tests the cell it gets
     (0x423550), and the damage hands its own x and z to the burn and to FeatureDie
     (0x4245C6, 0x42465A) -- a def by the defs' count, a weapon by the weapons'. The draws it made
     are armed around it (tagpu_puppet_draws): the feature code's six calls of the simulation RNG
     take the recording's values, in order, while their site and argument agree. The tick is the
     root that also carries cells: the grid's changes since the iteration's images, put in before
     it runs, since its reproduction reads the source cell's unit slot (0x424189).
   - A DEATH is replayed as ev_death replays it, with the feature gates open: the host's destructor
     makes what every machine makes, its corpse through 0x486360 (0x486D6F). The damage of its
     explosion it cannot make: the owner gate (0x49A01B..0x49A047) passes a type-3 owner's
     explosion by, and every puppet seat is one. So the roots that explosion made, which the
     recording carries inside the death's bracket, are made where the host's destructor makes
     its explosion (tagpu_replay_blast, once 0x49B000 has returned at 0x486D50): before the
     corpse, in the engine's order. A root the
     host's destructor did not reach, and a draw inside a death that no root holds, are counted.
     Deaths do not nest in stock (tagpu_recorder.c, *The features*); one that did would be
     replayed after the outer one, its roots at its own explosion.
   - What a root returned there is compared with what it returns here where that is a result
     (RK_EV_FEATURE_END), and a draw a call did not take as recorded is counted: each names a
     divergence, and nothing here repairs one.
   THE IMAGES then go in: a cell's and a record's values raw, their structure compared and never
   written -- a cell's def, whether it holds a record, and the record or a footprint cell's way
   back to its anchor; a record's links, anchor, def, flags and sequences; the lists' heads. Each is
   an invariant between the grid, the pool and its lists that only the engine's functions keep, so
   a difference is counted and the first few named, and the cell or record is left as it is. A
   value that indexes is bounded before it goes in: a cell's unit slots by the unit array's count,
   a sequence's frame by the sequence's own count, as 0x4B8B30 bounds it, and the reproduction
   cursor by the grid's cells, the range the tick keeps it in (0x4240A3..0x4240CC).
   THE UNIT SLOTS AND THE YARD BIT come from the images alone: every write the stamp, the clear and
   the re-claim make to them on this level -- for the placements below, the creates and deaths,
   and the host's own unit tick -- is undone at the writer's return (tagpu_puppet.c, THE
   FOOTPRINT WRITERS), so the cells the recording did not change keep the recording's values. */

#define OFF_FGRID      0x14287      /* the feature grid (tagpu_recorder.c, *The features*)          */
#define OFF_FGRIDW     0x14233
#define OFF_FGRIDH     0x14237
#define OFF_NFDEFS     0x14253
#define OFF_FDEFS      0x1426F
#define OFF_WRECKS     0x1420B      /* the wreck pool, TAGPU_LIM_WRECKS records                    */
#define OFF_WR_HEADS   0x14213      /* the active, settled and free lists' heads, i32 each         */
#define OFF_FCURSOR    0x14257      /* the reproduction cursor                                    */
#define FD_STRIDE      0x100
#define FD_MASK        0xFE         /* bit 0: a GAF feature                                       */
#define CELL_DEF       0x08
#define CELL_BACK      0x0A         /* a footprint cell's rows and columns back to its anchor, an
                                       anchor's record, or a resting GAF anchor's damage         */
#define CELL_FLAGS     0x0C         /* bit 0: the anchor holds a record                           */
#define DEF_FOOTPRINT  0xFFFE
#define WR_STATE1      0x04         /* a GAF record's sequence states: u16 frame, u16 timer, u8
                                       loop, the sequence at +8 (0x4B8B30)                        */
#define WR_STATE2      0x10
#define FDRAWS_MAX     4096         /* the draws one call may be armed with                       */

/* The feature functions (tagpu_recfmt.h, RecRoot), stdcall: the tick 0x424050, `ret`; the spawn
   0x423C50(cell, def, pos, turn, v), `ret 0x14`; the destroy 0x4246B0(cell, force), `ret 8`;
   FeatureDie 0x423550, the burn 0x4233A0 and the swap 0x423710 (x, z, a), `ret 0xC` each; the
   damage 0x4244B0(cell, x, z, weapon), `ret 0x10` */
typedef void (__stdcall* PFN_FTick)(void);
typedef uint32_t (__stdcall* PFN_FSpawn)(char* cell, unsigned def, const int32_t* pos,
                                         const uint16_t* turn, unsigned v);
typedef uint32_t (__stdcall* PFN_FDestroy)(char* cell, unsigned force);
typedef uint32_t (__stdcall* PFN_FAt)(int32_t x, int32_t z, unsigned a);
typedef uint32_t (__stdcall* PFN_FDamage)(char* cell, int32_t x, int32_t z, const char* weapon);
#define E_FTick    ((PFN_FTick)0x00424050u)
#define E_FSpawn   ((PFN_FSpawn)0x00423C50u)
#define E_FDestroy ((PFN_FDestroy)0x004246B0u)
#define E_FDie     ((PFN_FAt)0x00423550u)
#define E_FBurn    ((PFN_FAt)0x004233A0u)
#define E_FSwap    ((PFN_FAt)0x00423710u)
#define E_FDamage  ((PFN_FDamage)0x004244B0u)

static unsigned char s_cval[REC_CELL_BYTES];                  /* a cell's values               */
static unsigned char s_w3val[REC_WRECK_BYTES], s_wgval[REC_WRECK_BYTES];   /* a record's        */
static PuppetDraw    s_fq[FDRAWS_MAX];                        /* the draws armed for a call    */

/* the value masks from the exclusion file (RL_CELL, RL_WRECK3DO, RL_WRECKGAF) less the structure
   the images never write */
static void feat_masks_init(void)
{
    unsigned i, k;
    memset(s_cval, 0xFF, sizeof s_cval);
    memset(s_w3val, 0xFF, sizeof s_w3val);
    memset(s_wgval, 0xFF, sizeof s_wgval);
    for (i = 0; i < sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0]; i++) {
        const ReplayLayoutRow* r = &tagpu_replay_layout[i];
        unsigned char* m = r->record == RL_CELL ? s_cval : r->record == RL_WRECK3DO ? s_w3val :
                           r->record == RL_WRECKGAF ? s_wgval : NULL;
        if (!m) continue;
        for (k = 0; k < r->size; k++)
            m[r->offset + k] &= (unsigned char)(r->mask ? ~(r->mask >> (8 * k)) : 0);
    }
    s_cval[CELL_DEF] = s_cval[CELL_DEF + 1] = 0;
    s_cval[CELL_FLAGS] &= (unsigned char)~1u;
    for (k = 0; k < 4; k++) s_w3val[k] = s_wgval[k] = 0;                         /* the links   */
    for (k = REC_WRECK_ANCHOR; k < REC_WRECK_DEF + 2; k++) s_w3val[k] = s_wgval[k] = 0;
    s_w3val[REC_WRECK_FLAGS] = s_wgval[REC_WRECK_FLAGS] = 0;
}

static const char* fdef_of(const Lv* L, unsigned def)
{
    return def < L->nfdefs ? L->fdefs + (size_t)def * FD_STRIDE : NULL;
}

/* a cell of the grid, its column and row bounded; NULL outside it */
static char* cell_at(const Lv* L, int32_t x, int32_t z)
{
    if (x < 0 || z < 0 || (uint32_t)x >= L->gw || (uint32_t)z >= L->gh) return NULL;
    return L->grid + ((size_t)(uint32_t)z * L->gw + (uint32_t)x) * REC_CELL_BYTES;
}

/* the def field that holds `v`, as its ID (tagpu_recfmt.h, REC_SEQ_FIRST), as the recorder
   takes it */
static uint32_t seq_id(const char* def, uint32_t v)
{
    unsigned i;
    if (!v) return REC_ID_NULL;
    for (i = 0; def && i < REC_SEQ_FIELDS; i++)
        if (*(const uint32_t*)(def + REC_SEQ_FIRST + 4 * i) == v) return i + 1;
    return REC_ID_BAD;
}

/* a record's ID from what a spawn or a swap returned, as the recorder takes it */
static uint32_t wreck_id(const Lv* L, uint32_t p)
{
    size_t off;
    if (!p) return REC_ID_NULL;
    if ((char*)(size_t)p < L->pool) return REC_ID_BAD;
    off = (size_t)((char*)(size_t)p - L->pool);
    if (off % REC_WRECK_BYTES || off / REC_WRECK_BYTES >= L->nrec) return REC_ID_BAD;
    return (uint32_t)(off / REC_WRECK_BYTES) + 1;
}

/* One cell's image into the host's cell: 1 when its structure agrees, the values then written.
   (x, z) only name it in a note. */
static int cell_put(const Lv* L, char* c, const unsigned char* img, unsigned x, unsigned z)
{
    uint16_t hdef, rdef, slot[2];
    unsigned char back = (unsigned char)(img[CELL_FLAGS] & 1), k;
    memcpy(&hdef, c + CELL_DEF, 2);
    memcpy(&rdef, img + CELL_DEF, 2);
    if (rdef == DEF_FOOTPRINT) back = 1;
    if (hdef != rdef || ((unsigned char)c[CELL_FLAGS] & 1) != (img[CELL_FLAGS] & 1) ||
        (back && memcmp(c + CELL_BACK, img + CELL_BACK, 2))) {
        if (!s_fst.cstruct++)
            note("replay: GameTime %u: the cell (%u, %u) holds def 0x%04X here, 0x%04X in the "
                 "recording: its structure is the engine's, and it is left as it is", L->now, x, z,
                 hdef, rdef);
        return 0;
    }
    memcpy(slot, img, 4);
    /* a slot is a unit the engine follows from the cell: the stamp's contest dereferences the
       incumbent's owner at +0x96 with no NULL test (0x47CDCA), which a slot no unit has taken
       holds as NULL. A recorded slot names a live unit, the destructor taking the unit off the
       grid (0x48682D) before it clears the alive bit (0x486DCE), so a slot goes in bounded and
       alive */
    if (slot[0] >= L->nslots || slot[1] >= L->nslots ||
        (slot[0] && !host_live(L, slot[0])) || (slot[1] && !host_live(L, slot[1]))) {
        s_fst.cbad++;
        return 0;
    }
    for (k = 0; k < REC_CELL_BYTES; k++) {
        unsigned char m = s_cval[k];
        if (back && (k == CELL_BACK || k == CELL_BACK + 1)) m = 0;
        c[k] = (char)(((unsigned char)c[k] & ~m) | (img[k] & m));
    }
    return 1;
}

/* an RK_CELLS's run into the grid */
static void cells_put(const Lv* L, const unsigned char* b)
{
    uint16_t h[4];
    unsigned k;
    memcpy(h, b, 8);        /* bounded by the reader (cells_ok) and again by cell_at */
    for (k = 0; k < h[2]; k++) {
        char* c = cell_at(L, (int32_t)h[0] + (int32_t)k, h[1]);
        if (c && cell_put(L, c, b + 8 + (size_t)k * REC_CELL_BYTES, h[0] + k, h[1]))
            s_fst.cells++;
    }
}

/* One record's image into the host's record, by the recorded def's layout. */
static void wreck_put(const Lv* L, unsigned i, const unsigned char* img)
{
    char* w = L->pool + (size_t)i * REC_WRECK_BYTES;
    uint16_t def;
    const char* d;
    const unsigned char* m;
    int gaf;
    uint32_t id[2] = { REC_ID_NULL, REC_ID_NULL };
    unsigned k;
    memcpy(&def, img + REC_WRECK_DEF, 2);
    d = fdef_of(L, def);
    gaf = d && (d[FD_MASK] & 1);
    m = gaf ? s_wgval : s_w3val;
    if (gaf) {                       /* the IDs as the recorder takes them (REC_WRECK_SHADOW) */
        id[0] = seq_id(d, *(const uint32_t*)(w + REC_WRECK_SEQ));
        if (w[REC_WRECK_FLAGS] & REC_WRECK_SHADOW)
            id[1] = seq_id(d, *(const uint32_t*)(w + REC_WRECK_SEQ2));
    }
    if (memcmp(w, img, 4) || memcmp(w + REC_WRECK_ANCHOR, img + REC_WRECK_ANCHOR, 6) ||
        w[REC_WRECK_FLAGS] != (char)img[REC_WRECK_FLAGS] ||
        (gaf && (id[0] != *(const uint32_t*)(img + REC_WRECK_SEQ) ||
                 id[1] != *(const uint32_t*)(img + REC_WRECK_SEQ2)))) {
        if (!s_fst.wstruct++)
            note("replay: GameTime %u: wreck record %u differs from the recording in its links, "
                 "anchor, def, flags or sequences: its structure is the engine's, and it is left "
                 "as it is", L->now, i);
        return;
    }
    if (gaf) {
        /* A state's frame is bounded by its sequence's count, and the count is read through the
           def's own field that the ID names, never through the record: a record's pointer is
           whatever its last tenant left until a state is in use (REC_WRECK_SHADOW). */
        static const unsigned char state[2] = { WR_STATE1, WR_STATE2 };
        for (k = 0; k < 2; k++) {
            const uint16_t* seq;
            uint16_t frame;
            if (id[k] < 1 || id[k] > REC_SEQ_FIELDS) continue;
            seq = *(const uint16_t* const*)(d + REC_SEQ_FIRST + 4 * (id[k] - 1));
            memcpy(&frame, img + state[k], 2);
            if (frame >= *seq) {
                s_fst.wbad++;
                return;
            }
        }
    }
    for (k = 0; k < REC_WRECK_BYTES; k++) w[k] = (char)(((unsigned char)w[k] & ~m[k]) | (img[k] & m[k]));
    s_fst.wrecks++;
}

static void heads_put(const Lv* L, const unsigned char* b)
{
    int32_t v[4];
    memcpy(v, b, 16);
    if (memcmp(L->ta + OFF_WR_HEADS, v, 12) && !s_fst.heads++)
        note("replay: GameTime %u: the wreck pool's heads are %d %d %d here, %d %d %d in the "
             "recording", L->now, *(const int32_t*)(L->ta + OFF_WR_HEADS),
             *(const int32_t*)(L->ta + OFF_WR_HEADS + 4), *(const int32_t*)(L->ta + OFF_WR_HEADS + 8),
             v[0], v[1], v[2]);
    if (v[3] >= 0 && (uint32_t)v[3] < L->gw * L->gh) memcpy(L->ta + OFF_FCURSOR, &v[3], 4);
    else s_fst.cursor++;
}

/* THE TICK'S INPUT. The runs inside the feature tick's bracket are the cells that changed since
   the iteration's images, as the recording machine's tick read them (tagpu_recorder.c,
   cells_tick), and root_play writes them before the tick. They are the replay's only write of its
   own between two images, and no image need carry them: the reader keeps them as events, out of
   its world's grid, so a cell the input changed and the rest of the step changed back is in
   neither image (MEASURED on links, 2026-10-01: the commander's move north wrote (121, 491) n 2
   as the tick's input, and the kill-all in the same step cleared its footprint; the image after
   it carries rows 492 and 493 only, and the host kept the slot to the level's end). So each run is
   noted here and written back from the world at the next images. With that, at every capture
   point the host's grid is the world's: the images carry every cell that changed between two
   images, the footprint writers' writes are undone at their return (tagpu_puppet.c), the feature
   calls are the recording machine's own with its inputs, and this write-back undoes the input.
   It takes the NEXT iteration's world: there the grid names only units live in its image, which
   the step's creates and deaths have made the host's (the delta's own precondition), so
   cell_put's slot rule refuses nothing it would not refuse in the delta. The step's own world
   would name a unit that died in the step. play runs events_put, iteration_read and images_put
   back to back, and nothing reads the grid between them. When the playing ends at the reader,
   the last step's runs keep the input: there is no world after the last step, no dump sees them
   (a dump is taken at a capture point, before that step's events), and a seek rewrites every
   cell (R10). */
static void tin_note(const unsigned char* b)
{
    if (s_tin.n == TIN_RUNS) {
        s_tin.overflow = 1;
        return;
    }
    memcpy(s_tin.run[s_tin.n++], b, 6);
}

static void tin_put(const Lv* L, const RecWorld* W)
{
    unsigned r, k, c, ncells = L->gw * L->gh;
    if (s_tin.overflow) {
        s_fst.tover++;
        for (c = 0; c < ncells; c++)
            if (cell_put(L, L->grid + (size_t)c * REC_CELL_BYTES, W->grid + (size_t)c * REC_CELL_BYTES,
                         c % L->gw, c / L->gw))
                s_fst.tcells++;
    } else {
        for (r = 0; r < s_tin.n; r++) {
            const uint16_t* run = s_tin.run[r];
            for (k = 0; k < run[2]; k++) {
                char* cell = cell_at(L, (int32_t)run[0] + (int32_t)k, run[1]);
                if (cell && cell_put(L, cell, W->grid + (cell - L->grid), run[0] + k, run[1]))
                    s_fst.tcells++;
            }
        }
    }
    s_tin.n = 0;
    s_tin.overflow = 0;
}

static void features_put(const Lv* L, const RecWorld* W)
{
    unsigned i;
    uint16_t ix;
    for (i = 0; i < W->nfeat; i++) {
        const unsigned char* b = W->feat[i].b;
        switch (W->feat[i].kind) {
        case RK_CELLS:
            cells_put(L, b);
            break;
        case RK_WRECK:
            memcpy(&ix, b, 2);
            if (ix < L->nrec) wreck_put(L, ix, b + 4);
            break;
        case RK_FEATURES:
            heads_put(L, b);
            break;
        }
    }
    tin_put(L, W);
}

/* the draws inside the root that opens at `i` into the queue; its RK_EV_FEATURE_END's index. A
   root's bracket holds no other, and the reader has checked that it closes inside the step
   (recr_tick). */
static unsigned draws_gather(const RecWorld* W, unsigned i, unsigned* n)
{
    unsigned j;
    *n = 0;
    for (j = i + 1; j < W->nev && W->ev[j].kind != RK_EV_FEATURE_END; j++) {
        const unsigned char* b = W->ev[j].b;
        if (W->ev[j].kind == RK_EV_DRAW && *n < FDRAWS_MAX) {
            s_fq[*n].site = b[0];
            memcpy(&s_fq[*n].n, b + 4, 4);
            memcpy(&s_fq[*n].v, b + 8, 4);
            (*n)++;
        }
    }
    return j;
}

/* the recorded arguments of a root, bounded, and the call; 0 when a bound refused it, with the
   result in `got` otherwise */
static int root_call(const Lv* L, unsigned root, const unsigned char* a, uint32_t* got)
{
    int32_t x, z, pos[3];
    uint16_t cx, cz, def, turn[3];
    uint32_t v, wid;
    char* cell;
    char* weapon;
    *got = 0;
    switch (root) {
    case FR_TICK:
        E_FTick();
        return 1;
    case FR_SPAWN:
        memcpy(&cx, a, 2);
        memcpy(&cz, a + 2, 2);
        memcpy(&def, a + 4, 2);
        memcpy(pos, a + 8, 12);
        memcpy(turn, a + 20, 6);
        if (!(cell = cell_at(L, cx, cz)) || def >= L->nfdefs) return 0;
        *got = wreck_id(L, E_FSpawn(cell, def, (a[7] & 1) ? pos : NULL, (a[7] & 2) ? turn : NULL,
                                    a[6]));
        return 1;
    case FR_DESTROY:
        memcpy(&cx, a, 2);
        memcpy(&cz, a + 2, 2);
        memcpy(&v, a + 4, 4);
        if (!(cell = cell_at(L, cx, cz))) return 0;
        *got = E_FDestroy(cell, v);
        return 1;
    case FR_DIE:
    case FR_BURN:
    case FR_SWAP:
        memcpy(&x, a, 4);
        memcpy(&z, a + 4, 4);
        memcpy(&v, a + 8, 4);
        if (!cell_at(L, x, z)) return 0;
        if (root == FR_DIE) E_FDie(x, z, v);
        else if (root == FR_BURN) E_FBurn(x, z, v);
        else *got = wreck_id(L, E_FSwap(x, z, v));
        return 1;
    case FR_DAMAGE:
        memcpy(&cx, a, 2);
        memcpy(&cz, a + 2, 2);
        memcpy(&x, a + 4, 4);
        memcpy(&z, a + 8, 4);
        memcpy(&wid, a + 12, 4);
        if (!(cell = cell_at(L, cx, cz)) || !cell_at(L, x, z) || !wid ||
            !weapon_ptr(L, wid, &weapon))
            return 0;
        E_FDamage(cell, x, z, weapon);
        return 1;
    default:
        return 0;
    }
}

/* The root that opens at event `i`: its cells if it is the tick, its draws armed, the call with
   the feature gates open, and its result compared. */
static void root_play(const Lv* L, const RecWorld* W, unsigned i)
{
    const unsigned char* b = W->ev[i].b;
    unsigned root = b[0], site, n, end, j, left;
    uint32_t want, got;
    int called, was;
    memcpy(&site, b + 4, 4);
    end = draws_gather(W, i, &n);
    if (end >= W->nev) return;
    memcpy(&want, W->ev[end].b + 4, 4);
    if (root == FR_TICK)
        for (j = i + 1; j < end; j++)
            if (W->ev[j].kind == RK_CELLS) {
                cells_put(L, W->ev[j].b);
                tin_note(W->ev[j].b);
            }
    tagpu_puppet_draws(s_fq, n);
    was = tagpu_puppet_features(1);
    called = root_call(L, root, b + 8, &got);
    tagpu_puppet_features(was);
    left = tagpu_puppet_draws_end();
    if (!called) {
        if (!s_fst.rrefused++)
            note("replay: GameTime %u: a feature call from 0x%06X is out of this level's bounds: "
                 "not made", L->now, site);
        return;
    }
    s_fst.roots++;
    s_fst.left += left;
    if ((root == FR_SPAWN || root == FR_SWAP || root == FR_DESTROY) && got != want &&
        !s_fst.rdiffer++)
        note("replay: GameTime %u: the feature call from 0x%06X returned %u here, %u in the "
             "recording", L->now, site, got, want);
}

/* The death being replayed: its bracket, for the explosion its destructor makes (THE FEATURES) */
static struct {
    const Lv*       L;
    const RecWorld* W;              /* NULL outside death_play                                    */
    unsigned        from, to;       /* its RK_EV_DEATH and RK_EV_DEATH_END                        */
    uint32_t        id;
    int             blasted;        /* its explosion's roots were made                            */
} s_dying;

/* the RK_EV_DEATH_END that closes the bracket opening at `i`; the reader has checked that it
   closes inside the step (recr_tick) */
static unsigned death_end(const RecWorld* W, unsigned i)
{
    unsigned j, depth = 0;
    for (j = i + 1; j < W->nev; j++) {
        if (W->ev[j].kind == RK_EV_DEATH) depth++;
        else if (W->ev[j].kind == RK_EV_DEATH_END && !depth--) break;
    }
    return j;
}

/* A death at event `i`: ev_death with the feature gates open; the roots its explosion made are
   made inside it (tagpu_replay_blast). */
static int death_play(const Lv* L, const RecWorld* W, unsigned i)
{
    unsigned end = death_end(W, i), j, depth = 0, roots = 0, stray = 0, inroot = 0;
    int done, was;
    for (j = i + 1; j < end; j++) {
        unsigned k = W->ev[j].kind;
        if (k == RK_EV_DEATH) depth++;
        else if (k == RK_EV_DEATH_END) depth--;
        else if (depth) continue;
        else if (k == RK_EV_FEATURE) roots++, inroot = 1;
        else if (k == RK_EV_FEATURE_END) inroot = 0;
        else if (k == RK_EV_DRAW && !inroot) stray++;
    }
    s_dying.L = L;
    s_dying.W = W;
    s_dying.from = i;
    s_dying.to = end;
    s_dying.blasted = 0;
    memcpy(&s_dying.id, W->ev[i].b, 4);
    was = tagpu_puppet_features(1);
    done = ev_death(L, W->ev[i].b, W->ev[i].len);
    tagpu_puppet_features(was);
    s_dying.W = NULL;
    if (roots && !s_dying.blasted && !s_fst.dmissed++)
        note("replay: GameTime %u: the death of slot %u made no explosion here, and %u feature "
             "calls its explosion made are not made", L->now, s_dying.id - 1, roots);
    s_fst.left += stray;
    s_fst.deaths++;
    return done;
}

void tagpu_replay_blast(const char* unit)
{
    const Lv* L = s_dying.L;
    const RecWorld* W = s_dying.W;
    unsigned j, depth = 0;
    if (!W || s_dying.blasted || s_dying.id - 1 >= L->nslots || unit != unit_at(L, s_dying.id - 1))
        return;
    s_dying.blasted = 1;
    for (j = s_dying.from + 1; j < s_dying.to; j++) {
        unsigned k = W->ev[j].kind;
        if (k == RK_EV_DEATH) depth++;
        else if (k == RK_EV_DEATH_END) depth--;
        else if (!depth && k == RK_EV_FEATURE) root_play(L, W, j);
    }
}

static void events_put(const Lv* L, const RecWorld* W)
{
    unsigned i, deaths = 0;
    for (i = 0; i < W->nev; i++) {
        const unsigned char* b = W->ev[i].b;
        unsigned n = W->ev[i].len, k = W->ev[i].kind;
        int done;
        switch (k) {
        case RK_EV_CREATE:      done = ev_create(L, b, n); break;
        case RK_EV_DEATH:       done = death_play(L, W, i); deaths++; break;
        case RK_EV_DEATH_END:   deaths--; continue;
        case RK_EV_FEATURE:     if (!deaths) root_play(L, W, i); continue;
        case RK_EV_SHOT:        done = ev_shot(L, b, n); break;
        case RK_EV_INTERCEPT:   done = ev_intercept(L, b, n); break;
        case RK_EV_START:       done = ev_start(L, b, n); break;
        case RK_EV_START_NAMED: done = ev_start_named(L, b, n); break;
        case RK_EV_BUILT:       done = ev_built(L, b, n); break;
        case RK_EV_STATE:       done = ev_state(L, b, n); break;
        case RK_EV_REPLY:       done = ev_reply(L, b, n); break;
        case RK_EV_SPRAY:       done = ev_spray(L, b, n); break;
        default:                continue;   /* a bracket's draws, the tick's cells, a root's end */
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
    features_put(L, W);
}

/* 0x466C20(): the minimap's fog composite, no argument, plain `ret` (0x466DB1; DISASSEMBLED
   0x466C20..0x466DB1). Only when main+0x142F1 bit 2 asks for it (0x466C29..0x466C44), it clears
   the bit and rebuilds the minimap's fog from the explored map and the viewed record's counters,
   then sets bit 1, DrawMinimap's redraw flag (0x466DA4). */
typedef void (__cdecl* PFN_MinimapFog)(void);
#define E_MinimapFog  ((PFN_MinimapFog)0x00466C20u)

/* FULL VISIBILITY (demo-recorder-s2-host.md §2.9), at the first step, once the loader's sight
   rebuild 0x4816A0(1) (0x497AE5) has filled every seat's: V's sight counters all 1, the value
   that rebuild writes when true LOS is off, and V's bit in every cell of the explored map, each
   within the engine's own size of it -- the counters' byte count at +0x88, and the map's bytes as
   0x483CF6 allocates them and 0x4816A0 fills them. V owns no unit, so no stamp ever takes a
   count or a bit away, and the level shows everything while V is viewed: a perspective is only
   the viewed player (s_view). Then the draw rebuilds its fog grid from the viewed player's sight
   (LosType bit 3 clear, on the game thread, as the camera's own clears are).
   The minimap's fog follows V's counters here too. 0x466C20 composites it from the explored map
   and the viewed record's counters, and only when main+0x142F1 bit 2 asks (0x466C29..0x466C44);
   the engine asks for the viewed player's own units' stamps, explored-map sets and removals
   (0x4822A1, 0x481D3B, 0x481D81) and in the sight rebuild, just before running it (0x481905,
   0x48191A). V owns no unit, so while V is viewed nothing else would ask, and the minimap would
   keep the rebuild's picture of V's base counters: all shaded under true LOS. So this asks and
   runs it as 0x4816A0 does. It composites whoever is viewed; in a seat's view it redoes that
   seat's picture from the same inputs. Its images exist from the level init (0x4919C3) to the
   teardown (0x491BAE), and so do the viewed record's counters, V's or a seated record's (the
   player init 0x464700 to 0x464A00); every caller runs between the first step and the teardown
   wrap, on the game thread, the one thread that reads them (the frame packet copies the images
   in the in-play draw).
   V's bit in the map is the one the engine's own setter 0x481930 sets for V, 1 << [V+0x146]
   (DISASSEMBLED 0x48194B..0x481951: `mov cl,[eax+0x146]; shl ebx,cl`), each entry a u16, so a
   value of 16 or more names no bit of it and nothing is set. */
static void sight_fill(char* ta, unsigned v)
{
    char* rec = player_rec(ta, v);
    unsigned char* cnt;
    unsigned char* mapped;
    uint32_t len, w, h, n, i;
    unsigned b = (unsigned char)rec[PL_MAPBIT];
    memcpy(&cnt, rec + PL_SIGHT, 4);
    memcpy(&len, rec + PL_SIGHTLEN, 4);
    if (cnt) memset(cnt, 1, len);
    memcpy(&mapped, ta + OFF_MAPPED, 4);
    memcpy(&w, ta + OFF_GRIDW, 4);
    memcpy(&h, ta + OFF_GRIDH, 4);
    n = (w * h * 2u) >> 2;
    if (mapped && b < 16)
        for (i = b >= 8; i < n; i += 2) mapped[i] |= (unsigned char)(1u << (b & 7));
    *(uint16_t*)(ta + OFF_LOSTYPE) &= (uint16_t)~8u;
    *(unsigned char*)(ta + OFF_MMFLAGS) |= 4u;
    E_MinimapFog();
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

/* ---- the globals and the projectiles -------------------------------------------------------- */

/* THE GLOBALS AND THE PROJECTILES (the slice note's S3b). The recording carries the fields of the
   main block and .data that replay-globals.tsv lists, as one image, and the projectile pool
   *(main+0x141F7) with its count main+0x141F3, every iteration. recorded_put writes the world's
   into the host, in this order:
   1. LosType bits 0-2, the list's `react` row, only when they differ from the host's, with the
      engine's own reaction (los_react);
   2. the list's `write` rows, under their masks; `hold` rows are never written on a puppet host;
   3. the pool: the count and the records of the recording's written extent, their pointers back
      from IDs, and zeros over what the host's extent has past the recording's;
   4. the explored map *(main+0x14273), with V's bit kept set (mapped_put).
   It runs at two points, each from the world as it stands there:
   - THE CAPTURE POINT of T, first in B2's stub (tagpu_replay_capture). At the capture point of T
     the world holds iteration T -- the step before read it -- whose globals and pool are the
     recording machine's at its own capture point of T, the same point of the step. Written here,
     before the recorder's capture and dump and before the unit tick, the replay is the last
     writer of each before anything in step T reads it. After the step's unit tick the host's
     projectile tick moves the pool for the picture and the sounds; the frame visit's write and
     the next capture point's replace what it computed.
   - THE FRAME VISIT after a step (tagpu_replay_frame), once a GameTime, when the world holds the
     next iteration: the draw after step T then shows iteration T+1's globals and pool, what the
     recording machine drew after its step T, since nothing between its step's end and its capture
     point of T+1 writes them in a skirmish but the pump [INFERRED: no skirmish message writes
     them]. Without it the draw would show the host's own projectile tick for a frame: burst copies
     at the host's RNG's angles (0x49B8D1, 0x49B903) and no meteor projectiles (the shower is gated
     on a puppet host). With it, the capture point's write finds nothing to change whenever a frame
     visit ran between two steps.
   Every `write` row is safe to write on a puppet host at each of them: each changes nothing a
   puppet host computes (replay-globals.tsv says why, row by row) and makes the host's bytes the
   recording's. The image is DATA: a slot is written within its own size, the generator having
   checked each inside the main block or .data; the pool's count and extent against the pool's
   records, and each pointer against its table (proj_put); the map within the host's own size of
   it (mapped_put). */

#define OFF_NPROJ      0x141F3      /* the projectile pool's count                               */
#define OFF_PROJ       0x141F7      /* the pool: TAGPU_LIM_PROJ records of REC_PROJ_BYTES (0x499A30;
                                       tagpu_limits.h: the raise goes in whole or not at all)  */
#define OFF_FOLLOW     0x142F7      /* the camera's followed projectile, or NULL: a pool record's
                                       position (+4) is all the camera reads (0x41CA3F..0x41CA49) */
#define OFF_SIGHTRECS  0x1427B      /* the temporary sight records, SR_STRIDE bytes each          */
#define OFF_NSIGHTRECS 0x14277      /* their count: the add refuses a 21st (0x48292C)             */
#define SR_STRIDE      0x24
#define SR_EXPIRY      0x1C         /* the expiry removes it below this GameTime (0x482153..)     */
#define SR_MAX         20
#define DATA_BEGIN     0x00501000u  /* .data: the list's data rows are offsets from here          */
#define LOS_REACT      0x7u         /* LosType bits 0-2: Mapping, true LOS, the ray fan           */
#define LOS_MAPPING    0x1u
/* 0x4816A0(arg): the sight rebuild every command that changes LosType bits 0-2 calls (stdcall,
   `ret 4` at 0x48192B; DISASSEMBLED, its calls: LOSType 0x4166B1, LOS 0x416D76 with 0, Mapping
   0x416DA6 with 1, NowISee 0x41755E): every record that passes its seat test filled with the
   mode's base, its in-line re-stamp of every live unit, LosType bit 3 cleared; with arg 1 the
   explored map filled too */
typedef void (__stdcall* PFN_SightRebuild)(unsigned arg);
#define E_SightRebuild ((PFN_SightRebuild)0x004816A0u)
/* 0x482130(): the temporary sight records' expiry, plain `ret` (DISASSEMBLED 0x482130..0x48226D):
   each record whose +0x1C is below GameTime has its stamp removed by 0x481D50 from the counters it
   stamped, in the mode it was made in, and the block is compacted */
typedef void (__cdecl* PFN_SightExpire)(void);
#define E_SightExpire ((PFN_SightExpire)0x00482130u)

/* the slot's bytes in the host: the main block's, or .data's */
static unsigned char* global_at(char* ta, const ReplayGlobalSlot* g)
{
    return g->where == RG_MAIN ? (unsigned char*)ta + g->offset
                               : (unsigned char*)(size_t)DATA_BEGIN + g->offset;
}

static void globals_put(char* ta, const RecWorld* W)
{
    unsigned i;
    for (i = 0; i < REC_GLOBALS_SLOTS; i++) {
        const ReplayGlobalSlot* g = &tagpu_replay_globals[i];
        unsigned char* at = global_at(ta, g);
        if (!g->write) continue;
        if (g->size <= 4) {
            uint32_t v = 0;
            memcpy(&v, at, g->size);
            v = (v & ~g->write) | (W->globals[g->dword] & g->write);
            memcpy(at, &v, g->size);
        } else {
            memcpy(at, W->globals + g->dword, g->size);   /* one row's: every byte (the header) */
        }
    }
}

/* Every temporary sight record dropped through the engine's own expiry: each one's +0x1C set to 0,
   then 0x482130, which removes every record whose expiry is below GameTime -- every one, GameTime
   being 1 or more wherever this runs, after a level's first step. The records are the viewed
   player's death vision, made by the destructor under true LOS only; a record kept past a refill
   of the counters would later decrement counters the refill reset (an undercount, or 0 -> 255, a
   cell seen for good), and the engine has no other way to drop one mid-level. The count is the
   engine's, which its add keeps at SR_MAX or below; -1 when it is not, and nothing is touched. */
static int sight_records_drop(char* ta)
{
    int n;
    char* recs;
    int i;
    memcpy(&n, ta + OFF_NSIGHTRECS, 4);
    memcpy(&recs, ta + OFF_SIGHTRECS, 4);
    if (n < 0 || n > SR_MAX || (n && !recs)) return -1;
    for (i = 0; i < n; i++) memset(recs + (size_t)i * SR_STRIDE + SR_EXPIRY, 0, 4);
    if (n) E_SightExpire();
    return n;
}

/* LosType bits 0-2 made the recording's, with the engine's own reaction: every command that
   changes them calls 0x4816A0 (the list's lostype row), and the bits written without it would
   leave the counters stamped in another mode than the one the stamps are later removed in. So,
   as the command does: the bits written (3-15 kept), 0x4816A0(1) when Mapping changed and
   0x4816A0(0) otherwise. Under true LOS 0x4816A0 re-stamps the live units in-line, at the
   positions the host holds, iteration T's: where the recording machine's command found them,
   between the same two steps [INFERRED: a typed command runs between two ticks' unit passes].
   One departure from the command: the temporary sight records are dropped first
   (sight_records_drop), at the cost of at most 2 s of the viewed seat's death vision, under a
   seat's view only. V's counters and map bit then go back (sight_fill), as at the first step. */
static void los_react(char* ta, const RecWorld* W)
{
    const ReplayGlobalSlot* g = &tagpu_replay_globals[0];
    uint16_t host, was, want;
    int n;
    /* the list's react row is LosType's, the image's first slot (replay-globals.tsv) */
    if (g->where != RG_MAIN || g->offset != OFF_LOSTYPE || g->react != LOS_REACT) return;
    want = (uint16_t)(W->globals[g->dword] & LOS_REACT);
    memcpy(&host, ta + OFF_LOSTYPE, 2);
    was = host;
    if ((host & LOS_REACT) == want) return;
    n = sight_records_drop(ta);
    if (n < 0) {
        s_gst.unbounded++;
        note("replay: the sight mode not changed to %u at GameTime %u: the engine counts %d "
             "temporary sight records", want, *(const unsigned*)(ta + OFF_GAMETIME),
             *(const int*)(ta + OFF_NSIGHTRECS));
        return;
    }
    s_gst.dropped += (unsigned)n;
    s_gst.reacts++;
    host = (uint16_t)((host & ~LOS_REACT) | want);
    memcpy(ta + OFF_LOSTYPE, &host, 2);
    E_SightRebuild(((host ^ was) & LOS_MAPPING) ? 1u : 0u);
    sight_fill(ta, s_viewer);
}

/* an ID of a projectile record's id row as its pointer; 0 when it is past its table */
static int proj_ptr(const Lv* L, char* pool, unsigned table, uint32_t id, char** p)
{
    switch (table) {
    case RL_T_WEAPON: return weapon_ptr(L, id, p);
    case RL_T_UNIT:   return unit_ptr(L, id, p);
    case RL_T_PROJ:
        if (!id) { *p = NULL; return 1; }
        if (id - 1 >= TAGPU_LIM_PROJ) return 0;
        *p = pool + (size_t)(id - 1) * REC_PROJ_BYTES;
        return 1;
    default:
        return 0;
    }
}

/* the first record at or after `from` whose weapon is NULL, or the pool's end: the written extent
   (tagpu_recorder.c, *The globals and the projectiles*) */
static unsigned proj_extent(const char* pool, unsigned from)
{
    unsigned hw = from;
    for (; hw < TAGPU_LIM_PROJ; hw++) {
        uint32_t weapon;
        memcpy(&weapon, pool + (size_t)hw * REC_PROJ_BYTES, 4);
        if (!weapon) break;
    }
    return hw;
}

/* The pool made the recording's. Every ID is DATA: a record below the count is the projectile
   tick's to walk, and it dereferences +0x00, the weapon, of every one; so a record there whose
   weapon (the id row at +0x00) is NULL, or with any ID past its table (REC_ID_BAD included),
   refuses the whole write -- the host's pool is emptied instead (count 0, and the camera's follow
   cleared when it points into it), counted, and noted the first time in a level. A recorded NULL
   target, attacker or quarry below the count is stock's own value and goes in: the recording
   machine's tick walked the same record. Above the count no reader walks, and an append's init
   writes the four id rows before anything reads them, so an ID there that does not resolve goes
   in as NULL. The host's records past the recording's extent are zeroed, so the host's written
   extent is the recording's afterwards, by the scan's own rule. The camera's follow is the
   viewer's own and is left: it points into the pool, which never moves, and the camera reads
   only a record's position. */
/* 1 when the world's pool may go in (the bounds above), else 0 with `why` */
static int proj_bounded(const Lv* L, const RecWorld* W, char* pool, char* why, unsigned cap)
{
    unsigned count = W->nproj, hw = W->proj_hw, i, k;
    if (count > hw || hw > TAGPU_LIM_PROJ) {
        snprintf(why, cap, "a projectile count of %u and an extent of %u past the pool's %u "
                 "records", count, hw, (unsigned)TAGPU_LIM_PROJ);
        return 0;
    }
    for (i = 0; i < count; i++) {
        const unsigned char* img = W->proj + (size_t)i * REC_PROJ_IMAGE;
        for (k = 0; k < sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0]; k++) {
            const ReplayLayoutRow* r = &tagpu_replay_layout[k];
            uint32_t id;
            char* p;
            if (r->record != RL_PROJ || r->cls != RL_ID) continue;
            memcpy(&id, img + r->offset, 4);
            if (!proj_ptr(L, pool, r->target, id, &p) || (!r->offset && !id)) {
                snprintf(why, cap, "projectile record %u's %s is ID %u", i, r->name, id);
                return 0;
            }
        }
    }
    return 1;
}

/* the pool emptied: its count 0, and the camera's follow cleared when it points into it */
static void proj_empty(const Lv* L, char* pool)
{
    char* follow;
    unsigned zero = 0;
    memcpy(L->ta + OFF_NPROJ, &zero, 4);
    memcpy(&follow, L->ta + OFF_FOLLOW, 4);
    if (follow >= pool && follow < pool + (size_t)TAGPU_LIM_PROJ * REC_PROJ_BYTES)
        memcpy(L->ta + OFF_FOLLOW, &zero, 4);
}

static void proj_put(const Lv* L, const RecWorld* W)
{
    char* pool = *(char**)(L->ta + OFF_PROJ);
    unsigned count = W->nproj, hw = W->proj_hw, i, k, hwh, ch;
    char why[160];
    if (!pool) return;
    if (!proj_bounded(L, W, pool, why, sizeof why)) {
        if (!s_gst.refused++)
            note("replay: the projectile pool not written at GameTime %u: %s", L->now, why);
        proj_empty(L, pool);
        return;
    }
    memcpy(&ch, L->ta + OFF_NPROJ, 4);
    hwh = proj_extent(pool, hw);
    if (hwh < ch && ch <= TAGPU_LIM_PROJ) hwh = ch;
    for (i = 0; i < hw; i++) {
        const unsigned char* img = W->proj + (size_t)i * REC_PROJ_IMAGE;
        char* rec = pool + (size_t)i * REC_PROJ_BYTES;
        put_masked(rec, (const uint32_t*)img, s_jval, 0, REC_PROJ_BYTES);
        for (k = 0; k < sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0]; k++) {
            const ReplayLayoutRow* r = &tagpu_replay_layout[k];
            uint32_t id;
            char* p;
            if (r->record != RL_PROJ || r->cls != RL_ID) continue;
            memcpy(&id, img + r->offset, 4);
            if (!proj_ptr(L, pool, r->target, id, &p)) p = NULL;
            memcpy(rec + r->offset, &p, 4);
        }
    }
    if (hwh > hw)
        memset(pool + (size_t)hw * REC_PROJ_BYTES, 0, (size_t)(hwh - hw) * REC_PROJ_BYTES);
    memcpy(L->ta + OFF_NPROJ, &count, 4);
}

/* THE EXPLORED MAP: the host's made the recording's, each u16 entry `img | 1 << b` with b V's bit
   [V+0x146], as sight_fill sets it, so that V's view stays whole. The host's own stamps set bits
   between two writes; each write replaces them, as with the pool. When an entry changed, LosType
   bit 3 is cleared so that the draw rebuilds the fog from it, on the game thread, as sight_fill
   does. The bound: the map's bytes as LoadMap computes them from the host's grid
   (0x483CF6..0x483D02), equal to the recording's -- data_match has found the two grids the same
   size -- and the block lives as long as the level (allocated by LoadMap, freed at its end). */
static void mapped_put(char* ta, const RecWorld* W)
{
    unsigned char* host;
    uint32_t w, h, n, i;
    unsigned b = (unsigned char)player_rec(ta, s_viewer)[PL_MAPBIT], changed = 0;
    unsigned char keep[2] = { 0, 0 };
    memcpy(&host, ta + OFF_MAPPED, 4);
    memcpy(&w, ta + OFF_GRIDW, 4);
    memcpy(&h, ta + OFF_GRIDH, 4);
    n = (w * h * 2u) >> 2;
    if (!host || !W->mapped_whole || n != W->mapped_bytes) {
        if (!s_gst.mrefused++)
            note("replay: the host's explored map (%u bytes) is not the recording's (%u): not "
                 "written", n, W->mapped_bytes);
        return;
    }
    if (b < 16) keep[b >> 3] = (unsigned char)(1u << (b & 7));
    for (i = 0; i < n; i += 2) {
        unsigned k, differ = 0;
        for (k = 0; k < 2 && i + k < n; k++) {
            unsigned char want = W->mapped[i + k] | keep[k];
            if (host[i + k] != want) { host[i + k] = want; differ = 1; }
        }
        changed += differ;
    }
    if (!changed) return;
    s_gst.mputs++;
    s_gst.mentries += changed;
    *(uint16_t*)(ta + OFF_LOSTYPE) &= (uint16_t)~8u;
}

/* the world's globals, pool and explored map into the host (the head of this section) */
static void recorded_put(const Lv* L, const RecWorld* W)
{
    masks_init();
    los_react(L->ta, W);
    globals_put(L->ta, W);
    proj_put(L, W);
    mapped_put(L->ta, W);
    s_gst.puts++;
}

void __cdecl tagpu_replay_capture(void)
{
    char* ta;
    Lv L;
    const RecWorld* W;
    if (!tagpu_puppet_on() || !s_rd || s_dead || s_first || s_over) return;
    ta = ta_main();
    if (!ta || !level_of(ta, &L)) return;
    W = recr_world(s_rd);
    if (W->tick != L.now) {
        s_gst.late++;
        note("replay: the capture point of GameTime %u holds iteration %u: its globals and "
             "projectiles not written", L.now, W->tick);
        return;
    }
    recorded_put(&L, W);
}

/* ---- the seek ------------------------------------------------------------------------------- */

/* THE SEEK (the slice note's S3b), run by the frame visit on a level that plays a recording, on the
   game thread between two sim steps: `seek <T>` in the trigger, or `at <T>` in the play request.
   THE INVARIANT: afterwards the host holds what it would hold at the frame visit after step T-1
   of a playing from the start, in everything the recording carries; everything else it keeps is
   made by the creates, reset to what a level start gives, or set from T. Four things make that
   true by construction:
   - the world is positioned and validated before the host is touched (R0), so a refused seek
     changes nothing but the reader, which goes back to where the host is;
   - every unit is killed and made again (R3, R7), never kept, so no unit's host-own state (its
     COB, its model's pose, its mover's course) survives from before, and the unit fields playback
     leaves to the host's engine because both machines derive them from the same history -- the
     sight stamp's fields, the water state, the state byte -- are set from T (R8b), since a seek
     cuts that history, and a create played after the seek takes its water state from its event
     (ev_create), since a slot dead at T holds the host's history;
   - every player's sight counters are refilled with no unit alive and then hold exactly the
     stamps the units' fields describe (R5, R7, R8, R8b), so they owe nothing to the host's
     history;
   - every deadline the recording does not carry is reset by the engine's own level-start reset or
     set from T (R15).
   So a seek in a fresh process, a seek forwards and a seek backwards to T agree.
   The contact bits +0x110 8-10 and 12 and the minimap's fog are display state (D1). On a puppet
   host only the viewed record's 30-tick pass writes the contact bits, and R14b sets them from T
   with that pass. The minimap's fog is composited by that pass when a sight change has asked for
   it, by the sight rebuild 0x4816A0, and by sight_fill, so at a seek R14 composites it from T's
   counters. A playing holds its last pass's values until its next, so the two hosts agree from
   the next pass on, which R15 schedules at the same GameTime on both, and at T itself when T-1
   is a multiple of 30 (O14). In V's view the minimap's fog is the same on both hosts at every T:
   V's counters are all 1 and V's bit is in every cell.
   Host-own state that survives, none of it read on a puppet host: B4's per-slot columns (births
   stay strictly increasing per slot), the passability cache, the order inside the seats'
   control-group vectors. The bytes a slot dead at T holds also survive, and a later create starts
   from them where it writes nothing: the water state comes from the create's event instead,
   contact bit 12 lasts until the next pass, and the control group is harmless.
   THE ORDER is the argument: R0 touches nothing; R3-R4 run on the clock the host holds; R5 needs
   no unit alive and no temporary sight record; R6 comes before everything that stamps GameTime for
   the target; R11-R13 follow everything that writes effects, tables and the simulation RNG (the
   kills, the creates, their first COB slices). Each step's comment names what it rests on. */

#define OFF_BUCKETS    0x1429F      /* the sort buckets' allocation, 10 bytes each (LoadMap)       */
#define OFF_BCOLS      0x142A3      /* its columns and rows, u32 each ...                          */
#define OFF_BROWS      0x142A7
#define OFF_BCOUNT     0x142AB      /* ... and its bucket count: cols * rows rounded up to 8, the
                                       allocation count * 10 bytes (0x482CAE..0x482CC5)          */
#define OFF_OFFMAP     0x142B7      /* the off-map bucket's address                               */
#define BUCKET_STRIDE  10
#define FD_MODEL       0x98         /* a 3DO def's model template: the spawn passes it to 0x45A8D0
                                       untested (0x423EBB..0x423EC2)                              */
#define CELL_DRAWDEFER 0x04         /* flags bit 2: the draw's own (replay-layout.tsv, drawdefer)   */
#define DEF_BORDER     0xFFFB       /* a cell def at or above this is no feature (0x4246EC)       */
#define WR_NEXT        0x00         /* a wreck record's list links, i16: the next, the previous   */
#define WR_PREV        0x02         /*   (the list mover 0x4232F0)                                */
#define PL_RECORDS     11           /* the player records a SIM_ID_PLAYER may name                */
#define PL_DISPLAY     0xF8         /* DisplayTimer, a GameTime deadline (0x464765)               */
#define PASS_TICKS     30           /* the players phase's period (0x46507D..0x465092)            */

/* 0x47CB00(unit): a bucket's unlink, thiscall on the bucket, `ret 4` (DISASSEMBLED
   0x47CB00..0x47CB30): it walks the bucket's chain from +6 to the unit with no NULL test, splices
   it out and nulls its +0x8E */
typedef void (__thiscall* PFN_Unlink)(char* bucket, char* unit);
#define E_Unlink   ((PFN_Unlink)0x0047CB00u)
/* 0x47CB60(unit, bucket): the move, stdcall `ret 8` (DISASSEMBLED 0x47CB60..0x47CBC0): nothing
   when the unit's +0x82 is the bucket already; for a carried unit (+0x86 set) only +0x82 = bucket;
   else the unlink from +0x82's chain when it is not NULL, a push at the front of the bucket's +6,
   and +0x82 = bucket */
typedef void (__stdcall* PFN_Move)(char* unit, char* bucket);
#define E_Move     ((PFN_Move)0x0047CB60u)
/* 0x45A8D0(template): a model instance, stdcall `ret 4` (0x45A941); 0x45AAA0(instance): its free,
   stdcall `ret 4`, tagpu_reclaim's detour */
typedef char* (__stdcall* PFN_ObjMake)(const char* model);
typedef void (__stdcall* PFN_ObjFree)(char* instance);
#define E_ObjMake  ((PFN_ObjMake)0x0045A8D0u)
#define E_ObjFree  ((PFN_ObjFree)0x0045AAA0u)
/* 0x467440(): the radar pass, no argument, plain `ret` (0x467836; DISASSEMBLED 0x467440..0x467836).
   It runs only when the player count main+0x2A3C is 2 or more (0x46744D), reads the viewed record
   main+0x2A43, every unit's +0x110, +0xFF and owner record with its alliance row +0x108 and
   PlayerInfo bits, and in its last loop the explored map *(main+0x14273) and LosType bit 1
   (0x46775D..0x46780E). It writes +0x110 bits 8-10 and 12, and +0xB0 only for a type-1 or type-2
   owner, which a puppet host has none of. */
typedef void (__cdecl* PFN_RadarPass)(void);
#define E_RadarPass   ((PFN_RadarPass)0x00467440u)
/* the level init's resets, no argument, plain `ret`: the notices 0x47EE30 (DISASSEMBLED
   0x47EE30..0x47EEDA: the queue *(0x51E68C) emptied from its last entry, each queued text freed,
   the last sound's time +0x9D = 0, the 24 per-kind cooldowns at 0x5086EC + 0x18 k = 0) and the chat
   lines 0x463C80 (0x463C80..0x463C9C: the ring's head main+0x2A3E and tail +0x2A40 zeroed) */
typedef void (__cdecl* PFN_Reset)(void);
#define E_NoticeReset ((PFN_Reset)0x0047EE30u)
#define E_ChatReset   ((PFN_Reset)0x00463C80u)

static struct {
    unsigned killed, left, made, refused;
    unsigned outside, moved, carried;
    unsigned frecs, fcells, fdropped, models, freed;
    unsigned sdropped;
    unsigned water, states, stamps, brefused;
    unsigned twritten, tskipped, tkept, trepaired;
    int      pool, effects;
} s_sk;                             /* one seek's counts, for its second line                    */

static unsigned char  s_inU[0x10000];      /* a slot of U: R9 puts it in its recorded bucket     */
static unsigned char  s_named[0x10000];    /* how many members of U name it as their next        */
static unsigned char  s_taken[0x10000];    /* R9's first pass took it out of its bucket          */
static char*          s_ubucket[0x10000];  /* its recorded bucket, resolved                      */
static unsigned short s_uheads[0x10000];   /* the heads of U's chains                            */
static unsigned char  s_fseen[TAGPU_LIM_WRECKS];   /* a wreck record on a list already          */

static void counts_log(const char* when, unsigned now);

/* A recorded bucket ID as the host's bucket: the off-map bucket, or a bucket below the
   allocation's count -- taken no larger than the count LoadMap computes from the columns and
   rows, so a corrupt count cannot widen it. NULL for none, and for one that does not resolve. */
static char* bucket_at(const Lv* L, uint32_t id)
{
    char* base = *(char**)(L->ta + OFF_BUCKETS);
    unsigned long long cap = ((unsigned long long)*(const uint32_t*)(L->ta + OFF_BCOLS) *
                              *(const uint32_t*)(L->ta + OFF_BROWS) + 7) & ~7ull;
    uint32_t n = *(const uint32_t*)(L->ta + OFF_BCOUNT);
    if (id == REC_ID_OFFMAP) return *(char**)(L->ta + OFF_OFFMAP);
    if (!id || !base) return NULL;
    if (n > cap) n = (uint32_t)cap;
    return id - 1 < n ? base + (size_t)(id - 1) * BUCKET_STRIDE : NULL;
}

static uint32_t img_u32(const RecWorld* W, unsigned slot, unsigned off)
{
    uint32_t v;
    memcpy(&v, img_at(W, slot) + off, 4);
    return v;
}

/* the slot an image's unit ID names, or 0 for none and for one past the array */
static unsigned img_unit(const Lv* L, uint32_t id)
{
    return id && id - 1 < L->nslots ? id - 1 : 0;
}

static int sk_bucket_cmp(const void* a, const void* b)
{
    const char* x = s_ubucket[*(const unsigned short*)a];
    const char* y = s_ubucket[*(const unsigned short*)b];
    return x < y ? -1 : x > y;
}

/* R0's buckets: U, the units R9 moves -- live in the image, not carried there, with a recorded
   bucket that resolves (a NULL or unresolved one puts the unit outside U, counted) -- and, for
   each bucket, its members forming one chain through their recorded +0x8E: exactly one member no
   member names, the walk from it visiting every member once and ending at NULL or outside U.
   Leaves the heads in s_uheads[0, *nheads), sorted by bucket. */
static int sk_units_bound(const Lv* L, const RecWorld* W, unsigned* nheads, char* why,
                          unsigned cap)
{
    unsigned slot, n = 0, nu = 0, walked = 0, i;
    memset(s_inU, 0, L->nslots);
    memset(s_named, 0, L->nslots);
    s_sk.outside = 0;
    for (slot = 1; slot < L->nslots; slot++) {
        s_ubucket[slot] = NULL;
        if (!W->live[slot] || img_u32(W, slot, U_CARRIER)) continue;
        s_ubucket[slot] = bucket_at(L, img_u32(W, slot, U_BUCKET));
        if (!s_ubucket[slot]) { s_sk.outside++; continue; }
        s_inU[slot] = 1;
        nu++;
    }
    for (slot = 1; slot < L->nslots; slot++) {
        unsigned nx;
        if (!s_inU[slot]) continue;
        nx = img_unit(L, img_u32(W, slot, U_NEXT));
        if (!nx || !s_inU[nx]) continue;
        if (s_ubucket[nx] != s_ubucket[slot] || s_named[nx]++) {
            snprintf(why, cap, "unit %u's recorded bucket chain is not one chain at unit %u",
                     slot, nx);
            return 0;
        }
    }
    for (slot = 1; slot < L->nslots; slot++)
        if (s_inU[slot] && !s_named[slot]) s_uheads[n++] = (unsigned short)slot;
    qsort(s_uheads, n, sizeof s_uheads[0], sk_bucket_cmp);
    for (i = 0; i < n; i++) {
        unsigned u = s_uheads[i], steps = 0;
        if (i && s_ubucket[s_uheads[i - 1]] == s_ubucket[u]) {
            snprintf(why, cap, "units %u and %u both head one recorded bucket", s_uheads[i - 1], u);
            return 0;
        }
        while (u && s_inU[u] && steps < L->nslots) {
            steps++;
            u = img_unit(L, img_u32(W, u, U_NEXT));
        }
        walked += steps;
    }
    if (walked != nu) {
        snprintf(why, cap, "the recorded buckets' chains reach %u of their %u units", walked, nu);
        return 0;
    }
    *nheads = n;
    return 1;
}

/* R0's features: the structure the engine walks without bounds -- the feature tick follows the
   active list to -1, the destroy follows a footprint cell's way back -- each value DATA. */
static int sk_features_bound(const Lv* L, const RecWorld* W, char* why, unsigned cap)
{
    unsigned list, c, ncells = L->gw * L->gh;
    if (W->heads[3] < 0 || (uint32_t)W->heads[3] >= ncells) {
        snprintf(why, cap, "the reproduction cursor %d is outside the grid", W->heads[3]);
        return 0;
    }
    memset(s_fseen, 0, sizeof s_fseen);
    for (list = 0; list < 3; list++) {
        int32_t i = W->heads[list], prev = -1;
        unsigned steps = 0;
        if (i < -1 || i >= (int32_t)L->nrec) {
            snprintf(why, cap, "wreck list %u's head %d is outside the pool", list, i);
            return 0;
        }
        while (i != -1) {
            const unsigned char* w = W->pool + (size_t)i * REC_WRECK_BYTES;
            int16_t nx, pv;
            memcpy(&nx, w + WR_NEXT, 2);
            memcpy(&pv, w + WR_PREV, 2);
            if (s_fseen[i] || pv != prev || ++steps > L->nrec || nx < -1 ||
                nx >= (int32_t)L->nrec) {
                snprintf(why, cap, "wreck list %u is not a list at record %d", list, i);
                return 0;
            }
            s_fseen[i] = (unsigned char)(list + 1);
            if (list < 2) {
                uint16_t def, x, z;
                const char* d;
                memcpy(&def, w + REC_WRECK_DEF, 2);
                memcpy(&x, w + REC_WRECK_ANCHOR, 2);
                memcpy(&z, w + REC_WRECK_ANCHOR + 2, 2);
                d = fdef_of(L, def);
                if (!d || x >= L->gw || z >= L->gh) {
                    snprintf(why, cap, "wreck record %d's def %u or anchor (%u, %u) is out of "
                             "bounds", i, def, x, z);
                    return 0;
                }
                if (!(d[FD_MASK] & 1)) {
                    if (!*(char* const*)(d + FD_MODEL)) {
                        snprintf(why, cap, "wreck record %d's def %u has no model", i, def);
                        return 0;
                    }
                } else {
                    static const unsigned char state[2] = { WR_STATE1, WR_STATE2 };
                    static const unsigned char seq[2] = { REC_WRECK_SEQ, REC_WRECK_SEQ2 };
                    unsigned k;
                    for (k = 0; k < 2; k++) {
                        const uint16_t* sq;
                        uint32_t id;
                        uint16_t frame;
                        memcpy(&id, w + seq[k], 4);
                        if (!id) continue;
                        if (id > REC_SEQ_FIELDS ||
                            !(sq = *(const uint16_t* const*)(d + REC_SEQ_FIRST + 4 * (id - 1)))) {
                            snprintf(why, cap, "wreck record %d's sequence ID %u names no "
                                     "sequence of def %u", i, id, def);
                            return 0;
                        }
                        memcpy(&frame, w + state[k], 2);
                        if (frame >= *sq) {
                            snprintf(why, cap, "wreck record %d's frame %u is past its "
                                     "sequence's %u", i, frame, *sq);
                            return 0;
                        }
                    }
                }
            }
            prev = i;
            i = nx;
        }
    }
    for (c = 0; c < ncells; c++) {
        const unsigned char* g = W->grid + (size_t)c * REC_CELL_BYTES;
        uint16_t def, rec;
        memcpy(&def, g + CELL_DEF, 2);
        if (def == DEF_FOOTPRINT) {
            if (c % L->gw < g[CELL_BACK + 1] || c / L->gw < g[CELL_BACK]) {
                snprintf(why, cap, "cell (%u, %u)'s way back leaves the grid", c % L->gw,
                         c / L->gw);
                return 0;
            }
            continue;
        }
        if (def >= DEF_BORDER) continue;
        memcpy(&rec, g + CELL_BACK, 2);
        if (def >= L->nfdefs || ((g[CELL_FLAGS] & 1) && rec >= L->nrec)) {
            snprintf(why, cap, "cell (%u, %u)'s def %u or record %u is out of bounds",
                     c % L->gw, c / L->gw, def, rec);
            return 0;
        }
    }
    return 1;
}

/* the engine's temporary sight records' count within what its add allows */
static int sight_records_bounded(const char* ta)
{
    int n;
    const char* recs;
    memcpy(&n, ta + OFF_NSIGHTRECS, 4);
    memcpy(&recs, ta + OFF_SIGHTRECS, 4);
    return n >= 0 && n <= SR_MAX && (!n || recs);
}

/* R0: every value of the world at T that the rebuild hands to the engine, bounded. */
static int sk_bound(const Lv* L, const RecWorld* W, unsigned* nheads, char* why, unsigned cap)
{
    const ReplayGlobalSlot* g = &tagpu_replay_globals[0];
    char* pool = *(char**)(L->ta + OFF_PROJ);
    unsigned char* map;
    uint32_t n;
    if (!sk_features_bound(L, W, why, cap) || !sk_units_bound(L, W, nheads, why, cap)) return 0;
    memcpy(&map, L->ta + OFF_MAPPED, 4);
    n = (*(const uint32_t*)(L->ta + OFF_GRIDW) * *(const uint32_t*)(L->ta + OFF_GRIDH) * 2u) >> 2;
    if (!map || !W->mapped_whole || n != W->mapped_bytes) {
        snprintf(why, cap, "the host's explored map (%u bytes) is not the recording's (%u)", n,
                 W->mapped_bytes);
        return 0;
    }
    if (!pool) {
        snprintf(why, cap, "the level has no projectile pool");
        return 0;
    }
    if (!proj_bounded(L, W, pool, why, cap)) return 0;
    if (g->where != RG_MAIN || g->offset != OFF_LOSTYPE || g->react != LOS_REACT) {
        snprintf(why, cap, "replay-globals.tsv's first slot is not LosType's react row");
        return 0;
    }
    if (!sight_records_bounded(L->ta)) {
        snprintf(why, cap, "the engine counts %d temporary sight records",
                 *(const int*)(L->ta + OFF_NSIGHTRECS));
        return 0;
    }
    return 1;
}

/* R3: every unit killed through the destructor, as a replayed death goes (ev_death: C4's wrap,
   mode 0), with severity 0 and corpse 0 -- no statistics, no Killed script, no explosion, no
   corpse -- and the killer none: netid 0xFFFFFFFF, slot 0. Three passes, as the destructor's
   own dependencies ask: the unfinished first (a factory's cleanup deals its frame 30 000 while
   the frame still exists, 0x402701), then the carried, then the rest. +0x110 bit 28 is tested
   again at each unit's turn, since a death can take others with it. Afterwards every slot's
   +0xA6 is 0 (0x486DC7), which make requires; one still live is counted, and R7 refuses its
   slot. */
static void sk_kills(const Lv* L)
{
    unsigned pass, slot;
    for (pass = 0; pass < 3; pass++)
        for (slot = 1; slot < L->nslots; slot++) {
            const char* u = unit_at(L, slot);
            unsigned char rec[16];
            uint32_t carrier;
            float built;
            unsigned kind;
            if (!host_live(L, slot)) continue;
            memcpy(&built, u + U_BUILT, 4);
            memcpy(&carrier, u + U_CARRIER, 4);
            kind = built != 0.0f ? 0 : carrier ? 1 : 2;
            if (kind != pass) continue;
            memset(rec, 0, sizeof rec);
            rec[0] = 0x0C;
            rec[1] = (unsigned char)slot;
            rec[2] = (unsigned char)(slot >> 8);
            memset(rec + 3, 0xFF, 4);
            if (tagpu_tx_replay_death(rec, 0)) s_sk.killed++;
        }
    for (slot = 1; slot < L->nslots; slot++)
        if (host_live(L, slot)) s_sk.left++;
}

/* R5: LosType bits 0-2 the recording's, then 0x4816A0 always, with 1 when Mapping changed (the
   Mapping command's own call, 0x416DA6) and 0 otherwise. With no unit alive and no temporary
   record left, its fill gives every record that passes its seat test the mode's base and its unit
   loop finds nothing, so the counters after R8 are the base plus the stamps of the units made at
   T at their image positions, and after R8b the base plus exactly the stamps the image's fields
   describe, the recording machine's at T: a function of T alone either way. It undoes
   sight_fill(V), which R14 redoes. */
static void sk_sight_reset(char* ta, const RecWorld* W)
{
    const ReplayGlobalSlot* g = &tagpu_replay_globals[0];
    uint16_t host, was;
    memcpy(&host, ta + OFF_LOSTYPE, 2);
    was = host;
    host = (uint16_t)((host & ~LOS_REACT) | (W->globals[g->dword] & LOS_REACT));
    memcpy(ta + OFF_LOSTYPE, &host, 2);
    E_SightRebuild(((host ^ was) & LOS_MAPPING) ? 1u : 0u);
}

#define U_POS          0x6A         /* x, altitude, z: i32 each                                    */
#define U_LOSXZ        0x7A         /* the sight stamp: its cell or corner, i16 x and z ...        */
#define U_LOSH         0xF8         /* ... and its height or circle radius index, u8              */
#define U_OWNERREC     0x96         /* the owner's player record                                   */
#define U_STATEBYTE    0x10E        /* the state byte: the setter 0x48B090's                       */
#define FD_SIGHTDIST   0x202        /* a unit def's sight, u16 ...                                 */
#define FD_SIGHTH      0x170        /* ... and its height, u8 (0x4820AE, 0x4820DB)                 */
#define OFF_SEALEVEL   0x1427F      /* u8: a sight record's y is raised to (it + 1) << 16          */
#define OFF_CIRCLES    0x1485B      /* the circle frames' table: its u16 count first, and 0x4B7F30
                                       returns NULL for an index at or past it (0x4B7F44); it
                                       reads the count at 0x4B7F41 before its NULL test at
                                       0x4B7F48..0x4B7F4A, so a NULL table bounds nothing       */
#define LOS_TRUE       0x2u         /* LosType bit 1: units stamp the counters                    */
#define LOS_FAN        0x4u         /* LosType bit 2: the ray fan; clear, the circle              */
#define SIGHT_REC      0x24
/* 0x482090(unit): the destructor's sight removal, stdcall `ret 4` (DISASSEMBLED
   0x482090..0x48210C): it builds the record sight_rec builds and calls the removal 0x481D50 */
typedef void (__stdcall* PFN_SightRemove)(char* unit);
#define E_SightRemove ((PFN_SightRemove)0x00482090u)
/* 0x482270(rec): the stamp, stdcall `ret 4` (0x48259E): reads the record's +0, +4, +8 and +0xC
   only, and writes only the owner's counters, with LosType bit 3 cleared and main+0x142F1 bit 2
   set for the viewed player's stamps (0x482283..0x4822A1), as every emit in playback does */
typedef void (__stdcall* PFN_SightStamp)(unsigned char* rec);
#define E_SightStamp ((PFN_SightStamp)0x00482270u)

/* The unit's sight record as 0x482090 builds it (DISASSEMBLED 0x482093..0x4820FB, the emit
   wrapper's 0x4827B3..0x48281B the same): the owner, the address of the stamp's words, the def's
   sight and height, the address of the stamp's byte, the position with y raised to the sea level's
   floor by a signed compare. The def is the one R7's create gave the unit, never a recorded
   value. */
static void sight_rec(const char* ta, char* u, unsigned char* rec)
{
    const char* def = *(char* const*)(u + U_DEF);
    uint32_t at;
    int32_t y, floor = ((int32_t)(unsigned char)ta[OFF_SEALEVEL] + 1) << 16;
    memset(rec, 0, SIGHT_REC);
    memcpy(rec, u + U_OWNERREC, 4);
    at = (uint32_t)(size_t)(u + U_LOSXZ);
    memcpy(rec + 4, &at, 4);
    memcpy(rec + 8, def + FD_SIGHTDIST, 2);
    rec[0xA] = (unsigned char)def[FD_SIGHTH];
    at = (uint32_t)(size_t)(u + U_LOSH);
    memcpy(rec + 0xC, &at, 4);
    memcpy(rec + 0x10, u + U_POS, 12);
    memcpy(&y, rec + 0x14, 4);
    if (y < floor) memcpy(rec + 0x14, &floor, 4);
}

/* R8b: the units' own bookkeeping from iteration T's image -- the water state, the state byte and
   the sight stamp's fields, which masks_init leaves to the host's engine because in a playing from
   the start it derives them from the same history as the recording machine. A seek cuts that
   history, and the engine does not derive them again from the state at T: no create code writes
   the water state (0x485A40..0x4861BA store field by field), the create sets the state byte only
   as the def asks (0x48612F..0x486148), and the emitter's hysteresis keeps a stamp whose cell is
   unchanged and whose height is within 5 (0x48261F..0x48262E). It runs after R8, whose moves
   re-emit and would overwrite the image's fields, and nothing between it and the capture point of
   T emits (R9-R16).
   - The water state: 0x43DB50 stores only 0..4 and only compares the value before overwriting
     it, so the bound is its enum's; a value past it keeps the host's and is counted. Setting it
     starts no script: COB restarts at a seek, and the unit's next change starts setSFXoccupy.
   - The state byte through the setter, as ev_state sets it; its scripts and its send run as in
     R7's creates, which call it from the frame visit already (0x486148).
   - The sight stamp, by the engine's invariant (DISASSEMBLED): with LosType bit 1, in the ray fan
     a unit's byte is not 0 exactly when its fields describe a stamp it made, and in the circle its
     fields always do -- the create's 0x482AC0 stores byte 0 and goes through the emitter in the
     fan (0x482B44..0x482B5E) and stamps directly in the circle (0x482B6C..0x482BFE), the emitter
     stores a height of at least 1 inside the sight grid (0x48266E) and 0 outside it, stamping
     nothing (0x4826AC), and its early returns keep fields that describe a stamp. The host's fields
     after R8 hold it for the host's counters and the image's for the recording machine's, so
     removing the host's stamp as the emitter would (0x482634..0x482641 in the fan, always in the
     circle, 0x48275D), writing the image's three and stamping them where they describe a stamp
     leaves the counters the recording machine's. A removal undoes its stamp exactly: both walks
     read only the record's +0, +4, +8 and +0xC, so they are a function of the owner, the stored
     words and byte, the def's sight, the static sight grid and LosType bits 1-2. Without bit 1
     no counter carries a stamp (R5 filled them, and every removal is gated on bit 1: 0x48263C,
     0x482757, 0x486838..0x486842), so the three are written alone.
     THE BOUND, first: in the circle the image's radius index must be below the frame table's
     count, since 0x4B7F30 returns NULL past it and both the stamp and the removal dereference the
     answer untested (0x4824ED..0x4824F1, 0x481FCD..0x481FD1); a NULL table refuses every unit in
     the circle (OFF_CIRCLES). A refused unit keeps the three R8 left, which describe its own
     stamp, and is counted. The walks bound the words themselves (0x4822F8/0x482304, 0x481DD8,
     the circle's frame clipped at 0x4824F5..0x482547), and every byte is a height the emitter
     can store (0x4825EA..0x4825F2). */
static void seek_unit_books(const Lv* L, const RecWorld* W)
{
    unsigned slot;
    uint16_t los;
    const uint16_t* circles = *(const uint16_t* const*)(L->ta + OFF_CIRCLES);
    unsigned char rec[SIGHT_REC];
    memcpy(&los, L->ta + OFF_LOSTYPE, 2);
    for (slot = 1; slot < L->nslots; slot++) {
        const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
        const unsigned char* ib = (const unsigned char*)img;
        char* u;
        if (!W->live[slot] || !holds(L, slot, img)) continue;
        u = unit_at(L, slot);
        if ((unsigned char)u[U_WATER] != ib[U_WATER]) {
            if (ib[U_WATER] <= WATER_MAX) {
                u[U_WATER] = (char)ib[U_WATER];
                s_sk.water++;
            } else {
                s_sk.brefused++;
            }
        }
        if ((unsigned char)u[U_STATEBYTE] != ib[U_STATEBYTE]) {
            E_SetState(u, ib[U_STATEBYTE], 1);
            E_SetState(u, (unsigned char)~ib[U_STATEBYTE], 0);
            s_sk.states++;
        }
        if (!(los & LOS_FAN) && (!circles || ib[U_LOSH] >= *circles)) {
            s_sk.brefused++;
            continue;
        }
        if (!memcmp(u + U_LOSXZ, ib + U_LOSXZ, 4) && (unsigned char)u[U_LOSH] == ib[U_LOSH])
            continue;
        if ((los & LOS_TRUE) && (!(los & LOS_FAN) || u[U_LOSH])) E_SightRemove(u);
        memcpy(u + U_LOSXZ, ib + U_LOSXZ, 4);
        u[U_LOSH] = (char)ib[U_LOSH];
        if ((los & LOS_TRUE) && (!(los & LOS_FAN) || ib[U_LOSH])) {
            sight_rec(L->ta, u, rec);
            E_SightStamp(rec);
        }
        s_sk.stamps++;
    }
}

/* R9: the buckets made the recorded ones (sk_units_bound made U). The move 0x47CB60 does nothing
   for a unit already in the bucket, so it cannot reorder one; hence two passes.
   (a) Out: each unit of U that R7 made and the host does not carry is unlinked by 0x47CB00 from
       the bucket its +0x82 names, then +0x82 = NULL. Its walk has no NULL test, and is safe
       because every link of the host's bucket chains is an engine function's -- the creates'
       placement, the stamps, the attach and the detach, the destructor's unlink, and this --
       while units_put never writes +0x82 or +0x8E (id rows, out of the raw mask): each such unit
       is in the chain its +0x82 names, the invariant the engine's own stamp relies on.
   (b) In: each recorded chain, restricted to the units (a) took out, from its tail to its head,
       through 0x47CB60(unit, bucket): with +0x82 NULL and +0x86 NULL it pushes at the front of
       the bucket's +6. Each bucket then starts with its recorded chain in order, its head the
       recorded head, whatever order the buckets are visited in; a unit outside U can only trail.
   A carried unit gets +0x82 from the image when it resolves (the move only sets +0x82 while a
   unit is carried, 0x47CB79); otherwise the host's stays. AFTERWARDS every live unit's +0x82 is
   non-NULL: the creates give every unit a bucket, the attach's unlink keeps +0x82 (0x48AC5E), and
   this replaces a bucket only with a non-NULL one -- which a drop's re-link (0x48ACCC ->
   0x47CB40) and the attach's unlink of a child in no chain dereference untested. */
static void sk_buckets(const Lv* L, const RecWorld* W, unsigned nheads)
{
    unsigned slot, i;
    memset(s_taken, 0, L->nslots);
    for (slot = 1; slot < L->nslots; slot++) {
        char* u = unit_at(L, slot);
        char* b;
        uint32_t carrier;
        if (!host_live(L, slot)) continue;
        memcpy(&carrier, u + U_CARRIER, 4);
        if (W->live[slot] && img_u32(W, slot, U_CARRIER)) {
            if (carrier && (b = bucket_at(L, img_u32(W, slot, U_BUCKET)))) {
                memcpy(u + U_BUCKET, &b, 4);
                s_sk.carried++;
            }
            continue;
        }
        if (!s_inU[slot] || carrier) continue;
        memcpy(&b, u + U_BUCKET, 4);
        if (b) E_Unlink(b, u);
        memset(u + U_BUCKET, 0, 4);
        s_taken[slot] = 1;
    }
    for (i = 0; i < nheads; i++) {
        unsigned u = s_uheads[i], n = 0;
        while (u && s_inU[u] && n < L->nslots) {
            if (s_taken[u]) s_chain[n++] = (unsigned short)u;
            u = img_unit(L, img_u32(W, u, U_NEXT));
        }
        while (n) {
            E_Move(unit_at(L, s_chain[--n]), s_ubucket[s_uheads[i]]);
            s_sk.moved++;
        }
    }
}

/* R10: the features whole, from the image, after R0 bounded it.
   - The host's 3DO model instances freed, walking its active and settled lists as the teardown's
     purge 0x422170 walks them (0x42219A..0x4221D5, a 3DO def's +0xFE bit 0 clear), each bounded:
     0x45AAA0 is tagpu_reclaim's detour, so an instance is held until the render thread has
     finished every pass that could hold it.
   - Every pool record from its image, a GAF record's sequences back from their IDs (the def's
     fields, REC_SEQ_FIRST), a 3DO record's model 0 until the next step; the three heads and the
     cursor.
   - Each active and settled 3DO record's model made by the spawn's own call and store
     (0x423EC2, 0x423ECB).
   - Every cell from its image less the draw's own bit, its unit slots only when 0 or naming a
     live unit, else 0 and counted: the stamp's contest dereferences an incumbent's +0x96 with no
     NULL test (0x47CDCA). */
static void sk_features(const Lv* L, const RecWorld* W)
{
    unsigned list, i, c, ncells = L->gw * L->gh;
    for (list = 0; list < 2; list++) {
        int32_t k = *(const int32_t*)(L->ta + OFF_WR_HEADS + 4 * list);
        unsigned steps = 0;
        while (k >= 0 && (uint32_t)k < L->nrec && steps++ < L->nrec) {
            char* w = L->pool + (size_t)k * REC_WRECK_BYTES;
            uint16_t def;
            const char* d;
            char* inst;
            memcpy(&def, w + REC_WRECK_DEF, 2);
            d = fdef_of(L, def);
            memcpy(&inst, w + REC_WRECK_MODEL, 4);
            if (d && !(d[FD_MASK] & 1) && inst) {
                E_ObjFree(inst);
                memset(w + REC_WRECK_MODEL, 0, 4);
                s_sk.freed++;
            }
            k = *(const int16_t*)(w + WR_NEXT);
        }
    }
    for (i = 0; i < L->nrec; i++) {
        const unsigned char* img = W->pool + (size_t)i * REC_WRECK_BYTES;
        char* w = L->pool + (size_t)i * REC_WRECK_BYTES;
        uint16_t def;
        const char* d;
        memcpy(&def, img + REC_WRECK_DEF, 2);
        d = fdef_of(L, def);
        memcpy(w, img, REC_WRECK_BYTES);
        if (d && (d[FD_MASK] & 1)) {
            static const unsigned char seq[2] = { REC_WRECK_SEQ, REC_WRECK_SEQ2 };
            unsigned k;
            for (k = 0; k < 2; k++) {
                uint32_t id, v = 0;
                memcpy(&id, img + seq[k], 4);
                if (id >= 1 && id <= REC_SEQ_FIELDS)
                    memcpy(&v, d + REC_SEQ_FIRST + 4 * (id - 1), 4);
                memcpy(w + seq[k], &v, 4);
            }
        } else {
            memset(w + REC_WRECK_MODEL, 0, 4);
        }
        s_sk.frecs++;
    }
    memcpy(L->ta + OFF_WR_HEADS, W->heads, 12);
    memcpy(L->ta + OFF_FCURSOR, &W->heads[3], 4);
    for (list = 0; list < 2; list++) {
        int32_t k = W->heads[list];
        unsigned steps = 0;
        while (k >= 0 && (uint32_t)k < L->nrec && steps++ < L->nrec) {
            char* w = L->pool + (size_t)k * REC_WRECK_BYTES;
            uint16_t def;
            const char* d;
            memcpy(&def, w + REC_WRECK_DEF, 2);
            d = fdef_of(L, def);
            if (d && !(d[FD_MASK] & 1)) {
                char* inst = E_ObjMake(*(char* const*)(d + FD_MODEL));
                memcpy(w + REC_WRECK_MODEL, &inst, 4);
                s_sk.models++;
            }
            k = *(const int16_t*)(w + WR_NEXT);
        }
    }
    for (c = 0; c < ncells; c++) {
        const unsigned char* img = W->grid + (size_t)c * REC_CELL_BYTES;
        char* cell = L->grid + (size_t)c * REC_CELL_BYTES;
        unsigned char keep = (unsigned char)cell[CELL_FLAGS] & CELL_DRAWDEFER;
        uint16_t slot[2];
        unsigned k;
        memcpy(cell, img, REC_CELL_BYTES);
        cell[CELL_FLAGS] = (char)(((unsigned char)cell[CELL_FLAGS] & ~CELL_DRAWDEFER) | keep);
        memcpy(slot, img, 4);
        for (k = 0; k < 2; k++)
            if (slot[k] && (slot[k] >= L->nslots || !host_live(L, slot[k]))) {
                slot[k] = 0;
                s_sk.fdropped++;
            }
        memcpy(cell, slot, 4);
        s_sk.fcells++;
    }
    s_tin.n = 0;                    /* every cell is the world's: no input is left to write back */
    s_tin.overflow = 0;
}

/* ---- R12: the declared tables ---- */

/* A recorded SIMF_ADDR's ID as the host's address: 0 is NULL, else a row of its table bounded by
   the table's count. 1 resolved. */
static int addr_of(const Lv* L, unsigned simid, uint32_t id, char** p)
{
    char* defs;
    if (!id) { *p = NULL; return 1; }
    switch (simid) {
    case SIM_ID_UNIT_SLOT: return unit_ptr(L, id, p);
    case SIM_ID_WEAPON:    return weapon_ptr(L, id, p);
    case SIM_ID_UNIT_TYPE:
        defs = *(char**)(L->ta + OFF_UDEFS);
        if (!defs || id - 1 >= L->ndefs) return 0;
        *p = defs + (size_t)(id - 1) * DEF_STRIDE;
        return 1;
    case SIM_ID_PLAYER:
        if (id - 1 >= PL_RECORDS) return 0;
        *p = player_rec(L->ta, id - 1);
        return 1;
    default:
        return 0;
    }
}

/* the declaration of this build a recorded table pairs with: the same name, stride, class, key,
   flags and layout hash (tagpu_recfmt.h, RT_TABLE); NULL with `why` otherwise */
static const SimDesc* table_pair(const RecTableInfo* t, const char** why)
{
    unsigned i;
    for (i = 0; i < tagpu_simreg_count(); i++) {
        const SimDesc* d = tagpu_simreg_at(i);
        if (!d || strcmp(d->name, t->name)) continue;
        if (d->stride != t->stride || d->cls != t->cls || d->key != t->key ||
            d->flags != t->flags || tagpu_simreg_layout_hash(d) != t->lhash) {
            *why = "its stride, class, key, flags or layout differ";
            return NULL;
        }
        return d;
    }
    *why = "this build declares no table of that name";
    return NULL;
}

/* the declaration whose live or heap row count is `d`'s variable, or NULL */
static const SimDesc* count_owner(const SimDesc* d)
{
    unsigned i;
    for (i = 0; i < tagpu_simreg_count(); i++) {
        const SimDesc* e = tagpu_simreg_at(i);
        if (e && e->rows && (const void*)e->rows == d->addr) return e;
    }
    return NULL;
}

/* One recorded table into its paired declaration, in place: the inverse of the recorder's
   row_image. Plain bytes, and SIMF_ID, SIMF_TARGET and SIMF_PINNED fields, from the image (a pinned
   field's object is written where it is, never moved); a SIMF_ADDR with an ID kind back to its
   address, or left and counted when its ID does not resolve; left as the host holds them:
   SIMF_LOCAL, SIMF_OWNED, SIMF_COLUMN, a SIMF_ADDR of SIM_ID_NONE or not a pointer's size, and a
   SIMF_COUNT in a row with SIMF_COLUMN fields (the columns are not restored, so neither is their
   count). Rows written: the recorded ones within what the host holds now -- a static table's rows,
   a heap table's allocation as its count or its block size says -- so a write never reaches past
   the host's memory and a heap table is never reallocated here. A count variable is written only
   as its table allows: never a heap table's (its block is sized by it), a live prefix's only within
   the array. */
static void table_put(const Lv* L, const SimDesc* d, const RecTable* t)
{
    unsigned char* base = (d->flags & SIMD_HEAP) ? *(unsigned char**)d->addr
                                                 : (unsigned char*)d->addr;
    unsigned host = !base ? 0 : !d->nrows ? *d->rows * d->stride : d->nrows * d->stride;
    unsigned rows = (t->size < host ? t->size : host) / d->stride, r, b, nkeep = 0;
    const SimDesc* owner = (!(d->flags & SIMD_HEAP) && d->nrows == 1 && !d->rows)
                               ? count_owner(d) : NULL;
    unsigned char* keep;
    const SimField* f;
    int column = 0;
    if (owner) {
        uint32_t v;
        memcpy(&v, t->img, 4);
        if (!owner->nrows || t->size < 4 || v > owner->nrows) {
            s_sk.tkept++;
            return;
        }
    }
    keep = (unsigned char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, d->stride);
    if (!keep) {
        s_sk.tskipped++;
        tagpu_logf("replay: %s: the seek skipped %s: no memory", s_name, d->name);
        return;
    }
    for (f = d->layout; f && f->size; f++) column |= f->kind == SIMF_COLUMN;
    for (f = d->layout; f && f->size; f++) {
        unsigned k, n = f->repeat ? f->repeat : 1;
        int left = f->kind == SIMF_LOCAL || f->kind == SIMF_OWNED || f->kind == SIMF_COLUMN ||
                   f->kind == SIMF_ADDR || (f->kind == SIMF_COUNT && column);
        if (!left) continue;
        for (k = 0; k < n; k++) memset(keep + f->off + (size_t)k * f->step, 1, f->size);
    }
    for (b = 0; b < d->stride; b++) nkeep += keep[b];
    for (r = 0; r < rows; r++) {
        unsigned char* row = base + (size_t)r * d->stride;
        const unsigned char* img = t->img + (size_t)r * d->stride;
        if (!nkeep) {
            memcpy(row, img, d->stride);
            continue;
        }
        for (b = 0; b < d->stride; b++)
            if (!keep[b]) row[b] = img[b];
        for (f = d->layout; f->size; f++) {
            unsigned k, n = f->repeat ? f->repeat : 1;
            if (f->kind != SIMF_ADDR || f->id == SIM_ID_NONE || f->size != sizeof(char*)) continue;
            for (k = 0; k < n; k++) {
                unsigned at = f->off + k * f->step;
                uint32_t id;
                char* p;
                memcpy(&id, img + at, 4);
                if (addr_of(L, f->id, id, &p)) memcpy(row + at, &p, 4);
                else s_sk.tkept++;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, keep);
    s_sk.twritten++;
}

/* R12: each table the recording carries into the declaration it pairs with, or skipped and named;
   then each sim file that owns an invariant its layouts cannot express re-establishes it, before
   anything runs. The air pool's lock is held over the writes (tagpu_patches.h). */
static void sk_tables(const Lv* L, const RecWorld* W)
{
    const RecHeader* h = recr_header(s_rd);
    unsigned i;
    tagpu_patches_restore_begin();
    for (i = 0; i < h->ntab && i < W->ntab; i++) {
        const char* why = NULL;
        const SimDesc* d = table_pair(&h->tab[i], &why);
        if (!d) {
            s_sk.tskipped++;
            tagpu_logf("replay: %s: the seek skipped %s: %s", s_name, h->tab[i].name, why);
            continue;
        }
        table_put(L, d, &W->tab[i]);
    }
    s_sk.trepaired = tagpu_patches_restored();
    s_sk.trepaired += tagpu_weapons_restored();
}

/* R15: the deadlines the recording does not carry (or that the step masks), each by the engine's
   own rule, after which the clock is the recording's:
   - each seat's +0xF0, the players phase's 30-tick deadline, from image T: the step masks it
     because image T+1 carries it advanced by step T's phase, but image T's is what the recording
     machine's phase found at step T (0x46507D..0x465092: `if (+0xF0 <= GameTime) +0xF0 += 30`);
   - V's +0xF0 the next multiple of 30 above GameTime, the engine's invariant (0x464990 zeroes it
     at GameTime 0, the loader's players phase 0x497C10 sets 30, each step adds 30 once GameTime
     reaches it): V's pass runs the radar pass and the minimap fog while V is viewed;
   - every record's DisplayTimer +0xF8 = GameTime, as the player init sets it (0x464765);
   - the notices and the chat lines through the level init's own resets 0x47EE30 and 0x463C80:
     the creates' queued replies go, and a backward seek leaves no cooldown or line stamped in the
     future.
   The particles' and the temporary sight records' deadlines went with them (R11, R4); the pacing
   stamp is R16's. */
static void sk_deadlines(char* ta, const RecWorld* W, unsigned now)
{
    unsigned p, next = PASS_TICKS * (now / PASS_TICKS + 1);
    for (p = 0; p < REC_PLAYERS; p++) {
        char* rec = player_rec(ta, p);
        if (p == s_viewer) memcpy(rec + PL_PASS, &next, 4);
        else if (tagpu_puppet_seat(p)) memcpy(rec + PL_PASS, (const char*)W->player[p] + PL_PASS, 4);
        memcpy(rec + PL_DISPLAY, &now, 4);
    }
    E_NoticeReset();
    E_ChatReset();
}

/* The reader back where the host is after a refused seek; when it cannot be, the playing stops
   there, as iteration_read's failure stops it. */
static void sk_back(const Lv* L, unsigned prev)
{
    char why[200];
    if (recr_seek(s_rd, prev, why, sizeof why) == 1) return;
    s_over = 1;
    tagpu_logf("replay: %s cannot be read back to GameTime %u: %s; the units stop here", s_name,
               prev, why);
    counts_log("a refused seek", L->now);
}

static unsigned sk_ms(LONGLONG ticks, LONGLONG hz)
{
    return hz ? (unsigned)(ticks * 1000 / hz) : 0;
}

/* THE SEEK's steps, R0-R17, in their order */
static void seek_run(char* ta)
{
    RecWorld* W = recr_world(s_rd);
    unsigned T = s_seek.T, prev = W->tick, h, nheads = 0, refused0;
    LARGE_INTEGER hz, t0, t1, t2;
    char why[200], dump[sizeof s_seek.dump];
    Lv L;
    int k;
    s_seek.pending = 0;
    snprintf(dump, sizeof dump, "%s", s_seek.dump);
    /* R0: position and validate; nothing of the host is touched yet */
    if (!level_of(ta, &L)) {
        tagpu_logf("replay: %s: seek to %u refused: the level's unit array or features are not "
                   "the ones the counts describe", s_name, T);
        return;
    }
    h = L.now;
    QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&t0);
    k = recr_seek(s_rd, T, why, sizeof why);
    if (k <= 0) {
        tagpu_logf("replay: %s: seek to %u refused: %s", s_name, T, why);
        if (k < 0) sk_back(&L, prev);
        return;
    }
    QueryPerformanceCounter(&t1);
    memset(&s_sk, 0, sizeof s_sk);
    masks_init();
    if (!sk_bound(&L, W, &nheads, why, sizeof why)) {
        tagpu_logf("replay: %s: seek to %u refused: %s", s_name, T, why);
        sk_back(&L, prev);
        return;
    }
    /* R1: the rebuild's kills are not the recording's eliminations */
    tagpu_puppet_rebuild(1);
    /* R2: the projectile pool emptied before the kills: each kill's purge 0x49C880 then scans
       nothing, and its stock miss -- the record that slides into the slot it compacted keeps
       naming the dead unit -- cannot arise. R13 writes the pool whole. */
    proj_empty(&L, *(char**)(ta + OFF_PROJ));
    /* R3, on the clock the host holds: GameTime h >= 1, a seek running only after the first step */
    refused0 = s_st.refused;
    sk_kills(&L);
    /* R4: every temporary sight record dropped through the engine's own removal (the drop's
       comment): no temporary record outlives R5's refill, whose counters its later expiry would
       otherwise decrement. R0 bounded the engine's count of them. */
    s_sk.sdropped = (unsigned)sight_records_drop(ta);
    /* R5 */
    sk_sight_reset(ta, W);
    /* R6: the target's clock, before everything that stamps GameTime for it: the creates' mover
       claim stamps and B4 births, and R15's deadlines */
    *(unsigned*)(ta + OFF_GAMETIME) = T - 1;
    L.now = T - 1;
    /* R7: every unit of iteration T, made as the first step makes them; a refused create is a
       difference the comparison names, never a hazard (make bounds what it passes) */
    s_sk.made = level_units(&L, W);
    /* R8: S2b's appliers, the features left to R10 */
    links_put(&L, W);
    units_put(&L, W);
    yards_put(&L, W);
    states_put(&L, W);
    players_put(&L, W);
    orders_put(&L, W);
    s_sk.refused = s_st.refused - refused0;
    /* R8b, after R8's moves, which re-emit */
    seek_unit_books(&L, W);
    /* R9, R10, R11, R12 */
    sk_buckets(&L, W, nheads);
    sk_features(&L, W);
    s_sk.effects = tagpu_effects_clear();
    sk_tables(&L, W);
    /* R13: the globals, the pool and the explored map, after R7's draws of the simulation RNG and
       R11; the LosType step finds R5's bits and does nothing */
    refused0 = s_gst.refused;
    recorded_put(&L, W);
    s_sk.pool = s_gst.refused == refused0;
    /* R14: V's sight, which R5 undid, and the minimap's fog from the counters at T (sight_fill) */
    sight_fill(ta, s_viewer);
    /* R14b: the viewed record's radar pass, once, as its 30-tick pass runs it (0x46556D). The
       contact bits are display state (D1). On a puppet host, after the creates, only that pass
       writes them: R7's creates clear bits 8-10 and set bit 9 from the def (0x485B4C,
       0x485C3E..0x485C44), so until the next pass no unit has a contact. One pass at the end of
       the seek computes them from the world at T. A playing's last pass ran in step
       P = 30*floor((T-1)/30), on iteration P+1, because the players phase 0x49555F follows the
       replay step in the unit tick's stub 0x4954ED. So the two hosts agree from the next pass
       on, at GameTime P+30, which R15 schedules on both, and at T itself when T-1 is a multiple
       of 30; until the next pass the seek shows T's contacts, and in a seat's view T's minimap
       fog, where a playing shows those of P+1 (O14). The pass's other call, the minimap fog
       0x466C20, ran in R14 (sight_fill), and the radar pass asks for no other: of main+0x142F1
       it sets bit 1 alone (0x46742F). It runs after the last write it reads -- R7's units, R8's
       positions and alliances, R13's LosType and explored map -- and before R15, which it does
       not read. */
    E_RadarPass();
    /* R15 */
    sk_deadlines(ta, W, T - 1);
    /* R16: inside the frame visit, before the next frame's scheduler 0x495230 sees the stamps: no
       lag, even after a forward seek of 900 ticks or more */
    pace(ta, T - 1);
    /* R17: the next tick makes GameTime T; at its capture point the world holds iteration T and
       the step plays T's events and reads T+1, the play loop's own invariant */
    tagpu_puppet_rebuild(0);
    s_over = 0;
    s_frame_tick = T - 1;
    if (dump[0]) tagpu_rec_dump_arm(dump, 0);
    QueryPerformanceCounter(&t2);
    tagpu_logf("replay: %s: at GameTime %u, sought from GameTime %u in %u ms (read %u, rebuild "
               "%u); units %u killed, %u made, %u refused", s_name, T, h,
               sk_ms(t2.QuadPart - t0.QuadPart, hz.QuadPart),
               sk_ms(t1.QuadPart - t0.QuadPart, hz.QuadPart),
               sk_ms(t2.QuadPart - t1.QuadPart, hz.QuadPart), s_sk.killed, s_sk.made,
               s_sk.refused);
    tagpu_logf("replay: %s: the seek to %u wrote: %u units left alive by the kills; buckets: %u "
               "units moved, %u carried units' buckets set, %u units outside the recorded "
               "buckets; features: %u records and %u cells, %u cell slots dropped, %u models "
               "freed and %u made; %u temporary sight records dropped; units' bookkeeping from "
               "T: %u water states, %u state bytes, %u sight stamps, %u refused; tables: %u "
               "written, %u skipped, %u values left as the host holds them, %u repaired; the "
               "projectile pool %s, %u records; the effects %s", s_name, T, s_sk.left, s_sk.moved,
               s_sk.carried, s_sk.outside, s_sk.frecs, s_sk.fcells, s_sk.fdropped, s_sk.freed,
               s_sk.models, s_sk.sdropped, s_sk.water, s_sk.states, s_sk.stamps, s_sk.brefused,
               s_sk.twritten, s_sk.tskipped, s_sk.tkept, s_sk.trepaired,
               s_sk.pool ? "written" : "REFUSED", W->proj_hw,
               s_sk.effects ? "cleared" : "NOT cleared");
}

/* ---- the frame visit ------------------------------------------------------------------------ */

#define TRIGGER    SIM_TOOL_FILE("tagpu_replay.trigger")
#define TRIGGER_MS 100              /* the most often the trigger is looked for: the engine draws
                                       330 to 4900 times a second (tagpu_packet_pub.c)          */

/* The trigger: one line, the file deleted whatever it held.
   - `dump <name>`: a dump armed for the recorder's next capture point (tagpu_rec_dump_arm), on any
     level in play, live or playing; while paused that is the first tick after the pause.
   - `dump <name> at <T>`, T a decimal u32 other than 0: a dump armed for the capture point of GameTime T
     exactly, on any level in play, whatever the speed or pauses (the recorder's table of armed
     dumps). A playing host's speed keys change nothing, so stepping to T by keys is timing; the
     capture point is where every dump is taken, and the DLL takes it at T.
   - `seek <T>` or `seek <T> dump <name>`: on a level that plays a recording, the seek waits for
     the next frame visit after the first step (THE SEEK), and the dump is taken at the capture
     point of T. */
/* `p` is a decimal u32 and nothing after it: 1 with its value */
static int u32_word(const char* p, unsigned* v)
{
    unsigned long long x = 0;
    unsigned k = 0;
    while (p[k] >= '0' && p[k] <= '9') {
        x = x * 10 + (unsigned)(p[k] - '0');
        if (++k > 10 || x > 0xFFFFFFFFull) return 0;
    }
    if (!k || p[k]) return 0;
    *v = (unsigned)x;
    return 1;
}

static void trigger_read(void)
{
    static DWORD s_looked;
    char line[96], name[40], dump[40];
    unsigned T;
    DWORD now = GetTickCount();
    FILE* f;
    int end = 0;
    size_t n = 0;
    if (now - s_looked < TRIGGER_MS) return;
    s_looked = now;
    if (GetFileAttributesA(TRIGGER) == INVALID_FILE_ATTRIBUTES) return;
    f = fopen(TRIGGER, "rb");
    if (f) {
        n = fread(line, 1, sizeof line - 1, f);
        fclose(f);
    }
    line[n] = 0;
    DeleteFileA(TRIGGER);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = 0;
    if (sscanf(line, "dump %39s%n", name, &end) == 1) {
        if (!line[end]) {
            tagpu_rec_dump_arm(name, 0);
            return;
        }
        if (!strncmp(line + end, " at ", 4) && u32_word(line + end + 4, &T) && T) {
            tagpu_rec_dump_arm(name, T);
            return;
        }
    }
    if (!strncmp(line, "seek ", 5) && seek_words(line + 5, &T, dump, sizeof dump)) {
        if (!tagpu_puppet_on() || !s_rd || s_dead) {
            tagpu_logf("replay: seek to %u refused: this level plays no recording", T);
            return;
        }
        s_seek.pending = 1;
        s_seek.T = T;
        snprintf(s_seek.dump, sizeof s_seek.dump, "%s", dump);
        return;
    }
    tagpu_logf("replay: %s: not understood: %s", TRIGGER, line);
}

/* THE FRAME VISIT, before the in-play draw call 0x4969CD, on the game thread, between two sim
   steps and on paused frames too: the trigger; then, on a level that plays a recording and has
   played its first step, a pending seek (THE SEEK), or else, once a GameTime, what the draw after
   a step shows (THE GLOBALS AND THE PROJECTILES). */
static void __cdecl tagpu_replay_frame(void)
{
    char* ta;
    unsigned now;
    Lv L;
    const RecWorld* W;
    trigger_read();
    if (!tagpu_puppet_on() || !s_rd || s_dead || s_first) return;
    ta = ta_main();
    if (!ta) return;
    if (s_seek.pending) {
        seek_run(ta);
        return;
    }
    if (s_over) return;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    W = recr_world(s_rd);
    if (now == s_frame_tick || W->tick != now + 1 || !level_of(ta, &L)) return;
    s_frame_tick = now;
    recorded_put(&L, W);
}

/* once a level: at the recording's end, or at the level's when that comes first */
static void counts_log(const char* when, unsigned now)
{
    static const char* const name[EV_KINDS] = {
        "create", "death", "shot", "intercept", "start", "named start", "built", "state", "reply",
        "spray"
    };
    char ev[400], gates[400];
    unsigned k, n = 0;
    ev[0] = 0;
    for (k = 0; k < EV_KINDS && n < sizeof ev; k++)
        n += (unsigned)snprintf(ev + n, sizeof ev - n, "%s%s %u/%u", k ? ", " : "", name[k],
                                s_st.played[k], s_st.skipped[k]);
    tagpu_logf("replay: %s at %s, GameTime %u: %u iterations applied (%u late); units made %u at "
               "the start; %u refused, %u missing here, %u here only, %u of another type; %u IDs "
               "past their array; %u carry links made, %u refused; order nodes %u built, %u torn "
               "down, %u refused; %u yards changed, %u refused; %u control groups not set", s_name,
               when, now, s_st.steps, s_st.late, s_st.made, s_st.refused, s_st.missing,
               s_st.lingering, s_st.other, s_st.bad_ids, s_st.links, s_st.unlinked, s_st.obuilt,
               s_st.ofreed, s_st.orefused, s_st.yards, s_st.yrefused, s_st.grefused);
    tagpu_logf("replay: %s: events played/not played: %s", s_name, ev);
    tagpu_logf("replay: %s: features: %u calls made, %u out of bounds, %u returned otherwise; "
               "%u deaths, %u whose explosion's calls were not made; %u draws not taken as "
               "recorded; %u cells and %u wreck records written, %u and %u left for their "
               "structure, %u and %u for a value out of bounds; the heads differed %u times, the "
               "cursor out of bounds %u; the tick's input put back: %u cells, %u overflows",
               s_name, s_fst.roots, s_fst.rrefused, s_fst.rdiffer, s_fst.deaths, s_fst.dmissed,
               s_fst.left, s_fst.cells, s_fst.wrecks, s_fst.cstruct, s_fst.wstruct, s_fst.cbad,
               s_fst.wbad, s_fst.heads, s_fst.cursor, s_fst.tcells, s_fst.tover);
    tagpu_logf("replay: %s: globals and projectiles: written %u times, %u capture points holding "
               "another iteration; the pool refused %u times; the sight mode changed %u times, "
               "%u temporary sight records dropped, %u changes not made; the explored map changed "
               "by %u writes, %u entries in all, refused %u times", s_name, s_gst.puts,
               s_gst.late, s_gst.refused, s_gst.reacts, s_gst.dropped, s_gst.unbounded,
               s_gst.mputs, s_gst.mentries, s_gst.mrefused);
    tagpu_puppet_refusals(gates, sizeof gates);
    tagpu_logf("replay: %s: refused on puppets: %s", s_name, gates);
    tagpu_puppet_footprints(gates, sizeof gates);
    tagpu_logf("replay: %s: footprint writes: %s", s_name, gates);
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

/* The level and the recording's first iteration at the first step, whose GameTime must be the
   level's: checked before the step logs whether it plays, so that `playing` is a verdict. */
static int first_iteration(char* ta, Lv* L, char* why, unsigned cap)
{
    char r[200];
    if (!level_of(ta, L)) {
        snprintf(why, cap, "the level's unit array or its features are not the ones the counts "
                 "describe");
        return 0;
    }
    if (recr_tick(s_rd, r, sizeof r) <= 0) {
        snprintf(why, cap, "its first iteration cannot be read: %s", r);
        return 0;
    }
    if (recr_world(s_rd)->tick != L->now) {
        snprintf(why, cap, "its first iteration is GameTime %u, the level's first step %u",
                 recr_world(s_rd)->tick, L->now);
        return 0;
    }
    return 1;
}

/* the step's events and the next image, by THE APPLY above */
static void play(const Lv* L, int first)
{
    RecWorld* W = recr_world(s_rd);
    masks_init();
    if (first) {
        s_st.made += level_units(L, W);
        images_put(L, W);
        wind_seed(W);
        sight_fill(L->ta, s_viewer);
        /* V owns nothing, so its energy and metal are 0. The level start 0x465E30 reads them from
           V's skirmish settings entry, which the SKIRMISH screen does not fill (0x465EF5..0x465F1A;
           MEASURED 1365/16385 and 11/10 in three processes). Nothing writes them afterwards on a
           puppet host: the economy 0x401360 runs only for a type-1 or type-2 record
           (0x465539..0x46555A). So every host of a recording holds the same V. The first step
           follows the loader's last act (THE PLAYER'S SETTINGS), so the level start has run. */
        memset(player_rec(L->ta, s_viewer) + PL_ENERGY, 0, 4);
        memset(player_rec(L->ta, s_viewer) + PL_METAL, 0, 4);
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
    char why[200], shown[32];
    Lv L;
    int first = 0;
    if (!tagpu_puppet_on() || !s_rd || s_dead) return;
    ta = ta_main();
    if (!ta) return;
    if (s_first) {
        s_first = 0;
        s_opt.played = 1;                   /* THE PLAYER'S SETTINGS: the loader is done */
        if (!first_checks(ta, recr_header(s_rd), why, sizeof why) ||
            !first_iteration(ta, &L, why, sizeof why)) {
            s_dead = 1;
            tagpu_logf("replay: not playing %s: %s; the level stays empty", s_name, why);
            return;
        }
        if (s_view == s_viewer) snprintf(shown, sizeof shown, "full visibility");
        else snprintf(shown, sizeof shown, "the view of record %u", s_view);
        tagpu_logf("replay: playing %s: %u seats, the viewer at record %u, from GameTime %u, %s",
                   s_name, recr_header(s_rd)->nseats, s_viewer, recr_header(s_rd)->start, shown);
        tagpu_puppet_play();                /* the feature gates close (THE FEATURES) */
        first = 1;
    } else if (!s_over && !level_of(ta, &L)) {
        s_over = 1;
        tagpu_logf("replay: %s stops at GameTime %u: the unit array or the features are not the "
                   "ones the level began with", s_name, *(const unsigned*)(ta + OFF_GAMETIME));
        counts_log("the stop", *(const unsigned*)(ta + OFF_GAMETIME));
    }
    if (!s_over) play(&L, first);
    pace(ta, *(const unsigned*)(ta + OFF_GAMETIME));
}

/* ---- S-e: the teardown ---------------------------------------------------------------------- */

void tagpu_replay_level_end(void)
{
    char* ta = ta_main();
    int stale = s_opt.stale;
    if (options_restore(ta) && stale) E_SaveSettings();     /* THE PLAYER'S SETTINGS */
    /* both cases log `replay: <name> at its level's end, GameTime <t>`, the line tacli's
       `replay seek` takes as the end of the level a playback was on */
    if (s_rd && !s_dead && !s_over)
        counts_log("its level's end", ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0);
    else if (s_rd && !s_dead)
        tagpu_logf("replay: %s at its level's end, GameTime %u: the recording had ended before it",
                   s_name, ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0);
    replay_close();
    tagpu_puppet_end();
}

/* THE VIEWER'S SEAT. V is a record the playback seats for itself, and the only one whose PlayerInfo
   the recording does not describe: seat_level sets its watcher bit (+0x9B bit 6) and its side. The
   seat activation clears only bit 5 (0x464378); the front end's reset 0x4644D0 zeroes every
   PlayerInfo (0x46458B..0x464598), but only from its three calls (0x42734F, 0x427D73, 0x4518AC),
   and a surrender to the main menu reaches none of them (MEASURED 2026-09-30: record 0's name still
   in place at the menu). So V's PlayerInfo goes back as the Start's fill left it, whatever path
   follows, once 0x491B60 has returned (tagpu_reclaim.c's post hook), on the thread that ran
   tagpu_replay_level_end: the bit must hold until the teardown has built the post-game table, which
   leaves watchers out (0x41DD43, called at 0x491B8B). The pointer taken at the seat fill is still
   V's then: main's PlayerInfo blocks are made once by 0x463BE0 and never freed (the destructor
   0x463C40 runs only on stack-local records; replay-layout.tsv, `info`). Afterwards the results
   screen reads a listed player's colour (0x41E5CD..0x41E5DC), and V is never listed; it reads the
   controlled player's watcher bit, V's, only while the win flag main+0x391AF is set
   (0x41F347..0x41F35B), which the post-game table copies from main+0x3923B bit 4
   (0x41DC2E..0x41DC3C), and on a puppet level nothing sets that bit: +IWin's 0x416A0A is a typed
   command, and the players phase's 0x4658C0 follows the objective's test (0x46586F), which is
   disarmed. A Start always reaches the teardown or ends the process (tagpu_puppet.h). The seats'
   PlayerInfo stays as the recording's last image left it, as an ordinary game leaves its own, every
   seat's side and colour one of this game's (players_put); the next Start's fill writes both for
   every seat it activates (0x47A79F..0x47A7C1 for a player, 0x47A7F2..0x47A813 for an AI). */
void tagpu_replay_level_after(void)
{
    if (s_vinfo.at) memcpy(s_vinfo.at, s_vinfo.was, REC_INFO_BYTES);
    s_vinfo.at = NULL;
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

/* `fn` with every register and the flags kept, then a jump to `engine`: entered by the site's
   call, so `engine` runs with the site's own return address on top */
static unsigned char* before_call(unsigned engine, void (__cdecl* fn)(void))
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x9C;                                        /* pushfd */
    *p++ = 0x60;                                        /* pushad */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)fn); p += 4;
    *p++ = 0x61;                                        /* popad  */
    *p++ = 0x9D;                                        /* popfd  */
    *p++ = 0xE9; tagpu_detour_rel(p, engine); p += 4;  /* jmp    */
    return s;
}

/* The frame visit's site: the in-play draw call `call 0x468CF0` at 0x4969CD (`push ebx; push ebx`
   before it, DISASSEMBLED). Two facts it rests on:
   - the stub is entered by the site's call and leaves by a jump, so DrawGameScreen runs with the
     site's own return address 0x4969D2 on top: the in-play gate of the observers on 0x468CF0
     (tagpu_packet_pub.c, before_draw; tagpu_menu.c) sees exactly the frames it saw;
   - it is reached on paused frames: the pause test (0x496920..0x496926) jumps over only the
     scheduler 0x495230 and the tick 0x495490, and every path through the frame callback 0x496790
     falls through to 0x4969CD (DISASSEMBLED; tagpu_scenario.c rests on the same reading).
   The scenario applier's tick hook lands at the next instruction, 0x4969D2..0x4969D6: disjoint
   bytes. Installed on its own, before the play sites, since the trigger's `dump` serves live
   recordings too. */
static void frame_site_init(void)
{
    enum { DRAW_SITE = 0x004969CDu, DRAW = 0x00468CF0u };
    unsigned char* stub;
    int ok = 1;
    if (!call_is(DRAW_SITE, DRAW)) {
        tagpu_logf("replay: the frame visit at 0x4969CD holds other bytes: no trigger, so no dumps "
                   "and no seeks");
        return;
    }
    stub = before_call(DRAW, tagpu_replay_frame);
    if (stub) call_write(DRAW_SITE, stub, &ok);
    if (!stub || !ok) {
        tagpu_detour_stub_free(stub);
        tagpu_logf("replay: FAILED the frame visit at 0x4969CD could not be written: no "
                   "trigger, so no dumps and no seeks");
        return;
    }
    tagpu_logf("replay: the frame visit at 0x4969CD in");
}

void tagpu_replay_init(void)
{
    enum { FILL = 0x0047B092u, PERSIST = 0x0047B09Cu, START1 = 0x00497963u, START2 = 0x00497ACDu };
    static const unsigned char SAVE[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };   /* mov eax,[main] */
    unsigned char *fill, *persist, *skip, *p;
    int ok = 1;
    frame_site_init();
    if (!call_is(FILL, 0x0047A760u) || !call_is(PERSIST, 0x00430F00u) ||
        !call_is(START1, 0x00496EE0u) || !call_is(START2, 0x00496EE0u)) {
        tagpu_logf("replay: the Start's or the loader's calls hold other bytes: no replays");
        return;
    }
    if (!tagpu_detour_bytes_ok(0x00430F00u, SAVE, 5)) {
        tagpu_logf("replay: the settings save 0x430F00 holds other bytes: no replays");
        return;
    }
    fill = after_call(0x0047A760u, replay_fill);
    persist = after_call(0x00430F00u, replay_options);
    /* the seat setup 0x496EE0(seat, start) is stdcall `ret 8`: on a puppet level the stub returns
       as it would, with the two arguments popped; otherwise it jumps to it with the site's
       return address on top */
    skip = p = tagpu_detour_stub();
    /* THE PLAYER'S SETTINGS: no site goes in unless the save's observer is in. Without the
       persist's site it stays inert: nothing else puts a recording's options in. */
    if (!fill || !persist || !skip ||
        !tagpu_detour_observe(0x00430F00u, SAVE, 5, save_before, save_after)) {
        tagpu_detour_stub_free(fill);
        tagpu_detour_stub_free(persist);
        tagpu_detour_stub_free(skip);
        tagpu_logf("replay: no memory for the sites, or the settings save cannot be observed: no "
                   "replays");
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
    tagpu_logf("replay: 4 sites and the settings save's observer in");
}
