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
#include <stdio.h>
#include <string.h>
#include "tagpu_replay.h"
#include "tagpu_recreader.h"
#include "tagpu_recfmt.h"
#include "tagpu_recorder.h"
#include "tagpu_replay_layout.h"
#include "tagpu_puppet.h"
#include "tagpu_limits.h"
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
    s_first = s_dead = 0;
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

void __cdecl tagpu_replay_step(void)
{
    char* ta;
    char why[200];
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
    }
}

/* ---- S-e: the teardown ---------------------------------------------------------------------- */

void tagpu_replay_level_end(void)
{
    char* ta = ta_main();
    options_restore(ta);
    if (s_rd) tagpu_logf("replay: %s ended at GameTime %u", s_name,
                         ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0);
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
