/* tagpu_replay.c -- playing a demo recording back on puppets (tagpu_replay.h;
   research/notes/tadr-port/demo-recorder-s2-host.md, which this file cites by section).

   OPENING (§6). The request file, `play <name> [<record>] [timeline <N>] [at <T> [dump <name>]]`,
   names a recording, optionally the recorded player whose view the level shows -- without one it
   shows everything, V's full visibility (§2.9) -- optionally the timeline to play, a fork of the
   recording the file also holds (THE BOUNDARY), and optionally a seek to run once the first step
   has played (THE SEEK). The SKIRMISH Start reads it. Four call sites turn the level that Start loads
   into a puppet level, each inert unless the request was accepted:
   - S-a, the Start's call of the seat fill (0x47B092). The stock fill runs first. Then the
     request is validated before anything is written: every puppet gate in (tagpu_puppet.c) and
     the teardown wrap in (tagpu_reclaim.c), which ends the puppet level before its units are
     destroyed; the recording's format and exclusion file, a skirmish, no network, the map the
     Start has just read, the seats, the unit limit and the view. On success every recorded seat
     becomes a type-3 record with its recorded side, logo, name and alliance row, the viewer's
     own seat V, the engine's no-player record 10, is seated beside them (THE VIEWER'S SEAT) and
     +0x2A43 names the view, the objective's victory
     and defeat tests are disarmed (0x4904B0), and the puppet table is built. On any failure the
     log says why, and the Start goes on as the skirmish its settings describe.
   - S-b, the Start's call of the settings persist 0x430F00 (0x47B09C). After the persist has
     written the player's own settings to the registry, the recording's options go into the
     settings the loader copies from. From the level's first step, every settings save runs with
     the player's in their place, and the teardown puts the player's back (THE PLAYER'S
     SETTINGS).
   - S-c, the loader's two calls of the seat setup 0x496EE0 (0x497963, 0x497ACD), which create
     the start units: skipped on a puppet level. And the level init's call 0x4919C8, which on a
     puppet level also gives V's record its buffers (THE VIEWER'S SEAT).
   - S-d, the first step, at the capture point of the level's first iteration, before its unit
     tick: what needs the loaded level is checked -- its counts, its map, the unit and weapon
     data, its first GameTime. On a mismatch nothing is ever applied and the level stays empty,
     which the viewer sees and the log explains. Otherwise the feature gates close and the
     contest's rule the recording needs is written, before any create, so that every footprint
     the creates stamp follows it (tagpu_puppet.c, THE CONTEST'S RULE); a rule that cannot be
     written refuses the playing there, and the level stays empty too. Then the start units are
     made, the first image goes in, the wind is seeded, V's sight is filled and the capture
     point's write follows (THE APPLY, THE FIRST STEP'S UNITS).
   Two more sites serve a level that plays: B2's stub calls tagpu_replay_capture first at each
   capture point, and the frame visit before the in-play draw call 0x4969CD reads the trigger,
   runs a seek (THE SEEK) and writes what the draw after a step shows (THE GLOBALS AND THE
   PROJECTILES), then the engine's cull when it wrote anything the cull reads (THE DRAW'S UNIT
   LIST).
   The teardown (S-e) ends the replay and gives V's record and PlayerInfo back once it has
   returned (THE VIEWER'S SEAT), and the recorder refuses a puppet level (S-f).
   The Replays browser starts a playback without the request file or the SKIRMISH screen: its
   Watch runs the Start's route itself, with the seating and the options where S-a and S-b run
   them, and refuses rather than falling back to a skirmish (THE BROWSER'S START). A puppet level
   refuses RESTART, which would refill the seats from the player's settings (THE RESTART GATE).
   A takeover (THE TAKEOVER) ends the playing mid-level instead: the seek's rebuild makes the world
   of GameTime T with the recorded seats local, one of them the player's, and the level goes on as
   a live skirmish, recorded as a new timeline of the same file.

   WHAT IT READS AND WRITES. The allocator ownership callback and the level init's call run on
   the loader thread and consume only a copied plan published before that thread starts. Their
   atomic results are read at the first game-thread step after the engine's load-done gate. The
   other paths run on the
   game thread: the Start handler, sim step and frame callback 0x496790, and the
   teardown is entered only from the engine's state handlers there. The player records and their
   PlayerInfo blocks live in the main block for the process (tagpu_recorder.c says why); a seat
   index is below 10 before it addresses one (the reader refuses anything else), and V is record
   10, the eleventh the engine constructs (0x41D97E..0x41D997). The skirmish
   settings [main+0x29A0], GameingState [main+0x391E9] and the objective [main+0x391ED] are the
   ones the Start has just used: the settings for its fill, GameingState for its mode, and the
   objective armed by the .ota read at 0x47AF77 (the browser's Start: its own call of the same
   read). */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_replay.h"
#include "tagpu_cover.h"
#include "tagpu_recreader.h"
#include "tagpu_recmp_arena.h"
#include "tagpu_recmp_lifecycle.h"
#include "tagpu_recmp_text.h"
#include "tagpu_recfmt.h"
#include "tagpu_recorder.h"
#include "tagpu_replay_layout.h"
#include "tagpu_puppet.h"
#include "tagpu_reclaim.h"
#include "tagpu_refuse.h"
#include "tagpu_limits.h"
#include "tagpu_patches.h"
#include "tagpu_weapons.h"
#include "tagpu_simreg.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "tagpu_regstore.h"
#include "tagpu_replays.h"
#include "tagpu_persp.h"
#include "tagpu_zoom.h"
#include "tagpu_gaf.h"
#include "tagpu_packet.h"
#include "tagpu_hudinput.h"
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
#define PL_BLOCKFIRST  0x67         /* the record's first and last unit, inclusive                 */
#define PL_BLOCKLAST   0x6B
#define PL_IDFIRST     0x6F         /* u16: the first and last ID of the block, the range the ID
                                       cycle 0x48C190 walks for the controlled record (0x48C1CA) */
#define PL_IDLAST      0x71
#define PL_AI          0x74         /* the AI object                                               */
#define PL_GROUPS      0x78         /* the control-group table (0x480190)                          */
#define PL_EC          0xEC         /* the object the player init makes when NULL (0x4647CF)       */
#define PL_LIVE        0x144        /* u16, the live count (0x486DFC)                              */
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
#define SET_MAPLEN     0x100        /* the engine's own bound on the map's name (0x47AA12..0x47AA31) */
#define SET_ENTRIES    10           /* the seat entries at +0, one a record ...                   */
#define SET_ENTRY      0x18         /* ... +0 controller, +4 side, +8 ally group, +0xC metal, +0x10
                                       energy, +0x14 colour (the save's fields, 0x4314F0..0x43171D) */
#define ENT_SIDE       0x04
#define ENT_ALLY       0x08
#define ENT_COLOUR     0x14
#define ALLY_NONE      5            /* no ally group: the fill marks no alliance for it
                                       (0x47A83C..0x47A89E), and 0x479760 passes it             */
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
#define OFF_DUE_TICKS  0x38A3B      /* u32: scheduler 0x495230 bounds this frame's steps to 0..5 */
#define OFF_SPEED_CEIL 0x38A4B      /* u16: the game speed the player chose, 10 = 1x                 */
#define OFF_SPEED_LIVE 0x38A4D      /* u16: the speed the scheduler runs at, raised toward the
                                       ceiling by its throttle (0x495465..0x49547B)               */
#define OFF_PAUSE      0x38A51      /* u16: bit 0 the pause, the bit the Pause key flips
                                       (0x4960AA..0x4960BC)                                       */
#define OFF_MAPPED     0x14273      /* u16* the explored map, a bit a player (0x4845A9); its bytes
                                       are ([+0x14233]·[+0x14237]·2) >> 2 (0x483CF6, 0x4816B5)   */
#define OFF_LOSTYPE    0x14281      /* u16: bit 3 clear makes the draw rebuild the fog grid from
                                       the viewed player's sight (0x4848F2)                      */
#define OFF_MMFLAGS    0x142F1      /* u8: bit 2 asks 0x466C20 for the minimap's fog; bit 1 is
                                       DrawMinimap's redraw (0x466B11)                           */
#define MODE_SKIRMISH  2
#define MODE_NETWORK   3   /* RT_MODE of a multiplayer recording (tagpu_recorder.c) */

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
static char       s_name[MAX_PATH];  /* the request's name, or the browser's file stem          */
static char       s_path[MAX_PATH];  /* its path, "replays\\<file>"                            */
static struct { unsigned first, last; int ok; } s_span;   /* its summary's first and last GameTime */
static unsigned   s_viewer;         /* V                                                          */
static unsigned   s_view;           /* the viewed player: V for full visibility, or a seat         */
static unsigned   s_viewPending;    /* seat + 1, or 0: consumed by the game-thread frame visit     */
static int        s_first;          /* the first step's checks are still to come                  */
static int        s_dead;           /* they failed: nothing is applied this level                 */
static int        s_over;           /* the recording has nothing more to apply                    */
static int        s_endlog;         /* its counts wait for the last step's phases (THE PHASES)    */
static unsigned   s_endat;          /* ... and the GameTime it ended at                           */
static unsigned   s_notes;          /* the level's anomalies logged                               */
#define NOTE_SITES     48           /* above the call sites of note(), so each has its own count  */
static struct { const char* fmt; unsigned n; } s_nsite[NOTE_SITES]; /* per call site, by format */
#define EV_KINDS (RK_EV_SPRAY - RK_EV_CREATE + 1)
static struct {
    unsigned steps, late, made, refused, missing, lingering, other, bad_ids, links, unlinked;
    unsigned obuilt, ofreed, orefused, yards, yrefused, grefused;
    unsigned played[EV_KINDS], skipped[EV_KINDS];   /* each event kind's, from RK_EV_CREATE    */
} s_st;                             /* the level's counts, logged at its end                      */
static struct {
    unsigned roots, rrefused, rdiffer, deaths, dmissed, left, cells, cstruct, cbad, wrecks,
             wstruct, wbad, heads, cursor, tcells, tover, corpses;
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
/* A table's write counts: tables written and skipped, values left as the host holds them,
   invariants repaired, rows the write changed, and the first tables it changed */
typedef struct {
    unsigned written, skipped, kept, repaired, changed;
    char     names[160];
} TabCounts;

typedef struct {
    unsigned water, states, stamps, refused;
} Books;                            /* the units' bookkeeping written (THE UNITS' BOOKKEEPING)     */
static struct {
    unsigned puts, late, refused, reacts, dropped, unbounded, mputs, mentries, mrefused;
    Books    books;
    unsigned bmade, bother;
    TabCounts tab;
    int      tnamed;                /* an unpaired table named once a playback                   */
} s_gst;                            /* ... and its globals' and projectiles'                      */
static uint32_t   s_made[0x10000];  /* the capture point at which a slot's books are the replay's:
                                       the one after the step whose create made it, or the first
                                       step's own; the slot count is a u16 (THE UNITS'
                                       BOOKKEEPING)                                               */
static unsigned   s_frame_tick;     /* the GameTime the frame visit last wrote at                 */
enum { SK_SEEK, SK_TAKEOVER, SK_REWIND, SK_NOW };
enum { RW_IDLE, RW_FLUSH, RW_OPEN, RW_PAST, RW_ENDING };
static struct {
    unsigned state, G, P, viewed, viewer, timeline, T, objective_armed;
    unsigned R;                     /* the fork's start, found before the original ends (rw_trigger) */
    unsigned short pause, message_at[2];
    unsigned char types[REC_PLAYERS], messages[2160];
    char path[MAX_PATH], dump[40], result_dump[40];
    DWORD started;
    RecReader* now;
    RecReader* live;
    HANDLE done;
} s_rw;
static int s_rw_pause_in;
static int rw_prepare(char* ta, char* why, unsigned cap);
static void rw_seats(char* ta);
/* rewind keeps a live skirmish's own recording: a live multiplayer game has no rewind */
static int rw_skirmish(const RecHeader* h, char* why, unsigned cap)
{
    if (h->mode != MODE_NETWORK) return 1;
    snprintf(why, cap, "a multiplayer recording has no rewind");
    return 0;
}
static void rw_finish(char* ta, int back);
static void rw_poll(void);
static int rw_trigger(const char* line);
static void rw_dispose(void);
static int u32_word(const char* s, unsigned* v);
static struct {
    int      pending;               /* a seek waits for the next frame visit after the first step */
    unsigned T;
    char     dump[40];              /* a dump to take at its capture point, or ""                 */
    int      poison;                /* R12 poisons the tables first (THE POISON CHECK)            */
    int      mode;                  /* SK_SEEK or SK_TAKEOVER, consumed by seek_run               */
    int      boundary;              /* the seek to a timeline's first iteration (THE BOUNDARY)    */
    unsigned P;                     /* a takeover's player ...                                    */
    int      D;                     /* ... and difficulty, -1 for the default (tk_bound)          */
} s_seek;
static int      s_hold;             /* a boundary waits for its seek: the step plays nothing     */
static unsigned s_tl;               /* the request's timeline; 0 the original                     */
static struct {
    int      on;                    /* seek_run's TAKEOVER steps are live: from TK0 to TK4b       */
    RecFork  fork;                  /* TK0's handle and append point, until the arm moves them
                                       (tagpu_recorder.c, THE FORK'S HANDLE)                     */
    unsigned adopted, resumed, bounded;     /* R8's counts (tk_node)                             */
    unsigned orphans;                       /* R8's units whose lists were torn down (tk_orphans) */
    LONGLONG t_ai, t_pass;                  /* TK1's and TK4a's ticks                            */
} s_tk;
#define RT_ARMED 24                 /* a broad entry's round trips, armed at once (fixtures.json) */
static int s_rt_pace_in;            /* attach-time guard before the skirmish step loop */
/* THE TURBO (turbo_due): k, read once at attach, 0 when off; the rest the game thread's alone */
static unsigned s_turbo_k;
static struct {
    int      live;                  /* `turbo on` holds now (`turbo off` clears it)              */
    int      engaged;               /* ... or did at some point of this level                    */
    unsigned g0;                    /* GameTime at the first acknowledgement                     */
    unsigned frames, lagged, held;  /* frames run at k; left to the lag scale; held for writer */
    DWORD    last, ms;              /* the last frame's GetTickCount; the play between frames    */
    unsigned last_g, ticks;         /* its GameTime; the ticks between frames at k               */
} s_turbo;
static unsigned s_pause_at;         /* THE PAUSE AT T: the GameTime armed, 0 none; game thread    */
static int s_pause_taken;           /* the pause bit is set by THE PAUSE AT T or THE LAUNCH'S
                                       HOLD, not by the player: the hold keeper keeps it set     */
/* THE LAUNCH'S HOLD: the state, from tagpu_hold.on read once at attach; then the game thread's
   alone. STEP: the batch that began the hold's step ran from GameTime g0. */
enum { LHOLD_OFF, LHOLD_ARMED, LHOLD_STEP, LHOLD_DONE };
static struct { int state; unsigned g0; } s_lhold;
static int s_mp_arena_in;           /* allocator's block-ownership boundary */
static int s_engine_cells;          /* the grid's unit slots and yard bits are the engine's:
                                       a multiplayer segment, or s_cells_held (tagpu_puppet_begin) */
static int s_cells_held;            /* a skirmish segment that holds its cells (recr_holds_cells):
                                       the marks are the engine's too, and a takeover writes the
                                       recording's cells (THE UNIT SLOTS AND THE YARD BIT) */
enum { MP_ARENA_OFF, MP_ARENA_ARMED, MP_ARENA_WORKING, MP_ARENA_READY, MP_ARENA_FAILED };
static struct { RecMpRoster roster; volatile LONG state; } s_mp_arena;
#define MP_CREATE 29                /* a create's 28 argument bytes, then 1 once it returned     */
/* THE MULTIPLAYER MATERIALIZER's state */
static struct {
    int             level;          /* this level plays a mode-3 file (mp_begin)                 */
    int             on;             /* ... and the reader stands in its multiplayer segment       */
    int             failed;
    RecMpLifecycle* life;
    uint64_t        ordinal;        /* the last envelope the consumer was given                  */
    unsigned        slots;
    const struct Lv* L;             /* the step's level, for the action callback                 */
    unsigned char*  held;           /* [slots] live here, no composed image yet                  */
    unsigned char*  create;         /* [slots * MP_CREATE] a held unit's create, for a seek      */
    unsigned char*  live;           /* [slots] the composed world's                              */
    uint32_t*       unit;           /* [slots * REC_UNIT_DWORDS]                                 */
    RecList*        list;           /* [slots * 2]                                               */
    unsigned char*  nodes;          /* one list's query buffer, RECMP_ORDER_MAX nodes            */
    uint32_t        player[REC_PLAYERS][REC_PLAYER_DWORDS];
    unsigned char   actor[REC_PLAYERS]; /* the record is an actor's: its owner describes it      */
    unsigned        xbytes;         /* one owner's explored bitmap: a bit per map entry          */
    unsigned char*  xmap;           /* [REC_PLAYERS * xbytes] each actor's, by its record          */
    unsigned char   xvalid[REC_PLAYERS];
    unsigned char*  mapped;         /* the composed explored map (mp_world)                      */
    /* each actor's perspective at the application tick, by its record (mp_authority), in
       RecWorld's shape: the samples' ticks are application ticks (persp_world) */
    unsigned        npov[REC_PLAYERS];
    RecPov          pov[REC_PLAYERS][2];
    unsigned        pov_tick[REC_PLAYERS][2];
    unsigned        nsel[REC_PLAYERS];
    RecSelEntry*    sel;            /* [REC_PLAYERS * REC_SEL_MAX], seat-major                     */
    RecWorld        W;
    RecWorld        P;              /* the world the perspective reads (persp_world)             */
    unsigned char*  s_live;         /* the level-start composed world, which a seek composes on  */
    uint32_t*       s_unit;
    RecList*        s_list;
    uint32_t        s_player[REC_PLAYERS][REC_PLAYER_DWORDS];
    int             compose;        /* the actions compose data only (mp_rebuild)                */
    unsigned        rebuilt, croots;
    struct MpHeld*  q;              /* envelopes read and not yet applied, in ledger order       */
    unsigned        nq, capq;
    unsigned        observed, ocorpse;  /* the recorder's deaths, and those with a corpse level   */
    unsigned        made, destroyed, images, roots, rplayed;
    unsigned        bfinished, bunfinished; /* composites cleared: an image finished a build, or
                                               unfinished one (THE BUILD FINISH ON A MULTIPLAYER
                                               LEVEL)                                             */
    unsigned        said, shown;   /* text entries applied, and lines given to the message log  */
    unsigned        nown;           /* the recording machine's own blocks: the seats of every
                                       actor whose contributions it produced (mp_begin)          */
    struct { unsigned first, last; } own[RECMP_ACTORS];
    char            why[200];
} s_mp;
static int s_chat_hidden;           /* 1 = the HUD's Chat toggle is off (`chat hide`,
                                       `chat show`, `overlay chat`): THE OVERLAYS               */
/* THE OVERLAYS' state (game thread only) */
#define OV_RING 64                  /* the clicks held: the most recent, each with its tick      */
static struct {
    unsigned char    off[TAGPU_OV_CHAT];   /* a switched-off overlay, by TAGPU_OV_*: 0 is on      */
    int              live;                 /* item, n and s_rsel are this playing's this frame   */
    int              col_ok;               /* col holds this playing's colours                   */
    unsigned char    col[TAGPU_PK_OV_PLAYERS];
    TAGPU_PK_OVERLAY item[TAGPU_PK_MAX_OVERLAY];
    unsigned         n;
    unsigned         nrsel;                /* the slots s_rselAt lists                            */
    struct { unsigned tick; RecClick c; } ring[OV_RING];
    unsigned         rn, rnext;            /* clicks held, and the entry the next one takes       */
} s_ov;
/* each slot's recorded-selection bits, bit p for player p, and the slots that have any: every
   slot is listed once, when its bits first become nonzero, so nrsel <= TAGPU_PK_MAX_UNITS */
static uint16_t s_rsel[TAGPU_PK_MAX_UNITS];
static uint16_t s_rselAt[TAGPU_PK_MAX_UNITS];
_Static_assert(TAGPU_PK_OV_PLAYERS == REC_PLAYERS, "ov_col has a row for every recorded player");
_Static_assert(TAGPU_PK_OV_PLAYERS <= 16, "rsel has a bit for every recorded player");
_Static_assert(REC_CLICK_SHOW <= 256, "a click's age fits PK_OVERLAY.age");
/* FOLLOW, JUMP AND FREE CAM's state (game thread only) */
static struct {
    int      req;                   /* a request for the next frame visit: FQ_*                  */
    int      rseat, ronce;          /* ... a Follow's or Jump's seat, and which                  */
    int      seat;                  /* the seat the epoch in force shows, -1 none                */
    int      once;                  /* ... and whether it is a Jump                              */
    unsigned E;                     /* that epoch (tagpu_zoom_follow), 0 none                    */
    int      handed;                /* the epoch has been handed to tagpu_zoom                   */
    int      pov;                   /* `pov` asked: logged after this visit's answer             */
    struct { unsigned n; int how, vw, vh, x, y; uint32_t level; } at;   /* that answer           */
} s_fl = { 0, 0, 0, -1, 0, 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0 } };
/* the epochs handed out since the DLL loaded: never reset, so tagpu_zoom never sees one twice */
static unsigned s_flEpochs;
enum { FQ_NONE, FQ_FOLLOW, FQ_FREE };
static struct {
    unsigned n;
    struct { char name[37]; unsigned T; } a[RT_ARMED];
    int      tsaved;                /* the TRANSIENT declarations saved for this playing          */
} s_rt;                             /* the armed round trips (THE ROUND TRIP)                     */
static struct {
    int            saved;
    int            played;          /* the level's first step has run (THE PLAYER'S SETTINGS)     */
    int            stale;           /* a save ran before it, with the recording's options in      */
    unsigned char  set[20];         /* the settings' +0x108..+0x11B, as the player had them       */
    unsigned       difficulty;
    unsigned short each;
    unsigned short speed;           /* the game speed's ceiling main+0x38A4B, as the player had it */
    /* The browser's Start writes the recording's seats and map where the stock Start finds the
       player's (THE BROWSER'S START); a tacli playback finds them already in the registry. */
    int            seats_saved;
    unsigned char  entries[SET_ENTRIES * SET_ENTRY];
    char           map[SET_MAPLEN];
    unsigned       nseats;
} s_opt;
static struct {
    unsigned char* at;              /* V's PlayerInfo, or NULL                                    */
    unsigned char  was[REC_INFO_BYTES];
} s_vinfo;                          /* as the Start's fill left it (THE VIEWER'S SEAT)            */
static struct {
    int            taken;
    unsigned char  was[PL_STRIDE];
} s_vrec;                           /* record 10 as the level's first seating found it            */
enum { VB_OFF, VB_ARMED, VB_READY, VB_FAILED };
static volatile LONG s_vbuf;        /* record 10 was activated for this level's viewer (ARMED), and
                                       the level init's call gave it its buffers (READY) or could
                                       not (FAILED): THE VIEWER'S SEAT's ORDERING                 */
static unsigned   s_vlowest;        /* the lowest recorded seat, where FAILED moves the indexes   */
static int        s_vready;         /* viewer_init has run this level                             */
static unsigned   s_vframe = ~0u;   /* the GameTime of viewer_frame's last run                    */

static char* ta_main(void) { return *(char**)(size_t)TA_MAINPP; }

static char* player_rec(char* ta, unsigned i) { return ta + OFF_PLAYERS + (size_t)i * PL_STRIDE; }

/* the colours a PlayerInfo +0x96 may name: the sequence's own count, as 0x4B7F30 bounds it */
static unsigned colour_count(const char* ta)
{
    const unsigned short* seq = *(const unsigned short* const*)(ta + OFF_COLOURS);
    return seq ? *seq : 0;
}

static void pend_reset(void);
static void counts_log(const char* when, unsigned now);
static void mp_end(void);
static void follow_close(const char* why);
static void follow_visit(char* ta);
static void overlay_close(void);
static void overlay_visit(char* ta);
static int overlay_trigger(const char* line);
static void ov_clicks_take(const RecWorld* W);
static void ov_clicks_clear(void);
static void ov_click_put(unsigned tick, const RecClick* c);
static void first_step(char* ta);
static void slice_disarm(void);
static void slice_reset(void);
static void query_reset(void);

static void replay_close(void)
{
    /* Start's undo precedes thread creation; level teardown follows the engine's
       load-done gate. No loader can still consume this immutable plan. */
    InterlockedExchange(&s_mp_arena.state,MP_ARENA_OFF);
    s_viewPending = 0;
    follow_close("the playing ended");
    overlay_close();
    mp_end();
    recr_close(s_rd);
    recfile_leave();
    s_rd = NULL;
    s_path[0] = 0;
    memset(&s_span, 0, sizeof s_span);
    s_first = s_dead = s_over = 0;
    s_engine_cells = 0;
    s_cells_held = 0;
    s_endlog = 0;
    s_notes = 0;
    memset(s_nsite, 0, sizeof s_nsite);
    memset(&s_st, 0, sizeof s_st);
    memset(&s_fst, 0, sizeof s_fst);
    s_tin.n = 0;
    s_tin.overflow = 0;
    pend_reset();
    memset(&s_gst, 0, sizeof s_gst);
    memset(&s_rt, 0, sizeof s_rt);
    memset(s_made, 0, sizeof s_made);
    memset(&s_seek, 0, sizeof s_seek);
    s_hold = 0;                         /* a boundary's hold ends with its playing */
    s_frame_tick = ~0u;
    slice_reset();
    query_reset();
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

static int u32_take(const char** p, unsigned* v);

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

#define PLAY_FORM "`play <name> [<seat>] [timeline <N>] [at <T> [dump <name>]]`"

/* The request, PLAY_FORM: the recording's name, the seat whose view to show, or -1 for full
   visibility, the timeline to play (s_tl, 0 the original; first_iteration chooses it), and a seek
   to run at the first frame visit after the first step (THE SEEK), with the dump to take at its
   capture point. 1 read, 0 no request, -1 not understood (`why`). The file is gone afterwards
   whatever it held: one reader, one time (§6.3). */
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
    s_hold = 0;
    s_tl = 0;
    if (sscanf(line, "play %63s%n", got, &end) != 1) {
        snprintf(why, whycap, "%s is not %s", REQUEST, PLAY_FORM);
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
    if (!strncmp(p, "timeline ", 9)) {
        p += 9;
        if (!u32_take(&p, &s_tl)) {
            snprintf(why, whycap, "%s is not %s", REQUEST, PLAY_FORM);
            return -1;
        }
        while (*p == ' ') p++;
    }
    if (*p) {
        k = 0;
        if (strncmp(p, "at ", 3) || !seek_words(p + 3, &s_seek.T, s_seek.dump, sizeof s_seek.dump)) {
            snprintf(why, whycap, "%s is not %s", REQUEST, PLAY_FORM);
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

/* A refusal the browser marks the recording for (WATCH'S DATA REFUSALS, tagpu_replay.h): the
   code, and the running values it compared; code 0 for every other refusal. */
typedef struct { unsigned code, a, b; } Refusal;

/* The refusal to the browser, which marks the recording's row in replays\\index.dat
   (tagpu_replays.h); on the game thread, at the three checks that make one */
static void refused(const Refusal* r, const char* path)
{
    if (r->code && !_strnicmp(path, "replays\\", 8))
        tagpu_replays_refused(path + 8, r->code, r->a, r->b);
}

unsigned tagpu_replay_build_key(void)
{
    unsigned h = 2166136261u, v[2], i;
    const char* s;
    for (s = TAGPU_REPLAY_LAYOUT_SHA256; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    for (s = TAGPU_REPLAY_GLOBALS_SHA256; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    v[0] = TAGPU_LIM_WRECKS;
    v[1] = (unsigned)tagpu_limits_weapon_slots();
    for (i = 0; i < sizeof v; i++) h = (h ^ ((const unsigned char*)v)[i]) * 16777619u;
    return h;
}

/* The unit types, the weapons and the features against the header's (D8). Only at the first step: the level's
   loader reloads the unit data, so at the Start the types are the last level's, and a check there
   compared stale names (measured 2026-09-30: 279 types on both sides, the names' hashes apart). */
static int data_match(const char* ta, const RecHeader* h, char* why, unsigned cap, Refusal* r)
{
    unsigned n, hash;
    tagpu_rec_defs_print(ta, &n, &hash);
    if (!h->has_unitdefs || n != h->ndefs || hash != h->defs_hash) {
        snprintf(why, cap, "the unit types differ from the recording's (%u here, %u there)", n,
                 h->ndefs);
        r->code = TAGPU_REPLAY_REFUSED_UNITS;
        r->a = n;
        r->b = hash;
        return 0;
    }
    tagpu_rec_weapons_print(&n, &hash);
    if (n && (!h->has_weapons || n != h->nweapons || hash != h->weapons_hash)) {
        snprintf(why, cap, "the weapons differ from the recording's (%u slots here, %u there)", n,
                 h->nweapons);
        r->code = TAGPU_REPLAY_REFUSED_WEAPONS;
        r->a = n;
        r->b = hash;
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
            r->code = TAGPU_REPLAY_REFUSED_FEATURES;
            r->a = n;
            r->b = hash;
            return 0;
        }
    }
    return 1;
}

/* Everything the seating rests on that the recording alone decides, and the sites it needs in
   (§6.4). 1 with V, record 10, in `viewer`. */
/* The map a playback's Start reads. A skirmish starts from the SKIRMISH screen's map, which its
   recording keeps as the settings map; a network game's is the battleroom's, its level's own
   (RT_MAP), while its settings map is whatever the host's SKIRMISH screen last held. tacli's
   replay play makes the same choice. */
static const char* start_map(const RecHeader* h)
{
    return h->mode == MODE_NETWORK ? h->map : h->settings_map;
}

static int validate_header(const RecHeader* h, char* why, unsigned cap, unsigned* viewer,
                           Refusal* r)
{
    unsigned i;
    if (strcmp(h->layout, TAGPU_REPLAY_LAYOUT_SHA256)) {
        snprintf(why, cap, "it was recorded with another exclusion file (replay-layout.tsv)");
        r->code = TAGPU_REPLAY_REFUSED_LAYOUT;
        r->a = 0;
        r->b = tagpu_replay_build_key();
        return 0;
    }
    if (strcmp(h->globals, TAGPU_REPLAY_GLOBALS_SHA256)) {
        snprintf(why, cap, "it was recorded with another positive list (replay-globals.tsv)");
        r->code = TAGPU_REPLAY_REFUSED_GLOBALS;
        r->a = 0;
        r->b = tagpu_replay_build_key();
        return 0;
    }
    /* a recorded projectile ID is a record of a pool this size: the pool the limits patch
       allocates, as the wreck pool's size is checked (data_match) */
    if (h->proj_cap != TAGPU_LIM_PROJ) {
        snprintf(why, cap, "it was recorded with a projectile pool of %u records, this build's "
                 "holds %u", h->proj_cap, (unsigned)TAGPU_LIM_PROJ);
        return 0;
    }
    if (h->mode != MODE_SKIRMISH && h->mode != MODE_NETWORK) {
        snprintf(why, cap, "it is neither a skirmish nor a multiplayer game (mode %u)", h->mode);
        return 0;
    }
    if (h->mode == MODE_NETWORK && (!h->has_mp || !h->has_mp_seats || !h->has_unitdefs)) {
        snprintf(why, cap, "it is a multiplayer game without its contributors, seats or unit "
                 "types");
        return 0;
    }
    if (!tagpu_puppet_gates_in()) {
        snprintf(why, cap, "the puppet gates are not in (tagpu_puppet.c)");
        return 0;
    }
    if (h->mode != MODE_NETWORK && recr_holds_cells(h) && !tagpu_puppet_contest_in()) {
        snprintf(why, cap, "it holds its cells, and a contest site holds another module's "
                 "bytes (tagpu_puppet.c, THE CONTEST'S RULE)");
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
    if (!start_map(h)[0] || (h->mode != MODE_NETWORK && !h->has_settings_map) ||
        !h->has_options) {
        snprintf(why, cap, "the recording lacks the map or the options");
        return 0;
    }
    /* and at most ten, every record a game seats: the reader refuses an eleventh (RT_PLAYER), and
       the viewer takes record 10 (THE VIEWER'S SEAT) */
    if (h->nseats < 2) {
        snprintf(why, cap, "it seats %u players", h->nseats);
        return 0;
    }
    /* a multiplayer recording seats every remote player, human or AI, as 3, as its recorder held
       it (MEASURED 2026-10-05, the four-actor recording). The explosion's owner gate tests 3 alone
       (0x49A043), and THE DEATHS ON A MULTIPLAYER LEVEL rest on it, so no other type is
       admitted */
    for (i = 0; i < h->nseats; i++) {
        if (h->seats[i].controller < 1 ||
            h->seats[i].controller > (h->mode == MODE_NETWORK ? 3u : 2u)) {
            snprintf(why, cap, "seat %u's controller is %u", h->seats[i].index,
                     h->seats[i].controller);
            return 0;
        }
    }
    /* the engine sizes the share list by the count (0x4938B7..0x4938C7) and copies every active
       record that is neither local nor a watcher into it (0x4938FF..0x4939BA): every seat */
    if (h->nplayers != h->nseats) {
        snprintf(why, cap, "it counts %u players and seats %u", h->nplayers, h->nseats);
        return 0;
    }
    if (h->units_each < 1 || h->units_each > TAGPU_LIM_UNITS) {
        snprintf(why, cap, "it allowed %u units a player, this build at most %u", h->units_each,
                 TAGPU_LIM_UNITS);
        return 0;
    }
    *viewer = REC_VIEWER;
    return 1;
}

#define REPORTER_DLL   0x0051E58Cu  /* nonzero once reporter.dll is loaded: set only by 0x46C00F */

/* Record 10 is the engine's free record (THE VIEWER'S SEAT): no type, no AI object, no unit, its
   PlayerInfo made; and at a Start none of the three buffers the player init makes, so the level
   init's call leaks nothing (a live rewind finds the ones its earlier rewind made, and reuses
   them). And no reporter.dll: its export receives +0x2A42 as a value (0x46C7CC..0x46C7E7), and
   10 names no seat there. */
static int viewer_free(const char* ta, int start, char* why, unsigned cap)
{
    const char* rec = ta + OFF_PLAYERS + (size_t)REC_VIEWER * PL_STRIDE;
    if (rec[PL_CONTROLLER] || *(void* const*)(rec + PL_AI) ||
        *(const unsigned short*)(rec + PL_LIVE) || !*(void* const*)(rec + PL_INFO)) {
        snprintf(why, cap, "the viewer's record 10 is not the engine's free record");
        return 0;
    }
    if (start && (*(void* const*)(rec + PL_GROUPS) || *(void* const*)(rec + PL_SIGHT) ||
                  *(void* const*)(rec + PL_EC))) {
        snprintf(why, cap, "the viewer's record 10 holds buffers already");
        return 0;
    }
    if (*(const unsigned*)(size_t)REPORTER_DLL) {
        snprintf(why, cap, "the network reporter is loaded");
        return 0;
    }
    return 1;
}

/* ... and what it rests on in the engine's state the Start has just made (`start`) or a live
   rewind finds: not a network game, a skirmish's GameingState, the map the Start read, the tables
   the fill indexes, and the viewer's record free. */
static int validate_engine(const char* ta, const RecHeader* h, char* why, unsigned cap, int start)
{
    const char* set = *(const char* const*)(ta + OFF_SETTINGS);
    const char* gs = *(const char* const*)(ta + OFF_GSTATE);
    char map[RECR_TEXT];
    unsigned i;
    if (ta[OFF_NETWORK] & 1) {
        snprintf(why, cap, "this is a network game");
        return 0;
    }
    if (!gs || *(const int*)gs != MODE_SKIRMISH) {
        snprintf(why, cap, "this Start is not a skirmish's");
        return 0;
    }
    if (!set) {
        snprintf(why, cap, "the settings lack the map or the options");
        return 0;
    }
    for (i = 0; i + 1 < sizeof map && set[SET_MAP + i]; i++) map[i] = set[SET_MAP + i];
    map[i] = 0;
    if (_stricmp(map, start_map(h))) {
        snprintf(why, cap, "the Start read the map %s, the recording's is %s", map,
                 start_map(h));
        return 0;
    }
    /* the engine indexes its side and colour tables with a seat's side and colour unbounded
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
    if (!viewer_free(ta, start, why, cap)) return 0;
    if (strcmp(h->build, GIT_COMMIT))
        tagpu_logf("replay: recorded by the build %s, played by %s: a puppet computes nothing a "
                   "build changes (D8)", h->build, GIT_COMMIT);
    return 1;
}

/* the view a request names: -1 for full visibility, or a seat the recording seats */
static int view_ok(const RecHeader* h, int view, char* why, unsigned cap)
{
    unsigned i;
    if (view < 0) return 1;
    for (i = 0; i < h->nseats; i++)
        if (h->seats[i].index == (unsigned)view) return 1;
    snprintf(why, cap, "the request's view names record %d, which the recording does not seat",
             view);
    return 0;
}

/* a recorded name into one of the record's 0x1E-byte name fields, NUL-ended */
static void name_put(char* dst, const char* name)
{
    size_t n = strlen(name);
    if (n > PL_NAMELEN - 1) n = PL_NAMELEN - 1;
    memset(dst, 0, PL_NAMELEN);
    memcpy(dst, name, n);
}

/* THE VIEWER'S SEAT (demo-recorder-ten-seats.md). V is record 10 on every playback. The engine
   constructs eleven player records with their PlayerInfo (0x41D97E..0x41D997, each through
   0x463BE0), its players reset clears eleven (0x4644D7..0x46450F) and its player teardown frees
   eleven records' buffers (0x464A00..0x464A9B); record 10 is its "no player", which no players
   loop reaches (0x4655A8, 0x481707..0x48177A, 0x465F87, 0x41DEC5) and no recording carries
   (replay-globals.tsv, `viewerrec`). So nothing recorded is ever overwritten with viewer state,
   and every record 0-9, seated or not, is applied and compared as recorded.
   - The activation 0x464290(10, 3) writes only inside record 10 -- its five 11-byte rows, +0x146,
     +0x147 and +0x148 = 10, [rec+0] = 1, +0x74 = 0 without a free -- and clears its PlayerInfo's
     +0x9B bit 5 (0x4642BE..0x4644BE). BOUND:
     type 3 with [rec+0] set before the player init 0x464700 ever sees the record is what makes it
     skip the AI object and 0x40B320 (0x46489D..0x4648A5), which would write the ten-entry table
     0x5119C0 at index 10 (0x40B340).
   - The empty block, first above last (+0x67 = 1, +0x6B = 0, no address at all): every walk of
     the controlled or viewed block tests `first > last` before its first unit (the eleven, from
     0x41C345 to 0x48DD40, demo-recorder-ten-seats.md §3), and a walker that missed the test would
     fault at once rather than act on a seat's unit. And the ID range [+0x6F, +0x71] = [1, 1]: the
     ID cycle 0x48C190 walks the controlled record's range and turns ID 0 into a NULL unit it
     reads (0x48C1EF..0x48C213), so with [1, 1] it reads slot 1 alone, which every unit array has,
     and finds it unselected, since every selection setter walks the controlled block. The level
     init gives blocks to records 0-9 only (0x48592B), so nothing else writes these.
   - The watcher bit, as the network watcher switch sets it (0x46569D), and the side and colour of
     `shown`, a seat of the recording. BOUND: both are the header's, which validate_engine has
     checked against the side count and the colour sequence's count before either caller runs; the
     sequence main+0x148DB has one writer, at start-up (0x431911). Record 10's own colour is not
     one: the players reset gives every record its index (0x46461E, the loop 0..10), the sequence
     holds ten frames, and the resource bar draws the viewed record's colour frame with no bound
     (0x468E4B, 0x467C18..0x467C24, GAF_DrawTransformed 0x4C7580), so with V viewed it would draw
     the dword past the ten frames as a frame.
   seat_level and rw_prepare share it; its buffers are the level init's call's (THE VIEWER'S
   BUFFERS) or rw_prepare's. */
static void viewer_seat(char* ta, const RecSeat* shown)
{
    char* rec = player_rec(ta, REC_VIEWER);
    unsigned char* info;
    const uint32_t first = 1, last = 0;
    const uint16_t id = 1;
    E_ActivateSeat(REC_VIEWER, 3);
    info = *(unsigned char**)(rec + PL_INFO);
    if (info) {
        info[INFO_FLAGS] |= INFO_WATCHER;
        info[INFO_SIDE] = shown->side;
        info[INFO_LOGO] = shown->logo;
    }
    memcpy(rec + PL_BLOCKFIRST, &first, 4);
    memcpy(rec + PL_BLOCKLAST, &last, 4);
    memcpy(rec + PL_IDFIRST, &id, 2);
    memcpy(rec + PL_IDLAST, &id, 2);
}

/* record 10 whole, as the first seating of the level found it, for tagpu_replay_level_after */
static void viewer_take(const char* ta)
{
    if (s_vrec.taken) return;
    memcpy(s_vrec.was, ta + OFF_PLAYERS + (size_t)REC_VIEWER * PL_STRIDE, PL_STRIDE);
    s_vrec.taken = 1;
}

/* §6.4's seating, after validate; `view` the viewed player, V or a recorded seat (§2.9) */
static void seat_level(char* ta, const RecHeader* h, unsigned view)
{
    unsigned char seatv[REC_PLAYERS] = { 0 }, localv[REC_PLAYERS] = { 0 };
    unsigned char* info;
    void* objective;
    unsigned i, lowest = REC_PLAYERS;
    const RecSeat* shown = &h->seats[0];        /* validate_header: at least two seats */
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
        localv[s->index] = s->controller == 1 || s->controller == 2;   /* getter 75 there */
        if (s->index == h->controlled) shown = s;
        if (s->index < lowest) lowest = s->index;
    }
    /* V shows the side and colour of the recording's controlled player, or of its first seat when
       the controlled record is not seated. With full visibility V is the viewed record, and the
       interface takes the viewed record's side and colour: MAIN2 is loaded once from its side
       (0x497B56..0x497BD8), the chrome and both bars read the side every frame
       (0x467D98..0x467DB3), and the resource bar draws its colour (0x467C00 at 0x4690AB); the
       side menus %sGEN and %sDL take the controlled record's, which is V too (0x41B2E0). What
       record 10 and its PlayerInfo held goes back after the teardown (tagpu_replay_level_after).
       V is no puppet seat: the puppet table has ten entries, and every gate keys on a unit's
       owner, which is never 10. */
    viewer_take(ta);
    info = *(unsigned char**)(player_rec(ta, REC_VIEWER) + PL_INFO);
    s_vinfo.at = info;
    if (info) memcpy(s_vinfo.was, info, REC_INFO_BYTES);
    viewer_seat(ta, shown);
    /* a record the stock fill activated that the recording does not seat */
    for (i = 0; i < REC_PLAYERS; i++) {
        char c = player_rec(ta, i)[PL_CONTROLLER];
        if (!seatv[i] && (c == 1 || c == 2)) E_ActivateSeat(i, 0);
    }
    /* +0x2A43 = 10 before the load is safe: the first reader of the viewed record's counters, the
       sight rebuild's fog 0x466C20 through 0x4816A0(1) at 0x497AE5, follows the level init's call
       0x4919C8 at 0x497581, which gives record 10 its counters (THE VIEWER'S BUFFERS) */
    ta[OFF_CONTROLLED] = (char)REC_VIEWER;
    ta[OFF_VIEWED] = (char)view;
    *(unsigned short*)(ta + OFF_NPLAYERS) = (unsigned short)h->nplayers;
    objective = *(void**)(ta + OFF_OBJECTIVE);
    if (objective) E_Disarm(objective);
    s_cells_held = h->mode != MODE_NETWORK && recr_holds_cells(h);
    s_engine_cells = h->mode == MODE_NETWORK || s_cells_held;
    s_vlowest = lowest;
    InterlockedExchange(&s_vbuf, VB_ARMED);
    tagpu_puppet_begin(seatv, localv, s_engine_cells, h->mode == MODE_NETWORK);
}

/* 0x464990(): the level init's players, no argument, plain `ret` (0x4649C6): 0x464700 for each of
   records 0-9 with a type (+0x73), then the AI script 0x4648E0 */
typedef void (__cdecl* PFN_PlayersInit)(void);
#define E_PlayersInit ((PFN_PlayersInit)0x00464990u)
/* 0x464700(rec): the player init, stdcall `ret 4` (0x4648D6). It allocates the sight counters
   +0x7C (freeing the old ones, 0x464855) at the map's size, the group table +0x78 through 0x480190
   (0x464898; it frees no old one, 0x4801A1..0x4801B1) and +0xEC when NULL (0x4647CF..0x4647EA),
   zeroes the resources and storage (0x46476B..0x464795) and sets +0xF0, +0xF4 and +0xF8 to
   GameTime (0x464715, 0x464727, 0x464765) */
typedef void (__stdcall* PFN_PlayerInit)(char* rec);
#define E_PlayerInit ((PFN_PlayerInit)0x00464700u)

/* record 10's buffers through the player init, after the two values that make it skip the AI
   object (THE VIEWER'S SEAT) and with none of its buffers there yet, so the call leaks nothing;
   1 when all three exist afterwards */
static int viewer_buffers(char* ta)
{
    char* rec = player_rec(ta, REC_VIEWER);
    if (*(const uint32_t*)rec != 1 || rec[PL_CONTROLLER] != 3 || *(void**)(rec + PL_GROUPS) ||
        *(void**)(rec + PL_SIGHT))
        return 0;
    E_PlayerInit(rec);
    return *(void**)(rec + PL_SIGHT) && *(void**)(rec + PL_GROUPS) && *(void**)(rec + PL_EC);
}

/* THE VIEWER'S BUFFERS: the level init's call 0x4919C8, in LoadGameData_Main 0x4917D0 on the
   LOADER thread (0x497581), its only call site, retargeted here. The engine's own init first,
   then, on a level the seating armed, record 10's buffers. Three invariants:
   - ORDERING. s_vbuf and s_vlowest are written on the game thread at the seating, before the load
     state creates the loader thread (0x4982C5..0x4982CA), and s_vbuf is cleared at the level's
     end, after the loader has exited, and at every fill before a seating, so a seating that never
     reached its level's end arms no later level; the loader acts on it only on a puppet level,
     whose mode the same seating set before the loader thread existed. This thread's result is published through an interlocked
     write and read at the first step, which follows the loader's last write (bit 1 of
     main+0x38D75, set at 0x497C5F, tested at 0x498342).
   - LIFETIME. Record 10 gets its buffers at the point of the load where records 0-9 get theirs,
     and loses them where they do (0x464A00 at 0x491BA9), so from the loader's first reader of the
     viewed record's counters (0x497AE5) to the teardown, record 10 is shaped as a seat's.
   - BOUND. The activation as type 3 before this call (seat_level) keeps 0x5119C0[10] unwritten.
   On a failure the controlled and viewed indexes move to the lowest seat, whose counters the
   engine's own init made, and the first step refuses: the level stays empty, as the engine's own
   out-of-memory leaves a seat. */
static void __cdecl tagpu_replay_level_players(void)
{
    char* ta;
    int ok;
    E_PlayersInit();
    if (!tagpu_puppet_on() ||
        InterlockedCompareExchange(&s_vbuf, VB_ARMED, VB_ARMED) != VB_ARMED) return;
    ta = ta_main();
    ok = ta && viewer_buffers(ta);
    if (!ok && ta) ta[OFF_CONTROLLED] = ta[OFF_VIEWED] = (char)s_vlowest;
    InterlockedExchange(&s_vbuf, ok ? VB_READY : VB_FAILED);
    if (!ok) tagpu_log("replay: the viewer's record 10 could not be given its buffers");
}

/* The open recording's path and its first and last GameTime, for the replay HUD's timeline and
   bookmarks (tagpu_hudstate.c, tagpu_hudmarks.c): the summary walks the block headers only. */
static void span_read(const char* path)
{
    RecSummary sm;
    char why[200];
    snprintf(s_path, sizeof s_path, "%s", path);
    memset(&s_span, 0, sizeof s_span);
    if (recr_summary(path, &sm, why, sizeof why) && sm.last > sm.start) {
        s_span.first = sm.start;
        s_span.last = sm.last;
        s_span.ok = 1;
    } else {
        tagpu_logf("replay: %s: no span for the HUD: %s", path, why);
    }
}

/* The seating, once everything it rests on has been checked: S-a's, for both routes. */
static void mp_arena_prepare(const RecHeader* header)
{
    if (header->mode!=3) { InterlockedExchange(&s_mp_arena.state,MP_ARENA_OFF); return; }
    if (!s_mp_arena_in || !header->has_mp || !header->has_mp_seats ||
        header->slots!=header->mp_seats.slots || header->units_each!=header->mp_seats.each ||
        !tagpu_recmp_roster_valid(&header->mp_seats)) {
        InterlockedExchange(&s_mp_arena.state,MP_ARENA_FAILED);
        return;
    }
    /* The game thread fills this once before the loader-start call at 0x4982CA. The
       plan stays immutable through loader completion; publication and the
       loader's result use interlocked operations, not an assumed delay. */
    s_mp_arena.roster=header->mp_seats;
    InterlockedExchange(&s_mp_arena.state,MP_ARENA_ARMED);
}

static void seat(char* ta, const char* name, unsigned v, int view)
{
    s_view = view >= 0 ? (unsigned)view : v;
    seat_level(ta, recr_header(s_rd), s_view);
    mp_arena_prepare(recr_header(s_rd));
    snprintf(s_name, sizeof s_name, "%s", name);
    s_viewer = v;
    s_first = 1;
    tagpu_logf("replay: %s seated: %u players on %s, the viewer at record %u", name,
               recr_header(s_rd)->nseats, start_map(recr_header(s_rd)), v);
    if (s_seek.pending)
        tagpu_logf("replay: %s: a seek to GameTime %u waits for the first step", name, s_seek.T);
}

static void __cdecl replay_fill(void)
{
    char* ta = ta_main();
    char name[NAME_MAX_], path[MAX_PATH], why[200];
    unsigned v = 0;
    int k, view = -1;
    Refusal r = { 0, 0, 0 };
    replay_close();
    tagpu_puppet_end();
    InterlockedExchange(&s_vbuf, VB_OFF);
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
    if (!ta || !validate_header(recr_header(s_rd), why, sizeof why, &v, &r) ||
        !validate_engine(ta, recr_header(s_rd), why, sizeof why, 1) ||
        !view_ok(recr_header(s_rd), view, why, sizeof why)) {
        tagpu_logf("replay: not playing %s: %s", name, ta ? why : "no main block");
        refused(&r, path);
        replay_close();
        return;
    }
    span_read(path);
    seat(ta, name, v, view);
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
    s_opt.speed = *(unsigned short*)(ta + OFF_SPEED_CEIL);
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

/* the live seats, map and count and s_opt's, each put in the other's place */
static void seats_exchange(char* ta, char* set)
{
    unsigned char e[sizeof s_opt.entries];
    char m[sizeof s_opt.map];
    unsigned n = *(unsigned*)(ta + OFF_NSEATS);
    memcpy(e, set, sizeof e);
    memcpy(set, s_opt.entries, sizeof e);
    memcpy(s_opt.entries, e, sizeof e);
    memcpy(m, set + SET_MAP, sizeof m);
    memcpy(set + SET_MAP, s_opt.map, sizeof m);
    memcpy(s_opt.map, m, sizeof m);
    *(unsigned*)(ta + OFF_NSEATS) = s_opt.nseats;
    s_opt.nseats = n;
}

/* the player's seats, map and count back, when the browser's Start took them */
static void seats_restore(char* ta)
{
    char* set = ta ? *(char**)(ta + OFF_SETTINGS) : NULL;
    if (!s_opt.seats_saved) return;
    s_opt.seats_saved = 0;
    if (!set) return;
    memcpy(set, s_opt.entries, sizeof s_opt.entries);
    memcpy(set + SET_MAP, s_opt.map, sizeof s_opt.map);
    *(unsigned*)(ta + OFF_NSEATS) = s_opt.nseats;
}

/* 1 when the recording's options were in and the player's are back */
static int options_restore(char* ta)
{
    char* set;
    seats_restore(ta);
    if (!s_opt.saved) return 0;
    s_opt.saved = 0;
    if (!ta) return 0;
    set = *(char**)(ta + OFF_SETTINGS);
    if (set) memcpy(set + SET_OPTIONS, s_opt.set, sizeof s_opt.set);
    *(unsigned*)(ta + OFF_DIFFICULTY) = s_opt.difficulty;
    *(unsigned short*)(ta + OFF_EACH_MENU) = s_opt.each;
    /* both fields: the throttle only raises the live speed toward the ceiling
       (0x495465..0x49547B), so a live 80 under a ceiling of 10 would stay 80, and a skirmish
       keeps its speed into the next level (only mode 3 resets it, 0x4912C9, 0x49199F) */
    *(unsigned short*)(ta + OFF_SPEED_CEIL) = s_opt.speed;
    *(unsigned short*)(ta + OFF_SPEED_LIVE) = s_opt.speed;
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
   is in flight at a time.
   When the browser's Start put the recording's seat entries, map and seat count (main+0x38D81)
   in, they travel with the options: exchanged around each save, put back by the teardown. The
   save writes SkirmishMap from +0x11C and Player%dController..Energy from the entries below the
   count (0x4314F0..0x43171D), so with those three exchanged it writes the player's own.
   The game speed's ceiling main+0x38A4B travels the same way: the save writes it to the registry's
   `gamespeed` (0x43136B), and the HUD's speed button changes it during a playback
   (tagpu_replay_speed_set), so the save writes the player's speed and the teardown puts it back. A
   change the Options screen's GAME speed slider makes during a playback is put back with the
   rest. */
static void* s_saveRet;

/* the live options and s_opt's, each put in the other's place */
static void options_exchange(char* ta)
{
    char* set = *(char**)(ta + OFF_SETTINGS);
    unsigned char t[sizeof s_opt.set];
    unsigned d = *(unsigned*)(ta + OFF_DIFFICULTY);
    unsigned short e = *(unsigned short*)(ta + OFF_EACH_MENU), sp;
    if (set) {
        memcpy(t, set + SET_OPTIONS, sizeof t);
        memcpy(set + SET_OPTIONS, s_opt.set, sizeof t);
        memcpy(s_opt.set, t, sizeof t);
        if (s_opt.seats_saved) seats_exchange(ta, set);
    }
    *(unsigned*)(ta + OFF_DIFFICULTY) = s_opt.difficulty;
    s_opt.difficulty = d;
    *(unsigned short*)(ta + OFF_EACH_MENU) = s_opt.each;
    s_opt.each = e;
    sp = *(unsigned short*)(ta + OFF_SPEED_CEIL);
    *(unsigned short*)(ta + OFF_SPEED_CEIL) = s_opt.speed;
    s_opt.speed = sp;
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
static int first_checks(const char* ta, const RecHeader* h, char* why, unsigned cap, Refusal* r,
                        int at_start)
{
    char stem[RECR_TEXT];
    unsigned each = *(const unsigned short*)(ta + OFF_EACH);
    unsigned slots = *(const unsigned short*)(ta + OFF_NSLOTS);
    unsigned now = *(const unsigned*)(ta + OFF_GAMETIME);
    if (at_start && InterlockedCompareExchange(&s_vbuf, VB_READY, VB_READY) != VB_READY) {
        snprintf(why, cap, "the viewer's record could not be prepared");
        return 0;
    }
    if (h->mode==3 && InterlockedCompareExchange(&s_mp_arena.state,0,0)!=MP_ARENA_READY) {
        snprintf(why,cap,"the loader did not establish the recorded unit-block ownership");
        return 0;
    }
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
    if (!data_match(ta, h, why, cap, r)) return 0;
    if (at_start && now != h->start) {
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
   The first step comes in two parts. At the capture point of the level's first iteration T,
   before the host's unit tick, it makes the units the level started with -- recorded as images,
   with no create before them -- through the create, as the loader made them, writes image T and
   seeds the wind from image T (wind_seed), and then makes the capture point's write (THE GLOBALS
   AND THE PROJECTILES), so that the host's unit tick T runs on the units and the globals the
   recording machine's ran on (THE FIRST STEP'S UNITS). After the unit tick it plays the events of
   T and image T+1, as every later step does.

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
   the contact bits 8-10 and 12, which the host's radar pass computes for its own view (D1),
   except that a seek, a skirmish recording's takeover included, puts the recorded ones back
   through rw_contacts: bits 8-10 when the host shows the recording's own viewed player, as a
   change to that view does (sight_change), all four on return-to-now (R14b). The harness
   compares them all but the contact bits (tools/replay/tarec.py compare --puppet), and bits 8-10
   too when the host shows the recording's own viewed player. +0x110 bit 14, dying, is
   a value like the rest: the reaper's Send_UnitDeath is refused for a puppet (tagpu_puppet.c),
   so a unit the recording marks dying lives until its recorded death, in the step it died in
   there.

   A PLAYER'S IMAGE is written raw under the same exclusions, for every record 0-9, but
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
#define U_ALT          0x6E
#define U_TYPEIX       0xA6         /* u16, the type's index; 0 for a free slot (0x486DC7)          */
#define U_GROUP        0xAC         /* the control group, or -1: the setter 0x480250's alone        */
#define GROUPS         10           /* the owner's group table: 10 entries of 0x20 (0x480190)     */
#define U_ATTACKER     0xF0
#define U_OWNERIX      0xFF
#define U_BUILT        0x104        /* the build left, a float: 0.0 when finished                  */
#define U_STATE        0x110
#define U_SELECTED     0x10u        /* +0x110 bit 4: the viewer's own selection (a drop row)      */
#define CONTACT_VIEWED 0x700u       /* +0x110 bits 8-10: the viewed record's contacts, which only
                                       0x467440 and its visitors write on a puppet host           */
#define CONTACT_SCAN   0x1700u      /* ... and bit 12, which loop 1 clears and loop 4 sets for a
                                       type-1 or type-2 owner: the whole scan                     */
#define U_MODEL        0x9E         /* the model instance (Object3do): the create 0x485D40 stores it
                                       (0x485DCC, 0x485E0E) and only the destructor frees it,
                                       nulling the field at once (0x486D93..0x486DA3)            */
#define O3_FRAME       0x10         /* ... its composite frame, the cached 1x bake (stored through
                                       0x4586FA..0x45871E); 0 makes the next draw bake it again  */
#define OFF_GUIWORD    0x37EBE      /* the GUI word, the viewer's own: bit 4 asks for a refresh  */
#define GUI_REFRESH    0x10u
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
#define U_POS          0x6A         /* x, altitude, z: i32 each                                    */
#define U_TURN         0x64         /* the turn, u16 x3                                            */
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
#define OF_KEEPTARGETS 0x10000u     /* +0x42 bit 16: the weapon reset held off (§2.4)              */
#define OF_STOPBUILDING 0x400000u   /* +0x42 bit 22: the destructor's StopBuilding                 */
#define ORD_STATE      0x05         /* u8, the handler's state                                     */
#define ORD_EVMASK     0x06         /* u32, the events its next call receives; bit 1 the
                                       destructor's handler call (0x43A1F5..0x43A21A)              */
#define ORD_DEADLINE   0x0A         /* u32 GameTime, -1 for none (0x43B7E2..0x43B7FB)              */
#define ORD_SEENXZ     0x32         /* u32, the last-seen place                                    */
#define ORD_EVENTS     0x4E         /* u32, the events fired since the last call (0x438870)        */
#define DESC_HANDLER   0x04         /* the handler's address, stdcall (unit, node, events): a
                                       handler is matched by it, never by the action's rank, which
                                       the sort 0x43C050 gives                                     */
#define H_BUILDINGBUILD   0x00402640u
#define H_MOBILEBUILD     0x00403A20u
#define H_VTOLMOBILEBUILD 0x00413D80u
#define H_BUILDWEAPON     0x00402B70u
#define H_PARALYZE        0x00402D10u
#define H_ATTACKCHASE     0x004034A0u
#define U_SLOTWEAPON   0x10         /* + p1 * 0x1C: a stock slot's weapon (0x402B7F..0x402B89)     */
#define U_SLOTTARGET   0x04         /* + i * 0x1C: a stock slot's target, two words; none is
                                       (0, 0x8000) (0x48A160, and AutoAim's 0x48A2AF, 0x48A2B4)    */
#define ACQ_TABLE      0x005119C0u  /* ten acquisition records, by seat (0x40A10A..0x40A13A)       */
#define OBJ_ARMED      0x88         /* the objective's test switch (0x48DF90, 0x4904B0)            */
#define DESC_BEGIN     0x00512344u  /* the order descriptors, 0x19 bytes each, to *(0x512348)      */
#define DESC_END       0x00512348u
#define DESC_STRIDE    0x19
#define DESC_MARKS     0x0C         /* the marker mask; bit 0 draws the build site (0x439B87)      */
#define REPLY_KINDS    24           /* the notice table 0x5086D8                                   */
#define OFF_FXLAYERS   0x38D77      /* the particle layer table, FX_LAYERS of 16 bytes (0x471D90)  */
#define FX_LAYERS      10
/* +0x110 bits excluded from the general image writes: layer and walk class (0-3), contact bits
   (8-10 and 12; R14b's rw_contacts restores 8-10 in a seek and a skirmish takeover in the
   recording's viewed record, all four on return-to-now), the fix-up's bit (16, step 3), carried
   (17), and the create's 28-31 */
#define U_ENGINE_BITS  0xF003170Fu
/* +0x110 bits 26 and 27, the contest's marks: the holder's and the loser's (THE FOOTPRINT WRITERS,
   tagpu_puppet.c). The footprint writers' alone where they write the cells (s_cells_held) */
#define U_MARKS        0x0C000000u
#define NOTES_MAX      32           /* the log lines a level's anomalies may take                 */
#define NOTES_PER_SITE 3            /* ... and one call site's: an anomaly that repeats every step
                                       is counted in the level's counts, so its first lines stand
                                       for the rest and leave the budget to the other kinds        */

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

/* THE TAKEOVER's engine calls (DISASSEMBLED). 0x463C60(type): a player record's type, thiscall
   `ret 4`: +0x73 = type (0x463C67), and PlayerInfo +0x94 = type unless it is 3
   (0x463C64..0x463C6F). */
typedef void (__thiscall* PFN_SetType)(char* rec, unsigned type);
#define E_SetType  ((PFN_SetType)0x00463C60u)
/* 0x408CB0(rec): the AI object's constructor on a 0x3D-byte block, thiscall `ret 4`, returning the
   block, which the player init stores to +0x74 (0x4648A7..0x4648C1). Its sub-objects point at
   [rec+0x78] + 0x20k (0x408CF0..0x408D0C), so the group table must exist. */
typedef void* (__thiscall* PFN_AiMake)(void* obj, char* rec);
#define E_AiMake   ((PFN_AiMake)0x00408CB0u)
/* 0x40B320(seat): the acquisition record's maker, stdcall `ret 4`: allocates 0x10D bytes,
   constructs them through 0x409160 -- which draws the simulation RNG 8 times in 0x40A150
   (0x40A15F..0x40A23B, called at 0x4092E8) -- stores 0x5119C0[seat] unconditionally (0x40B340)
   and then calls 0x409730 on it. On an allocation failure it stores 0 and calls 0x409730 with
   ecx 0 (0x40B350..0x40B35B): the engine's own crash, as at every level load. */
typedef void (__stdcall* PFN_AcqMake)(unsigned seat);
#define E_AcqMake  ((PFN_AcqMake)0x0040B320u)
/* 0x4648E0(): the AI profiles, plain `ret` (0x46498D), over the ten records. It parses the AI
   profile (0x4356C0(7), falling back to ai\default.txt at 0x507308) through 0x4B7A30; its Weight
   (0x409DC0) runs for every record with +0x74 set and reads 0x5119C0[seat] untested, its Limit
   (0x409E90) for type 2; then 0x409F80(i) and 0x40A040(i) for each [rec+0] && +0x73 == 2
   (0x46494E..0x464982). */
typedef void (__cdecl* PFN_AiProfiles)(void);
#define E_AiProfiles ((PFN_AiProfiles)0x004648E0u)
/* 0x48BD00(): every unit deselected, plain `ret`: +0x110 &= 0xFFFFFF2F on every slot from
   [main+0x14357] to [main+0x1435B], then 0x491D70(0) (0x48BD00..0x48BD42), which defers under a
   modal screen */
typedef void (__cdecl* PFN_DeselectAll)(void);
#define E_DeselectAll ((PFN_DeselectAll)0x0048BD00u)
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
/* 0x4B0BC0(name, a0, a1, a2, a3): a query, thiscall on the COB object, `ret 0x14`; each argument
   a pointer the query reads in (0x4B0C5C..0x4B0CCA) and writes back (0x4B0CE6..0x4B0D11), NULL
   for none */
typedef int (__thiscall* PFN_Query)(void* cob, const char* name, int* a0, int* a1, int* a2,
                                    int* a3);
#define E_Query      ((PFN_Query)0x004B0BC0u)
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
static uint32_t s_raw1h[REC_UNIT_DWORDS], s_stateh[REC_UNIT_DWORDS];  /* the two, less U_MARKS */
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
    m_bytes(s_raw1, 0x76, 8, 0);            /* the cell; the sight stamp's +0x7A/+0x7C, books_put's */
    m_bytes(s_raw1, U_TYPEIX, 4, 0);        /* the type's index, the slot +0xA8                */
    m_bytes(s_raw1, 0xF8, 2, 0);            /* the sight stamp's +0xF8, books_put's; the attach piece */
    m_bytes(s_raw1, U_OWNERIX, 1, 0);
    m_bytes(s_raw1, U_GROUP, 4, 0);         /* the control group, group_put's              */
    m_bytes(s_raw1, 0x10A, 1, 0);           /* the water state, books_put's                    */
    m_bytes(s_raw1, 0x10E, 1, 0);           /* the state byte: the state events' in a skirmish,
                                               books_put's from the composed image on a
                                               multiplayer segment                             */
    m_field(s_raw1, U_YARD, 1, 0x04, 0);    /* the yard, YARD_OPEN's                           */
    m_field(s_raw1, U_STATE, 4, U_ENGINE_BITS, 0);
    m_bytes(s_raw1, REC_MOVER_AT + 0x26, 8, 0);
    memcpy(s_raw1h, s_raw1, sizeof s_raw1h);
    memcpy(s_stateh, s_state, sizeof s_stateh);
    m_field(s_raw1h, U_STATE, 4, U_MARKS, 0);
    m_field(s_stateh, U_STATE, 4, U_MARKS, 0);
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
    unsigned i;
    if (s_notes >= NOTES_MAX) return;
    for (i = 0; i < NOTE_SITES && s_nsite[i].fmt && s_nsite[i].fmt != fmt; i++) ;
    if (i < NOTE_SITES) {
        s_nsite[i].fmt = fmt;
        if (s_nsite[i].n >= NOTES_PER_SITE) return;
        if (++s_nsite[i].n == NOTES_PER_SITE)
            tagpu_log("replay: (the last line of this kind; the level's counts hold the rest)");
    }
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

/* Whether a build fraction is finished, by the engine's own test: the C3 of an x87 compare with
   0.0 (0x4586DF and its siblings), true for 0.0, -0.0 and a NaN alike. */
static int build_finished(float f)
{
    return !(f < 0.0f || f > 0.0f);
}

/* one unit's image, by the recipe above; the slot holds() it.

   THE BUILD FINISH ON A MULTIPLAYER LEVEL.

   What the finish writes. 0x41B8D0(builder, built) (DISASSEMBLED 0x41B8D0..0x41BA51) writes on
   the built unit its model's composite frame [+0x9E]+0x10 = 0, +0x104 = 0 and +0x110 bit 13.
   Behind owner type 1/2 it makes a GUI refresh or the detach 0x48AAC0; under def+0x241 bit 18 it
   calls the activation 0x48B090(1, 1); when the GUI tracks the builder it refreshes the builder's
   menu counts (0x4199B0); under def bit 24 it sets +0xF5 = 7 and +0x110 bit 14; for a type-1/2
   owner it sends the 0x12; when either unit is selected it sets the GUI word's refresh bit.

   Where each effect comes from here. On a multiplayer segment the owner's image carries every
   unit field of that on the owner's clock: the fraction, +0x110 bits 13 and 14 and +0xF5 by this
   recipe, the state byte by book_state. A puppet's owner is type 3, so the owner-only parts never
   run here. The primary's build done -- the recorder's copy finishing at its receipt of the
   engine's 0x12, on its own clock -- is not played (mp_owned_event).

   What the image cannot carry: the composite, the host's cached 1x bake of the unit. Its bytes
   depend on the fraction only through whether it is 0.0: an unfinished unit's bake has a depth
   plane (0x4586DF..0x4586F0) and every visible piece, a finished unit's holds only the cached
   pieces (0x459963..0x459985, 0x459DB8..0x459DCC). The draw bakes it again only when it is 0, or
   for a structure that is still a nanoframe (0x458870..0x4588F2). So a write here that changes
   whether the fraction is 0.0 clears it, as the finish clears its own (0x41B923) and the remote
   refresh a copy's (0x48B4A6), and the next draw bakes it for the unit the image describes. Kept,
   a finished unit would draw its nanoframe bake: on GDI its dont-cache pieces twice (0x459641
   redraws them over the bake's frozen copy), and in both renderers the waterline and digger
   clips a depth plane selects (0x45927E; tagpu_native.c).

   THE INVARIANT is an ordering. On such a segment this write is the fraction's one writer after a
   unit's create:
   - a puppet runs no order code, hence no build step (0x48AEB5..0x48AECD);
   - a puppet level is no network game, so neither the 0x12 receiver nor the round robin runs
     [INFERRED];
   - the primary's build done is not played;
   - the create clears the composite itself (0x485DF2, 0x485E1F).
   So every bake follows the last class written here. A seek, whose creates start every unit with
   no composite, holds the same as a forward playing at every draw.

   THE LIFETIME. +0x9E is the model the host's create made for this unit (0x485DCC, 0x485E0E).
   Only the destructor frees it, nulling the field in the same call (0x486D93..0x486DA3), on this
   thread. replay-layout.tsv's `model` row keeps the image off it, and every caller has bounded
   the slot (holds). A NULL model is skipped, as the destructor skips one (0x486D99).

   The finish's GUI refresh is made for the built unit when the viewer has it selected (+0x110
   bit 4, the viewer's own). The builder's half -- its refresh bit and its menu counts 0x4199B0 --
   has no unit here to name and is not made. */
static void unit_put(const Lv* L, unsigned slot, const uint32_t* img)
{
    char* u = unit_at(L, slot);
    char* mover = *(char**)u;           /* the create made it for a type with one (def +0x22F) */
    float was, now;
    memcpy(&was, u + U_BUILT, 4);
    put_masked(u, img, s_cells_held ? s_raw1h : s_raw1, 0, REC_UNIT_BYTES);
    memcpy(&now, u + U_BUILT, 4);
    if (s_mp.on && build_finished(was) != build_finished(now)) {
        char* model;
        uint32_t st;
        memcpy(&model, u + U_MODEL, 4);
        if (model) *(uint32_t*)(model + O3_FRAME) = 0;
        if (build_finished(now)) {
            memcpy(&st, u + U_STATE, 4);
            if (st & U_SELECTED) *(uint16_t*)(L->ta + OFF_GUIWORD) |= GUI_REFRESH;
            s_mp.bfinished++;
        } else {
            s_mp.bunfinished++;
        }
    }
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

/* `slot` is in the unit block of `owner`, a puppet seat: the create scales the owner's record
   unbounded (0x485F5C..0x485F78), and a slot outside its block would be another seat's */
static int owner_holds(const Lv* L, unsigned owner, unsigned slot)
{
    const char* rec;
    const char *first, *last, *u;
    if (owner >= REC_PLAYERS || !tagpu_puppet_seat(owner)) return 0;
    if (slot < 1 || slot >= L->nslots) return 0;
    rec = player_rec(L->ta, owner);
    memcpy(&first, rec + PL_BLOCKFIRST, 4);
    memcpy(&last, rec + PL_BLOCKLAST, 4);
    u = unit_at(L, slot);
    return first && last && u >= first && u <= last;
}

/* the create of a recorded unit in its recorded slot; 1 made. `open` lets the create's nested
   COB writes and replies through: a skirmish's exit-only create, whose recorded children have no
   unit when played (THE OTHER EVENTS). A multiplayer owner's create keeps them shut: its
   recording carries those children inside its bracket (THE MULTIPLAYER MATERIALIZER). */
static int make(const Lv* L, unsigned slot, unsigned owner, unsigned type, const int32_t pos[3],
                uint32_t finished, unsigned layer, int open)
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
    if (open) tagpu_puppet_open(1);
    u = E_Create(owner, type, pos[0], pos[1], pos[2], finished, layer, slot);
    if (open) tagpu_puppet_open(0);
    if (u != unit_at(L, slot)) {
        s_st.refused++;
        note("replay: the create of slot %u gave %s %u", slot, u ? "slot" : "nothing,",
             u ? (unsigned)((u - L->begin) / REC_UNIT_BYTES) : 0);
        return 0;
    }
    s_made[slot] = L->now + 1;
    return 1;
}

/* every unit of the world's image, made as the loader made it, at the first step and at a seek;
   how many were made. `books_at` is the capture point at which their books are the replay's to
   write (THE UNITS' BOOKKEEPING, s_made): the next one after a seek, which runs between two
   steps; this one at the first step, which runs at the capture point before its write. */
static unsigned level_units(const Lv* L, const RecWorld* W, uint32_t books_at)
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
        if (make(L, slot, (unsigned char)ib[U_OWNERIX], img_type(img), pos, build_finished(built),
                 img[U_STATE / 4] & 3u, 1)) {
            s_made[slot] = books_at;
            made++;
        }
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
            if (host_live(L, slot) && !(s_mp.on && s_mp.held[slot])) {
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
   26 and 27, which states_put then writes as recorded -- but on a segment that holds its cells,
   where the marks are the footprint writers' (U_MARKS). */
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
   and the yards are done: their contests mark bits 26 and 27 on incumbents too (0x47C828..0x47C89C).
   On a segment that holds its cells the marks are those contests' own (U_MARKS): the grid they
   mark is the engine's there, and a recorded mark beside it would name a contest the host's grid
   never held. */
static void states_put(const Lv* L, const RecWorld* W)
{
    const uint32_t* m = s_cells_held ? s_stateh : s_state;
    unsigned slot;
    for (slot = 1; slot < L->nslots; slot++) {
        const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
        if (W->live[slot] && holds(L, slot, img))
            put_masked(unit_at(L, slot), img, m, 0, REC_UNIT_BYTES);
    }
}

/* Every record the recording carries, 0-9 (V's, record 10, it does not): the seats, and the records
   neither the recording nor the host seats, which hold what the level start 0x465E30 gave all
   ten from the skirmish menu's entries (their energy and metal, 0x465EF5..0x465F1A), and which
   no loop over the players reads while inactive. An
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

/* the handler of the node image's action, which ord_bounded has bounded */
static uint32_t ord_handler(const unsigned char* img)
{
    const char* desc = *(const char* const*)(size_t)DESC_BEGIN +
                       (size_t)img[ORD_ACTION] * DESC_STRIDE;
    uint32_t h;
    memcpy(&h, desc + DESC_HANDLER, 4);
    return h;
}

/* THE TAKEOVER'S BOUND ON P1 (orders_put's image scan, while s_tk.on): a node whose handler
   indexes a table with p1 the moment it runs as a local unit's. The three build handlers index
   [main+0x1439B] + p1 * 0x249 with the whole dword (0x402827, 0x403A8D, 0x413EB7), so p1 is below
   UNITINFOCount; ord_values' u16 bound stays for the build-site draw 0x438C07. BuildWeapon reads
   its slot's weapon at entry, dereferences it in state 1 (0x402BE0) and writes [unit + p1 * 0x1C
   + 0x1E] in state 2 (0x402BB9), so p1 is one of the three slots stock's addressing holds, and the
   unit's weapon there -- units_put's, through weapon_ptr -- is set. A node that fails ends its
   list there. */
static int tk_node_bounded(const Lv* L, unsigned slot, const unsigned char* img)
{
    uint32_t h = ord_handler(img), p1;
    memcpy(&p1, img + ORD_P1, 4);
    if (h == H_BUILDINGBUILD || h == H_MOBILEBUILD || h == H_VTOLMOBILEBUILD) return p1 < L->ndefs;
    if (h == H_BUILDWEAPON)
        return p1 < 3 && *(char* const*)(unit_at(L, slot) + U_SLOTWEAPON + p1 * 0x1C) != NULL;
    return 1;
}

/* THE TAKEOVER'S NODES (R8, last in ord_patch and ord_build, once the target is final). Every node
   restarts in the constructor's handler state (0x43A13C..0x43A19C): +0x05, +0x06, +0x4E and +0x32
   0, +0x0A -1, and +0x42's bits 21 and 22 clear and bit 16 the image's, which undoes the puppet
   hold ord_values applied; +0x52 is already NULL, the puppet row's, and ord_patch names a node that
   owns an object. That is the state every order is issued in and every handler starts from: the
   engine makes it 54 times (s2-orders App. B).
   WHY IT IS SAFE. No value taken from the recording is a pointer: the target goes through the
   engine's setter, bounded by the host's live slot (ord_target); +0x52 is NULL; every index a
   handler reads from p1 is bounded above, and a weapon slot it names holds a weapon
   (tk_node_bounded, and Attack_Chase's below). +0x06 bit 1 is clear, so the destructor's
   unbounded handler call (0x43A1F5..0x43A21A) cannot run a handler before the handler arms it
   itself. Nine handler functions arm it: SelfDestruct, BuildingBuild, MobileBuild, HelpBuild,
   AirStrike, AirToGround, VTOL_Pickup, VTOL_MobileBuild and VTOL_HelpBuild
   (exe-reverse-engineering.md, the order node).
   THE ADOPTION: a builder whose node holds its nanoframe goes on building it instead of starting
   over, at state 3, armed as each handler arms itself less bit 1: +0x06 = 9, the timer and the
   target's death, with the deadline at T, so the first call is the step at T. A frame that dies
   before that call fires the node's event 8 through vtable slot 0 (0x489762) before it clears the
   target (0x48978E), and both handlers test event 8 before their states (BuildingBuild 0x402726,
   MobileBuild 0x403A27), so state 3 never runs with a NULL target. That matters to both:
   MobileBuild's state 3 has no NULL test (0x403E43, 0x403EEB), and BuildingBuild's returns 7 on
   one (0x4029CA..0x4029CF, 0x402B4D), which the dispatcher's case 7 answers by destroying the
   factory's whole main list (0x43BA3D..0x43BA6D). Each arms itself with the timer 0x439E80,
   which sets bit 0, and then 0xA (BuildingBuild 0x402ADC..0x402AE6, MobileBuild
   0x403F00..0x403F0C): 0xB, of which the adoption's 9 is all but bit 1.
   THE RESUMED HANDLERS: Paralyze and BuildWeapon keep their progress. Neither arms bit 1, so the
   destructor never calls them, and their state is values -- a count, progress and a deadline
   compared with GameTime only (0x43B7E2..0x43B7FB) -- each deadline clamped to the handler's own
   longest: Paralyze clamps its p1 to 0x708 (0x402D33..0x402D3B), BuildWeapon's longest timer is
   300 (0x402CD0). BuildWeapon's states are 0 to 2; its p3 is the image's (ord_values). */
static void tk_node(const Lv* L, char* n, const unsigned char* img, unsigned slot)
{
    uint32_t h, flags, iflags, v, ev, dl, idl;
    char* target;
    if (!s_tk.on) return;
    h = ord_handler(img);
    n[ORD_STATE] = 0;
    memset(n + ORD_EVMASK, 0, 4);
    dl = 0xFFFFFFFFu;
    memcpy(n + ORD_DEADLINE, &dl, 4);
    memset(n + ORD_EVENTS, 0, 4);
    memset(n + ORD_SEENXZ, 0, 4);
    memcpy(&flags, n + ORD_FLAGS, 4);
    memcpy(&iflags, img + ORD_FLAGS, 4);
    flags = (flags & ~(OF_SEEN | OF_STOPBUILDING | OF_KEEPTARGETS)) | (iflags & OF_KEEPTARGETS);
    memcpy(n + ORD_FLAGS, &flags, 4);
    memcpy(&target, n + ORD_TARGET, 4);
    memcpy(&ev, img + ORD_EVMASK, 4);
    memcpy(&idl, img + ORD_DEADLINE, 4);
    if ((h == H_BUILDINGBUILD || h == H_MOBILEBUILD) && target) {
        n[ORD_STATE] = 3;
        v = 9;
        memcpy(n + ORD_EVMASK, &v, 4);
        dl = L->now + 1;
        memcpy(n + ORD_DEADLINE, &dl, 4);
        s_tk.adopted++;
    } else if (h == H_ATTACKCHASE) {
        /* p1 0 is the handler's "unset": state 0 picks a slot then (0x4035A1). The slot's weapon
           is read without a NULL test from p1's low byte (0x49ABCB in 0x49ABB0, 0x49AE04 in
           0x49ADF0), so every other p1 names a slot that holds one: a stock slot 1 or 2 left
           empty, a p1 negative as an int -- which would reach the stock slot write through
           my_SetTarget's signed idx < 3 (tagpu_weapons.c) -- and one at or past the unit's
           count all have none. */
        memcpy(&v, n + ORD_P1, 4);
        if (v && !tagpu_weapons_slot_weapon(unit_at(L, slot), v)) {
            v = 0;
            memcpy(n + ORD_P1, &v, 4);
            s_tk.bounded++;
        }
    } else if (h == H_PARALYZE) {
        v = ev & 1;
        memcpy(n + ORD_EVMASK, &v, 4);
        if (v) {
            dl = idl < L->now + 0x708 ? idl : L->now + 0x708;
            memcpy(n + ORD_DEADLINE, &dl, 4);
        }
        s_tk.resumed++;
    } else if (h == H_BUILDWEAPON) {
        n[ORD_STATE] = img[ORD_STATE] <= 2 ? (char)img[ORD_STATE] : 0;
        v = ev & 1;
        memcpy(n + ORD_EVMASK, &v, 4);
        dl = idl < L->now + 300 ? idl : L->now + 300;
        memcpy(n + ORD_DEADLINE, &dl, 4);
        s_tk.resumed++;
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
    tk_node(L, n, img, slot);
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
    tk_node(L, n, img, slot);
    s_st.obuilt++;
    return 1;
}

static void ord_teardown(char* n)
{
    E_OrdFree(n);
    E_MemFree(n);
    s_st.ofreed++;
}

/* THE TAKEOVER'S OTHER UNITS (R8, after orders_put): a host unit the kills left alive and that
   the image does not describe (not live in it, or of another type) keeps the order nodes its
   puppet steps left, which tk_node never sees, and from T its owner's type makes the unit tick
   run their handlers. Its lists are torn down instead, as orders_put tears down a puppet's extra
   nodes and for the same reason: no handler has run on a puppet's node, since the unit tick calls
   them only for an owner of type 1 or 2 (0x48AEB5..0x48AECD), so none has armed the destructor's
   handler call (+0x06 bit 1, which only the handlers set) or its StopBuilding (+0x42 bit 22,
   0x438590's). A block that is no order ends the walk, as in orders_put, and the list is cut
   there. The units are counted on the takeover's line.
   Their three stock weapon slots' targets are set to none, (0, 0x8000), the words AutoAim writes
   when it drops a target (0x48A2AF, 0x48A2B4, DISASSEMBLED): such a unit's image was last
   written from the stream before T, where a target is a held value (tagpu_reclossy.c) that the
   refresh at T corrects only for the units the image describes, and from T the owner code that
   resolves a target runs on it (tk_bound, THE REFRESH). The side slots past the third are no
   part of the unit image, so the stream holds none of them. */
static unsigned tk_orphans(const Lv* L, const RecWorld* W)
{
    unsigned slot, list, i, n = 0;
    for (slot = 1; slot < L->nslots; slot++) {
        if (!host_live(L, slot) ||
            (W->live[slot] && holds(L, slot, W->unit + (size_t)slot * REC_UNIT_DWORDS)))
            continue;
        for (i = 0; i < 3; i++) {
            uint16_t* target = (uint16_t*)(unit_at(L, slot) + U_SLOTTARGET + i * 0x1C);
            target[0] = 0;
            target[1] = 0x8000;
        }
        for (list = 0; list < 2; list++) {
            char** head = (char**)(unit_at(L, slot) + (list ? U_SUBORDERS : U_ORDERS));
            char* o = *head;
            if (o) n++;
            while (o && *(const uint32_t*)o == ORD_VTABLE) {
                char* next = *(char**)(o + ORD_NEXT);
                *head = next;
                ord_teardown(o);
                o = next;
            }
            *head = NULL;
        }
    }
    return n;
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
                const unsigned char* ik = rl->img + (size_t)k * REC_ORDER_IMAGE;
                if (!ord_bounded(ik, ndesc)) {
                    s_st.orefused++;
                    note("replay: GameTime %u: unit %u's %s list holds action %u at %u, past the "
                         "%u orders", L->now, slot, list ? "sub" : "main", ik[ORD_ACTION], k,
                         ndesc);
                    want = k;
                    break;
                }
                if (s_tk.on && !tk_node_bounded(L, slot, ik)) {
                    s_tk.bounded++;
                    note("replay: GameTime %u: unit %u's %s list: action %u at %u fails the "
                         "takeover's bound on p1", L->now, slot, list ? "sub" : "main",
                         ik[ORD_ACTION], k);
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
static void slice_created(const Lv* L, unsigned slot);
static void slice_died(unsigned slot);
static int slice_own(unsigned slot);

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
    if (!make(L, id - 1, b[6], type, pos, finished, b[7] & 3u, 1)) return 0;
    water = b[24];
    if (water <= WATER_MAX) {
        memcpy(unit_at(L, id - 1) + U_WATER, &water, 4);
    } else {
        s_st.bad_ids++;
        note("replay: GameTime %u: unit %u's create carries water state %u, past the 0..4 "
             "0x43DB50 stores: the host's stays", L->now, id - 1, water);
    }
    slice_created(L, id - 1);
    return 1;
}

/* RK_EV_DEATH: the victim's fields as the destructor found them (s2-events §1.9), then the
   destructor through C4's wrap, as a remote's death is applied: mode 0, the killer's player by
   the host's netid for it (every seat's is its index, 0x464290), and C4's carried decision armed
   from the recording; its Killed as the recording machine ran it (tagpu_patches.c, THE KILLED
   SCRIPT OF A REPLAYED DEATH). `corpse` 0 clears the record's corpse level, +0xA's low nibble: the
   destructor reads it only at 0x486D55..0x486D69, to call the corpse helper 0x486360 at 0x486D6F
   (its only caller); its other reads of +0xA take the high nibble (0x4867DA, 0x486880, 0x486CA2,
   0x486D42), and the record reaches no callee (DISASSEMBLED 0x4866D0..0x486E60). So 0 removes
   the corpse and its flare and nothing else. */
static int ev_death(const Lv* L, const unsigned char* b, unsigned len, int corpse)
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
    if (kp < REC_PLAYERS && tagpu_puppet_seat(kp))
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
    rec[10] = corpse ? b[10] : (unsigned char)(b[10] & 0xF0);
    /* every death of a skirmish recording, and of its own blocks on a multiplayer one, was the
       recording machine's own (THE KILLED SCRIPT OF A REPLAYED DEATH) */
    if (tagpu_tx_replay_death(rec, b[11], slice_own(slot))) {
        slice_died(slot);
        return 1;
    }
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
   lives itself (0x41B8D6..0x41B917); of what it writes, the model's composite frame [+0x9E]+0x10
   is the one the images do not carry (THE BUILD FINISH ON A MULTIPLAYER LEVEL); a multiplayer
   segment does not play it (mp_owned_event) */
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

/* THE SCRIPT SLICES. The unit tick gives each unit it reaches one slice, DoScriptsNow 0x4B0D60 with
   dt 1 (tagpu_patches.c, THE SCRIPT SLICE), and a COB clock is the number of slices it has had. The
   recording machine's slice of a unit came after every event its unit tick made before that unit's
   turn: AutoAim's TargetCleared, AimPrimary broadcast, FirePrimary and RockUnit (0x48A1E0,
   0x49E31C, 0x49E3A6, 0x49CB94, 0x49CBEB, in the unit's own turn, before its script call), and the
   create of a unit an earlier turn made in a later slot, which the loop reaches in the same tick
   (it re-reads +0xA6 at each slot, 0x48ADB0). A host that gives every slice in its own unit tick
   gives those starts' first slice a tick late, and a unit its step creates none in that step. So
   the recording names the slices that follow such a start (RK_EV_SLICE: the unit's COB had an
   engine start since the capture point; tagpu_recorder.c, THE SLICE MARKERS), and a playing gives
   each of them at its marker instead:
   - THE PLAN, at the capture point of the iteration the host's step is (slice_plan): each slot a
     marker names is planned, SL_SKIP for a unit the iteration's images hold, SL_AFTER_CREATE for one
     its events create before the marker. A late iteration plans nothing, and every host slice
     stays in the host's tick.
   - THE GATE, in the host's unit tick (tagpu_replay_slice_defer): an SL_SKIP unit's slice is
     skipped and owed (SL_OWED).
   - THE PAYMENT, at the marker in the replayed events (ev_slice), through the same function the
     unit tick's stub calls (tagpu_slice_give): an owed slice, or a created unit's once its create
     has been made (SL_MADE). On a multiplayer segment an own unit the step creates is made at its
     owner's CREATE_BEGIN, after the primary events, so its marker waits (SL_WAITING) and its
     CREATE_END pays it (slice_created).
   - THE SETTLE, after the step's events (slice_settle): an owed slice its marker did not pay is
     given there, counted and noted; and the step's end disarms the plan (slice_finish).
   THE INVARIANT, an ordering: the plan is armed and spent inside one call of B2's stub at 0x4954ED
   -- the capture point, the unit tick, the replay step -- where no frame visit, and so no seek,
   falls. The gate turns only SL_SKIP into SL_OWED, and every SL_OWED is paid exactly once, at its
   marker or at the settle, or voided by a create or a death in its slot (slice_created,
   slice_died), so every unit the host's unit tick reaches gets exactly one DoScriptsNow(1) in the
   step -- in the tick, at its recorded place, or at the step's end -- and a unit the step creates
   gets one only when the recording says it had one. A voided debt is counted and never given to
   another unit: the recording never orders a create or a death into a slot between its turn and
   its marker, since B4 never puts two units in one slot in one tick (0x486036).
   THE BOUND: the gate names a unit only by arithmetic against the level's array and its count, as
   model_root does, and never reads it; every index below is a slot below nslots, at most 0xFFFF,
   which the writer bounds (tagpu_recwriter.c) and the reader takes from the header.
   THE LIFETIME: a COB object lives from its unit's setup (0x485D9F stores +0x9A) to its destructor
   (0x486D8A deletes it, 0x486D8D nulls +0x9A), both on the game thread. A payment runs on that
   thread, on the +0x9A of a unit live in its slot, whose incarnation is the one the host skipped
   because a create or a death in the slot voids the debt.
   The puppet gates stay shut around a payment, as around the host's own slices: COB SET, ATTACH
   and DROP are inert, and the recorder stays the only writer of what the recording carries.
   MULTIPLAYER: the primary stream carries every marker; only the recording machine's own blocks
   are planned (s_mp.own), and a remote owner's unit keeps its slice in the host's tick. An own
   unit's ledger-borne starts -- TargetCleared and the other owner-only named starts, applied after
   the primary events (THE CLOCK) -- keep their one-tick lag. */
enum { SL_SKIP = 1, SL_OWED, SL_CREATED, SL_AFTER_CREATE, SL_MADE, SL_WAITING, SL_PAID, SL_LOST };
static struct {
    int armed;                      /* between this step's capture point and its end            */
    uint32_t stamp;                 /* this plan's: never 0, never reused (wraps by clearing at) */
    char* begin;                    /* the level's unit array and its count, as level_of found  */
    unsigned nslots;                /* them                                                     */
    uint32_t at[0x10000];           /* a slot's entry belongs to the plan of this stamp         */
    unsigned char st[0x10000];
    uint16_t list[0x10000];         /* the slots the plan's markers name, each once             */
    unsigned n;
    struct { unsigned planned, made, deferred, given, atcreate, settled, notgiven, unreached,
                      unmade, twice, lost, late, remote; } c;
} s_sl;

static void slice_disarm(void)
{
    s_sl.armed = 0;
}

/* the next playing's: disarmed, its counts zero */
static void slice_reset(void)
{
    slice_disarm();
    memset(&s_sl.c, 0, sizeof s_sl.c);
}

/* a slot of the recording machine's own blocks; every slot outside a multiplayer segment */
static int slice_own(unsigned slot)
{
    unsigned i;
    if (!s_mp.on) return 1;
    for (i = 0; i < s_mp.nown; i++)
        if (slot >= s_mp.own[i].first && slot <= s_mp.own[i].last) return 1;
    return 0;
}

/* the unit live in a planned slot, by the plan's array, or NULL */
static char* slice_unit(unsigned slot)
{
    char* u;
    if (slot < 1 || slot >= s_sl.nslots) return NULL;
    u = s_sl.begin + (size_t)slot * REC_UNIT_BYTES;
    return (*(const uint32_t*)(u + U_STATE) & U_ALIVE) ? u : NULL;
}

/* THE PLAN of the iteration W, which is the host's step L->now: at its capture point */
static void slice_plan(const Lv* L, const RecWorld* W)
{
    unsigned i;
    slice_disarm();
    if (++s_sl.stamp == 0) {
        memset(s_sl.at, 0, sizeof s_sl.at);
        s_sl.stamp = 1;
    }
    s_sl.n = 0;
    s_sl.begin = L->begin;
    s_sl.nslots = L->nslots;
    for (i = 0; i < W->nev; i++) {
        uint32_t id, slot;
        unsigned k = W->ev[i].kind;
        if ((k != RK_EV_CREATE && k != RK_EV_SLICE) || W->ev[i].len < 4) continue;
        memcpy(&id, W->ev[i].b, 4);
        slot = id - 1;
        if (slot < 1 || slot >= s_sl.nslots) continue;
        if (k == RK_EV_CREATE) {
            if (s_sl.at[slot] != s_sl.stamp) {
                s_sl.at[slot] = s_sl.stamp;
                s_sl.st[slot] = SL_CREATED;
            }
            continue;
        }
        if (!slice_own(slot)) {
            s_sl.c.remote++;
            continue;
        }
        if (s_sl.at[slot] == s_sl.stamp) {
            if (s_sl.st[slot] == SL_CREATED) {
                s_sl.st[slot] = SL_AFTER_CREATE;
                s_sl.list[s_sl.n++] = (uint16_t)slot;
            } else {
                s_sl.c.twice++;
            }
            continue;
        }
        s_sl.at[slot] = s_sl.stamp;
        s_sl.st[slot] = SL_SKIP;
        s_sl.list[s_sl.n++] = (uint16_t)slot;
    }
    s_sl.c.planned += s_sl.n;
    s_sl.armed = 1;
}

/* THE GATE, in the host's unit tick: the unit pointer is the engine's loop variable, an index
   only by arithmetic against the plan's array and count, never dereferenced here */
int __cdecl tagpu_replay_slice_defer(const char* unit)
{
    size_t off;
    unsigned slot;
    if (!s_sl.armed || unit < s_sl.begin) return 0;
    off = (size_t)(unit - s_sl.begin);
    if (off % REC_UNIT_BYTES || (slot = (unsigned)(off / REC_UNIT_BYTES)) >= s_sl.nslots)
        return 0;
    if (s_sl.at[slot] != s_sl.stamp || s_sl.st[slot] != SL_SKIP) return 0;
    s_sl.st[slot] = SL_OWED;
    s_sl.c.deferred++;
    return 1;
}

/* a create in the step made its unit in `slot` (ev_create, mp_created) */
static void slice_created(const Lv* L, unsigned slot)
{
    char* u;
    void* cob;
    (void)L;
    if (!s_sl.armed || slot >= s_sl.nslots || s_sl.at[slot] != s_sl.stamp) return;
    switch (s_sl.st[slot]) {
    case SL_AFTER_CREATE:
        s_sl.st[slot] = SL_MADE;
        s_sl.c.made++;
        break;
    case SL_WAITING:                    /* a multiplayer create: its marker has passed */
        if ((u = slice_unit(slot)) && (cob = cob_of(u))) {
            tagpu_slice_give(cob, 1);
            s_sl.st[slot] = SL_PAID;
            s_sl.c.atcreate++;
        } else {
            s_sl.st[slot] = SL_LOST;
            s_sl.c.lost++;
        }
        break;
    case SL_OWED:                       /* another incarnation: the debt is void */
        s_sl.st[slot] = SL_LOST;
        s_sl.c.lost++;
        break;
    }
}

/* a death in the step ended the unit in `slot` (ev_death) */
static void slice_died(unsigned slot)
{
    if (!s_sl.armed || slot >= s_sl.nslots || s_sl.at[slot] != s_sl.stamp) return;
    if (s_sl.st[slot] == SL_OWED) {
        s_sl.st[slot] = SL_LOST;
        s_sl.c.lost++;
    }
}

/* THE PAYMENT, at an RK_EV_SLICE of the step's events */
static void ev_slice(const Lv* L, const unsigned char* b, unsigned len)
{
    uint32_t id, slot;
    char* u;
    void* cob;
    (void)L;
    if (len != 4) return;
    memcpy(&id, b, 4);
    slot = id - 1;
    if (!s_sl.armed || slot < 1 || slot >= s_sl.nslots || s_sl.at[slot] != s_sl.stamp ||
        !slice_own(slot)) {
        s_sl.c.notgiven++;              /* a late iteration's, or a remote unit's */
        return;
    }
    switch (s_sl.st[slot]) {
    case SL_OWED:
    case SL_MADE:
        if ((u = slice_unit(slot)) && (cob = cob_of(u))) {
            tagpu_slice_give(cob, 1);
            s_sl.st[slot] = SL_PAID;
            s_sl.c.given++;
        } else {
            s_sl.st[slot] = SL_LOST;
            s_sl.c.lost++;
        }
        return;
    case SL_AFTER_CREATE:               /* its create is not made yet */
        if (s_mp.on) {
            s_sl.st[slot] = SL_WAITING; /* paid at its CREATE_END */
            return;
        }
        s_sl.st[slot] = SL_LOST;        /* the create was refused */
        s_sl.c.unmade++;
        return;
    case SL_SKIP:                       /* the host's tick never reached that unit */
        s_sl.c.unreached++;
        return;
    default:
        s_sl.c.twice++;
        return;
    }
}

/* THE OWNER'S QUERIES. The recording machine's order handlers made two COB queries a playing's
   puppets never make, since a puppet runs no order handler: QueryBuildInfo and QueryNanoPiece
   (tagpu_recorder.c, THE OWNER'S QUERIES). A query runs one thread of the unit's COB to its end at
   once and can change its statics and pieces, so the recording carries each at its place in the
   step (RK_EV_QUERY) and the playing makes it there, through the same 0x4B0BC0 with the
   arguments the engine's call passes: the first a pointer to the value BuildingBuild (-1,
   0x4027F3) or 0x43E400 (0, 0x43E41F) puts there first, the others NULL. 0x43E400's own work after
   its query -- the piece's point, through 0x43DEF0 -- is not made: it writes only the caller's
   buffer, and the spray that takes the point is recorded (RK_EV_SPRAY). The puppet gates are as
   around the host's own slices: COB SET, ATTACH and DROP are inert.
   THE BOUND and THE LIFETIME are the slices': the unit is named by ID, a slot below the level's
   count holding a live unit (live_unit), and its +0x9A COB object, which only that unit's
   destructor frees (0x486D8A), is used on the game thread inside the step. A unit with no COB
   object (0x485DFF) made no query, since 0x4B0BC0 reads its object at once (0x4B0BC1); such an
   event is not made, and counted. */
static struct { unsigned made, unmade; } s_q;

static void query_reset(void)
{
    memset(&s_q, 0, sizeof s_q);
}

static void ev_query(const Lv* L, const unsigned char* b, unsigned len)
{
    static const char* const NAME[REC_QUERIES] = { "QueryBuildInfo", "QueryNanoPiece" };
    static const int FIRST[REC_QUERIES] = { -1, 0 };
    uint32_t id;
    char* u;
    void* cob;
    int a0;
    if (len != 8 || b[4] >= REC_QUERIES) return;
    memcpy(&id, b, 4);
    if (!(u = live_unit(L, id)) || !(cob = cob_of(u))) {
        s_q.unmade++;
        return;
    }
    a0 = FIRST[b[4]];
    E_Query(cob, NAME[b[4]], &a0, NULL, NULL, NULL);
    s_q.made++;
}

/* THE SETTLE, after the step's events and before image T+1: a debt its marker did not pay */
static void slice_settle(unsigned now)
{
    unsigned i;
    for (i = 0; i < s_sl.n; i++) {
        unsigned slot = s_sl.list[i];
        char* u;
        void* cob;
        switch (s_sl.st[slot]) {
        case SL_OWED:
        case SL_MADE:
            if ((u = slice_unit(slot)) && (cob = cob_of(u))) {
                tagpu_slice_give(cob, 2);
                s_sl.c.settled++;
                note("replay: GameTime %u: unit %u's script slice given at the step's end: its "
                     "marker did not give it", now, slot);
                s_sl.st[slot] = SL_PAID;
            } else {
                s_sl.st[slot] = SL_LOST;
                s_sl.c.lost++;
            }
            break;
        case SL_WAITING:
        case SL_AFTER_CREATE:           /* the unit was never made */
            s_sl.st[slot] = SL_LOST;
            s_sl.c.unmade++;
            break;
        }
    }
}

/* the step's end, on every path out of tagpu_replay_step */
static void slice_finish(unsigned now)
{
    if (s_sl.armed) slice_settle(now);
    slice_disarm();
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
   slots again. Its +1 is the start the live build used, which the receiver builds from
   (0x49D547..0x49D558), so the puppet's projectile starts where the live one did, also for
   0x49D9C0's shots, whose sent packet carries the shooter's centre (tagpu_patches.c,
   wpn_tx_fired). The receiver's own bounds apply (wpn_rx_fired at 0x49D280: the weapon, the units
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
     it runs, since its reproduction reads the source cell's unit slot (0x424189) -- on a
     multiplayer recording's level, whose slots are the host's, that read takes the recording's
     value instead (FS_SOURCE, tagpu_replay_source).
   - A multiplayer recording's DEATH: THE DEATHS ON A MULTIPLAYER LEVEL. A skirmish recording's
     DEATH is replayed as ev_death replays it, with the feature gates open: the host's destructor
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
   THE UNIT SLOTS AND THE YARD BIT are the host engine's own (s_engine_cells) on a multiplayer
   recording's segment, whose units follow their owners, not the recorder's replicas, and on a
   skirmish segment that holds its cells (s_cells_held, RECL_CELLS), whose stream carries them
   exact only at a refresh (tagpu_reclossy.c): the stamp, the clear and the re-claim write them
   for the placements, creates, deaths and yard changes the images make, at the recorded
   positions, and decide a contest by the recording machine's rule (tagpu_puppet.c, THE
   FOOTPRINT WRITERS, THE CONTEST'S RULE). A takeover of such a segment, at a refresh, writes the
   recording's slots, yard bits and marks once and refreshes the passability over them, so the
   engine plays on from the recording machine's grid (sk_features, tk_cells). On a skirmish
   recording's segment that does not hold them they come from the images alone: every write the
   three make is undone at the writer's return, so the cells the recording did not change keep
   the recording's values. */

#define OFF_FGRID      0x14287      /* the feature grid (tagpu_recorder.c, *The features*)          */
#define OFF_FGRIDW     0x14233
#define OFF_FGRIDH     0x14237
#define OFF_NFDEFS     0x14253
#define OFF_FDEFS      0x1426F
#define OFF_WRECKS     0x1420B      /* the wreck pool, TAGPU_LIM_WRECKS records                    */
#define OFF_WR_HEADS   0x14213      /* the active, settled and free lists' heads, i32 each         */
#define WR_NEXT        0x00         /* a wreck record's list links, i16: the next, the previous   */
#define WR_PREV        0x02         /*   (the list mover 0x4232F0)                                */
#define OFF_FCURSOR    0x14257      /* the reproduction cursor                                    */
#define FD_STRIDE      0x100
#define FD_MASK        0xFE         /* bit 0: a GAF feature                                       */
#define CELL_DEF       0x08
#define CELL_BACK      0x0A         /* a footprint cell's rows and columns back to its anchor, an
                                       anchor's record, or a resting GAF anchor's damage         */
#define CELL_FLAGS     0x0C         /* bit 0: the anchor holds a record                           */
#define CELL_YARDBIT   0x02         /* +0x0C bit 1, the yard's (tagpu_puppet.c, THE FOOTPRINT WRITERS) */
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
    if (s_engine_cells) {
        /* THE UNIT SLOTS AND THE YARD BIT: the engine's own */
        for (k = 0; k < REC_CELL_BYTES; k++) {
            unsigned char m = k < 4 ? 0 : s_cval[k];
            if (k == CELL_FLAGS) m &= (unsigned char)~CELL_YARDBIT;
            if (back && (k == CELL_BACK || k == CELL_BACK + 1)) m = 0;
            c[k] = (char)(((unsigned char)c[k] & ~m) | (img[k] & m));
        }
        return 1;
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
   images, the footprint writers' writes are undone at their return (tagpu_puppet.c) or, where the
   cells are the engine's, made at the recorded positions by the recording machine's rule, the
   feature calls are the recording machine's own with its inputs, and this write-back undoes the
   input.
   It takes the NEXT iteration's world: there the grid names only units live in its image, which
   the step's creates and deaths have made the host's (the delta's own precondition), so
   cell_put's slot rule refuses nothing it would not refuse in the delta. The step's own world
   would name a unit that died in the step. Nothing reads the grid between the tick and the
   write-back: the tick is a root of the step's late phase, and the late phase's roots are made
   at tagpu_replay_phase immediately before features_put (THE PHASES), or, for an iteration played
   late, by events_put and images_put back to back. When the playing ends at the reader,
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

static int addr_of(const Lv* L, unsigned simid, uint32_t id, char** p);

typedef void (__stdcall* PFN_FCorpse)(char* unit, unsigned level, unsigned smoke);
#define E_FCorpse  ((PFN_FCorpse)0x00486360u)   /* `ret 0xC` (0x486454)                      */
#define UD_CORPSE  0x1BC     /* u16 the unit def's corpse feature (0x48636E)                       */
#define FD_DEAD    0xF4      /* u16 a feature def's next corpse level (0x48639F)                    */
#define FDEF_NONE  0xFFFB    /* a def at or above it is no feature (0x486384)                       */

/* 0x486360's walk to the corpse def (0x48636E..0x4863B1): the unit def's +0x1BC, then level - 1
   times FeatureDef +0xF4 while below 0xFFFB. The helper bounds nothing but 0xFFFB, and the spawn
   indexes FeatureDef by what the walk ends on (0x423CD2). So each index it reads through, and a
   last one below 0xFFFB, must name a feature def. 1 when they do. */
static int corpse_def_ok(const Lv* L, const char* udef, unsigned level)
{
    uint16_t f;
    const char* d;
    memcpy(&f, udef + UD_CORPSE, 2);
    for (; level > 1 && f < FDEF_NONE; level--) {
        if (!(d = fdef_of(L, f))) return 0;
        memcpy(&f, d + FD_DEAD, 2);
    }
    return f >= FDEF_NONE || fdef_of(L, f) != NULL;
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
    case FR_CORPSE: {
        /* The corpse helper on a stack record holding exactly what it reads of its unit: +0x92
           (0x486368, 0x486404), +0x76 and +0x78 (0x4863B7, 0x4863BB), the position +0x6A and the
           turn +0x64 by address (0x4863D0, 0x4863F1, 0x48642C), and +0xFF (0x4863EB, 0x486426).
           It writes none of them, and every callee copies what it takes: the spawn the position
           and the turn (0x423E28..0x423E38, 0x423E9B..0x423EA6), the smoke's and the flare's
           inits 12 bytes of the point (0x47515D..0x475178, 0x474D5D..0x474D7C). So nothing keeps
           a pointer into the record past the call (DISASSEMBLED). The effect is the helper's
           own: the spawn, the underwater velocity (0x486416) and the flare, as on the recording
           machine. Bounds: the type by the unit types' count (addr_of); the level 1..15 and the
           smoke 0..1, as 0x486D58..0x486D69 pass them; the corpse defs (corpse_def_ok); the cell
           by 0x481550 itself (0x481551..0x481593), the height by 0x485070 itself
           (0x48508F..0x4850C4); the owner byte reaches only the anchor's flag nibble
           (0x423EFB). */
        char u[REC_UNIT_BYTES];
        char* udef;
        uint32_t type;
        int16_t fx, fz;
        memcpy(&type, a, 4);
        memcpy(&fx, a + 8, 2);
        memcpy(&fz, a + 10, 2);
        if (a[4] < 1 || a[4] > 15 || a[5] > 1 || a[7] || a[30] || a[31] ||
            !addr_of(L, SIM_ID_UNIT_TYPE, type, &udef) || !udef || !corpse_def_ok(L, udef, a[4]))
            return 0;
        memset(u, 0, sizeof u);
        memcpy(u + U_DEF, &udef, 4);
        memcpy(u + U_TURN, a + 24, 6);
        memcpy(u + U_POS, a + 12, 12);
        memcpy(u + U_FOOTXZ, a + 8, 4);
        u[U_OWNERIX] = (char)a[6];
        E_FCorpse(u, a[4], a[5]);
        cell = cell_at(L, fx, fz);
        *got = cell && (cell[CELL_FLAGS] & 1) ? (uint32_t)*(const uint16_t*)(cell + CELL_BACK) + 1
                                              : REC_ID_NULL;
        return 1;
    }
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
    if (root == FR_CORPSE) s_fst.corpses++;
    s_fst.left += left;
    /* a corpse whose spawn took no record on the recording machine is checked by the next
       images' structure compare */
    if ((root == FR_SPAWN || root == FR_SWAP || root == FR_DESTROY ||
         (root == FR_CORPSE && want != REC_ID_NULL)) && got != want && !s_fst.rdiffer++)
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
    done = ev_death(L, W->ev[i].b, W->ev[i].len, 1);
    tagpu_puppet_features(was);
    s_dying.W = NULL;
    if (roots && !s_dying.blasted && !s_fst.dmissed++)
        note("replay: GameTime %u: the death of slot %u made no explosion here, and %u feature "
             "calls its explosion made are not made", L->now, s_dying.id - 1, roots);
    s_fst.left += stray;
    s_fst.deaths++;
    return done;
}

int tagpu_replay_source(unsigned n, unsigned* v)
{
    return s_engine_cells && tagpu_puppet_on() && tagpu_puppet_draw(FS_SOURCE, n, v);
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

/* THE PHASES. The recording machine made each feature root at some point of its step 0x495490
   (REC_PHASE_*, recorded with the root): in the unit tick, in the projectile tick 0x49B720 -- an
   impact's damage burning or felling a tree -- or from the explosion tick 0x420F30 on, the
   feature tick 0x424050 among them. The host's projectile tick tests each projectile against the
   cell it is in, the feature there and its height (0x49B090 -> 0x49B2DC..0x49B3CD), so a root
   made before it takes away a tree the recording machine's projectiles still met: MEASURED in
   forest-circle 2026-10-01, every impact the host missed (23 of 134) had the tree in the
   recording machine's cell and its successor in the host's. So a root goes in at its own phase:
   the unit tick's at the replay step, in recorded order with the step's other events; the
   projectile tick's once the host's projectile tick has returned; the rest once its explosion
   tick has returned, and the step's feature images after them, since they are the world
   after every root of the step (THE TICK'S INPUT). Roots inside a skirmish recording's death go
   with the death, at the replay step; inside a multiplayer recording's, each at its own phase,
   like any other (THE DEATHS ON A MULTIPLAYER LEVEL). The deferred brackets are copied, since the
   iteration is read on
   before the projectile tick runs; past the store's room the step's remaining roots go in at
   the replay step, in recorded order, the ones already held first.
   The cells' unit slots of the next image go in early (slots_put), for the host's projectile
   tick; the feature tick, a root of the late phase, reads them too (its reproduction tests the
   source cell's unit slot, 0x424189), and the recording machine's tick saw the iteration's slots
   and its input, not the next image's. So each early slot's previous bytes are kept, and put back
   just before the tick root, whose input then goes in over them (THE TICK'S INPUT); features_put
   writes the next image's afterwards. A cell past the store's room is not written early. Where
   the cells are the engine's (s_engine_cells) no slot is written, and the tick's one read of one,
   the source test, takes the recording's value (FS_SOURCE). */
#define PEND_EVENTS 8192
#define PEND_BYTES  (2u << 20)
#define PEND_SLOTS  16384
static struct {
    Lv            L;                /* the level of the step the store holds                     */
    unsigned      n, used;          /* events and bytes held                                      */
    unsigned      next;             /* the first event not yet played                             */
    int           images;           /* the step's feature images wait for the late phase          */
    int           full;             /* the store refused a bracket: none deferred until the step  */
    unsigned      deferred, over;   /* brackets deferred, steps whose store was full              */
    unsigned      nslots;           /* cells slots_put wrote early                                */
    int           restored;         /* their previous slots are back for the tick                 */
    RecEvent      ev[PEND_EVENTS];
    unsigned char b[PEND_BYTES];
    struct { char* cell; unsigned char was[4]; } slot[PEND_SLOTS];
} s_pend;
static RecWorld s_pendW;            /* only ev and nev: what root_play reads of a world           */

/* one event that is neither a root nor a death's, as the step plays it */
static void ev_play(const Lv* L, const RecEvent* e)
{
    const unsigned char* b = e->b;
    unsigned n = e->len, k = e->kind;
    int done;
    if (k == RK_EV_SLICE) {             /* s_st's counts are RK_EV_CREATE..RK_EV_SPRAY's */
        ev_slice(L, b, n);
        return;
    }
    if (k == RK_EV_QUERY) {
        ev_query(L, b, n);
        return;
    }
    switch (k) {
    case RK_EV_CREATE:      done = ev_create(L, b, n); break;
    case RK_EV_SHOT:        done = ev_shot(L, b, n); break;
    case RK_EV_INTERCEPT:   done = ev_intercept(L, b, n); break;
    case RK_EV_START:       done = ev_start(L, b, n); break;
    case RK_EV_START_NAMED: done = ev_start_named(L, b, n); break;
    case RK_EV_BUILT:       done = ev_built(L, b, n); break;
    case RK_EV_STATE:       done = ev_state(L, b, n); break;
    case RK_EV_REPLY:       done = ev_reply(L, b, n); break;
    case RK_EV_SPRAY:       done = ev_spray(L, b, n); break;
    default:                return;     /* a bracket's draws, the tick's cells, a root's end */
    }
    if (done) s_st.played[k - RK_EV_CREATE]++;
    else s_st.skipped[k - RK_EV_CREATE]++;
}

/* the bracket opening at `i` into the store; its RK_EV_FEATURE_END's index, 0 when it does not
   fit */
static unsigned pend_add(const RecWorld* W, unsigned i)
{
    unsigned end, j, bytes = 0, n;
    for (end = i + 1; end < W->nev && W->ev[end].kind != RK_EV_FEATURE_END; end++) {}
    if (end >= W->nev) return 0;
    n = end - i + 1;
    for (j = i; j <= end; j++) bytes += W->ev[j].len;
    if (s_pend.n + n > PEND_EVENTS || s_pend.used + bytes > PEND_BYTES) return 0;
    for (j = i; j <= end; j++) {
        RecEvent* e = &s_pend.ev[s_pend.n++];
        e->kind = W->ev[j].kind;
        e->len = W->ev[j].len;
        e->b = s_pend.b + s_pend.used;
        memcpy(s_pend.b + s_pend.used, W->ev[j].b, W->ev[j].len);
        s_pend.used += W->ev[j].len;
    }
    s_pend.deferred++;
    return end;
}

/* the early slots' previous bytes back, once a step, before the feature tick reads them */
static void slots_restore(void)
{
    unsigned i;
    if (s_pend.restored) return;
    s_pend.restored = 1;
    for (i = s_pend.nslots; i-- > 0;) memcpy(s_pend.slot[i].cell, s_pend.slot[i].was, 4);
}

static int mp_owned_event(unsigned kind);

/* the held brackets of phases up to `phase`, in recorded order: each root, then the events its
   bracket holds, as the step plays them. On a multiplayer level the owners' private events a
   held root nested are their ROOT actions (the materializer), as at the step. */
static void pend_play(unsigned phase)
{
    s_pendW.ev = s_pend.ev;
    s_pendW.nev = s_pend.n;
    while (s_pend.next < s_pend.n) {
        const RecEvent* e = &s_pend.ev[s_pend.next];
        if (e->kind == RK_EV_FEATURE) {
            if (e->b[1] > phase) return;
            if (e->b[0] == FR_TICK) slots_restore();
            root_play(&s_pend.L, &s_pendW, s_pend.next);
        } else if (!mp_owned_event(e->kind)) {
            ev_play(&s_pend.L, e);
        }
        s_pend.next++;
    }
}

static void pend_clear(void)
{
    s_pend.n = s_pend.used = s_pend.next = 0;
    s_pend.images = s_pend.full = 0;
    s_pend.nslots = 0;
    s_pend.restored = 0;
}

/* the store empty and its counts zero, for the next playing */
static void pend_reset(void)
{
    pend_clear();
    s_pend.deferred = s_pend.over = 0;
}

/* Whatever a step left held, in order, its images after: a step whose phases did not run. A
   recording that ended in that step logs its counts now, with every call it made. */
static void pend_finish(void)
{
    if (s_pend.next < s_pend.n || s_pend.images) {
        pend_play(REC_PHASE_LATE);
        if (s_pend.images && s_rd) features_put(&s_pend.L, recr_world(s_rd));
    }
    pend_clear();
    if (s_endlog) {
        s_endlog = 0;
        counts_log("the recording's end", s_endat);
    }
}

void __cdecl tagpu_replay_phase(unsigned phase)
{
    if (!tagpu_puppet_on() || !s_rd || s_dead) return;
    pend_play(phase);
    if (phase >= REC_PHASE_LATE) pend_finish();
}

/* ---- THE MULTIPLAYER MATERIALIZER ----------------------------------------------------------
   A mode-3 recording (demo-recorder-multiplayer-slice.md, *Owner-authority materializer*). The
   recorder's primary stream supplies the shared world -- features, wrecks, the grid, the
   projectiles, the globals, the declared tables -- and the broadcast events it observed. Each
   unit's lifetime, record and mover, each player record and each order list come from the
   owners' contributions, applied in ledger order where the recorder's capture took them (THE
   CLOCK, mp_apply_held), through the lifecycle consumer
   (tagpu_recmp_lifecycle.h) and the reader's authority queries. images_put reads a composed
   world: the primary world's shared parts with this materializer's own unit, live, player and
   list arrays, so the reader's world stays its own delta baseline.

   An initial unit is made from the level-start image as the loader made it (level_units), and
   that image stays its composed one until its owner's first image applies: a remote owner's
   first contribution reaches the recorder some fifty steps after the level starts (MEASURED
   2026-10-05, the four-actor recording: ticks 54..57). A unit the recording creates is made at
   CREATE_BEGIN with the puppet gates shut -- the recording's bracketed children are the only
   ones (*Exactly-once private children*) -- and holds no composed image until its owner's
   first, while `held` keeps units_put from counting it as lingering. Its death runs at
   DEATH_END through ev_death with the entry's saved image, the feature gates shut and its
   corpse level cleared; its corpse and the rest of its feature calls come from the recorder's
   stream (THE DEATHS ON A MULTIPLAYER LEVEL).

   A unit's state byte +0x10E is its composed image's too. The recorder's state events are its
   own copy's changes at its own receipt times, so they are not played (mp_owned_event); books_put
   writes the composed byte through the setter at every capture point, as R8b does at a seek, and
   the puppet's state gate refuses every other change of a puppet's byte on this segment -- the
   create's, a script's -- so each change starts its scripts once (THE UNITS' BOOKKEEPING,
   tagpu_puppet.c). A unit's build finish is its image's too: the primary's build done is not
   played, and unit_put clears the unit's composite when the image changes whether its build is
   finished (THE BUILD FINISH ON A MULTIPLAYER LEVEL).

   Every action names a slot of the recording's frozen blocks, which first_checks has matched
   to the level's array; the consumer bounds every type by the level's definition count
   before a callback runs. A refused action stops the playing: the consumer cannot be resumed,
   and an applied effect is never retried. */

/* one envelope copied out of the reader, until its turn (THE MULTIPLAYER MATERIALIZER) */
typedef struct MpHeld {
    RecMpRecord   where;
    RecMpHeader   packet;
    unsigned      bytes;
    unsigned char body[RECMP_BODY_MAX];
} MpHeld;

static void mp_end(void)
{
    unsigned i;
    if (s_mp.list)
        for (i = 0; i < s_mp.slots * 2; i++) free(s_mp.list[i].img);
    if (s_mp.s_list)
        for (i = 0; i < s_mp.slots * 2; i++) free(s_mp.s_list[i].img);
    free(s_mp.create);
    free(s_mp.xmap);
    free(s_mp.mapped);
    free(s_mp.sel);
    free(s_mp.s_live);
    free(s_mp.s_unit);
    free(s_mp.s_list);
    if (s_mp.life) tagpu_recmp_lifecycle_end(s_mp.life);
    free(s_mp.held);
    free(s_mp.live);
    free(s_mp.unit);
    free(s_mp.list);
    free(s_mp.nodes);
    free(s_mp.q);
    memset(&s_mp, 0, sizeof s_mp);
}

static void mp_fail(const char* fmt, ...)
{
    va_list ap;
    if (s_mp.failed) return;
    s_mp.failed = 1;
    s_over = 1;                     /* whichever phase of the step refused, nothing follows it */
    va_start(ap, fmt);
    vsnprintf(s_mp.why, sizeof s_mp.why, fmt, ap);
    va_end(ap);
    tagpu_logf("replay: %s stops: %s", s_name, s_mp.why);
}

/* A slot's composed order lists emptied: a new incarnation, or none, shows no list until its owner
   describes one for its own birth (mp_authority) */
static void mp_lists_clear(unsigned slot)
{
    s_mp.list[(size_t)slot * 2].n = 0;
    s_mp.list[(size_t)slot * 2 + 1].n = 0;
}

/* a copy of a list image into a composed list, its buffer grown to fit */
static int mp_list_put(RecList* l, const unsigned char* img, unsigned n)
{
    if (n > l->cap) {
        unsigned char* p = realloc(l->img, (size_t)n * REC_ORDER_IMAGE);
        if (!p) return 0;
        l->img = p;
        l->cap = n;
    }
    if (n) memcpy(l->img, img, (size_t)n * REC_ORDER_IMAGE);
    l->n = n;
    return 1;
}

/* At the first step, once level_units has made the level-start world: the consumer, and the
   composed world as that image. 1 ready. */
static int mp_begin(const Lv* L, const RecWorld* W)
{
    const RecHeader* h = recr_header(s_rd);
    unsigned i;
    mp_end();
    s_mp.slots = W->slots;
    s_mp.life = tagpu_recmp_lifecycle_begin(&h->mp, &h->mp_seats, L->ndefs);
    s_mp.held = calloc(W->slots, 1);
    s_mp.live = malloc(W->slots);
    s_mp.unit = malloc((size_t)W->slots * REC_UNIT_IMAGE);
    s_mp.list = calloc((size_t)W->slots * 2, sizeof *s_mp.list);
    s_mp.nodes = malloc((size_t)RECMP_ORDER_MAX * REC_ORDER_IMAGE);
    s_mp.level = 1;
    s_mp.create = malloc((size_t)W->slots * MP_CREATE);
    s_mp.s_live = malloc(W->slots);
    s_mp.s_unit = malloc((size_t)W->slots * REC_UNIT_IMAGE);
    s_mp.s_list = calloc((size_t)W->slots * 2, sizeof *s_mp.s_list);
    s_mp.xbytes = (W->mapped_bytes / 2 + 7) / 8;
    s_mp.xmap = malloc((size_t)REC_PLAYERS * s_mp.xbytes + 1);
    s_mp.mapped = malloc(W->mapped_bytes + 1);
    s_mp.sel = malloc((size_t)REC_PLAYERS * REC_SEL_MAX * sizeof *s_mp.sel);
    s_mp.on = 1;
    if (W->slots != L->nslots || !s_mp.life || !s_mp.held || !s_mp.live || !s_mp.unit ||
        !s_mp.list || !s_mp.nodes || !s_mp.create || !s_mp.s_live || !s_mp.s_unit ||
        !s_mp.s_list || !s_mp.xmap || !s_mp.mapped || !s_mp.sel) {
        mp_fail("the materializer could not be set up for %u slots", W->slots);
        return 0;
    }
    memcpy(s_mp.live, W->live, W->slots);
    memcpy(s_mp.unit, W->unit, (size_t)W->slots * REC_UNIT_IMAGE);
    memcpy(s_mp.player, W->player, sizeof s_mp.player);
    for (i = 0; i < recr_header(s_rd)->mp_seats.count; i++) {
        unsigned seat = recr_header(s_rd)->mp_seats.actor[i].seat;
        if (seat < REC_PLAYERS) s_mp.actor[seat] = 1;
    }
    /* the recording machine's own blocks: the seats of the actors whose contributions it
       produced, the `local` the recorder copies into the header (tagpu_recmp_record.c), which
       THE SCRIPT SLICES plans alone */
    for (i = 0; i < h->mp_seats.count && i < RECMP_ACTORS; i++) {
        unsigned j;
        for (j = 0; j < h->mp.count && j < RECMP_ACTORS; j++)
            if (h->mp.actor[j].origin == h->mp_seats.actor[i].origin && h->mp.actor[j].local) {
                s_mp.own[s_mp.nown].first = h->mp_seats.actor[i].first;
                s_mp.own[s_mp.nown].last = h->mp_seats.actor[i].last;
                s_mp.nown++;
                break;
            }
    }
    for (i = 0; i < W->slots * 2; i++)
        if (!mp_list_put(&s_mp.list[i], W->list[i].img, W->list[i].n) ||
            !mp_list_put(&s_mp.s_list[i], W->list[i].img, W->list[i].n)) {
            mp_fail("no memory for the level-start order lists");
            return 0;
        }
    memcpy(s_mp.s_live, W->live, W->slots);
    memcpy(s_mp.s_unit, W->unit, (size_t)W->slots * REC_UNIT_IMAGE);
    memcpy(s_mp.s_player, W->player, sizeof s_mp.s_player);
    return 1;
}

/* the action's slot is one of the level's: first_checks matched the counts to the recording's */
static int mp_slot_ok(const Lv* L, const RecMpLifeAction* a)
{
    return a->slot >= 1 && a->slot < L->nslots && a->slot < s_mp.slots;
}

/* An initial unit the recorder's level start did not hold -- a remote owner's, which reaches the
   recorder as a network replica -- made from its owner's first image as level_units makes one:
   its loader made it with its Create script's children, which nothing records. */
static int mp_bootstrap_make(const Lv* L, unsigned slot, const uint32_t* img)
{
    const char* ib = (const char*)img;
    int32_t pos[3];
    float built;
    memcpy(pos, ib + 0x6A, 12);
    memcpy(&built, ib + U_BUILT, 4);
    if (!make(L, slot, (unsigned char)ib[U_OWNERIX], img_type(img), pos, build_finished(built),
              img[U_STATE / 4] & 3u, 1))
        return 0;
    s_mp.made++;
    return 1;
}

static int mp_create(const Lv* L, const RecMpLifeAction* a)
{
    const unsigned char* b = a->image;
    uint32_t id, finished;
    unsigned short type;
    int32_t pos[3];
    if (a->bytes != 28) return 0;
    memcpy(&id, b, 4);
    memcpy(&type, b + 4, 2);
    memcpy(&finished, b + 8, 4);
    memcpy(pos, b + 12, 12);
    if (id - 1 != a->slot || !make(L, a->slot, b[6], type, pos, finished, b[7] & 3u, 0))
        return 0;
    memcpy(s_mp.create + (size_t)a->slot * MP_CREATE, b, 28);
    s_mp.create[(size_t)a->slot * MP_CREATE + 28] = 0;
    s_mp.held[a->slot] = 1;
    s_mp.live[a->slot] = 0;
    mp_lists_clear(a->slot);
    s_mp.made++;
    return 1;
}

/* the create's water state, as ev_create writes it: the success exit's own value */
static int mp_created(const Lv* L, const RecMpLifeAction* a)
{
    uint32_t water;
    if (a->bytes != 28 || !host_live(L, a->slot)) return 0;
    water = a->image[24];
    if (water > WATER_MAX) return 0;
    memcpy(unit_at(L, a->slot) + U_WATER, &water, 4);
    s_mp.create[(size_t)a->slot * MP_CREATE + 24] = (unsigned char)water;
    s_mp.create[(size_t)a->slot * MP_CREATE + 28] = 1;
    slice_created(L, a->slot);
    return 1;
}

/* THE DEATHS ON A MULTIPLAYER LEVEL. A unit's end is its owner's: DEATH_END runs the destructor
   through ev_death at the owner's time, with the feature gates shut and the record's corpse level
   cleared, so it changes neither the grid nor the pool. A death's feature effects are the
   recorder's: its corpse (FR_CORPSE, the destructor's call of 0x486360 recorded as a root) and
   its explosion's feature calls are roots of the primary stream, made by root_play at their
   recorded phase and in recorded order like every other root. The unit's end and its corpse
   therefore split in time on the recorder's units seen from another machine: the approved rule
   keeps deaths on the owner's clock and features on the recorder's.

   THE INVARIANT is an ordering. On such a level the feature grid's structure and the wreck pool's
   structure change only through the recorder's feature roots, made here in recorded order at
   their recorded phases: the seven feature gates refuse every other call, and the owner-timed
   destructor runs with them shut and its corpse level cleared. Each root reads only what the
   recording makes equal -- the grid and the pool, by induction; the feature and unit defs, which
   data_match pins; its draws, armed; the one unit slot a feature call tests, FS_SOURCE; its
   recorded arguments, bounded. A spawn takes the free list's head (0x423DA8) and the list mover
   pushes at it (0x4232F0, LIFO), so one call out of order would shift every later record; in
   order, from the level's start, every spawn meets the free list the recorder's met and takes the
   record, the cell and the list place the recorder's took. The images keep comparing structure
   and never write it.

   A death's other events are its owner's (the materializer's ROOT actions), and a draw a death's
   destructor made outside any root is counted, not played. The pump 0x4954C8 runs before the
   capture 0x4954ED, so a remote unit's death the recorder applied from the network is recorded
   LATE, after its iteration's tick root, and its corpse goes in there. MEASURED 2026-10-05, the
   four-actor recording: 24 deaths; the 17 of remote units LATE with no root inside; the
   recorder's own 7 at phase 0, 6 holding explosion roots; every remote death applied by its owner
   2..7 ticks before the recorder observed it. */
static int mp_death(const Lv* L, const RecMpLifeAction* a)
{
    int done = ev_death(L, a->image, a->bytes, 0);
    s_fst.deaths++;
    return done;
}

static int mp_root(const Lv* L, const RecMpLifeAction* a)
{
    int done;
    unsigned k = a->kind;
    switch (k) {
    case RK_EV_START_NAMED: done = ev_start_named(L, a->image, a->bytes); break;
    case RK_EV_REPLY:       done = ev_reply(L, a->image, a->bytes); break;
    case RK_EV_SPRAY:       done = ev_spray(L, a->image, a->bytes); break;
    default:                return 0;
    }
    s_mp.roots++;
    if (done) {
        s_mp.rplayed++;
        s_st.played[k - RK_EV_CREATE]++;
    } else {
        s_st.skipped[k - RK_EV_CREATE]++;
    }
    return 1;
}

/* THE SEEK OF A MULTIPLAYER RECORDING. The composed world of iteration T is what the forward
   playing holds when the world holds iteration T: every envelope read through T, those of T
   applied up to its first post-capture root batch, the rest of T held (THE CLOCK). mp_rebuild makes
   it so by construction: the level-start composed world, a fresh consumer, every envelope from
   the first (recr_mp_apply from ordinal 0, the reader standing at T), then mp_apply_held and
   mp_authority as mp_step_image calls them -- the same reads, the same order, the same split.
   Only the actions differ: here they compose data and touch no engine state (mp_compose), since
   the seek then makes every unit of the composed world itself, as it makes a skirmish's (R7),
   and a unit created but not yet described from its create's own arguments (mp_make_held). A
   private root is not played, as a skirmish seek plays no event before T. What a refusal leaves:
   mp_fail stops the playing, so nothing composed half is ever applied. */
static int mp_compose(const Lv* L, const RecMpLifeAction* a)
{
    unsigned char* c = s_mp.create + (size_t)a->slot * MP_CREATE;
    switch (a->action) {
    case RECMP_LIFE_BOOTSTRAP:
    case RECMP_LIFE_IMAGE:
        if (a->bytes != REC_UNIT_IMAGE) break;
        memcpy(s_mp.unit + (size_t)a->slot * REC_UNIT_DWORDS, a->image, REC_UNIT_IMAGE);
        s_mp.live[a->slot] = 1;
        s_mp.held[a->slot] = 0;
        return 1;
    case RECMP_LIFE_ABSENCE:
        s_mp.live[a->slot] = 0;
        return 1;
    case RECMP_LIFE_CREATE_BEGIN:
        if (a->bytes != 28) break;
        memcpy(c, a->image, 28);
        c[28] = 0;
        s_mp.held[a->slot] = 1;
        s_mp.live[a->slot] = 0;
        mp_lists_clear(a->slot);
        return 1;
    case RECMP_LIFE_CREATE_END:
        if (a->bytes != 28) break;
        if (s_mp.held[a->slot]) {
            c[24] = a->image[24];
            c[28] = 1;
        }
        return 1;
    case RECMP_LIFE_DEATH_BEGIN:
        return 1;
    case RECMP_LIFE_DEATH_END:
        if (a->bytes != 12 + REC_UNIT_IMAGE) break;
        s_mp.live[a->slot] = 0;
        s_mp.held[a->slot] = 0;
        mp_lists_clear(a->slot);
        return 1;
    case RECMP_LIFE_ROOT:
        s_mp.croots++;
        return 1;
    default:
        break;
    }
    mp_fail("GameTime %u: an owner action %u of slot %u (%u bytes) cannot be composed", L->now,
            a->action, a->slot, a->bytes);
    return 0;
}

static int mp_action(void* ctx, const RecMpLifeAction* a)
{
    const Lv* L = s_mp.L;
    const char* what = "";
    (void)ctx;
    if (!mp_slot_ok(L, a)) {
        mp_fail("GameTime %u: an owner action names slot %u, outside the level", L->now, a->slot);
        return 0;
    }
    if (s_mp.compose) return mp_compose(L, a);
    switch (a->action) {
    case RECMP_LIFE_BOOTSTRAP:
        if (a->bytes == REC_UNIT_IMAGE && !host_live(L, a->slot) &&
            !mp_bootstrap_make(L, a->slot, (const uint32_t*)(const void*)a->image)) {
            what = "a first image";
            break;
        }
        /* fall through: the unit is here, as its owner's loader made it */
    case RECMP_LIFE_IMAGE:
        if (a->bytes != REC_UNIT_IMAGE ||
            !holds(L, a->slot, (const uint32_t*)(const void*)a->image)) {
            what = a->action == RECMP_LIFE_BOOTSTRAP ? "a first image" : "an image";
            break;
        }
        memcpy(s_mp.unit + (size_t)a->slot * REC_UNIT_DWORDS, a->image, REC_UNIT_IMAGE);
        s_mp.live[a->slot] = 1;
        s_mp.held[a->slot] = 0;
        s_mp.images++;
        return 1;
    case RECMP_LIFE_ABSENCE:
        if (host_live(L, a->slot)) {
            what = "an absence";
            break;
        }
        s_mp.live[a->slot] = 0;
        return 1;
    case RECMP_LIFE_CREATE_BEGIN:
        if (mp_create(L, a)) return 1;
        what = "a create";
        break;
    case RECMP_LIFE_CREATE_END:
        if (mp_created(L, a)) return 1;
        what = "a create's return";
        break;
    case RECMP_LIFE_DEATH_BEGIN:
        if (host_live(L, a->slot)) return 1;
        what = "a death";
        break;
    case RECMP_LIFE_DEATH_END:
        if (a->bytes == 12 + REC_UNIT_IMAGE && mp_death(L, a)) {
            s_mp.live[a->slot] = 0;
            s_mp.held[a->slot] = 0;
            mp_lists_clear(a->slot);
            s_mp.destroyed++;
            return 1;
        }
        what = "a death's return";
        break;
    case RECMP_LIFE_ROOT:
        if (mp_root(L, a)) return 1;
        what = "a private root";
        break;
    default:
        what = "an unknown action";
        break;
    }
    mp_fail("GameTime %u: %s of slot %u (owner %u, birth %u) does not apply here", L->now, what,
            a->slot, a->source.origin, a->birth);
    return 0;
}

/* THE CLOCK. The recorder's B2 at GameTime T (tagpu_recorder.c, mp_capture) flushes its own
   roots of unit tick T-1, advances, flushes the roots its pump made at T, publishes its own
   player images of T, consumes every remote contribution that has arrived, then publishes its
   own unit, order and explored images of T; the roots its unit tick T then makes are flushed
   at the next B2, still labelled T. So the
   envelopes of application tick T are, in ledger order, those the recorder held when it
   captured image T, then its own post-capture roots of T. The replay plays image T at step T-1:
   step T-1 applies tick T's envelopes up to the first post-capture root batch the recorder
   produced, and step T applies the rest after its primary events, where its unit tick made
   them. */
static void mp_envelope(void* ctx, const RecMpRecord* where, const RecMpHeader* packet,
                        const void* body, unsigned bytes)
{
    MpHeld* e;
    (void)ctx;
    if (s_mp.failed) return;
    if (bytes > RECMP_BODY_MAX) {
        mp_fail("an envelope of %u bytes, past the %u a packet holds", bytes,
                (unsigned)RECMP_BODY_MAX);
        return;
    }
    if (s_mp.nq == s_mp.capq) {
        unsigned cap = s_mp.capq ? s_mp.capq * 2 : 64;
        MpHeld* q = realloc(s_mp.q, (size_t)cap * sizeof *q);
        if (!q) {
            mp_fail("no memory for %u held envelopes", cap);
            return;
        }
        s_mp.q = q;
        s_mp.capq = cap;
    }
    e = &s_mp.q[s_mp.nq++];
    e->where = *where;
    e->packet = *packet;
    e->bytes = bytes;
    memcpy(e->body, body, bytes);
}

/* THE CHAT OF A MULTIPLAYER RECORDING. Each text entry is shown when the recorder applied it, the
   clock every composed contribution keeps (THE CLOCK), through the stock display sink 0x463CA0
   (text, flags, unit, source row) that the text receive case 0x45522E calls. Who sees it follows
   the viewed record: the viewer's own record V is the full map, where every message shows once,
   at its author's original (a SOURCE entry, whose origin is its recipient); a player's view shows
   what that player's machine showed -- its own lines as the formatter's local echo (source row
   10, the entry's display flags, 0x463ED4) and the lines its machine received (RECEIVED entries
   whose recipient is its origin, flags 8 as the receive case passes). An origin no actor of the
   roster holds names no row and shows nothing. The sink copies 64 bytes and ends the line at 63
   (0x463D25..0x463D41), so the text is copied out with a NUL after the packet's 64. A seek
   empties the message log (sk_deadlines) and shows no line before T, as it plays no event
   before T; hidden, nothing is shown. */
typedef void (__stdcall *PFN_ChatShow)(const char* text, int flags, int unit, int row);
#define E_ChatShow ((PFN_ChatShow)0x00463CA0u)

static int mp_row(uint32_t origin)
{
    const RecHeader* h = recr_header(s_rd);
    unsigned i;
    for (i = 0; i < h->mp_seats.count; i++)
        if (h->mp_seats.actor[i].origin == origin && h->mp_seats.actor[i].seat < REC_PLAYERS)
            return (int)h->mp_seats.actor[i].seat;
    return -1;
}

static void mp_text(const Lv* L, const MpHeld* e)
{
    unsigned count, i;
    unsigned viewed = (unsigned char)L->ta[OFF_VIEWED];
    int full = viewed == s_viewer;
    uint32_t mine = 0;
    if (!full) {
        const RecHeader* h = recr_header(s_rd);
        for (i = 0; i < h->mp_seats.count; i++)
            if (h->mp_seats.actor[i].seat == viewed) mine = h->mp_seats.actor[i].origin;
    }
    if (e->bytes < 4) return;
    memcpy(&count, e->body, 4);
    if (count > RECMP_TEXT_COUNT || e->bytes != 4 + count * sizeof(RecMpTextEntry)) return;
    for (i = 0; i < count; i++) {
        RecMpTextEntry t;
        char line[RECMP_CHAT_STOCK_BYTES];
        int author;
        memcpy(&t, e->body + 4 + (size_t)i * sizeof t, sizeof t);
        s_mp.said++;
        if (!tagpu_recmp_text_entry_valid(&t) || (author = mp_row(t.origin)) < 0) continue;
        memcpy(line, t.stock + 1, sizeof line - 1);
        line[sizeof line - 1] = 0;
        if (s_chat_hidden) continue;
        if (full ? t.phase == RECMP_TEXT_SOURCE :
            t.phase == RECMP_TEXT_SOURCE ? t.origin == mine : t.recipient == mine) {
            int own = !full && t.phase == RECMP_TEXT_SOURCE;
            E_ChatShow(line, own ? (int)t.display : 8, 0, own ? 10 : author);
            s_mp.shown++;
        }
    }
}

/* A recorded click, shown from its application tick (THE CLOCK), as chat is. The reader's history
   has checked its length and fields (tagpu_recmp_pov_put); its origin names the seat, bounded
   below REC_PLAYERS by mp_row, and an origin no actor holds shows nothing. A seek shows no click
   from before T: mp_rebuild composes with the actions only, which take none. */
static void mp_click(const MpHeld* e)
{
    RecClick c;
    int seat;
    if (e->bytes != sizeof c || (seat = mp_row(e->packet.origin)) < 0) return;
    memcpy(&c, e->body, sizeof c);
    if (c.player != RECMP_POV_PLAYER || !rec_click_ok(&c)) return;
    c.player = (uint8_t)seat;
    ov_click_put(e->where.tick, &c);
}

/* a root batch the recorder's own unit tick made after its capture point */
static int mp_post_capture(const MpHeld* e)
{
    return e->where.phase == RECMP_RECORD_PRODUCED && e->packet.kind == RECMP_ROOTS &&
           e->bytes >= 4 + sizeof(RecMpRootsEntry) &&
           !((const RecMpRootsEntry*)(const void*)(e->body + 4))->before_capture;
}

/* the held envelopes through tick T, and with `image` tick T+1's up to its post-capture roots */
static void mp_apply_held(const Lv* L, unsigned T, int image)
{
    unsigned i = 0;
    s_mp.L = L;
    for (; i < s_mp.nq && !s_mp.failed; i++) {
        const MpHeld* e = &s_mp.q[i];
        if (e->where.tick > T + (image ? 1u : 0u) ||
            (image && e->where.tick == T + 1 && mp_post_capture(e)))
            break;
        if (!tagpu_recmp_lifecycle_record(s_mp.life, &e->where, &e->packet, e->body, e->bytes,
                                          mp_action, NULL) && !s_mp.failed)
            mp_fail("GameTime %u: the owners' contributions refused: %s", L->now,
                    tagpu_recmp_lifecycle_error(s_mp.life));
        if (!s_mp.compose && e->where.phase != RECMP_RECORD_PROOF && e->packet.kind == RECMP_TEXT)
            mp_text(L, e);
        if (!s_mp.compose && e->where.phase != RECMP_RECORD_PROOF && e->packet.kind == RECMP_CLICK)
            mp_click(e);
    }
    memmove(s_mp.q, s_mp.q + i, (size_t)(s_mp.nq - i) * sizeof *s_mp.q);
    s_mp.nq -= i;
    s_mp.L = NULL;
}

/* The players and order lists through step T, by the reader's application authority; a player
   or a list no owner has described yet keeps its level-start image. An order list goes in only
   for the incarnation its owner described: the create and the death empty the slot's lists
   (mp_lists_clear), and a list whose birth is another incarnation's is not copied. */
static void mp_authority(const Lv* L)
{
    const RecHeader* h = recr_header(s_rd);
    unsigned i, slot, list;
    char why[sizeof s_mp.why];
    for (i = 0; i < h->mp_seats.count && !s_mp.failed; i++) {
        const RecMpSeat* st = &h->mp_seats.actor[i];
        RecMpPlayerState ps;
        int k = recr_mp_player(s_rd, st->origin, &ps, why, sizeof why);
        if (k < 0) mp_fail("GameTime %u: player authority: %s", L->now, why);
        else if (k && st->seat < REC_PLAYERS) memcpy(s_mp.player[st->seat], ps.image, sizeof ps.image);
    }
    /* each actor's explored map, by the same authority as its player record */
    for (i = 0; i < h->mp_seats.count && !s_mp.failed; i++) {
        const RecMpSeat* st = &h->mp_seats.actor[i];
        RecMpExploreState es;
        int k;
        if (st->seat >= REC_PLAYERS) continue;
        k = recr_mp_explored(s_rd, st->origin, &es, s_mp.xmap + (size_t)st->seat * s_mp.xbytes,
                             s_mp.xbytes, why, sizeof why);
        if (k < 0) mp_fail("GameTime %u: explored-map authority: %s", L->now, why);
        else if (k && es.bytes != s_mp.xbytes)
            mp_fail("GameTime %u: origin %u's explored map is %u bytes, the level's %u", L->now,
                    st->origin, es.bytes, s_mp.xbytes);
        else s_mp.xvalid[st->seat] = (unsigned char)k;
    }
    /* each actor's camera samples and selection, by the same authority: the samples' ticks are
       the recorder's application ticks, the clock the displayed tick n counts in (persp_world).
       The history has held every selection entry inside the actor's own block and below
       REC_SEL_MAX (tagpu_recmp_pov_put); a seat is below REC_PLAYERS */
    for (i = 0; i < h->mp_seats.count && !s_mp.failed; i++) {
        const RecMpSeat* st = &h->mp_seats.actor[i];
        RecMpPovState ps;
        RecMpSelectState ss;
        unsigned n;
        int k;
        if (st->seat >= REC_PLAYERS) continue;
        k = recr_mp_pov(s_rd, st->origin, &ps, why, sizeof why);
        if (k < 0) {
            mp_fail("GameTime %u: perspective authority: %s", L->now, why);
            break;
        }
        s_mp.npov[st->seat] = k ? ps.count : 0;
        for (n = 0; k && n < ps.count && n < 2; n++) {
            s_mp.pov[st->seat][n] = ps.sample[n].pov;
            s_mp.pov[st->seat][n].player = (uint8_t)st->seat;
            s_mp.pov_tick[st->seat][n] = ps.sample[n].applied_tick;
        }
        k = recr_mp_select(s_rd, st->origin, &ss, s_mp.sel + (size_t)st->seat * REC_SEL_MAX,
                           REC_SEL_MAX, why, sizeof why);
        if (k < 0) mp_fail("GameTime %u: selection authority: %s", L->now, why);
        else s_mp.nsel[st->seat] = k ? ss.count : 0;
    }
    for (slot = 1; slot < s_mp.slots && !s_mp.failed; slot++) {
        RecMpLifeState ls;
        if (!s_mp.live[slot] && !s_mp.held[slot]) continue;
        if (tagpu_recmp_lifecycle_get(s_mp.life, slot, &ls) != 1) continue;
        for (list = 0; list < 2; list++) {
            RecMpOrderState os;
            RecList* l = &s_mp.list[(size_t)slot * 2 + list];
            int k = recr_mp_orders(s_rd, slot, list, &os, s_mp.nodes, RECMP_ORDER_MAX, why,
                                   sizeof why);
            if (k < 0) {
                mp_fail("GameTime %u: order authority: %s", L->now, why);
                break;
            }
            if (!k || os.birth != ls.birth) continue;
            if (!mp_list_put(l, s_mp.nodes, os.count)) mp_fail("no memory for an order list");
        }
    }
}

/* Step T's own post-capture roots, after its primary events (THE CLOCK) */
static void mp_step_events(const Lv* L, unsigned T)
{
    if (!s_mp.on || s_mp.failed) return;
    mp_apply_held(L, T, 0);
    if (s_mp.failed) s_over = 1;
}

/* What image T+1 rests on, once the world holds iteration T+1 (THE CLOCK): the envelopes read
   through T+1, those of T+1 up to its post-capture roots applied, then the players and lists */
static void mp_step_image(const Lv* L, unsigned T)
{
    char why[160];
    if (!s_mp.on || s_mp.failed) return;
    if (recr_mp_apply(s_rd, &s_mp.ordinal, mp_envelope, NULL, why, sizeof why) < 0)
        mp_fail("GameTime %u: the contributions cannot be read: %s", L->now, why);
    if (!s_mp.failed) mp_apply_held(L, T, 1);
    if (!s_mp.failed) mp_authority(L);
    if (s_mp.failed) s_over = 1;
}

/* the world images_put reads: the primary world with the composed units, lists and actors'
   player records. A record no actor holds has no owner to describe it, so the recorder's
   observation is its one source: the destructor's rank update moves every record's +0x148, the
   unseated ones included -- its loop over the ten records has no activity test
   (0x486BE7..0x486C13, DISASSEMBLED). */
static const RecWorld* mp_world(const RecWorld* W)
{
    unsigned p, i, n;
    if (!s_mp.on || W == &s_mp.W) return W;
    s_mp.W = *W;
    s_mp.W.live = s_mp.live;
    s_mp.W.unit = s_mp.unit;
    s_mp.W.list = s_mp.list;
    for (p = 0; p < REC_PLAYERS; p++)
        if (s_mp.actor[p]) memcpy(s_mp.W.player[p], s_mp.player[p], sizeof s_mp.player[p]);
    /* The explored map: an actor's bit in every entry is its owner's, a bit for each entry in
       the map's own order (tagpu_recmp_explored.h), the other bits the recorder's. The bit is the
       record's +0x146 as the recording machine stamped it, bounded to the entry's 16. */
    memcpy(s_mp.mapped, W->mapped, W->mapped_bytes);
    n = W->mapped_bytes / 2;
    for (p = 0; p < REC_PLAYERS; p++) {
        const unsigned char* x = s_mp.xmap + (size_t)p * s_mp.xbytes;
        unsigned b = ((const unsigned char*)W->player[p])[PL_MAPBIT];
        if (!s_mp.xvalid[p]) continue;
        if (b >= 16) {
            mp_fail("record %u's explored-map bit is %u", p, b);
            break;
        }
        for (i = 0; i < n; i++) {
            uint16_t v;
            memcpy(&v, s_mp.mapped + 2 * i, 2);
            v = (uint16_t)((v & ~(1u << b)) | ((x[i >> 3] >> (i & 7) & 1u) << b));
            memcpy(s_mp.mapped + 2 * i, &v, 2);
        }
    }
    s_mp.W.mapped = s_mp.mapped;
    return &s_mp.W;
}

/* The world the perspective reads -- Follow, the overlays and the HUD's rows: in a multiplayer
   recording's own segment the reader's world with the composed units and lists and the actors'
   samples and selections (mp_authority), anywhere else the reader's own. The samples' ticks are
   the recorder's application ticks and W->tick is the application tick the world holds, so the
   displayed tick n is the composed application tick. A copy of the world's fixed part, rewritten
   at each call on the game thread; the explored map is the primary world's, which nothing here
   reads. */
static const RecWorld* persp_world(void)
{
    const RecWorld* W = recr_world(s_rd);
    if (!s_mp.on) return W;
    s_mp.P = *W;
    s_mp.P.live = s_mp.live;
    s_mp.P.unit = s_mp.unit;
    s_mp.P.list = s_mp.list;
    memcpy(s_mp.P.npov, s_mp.npov, sizeof s_mp.npov);
    memcpy(s_mp.P.pov, s_mp.pov, sizeof s_mp.pov);
    memcpy(s_mp.P.pov_tick, s_mp.pov_tick, sizeof s_mp.pov_tick);
    memcpy(s_mp.P.nsel, s_mp.nsel, sizeof s_mp.nsel);
    s_mp.P.sel = s_mp.sel;
    return &s_mp.P;
}

/* the composed world of iteration T, the reader standing at T (THE SEEK OF A MULTIPLAYER
   RECORDING); 1 composed, 0 the playing stopped */
static int mp_rebuild(const Lv* L, unsigned T)
{
    const RecHeader* h = recr_header(s_rd);
    char why[160];
    unsigned i;
    if (s_mp.failed) return 0;
    tagpu_recmp_lifecycle_end(s_mp.life);
    s_mp.life = tagpu_recmp_lifecycle_begin(&h->mp, &h->mp_seats, L->ndefs);
    if (!s_mp.life) {
        mp_fail("no memory for the seek's owner consumer");
        return 0;
    }
    memcpy(s_mp.live, s_mp.s_live, s_mp.slots);
    memcpy(s_mp.unit, s_mp.s_unit, (size_t)s_mp.slots * REC_UNIT_IMAGE);
    memcpy(s_mp.player, s_mp.s_player, sizeof s_mp.player);
    memset(s_mp.held, 0, s_mp.slots);
    memset(s_mp.xvalid, 0, sizeof s_mp.xvalid);
    memset(s_mp.npov, 0, sizeof s_mp.npov);
    memset(s_mp.nsel, 0, sizeof s_mp.nsel);
    for (i = 0; i < s_mp.slots * 2; i++)
        if (!mp_list_put(&s_mp.list[i], s_mp.s_list[i].img, s_mp.s_list[i].n)) {
            mp_fail("no memory for the seek's order lists");
            return 0;
        }
    s_mp.nq = 0;
    s_mp.ordinal = 0;
    s_mp.croots = 0;
    s_mp.compose = 1;
    if (recr_mp_apply(s_rd, &s_mp.ordinal, mp_envelope, NULL, why, sizeof why) < 0)
        mp_fail("the seek to %u cannot read the contributions: %s", T, why);
    if (!s_mp.failed) mp_apply_held(L, T - 1, 1);
    s_mp.compose = 0;
    if (!s_mp.failed) mp_authority(L);
    if (s_mp.failed) return 0;
    s_mp.rebuilt++;
    return 1;
}

/* a unit its owner created and has not yet described, made from its create's arguments as
   mp_create made it, its water from the create's return; how many */
static unsigned mp_make_held(const Lv* L)
{
    unsigned slot, made = 0;
    for (slot = 1; slot < s_mp.slots; slot++) {
        const unsigned char* c = s_mp.create + (size_t)slot * MP_CREATE;
        uint32_t id, finished, water;
        unsigned short type;
        int32_t pos[3];
        if (!s_mp.held[slot]) continue;
        memcpy(&id, c, 4);
        memcpy(&type, c + 4, 2);
        memcpy(&finished, c + 8, 4);
        memcpy(pos, c + 12, 12);
        if (id - 1 != slot || !make(L, slot, c[6], type, pos, finished, c[7] & 3u, 0)) continue;
        water = c[24];
        if (c[28] && water <= WATER_MAX) memcpy(unit_at(L, slot) + U_WATER, &water, 4);
        made++;
    }
    return made;
}

/* The materializer follows the segment the reader stands in. A multiplayer file's takeover timeline
   is a skirmish segment: a live skirmish recorded it, every unit its own seat's, so there the
   replay plays it as a skirmish's -- the recorded world whole, the cells as that segment holds
   them (THE UNIT SLOTS AND THE YARD BIT) -- and in the file's own segment it composes T's world
   (THE SEEK OF A MULTIPLAYER RECORDING). Called after every reader seek, before the seek writes
   anything, so the contest's rule a crossing changes is in before the seek's creates stamp. 1,
   or 0 the playing stopped. */
static int mp_follow(const Lv* L, unsigned T)
{
    int here, held;
    if (!s_mp.level) return 1;
    here = recr_segment_mp(s_rd);
    held = !here && recr_segment_cells(s_rd);
    if (!tagpu_puppet_engine_cells(here || held, here)) {
        mp_fail("the cells' rule could not follow the segment at GameTime %u (tagpu_puppet.c)", T);
        return 0;
    }
    s_mp.on = here;
    s_engine_cells = here || held;
    s_cells_held = held;
    return here ? mp_rebuild(L, T) : !s_mp.failed;
}

/* the primary events a mode-3 world carries for what the owners contribute. RK_EV_STATE and
   RK_EV_BUILT among them: the recorder's copy takes a state change from the engine's 0x11 and a
   build's finish from its 0x12 (the receiver 0x4555BA -> 0x41B8D0) at its own receipt time, on
   another session, so that step is not the composed image's. The image's state byte is written
   by books_put (THE UNITS' BOOKKEEPING), a finish by the image's own write (THE BUILD FINISH ON A
   MULTIPLAYER LEVEL). */
static int mp_owned_event(unsigned kind)
{
    return s_mp.on && (kind == RK_EV_CREATE || kind == RK_EV_START_NAMED ||
                       kind == RK_EV_REPLY || kind == RK_EV_SPRAY || kind == RK_EV_STATE ||
                       kind == RK_EV_BUILT);
}

/* The step's events in recorded order; with `defer`, the roots of a later phase into the store.
   A skirmish recording's death is played whole by death_play, the roots inside it among them; a
   multiplayer recording's death is its owner's, and only its roots go in here, each at its own
   phase like any other (THE DEATHS ON A MULTIPLAYER LEVEL). */
static void events_put(const Lv* L, const RecWorld* W, int defer)
{
    unsigned i, end, deaths = 0, mdeaths = 0;
    int inroot = 0;
    if (defer) s_pend.L = *L;
    for (i = 0; i < W->nev; i++) {
        unsigned k = W->ev[i].kind;
        if (mp_owned_event(k)) continue;
        switch (k) {
        case RK_EV_DEATH:
            if (s_mp.on) {
                mdeaths++;
                s_mp.observed++;
                if (W->ev[i].b[10] & 0xF) s_mp.ocorpse++;
                break;
            }
            if (death_play(L, W, i)) s_st.played[k - RK_EV_CREATE]++;
            else s_st.skipped[k - RK_EV_CREATE]++;
            deaths++;
            break;
        case RK_EV_DEATH_END:
            if (s_mp.on) mdeaths--;
            else deaths--;
            break;
        case RK_EV_FEATURE_END:
            inroot = 0;
            break;
        case RK_EV_FEATURE:
            if (deaths) break;          /* death_play made it inside the destructor */
            if (defer && !s_pend.full && W->ev[i].b[1] > REC_PHASE_UNITS) {
                if ((end = pend_add(W, i)) != 0) {
                    i = end;            /* the bracket's events go with its root */
                    break;
                }
                s_pend.full = 1;
                s_pend.over++;
                pend_play(REC_PHASE_LATE);  /* the held ones first: the order stays recorded */
            }
            root_play(L, W, i);
            inroot = 1;
            break;
        default:
            if (mdeaths && !inroot) {   /* a death's own events are its owner's */
                if (k == RK_EV_DRAW) s_fst.left++;  /* a draw no recorded call made */
                break;
            }
            ev_play(L, &W->ev[i]);
            break;
        }
    }
}

/* every seat's +0x18 = GameTime, each step while the level plays or has played a recording: the
   pacing gate would otherwise slow the level's ticks to a hundredth as the stamps age */
static void pace(char* ta, unsigned now)
{
    unsigned p;
    for (p = 0; p < REC_PLAYERS; p++)
        if (tagpu_puppet_seat(p)) memcpy(player_rec(ta, p) + PL_PACE, &now, 4);
}

/* The cells' unit slots of the next image, ahead of its features (THE PHASES): the host's
   projectile tick tests a projectile against the units the cell names (0x49B1C7..0x49B275), which
   on the recording machine its unit tick had just moved. A slot is a value only the images
   write (THE UNIT SLOTS AND THE YARD BIT), bounded and alive as cell_put takes it, and
   independent of the cell's feature, so it goes in whatever the cell's structure, and stays in a
   cell whose structure features_put then finds different and leaves alone. Each cell's previous
   slots are kept for the feature tick (THE PHASES). */
static void slots_put(const Lv* L, const RecWorld* W)
{
    unsigned i, k, j;
    uint16_t h[4], slot[2];
    if (s_engine_cells) return;
    for (i = 0; i < W->nfeat; i++) {
        const unsigned char* b = W->feat[i].b;
        if (W->feat[i].kind != RK_CELLS) continue;
        memcpy(h, b, 8);    /* bounded by the reader (cells_ok) and again by cell_at */
        for (k = 0; k < h[2]; k++) {
            char* c = cell_at(L, (int32_t)h[0] + (int32_t)k, h[1]);
            const unsigned char* img = b + 8 + (size_t)k * REC_CELL_BYTES;
            if (!c) continue;
            memcpy(slot, img, 4);
            if (slot[0] >= L->nslots || slot[1] >= L->nslots ||
                (slot[0] && !host_live(L, slot[0])) || (slot[1] && !host_live(L, slot[1])) ||
                s_pend.nslots == PEND_SLOTS)
                continue;
            s_pend.slot[s_pend.nslots].cell = c;
            memcpy(s_pend.slot[s_pend.nslots++].was, c, 4);
            for (j = 0; j < 4; j++)
                c[j] = (char)(((unsigned char)c[j] & ~s_cval[j]) | (img[j] & s_cval[j]));
        }
    }
}

/* the next image; with `defer`, its features wait for the step's late phase (THE PHASES), all but
   the cells' unit slots */
static void images_put(const Lv* L, const RecWorld* W, int defer)
{
    links_put(L, W);
    units_put(L, W);
    yards_put(L, W);
    states_put(L, W);
    players_put(L, W);
    orders_put(L, W);
    if (defer) {
        slots_put(L, W);
        s_pend.images = 1;
    } else {
        features_put(L, W);
    }
}

/* 0x466C20(): the minimap's fog composite, no argument, plain `ret` (0x466DB1; DISASSEMBLED
   0x466C20..0x466DB1). Only when main+0x142F1 bit 2 asks for it (0x466C29..0x466C44), it clears
   the bit and rebuilds the minimap's fog from the explored map and the viewed record's counters,
   then sets bit 1, DrawMinimap's redraw flag (0x466DA4). */
typedef void (__cdecl* PFN_MinimapFog)(void);
#define E_MinimapFog  ((PFN_MinimapFog)0x00466C20u)

/* bumped at the frame visit's writes the engine's cull reads; game thread (THE DRAW'S UNIT LIST) */
static unsigned s_cull_inputs;

/* FULL VISIBILITY (demo-recorder-s2-host.md §2.9), at the first frame visit or step
   (viewer_init), after the loader's sight rebuild 0x4816A0(1) (0x497AE5) has filled every seat's
   and reset the explored map: V's sight counters, record 10's, all 1, the value that rebuild
   writes when true LOS is off, and V's bit in every cell of the explored map, each
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
   player init 0x464700 to 0x464A00); every caller runs in the in-play frame callback 0x496790
   (the frame visit or the step) before the teardown wrap, on the game thread, the one thread that
   reads them (the frame packet copies the images in the in-play draw).
   V's bit in the map is the one the engine's own setter 0x481930 sets for V, 1 << [V+0x146]
   (DISASSEMBLED 0x48194B..0x481951: `mov cl,[eax+0x146]; shl ebx,cl`), each entry a u16, so a
   value of 16 or more names no bit of it and nothing is set. */
static void minimap_fog(char* ta)
{
    s_cull_inputs++;                    /* LosType (THE DRAW'S UNIT LIST) */
    *(uint16_t*)(ta + OFF_LOSTYPE) &= (uint16_t)~8u;
    *(unsigned char*)(ta + OFF_MMFLAGS) |= 4u;
    E_MinimapFog();
}

static void sight_fill(char* ta, unsigned v)
{
    char* rec = player_rec(ta, v);
    unsigned char* cnt;
    unsigned char* mapped;
    s_cull_inputs++;                    /* the viewed record's sight map (THE DRAW'S UNIT LIST) */
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
    minimap_fog(ta);
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
     writer of each before anything in step T reads it. At the level's first iteration the write
     follows the first step's creates and image T in the same call (THE FIRST STEP'S UNITS), so
     it is their last writer there too. After the step's unit tick the host's
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
/* 0x482910(point, sight, height, duration): the temporary sight record's add, stdcall, `ret 0x10`
   (DISASSEMBLED 0x482910..0x482AAF). It refuses, touching nothing, when LosType bit 1 is clear
   (0x482920) or the count is already SR_MAX (0x48292F); otherwise it makes the record for the
   viewed record main+0x2A43 with its expiry GameTime + duration (0x4829C4), stamps it (0x4829F4,
   the ray fan; the circle after it) and counts it (0x482AA9). The destructor calls it for a
   viewed seat's live unit with the def's +0x202, +0x170 and 60 (0x48672C..0x486748). */
typedef void (__stdcall* PFN_SightAdd)(const int32_t* point, int sight, int height, int duration);
#define E_SightAdd    ((PFN_SightAdd)0x00482910u)

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

/* A takeover's R13 (seek_run): the list's `hold` rows from the image, under their masks, as
   globals_put writes its `write` rows -- the end countdown main+0x39239, every bit, and the end
   flags main+0x3923B & 0x0014 -- since the level they describe goes on as a live game: a takeover
   inside a recorded end countdown continues it. */
static void holds_put(char* ta, const RecWorld* W)
{
    unsigned i;
    for (i = 0; i < REC_GLOBALS_SLOTS; i++) {
        const ReplayGlobalSlot* g = &tagpu_replay_globals[i];
        unsigned char* at = global_at(ta, g);
        if (!g->hold) continue;
        if (g->size <= 4) {
            uint32_t v = 0;
            memcpy(&v, at, g->size);
            v = (v & ~g->hold) | (W->globals[g->dword] & g->hold);
            memcpy(at, &v, g->size);
        } else {
            memcpy(at, W->globals + g->dword, g->size);
        }
    }
}

/* Every temporary sight record dropped through the engine's own expiry: each one's +0x1C set to 0,
   then 0x482130, which removes every record whose expiry is below GameTime -- every one, GameTime
   being 1 or more wherever this runs: R0 (sk_bound) refuses a seek at GameTime 0. The records are the viewed
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
    /* a takeover keeps no V bit: from TK4a nothing views through V, and the recording's map holds
       none */
    unsigned b = s_tk.on ? 16u : (unsigned char)player_rec(ta, s_viewer)[PL_MAPBIT], changed = 0;
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
   floor by a signed compare. The def is the one the unit's create gave it, never a recorded
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

/* R8b's water half (seek_unit_books, THE UNITS' BOOKKEEPING): the image's water state where it
   differs, as the dword 0x43DB50 stores and reads (0x43DB6B, 0x43DBED), bounded by its 0..4 enum;
   past it the host's stays, counted. 1 written */
static int book_water(char* u, const unsigned char* ib, Books* b)
{
    uint32_t host, img;
    memcpy(&host, u + U_WATER, 4);
    memcpy(&img, ib + U_WATER, 4);
    if (host == img) return 0;
    if (img > WATER_MAX) {
        b->refused++;
        return 0;
    }
    memcpy(u + U_WATER, &img, 4);
    b->water++;
    return 1;
}

/* R8b's state half: the image's state byte where the host's differs, through the setter as ev_state
   sets it -- the bits set, then the rest cleared -- so its scripts and replies start as on the
   recording machine. The state gate is open around the two calls alone (tagpu_puppet.c): on a
   multiplayer segment this is the byte's one writer on a puppet. 1 written */
static int book_state(char* u, const unsigned char* ib, Books* b)
{
    if ((unsigned char)u[U_STATEBYTE] == ib[U_STATEBYTE]) return 0;
    tagpu_puppet_state_open(1);
    E_SetState(u, ib[U_STATEBYTE], 1);
    E_SetState(u, (unsigned char)~ib[U_STATEBYTE], 0);
    tagpu_puppet_state_open(0);
    b->states++;
    return 1;
}

/* R8b's stamp half (seek_unit_books has the engine's invariant and the bound): the circle's radius
   index bounded first, then, where the fields differ, the host's stamp removed through 0x482090
   when it made one, the image's three written, and stamped through 0x482270 when they describe
   one, under LosType bit 1; without it the three alone. 1 written */
static int book_stamp(const Lv* L, char* u, const unsigned char* ib, uint16_t los,
                      const uint16_t* circles, unsigned char* rec, Books* b)
{
    if (!(los & LOS_FAN) && (!circles || ib[U_LOSH] >= *circles)) {
        b->refused++;
        return 0;
    }
    if (!memcmp(u + U_LOSXZ, ib + U_LOSXZ, 4) && (unsigned char)u[U_LOSH] == ib[U_LOSH])
        return 0;
    if ((los & LOS_TRUE) && (!(los & LOS_FAN) || u[U_LOSH])) E_SightRemove(u);
    memcpy(u + U_LOSXZ, ib + U_LOSXZ, 4);
    u[U_LOSH] = (char)ib[U_LOSH];
    if ((los & LOS_TRUE) && (!(los & LOS_FAN) || ib[U_LOSH])) {
        sight_rec(L->ta, u, rec);
        E_SightStamp(rec);
    }
    b->stamps++;
    return 1;
}

/* THE UNITS' BOOKKEEPING. masks_init leaves a unit's water state +0x10A and its sight stamp's
   fields +0x7A, +0x7C and +0xF8 to the host's engine, which derives them from the same history as
   the recording machine, except in the step that makes the unit. The recording machine makes a
   unit where its own code does: in the unit tick, or after the players phase, as the scenario
   applier does at 0x4969D2, after the in-play draw, and as the SHARE dialog's give does in a stock
   skirmish (its handler 0x4934B0 -> 0x4933E0 -> GiveUnit 0x488570, whose local create is the
   call of 0x485F50 at 0x488700, from the GUI dispatch 0x4A9FD0 in IdleTick, between two steps;
   DISASSEMBLED). The
   recorder takes every create at the create's own exit, whoever called it, so each such unit is
   made here through make and counted as made. The replay makes every unit of step k in the
   unit tick's stub, after the host's unit tick and before the players phase of step k (0x49555F
   follows 0x4954ED), but the level's own, which the first iteration's capture point makes before
   the unit tick (THE FIRST STEP'S UNITS), and unit_put runs E_Water on it at the next images. So
   a unit the recording machine made after its players phase meets two writers here one step
   early: the players phase emits every live unit's sight every step (0x46503C..0x465063, the
   call at 0x465053), which stores the stamp's fields, and 0x43DB50 can change the water state.
   MEASURED 2026-10-01 in the naval fixture (LosType 4): the end scenario's unit, made in step
   3643, held the stamp (31,29,136) at the capture point of 3644, where the recording holds the
   create's (0,0,0); from 3645 on the two agreed.
   So both are written here as R8b writes them: the water state bounded by its enum, and the stamp
   through the engine's removal 0x482090 and stamp 0x482270, so that the counters keep describing
   the stamps the fields name. This runs after the step's last emit and before the recorder's
   capture and dump and the next unit tick, so the replay is their last writer at every capture
   point; the frame visit's call makes the draw after a step show them too. For any other unit the
   host holds the recording's values and nothing is written. Each write is counted, and one to a
   unit the replay did not make in the step before, or at this capture point's first step, is
   noted (s_made): the host's derivation then differed elsewhere.
   THE INVARIANT is an ordering: at the capture point of k+1 the world holds iteration k+1, the
   host's last emit and its last 0x43DB50 of step k have run (the players phase, the images), the
   write puts the recording machine's values for that point, and nothing between the write and the
   recorder's capture, or the next unit tick, emits or runs 0x43DB50. The counters stay exact by
   R8b's own argument: the host's fields describe the host's stamps, so the removal takes back
   exactly what was stamped.
   ON A MULTIPLAYER SEGMENT the state byte +0x10E is written here too, from W, the composed world:
   its owner's image is the byte's one source there, since the recorder's state events follow the
   recorder's own receipt of each change (mp_owned_event leaves them out) and the state gate
   refuses every other change of a puppet's byte (tagpu_puppet.c). book_state writes it through
   the setter as R8b does, so the scripts and replies a change starts run once, here. The
   invariant is the same ordering: this is the last writer of the byte before the recorder's
   capture and dump at every capture point, and R8b writes the same composed byte at a seek, so a
   forward playing and a seek hold the same byte at every capture point. The bound is the loop's:
   a slot below the level's count whose unit is of the image's type (holds). In a skirmish the
   recorded state events are the byte's writer and nothing is written here. */
static void books_put(const Lv* L, const RecWorld* W)
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
        unsigned char water;
        int16_t was[2];
        unsigned char wasb;
        int made, wrote_w, wrote_s;
        if (!W->live[slot] || !holds(L, slot, img)) continue;
        u = unit_at(L, slot);
        if (s_mp.on) book_state(u, ib, &s_gst.books);
        water = (unsigned char)u[U_WATER];
        memcpy(was, u + U_LOSXZ, 4);
        wasb = (unsigned char)u[U_LOSH];
        wrote_w = book_water(u, ib, &s_gst.books);
        wrote_s = book_stamp(L, u, ib, los, circles, rec, &s_gst.books);
        if (!wrote_w && !wrote_s) continue;
        made = s_made[slot] == W->tick;
        if (made) {
            s_gst.bmade++;
            continue;
        }
        s_gst.bother++;
        if (wrote_w)
            note("replay: GameTime %u: unit %u's water state %u -> %u written from the recording; "
                 "the replay did not make it in the step before", W->tick, slot, water,
                 ib[U_WATER]);
        if (wrote_s)
            note("replay: GameTime %u: unit %u's sight stamp (%d, %d, %u) -> (%d, %d, %u) written "
                 "from the recording; the replay did not make it in the step before", W->tick,
                 slot, was[0], was[1], wasb, *(const int16_t*)(ib + U_LOSXZ),
                 *(const int16_t*)(ib + U_LOSXZ + 2), ib[U_LOSH]);
    }
}

static void tables_put(const Lv* L, const RecWorld* W, TabCounts* c, int seek);

/* the world's declared tables (THE TABLES, tables_put), globals, pool and explored map into the
   host (the head of this section), and the units' bookkeeping after the LosType reaction, whose
   bits 1-2 the stamp's pair reads */
static void recorded_put(const Lv* L, const RecWorld* W)
{
    const RecWorld* C = mp_world(W);    /* each unit's books and the explored map, whichever
                                           stream drives them */
    s_cull_inputs++;                    /* LosType and the units' bits (THE DRAW'S UNIT LIST) */
    masks_init();
    los_react(L->ta, W);
    books_put(L, C);
    tables_put(L, W, &s_gst.tab, 0);
    globals_put(L->ta, W);
    if (s_tk.on) holds_put(L->ta, W);
    proj_put(L, W);
    mapped_put(L->ta, C);
    s_gst.puts++;
}

/* the capture point's write of the iteration the world holds, when it is the host's GameTime's */
static void capture_put(const Lv* L)
{
    const RecWorld* W = recr_world(s_rd);
    if (W->tick != L->now) {
        s_gst.late++;
        s_sl.c.late++;                  /* no plan: every slice stays in the host's tick */
        note("replay: the capture point of GameTime %u holds iteration %u: its globals and "
             "projectiles not written", L->now, W->tick);
        return;
    }
    recorded_put(L, W);
    slice_plan(L, W);                   /* THE SCRIPT SLICES: this step's markers */
}

void __cdecl tagpu_replay_capture(void)
{
    char* ta;
    Lv L;
    if (!tagpu_puppet_on() || !s_rd || s_dead) return;
    ta = ta_main();
    if (!ta) return;
    if (s_first) {
        first_step(ta);                 /* THE FIRST STEP'S UNITS, then this write */
        return;
    }
    if (s_over || s_hold || !level_of(ta, &L)) return;
    capture_put(&L);
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
   The contact bits +0x110 8-10 and 12 hold the viewed seat's scan history (D1). On a puppet
   host only the viewed record's 30-tick pass writes the contact bits; R14b runs that pass and,
   when the host views the recording's viewed record, puts the recorded bits 8-10 back, which
   are what a playing in that view holds at T. The minimap's fog is composited by that pass when
   a sight change has asked for it, by the sight rebuild 0x4816A0, and by sight_fill, so at a
   seek R14 composites it from T's counters. A playing holds its last pass's values until its
   next, so in any other view, and for the fog in every seat's view, the two hosts agree from the
   next pass on, which R15 schedules at the same GameTime on both (O14). In V's view the
   minimap's fog is the same on both hosts at every T: V's counters are all 1 and V's bit is in
   every cell.
   Host-own state that survives, none of it read on a puppet host: B4's per-slot columns (births
   stay strictly increasing per slot), the passability cache, the order inside the seats'
   control-group vectors. The bytes a slot dead at T holds also survive, and a later create starts
   from them where it writes nothing: the water state comes from the create's event instead, and
   contact bit 12 lasts until the next pass. The control group is not among them: the unit init
   calls the setter 0x480250(unit, 0) (0x485D20..0x485D29), so every create starts in group 0.
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
/* THE DRAW'S UNIT LIST. DrawGameScreen walks HotUnits -- the u16 slots at *(main+0x1435F), the
   count at main+0x14367 -- without testing a slot's type +0xA6: its bar loop (0x469C09..0x469D1F)
   calls DrawHealthBars 0x46A430, whose unsigned divide by the def's maxHP +0x1FA at 0x46A4BB has
   no guard, and that faults on a slot the destructor 0x4866D0 emptied, which keeps HP +0x108 and
   the owner while +0x92 becomes the type-0 def *(main+0x1439B), whose maxHP is 0 (MEASURED
   2026-10-09: b7-word-outright's ARMMSTOR slot 2 after a seek past its death). The engine is
   safe by order: its cull 0x48BAE0, which lists only slots with a nonzero +0xA6 (0x48BB3E), runs
   at 0x49697B on every path to the draw call 0x4969CD (DISASSEMBLED). The frame visit runs
   between the two. Outside a step, it is where this file destroys or makes units (seek_run),
   changes the viewed record outside a level's start (seek_run's takeover and rewind,
   sight_change), moves the eye (Follow, tagpu_zoom_follow where no apply runs), and writes what
   the cull's visibility test 0x465AC0 reads -- the viewed record's sight map +0x7C (bounded by
   +0x80/+0x84), LosType main+0x14281 and the units' +0x110 bit 9 (0x465B38..0x465BA3) -- through
   sight_fill, minimap_fog, recorded_put (its LosType reaction and the units' books) and the
   viewed record's radar pass. Each of those writers bumps s_cull_inputs, and a visit that saw it change ends with
   the same cull, so the draw reads a list built after the visit's last write to anything the cull
   reads.
   ORDERING: between that call and the draw's reads lie only the stub's jump to 0x468CF0 and its
   entry observers, which write no unit; the zoom's apply there may move the eye, the one-draw
   skew exe-reverse-engineering.md names for the engine's own cull, and every unit it lists is
   live.
   THE CALL: cdecl, no argument, on the game thread, with the inputs of the engine's own call
   earlier in the same frame -- the unit array and the list buffer, made by 0x4854A0 and freed
   only by the teardown 0x485980; the rect main+0x37E27 and the eye; the viewed record, which a
   takeover or a return to now changes only to a seat or V, both with buffers from level init to
   teardown. Its writes are bounded by its walk of the unit array (BOUNDED, exe-reverse-
   engineering.md, *Who reads the eye, and what bounds each reader*). */
typedef void (__cdecl* PFN_HotUnitsCull)(void);
#define E_HotUnitsCull ((PFN_HotUnitsCull)0x0048BAE0u)
/* 0x466DC0(): the minimap's unit and projectile picture for the viewed record, no argument, plain
   `ret` (0x46743D), which the players phase runs every tick for the viewed record alone
   (0x465065..0x465072). Display state: its count main+0x1436B and main+0x142F1 bit 1 are the
   viewer's own (replay-globals.tsv). */
typedef void (__cdecl* PFN_MinimapRebuild)(void);
#define E_MinimapRebuild ((PFN_MinimapRebuild)0x00466DC0u)
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
    TabCounts tab;
    unsigned poisoned, residue;     /* tables poisoned, bytes the write left at the poison        */
    char     rnames[160];           /* the first tables with residue                              */
    int      pool, effects;
} s_sk;                             /* one seek's counts, for its second line                    */

static unsigned char  s_inU[0x10000];      /* a slot of U: R9 puts it in its recorded bucket     */
static unsigned char  s_named[0x10000];    /* how many members of U name it as their next        */
static unsigned char  s_taken[0x10000];    /* R9's first pass took it out of its bucket          */
static char*          s_ubucket[0x10000];  /* its recorded bucket, resolved                      */
static unsigned short s_uheads[0x10000];   /* the heads of U's chains                            */
static unsigned char  s_fseen[TAGPU_LIM_WRECKS];   /* a wreck record on a list already          */

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
    /* R4's precondition: the expiry 0x482130 removes a record only while its expiry is below
       GameTime (unsigned, cmp/jae at 0x48215C..0x48215E), so at GameTime 0 it removes none -- and
       R3's kills can add the viewed seat's death vision. GameTime is 0 only before the level's
       first step, or after a seek to the recording's first iteration that no step has followed. */
    if (!L->now) {
        snprintf(why, cap, "the level is at GameTime 0: no step has run since the level started "
                           "or since the last seek");
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
            kind = !build_finished(built) ? 0 : carrier ? 1 : 2;
            if (kind != pass) continue;
            memset(rec, 0, sizeof rec);
            rec[0] = 0x0C;
            rec[1] = (unsigned char)slot;
            rec[2] = (unsigned char)(slot >> 8);
            memset(rec + 3, 0xFF, 4);
            if (tagpu_tx_replay_death(rec, 0, 0)) s_sk.killed++;      /* +9 0: no Killed */
        }
    for (slot = 1; slot < L->nslots; slot++)
        if (host_live(L, slot)) s_sk.left++;
}

/* R3b, on a level whose cells are the engine's (s_engine_cells): every cell as the engine's own
   would be with no unit alive. A unit's death clears the footprint it stands in, which on such a
   level is where the engine put it; but the grid may hold recorded slots that name the unit
   somewhere else -- a takeover's (tk_cells), or a segment's whose cells were the recording's
   before the reader crossed (mp_follow) -- and there a death leaves them. So a slot naming no live unit is dropped
   (host_live), and with every unit gone (sk_kills' count) no yard bit stands. R7's creates and
   R8's moves then stamp the composed world on a grid that holds nothing else. */
static void sk_cells_clear(const Lv* L)
{
    unsigned c, k, ncells = L->gw * L->gh;
    for (c = 0; c < ncells; c++) {
        char* cell = L->grid + (size_t)c * REC_CELL_BYTES;
        uint16_t slot[2];
        memcpy(slot, cell, 4);
        for (k = 0; k < 2; k++)
            if (slot[k] && (slot[k] >= L->nslots || !host_live(L, slot[k]))) {
                slot[k] = 0;
                s_sk.fdropped++;
            }
        memcpy(cell, slot, 4);
        if (!s_sk.left) cell[CELL_FLAGS] = (char)((unsigned char)cell[CELL_FLAGS] & ~CELL_YARDBIT);
    }
}

/* R5: LosType bits 0-2 the recording's, then 0x4816A0 always, with 1 when Mapping changed (the
   Mapping command's own call, 0x416DA6) and 0 otherwise. With no unit alive and no temporary
   record left, its fill gives every record that passes its seat test the mode's base and its unit
   loop finds nothing, so the counters after R8 are the base plus the stamps of the units made at
   T at their image positions, and after R8b the base plus exactly the stamps the image's fields
   describe, the recording machine's at T: a function of T alone either way. Its fill stops
   before V's counters, record 10's (0x481707..0x48177A), and with 1 it clears V's bit of the
   explored map (0x4816AF..0x4816FB); R14's sight_fill writes both again. */
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
     R7's creates, which call it from the frame visit already (0x486148). On a multiplayer
     segment the state gate refuses the creates' call, and this write, book_state's, is the
     byte's one change.
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
     can store (0x4825EA..0x4825F2).
   The water, state and stamp halves are book_water, book_state and book_stamp. In playback,
   books_put runs the water and stamp halves at every capture point, and the state half on a
   multiplayer segment (THE UNITS' BOOKKEEPING). Here they run for the seek's own reason, and
   R13's books_put then finds them equal. */
static void seek_unit_books(const Lv* L, const RecWorld* W)
{
    unsigned slot;
    uint16_t los;
    const uint16_t* circles = *(const uint16_t* const*)(L->ta + OFF_CIRCLES);
    unsigned char rec[SIGHT_REC];
    Books b = { 0, 0, 0, 0 };
    memcpy(&los, L->ta + OFF_LOSTYPE, 2);
    for (slot = 1; slot < L->nslots; slot++) {
        const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
        const unsigned char* ib = (const unsigned char*)img;
        char* u;
        if (!W->live[slot] || !holds(L, slot, img)) continue;
        u = unit_at(L, slot);
        book_water(u, ib, &b);
        book_state(u, ib, &b);
        book_stamp(L, u, ib, los, circles, rec, &b);
    }
    s_sk.water += b.water;
    s_sk.states += b.states;
    s_sk.stamps += b.stamps;
    s_sk.brefused += b.refused;
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

/* R10b, a takeover of a segment that holds its cells (THE UNIT SLOTS AND THE YARD BIT): the
   contest's marks, +0x110 bits 26 and 27, from the images, after R10 wrote the recording's slots
   and yard bits. The engine plays on from here, and its writers read both: the clear re-claims
   around a bit-26 holder, the re-claim acts on bit 27 (tagpu_puppet.c, THE FOOTPRINT WRITERS). The
   world is exact here: a takeover starts at a refresh (takeover_point), where the stream's held
   bytes are the writer's own (tagpu_reclossy.c), and a return to now from the saved image of the
   present, a dump, which holds every byte. TK4a's passability refresh then runs over the grid
   whole (tk_live). The bound: a slot live in the image whose host slot holds a live unit of its
   type (holds), as states_put's. */
static void tk_cells(const Lv* L, const RecWorld* W)
{
    unsigned slot;
    for (slot = 1; slot < L->nslots; slot++) {
        const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
        uint32_t st;
        char* u;
        if (!W->live[slot] || !holds(L, slot, img)) continue;
        u = unit_at(L, slot);
        memcpy(&st, u + U_STATE, 4);
        st = (st & ~U_MARKS) | (img[U_STATE / 4] & U_MARKS);
        memcpy(u + U_STATE, &st, 4);
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
     NULL test (0x47CDCA). Where the cells are the engine's (s_engine_cells) the slots and the
     yard bit stay the host's, but in a takeover of a segment that holds its cells (`tk` and
     s_cells_held): there the recording's go in, as on any skirmish segment (tk_cells). */
static void sk_features(const Lv* L, const RecWorld* W, int tk)
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
        unsigned char yard = (unsigned char)cell[CELL_FLAGS] & CELL_YARDBIT;
        uint16_t slot[2], host[2];
        unsigned k;
        memcpy(host, cell, 4);
        memcpy(cell, img, REC_CELL_BYTES);
        cell[CELL_FLAGS] = (char)(((unsigned char)cell[CELL_FLAGS] & ~CELL_DRAWDEFER) | keep);
        if (s_engine_cells && !(tk && s_cells_held)) {
            /* the slots and the yard bit are the engine's own (THE UNIT SLOTS AND THE YARD BIT):
               R3's kills and R3b's clear took every unit off the grid and R7's creates and R8's
               moves put the world's units on it, so the host's are that world's */
            memcpy(cell, host, 4);
            cell[CELL_FLAGS] = (char)(((unsigned char)cell[CELL_FLAGS] & ~CELL_YARDBIT) | yard);
            s_sk.fcells++;
            continue;
        }
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

/* the declaration of this build a recorded table pairs with: one the recorder records
   (tagpu_simreg_recorded -- so no header can name a lock, a trampoline or a TLS index into R12's
   writes), with the same name, stride, class, key, flags and layout hash (tagpu_recfmt.h, RT_TABLE);
   NULL with `why` otherwise */
static const SimDesc* table_pair(const RecTableInfo* t, const char** why)
{
    unsigned i;
    for (i = 0; i < tagpu_simreg_count(); i++) {
        const SimDesc* d = tagpu_simreg_at(i);
        if (!d || strcmp(d->name, t->name)) continue;
        if (!tagpu_simreg_recorded(d)) {
            *why = "this build never records that declaration";
            return NULL;
        }
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

/* the first tables a write changed, comma-separated, as long as they fit */
static void tab_name(TabCounts* c, const char* name)
{
    size_t n = strlen(c->names), k = strlen(name);
    if (strstr(c->names, name) || n + k + 3 >= sizeof c->names) return;
    if (n) memcpy(c->names + n, ", ", 2), n += 2;
    memcpy(c->names + n, name, k + 1);
}

/* the bytes of a row the write leaves as the host holds them (1 in keep): SIMF_LOCAL, SIMF_OWNED,
   SIMF_COLUMN, SIMF_ADDR (written back as an address where its ID resolves), and a SIMF_COUNT in a
   row with SIMF_COLUMN fields; the count of such bytes */
static unsigned table_keep(const SimDesc* d, unsigned char* keep)
{
    const SimField* f;
    unsigned b, n = 0;
    int column = 0;
    for (f = d->layout; f && f->size; f++) column |= f->kind == SIMF_COLUMN;
    for (f = d->layout; f && f->size; f++) {
        unsigned k, r = f->repeat ? f->repeat : 1;
        int left = f->kind == SIMF_LOCAL || f->kind == SIMF_OWNED || f->kind == SIMF_COLUMN ||
                   f->kind == SIMF_ADDR || (f->kind == SIMF_COUNT && column);
        if (!left) continue;
        for (k = 0; k < r; k++) memset(keep + f->off + (size_t)k * f->step, 1, f->size);
    }
    for (b = 0; b < d->stride; b++) n += keep[b];
    return n;
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
/* a count variable the write leaves as the host holds it: never a heap table's (its block is sized
   by it), a live prefix's only within the array */
static int count_kept(const SimDesc* d, const RecTable* t)
{
    const SimDesc* owner = (!(d->flags & SIMD_HEAP) && d->nrows == 1 && !d->rows)
                               ? count_owner(d) : NULL;
    uint32_t v;
    if (!owner) return 0;
    memcpy(&v, t->img, t->size < 4 ? 0 : 4);
    return !owner->nrows || t->size < 4 || v > owner->nrows;
}

static void table_put(const Lv* L, const SimDesc* d, const RecTable* t, TabCounts* c)
{
    unsigned char* base = (d->flags & SIMD_HEAP) ? *(unsigned char**)d->addr
                                                 : (unsigned char*)d->addr;
    unsigned host = !base ? 0 : !d->nrows ? *d->rows * d->stride : d->nrows * d->stride;
    unsigned rows = (t->size < host ? t->size : host) / d->stride, r, b, nkeep = 0, changed = 0;
    unsigned nspan = 0, naddr = 0, *span, *addr;
    unsigned char* keep;
    const SimField* f;
    if (count_kept(d, t)) {
        c->kept++;
        return;
    }
    /* the row's written bytes as runs, [span[2k], span[2k+1]): at most stride/2 + 1 of them; the
       ID-resolved address fields, 4 bytes each, so at most stride/4, as (offset, ID kind); the keep */
    span = (unsigned*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                ((size_t)2 * d->stride + 4) * sizeof(unsigned) + d->stride);
    addr = span ? span + d->stride + 2 : NULL;
    keep = span ? (unsigned char*)(addr + d->stride + 2) : NULL;
    if (!keep) {
        c->skipped++;
        tagpu_logf("replay: %s: the tables skipped %s: no memory", s_name, d->name);
        return;
    }
    nkeep = table_keep(d, keep);
    /* a heap table sized otherwise than the recording's: rows past the shorter one keep the host's
       bytes or are not written, so the difference is counted with the values left */
    if ((d->flags & SIMD_HEAP) && t->size != host) c->kept++;
    for (b = 0; b < d->stride; b++)
        if (!keep[b] && (b == 0 || keep[b - 1])) {
            span[nspan] = b;
            while (b < d->stride && !keep[b]) b++;
            span[nspan + 1] = b;
            nspan += 2;
        }
    for (f = d->layout; f && f->size; f++) {
        unsigned k, n = f->repeat ? f->repeat : 1;
        if (f->kind != SIMF_ADDR || f->id == SIM_ID_NONE || f->size != sizeof(char*)) continue;
        for (k = 0; k < n && naddr < d->stride / 4; k++, naddr++) {
            addr[2 * naddr] = f->off + k * f->step;
            addr[2 * naddr + 1] = f->id;
        }
    }
    /* a table with nothing kept is compared in blocks of whole rows, about 512 bytes each, and row
       by row only where a block differs: most of these are a byte or a dword a unit slot, and
       unchanged from the last tick */
    if (!nkeep) {
        unsigned per = d->stride >= 512 ? 1 : 512 / d->stride;
        for (r = 0; r < rows; r += per) {
            unsigned n = rows - r < per ? rows - r : per, k;
            unsigned char* row = base + (size_t)r * d->stride;
            const unsigned char* img = t->img + (size_t)r * d->stride;
            if (!memcmp(row, img, (size_t)n * d->stride)) continue;
            for (k = 0; k < n; k++, row += d->stride, img += d->stride)
                if (memcmp(row, img, d->stride)) {
                    memcpy(row, img, d->stride);
                    changed++;
                }
        }
    }
    else for (r = 0; r < rows; r++) {
        unsigned char* row = base + (size_t)r * d->stride;
        const unsigned char* img = t->img + (size_t)r * d->stride;
        int differ = 0;
        unsigned k;
        for (k = 0; k < nspan; k += 2)
            if (memcmp(row + span[k], img + span[k], span[k + 1] - span[k])) {
                memcpy(row + span[k], img + span[k], span[k + 1] - span[k]);
                differ = 1;
            }
        for (k = 0; k < naddr; k++) {
            uint32_t id;
            char* p;
            memcpy(&id, img + addr[2 * k], 4);
            if (!addr_of(L, addr[2 * k + 1], id, &p)) {
                c->kept++;
                continue;
            }
            if (memcmp(row + addr[2 * k], &p, 4)) {
                memcpy(row + addr[2 * k], &p, 4);
                differ = 1;
            }
        }
        changed += differ;
    }
    HeapFree(GetProcessHeap(), 0, span);
    c->written++;
    if (changed) {
        c->changed += changed;
        tab_name(c, d->name);
    }
}

/* THE POISON CHECK, for the replay's round trip (`roundtrip <name> at <T>`): does the seek write
   every byte of every table this build records, or does one keep the host's value? R12 copies
   every declaration tagpu_simreg_recorded accepts aside -- not only those the recording pairs --
   and sets every byte its layout does not keep (table_keep), in every host row, to 0xA5; then the
   write; then a byte still 0xA5 where the recording's own byte is not (or, in a table the
   recording does not carry, where the host's was not) is residue: named, counted and put back from
   the copy. A byte the recording itself holds as 0xA5 is never residue, so in a table the
   recording carries the test has no false hit. An ordering: copy, fill, write and check run back
   to back inside R12, under the air pool's lock, on the game thread, with no engine or simulation
   code between them, and every residue byte is put back before the lock is left, so no 0xA5
   outlives R12. Poisoning before R1 would let the kills' and creates' hooks read 0xA5 from their
   own tables. A count that sizes a heap table is never poisoned: table_put bounds that table's
   rows by it, and the write keeps it whole (count_kept), so a poisoned one would only make the
   bound a lie. Every allocation is made before the first byte is poisoned, so no failure path
   leaves one. */
typedef struct {
    const SimDesc*        d;
    unsigned char*        base;
    size_t                bytes;    /* the host's rows when filled                                */
    size_t                at;       /* its copy in s_pz.saved                                     */
    const RecTable*       t;        /* the recording's image, NULL when it carries none          */
    unsigned char*        keep;     /* its row's table_keep, in s_pz.keeps                        */
} Poisoned;
static struct {
    Poisoned*      p;
    unsigned       n;
    unsigned char* saved;
    unsigned char* keeps;
    unsigned       lines;
} s_pz;

/* a count variable that sizes a heap table's block */
static int heap_count(const SimDesc* d)
{
    const SimDesc* owner = (!(d->flags & SIMD_HEAP) && d->nrows == 1 && !d->rows)
                               ? count_owner(d) : NULL;
    return owner && (owner->flags & SIMD_HEAP);
}

static unsigned char* tab_base(const SimDesc* d, size_t* bytes)
{
    unsigned char* base = (d->flags & SIMD_HEAP) ? *(unsigned char**)d->addr
                                                 : (unsigned char*)d->addr;
    *bytes = !base ? 0 : !d->nrows ? (size_t)*d->rows * d->stride : (size_t)d->nrows * d->stride;
    return base;
}

/* the declarations, a copy of their host rows and their keeps; 0 with a line when there is no
   memory */
static int poison_begin(void)
{
    unsigned i, n = tagpu_simreg_count();
    size_t total = 0, at = 0, strides = 0;
    memset(&s_pz, 0, sizeof s_pz);
    s_pz.p = (Poisoned*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (n ? n : 1) * sizeof *s_pz.p);
    if (!s_pz.p) goto none;
    for (i = 0; i < n; i++) {
        const SimDesc* d = tagpu_simreg_at(i);
        Poisoned* q = &s_pz.p[s_pz.n];
        if (!d || !tagpu_simreg_recorded(d) || heap_count(d)) continue;
        q->d = d;
        q->base = tab_base(d, &q->bytes);
        if (!q->base || !q->bytes) continue;
        q->at = total;
        total += q->bytes;
        strides += d->stride;
        s_pz.n++;
    }
    s_pz.saved = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, total ? total : 1);
    s_pz.keeps = (unsigned char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, strides ? strides : 1);
    if (!s_pz.saved || !s_pz.keeps) goto none;
    for (i = 0, strides = 0; i < s_pz.n; i++) {
        Poisoned* q = &s_pz.p[i];
        memcpy(s_pz.saved + at, q->base, q->bytes);
        at += q->bytes;
        q->keep = s_pz.keeps + strides;
        strides += q->d->stride;
        table_keep(q->d, q->keep);
    }
    return 1;
none:
    tagpu_logf("replay: %s: the round trip's poison check skipped: no memory", s_name);
    if (s_pz.saved) HeapFree(GetProcessHeap(), 0, s_pz.saved);
    if (s_pz.keeps) HeapFree(GetProcessHeap(), 0, s_pz.keeps);
    if (s_pz.p) HeapFree(GetProcessHeap(), 0, s_pz.p);
    memset(&s_pz, 0, sizeof s_pz);
    return 0;
}

static void poison_fill(void)
{
    unsigned i;
    size_t r, b;
    for (i = 0; i < s_pz.n; i++) {
        Poisoned* q = &s_pz.p[i];
        for (r = 0; r + q->d->stride <= q->bytes; r += q->d->stride)
            for (b = 0; b < q->d->stride; b++)
                if (!q->keep[b]) q->base[r + b] = 0xA5;
    }
    s_sk.poisoned = s_pz.n;
}

/* the recording's image of a declaration it pairs */
static void poison_image(const SimDesc* d, const RecTable* t)
{
    unsigned i;
    for (i = 0; i < s_pz.n; i++)
        if (s_pz.p[i].d == d) s_pz.p[i].t = t;
}

/* The bytes the write must have covered: those its layout does not keep (table_keep: those were
   never poisoned), in the rows it writes -- a live prefix's rows below its count after the write,
   no more rows than the recording carries -- of a declaration whose value it does not keep whole
   (count_kept). A poisoned byte outside them is put back silently: a dead row past a count, or a
   row the recording does not carry, which the seek's line counts with the values kept. */
static void poison_check(void)
{
    unsigned i;
    for (i = 0; i < s_pz.n; i++) {
        Poisoned* q = &s_pz.p[i];
        const unsigned char* was = s_pz.saved + q->at;
        const unsigned char* keep = q->keep;
        size_t b, run = 0, rows = q->bytes / q->d->stride;
        unsigned found = 0;
        if (q->d->nrows && q->d->rows && *q->d->rows < rows) rows = *q->d->rows;
        if (q->t && q->t->size / q->d->stride < rows) rows = q->t->size / q->d->stride;
        if (q->t && count_kept(q->d, q->t)) rows = 0;
        for (b = 0; b <= q->bytes; b++) {
            int left = 0;
            if (b < q->bytes && !keep[b % q->d->stride] && q->base[b] == 0xA5) {
                if (b / q->d->stride >= rows) {
                    q->base[b] = was[b];
                } else {
                    unsigned char want = q->t ? q->t->img[b] : was[b];
                    left = want != 0xA5;
                }
            }
            if (left) {
                if (!run++) found++;
                q->base[b] = was[b];
                s_sk.residue++;
                continue;
            }
            if (run && s_pz.lines < 16) {
                size_t at = b - run;
                s_pz.lines++;
                tagpu_logf("replay: %s: round trip %u: %s row %u +0x%X: %u bytes not written by the "
                           "seek", s_name, s_seek.T, q->d->name, (unsigned)(at / q->d->stride),
                           (unsigned)(at % q->d->stride), (unsigned)run);
            }
            run = 0;
        }
        if (found) {
            TabCounts n;
            memcpy(n.names, s_sk.rnames, sizeof n.names);
            tab_name(&n, q->d->name);
            memcpy(s_sk.rnames, n.names, sizeof s_sk.rnames);
        }
    }
}

static void poison_end(void)
{
    HeapFree(GetProcessHeap(), 0, s_pz.keeps);
    HeapFree(GetProcessHeap(), 0, s_pz.saved);
    HeapFree(GetProcessHeap(), 0, s_pz.p);
    memset(&s_pz, 0, sizeof s_pz);
}

/* Every table the recording carries into the declaration it pairs with, or skipped (named once a
   playback, or by each seek); then each sim file that owns an invariant its layouts cannot express
   re-establishes it, before anything runs. The air pool's lock is held over the writes
   (tagpu_patches.h). THE TABLES: on a puppet host the recording is the only writer of every table
   it carries at every tick boundary -- at each capture point and each frame visit after a step,
   every recorded table this build pairs holds the recording's values for the iteration the host
   holds, less the fields its layout keeps. Host writers in between (a replayed death's credit, a
   create's birth stamp) are overwritten at the next boundary, and the globals line names the
   tables they wrote. An ordering: inside recorded_put on the game thread, which B2's stub runs
   before the unit tick and the frame visit before the draw. */
static void tables_put(const Lv* L, const RecWorld* W, TabCounts* c, int seek)
{
    const RecHeader* h = recr_header(s_rd);
    unsigned i;
    int poison;
    tagpu_patches_restore_begin();
    poison = seek && s_seek.poison && poison_begin();
    if (poison) poison_fill();
    for (i = 0; i < h->ntab && i < W->ntab; i++) {
        const char* why = NULL;
        const SimDesc* d = table_pair(&h->tab[i], &why);
        if (!d) {
            c->skipped++;
            if (seek || !s_gst.tnamed)
                tagpu_logf("replay: %s: the tables skipped %s: %s", s_name, h->tab[i].name, why);
            continue;
        }
        if (poison) poison_image(d, &W->tab[i]);
        table_put(L, d, &W->tab[i], c);
    }
    s_gst.tnamed = 1;
    if (poison) poison_check();
    c->repaired += tagpu_patches_restored();
    c->repaired += tagpu_weapons_restored();
    if (poison) poison_end();
}

/* R12 */
static void sk_tables(const Lv* L, const RecWorld* W)
{
    tables_put(L, W, &s_sk.tab, 1);
}

/* R15: the deadlines the recording does not carry (or that the step masks), each by the engine's
   own rule, after which the clock is the recording's:
   - each seat's +0xF0, the players phase's 30-tick deadline, from image T: the step masks it
     because image T+1 carries it advanced by step T's phase, but image T's is what the recording
     machine's phase found at step T (0x46507D..0x465092: `if (+0xF0 <= GameTime) +0xF0 += 30`);
   - V's +0xF0, record 10's, the next multiple of 30 above GameTime, the engine's invariant for a
     seat (the player init sets it to GameTime, 0 at the load, 0x464715; the loader's players
     phase 0x497C10 sets 30; each step adds 30 once GameTime reaches it): V's pass runs the radar
     pass and the minimap fog while V is viewed (viewer_frame);
   - every record's DisplayTimer +0xF8 = GameTime, V's too, as the player init sets it (0x464765);
   - the notices and the chat lines through the level init's own resets 0x47EE30 and 0x463C80:
     the creates' queued replies go, and a backward seek leaves no cooldown or line stamped in the
     future.
   The particles' deadlines went with them (R11); the temporary sight records' are made with them
   (R14a); the pacing stamp is R16's. */
static void sk_deadlines(char* ta, const RecWorld* W, unsigned now)
{
    unsigned p, next = PASS_TICKS * (now / PASS_TICKS + 1);
    char* v = player_rec(ta, REC_VIEWER);
    for (p = 0; p < REC_PLAYERS; p++) {
        char* rec = player_rec(ta, p);
        if (tagpu_puppet_seat(p)) memcpy(rec + PL_PASS, (const char*)W->player[p] + PL_PASS, 4);
        memcpy(rec + PL_DISPLAY, &now, 4);
    }
    memcpy(v + PL_PASS, &next, 4);
    memcpy(v + PL_DISPLAY, &now, 4);
    E_NoticeReset();
    E_ChatReset();
}

/* THE SEEK'S SIGHT RECORDS (R14a, R14c). The viewed seat's death vision: the destructor makes a
   temporary sight record for each live unit of the viewed record that dies under true LOS, for
   EYE_LIFE ticks (0x486748), and its stamp is part of the viewed record's counters, which the
   radar pass's sight loop reads (0x467710..0x46782F: a unit over a counter that is not 0 gets
   contact bit 8). R4 drops every record and R5's refill leaves none, so the seek makes again the
   ones a playing holds at T, from the deaths the recording carries, each with the victim's image
   as the destructor found it (RK_EV_DEATH).
   - THE WINDOW. The reader starts at least EYE_LEAD iterations back (recr_seek's lead): two
     lifetimes and the window. eye_visit follows the engine's rules over every iteration it reads:
     an entry for each death in step k of a live unit whose owner's +0x146 is the viewed record,
     under the iteration's LosType bit 1; none past SR_MAX (counted in `capped`); all dropped at a
     change of LosType bits 0-2, as the playing's LosType write drops them; after each step k
     before T-1, those whose expiry is below k, as the expiry after the tick loop (0x495688)
     removes them. A record from before the lead has expired before the window and can change a
     later decision only through the cap, so the entries are a playing's unless the simulated cap
     refused a death, which is counted and logged.
   - THE BOUND. At most SR_MAX entries; the count is 0 after R4; every entry was made under T's
     LosType (a change empties the list), so the add refuses none; the point is bounded by the
     emitter's own grid test (0x48265B, 0x482663), and the circle's by its radius clamp
     (0x482A16..0x482A25), 0x482270's clip and 0x481930's tests (0x4819AB, 0x4819B7); the def
     by addr_of (UNITINFOCount).
   - THE LIFETIME. The add makes each record with its stamp and only the expiry removes both, so
     the counters stay the engine's own. Nothing after R14a rebuilds the viewed record's counters:
     R14 writes V's alone, and V owns no unit.
   - THE ORDER. R14a runs at GameTime T-1 (R6), after R13's LosType and before R14's fog
     composite and R14b's radar pass, as the players phase 0x49555F of step T-1 sees them; R14c,
     the expiry, after R14b, as 0x495688 follows the players phase. */
#define EYE_LIFE 60
#define EYE_LEAD 123
static struct {
    struct { unsigned made; uint32_t type; int32_t pos[3]; } e[SR_MAX];
    unsigned n, T, capped, made, refused;
    uint32_t los;
    int have_los;
    unsigned char viewed;
} s_eye;

static void eye_begin(const char* ta, unsigned T)
{
    memset(&s_eye, 0, sizeof s_eye);
    s_eye.T = T;
    s_eye.viewed = (unsigned char)ta[OFF_VIEWED];
}

static void eye_visit(void* ctx, const RecWorld* w)
{
    const Lv* L = (const Lv*)ctx;
    uint32_t los = w->globals[tagpu_replay_globals[0].dword] & LOS_REACT;
    unsigned i, j, k = w->tick;
    if (s_eye.have_los && los != s_eye.los) s_eye.n = 0;
    s_eye.los = los;
    s_eye.have_los = 1;
    if (k >= s_eye.T) return;
    for (i = 0; i < w->nev; i++) {
        const unsigned char* img = w->ev[i].b + 12;
        uint32_t state, owner, type;
        char* rec;
        if (w->ev[i].kind != RK_EV_DEATH || w->ev[i].len != 12 + REC_UNIT_IMAGE) continue;
        memcpy(&state, img + U_STATE, 4);
        memcpy(&owner, img + U_OWNERREC, 4);
        memcpy(&type, img + U_DEF, 4);
        if (!(state & U_ALIVE) || !(los & LOS_TRUE) || !addr_of(L, SIM_ID_PLAYER, owner, &rec) ||
            !rec || (unsigned char)rec[PL_MAPBIT] != s_eye.viewed)
            continue;
        if (s_eye.n == SR_MAX) {
            s_eye.capped++;
            continue;
        }
        s_eye.e[s_eye.n].made = k;
        s_eye.e[s_eye.n].type = type;
        memcpy(s_eye.e[s_eye.n].pos, img + U_POS, 12);
        s_eye.n++;
    }
    if (k + 1 < s_eye.T) {
        for (i = j = 0; i < s_eye.n; i++)
            if (s_eye.e[i].made + EYE_LIFE >= k) s_eye.e[j++] = s_eye.e[i];
        s_eye.n = j;
    }
}

/* R14a: the window's records, through the engine's add, each with the time it has left */
static void eye_put(const Lv* L, unsigned T)
{
    unsigned i;
    for (i = 0; i < s_eye.n; i++) {
        char* def;
        int before, after;
        memcpy(&before, L->ta + OFF_NSIGHTRECS, 4);
        if (!addr_of(L, SIM_ID_UNIT_TYPE, s_eye.e[i].type, &def) || !def) {
            s_eye.refused++;
            continue;
        }
        E_SightAdd(s_eye.e[i].pos, *(const int16_t*)(def + 0x202), *(const int16_t*)(def + 0x170),
                   (int)(s_eye.e[i].made + EYE_LIFE) - (int)(T - 1));
        memcpy(&after, L->ta + OFF_NSIGHTRECS, 4);
        if (after > before) s_eye.made++;
        else s_eye.refused++;
    }
}

/* Changing the viewed record leaves the units, their per-seat sight counters, the clock and
   the camera intact. Only death vision depends on which record was viewed during playback.
   Read that history with a separate reader, so a refused read cannot displace the next step's
   images or events. No host state changes until the history is whole and the temporary-record
   count is bounded by sight_records_drop. Its expiry removes each old stamp before the new
   viewed record is selected. All of this runs between steps on the game thread, before draw.
   At EOF the host clock can outlive the file: expired deaths are omitted, never restamped.
   The contact bits follow R14b's rule: the new record's radar pass runs once, and when the new
   record is the recording's own viewed record, bits 8-10 are then taken from the image the host
   holds, iteration GameTime+1 -- in that view a playing computes the same pass from the same
   inputs at the same phase (D1), so the recording's bits are a playing's, where the pass alone
   shows the world now instead of the last pass's. The bound is rw_contacts': a slot live in the
   image whose host slot holds a live unit of its type; any other keeps the pass's bits. With no
   whole image of GameTime+1 -- past the recording's end, or after a refused read -- nothing is
   copied. The residuals, display state on a puppet host until the record's next pass: a
   recording whose viewed record changed within the 30 ticks before holds the old record's
   scan; in any other view the pass's bits stand (O14); and in a multiplayer recording a slot
   rw_contacts lets through may carry an earlier unit's contacts (rw_contacts). */
static void rw_contacts(const Lv* L, const RecWorld* W, uint32_t mask);
static void sight_change(char* ta)
{
    unsigned seat = s_viewPending - 1, i, j, T;
    Lv L;
    RecReader* r;
    const RecWorld* W;
    char why[200] = "the selected timeline is unavailable";
    const RecTimeline* timeline;
    s_viewPending = 0;
    /* a recorded seat (tagpu_replay_sight_request), or V's record 10 for the full map */
    if (seat > REC_VIEWER || !level_of(ta, &L) || L.now == UINT32_MAX) return;
    /* Expiry removes stamps with an expiry strictly below GameTime. At zero it can remove
       none; a seek to the first image has no stamps, so that case still permits Sight. */
    if (!L.now && *(const int*)(ta + OFF_NSIGHTRECS) != 0) {
        snprintf(why, sizeof why, "temporary sight records cannot expire at GameTime zero");
        goto refused;
    }
    if ((unsigned char)ta[OFF_VIEWED] == seat) return;
    T = recr_world(s_rd)->tick;
    timeline = recr_selected(s_rd);
    r = recr_open(s_path, why, sizeof why);
    if (!r) goto refused;
    if (!timeline || !recr_timeline(r, timeline->id, why, sizeof why)) {
        recr_close(r);
        goto refused;
    }
    eye_begin(ta, L.now + 1);
    s_eye.viewed = (unsigned char)seat;
    if (recr_seek(r, T, EYE_LEAD, eye_visit, &L, why, sizeof why) != 1) {
        recr_close(r);
        goto refused;
    }
    recr_close(r);
    for (i = j = 0; i < s_eye.n; i++)
        if (s_eye.e[i].made + EYE_LIFE >= L.now) s_eye.e[j++] = s_eye.e[i];
    s_eye.n = j;
    if (sight_records_drop(ta) < 0) {
        snprintf(why, sizeof why, "the temporary sight record count is outside its pool");
        goto refused;
    }
    ta[OFF_VIEWED] = (char)seat;
    s_cull_inputs++;
    eye_put(&L, L.now + 1);
    minimap_fog(ta);
    E_RadarPass();
    W = recr_world(s_rd);
    if (!s_over && W->tick == L.now + 1 && W->view[1] == (unsigned char)seat)
        rw_contacts(&L, W, CONTACT_VIEWED);
    E_SightExpire();
    /* the new record's minimap picture now: on a paused frame no players phase runs, and
       viewer_frame rebuilds record 10's only on a new GameTime */
    E_MinimapRebuild();
    s_view = seat;
    tagpu_logf("replay: sight is seat %u at GameTime %u; death vision %u made, %u refused",
               seat, L.now, s_eye.made, s_eye.refused);
    return;
refused:
    tagpu_logf("replay: sight change to seat %u refused: %s", seat, why);
}

/* The reader back where the host is after a refused seek; when it cannot be, the playing stops
   there, as iteration_read's failure stops it. */
static void sk_back(const Lv* L, unsigned prev)
{
    char why[200];
    if (recr_seek(s_rd, prev, 0, NULL, NULL, why, sizeof why) == 1) {
        mp_follow(L, prev);             /* the materializer follows the reader */
        return;
    }
    s_over = 1;
    tagpu_logf("replay: %s cannot be read back to GameTime %u: %s; the units stop here", s_name,
               prev, why);
    counts_log("a refused seek", L->now);
}

static unsigned sk_ms(LONGLONG ticks, LONGLONG hz)
{
    return hz ? (unsigned)(ticks * 1000 / hz) : 0;
}

/* THE SEEK'S WORK: the game thread's own CPU time, user and kernel, in 100-ns units, from
   GetThreadTimes, reported beside the wall time and checked by nothing. A seek's cost is checked
   on its work instead -- the iterations read, the units made and the blocks decoded
   (recr_seek_work), each against the file (tools/replay/tareplay.py, work_verdict) -- because CPU
   time grows with contention for the core: the same fixed work took 2,510-2,730 ms quiet and
   3,830-3,920 ms with its SMT sibling busy (x1.5, a 7950X), and an all-core load lowers the clock
   further. MEASURED 2026-10-09 under Wine 9.0 (a probe calling it around fixed work): its
   resolution is 10 ms; kernel time is counted (file reads: 110 of 200 ms); a Sleep counts
   nothing; a call costs 15 us. QueryThreadCycleTime returned no usable count there. */
typedef struct { ULONGLONG user, kernel; } SkCpu;

static SkCpu sk_cpu(void)
{
    FILETIME c, e, k, u;
    SkCpu t = {0, 0};
    if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) {
        t.user = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
        t.kernel = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
    }
    return t;
}

/* THE TAKEOVER (seek_run's TAKEOVER mode; research/notes/tadr-port/demo-recorder-s4-takeover.md):
   `takeover <P> at <T>` makes the world of GameTime T as a seek does, with player P human and the
   other recorded seats AI, and the level goes on as a live skirmish from T, recorded as a new
   timeline of the same file. The seek's steps run in their order, the takeover's inserted:
   - R0 bounds what the takeover reads and writes (tk_bound), then TK0 opens the file and finds
     the append point (tk_open), the last act of R0;
   - TK1, after R6's clock and before R7's creates, flips the seats and makes their AI (tk_seats);
   - R8's nodes restart (tk_node), R13 writes the end countdown and flags (holds_put) and no V bit
     (mapped_put), R14 composites P's minimap fog only;
   - TK4a, after R16, makes the level live (tk_live), and TK4b, after the seek's lines, ends the
     playing and arms the recorder (tk_end).
   The recorded deaths' eye is P's from R0 on (s_eye.viewed), since P is the record viewed. */

/* The fork's header carries this build's recorded tables (rec_start: every declaration
   tagpu_simreg_recorded keeps, in registry order), and a chain is played only when each
   segment's tables are the first header's, on the fields recr_timeline compares
   (tagpu_recreader.c, shape_differs). A recording made by a build that declares other tables
   still plays, since the playing pairs tables by name and skips the rest (table_pair), but its
   fork would be refused by every reader, so such a takeover is refused before the rebuild (S4's D3: a fork is always recorded).
   1, or 0 (why). */
static int tk_tables(const RecHeader* h, char* why, unsigned cap)
{
    unsigned i, k = 0;
    for (i = 0; i < tagpu_simreg_count(); i++) {
        const SimDesc* d = tagpu_simreg_at(i);
        const RecTableInfo* t;
        if (!d || !tagpu_simreg_recorded(d)) continue;
        if (k == h->ntab) {
            snprintf(why, cap, "this build records %s, which the recording does not", d->name);
            return 0;
        }
        t = &h->tab[k++];
        if (t->index != i || t->stride != d->stride || t->cls != d->cls || t->key != d->key ||
            t->flags != (unsigned short)d->flags || t->lhash != tagpu_simreg_layout_hash(d) ||
            strncmp(t->name, d->name, sizeof t->name)) {
            snprintf(why, cap, "the recording's table %s is not this build's %s", t->name,
                     d->name);
            return 0;
        }
    }
    if (k != h->ntab) {
        snprintf(why, cap, "the recording records %s, which this build does not", h->tab[k].name);
        return 0;
    }
    return 1;
}

/* R0's bounds on what the takeover reads and writes; `D` from -1 to its default. 1, or 0 (why). */
static int rw_eye(const Lv* L, unsigned T, char* why, unsigned cap);

static int tk_bound(const Lv* L, const RecWorld* W, unsigned P, int* D, int back,
                    char* why, unsigned cap)
{
    const RecHeader* h = recr_header(s_rd);
    char* const* acq = (char* const*)(size_t)ACQ_TABLE;
    char* rec;
    char w2[160];
    unsigned i, seated = 0;
    for (i = 0; i < h->nseats; i++)
        if (h->seats[i].index == P) seated = 1;
    if (!seated || !tagpu_puppet_seat(P)) {
        snprintf(why, cap, "player %u is not one of the recording's seats", P);
        return 0;
    }
    /* THE REFRESH. A takeover from the file starts only at a refresh iteration, where the
       stream's world equals the recording writer's exact copy (tagpu_recwriter.c, iteration_done,
       checked there before every refresh block ends): a bound on what the takeover hands the
       engine, which from TK4a runs every unit's owner code on these values. Between two
       refreshes the stream holds the bytes RT_HOLD names, the weapon slots' targets among them,
       and two owner readers would take a held target as it is: the target resolver 0x48A190
       forms begin + movsx(word 0) * 0x118 with no bound and no alive test (0x48A19F..0x48A1D7),
       and AutoAim's target read 0x48A1E0 tests only the target's +0xA6 (0x48A295) (DISASSEMBLED).
       A held value is one the engine produced for that unit (tagpu_reclossy.c, THE INVARIANTS),
       so its index is a slot of the unit array: the hazard is a target that has died or whose
       slot has been reused since, not an address outside the array. At a refresh the targets
       are the engine's own. The callers round a request down to the refresh before it
       (takeover_point); a return to the saved present (`back`) starts from the live game's own
       frame, not from the file. */
    if (!back) {
        unsigned R = recr_refresh_at(s_rd, W->tick, NULL, w2, sizeof w2);
        if (R != W->tick) {
            if (R) snprintf(why, cap, "GameTime %u is not a refresh iteration: the last at or "
                            "before it is %u", W->tick, R);
            else snprintf(why, cap, "%s", w2);
            return 0;
        }
    }
    /* A takeover needs a playable seat. Returning to the saved present must also
       restore an empty human seat, which remains local after native +kill. */
    if (!back && !*(const uint16_t*)((const char*)W->player[P] + PL_LIVE)) {
        snprintf(why, cap, "player %u has no unit at GameTime %u", P, W->tick);
        return 0;
    }
    /* the values 0x426A66..0x426A9B write */
    if (*D < 0) {
        const RecTimeline* t = recr_selected(s_rd);
        *D = t->has_ai ? (int)(t->difficulty & 0x7FFFFFFFu) : 2;
    }
    if (*D > 2) {
        snprintf(why, cap, "difficulty %d is not 0, 1 or 2", *D);
        return 0;
    }
    for (i = 0; i < h->nseats; i++) {
        unsigned st = h->seats[i].index;
        rec = player_rec(L->ta, st);
        /* a puppet seat's player init skips both (0x46489D..0x4648A5) and nothing makes them
           after, so TK1 overwrites and leaks nothing */
        if (*(void* const*)(rec + PL_AI) || acq[st]) {
            snprintf(why, cap, "record %u holds an AI object already", st);
            return 0;
        }
        if (!*(void* const*)(rec + PL_GROUPS) || !*(void* const*)(rec + PL_SIGHT)) {
            snprintf(why, cap, "record %u's player init has not run", st);
            return 0;
        }
        /* 0x40B320 indexes the ten-entry table with the record's +0x146, and index 10 would alias
           the pathfinder's budget at 0x5119E8. R8's players_put then writes the image's byte over
           it (no row of replay-layout.tsv excludes it), and the teardown frees through
           0x5119C0[+0x146] (0x40B390, called at 0x464A4F). The level's slot setup 0x4854A0 copies
           it into each of the record's unit slots' +0xFF (0x485908..0x48590E), which the radar
           pass's loop 4 indexes the table with (0x40B0E5). So the host's byte and the image's are
           both bounded here. */
        if ((unsigned char)rec[PL_MAPBIT] != st) {
            snprintf(why, cap, "record %u's seat byte is %u", st, (unsigned char)rec[PL_MAPBIT]);
            return 0;
        }
        if (((const unsigned char*)W->player[st])[PL_MAPBIT] != st) {
            snprintf(why, cap, "record %u's seat byte at GameTime %u is %u", st, W->tick,
                     ((const unsigned char*)W->player[st])[PL_MAPBIT]);
            return 0;
        }
    }
    if (!tk_tables(h, w2, sizeof w2)) {
        snprintf(why, cap, "the fork could not be played: %s", w2);
        return 0;
    }
    /* TK4a's 0x464290 zeroes V's +0x74 without freeing it (0x46434A). The table 0x5119C0 has ten
       entries, and record 10's is never written: THE VIEWER'S SEAT's BOUND */
    rec = player_rec(L->ta, s_viewer);
    if (*(void* const*)(rec + PL_AI)) {
        snprintf(why, cap, "the viewer's record holds an AI object");
        return 0;
    }
    if (!*(void* const*)(L->ta + OFF_OBJECTIVE)) {
        snprintf(why, cap, "the level has no objective");
        return 0;
    }
    if (!(s_rw.state == RW_PAST && s_rd == s_rw.now) &&
        !tagpu_rec_fork_ready(w2, sizeof w2)) {
        snprintf(why, cap, "the recorder cannot record the fork: %s", w2);
        return 0;
    }
    return 1;
}

/* THE TAKEOVER'S START, found before anything is committed to it: R, the refresh at or before T
   that tk_bound requires, and the append point after R - 1 that tk_open finds again under the
   file's lock, so a request no takeover can serve is refused while the playing -- or a rewind's
   live original -- still runs. A refresh that is its timeline's first iteration cannot start one:
   the iteration before it is the parent's, or none for the original (recr_append_point). The
   writer makes every segment's second iteration a refresh (tagpu_reclossy.h,
   reclossy_is_refresh), so only a request for a first iteration meets that, and the refusal names
   the second. R with `a` filled, or 0 (why). */
static unsigned takeover_point(unsigned T, RecAppend* a, char* why, unsigned cap)
{
    RecTimeline seg;
    char w2[160];
    unsigned R = recr_refresh_at(s_rd, T, &seg, w2, sizeof w2);
    if (!R) {
        snprintf(why, cap, "%s", w2);
        return 0;
    }
    if (R == seg.first) {
        snprintf(why, cap, "GameTime %u rounds to the refresh %u, timeline %u's first iteration, "
                 "which no takeover starts at; the first GameTime one can is %u", T, R, seg.id,
                 R + 1);
        return 0;
    }
    if (!recr_append_point(s_rd, R - 1, a, w2, sizeof w2)) {
        snprintf(why, cap, "GameTime %u rounds to the refresh %u: %s", T, R, w2);
        return 0;
    }
    return R;
}

/* TK0, the last act of R0. THE ORDERING: the handle is opened first and the append point found
   after. While the handle is held no other writer can open the file -- its share mode is
   FILE_SHARE_READ -- and an earlier one would have made this open fail, so the point and the id
   stay current until the writer uses them (tagpu_recwriter.c, file_append). Every refusal of
   seek_run comes before this, and nothing after it refuses, so the handle reaches TK4b, which
   hands it to the recorder or closes it. 1, or 0 (why) with nothing held. */
static int tk_open(unsigned T, char* why, unsigned cap)
{
    RecAppend a;
    char path[MAX_PATH], w2[160];
    HANDLE f, lock;
    snprintf(path, sizeof path, "replays\\%s.imprec", s_name);
    lock = recfile_lock(path, why, cap);
    if (!lock) return 0;
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        recfile_unlock(&lock);
        snprintf(why, cap, "the recording cannot be written (%lu): another program may have it "
                 "open, or it is read-only", GetLastError());
        return 0;
    }
    /* the append point is found in the file the playing reads, so the handle must be that file,
       not one put at its path since */
    if (!recr_same_file(s_rd, f)) {
        CloseHandle(f);
        recfile_unlock(&lock);
        snprintf(why, cap, "%s is no longer the file this level plays", path);
        return 0;
    }
    if (!recr_append_point(s_rd, T - 1, &a, w2, sizeof w2)) {
        CloseHandle(f);
        recfile_unlock(&lock);
        snprintf(why, cap, "%s", w2);
        return 0;
    }
    s_tk.fork.file = f;
    s_tk.fork.lock = lock;
    s_tk.fork.append_off = a.off;
    s_tk.fork.nstates = a.nstates;
    memcpy(s_tk.fork.states, a.states, a.nstates * sizeof *a.states);
    if (a.evict != UINT32_MAX)
        tagpu_logf("replay: %s: fork would delete timeline %u (least recently used)", s_name,
                   a.evict);
    else tagpu_logf("replay: %s: fork would delete no timeline", s_name);
    s_tk.fork.id = a.next_id;
    s_tk.fork.parent = a.parent;
    s_tk.fork.fork_at = T;
    s_tk.fork.end_open = a.end_open;
    s_tk.fork.open_id = a.open_id;
    s_tk.fork.open_last = a.open_last;
    snprintf(s_tk.fork.path, sizeof s_tk.fork.path, "%s", path);
    s_tk.on = 1;
    return 1;
}

/* TK1, after R6 and before R7: P human, the other recorded seats AI, each with the objects the
   player init makes at a load (its tail, 0x4648A7..0x4648CD), then the AI profiles.
   THE ORDERING. The flip comes after the last kill and before the first create, so every create
   reads a local owner and gets a local movement class: the proxy is chosen only for
   [owner+0] != 0 && +0x73 == 3 (0x43DC48..0x43DC57) -- flip first, 896 of 1,151 units moved;
   rebuild first, 4 of 1,176 (MEASURED, experiment 2). The objects come before any unit exists, as
   the load makes them (0x464990 runs before any create), so 0x40B320 reads a live count of 0, as
   at load (0x409B3B). Both exist for every seat before 0x4648E0, whose Weight reads 0x5119C0[seat]
   untested for every +0x74 set (0x409DC0), and before R14b, whose 0x40B0D0 reads 0x5119C0[owner]
   untested (0x40B0E5..0x40B0EC) for a type-1 or type-2 owner (0x4676A1..0x4676C8). The 8 RNG
   draws of each 0x40B320 are overwritten by R13's RNG from the image, so iteration T's RNG is the
   recording's.
   THE LIFETIME. Both objects are the engine's allocations, stored where the engine stores them,
   and the teardown 0x464A00 frees them (0x40B390, 0x408F10, 0x4B4F20; 0x464A41..0x464A6B). R0
   found both NULL. The difficulty is s_opt's to put back at the level's end (options_restore). */
/* P human and every other recorded seat AI, through the engine's setter: +0x73 and PlayerInfo
   +0x94. TK1 runs it before the creates; R8's players_put then writes each seat's PlayerInfo from
   the image, +0x94 included, and +0x73 not (replay-layout.tsv, `controller`), so it runs again
   after it and the two copies agree, as the setter leaves them in a live game. */
static void tk_types(char* ta, unsigned P)
{
    const RecHeader* h = recr_header(s_rd);
    unsigned i;
    for (i = 0; i < h->nseats; i++)
        E_SetType(player_rec(ta, h->seats[i].index), s_rw.state ?
                  s_rw.types[h->seats[i].index] : h->seats[i].index == P ? 1u : 2u);
}

static void tk_seats(char* ta, unsigned P, unsigned D)
{
    const RecHeader* h = recr_header(s_rd);
    char* const* acq = (char* const*)(size_t)ACQ_TABLE;
    LARGE_INTEGER a, b;
    unsigned i;
    ta[OFF_VIEWED] = (char)P;               /* R14a's add, R14's fog and R14b's pass are P's */
    if (!s_rw.state) memcpy(ta + OFF_DIFFICULTY, &D, 4);
    QueryPerformanceCounter(&a);
    tk_types(ta, P);
    for (i = 0; i < h->nseats; i++) {
        unsigned st = h->seats[i].index;
        char* rec = player_rec(ta, st);
        void* obj = E_MemAlloc(0x3D);
        /* a type-1 or type-2 seat without its AI object cannot go on: 0x406FEE writes through it
           untested */
        if (!obj) {
            tagpu_refuse("Taking over the replay failed: there is no memory for a computer "
                         "player, and the game cannot go on without one. The game has stopped.");
            return;
        }
        *(void**)(rec + PL_AI) = E_AiMake(obj, rec);
        E_AcqMake(st);
        if (!acq[st]) {
            tagpu_refuse("Taking over the replay failed: a computer player's targeting record was "
                         "not made, and the game cannot go on without it. The game has stopped.");
            return;
        }
    }
    E_AiProfiles();
    QueryPerformanceCounter(&b);
    s_tk.t_ai = b.QuadPart - a.QuadPart;
}

/* TK4a, after R16 and before R17: the level made live. Each write's argument:
   - the passability refresh over every context whole: 0x440830 computes the rectangle in 32 bits
     from sign-extended words and clamps it to each context (0x440833..0x440897). Puppets never
     path; the fork's movers do;
   - the controlled and viewed record P: the end tests run only in the controlled record's
     iteration of the players phase (0x464FC7..0x46509D), so P's type 1 and this store are what
     let the level end;
   - V deactivated through the seat activation, index-safe for record 10 (THE VIEWER'S SEAT); it
     writes PlayerInfo +0x94 = 0. V's watcher bit, its buffers and s_vinfo stay, and
     tagpu_replay_level_after puts record 10 and its PlayerInfo back after the teardown, which
     frees the buffers (0x464A00). An active type-0 record
     matches a never-activated one in every later reader but as the viewed or controlled record,
     which the store above has moved away;
   - every unit deselected, 0x48BD00 alone: under a modal screen it defers through 0x491D70(0),
     where the forced 0x491D70(1) would pop that screen without clearing its bits and stall single
     player;
   - the objective armed, the exact inverse of E_Disarm (0x4904B0), which writes only +0x88. The
     objective is made at the level's setup (0x435DA0) and only the next setup frees it, and the
     end tests run only in a step's players phase (0x4650C9, 0x4650E1, 0x46516D, 0x46586F), so
     none sees the half-built world. */
static void tk_live(char* ta, unsigned P)
{
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    E_Passable(0, 0x7FFF7FFFu);
    QueryPerformanceCounter(&b);
    s_tk.t_pass = b.QuadPart - a.QuadPart;
    ta[OFF_CONTROLLED] = ta[OFF_VIEWED] = (char)P;
    E_ActivateSeat(s_viewer, 0);
    E_DeselectAll();
    *(uint32_t*)(*(char**)(ta + OFF_OBJECTIVE) + OBJ_ARMED) = 1;
}

/* TK4b, last: the playing ends and the recorder is armed. THE ORDER: the line is formatted first,
   since the step-down clears the counts; the puppet table goes off, so every gate predicate is
   false, the elimination's included; the step-down follows the seek's lines, which read the world
   through s_rd, and frees the reader -- afterwards every entry point returns at its `!s_rd` test,
   and s_opt and s_vinfo stay for the level's end; the arm comes last (tagpu_recorder.c,
   tagpu_rec_fork_arm). */
static void tk_end(unsigned T, unsigned P, unsigned D, unsigned ms, unsigned read,
                   unsigned rebuild, unsigned built, LONGLONG hz)
{
    const RecHeader* h = recr_header(s_rd);
    char line[700], seats[80], tail[96];
    unsigned i, n = 0, id = s_tk.fork.id, parent = s_tk.fork.parent;
    seats[0] = 0;
    for (i = 0; i < h->nseats && n < sizeof seats; i++)
        n += (unsigned)snprintf(seats + n, sizeof seats - n, "%s%u=%s", i ? " " : "",
                                h->seats[i].index, h->seats[i].index == P ? "human" : "ai");
    snprintf(line, sizeof line, "replay: %s: taken over at GameTime %u as player %u, difficulty "
             "%u, in %u ms (read %u, rebuild %u, AI %u, passability %u); units %u killed, %u "
             "made, %u refused; orders %u built, %u adopted, %u resumed, %u bounded, %u units' "
             "lists torn down; seats %s",
             s_name, T, P, D, ms, read, rebuild, sk_ms(s_tk.t_ai, hz), sk_ms(s_tk.t_pass, hz),
             s_sk.killed, s_sk.made, s_sk.refused, built, s_tk.adopted, s_tk.resumed,
             s_tk.bounded, s_tk.orphans, seats);
    tagpu_puppet_end();
    counts_log("the takeover", T - 1);
    pend_clear();
    replay_close();
    if (tagpu_rec_fork_arm(&s_tk.fork)) {
        snprintf(tail, sizeof tail, "; timeline %u (parent %u) is recorded from GameTime %u", id,
                 parent, T);
    } else {
        CloseHandle(s_tk.fork.file);
        recfile_unlock(&s_tk.fork.lock);
        s_tk.fork.file = NULL;
        tagpu_logf("replay: %s: timeline %u is not recorded: the recorder refused the arm", s_name,
                   id);
        snprintf(tail, sizeof tail, "; timeline %u is not recorded", id);
    }
    tagpu_logf("%s%s", line, tail);
    s_tk.on = 0;
}

/* a refusal of seek_run's own, named as the seek's or the takeover's */
static void sk_refused(int tk, unsigned T, const char* why)
{
    if (tk) tagpu_logf("replay: %s: takeover at %u refused: seek to %u refused: %s", s_name, T, T,
                       why);
    else tagpu_logf("replay: %s: seek to %u refused: %s", s_name, T, why);
}

/* The recorded contact bits `mask` of +0x110, put back from W's images (R14b):
   - return-to-now, CONTACT_SCAN: it keeps the viewed seat's scan history. The engine scans only
     on its 30-tick deadline (DISASSEMBLED 0x46507D..0x465092, call 0x46556D); running 0x467440
     between those deadlines would reveal newly created units early and rewrite +0xB0. The saved
     image holds both, and R15 restores the seated records' deadlines;
   - a seek, a skirmish recording's takeover, or a change of view (sight_change), in the
     recording's own viewed record, CONTACT_VIEWED, after its radar pass.
   W is the primary stream: in a multiplayer recording it is the recording machine's own scan,
   where mp_world's owners' images carry their machines'. The bound, slot by slot: below the
   array's count, live in W's image, and on the host a live unit of the image's type (holds); a
   slot that fails keeps what the host holds, and the next scheduled engine scan owns them all
   again. That is not an identity check. In a skirmish recording W is the world the host's units
   are written from; in a multiplayer one they are written from the composed world, so where W's
   image of a slot still describes an earlier unit of the same type than the one the composed
   world put there, the new unit takes that unit's contacts until the next scan: display state on
   a puppet host. It is never a fork's AI input: a multiplayer takeover does not call this (R14b). */
static void rw_contacts(const Lv* L, const RecWorld* W, uint32_t mask)
{
    unsigned slot;
    for (slot = 1; slot < L->nslots; slot++) {
        const uint32_t* img = W->unit + (size_t)slot * REC_UNIT_DWORDS;
        uint32_t state;
        char* u;
        if (!W->live[slot] || !holds(L, slot, img)) continue;
        u = unit_at(L, slot);
        memcpy(&state, u + U_STATE, 4);
        state = (state & ~mask) | (img[U_STATE / 4] & mask);
        memcpy(u + U_STATE, &state, 4);
    }
}

/* The seek's work, the line after its `wrote` line: what the reader `rd` read to reach the world
   it made (recr_seek_work) -- a return to now reads the live recording to G-1, every other seek its
   own reader to T -- the blocks it decoded to get there, what the image held and what became of
   it, and THE SEEK'S WORK's CPU time beside the wall time. The image's units: V's live slots, and a multiplayer world's units created
   and not yet described, each of which level_units or mp_make_held hands to one create, made or
   refused (make, whose three refusals each count); the kills leave none alive (sk_kills). */
static void sk_work_log(const Lv* L, const RecWorld* V, unsigned T, const RecReader* rd, SkCpu c0,
                        SkCpu c2, unsigned wall)
{
    const RecSeekWork* w = recr_seek_work(rd);
    unsigned slot, live = 0;
    ULONGLONG user = c2.user - c0.user, kernel = c2.kernel - c0.kernel;
    for (slot = 1; slot < L->nslots; slot++) live += V->live[slot] != 0;
    if (s_mp.on)
        for (slot = 1; slot < s_mp.slots; slot++) live += s_mp.held[slot] != 0;
    tagpu_logf("replay: %s: the seek to %u's work: %u iterations read to GameTime %u from %s at "
               "GameTime %u; the blocks: %u decoded (%u for the base), %llu bytes read, %llu "
               "unpacked; the image: %u units live, %u made, %u refused, %u killed, %u left "
               "alive; cpu %u ms (user %u, kernel %u) of the game thread, %u ms wall", s_name, T,
               w->iterations, w->target, w->from_snapshot ? "the snapshot" : "the first iteration",
               w->from, w->blocks, w->base, w->packed, w->raw, live, s_sk.made, s_sk.refused,
               s_sk.killed, s_sk.left,
               (unsigned)((user + kernel) / 10000), (unsigned)(user / 10000),
               (unsigned)(kernel / 10000), wall);
}

/* THE SEEK's steps, R0-R17, in their order, in one of two modes, read once at entry:
   - SEEK (THE SEEK): kills as puppets, makes as puppets, and the level plays on from T;
   - TAKEOVER (THE TAKEOVER): kills as puppets, then the seats turn local and get their AI, the
     creates run as local, and the level stops being a puppet level. */
static int seek_run(char* ta)
{
    RecWorld* W = recr_world(s_rd);
    const RecWorld* V = W;                  /* the world the units are made from: in mode 3, the
                                               composed one (THE SEEK OF A MULTIPLAYER RECORDING) */
    const int back = s_seek.mode == SK_NOW, convert = s_seek.mode == SK_REWIND;
    const int tk = s_seek.mode == SK_TAKEOVER || back;
    unsigned T = s_seek.T, prev = W->tick, h, nheads = 0, refused0, built0, P = s_seek.P;
    int D = s_seek.D;
    LARGE_INTEGER hz, t0, t1, t2;
    SkCpu c0, c2;
    char why[200], dump[sizeof s_seek.dump];
    Lv L;
    int k;
    /* first, before pend_finish plays a held step's creates and deaths: a refused seek re-culls
       too, which the cull's idempotence makes harmless (THE DRAW'S UNIT LIST) */
    s_cull_inputs++;
    slice_disarm();                         /* THE SCRIPT SLICES: a plan is a step's alone */
    s_seek.pending = 0;
    s_seek.mode = SK_SEEK;                  /* consumed: rt_due's seek_run is a SEEK */
    s_seek.boundary = 0;
    snprintf(dump, sizeof dump, "%s", s_seek.dump);
    ov_clicks_clear();                      /* no click from before T is shown after it */
    if (s_mp.level && (convert || back)) {
        sk_refused(tk, T, "a multiplayer recording has no rewind");
        return 0;
    }
    pend_finish();                          /* the step before it, whole (THE PHASES) */
    /* R0: position and validate; nothing of the host is touched before TK0 returns */
    if (!level_of(ta, &L)) {
        sk_refused(tk, T, "the level's unit array or features are not the ones the counts "
                   "describe");
        return 0;
    }
    h = L.now;
    QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&t0);
    c0 = sk_cpu();
    eye_begin(ta, T);
    if (tk) s_eye.viewed = (unsigned char)P;
    k = back ? rw_eye(&L, T, why, sizeof why) :
        recr_seek(s_rd, T, EYE_LEAD, eye_visit, &L, why, sizeof why);
    if (k <= 0) {
        sk_refused(tk, T, why);
        if (k < 0 && !back) sk_back(&L, prev);
        return 0;
    }
    if (!mp_follow(&L, T)) {
        sk_refused(tk, T, s_mp.why);
        return 0;
    }
    V = mp_world(W);
    QueryPerformanceCounter(&t1);
    memset(&s_sk, 0, sizeof s_sk);
    masks_init();
    if (!sk_bound(&L, V, &nheads, why, sizeof why)) {
        sk_refused(tk, T, why);
        if (!back) sk_back(&L, prev);
        return 0;
    }
    if (convert && !rw_prepare(ta, why, sizeof why)) {
        sk_refused(0, T, why);
        return 0;
    }
    if (tk) {
        memset(&s_tk, 0, sizeof s_tk);
        if (!tk_bound(&L, V, P, &D, back, why, sizeof why) ||
            (!back && !tk_open(T, why, sizeof why))) {
            tagpu_logf("replay: %s: takeover at %u refused: %s", s_name, T, why);
            if (!back) sk_back(&L, prev);
            return 0;
        }
        if (back) { s_tk.on = 1; }
    }
    /* R1: the rebuild's kills are not the recording's eliminations */
    tagpu_puppet_rebuild(1);
    /* R2: the projectile pool emptied before the kills: each kill's purge 0x49C880 then scans
       nothing, and its stock miss -- the record that slides into the slot it compacted keeps
       naming the dead unit -- cannot arise. R13 writes the pool whole. */
    proj_empty(&L, *(char**)(ta + OFF_PROJ));
    /* R3, on the clock the host holds: GameTime h >= 1, which R0 enforces (sk_bound) */
    refused0 = s_st.refused;
    sk_kills(&L);
    if (s_engine_cells) sk_cells_clear(&L);
    if (convert) rw_seats(ta);
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
    /* TK1 (THE TAKEOVER, tk_seats) */
    if (tk) tk_seats(ta, P, (unsigned)D);
    /* R7: every unit of iteration T, made as the first step makes them; a refused create is a
       difference the comparison names, never a hazard (make bounds what it passes) */
    s_sk.made = level_units(&L, V, L.now + 1);
    if (s_mp.on) s_sk.made += mp_make_held(&L);
    /* R8: S2b's appliers, the features left to R10 */
    links_put(&L, V);
    units_put(&L, V);
    yards_put(&L, V);
    states_put(&L, V);
    players_put(&L, V);
    if (tk) tk_types(ta, P);
    built0 = s_st.obuilt;
    orders_put(&L, V);
    if (tk) s_tk.orphans = tk_orphans(&L, V);
    s_sk.refused = s_st.refused - refused0;
    /* R8b, after R8's moves, which re-emit */
    seek_unit_books(&L, V);
    /* R9, R10, R11, R12 */
    sk_buckets(&L, V, nheads);
    sk_features(&L, V, tk);
    if (tk && s_cells_held) tk_cells(&L, V);
    s_sk.effects = tagpu_effects_clear();
    sk_tables(&L, V);
    /* R13: the globals, the pool and the explored map, after R7's draws of the simulation RNG and
       R11; the LosType step finds R5's bits and does nothing */
    refused0 = s_gst.refused;
    recorded_put(&L, V);
    s_sk.pool = s_gst.refused == refused0;
    /* R14a: the viewed seat's temporary sight records (THE SEEK'S SIGHT RECORDS) */
    eye_put(&L, T);
    /* R14: V's sight, which R5 undid, and the minimap's fog from the counters at T (sight_fill);
       in a takeover the fog alone, which composites the viewed record, P, from the counters R5-R8b
       made */
    if (tk) minimap_fog(ta);
    else sight_fill(ta, s_viewer);
    /* R14b: the contact bits, +0x110 8-10 and 12 (D1). Return-to-now restores the saved scan
       through rw_contacts. Otherwise the viewed record's radar pass runs once, as its 30-tick
       pass runs it (0x46556D). On a puppet host, after the creates, only that pass writes the
       bits: R7's creates clear bits 8-10 and set bit 9 from the def (0x485B4C,
       0x485C3E..0x485C44), so without it no unit would have a contact; and its first loop
       clears bit 12 on every live unit (0x4674AE), which its fourth sets only for a type-1 or
       type-2 owner (0x4676A1..0x4676AA), of which a puppet host has none.
       The pass alone does not give a playing's bits 8-10, not even at T = P+1. A playing's last
       pass ran in step P = 30*floor((T-1)/30), in the players phase 0x49555F after the unit tick
       0x4954ED: it read iteration P+1's units, but the globals as they stood mid-step, and a
       write after it in step P reaches image P+1 and not that pass. MEASURED 2026-10-09 on the
       tenseat fixture, whose LOS command is typed at GameTime 900: image 901 carries LosType 6
       and the bit 8 a LosType-4 pass set on every enemy unit, which a playing holds until its
       pass at 930 and this pass, run on any image of 901..930, clears on every unit the viewed
       record's sight counters do not cover (19 at 901).
       So in a seek, and in a skirmish recording's takeover, when the host views the recording's
       viewed record (a takeover views P from TK1 on), the recorded bits 8-10 replace the pass's. The invariant: in that
       view a playing computes the same pass from the same inputs at the same phase (D1; R15 keeps
       the phase), so what the recording holds at T is what a playing holds at T -- every
       capture-point dump in that view compares the radar picture strictly and is EQUAL (tarec.py
       compare --puppet), and so is a takeover's first iteration (tarec.py compare-fork; D2 masks
       bits 8-10 only when the views differ). The bound: a slot live in the
       image whose host slot holds a live unit of its type (rw_contacts); any other keeps the
       pass's bits. The residual: a recording whose viewed record changed within the 30 ticks
       before T holds the old record's scan, where a playing holds its own, until the next pass.
       With any other view nothing recorded is the truth: the seek shows the pass's contacts at
       T where a playing shows those of P+1, until the next pass at P+30, which R15 schedules on
       both (O14); display state on a puppet host. In a seat's view, the recording's viewed
       record's included, the minimap fog R14 composited is T's where a playing shows that of
       its last pass until the next (O14). In a takeover the bits are also its AIs' input: each
       local seat's acquisition scan 0x40AA40 puts every non-allied unit with bit 8 on its radar
       list (0x40AB3F..0x40AB5C), and the acquisition records TK1 makes start with +0xED 0
       (0x40918B, 0x4092DC), so each scans in iteration T's players phase (0x40B2D6..0x40B2E5)
       and reads the bits written here, as the recording's scans read them until its next pass.
       THE ORDERING: nothing after this writes bits 8-10 before the capture point of T -- R14c,
       TK4a (0x48BD00 clears bits 4, 6 and 7 only; 0x466DC0 reads) and the tick loop to 0x4954ED:
       one level of callees DISASSEMBLED, the rest MEASURED (2026-10-11, tenseat taken over at
       901 and 915: compare-fork EQUAL, 23 and 25 live units); every [reg+0x110]/[reg+0x111]
       store in the exe that can reach bits 8-10 is the radar pass's visitors, the create
       0x485C3E/0x485C48 or the restore 0x487080, a cross-check that does not see the pass's
       stores through a field pointer. In a takeover as another player the pass's bits stand,
       and the radar lists the fork's AIs build from them at T stand until their next scan at
       T+30. A multiplayer recording's takeover keeps the pass's bits too: rw_contacts' bound
       is a live unit of the image's type, not its identity, and the primary stream's image of
       a slot can lag a same-type reuse the composed world already holds, which a seek shows
       for a few ticks but which would be the fork's AI input.
       The pass's other call, the minimap fog 0x466C20, ran in R14 (sight_fill), and the radar
       pass asks for no other: neither it (0x467440..0x467836) nor its callees -- the unit walk
       0x47E890 with its visitors 0x467840, 0x467960 and 0x467980, 0x40B0D0, the math helpers
       0x4E4400 and 0x4E43D0 -- write main+0x142F1 (DISASSEMBLED; the `or` of bit 1 at 0x46742F,
       just before, is the radar rebuild 0x466DC0's last store, whose ret is at 0x46743D). It
       runs after the last write it reads -- R7's units, R8's positions and alliances, R13's
       LosType and explored map, R14a's records' stamps -- and before R15, which it does not
       read. */
    if (back) {
        rw_contacts(&L, W, CONTACT_SCAN);
    } else {
        E_RadarPass();
        if (W->view[1] == (unsigned char)ta[OFF_VIEWED] && !(tk && s_mp.on))
            rw_contacts(&L, W, CONTACT_VIEWED);
    }
    /* R14c: the records' expiry, as 0x495688 follows the players phase (THE SEEK'S SIGHT
       RECORDS) */
    E_SightExpire();
    /* R14d: the viewed record's minimap picture from the world at T, after the contacts it reads
       (R14b). A playing rebuilds it every tick, in the players phase for a seat and in
       viewer_frame for record 10; a seek on a paused frame runs neither, and a seat's picture
       would show the world before the seek until the next tick. A takeover's waits for TK4a, whose
       deselect removes the selected units' range circles the picture draws (+0x110 bit 4,
       0x466F42..0x467039). */
    if (!tk) E_MinimapRebuild();
    /* R15, from the recorder's images: a deadline is the host's own clock's, which the recorder's
       carries, where an owner's player image carries its own machine's (mp_world, players_put) */
    sk_deadlines(ta, W, T - 1);
    /* R16: inside the frame visit, before the next frame's scheduler 0x495230 sees the stamps: no
       lag, even after a forward seek of 900 ticks or more */
    pace(ta, T - 1);
    /* TK4a (THE TAKEOVER, tk_live) */
    if (tk) {
        tk_live(ta, P);
        E_MinimapRebuild();
    }
    /* R17: the next tick makes GameTime T; at its capture point the world holds iteration T and
       the step plays T's events and reads T+1, the play loop's own invariant */
    tagpu_puppet_rebuild(0);
    s_over = 0;
    s_frame_tick = T - 1;
    /* armed for T, not for the next capture point: a second seek before a step drops it with its
       own line instead of taking it at another GameTime under this seek's name */
    if (dump[0]) tagpu_rec_dump_arm(dump, T);
    QueryPerformanceCounter(&t2);
    c2 = sk_cpu();
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
               "projectile pool %s, %u records; the effects %s; temporary sight records: %u made "
               "again, %u refused, %u deaths past the engine's %u (the records may differ from a "
               "playing's)", s_name, T, s_sk.left, s_sk.moved,
               s_sk.carried, s_sk.outside, s_sk.frecs, s_sk.fcells, s_sk.fdropped, s_sk.freed,
               s_sk.models, s_sk.sdropped, s_sk.water, s_sk.states, s_sk.stamps, s_sk.brefused,
               s_sk.tab.written, s_sk.tab.skipped, s_sk.tab.kept, s_sk.tab.repaired,
               s_sk.pool ? "written" : "REFUSED", W->proj_hw,
               s_sk.effects ? "cleared" : "NOT cleared", s_eye.made, s_eye.refused, s_eye.capped,
               SR_MAX);
    sk_work_log(&L, V, T, back ? s_rw.live : s_rd, c0, c2,
                sk_ms(t2.QuadPart - t0.QuadPart, hz.QuadPart));
    if (s_mp.on) {
        unsigned slot, live = 0, held = 0;
        for (slot = 1; slot < s_mp.slots; slot++) {
            live += s_mp.live[slot];
            held += s_mp.held[slot];
        }
        tagpu_logf("replay: %s: the seek to %u composed the owners' contributions from the "
                   "first (composition %u): %u units described, %u created and not yet "
                   "described, %u envelopes of %u held, %u private roots not played", s_name, T,
                   s_mp.rebuilt, live, held, s_mp.nq, T, s_mp.croots);
    }
    /* TK4b (THE TAKEOVER, tk_end): s_rd is gone afterwards */
    if (back) rw_finish(ta, 1);
    else if (tk)
        tk_end(T, P, (unsigned)D, sk_ms(t2.QuadPart - t0.QuadPart, hz.QuadPart),
               sk_ms(t1.QuadPart - t0.QuadPart, hz.QuadPart),
               sk_ms(t2.QuadPart - t1.QuadPart, hz.QuadPart), s_st.obuilt - built0, hz.QuadPart);
    if (tk && !back && s_rw.state) rw_finish(ta, 0);
    return 1;
}

/* ---- the frame visit ------------------------------------------------------------------------ */

/* Live rewind owns a paused recording until it is closed, restored, or forked. All state
   transitions run at the game-thread frame visit; file completion is polled, never waited on. */
static void rw_dispose(void)
{
    char path[MAX_PATH];
    if (s_rw.now && s_rw.now != s_rd) recr_close(s_rw.now);
    if (s_rw.live && s_rw.live != s_rd) recr_close(s_rw.live);
    if (s_rw.done) CloseHandle(s_rw.done);
    if (s_rw.dump[0]) {
        snprintf(path, sizeof path, "replays\\dump-%s.bin", s_rw.dump);
        DeleteFileA(path);
    }
    memset(&s_rw, 0, sizeof s_rw);
}

static int rw_prepare(char* ta, char* why, unsigned cap)
{
    const RecHeader* h = recr_header(s_rd);
    unsigned i, v = s_rw.viewer;
    unsigned char seats[REC_PLAYERS] = {0}, locals[REC_PLAYERS] = {0};
    char path[MAX_PATH], *rec;
    unsigned char* info;
    const RecSeat* shown = NULL;
    static unsigned serial;
    s_rw.P = (unsigned char)ta[OFF_CONTROLLED];
    s_rw.viewed = (unsigned char)ta[OFF_VIEWED];
    s_rw.G = *(unsigned*)(ta + OFF_GAMETIME);
    if (s_rw.P >= REC_PLAYERS || s_rw.viewed >= REC_PLAYERS ||
        !*(void**)(ta + OFF_OBJECTIVE)) {
        snprintf(why, cap, "the live level has no controlled/viewed seat or objective");
        return 0;
    }
    rec = player_rec(ta, v);
    if (rec[PL_CONTROLLER] || *(void**)(rec + PL_AI) || *(unsigned short*)(rec + PL_LIVE) ||
        !*(void**)(rec + PL_INFO)) {
        snprintf(why, cap, "the spectator seat is occupied");
        return 0;
    }
    /* Record 10 holds no buffers until the level's first rewind takes it, and a later rewind
       reuses the ones that one made; the player init frees old sight counters but no old group
       table (0x464855, 0x4801A1..0x4801B1), so it runs only with both missing */
    if ((!s_vrec.taken && (*(void**)(rec + PL_SIGHT) || *(void**)(rec + PL_GROUPS) ||
                           *(void**)(rec + PL_EC))) ||
        (!*(void**)(rec + PL_SIGHT) && *(void**)(rec + PL_GROUPS))) {
        snprintf(why, cap, "the spectator seat holds buffers the rewind did not make");
        return 0;
    }
    for (i = 0; i < h->nseats; i++) {
        unsigned st = h->seats[i].index;
        char* p = player_rec(ta, st);
        s_rw.types[st] = (unsigned char)p[PL_CONTROLLER];
        if ((p[PL_CONTROLLER] != 1 && p[PL_CONTROLLER] != 2) ||
            (unsigned char)p[PL_MAPBIT] != st) {
            snprintf(why, cap, "seat %u has changed controller or identity", st);
            return 0;
        }
        seats[st] = locals[st] = 1;
        if (st == s_rw.P) shown = &h->seats[i];
    }
    /* seats[P] is set only in the iteration that sets `shown`: past this test, P's header seat */
    if (!seats[s_rw.P] || s_rw.types[s_rw.P] != 1) {
        snprintf(why, cap, "the controlled seat is not a recorded local player");
        return 0;
    }
    /* the contest's rule is written below, after the live writer is suspended, where a failure
       stops the game; whether the sites can take it is decided here, before anything changes,
       so a refusal leaves the live game as it was. Left below: a page write failing -- the NOPs
       going in for a recording that holds its cells, or, for one that does not, a NOP a failed
       restore left being put back (tagpu_puppet.c, contest_write) */
    if (recr_holds_cells(h) && !tagpu_puppet_contest_in()) {
        snprintf(why, cap, "a contest site holds another module's bytes (tagpu_puppet.c, "
                 "THE CONTEST'S RULE)");
        return 0;
    }
    snprintf(s_rw.dump, sizeof s_rw.dump, "rewind-%lu-%u", GetCurrentProcessId(), ++serial);
    if (!tagpu_rec_dump_now(s_rw.dump)) {
        snprintf(why, cap, "the present could not be saved");
        return 0;
    }
    snprintf(path, sizeof path, "replays\\dump-%s.bin", s_rw.dump);
    s_rw.now = recr_dump_world(path, why, cap);
    if (!s_rw.now) return 0;
    if (!tagpu_rec_suspend(1)) {
        recr_close(s_rw.now); s_rw.now = NULL;
        snprintf(why, cap, "the live writer failed");
        return 0;
    }
    s_rw.objective_armed = *(uint32_t*)(*(char**)(ta + OFF_OBJECTIVE) + OBJ_ARMED);
    memcpy(s_rw.messages, ta + 0x12EF, sizeof s_rw.messages);
    memcpy(s_rw.message_at, ta + 0x2A3E, sizeof s_rw.message_at);
    viewer_take(ta);
    info = *(unsigned char**)(rec + PL_INFO);
    if (!s_vinfo.at) {
        s_vinfo.at = info;
        memcpy(s_vinfo.was, info, REC_INFO_BYTES);
    }
    viewer_seat(ta, shown);
    if (!*(void**)(rec + PL_SIGHT)) E_PlayerInit(rec);
    if (!*(void**)(rec + PL_SIGHT) || !*(void**)(rec + PL_GROUPS) || !*(void**)(rec + PL_EC)) {
        tagpu_refuse("Rewinding failed: the spectator's player buffers could not be allocated.");
        return 0;
    }
    /* V represents a freshly loaded playback seat. Its WinLoseTime was zero at that
       load (0x464727); the loader sets bit 4 (0x497BFF), and 0x451090 copies the
       player count into bits 0..3 and main's version bytes into PlayerInfo+A7/A8. */
    *(unsigned*)(rec + 0xF4) = 0;
    info[INFO_FLAGS] = (unsigned char)((info[INFO_FLAGS] & ~0x0Fu) |
        INFO_WATCHER | 0x10u | (*(unsigned short*)(ta + OFF_NPLAYERS) & 0x0Fu));
    info[0xA7] = (unsigned char)ta[1];
    info[0xA8] = (unsigned char)ta[2];
    ta[OFF_CONTROLLED] = (char)v;
    ta[OFF_VIEWED] = (char)s_rw.P;
    s_viewer = v;
    s_view = s_rw.P;
    E_Disarm(*(void**)(ta + OFF_OBJECTIVE));
    /* the live recording's cells as a playing of it keeps them (THE UNIT SLOTS AND THE YARD BIT):
       its contests by its own rule from here, before R3's kills and R7's creates */
    s_cells_held = recr_holds_cells(h);
    s_engine_cells = s_cells_held;
    tagpu_puppet_begin(seats, locals, s_engine_cells, 0);
    if (!tagpu_puppet_play()) {
        tagpu_refuse("Rewinding failed: the replay's contest rule could not be written into the "
                     "game. The game has stopped.");
        return 0;
    }
    s_first = s_dead = s_over = 0;
    s_rw.state = RW_PAST;
    return 1;
}

static void rw_seats(char* ta)
{
    const RecHeader* h = recr_header(s_rd);
    unsigned i;
    if (s_sk.left) {
        tagpu_refuse("Rewinding failed: live units remain after destruction; their AI objects must stay alive.");
        return;
    }
    /* Local destructors need their owner's AI objects. R3 has destroyed every unit before
       any object is freed; no type-3 seat retains an AI pointer for the players phase to run.
       This is the engine teardown's sequence at 0x464A41..0x464A6B, on the same thread. */
    for (i = 0; i < h->nseats; i++) {
        unsigned st = h->seats[i].index;
        char* rec = player_rec(ta, st);
        void* ai = *(void**)(rec + PL_AI);
        E_SetType(rec, 3);
        ((void (__stdcall*)(unsigned))0x40B390u)(st);
        if (ai) {
            ((void (__thiscall*)(void*))0x408F10u)(ai);
            E_MemFree(ai);
            *(void**)(rec + PL_AI) = NULL;
        }
    }
}

static void rw_pending_eye(void* ctx, unsigned kind, const unsigned char* body, unsigned len)
{
    RecEvent ev = {kind, len, body};
    RecWorld w = *recr_world(s_rw.now);
    w.tick = s_rw.G;
    w.ev = &ev;
    w.nev = 1;
    eye_visit(ctx, &w);
}

static int rw_eye(const Lv* L, unsigned T, char* why, unsigned cap)
{
    int k = recr_seek(s_rw.live, s_rw.G - 1, EYE_LEAD, eye_visit, (void*)L, why, cap);
    if (k != 1) return k;
    /* The lead is complete through G-1. Step G belongs to the recorder's unpublished tail,
       whose lifetime suspension retains, and is visited before the saved G+1 image is applied. */
    tagpu_rec_pending_events(rw_pending_eye, (void*)L);
    (void)T;
    return 1;
}

static void rw_finish(char* ta, int back)
{
    unsigned short pause = s_rw.pause;
    unsigned G = s_rw.G;
    DWORD started = s_rw.started;
    char dump[40];
    int dumped;
    memcpy(dump, s_rw.result_dump, sizeof dump);
    if (back) {
        /* The same level owns this objective until teardown. Restore its saved switch
           before any step: +kill may have disabled victory/defeat before rewind. */
        *(uint32_t*)(*(char**)(ta + OFF_OBJECTIVE) + OBJ_ARMED) = s_rw.objective_armed;
        ta[OFF_VIEWED] = (char)s_rw.viewed;
        memcpy(ta + 0x12EF, s_rw.messages, sizeof s_rw.messages);
        memcpy(ta + 0x2A3E, s_rw.message_at, sizeof s_rw.message_at);
        tagpu_puppet_end();
        pend_clear();
        /* s_rd owns the saved image here; dispose closes the distinct live-file reader. */
        s_rw.now = NULL;
        replay_close();
        rw_dispose();
        tagpu_rec_suspend(0);
        s_tk.on = 0;
    } else {
        s_rw.live = NULL;             /* tk_end has closed it */
        rw_dispose();
    }
    dumped = !dump[0] || tagpu_rec_dump_now(dump);
    *(unsigned short*)(ta + OFF_PAUSE) =
        (unsigned short)((*(unsigned short*)(ta + OFF_PAUSE) & ~1u) | pause);
    if (!dumped) tagpu_logf("rewind: refused: operation finished but its frame dump failed");
    else tagpu_logf("rewind: %s at GameTime %u in %lu ms", back ? "returned to now" : "forked",
                   back ? G : *(unsigned*)(ta + OFF_GAMETIME), GetTickCount() - started);
}

static void rw_close_unmoved(char* ta)
{
    unsigned pause = s_rw.pause;
    int dumped = !s_rw.result_dump[0] || tagpu_rec_dump_now(s_rw.result_dump);
    rw_dispose();
    replay_close();
    *(unsigned short*)(ta + OFF_PAUSE) =
        (unsigned short)((*(unsigned short*)(ta + OFF_PAUSE) & ~1u) | pause);
    if (dumped) tagpu_logf("rewind: closed, nothing moved");
    else tagpu_logf("rewind: refused: closed but its frame dump failed");
}

static void rw_poll(void)
{
    char* ta = ta_main();
    char why[200];
    int k;
    Refusal refusal = {0, 0, 0};
    if (!s_rw.state || !ta) return;
    *(unsigned short*)(ta + OFF_PAUSE) |= 1;
    if (s_rw.state == RW_FLUSH) {
        k = tagpu_rec_rewind_flush(s_rw.path, sizeof s_rw.path);
        if (!k) return;
        if (k < 0) { tagpu_logf("rewind: open refused: writer failed"); rw_close_unmoved(ta); return; }
        if (!recfile_play(s_rw.path, s_rw.timeline)) {
            tagpu_logf("rewind: open refused: recording reserved"); rw_close_unmoved(ta); return;
        }
        s_rd = recr_open(s_rw.path, why, sizeof why);
        if (!s_rd || recr_timeline(s_rd, s_rw.timeline, why, sizeof why) != 1 ||
            !validate_header(recr_header(s_rd), why, sizeof why, &s_rw.viewer, &refusal) ||
            !rw_skirmish(recr_header(s_rd), why, sizeof why) ||
            !validate_engine(ta, recr_header(s_rd), why, sizeof why, 0) ||
            !first_checks(ta, recr_header(s_rd), why, sizeof why, &refusal, 0)) {
            tagpu_logf("rewind: open refused: %s", why); rw_close_unmoved(ta); return;
        }
        s_rw.live = s_rd;
        s_tl = s_rw.timeline;
        snprintf(s_path, sizeof s_path, "%s", s_rw.path);
        s_span.first = recr_header(s_rd)->start;
        s_span.last = recr_selected(s_rd)->last;
        s_span.ok = s_span.last > s_span.first;
        if (!s_span.ok) {
            tagpu_logf("rewind: open refused: no complete seek interval yet");
            rw_close_unmoved(ta); return;
        }
        {
            const char* base = strrchr(s_rw.path, '\\');
            size_t n;
            snprintf(s_name, sizeof s_name, "%s", base ? base + 1 : s_rw.path);
            n = strlen(s_name);
            if (n > 7 && !strcmp(s_name + n - 7, ".imprec")) s_name[n - 7] = 0;
        }
        s_rw.state = RW_OPEN;
        if (s_rw.result_dump[0] && !tagpu_rec_dump_now(s_rw.result_dump)) {
            tagpu_logf("rewind: open refused: opened but its frame dump failed");
            return;
        }
        tagpu_logf("rewind: opened at GameTime %u; the file holds %u to %u", s_rw.G,
                   s_span.first, s_span.last);
    } else if (s_rw.state == RW_ENDING && WaitForSingleObject(s_rw.done, 0) == WAIT_OBJECT_0) {
        /* the fork starts at the refresh rw_trigger found and checked before it ended the
           original: the writer only appends, so the blocks to that GameTime are the same */
        s_seek.mode = SK_TAKEOVER;
        s_seek.T = s_rw.R;
        s_seek.P = s_rw.P;
        s_seek.D = *(unsigned*)(ta + OFF_DIFFICULTY);
        if (!seek_run(ta)) {
            tagpu_logf("rewind: fork failed after the original writer closed; playback remains paused");
            s_rw.state = RW_PAST;
        }
    }
}

static int rw_trigger(const char* line)
{
    char* ta = ta_main();
    char why[200], command[80], dump[40] = "";
    char* suffix;
    unsigned T;
    int k;
    if (strncmp(line, "rewind ", 7)) return 0;
    line += 7;
    if (!ta) return 1;
    if (strlen(line) >= sizeof command) {
        tagpu_logf("rewind: refused: command too long"); return 1;
    }
    strcpy(command, line);
    suffix = strstr(command, " dump ");
    if (suffix) {
        *suffix = 0;
        suffix += 6;
        if (!*suffix || strlen(suffix) >= sizeof dump || !name_ok(suffix)) {
            tagpu_logf("rewind: refused: invalid frame dump name"); return 1;
        }
        strcpy(dump, suffix);
    }
    line = command;
    /* An asynchronous operation owns its dump and start time until its acknowledgement. */
    if (s_rw.state == RW_FLUSH || s_rw.state == RW_ENDING) {
        tagpu_logf("rewind: refused: waiting for the writer"); return 1;
    }
    memcpy(s_rw.result_dump, dump, sizeof dump);
    s_rw.started = GetTickCount();
    if (!strcmp(line, "open")) {
        if (!s_rw_pause_in || s_rw.state || s_rd || tagpu_puppet_on() || (ta[OFF_NETWORK] & 1)) {
            tagpu_logf("rewind: open refused: not a live recorded skirmish"); return 1;
        }
        s_rw.pause = *(unsigned short*)(ta + OFF_PAUSE) & 1;
        *(unsigned short*)(ta + OFF_PAUSE) |= 1;
        if (!tagpu_rec_rewind_open(&s_rw.timeline, why, sizeof why)) {
            tagpu_logf("rewind: open refused: %s", why); rw_close_unmoved(ta); return 1;
        }
        s_rw.G = *(unsigned*)(ta + OFF_GAMETIME);
        s_rw.state = RW_FLUSH;
        return 1;
    }
    if (!s_rw.state) { tagpu_logf("rewind: refused: no rewind is open"); return 1; }
    if (!strcmp(line, "close") || !strcmp(line, "now")) {
        if (s_rw.state == RW_OPEN || s_rw.state == RW_FLUSH) rw_close_unmoved(ta);
        else if (s_rw.state == RW_PAST && !s_rw.done) {
            s_rd = s_rw.now;
            s_seek.mode = SK_NOW;
            s_seek.T = s_rw.G + 1;
            s_seek.P = s_rw.P;
            s_seek.D = *(unsigned*)(ta + OFF_DIFFICULTY);
            if (!seek_run(ta)) { s_rd = s_rw.live; tagpu_logf("rewind: return to now refused"); }
        } else tagpu_logf("rewind: refused: the original ended for a fork; retry fork or leave the level");
        return 1;
    }
    if (s_rw.state != RW_OPEN && s_rw.state != RW_PAST) {
        tagpu_logf("rewind: refused: waiting for the writer"); return 1;
    }
    if (!strncmp(line, "seek ", 5) && u32_word(line + 5, &T)) {
        if (T <= s_span.first || T > s_span.last) {
            tagpu_logf("rewind: seek refused: iteration %u is outside %u to %u", T,
                       s_span.first + 1, s_span.last); return 1;
        }
        s_seek.mode = s_rw.state == RW_OPEN ? SK_REWIND : SK_SEEK;
        s_seek.T = T;
        s_seek.dump[0] = 0;
        k = seek_run(ta);
        if (k) s_rw.T = T;
        if (k && dump[0]) k = tagpu_rec_dump_now(dump);
        tagpu_logf("rewind: seek %u %s in %lu ms", T, k ? "complete" : "refused",
                   GetTickCount() - s_rw.started);
    } else if (!strcmp(line, "fork") && s_rw.state == RW_PAST) {
        /* Ending the original cannot be undone, so the fork's start is found and every refusal
           of it taken first (takeover_point), while the original can still go on from now.
           Ending publishes G's pending events. The done event transfers file access only after
           the old writer has closed both its file and S4c lock. Polling keeps the game thread
           responsive while the entire world remains paused. */
        RecAppend a;
        unsigned R = takeover_point(s_rw.T, &a, why, sizeof why);
        if (!R) {
            tagpu_logf("rewind: fork refused: %s", why);
            return 1;
        }
        if (R != s_rw.T)
            tagpu_logf("rewind: a fork asked for at GameTime %u takes over at %u, the refresh at "
                       "or before it", s_rw.T, R);
        s_rw.R = R;
        if (!s_rw.done) s_rw.done = tagpu_rec_rewind_end();
        if (s_rw.done) s_rw.state = RW_ENDING;
        else tagpu_logf("rewind: fork refused: the original recording cannot be ended");
    } else tagpu_logf("rewind: refused: expected open, seek T, now, fork or close");
    return 1;
}

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
   - `dump <name> now`: a frame dump (tagpu_rec_dump_now), taken at once, here, between two steps;
     two of one GameTime saw no step between them.
   - `roundtrip <name> at <T>`, T >= 2, the name 1 to 36 letters, digits, - and _: on a level
     that plays a recording, the round trip at the frame visit after step T-1 (THE ROUND TRIP).
   - `seek <T>` or `seek <T> dump <name>`: on a level that plays a recording, the seek waits for
     the next frame visit after the first step (THE SEEK), and the dump is taken at the capture
     point of T.
   - `hud seek <T>`: the seek the HUD's timeline asks for, through its own entry point
     (tagpu_replay_seek_request), which clamps T to the recording and takes no dump: a test drives
     the HUD's seek at a GameTime of its choosing, which a click on the timeline cannot name
     exactly.
   - `takeover <P> at <T>` or `takeover <P> at <T> difficulty <D>`: on a level that plays a
     recording, the takeover waits for the next frame visit after the first step (THE TAKEOVER);
     it takes over at the refresh at or before T, and says so when that is not T (tk_bound). A
     start no fork can be appended after is refused at once (takeover_point); the checks that
     need the world there are R0's, so their refusals come from seek_run.
   - `follow <seat>`, `jump <seat>`, `follow free`: the HUD's Follow, Jump and Free cam
     (FOLLOW, JUMP AND FREE CAM), for the next frame visit; a refusal is logged at once.
   - `pov`: the next frame visit logs its Follow's answer -- the epoch, the seat, the displayed
     tick, the sample the clock took, the view, the eye and the level -- or that nothing is
     followed.
   - `cover <name>`: the coverage build's functions that ran, at once (tagpu_cover_write); any
     other build refuses it with a line.
   - `turbo on`: k ticks every frame until `turbo off` or the level's end, in a test launch whose
     tagpu_turbo.on gave k (THE TURBO); refused with a line otherwise. `turbo off`: the game's own
     pacing again.
   - `pause at <T>`, T a decimal u32 past the level's GameTime: in a test launch's skirmish level
     outside rewind, the game paused at the frame visit after step T (THE PAUSE AT T); one at a
     time, a second replacing the first. `resume`: the pause bit cleared, as the Pause key clears
     it, when THE PAUSE AT T set it; refused with a line otherwise (pacing_refusal). */
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

/* a decimal u32 at *p, and *p moved past it: 1 read */
static int u32_take(const char** p, unsigned* v)
{
    char num[12];
    unsigned k = 0;
    while ((*p)[k] >= '0' && (*p)[k] <= '9' && k < sizeof num - 1) {
        num[k] = (*p)[k];
        k++;
    }
    num[k] = 0;
    if (!u32_word(num, v)) return 0;
    *p += k;
    return 1;
}

/* `<P> at <T>[ difficulty <D>]`, the whole of `p`: 1 parsed, D -1 when absent */
static int takeover_words(const char* p, unsigned* P, unsigned* T, int* D)
{
    unsigned d;
    *D = -1;
    if (!u32_take(&p, P) || strncmp(p, " at ", 4)) return 0;
    p += 4;
    if (!u32_take(&p, T)) return 0;
    if (!*p) return 1;
    if (strncmp(p, " difficulty ", 12)) return 0;
    p += 12;
    if (!u32_take(&p, &d) || *p || d > 0x7FFFFFFFu) return 0;
    *D = (int)d;
    return 1;
}

static void rt_arm(const char* name, unsigned T);
static void turbo_on(void);
static void turbo_off(void);
static void pace_level_end(void);
static const char* pacing_refusal(const char* ta);
static void pause_at_arm(unsigned T);
static void pause_resume(void);
static void pause_at_visit(void);
static void hold_visit(void);

static int fork_stem(const char* s)
{
    unsigned n = 0;
    if (!*s) return 0;
    for (; *s; s++, n++)
        if ((unsigned char)*s < 32 || strchr(".\\/:\"<>?*|", *s) || n >= 127) return 0;
    return 1;
}

static int forks_trigger(const char* line)
{
    char name[128], out[128], path[MAX_PATH], output[MAX_PATH], request[33] = {0};
    unsigned id, op, i;
    int end = 0;
    if (strncmp(line, "forks ", 6)) return 0;
    line += 6;
    if (*line == '@') {
        if (strlen(line) < 34) return 0;
        for (i = 0; i < 32; i++) if (!strchr("0123456789abcdef", line[i + 1])) return 0;
        memcpy(request, line + 1, 32);
        line += 33;
        if (*line++ != ' ') return 0;
    }
    if (sscanf(line, " \"%127[^\"]\" export %u \"%127[^\"]\"%n", name, &id,
               out, &end) == 3 && !line[end] && fork_stem(name) && fork_stem(out)) {
        snprintf(output, sizeof output, "replays\\%s.imprec", out);
        op = REC_FILE_EXPORT;
    } else if (sscanf(line, "\"%127[^\"]\" delete %u%n", name, &id, &end) == 2 &&
               !line[end] && fork_stem(name)) {
        output[0] = 0;
        op = REC_FILE_DELETE;
    } else return 0;
    snprintf(path, sizeof path, "replays\\%s.imprec", name);
    if (!recfile_request(path, op, id, output, request))
        tagpu_logf("replay: forks %s: refused: file worker unavailable [request=%s]", path,
                   request[0] ? request : "-");
    return 1;
}

static void trigger_read(void)
{
    static DWORD s_looked;
    char line[384], name[40], dump[40];
    unsigned T, P;
    int D;
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
    if (rw_trigger(line) || forks_trigger(line)) return;
    if (sscanf(line, "dump %39s%n", name, &end) == 1) {
        if (!line[end]) {
            tagpu_rec_dump_arm(name, 0);
            return;
        }
        if (!strncmp(line + end, " at ", 4) && u32_word(line + end + 4, &T) && T) {
            tagpu_rec_dump_arm(name, T);
            return;
        }
        if (!strcmp(line + end, " now")) {
            tagpu_rec_dump_now(name);
            return;
        }
    }
    if (!strcmp(line, "chat hide") || !strcmp(line, "chat show")) {
        tagpu_replay_overlay_set(TAGPU_OV_CHAT, line[5] == 's');
        return;
    }
    if (overlay_trigger(line)) return;
    if (sscanf(line, "cover %39s%n", name, &end) == 1 && !line[end]) {
        tagpu_cover_write(name);
        return;
    }
    if (!strcmp(line, "turbo on")) {
        turbo_on();
        return;
    }
    if (!strcmp(line, "turbo off")) {
        turbo_off();
        return;
    }
    if (!strncmp(line, "pause at ", 9) && u32_word(line + 9, &T)) {
        pause_at_arm(T);
        return;
    }
    if (!strcmp(line, "resume")) {
        pause_resume();
        return;
    }
    if (sscanf(line, "ai-activity %39s from %u until %u%n", name, &T, &P, &end) == 3 &&
        !line[end]) {
        tagpu_rec_ai_arm(name, T, P);
        return;
    }
    if (s_rw.state) {
        tagpu_logf("replay: refused: use rewind commands while live rewind owns the level");
        return;
    }
    if (!strcmp(line, "pov")) {
        if (tagpu_replay_live()) s_fl.pov = 1;
        else tagpu_logf("replay: pov refused: this level plays no recording");
        return;
    }
    if (!strcmp(line, "follow free")) {
        if (!tagpu_replay_follow_request(-1, 0))
            tagpu_logf("replay: free cam refused: this level plays no recording");
        return;
    }
    if (!strncmp(line, "follow ", 7) && u32_word(line + 7, &P)) {
        tagpu_replay_follow_request(P > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)P, 0);
        return;
    }
    if (!strncmp(line, "jump ", 5) && u32_word(line + 5, &P)) {
        tagpu_replay_follow_request(P > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)P, 1);
        return;
    }
    if (sscanf(line, "roundtrip %39s%n", name, &end) == 1 && !strncmp(line + end, " at ", 4) &&
        u32_word(line + end + 4, &T)) {
        rt_arm(name, T);
        return;
    }
    if (!strncmp(line, "hud seek ", 9) && u32_word(line + 9, &T)) {
        tagpu_replay_seek_request(T);
        return;
    }
    /* The harness compares the rebuilt frame before a step can alter it. The dump and
       pause belong to this frame visit, so the caller's polling latency cannot move it. */
    if (!strncmp(line, "seek-frame ", 11) &&
        seek_words(line + 11, &T, dump, sizeof dump) && dump[0]) {
        char* ta = ta_main();
        if (!ta || s_rw.state || !tagpu_puppet_on() || !s_rd || s_dead) {
            tagpu_logf("replay: frame seek refused: this level is not ordinary playback");
            return;
        }
        s_seek.pending = 0;
        s_seek.mode = SK_SEEK;
        s_seek.boundary = 0;
        s_seek.T = T;
        s_seek.dump[0] = 0;
        *(unsigned short*)(ta + OFF_PAUSE) |= 1;
        if (!seek_run(ta) || !tagpu_rec_dump_now(dump))
            tagpu_logf("replay: frame seek refused: seek or dump failed");
        return;
    }
    if (!strncmp(line, "seek ", 5) && seek_words(line + 5, &T, dump, sizeof dump)) {
        if (!tagpu_puppet_on() || !s_rd || s_dead) {
            tagpu_logf("replay: seek to %u refused: this level plays no recording", T);
            return;
        }
        s_seek.pending = 1;
        s_seek.mode = SK_SEEK;
        s_seek.boundary = 0;
        s_seek.T = T;
        snprintf(s_seek.dump, sizeof s_seek.dump, "%s", dump);
        return;
    }
    {
        int dry = n >= 8 && !strcmp(line + n - 8, " dry-run");
        if (dry) line[n - 8] = 0;
        if (!strncmp(line, "takeover ", 9) && takeover_words(line + 9, &P, &T, &D)) {
            char why[200];
            unsigned R;
            RecAppend a;
            if (!tagpu_puppet_on() || !s_rd || s_dead) {
                tagpu_logf("replay: takeover refused: this level plays no recording");
                return;
            }
            /* a takeover starts at the refresh at or before the GameTime asked (tk_bound), and
               one that cannot start there is refused before the seek (takeover_point) */
            R = takeover_point(T, &a, why, sizeof why);
            if (!R) {
                tagpu_logf("replay: %s: %s refused: %s", s_name,
                           dry ? "fork prediction" : "takeover", why);
                return;
            }
            if (R != T)
                tagpu_logf("replay: %s: a takeover asked for at GameTime %u takes over at %u, "
                           "the refresh at or before it", s_name, T, R);
            T = R;
            if (dry) {
                if (a.evict == UINT32_MAX)
                    tagpu_logf("replay: %s: fork would delete no timeline", s_name);
                else tagpu_logf("replay: %s: fork would delete timeline %u (least recently used)",
                                s_name, a.evict);
                return;
            }
            s_seek.pending = 1;
            s_seek.mode = SK_TAKEOVER;
            s_seek.boundary = 0;
            s_seek.T = T;
            s_seek.P = P;
            s_seek.D = D;
            s_seek.dump[0] = 0;
            tagpu_logf("replay: %s: a takeover as player %u at GameTime %u waits for the frame visit",
                       s_name, P, T);
            return;
    }
    }
    tagpu_logf("replay: %s: not understood: %s", TRIGGER, line);
}

/* The common game-thread flip observer runs at menus too. Only file-maintenance requests are
   consumed here; seeks and dumps keep the in-play frame visit's ordering. */
void tagpu_replay_files_frame(unsigned frame)
{
    char prefix[6] = {0};
    FILE* f;
    if (frame % 5 || GetFileAttributesA(TRIGGER) == INVALID_FILE_ATTRIBUTES) return;
    f = fopen(TRIGGER, "rb");
    if (!f) return;
    fread(prefix, 1, 5, f);
    fclose(f);
    if (!strcmp(prefix, "forks")) trigger_read();
}

/* THE ROUND TRIP: a playing's state at the frame visit after step T-1 against a rebuild to T in
   place with nothing between them, so a difference is the seek's, by construction. Run right
   after that visit's recorded_put, which made A's tables the recording's (THE TABLES) and so the
   poison check's comparison exact: a frame dump <name>-a; the TRANSIENT declarations put back to
   the values saved at the playing's first frame visit (tagpu_simreg.h); the seek to T with the
   poison check; a frame dump <name>-b; one line. tarec compare-dumps A B must be EQUAL. One the
   reader already passed is dropped with a line. The pacing guard bounds the ordinary step
   batch at each upcoming T-1, so simulation progression cannot skip an armed future target. */
static void rt_arm(const char* name, unsigned T)
{
    size_t i, n = strlen(name);
    if (!tagpu_puppet_on() || !s_rd || s_dead) {
        tagpu_logf("replay: round trip %s refused: this level plays no recording", name);
        return;
    }
    if (!s_rt_pace_in) {
        tagpu_logf("replay: round trip %s refused: the tick-batch guard is unavailable", name);
        return;
    }
    if (!n || n > 36 || T < 2) {
        tagpu_logf("replay: round trip %s at %u refused: a name of 1 to 36 characters and a "
                   "GameTime of 2 or more, please", name, T);
        return;
    }
    for (i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '-' || name[i] == '_')) {
            tagpu_logf("replay: round trip %s refused: the name is not letters, digits, - and _",
                       name);
            return;
        }
    if (s_rt.n == RT_ARMED) {
        tagpu_logf("replay: round trip %s refused: %u are armed already", name, RT_ARMED);
        return;
    }
    memcpy(s_rt.a[s_rt.n].name, name, n + 1);
    s_rt.a[s_rt.n].T = T;
    s_rt.n++;
    tagpu_logf("replay: %s: round trip %s armed for GameTime %u", s_name, name, T);
}

/* the armed round trips at the frame visit whose next iteration is `next` */
static void rt_due(char* ta, unsigned next)
{
    unsigned i, k = 0;
    for (i = 0; i < s_rt.n; i++) {
        char a[48], b[48], tnames[200];
        unsigned T = s_rt.a[i].T, treset;
        LARGE_INTEGER hz, t0, t1;
        int ok;
        if (T > next) {
            s_rt.a[k++] = s_rt.a[i];
            continue;
        }
        if (T < next) {
            tagpu_logf("replay: %s: round trip %s at %u not run: the level is at GameTime %u",
                       s_name, s_rt.a[i].name, T, next - 1);
            continue;
        }
        snprintf(a, sizeof a, "%s-a", s_rt.a[i].name);
        snprintf(b, sizeof b, "%s-b", s_rt.a[i].name);
        tagpu_rec_dump_now(a);
        treset = tagpu_simreg_transient_reset(tnames, sizeof tnames);
        s_seek.T = T;
        s_seek.dump[0] = 0;
        s_seek.poison = 1;
        QueryPerformanceFrequency(&hz);
        QueryPerformanceCounter(&t0);
        ok = seek_run(ta);
        QueryPerformanceCounter(&t1);
        s_seek.poison = 0;
        if (!ok) {
            tagpu_logf("replay: %s: round trip %s at %u: the seek refused (above); no dump B",
                       s_name, s_rt.a[i].name, T);
            continue;
        }
        tagpu_rec_dump_now(b);
        tagpu_logf("replay: %s: round trip %s at %u: TRANSIENT %u bytes reset (%s); %u tables "
                   "poisoned, residue %u (%s); seek %u ms", s_name, s_rt.a[i].name, T, treset,
                   tnames[0] ? tnames : "none", s_sk.poisoned, s_sk.residue,
                   s_sk.rnames[0] ? s_sk.rnames : "none", sk_ms(t1.QuadPart - t0.QuadPart,
                   hz.QuadPart));
    }
    s_rt.n = k;
}

/* THE BOUNDARY: the playing waits for its seek to the next timeline's first iteration, at the
   next frame visit. That iteration replaces the world whole (tagpu_recreader.c), which is what a
   seek to fork_at makes from it, so the seek is exact. While the hold lasts the step applies
   nothing and the recorder's armed dumps wait (tagpu_replay_held). A frame may run several steps,
   so GameTime can pass fork_at before the visit; the seek sets it to fork_at - 1 (R6) whatever the
   host reached, as any backward seek does, so every armed dump's GameTime comes again. No state of
   the hold survives the seek (THE SEEK's invariant). */
static void boundary_wait(void)
{
    s_hold = 1;
    memset(&s_seek, 0, sizeof s_seek);
    s_seek.pending = 1;
    s_seek.mode = SK_SEEK;
    s_seek.boundary = 1;
    s_seek.T = recr_world(s_rd)->tick + 1;
}

int tagpu_replay_held(void)
{
    return s_rd && s_hold;
}

/* The viewer's record's first fill, once a level whose record 10 the level init's call prepared:
   its sight counters all 1 and its bit in every entry of the explored map, with the minimap's fog
   (sight_fill), and its pass due at the next multiple of 30 above GameTime, as the loader's
   players phase leaves a seat's (0x497C10). Called at the frame visit before its first-step
   return, at the first iteration's capture point and at the step, so a level's first draw, which
   can precede its first step (the frame callback 0x496790 runs the steps 0x495490, possibly none,
   before the draw call 0x4969CD), shows the whole map. ORDERING: every caller runs inside
   0x496790, the in-play callback, which runs only once the loading loop has seen the loader's
   last write (bit 1 of main+0x38D75, set at 0x497C5F, tested at 0x498342), so the loader's READY
   is read after it was written. */
static void viewer_init(char* ta)
{
    unsigned now, next;
    if (s_vready || !tagpu_puppet_on() || !s_rd || s_dead ||
        InterlockedCompareExchange(&s_vbuf, VB_READY, VB_READY) != VB_READY)
        return;
    s_vready = 1;
    sight_fill(ta, REC_VIEWER);
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    next = PASS_TICKS * (now / PASS_TICKS + 1);
    memcpy(player_rec(ta, REC_VIEWER) + PL_PASS, &next, 4);
}

/* The players phase's work for the viewed record, while record 10 is viewed: its loop runs over
   records 0-9 (0x4655A8..0x4655AF), so it never reaches V's iteration, where it would rebuild the
   minimap every tick (0x465065..0x465072) and, on the record's 30-tick pass (+0xF0,
   0x46507D..0x465092), run the radar pass and the minimap's fog (0x465565..0x465572). Once a
   GameTime, at the frame visit, it shows what the draw after the frame's last tick would show: the
   rebuild reads the projectile pool, which the step moves after its hook (0x49B720 at 0x495513).
   A paused frame keeps its GameTime and runs nothing, as stock does. Several 30-tick boundaries in
   one frame run the pass once [INFERRED to be equivalent: the pass recomputes the contact bits
   from the present state]. LIFETIME: +0x2A43 names record 10 only while its buffers exist, from
   the level init's call (THE VIEWER'S BUFFERS) or rw_prepare to the teardown, and the teardown
   wrap's entry ends puppet mode (tagpu_replay_level_end) before 0x464A00 frees them. */
static void viewer_frame(void)
{
    char* ta = ta_main();
    char* rec;
    unsigned now, pass;
    if (!ta || !tagpu_puppet_on() || (unsigned char)ta[OFF_VIEWED] != REC_VIEWER) return;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    if (now == s_vframe) return;
    s_vframe = now;
    E_MinimapRebuild();
    rec = player_rec(ta, REC_VIEWER);
    memcpy(&pass, rec + PL_PASS, 4);
    if (pass > now) return;
    pass = PASS_TICKS * (now / PASS_TICKS + 1);
    memcpy(rec + PL_PASS, &pass, 4);
    s_cull_inputs++;                    /* the units' contact bits (THE DRAW'S UNIT LIST) */
    E_RadarPass();
    E_MinimapFog();
}

/* THE FRAME VISIT, before the in-play draw call 0x4969CD, on the game thread, between two sim
   steps and on paused frames too: the trigger; then, on a level that plays a recording and has
   played its first step, a pending seek (THE SEEK) or takeover (THE TAKEOVER), or else, once a
   GameTime, what the draw after a step shows (THE GLOBALS AND THE PROJECTILES). A seek or a
   takeover the trigger puts in place of a boundary's and that is refused leaves the reader at the
   boundary (sk_back), so the hold and its seek are put back. */
static void frame_visit(void)
{
    char* ta;
    unsigned now;
    Lv L;
    const RecWorld* W;
    trigger_read();
    rw_poll();
    if (s_rw.state) return;
    pause_at_visit();
    if (!tagpu_puppet_on() || !s_rd || s_dead || s_first) return;
    ta = ta_main();
    if (!ta) return;
    if (s_seek.pending) {
        const int boundary = s_seek.boundary, held = s_hold, ok = seek_run(ta);
        s_hold = 0;
        if (ok || !s_rd || s_over) return;
        if (boundary) {
            now = *(const unsigned*)(ta + OFF_GAMETIME);
            s_over = 1;
            tagpu_logf("replay: %s stops at GameTime %u: the next timeline's first iteration "
                       "cannot be applied", s_name, now);
            counts_log("the stop", now);
        } else if (held) {
            boundary_wait();
        }
        return;
    }
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    W = recr_world(s_rd);
    if (!s_over && now != s_frame_tick && W->tick == now + 1 && level_of(ta, &L)) {
        s_frame_tick = now;
        recorded_put(&L, W);
        if (!s_rt.tsaved) {
            tagpu_simreg_transient_save();
            s_rt.tsaved = 1;
        }
        if (s_rt.n) rt_due(ta, now + 1);
    }
    /* Sight sees the same LosType and explored map as the draw, including a paused frame or
       the frozen state after EOF. A timeline boundary or takeover still has priority. */
    if (s_rd && s_viewPending && !s_seek.pending && !s_hold) sight_change(ta);
}

/* the frame visit, with the viewer's record's first fill before it and its viewed work and
   Follow's camera after it, whichever way the visit returns, and last the engine's cull when the
   visit wrote anything it reads (THE DRAW'S UNIT LIST) */
static void __cdecl tagpu_replay_frame(void)
{
    char* ta = ta_main();
    const unsigned inputs = s_cull_inputs;
    if (ta) viewer_init(ta);
    frame_visit();
    hold_visit();
    viewer_frame();
    if (ta) {
        overlay_visit(ta);
        follow_visit(ta);
        if (s_cull_inputs != inputs) E_HotUnitsCull();
    }
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
    tagpu_logf("replay: %s: feature calls made at their phase of the step: %u deferred past the "
               "unit tick, %u steps whose store was full", s_name, s_pend.deferred, s_pend.over);
    tagpu_logf("replay: %s: globals and projectiles: written %u times, %u capture points holding "
               "another iteration; the pool refused %u times; the sight mode changed %u times, "
               "%u temporary sight records dropped, %u changes not made; the explored map changed "
               "by %u writes, %u entries in all, refused %u times; the units' bookkeeping: %u water "
               "states, %u state bytes and %u sight stamps written, %u refused; %u units made in "
               "the step before (water and stamps), "
               "%u others; the declared tables: %u rows written over the host's own values (in "
               "%s), %u values kept, %u skipped, %u repaired", s_name, s_gst.puts, s_gst.late,
               s_gst.refused, s_gst.reacts, s_gst.dropped, s_gst.unbounded, s_gst.mputs,
               s_gst.mentries, s_gst.mrefused, s_gst.books.water, s_gst.books.states,
               s_gst.books.stamps,
               s_gst.books.refused, s_gst.bmade, s_gst.bother, s_gst.tab.changed,
               s_gst.tab.names[0] ? s_gst.tab.names : "none", s_gst.tab.kept, s_gst.tab.skipped,
               s_gst.tab.repaired);
    tagpu_logf("replay: %s: script slices: %u planned (%u for units the step creates), %u "
               "deferred by the unit tick, %u given at their marker, %u at a create's end, %u at "
               "the step's end, %u not given (%u not reached, %u not made, %u voided, %u twice), "
               "%u late iterations, %u remote markers left in the unit tick", s_name,
               s_sl.c.planned, s_sl.c.made, s_sl.c.deferred, s_sl.c.given, s_sl.c.atcreate,
               s_sl.c.settled, s_sl.c.notgiven + s_sl.c.unreached + s_sl.c.unmade + s_sl.c.lost +
               s_sl.c.twice, s_sl.c.unreached, s_sl.c.unmade, s_sl.c.lost, s_sl.c.twice,
               s_sl.c.late, s_sl.c.remote);
    tagpu_logf("replay: %s: the owner's queries: %u made, %u not made (no live unit or no script)",
               s_name, s_q.made, s_q.unmade);
    tagpu_puppet_refusals(gates, sizeof gates);
    tagpu_logf("replay: %s: refused on puppets: %s", s_name, gates);
    tagpu_puppet_footprints(gates, sizeof gates);
    tagpu_logf("replay: %s: footprint writes: %s", s_name, gates);
    if (s_mp.on)
        tagpu_logf("replay: %s: owners: %u units made, %u destroyed, %u images, %u private roots "
                   "(%u played), envelope %llu; %u deaths the recorder observed, %u with a corpse "
                   "level; %u corpses made from its stream; %u builds finished and %u unfinished "
                   "by an image; %u text entries, %u lines shown%s%s",
                   s_name, s_mp.made, s_mp.destroyed, s_mp.images, s_mp.roots, s_mp.rplayed,
                   (unsigned long long)s_mp.ordinal, s_mp.observed, s_mp.ocorpse, s_fst.corpses,
                   s_mp.bfinished, s_mp.bunfinished, s_mp.said, s_mp.shown,
                   s_mp.failed ? "; stopped: " : "", s_mp.failed ? s_mp.why : "");
}

/* The next iteration into the world; 0 when there is none, and the playing stops, or when it is
   the next timeline's, and the playing holds for its seek (THE BOUNDARY). */
static int iteration_read(const Lv* L)
{
    char why[200];
    int k = recr_tick(s_rd, why, sizeof why);
    if (k == 1) return 1;
    if (k == RECR_BOUNDARY) {
        boundary_wait();
        tagpu_logf("replay: %s: %s; the seek to it waits for the frame visit", s_name, why);
        return 0;
    }
    s_over = 1;
    if (k == 0) tagpu_logf("replay: %s played to its end at GameTime %u: %s", s_name, L->now, why);
    else tagpu_logf("replay: %s cannot be read on from GameTime %u: %s; the units stop here",
                    s_name, L->now, why);
    s_endlog = 1;
    s_endat = L->now;
    return 0;
}

/* The level and the recording's first iteration at the first step, whose GameTime must be the
   level's: checked before the step logs whether it plays, so that `playing` is a verdict. The
   request's timeline is chosen here, before the first read, which recr_timeline requires; the
   seating before it read only the file's first header, which every segment of a chain matches in
   shape (recr_timeline's bound). */
static int first_iteration(char* ta, Lv* L, char* why, unsigned cap)
{
    char r[200];
    if (!level_of(ta, L)) {
        snprintf(why, cap, "the level's unit array or its features are not the ones the counts "
                 "describe");
        return 0;
    }
    if (!recfile_play(s_path, s_tl)) {
        snprintf(why, cap, "timeline %u is reserved for deletion or its file cannot be opened", s_tl);
        return 0;
    }
    if (recr_timeline(s_rd, s_tl, r, sizeof r) != 1) {
        snprintf(why, cap, "timeline %u: %s", s_tl, r);
        return 0;
    }
    if (recr_tick(s_rd, r, sizeof r) != 1) {
        snprintf(why, cap, "its first iteration cannot be read: %s", r);
        return 0;
    }
    if (recr_world(s_rd)->tick != L->now) {
        snprintf(why, cap, "its first iteration is GameTime %u, the level's first step %u",
                 recr_world(s_rd)->tick, L->now);
        return 0;
    }
    if (s_tl) {
        const RecTimeline* t = recr_selected(s_rd);
        tagpu_logf("replay: %s: playing timeline %u (parent %u, forked at GameTime %u)", s_name,
                   t->id, t->parent, t->fork_at);
    }
    return 1;
}

/* the level's own units, the first image, the wind and V's sight, by THE APPLY above */
static void play_first(const Lv* L)
{
    RecWorld* W = recr_world(s_rd);
    masks_init();
    s_st.made += level_units(L, W, L->now);
    if (recr_header(s_rd)->mode == MODE_NETWORK) {
        if (mp_begin(L, W)) mp_step_image(L, W->tick - 1);
        if (s_mp.failed) {
            s_over = 1;
            return;
        }
    }
    images_put(L, mp_world(W), 0);
    wind_seed(W);
    sight_fill(L->ta, s_viewer);
}

/* the step's events and the next image, by THE APPLY above */
static void play(const Lv* L)
{
    RecWorld* W = recr_world(s_rd);
    masks_init();
    /* The world holds iteration k: its images went in at the step before k, its events are
       step k's. An iteration of an earlier GameTime is played late rather than dropped, since its
       events are the only record of what they made and destroyed. */
    while (W->tick <= L->now) {
        int now = W->tick == L->now;        /* an iteration played late goes in whole */
        if (!now) {
            s_st.late++;
            note("replay: the events of GameTime %u played at GameTime %u", W->tick, L->now);
        }
        events_put(L, W, now);
        ov_clicks_take(W);
        mp_step_events(L, W->tick);
        if (now && s_sl.armed) slice_settle(L->now);   /* before image T+1 */
        s_st.steps++;
        if (s_over || !iteration_read(L)) break;
        mp_step_image(L, W->tick - 1);
        if (s_over) break;
        images_put(L, mp_world(W), now && !s_pend.full);
    }
}

/* THE FIRST STEP'S UNITS, at the capture point of the level's first iteration, which B2's stub
   reaches before it calls the unit tick 0x48AD30. The recording machine's loader made the level's
   own units before its first unit tick, and that tick gave each one a script slice: the unit tick
   is the only caller of DoScriptsNow 0x4B0D60, with dt 1 (`push 1` at 0x48ADE9, the call at
   0x48ADEB), and dt is the only amount by which the runner counts a sleep down
   (0x4B0DEF..0x4B0E01; DISASSEMBLED). Made here, each unit gets that slice from the host's first
   unit tick, at the same GameTime, and every COB thread of a level-start unit keeps the recording
   machine's clock for the whole level. Made after the unit tick, each would run one slice late, and
   a walker's MotionControl would sample its moving and aiming inputs a tick late and choose walk,
   walklegs or stand differently from the first input change that falls inside the lag on.
   MEASURED 2026-10-12 with tools/tacob's VM, fed each commander's recorded inputs, against two
   tenseat recordings: made after the unit tick, the VM gives walk/walklegs/stand 103/3/394 over
   [481, 1411) and 109/0/445 over [301, 1291), the puppets' measured counts; made before it,
   102/6/393 and 108/3/442, the live machines' measured counts, call for call per commander.
   After the creates and image T comes the capture point's write every later capture point makes
   (capture_put, THE GLOBALS AND THE PROJECTILES), so that here too the replay is the last writer
   of the recorded globals before the unit tick reads them. Among them is the simulation RNG
   0x51FC88 (replay-globals.tsv's rngseed), which the loader seeded from QueryPerformanceCounter
   (0x49718C, the seed's call at 0x49719D) and which a slice's COB RAND draws from (0x4B15C9..
   0x4B15EE -> 0x4B6C30; DISASSEMBLED): without the write the first slice of a level-start unit
   would draw from the host's seed. Create's own run-now draws come before the write, inside each
   create, as at a seek, whose R7 creates also precede its globals. The units made here have their
   books written as made at this capture point (level_units' books_at), since the books are image
   T's and no step of the host's has derived them yet.
   Fixture: randprobe (fixtures.json), whose commanders draw from the RNG in their first slice and
   call a script chosen by the draw at a period chosen by it for the whole level. MEASURED
   2026-10-12 over [361, 1111): with this write left out, RandTick0..3 counted 133/258/163/0 live
   against 107/57/114/253 on the puppet; with it, every key equal.
   The rest of the first step stays in tagpu_replay_step, after the unit tick, as for every later
   step: step T's events, then image T+1. */
static void first_step(char* ta)
{
    char why[200], shown[32];
    Lv L;
    Refusal r = { 0, 0, 0 };
    viewer_init(ta);                    /* first_checks tests VB_READY */
    s_first = 0;
    s_opt.played = 1;                   /* THE PLAYER'S SETTINGS: the loader is done */
    if (!first_checks(ta, recr_header(s_rd), why, sizeof why, &r, 1) ||
        !first_iteration(ta, &L, why, sizeof why)) {
        s_dead = 1;
        recfile_leave();
        tagpu_logf("replay: not playing %s: %s; the level stays empty", s_name, why);
        refused(&r, s_path);
        return;
    }
    if (s_view == s_viewer) snprintf(shown, sizeof shown, "full visibility");
    else snprintf(shown, sizeof shown, "the view of record %u", s_view);
    tagpu_logf("replay: playing %s: %u seats, the viewer at record %u, from GameTime %u, %s",
               s_name, recr_header(s_rd)->nseats, s_viewer, recr_header(s_rd)->start, shown);
    /* the feature gates close (THE FEATURES), and the contests follow the recording machine's
       rule before play_first's level_units makes the first unit */
    if (!tagpu_puppet_play()) {
        s_dead = 1;
        recfile_leave();
        tagpu_logf("replay: not playing %s: the contest's rule could not be written "
                   "(tagpu_puppet.c, THE CONTEST'S RULE); the level stays empty", s_name);
        return;
    }
    play_first(&L);
    if (!s_over) capture_put(&L);
}

static void replay_step(void)
{
    char* ta;
    Lv L;
    if (!tagpu_puppet_on() || !s_rd || s_dead || s_first) return;
    ta = ta_main();
    if (!ta) return;
    viewer_init(ta);
    pend_finish();
    if (!s_over && !level_of(ta, &L)) {
        s_over = 1;
        tagpu_logf("replay: %s stops at GameTime %u: the unit array or the features are not the "
                   "ones the level began with", s_name, *(const unsigned*)(ta + OFF_GAMETIME));
        counts_log("the stop", *(const unsigned*)(ta + OFF_GAMETIME));
    }
    if (!s_over && !s_hold) play(&L);       /* THE BOUNDARY: nothing until the seek */
    pace(ta, *(const unsigned*)(ta + OFF_GAMETIME));
}

/* the step, then the end of THE SCRIPT SLICES' plan on every path out of it */
void __cdecl tagpu_replay_step(void)
{
    char* ta = ta_main();
    replay_step();
    slice_finish(ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0);
}

/* ---- S-e: the teardown ---------------------------------------------------------------------- */

void tagpu_replay_level_end(void)
{
    char* ta = ta_main();
    int stale = s_opt.stale;
    slice_disarm();
    pace_level_end();
    rw_dispose();
    if (options_restore(ta) && stale) E_SaveSettings();     /* THE PLAYER'S SETTINGS */
    /* both cases log `replay: <name> at its level's end, GameTime <t>`, the line tacli's
       `replay seek` takes as the end of the level a playback was on */
    if (s_endlog) {                     /* the level ended inside the recording's last step */
        s_endlog = 0;
        counts_log("the recording's end", s_endat);
    }
    if (s_rd && !s_dead && !s_over)
        counts_log("its level's end", ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0);
    else if (s_rd && !s_dead)
        tagpu_logf("replay: %s at its level's end, GameTime %u: the recording had ended before it",
                   s_name, ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0);
    pend_clear();
    if (s_rd && !s_dead) {
        unsigned id = recr_selected(s_rd)->id;
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s", s_path);
        replay_close();
        if (!recfile_request(path, REC_FILE_TOUCH, id, NULL, NULL))
            tagpu_logf("replay: forks %s: touch skipped: file worker unavailable", path);
    } else replay_close();
    tagpu_puppet_end();
    /* the loader has exited (THE VIEWER'S BUFFERS' ORDERING), and from here no frame visit runs
       viewer_frame: the puppet mode is off */
    InterlockedExchange(&s_vbuf, VB_OFF);
    s_vready = 0;
    s_vframe = ~0u;
}

/* ---- THE REPLAY HUD'S REQUESTS (tagpu_hudinput.c) -------------------------------------------
   Every caller is on the game thread: the HUD's clicks from the shield's mouse paths and the
   wndproc TA pumps on that thread, the timeline drag's seek and Leave's pause from the packet's
   fill, and Leave's pause from the DrawGameScreen observer when its question does not open.
   The fields written here are the player's own, which a playback level does not record
   (replay-globals.tsv lists neither the speeds nor the pause word). */

int tagpu_replay_live(void)
{
    return tagpu_puppet_on() && s_rd && !s_dead && !s_first;
}

int tagpu_replay_span(unsigned* first, unsigned* last)
{
    if (!tagpu_puppet_on() || !s_rd || !s_span.ok) return 0;
    if (first) *first = s_span.first;
    if (last) *last = s_span.last;
    return 1;
}

const char* tagpu_replay_path(void)
{
    return tagpu_puppet_on() && s_rd && s_path[0] ? s_path : NULL;
}

unsigned tagpu_replay_timeline(void)
{
    return s_tl;
}

/* THE SEEK, as tacli's `seek` asks for it: the frame visit before 0x4969CD consumes it, paused
   frames included, on the game thread as this is, so a second request before the visit replaces
   the first. T is clamped to [first + 1, last]: the seek writes GameTime T - 1 (seek_run), and the
   reader refuses a T outside the recording before anything changes (recr_seek). */
int tagpu_replay_seek_request(unsigned T)
{
    if (s_rw.state || !tagpu_replay_live() || !s_span.ok) {
        tagpu_logf("replay: a seek to %u from the HUD refused: the level plays no recording", T);
        return 0;
    }
    if (T < s_span.first + 1) T = s_span.first + 1;
    if (T > s_span.last) T = s_span.last;
    s_seek.T = T;
    s_seek.dump[0] = 0;
    s_seek.poison = 0;
    s_seek.pending = 1;
    tagpu_logf("replay: %s: seek to %u asked by the HUD", s_name, T);
    return 1;
}

/* ---- THE SEATS' COLOURS -------------------------------------------------------------------
   The engine's colour sequence lives with the main block, loaded at startup (0x431911), and a
   seat's logo is bounded against its count. A frame is decoded, at most 64x64, into a fixed plane
   and only a palette index escapes. The engine reads the same sequence at 0x467C1E..0x467C24. */
#define OV_GREY      59             /* a player with no colour                                */

static unsigned logo_colour(unsigned logo)
{
    const char* ta = ta_main();
    const unsigned char *seq, *f;
    unsigned w, h;
    unsigned char pix[64 * 64];
    if (!ta) return OV_GREY;
    seq = *(const unsigned char* const*)(ta + 0x148DB);
    if (!seq || logo >= *(const unsigned short*)seq) return OV_GREY;
    f = *(const unsigned char* const*)(seq + 0x28 + logo * 8);
    if (!f) return OV_GREY;
    w = *(const unsigned short*)f; h = *(const unsigned short*)(f + 2);
    if (!w || !h || w > 64 || h > 64 || !tagpu_gaf_decode(f, w, h, pix)) return OV_GREY;
    return pix[(h / 2) * w + w / 2];
}

/* each recorded seat's colour, taken once a playing: the HUD's rows and the overlays' marks */
static const unsigned char* seat_colours(void)
{
    const RecHeader* h;
    unsigned i;
    if (!s_ov.col_ok) {
        h = recr_header(s_rd);
        memset(s_ov.col, OV_GREY, sizeof s_ov.col);
        for (i = 0; i < h->nseats && i < REC_PLAYERS; i++)
            if (h->seats[i].index < TAGPU_PK_OV_PLAYERS)
                s_ov.col[h->seats[i].index] = (unsigned char)logo_colour(h->seats[i].logo);
        s_ov.col_ok = 1;
    }
    return s_ov.col;
}

int tagpu_replay_players(TAGPU_REPLAY_PLAYER* out, int cap)
{
    const RecHeader* h;
    const RecWorld* W;
    const RecWorld* P;
    char* ta = ta_main();
    Lv L;
    unsigned i, slot;
    int rows[REC_PLAYERS], sides[REC_PLAYERS], valid;
    int n = 0;
    if (!out || cap <= 0 || !tagpu_replay_live()) return 0;
    h = recr_header(s_rd);
    W = mp_world(recr_world(s_rd));     /* in mode 3 the composed world the puppets hold */
    P = persp_world();
    valid = ta && level_of(ta, &L);
    for (i = 0; i < REC_PLAYERS; i++) rows[i] = sides[i] = -1;
    for (i = 0; i < h->nseats && i < REC_PLAYERS && n < cap; i++) {
        unsigned seat = h->seats[i].index;
        const char* p;
        TAGPU_REPLAY_PLAYER* dst;
        if (seat >= REC_PLAYERS) continue;
        p = (const char*)W->player[seat];
        dst = &out[n++];
        memset(dst, 0, sizeof *dst);
        rows[seat] = n - 1;
        sides[seat] = h->seats[i].side;
        dst->commander_alive = valid && h->seats[i].side < L.sides && h->seats[i].side < 5 ? 0 : -1;
        dst->seat = seat;
        dst->colour = seat < TAGPU_PK_OV_PLAYERS ? seat_colours()[seat] : OV_GREY;
        /* a seat with a camera sample: a skirmish's recorded, a multiplayer actor's contributed */
        dst->pov = P->npov[seat] != 0;
        lstrcpynA(dst->name, h->seats[i].name, sizeof dst->name);
        /* The same image the puppet applier uses, held at the final image after EOF. The reader
           owns REC_PLAYERS fixed records and only this thread advances it. None of these fields
           is excluded by replay-layout.tsv. The counters are words (0x486996, 0x486E09). */
        dst->units = *(const uint16_t*)(p + 0x144);
        dst->commander_losses = *(const uint16_t*)(p + 0x106);
        memcpy(&dst->energy, p + PL_ENERGY, 4);
        memcpy(&dst->energy_in, p + 0x90, 4);
        memcpy(&dst->energy_out, p + 0x94, 4);
        memcpy(&dst->metal, p + PL_METAL, 4);
        memcpy(&dst->metal_in, p + 0x9C, 4);
        memcpy(&dst->metal_out, p + 0xA0, 4);
        memcpy(&dst->energy_max, p + 0xA4, 4);
        memcpy(&dst->metal_max, p + 0xA8, 4);
        /* the alliance row, one byte per player record (+0x108, 11 of them: the seat fill
           0x47A890 writes it, the allied victory 0x490080 reads it) */
        {
            unsigned j;
            for (j = 0; j < REC_PLAYERS; j++)
                if (p[0x108 + j]) dst->allies |= 1u << j;
        }
    }
    /* The engine's commander test compares UnitName with the owning side's commander name
       (0x4864CB..0x48650F). The loader stores a 32-byte name (0x431B6D); the table has five
       0x232-byte records, bounded by its free loop (0x431A20). Recorded slot, owner and def IDs
       are data: every array index is bounded before use, and addr_of bounds the def ID by
       UNITINFOCount. The same recorded unit image drives the puppet world and this status. */
    for (slot = 1; valid && slot < W->slots; slot++) {
        unsigned owner;
        int row;
        char* def;
        if (!W->live[slot] || !(img_u32(W, slot, U_STATE) & U_ALIVE)) continue;
        owner = img_u32(W, slot, U_OWNERREC) - 1;
        if (owner >= REC_PLAYERS || (row = rows[owner]) < 0 || out[row].commander_alive < 0) continue;
        if (!addr_of(&L, SIM_ID_UNIT_TYPE, img_u32(W, slot, U_DEF), &def) || !def) {
            out[row].commander_alive = -1;
            continue;
        }
        if (!_strnicmp(def + 0x20, ta + 0x37F5F + (size_t)sides[owner] * 0x232, 0x20))
            out[row].commander_alive = 1;
    }
    return n;
}

/* The defs are the level's, which a load makes and only the teardown frees, and their count the
   engine's own, UNITINFOCount. A type's metal cost is the float def+0x18A (0x438650 takes
   max(+0x18A, 10.0) as one; 0x402640 credits it times one less the build left); it moves when
   def+0x22F is 1, the test 0x4862CF gives a unit its mover by; and def+0x245 bit 0x40000 is the
   FBI `commander` tag. */
unsigned tagpu_replay_army_values(float* out, unsigned cap)
{
    const char* ta = ta_main();
    const char* defs;
    unsigned n, i;
    if (!ta || !tagpu_replay_live()) return 0;
    defs = *(const char* const*)(ta + OFF_UDEFS);
    n = *(const unsigned*)(ta + OFF_UCOUNT);
    if (!defs) return 0;
    if (!out) return n;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) {
        const char* d = defs + (size_t)i * DEF_STRIDE;
        float cost;
        uint32_t tags;
        memcpy(&cost, d + 0x18A, 4);
        memcpy(&tags, d + 0x245, 4);
        out[i] = d[0x22F] == 1 && !(tags & 0x40000u) ? cost : 0.0f;
    }
    return n;
}

int tagpu_replay_sight(void)
{
    const char* ta = ta_main();
    unsigned p;
    if (!ta || !tagpu_replay_live()) return -1;
    p = (unsigned char)ta[OFF_VIEWED];
    return p == s_viewer ? -1 : (int)p;
}

int tagpu_replay_sight_request(int seat)
{
    const RecHeader* h;
    unsigned i;
    if (!tagpu_replay_live() || s_seek.pending || s_hold) return 0;
    h = recr_header(s_rd);
    if (seat == -1) {
        s_viewPending = s_viewer + 1;
        return 1;
    }
    for (i = 0; i < h->nseats && i < REC_PLAYERS; i++)
        if (seat >= 0 && seat < REC_PLAYERS && h->seats[i].index == (unsigned)seat) {
            s_viewPending = (unsigned)seat + 1;
            return 1;
        }
    return 0;
}

/* ---- FOLLOW, JUMP AND FREE CAM (research/notes/tadr-port/demo-recorder-perspective-streams.md,
   *Playback*) ---------------------------------------------------------------------------------
   A Follow shows a recorded seat's camera: at every frame visit the perspective clock picks or
   interpolates the seat's sample for the displayed GameTime, W->tick (tagpu_persp.h), Follow's fit
   puts it on this screen, and tagpu_zoom_follow hands the eye and the level to the apply at the
   top of the draw that follows, which writes the camera on the thread that owns it. A Jump is the
   same for one epoch that tagpu_zoom ends once the render thread has drawn its level; Free cam
   ends the epoch. tagpu_zoom ends a Follow on its own when the camera is moved under it or the
   wheel is turned, and the next visit logs why. Each start and each end is one log line.
   A seek leaves the camera alone and a Follow goes on from the new GameTime; the end of the
   recording holds its last samples; a takeover, the level's end and any other end of the playing
   end the epoch (follow_close). A multiplayer recording's perspectives are its contributions,
   composed at the application tick (persp_world), so there W->tick is the composed application
   tick and a sample's tick the one the recorder applied it at. Everything here is the game
   thread's: the requests come from the HUD (in the wndproc TA pumps on that thread) and the
   trigger, and the visit runs before the in-play draw call 0x4969CD. */

#define OFF_VIEW_W     0x37E37      /* the 1x view, as the recorder's samples carry it (RecPov)    */
#define OFF_VIEW_H     0x37E3B

/* the seat has a sample to show, or why not */
static const char* follow_refusal(int seat)
{
    const RecHeader* h;
    unsigned i;
    if (!tagpu_replay_live()) return "this level plays no recording";
    h = recr_header(s_rd);
    for (i = 0; i < h->nseats && i < REC_PLAYERS; i++)
        if (seat >= 0 && seat < REC_PLAYERS && h->seats[i].index == (unsigned)seat)
            return persp_world()->npov[seat] ? NULL : "its camera was not recorded";
    return "no such recorded seat";
}

int tagpu_replay_follow_request(int seat, int once)
{
    const char* why;
    if (seat < 0) {
        if (!tagpu_replay_live()) return 0;
        s_fl.req = FQ_FREE;
        return 1;
    }
    why = follow_refusal(seat);
    if (why) {
        tagpu_logf("replay: %s: %s %d refused: %s", tagpu_replay_live() ? s_name : "-",
                   once ? "jump" : "follow", seat, why);
        return 0;
    }
    s_fl.req = FQ_FOLLOW;
    s_fl.rseat = seat;
    s_fl.ronce = once;
    return 1;
}

int tagpu_replay_followed(void)
{
    if (s_fl.req == FQ_FREE) return -1;
    if (s_fl.req == FQ_FOLLOW) return s_fl.ronce ? -1 : s_fl.rseat;
    return s_fl.E && !s_fl.once ? s_fl.seat : -1;
}

static const char* follow_why(int st)
{
    switch (st) {
    case TAGPU_FOLLOW_MOVED:   return "camera moved";
    case TAGPU_FOLLOW_ZOOMED:  return "zoomed";
    case TAGPU_FOLLOW_DRAWN:   return "drawn";
    case TAGPU_FOLLOW_STOPPED: return "stopped";
    case TAGPU_FOLLOW_LEVEL:   return "the level ended";
    default:                   return "unknown";
    }
}

/* the epoch in force ends, tagpu_zoom's side with it, with one line saying why */
static void follow_end_log(const char* why)
{
    if (!s_fl.E) return;
    tagpu_zoom_follow_stop(s_fl.E);
    tagpu_logf("replay: %s: %s ended: %s", s_name, s_fl.once ? "jump" : "follow", why);
    s_fl.E = 0;
    s_fl.seat = -1;
    s_fl.handed = 0;
}

static void follow_close(const char* why)
{
    follow_end_log(why);
    s_fl.req = FQ_NONE;
    s_fl.pov = 0;
}

/* The frame visit's part, after any seek and on every path out of the visit, paused frames
   included. */
static void follow_visit(char* ta)
{
    const RecWorld* W;
    RecPov p;
    if (s_fl.req) {
        const int req = s_fl.req;
        const char* why = req == FQ_FOLLOW ? follow_refusal(s_fl.rseat) : NULL;
        s_fl.req = FQ_NONE;
        /* a seek since the request can have taken the seat's samples away */
        if (why) tagpu_logf("replay: %s: %s %d refused: %s", s_name,
                            s_fl.ronce ? "jump" : "follow", s_fl.rseat, why);
        else if (req == FQ_FREE && !s_fl.E)
            tagpu_logf("replay: %s: free cam: nothing was followed", s_name);
        else follow_end_log(req == FQ_FREE ? "free cam" : s_fl.ronce ? "a jump" : "another follow");
        if (!why && req == FQ_FOLLOW) {
            s_fl.seat = s_fl.rseat;
            s_fl.once = s_fl.ronce;
            s_fl.E = ++s_flEpochs;
            s_fl.handed = 0;
            tagpu_logf("replay: %s: %s seat %d, epoch %u", s_name, s_fl.once ? "jump to" : "follow",
                       s_fl.seat, s_fl.E);
        }
    }
    if (s_fl.E && !tagpu_replay_live()) follow_end_log("the playing ended");
    if (s_fl.E && s_fl.handed) {
        const int st = tagpu_zoom_follow_state(s_fl.E);
        if (st != TAGPU_FOLLOW_ACTIVE) follow_end_log(follow_why(st));
    }
    memset(&s_fl.at, 0, sizeof s_fl.at);
    if (s_fl.E) {
        W = persp_world();
        s_fl.at.n = W->tick;
        s_fl.at.vw = *(const int*)(ta + OFF_VIEW_W);
        s_fl.at.vh = *(const int*)(ta + OFF_VIEW_H);
        /* the seat is a header seat (follow_refusal), so below REC_PLAYERS. A seek to before the
           seat's first sample leaves it none: the camera stays where the last write put it */
        s_fl.at.how = persp_sample(W->npov[s_fl.seat], W->pov[s_fl.seat], W->pov_tick[s_fl.seat],
                                   W->tick, &p);
        if (s_fl.at.how && persp_follow(&p, s_fl.at.vw, s_fl.at.vh, &s_fl.at.x, &s_fl.at.y,
                                        &s_fl.at.level)) {
            if (tagpu_zoom_follow(ta, s_fl.at.x, s_fl.at.y, s_fl.at.level, s_fl.E, s_fl.once))
                s_cull_inputs++;
            s_fl.handed = 1;
        } else s_fl.at.how = PERSP_NONE;
    }
    if (s_fl.pov) {
        static const char* const how[] = { "none", "latest", "between", "earlier" };
        s_fl.pov = 0;
        if (!s_fl.E)
            tagpu_logf("replay: %s: pov at GameTime %u: no follow; overlays: %u items, %u "
                       "selected units", s_name, *(const unsigned*)(ta + OFF_GAMETIME), s_ov.n,
                       s_ov.nrsel);
        else
            tagpu_logf("replay: %s: pov at GameTime %u: %s epoch %u seat %d, tick %u, sample %s, "
                       "view %dx%d, eye (%d, %d), level %u/65536 (%.4f); overlays: %u items, %u "
                       "selected units", s_name,
                       *(const unsigned*)(ta + OFF_GAMETIME), s_fl.once ? "jump" : "follow",
                       s_fl.E, s_fl.seat, s_fl.at.n, how[s_fl.at.how], s_fl.at.vw, s_fl.at.vh,
                       s_fl.at.x, s_fl.at.y, (unsigned)s_fl.at.level,
                       (double)s_fl.at.level / 65536.0, s_ov.n, s_ov.nrsel);
    }
}

/* ---- THE OVERLAYS (research/notes/tadr-port/demo-recorder-perspective-streams.md, *Playback*)
   Every recorded player's cursor, clicks and selection at once, whatever Sight shows (the owner's
   answer Q5), each behind its switch on the replay HUD's foot plate (tagpu_hudinput.c), which also
   holds the chat's. Built at the frame visit, after any seek and after Follow's own reading of the same
   world, from the reader's world alone (persp_world) -- nothing of the engine's:
   - a cursor: the perspective clock's sample for the displayed tick W->tick, as Follow takes it
     (tagpu_persp.h), when it was over the world (REC_POV_WORLD);
   - a click: REC_CLICK_SHOW ticks from the iteration it came with, aged against GameTime + 1,
     which is W->tick while the playing runs and goes on growing past the recording's end, so a
     last click expires there while the last cursor and selection hold;
   - a selection's unit: only while its slot holds a live unit whose birth is the entry's, read
     from the recording's own births column -- on a puppet host the engine's column is host-own
     (THE SEEK, "B4's per-slot columns"), so a unit made in the slot since is never taken for the
     one selected; in a multiplayer recording's own segment from the composed lifecycle's birth,
     the reference its order lists are held to (mp_authority).
   THE PUBLISHER COPIES THE RESULT BY VALUE (tagpu_packet_pub.c, fill_overlay and the unit walk),
   in the `after` of the draw this visit precedes, on this same thread; the render thread sees
   only that copy. The switches are the game thread's too: the menu's clicks come in the wndproc
   TA pumps on it, the trigger at the frame visit. A switched-off overlay produces nothing, so the
   render side has no switch. A multiplayer recording's perspectives are its contributions: its
   cursors and selections at the composed application tick (persp_world), its clicks from the
   held envelopes at their application tick (mp_click). */

#define BIRTHS_TABLE "src/tagpu_patches.c:s_hitStamp"   /* the births column's RT_TABLE name */

static const char* const OV_NAME[TAGPU_OV_N] = { "cursors", "clicks", "selections", "chat" };

int tagpu_replay_overlay(int which)
{
    if (which == TAGPU_OV_CHAT) return !s_chat_hidden;
    return which >= 0 && which < TAGPU_OV_CHAT && !s_ov.off[which];
}

void tagpu_replay_overlay_set(int which, int on)
{
    if (which == TAGPU_OV_CHAT) {
        s_chat_hidden = !on;
        tagpu_logf("replay: chat %s", s_chat_hidden ? "hidden" : "shown");
        return;
    }
    if (which < 0 || which >= TAGPU_OV_CHAT) return;
    s_ov.off[which] = (unsigned char)!on;
    tagpu_logf("replay: overlay %s %s", OV_NAME[which], on ? "on" : "off");
}

/* `overlay <cursors|clicks|selections|chat> <on|off>` */
static int overlay_trigger(const char* line)
{
    char name[16], state[8];
    int end = 0, k;
    if (sscanf(line, "overlay %15s %7s%n", name, state, &end) != 2 || line[end]) return 0;
    if (strcmp(state, "on") && strcmp(state, "off")) return 0;
    for (k = 0; k < TAGPU_OV_N; k++)
        if (!strcmp(name, OV_NAME[k])) {
            tagpu_replay_overlay_set(k, state[1] == 'n');
            return 1;
        }
    return 0;
}

static void ov_clicks_clear(void)
{
    s_ov.rn = s_ov.rnext = 0;
}

/* a click into the ring, shown from `tick`; the player is bounded here against ov_col */
static void ov_click_put(unsigned tick, const RecClick* c)
{
    if (c->player >= TAGPU_PK_OV_PLAYERS) return;
    s_ov.ring[s_ov.rnext].tick = tick;
    s_ov.ring[s_ov.rnext].c = *c;
    s_ov.rnext = (s_ov.rnext + 1) % OV_RING;
    if (s_ov.rn < OV_RING) s_ov.rn++;
}

/* The iteration's clicks into the ring, from play(), which applies each iteration's events once:
   so each click is taken once, at the tick of the iteration it came with. The reader checked
   each one's length and fields (rec_click_ok). */
static void ov_clicks_take(const RecWorld* W)
{
    unsigned i;
    for (i = 0; i < W->nclick; i++) {
        RecClick c;
        if (W->click[i].kind != RK_CLICK || W->click[i].len != sizeof c) continue;
        memcpy(&c, W->click[i].b, sizeof c);
        ov_click_put(W->tick, &c);
    }
}

static void ov_rsel_clear(void)
{
    unsigned i;
    for (i = 0; i < s_ov.nrsel; i++) s_rsel[s_rselAt[i]] = 0;
    s_ov.nrsel = 0;
}

static void ov_item(unsigned kind, unsigned p, unsigned age, int32_t x, int32_t h, int32_t z)
{
    TAGPU_PK_OVERLAY* o;
    if (s_ov.n >= TAGPU_PK_MAX_OVERLAY) return;
    o = &s_ov.item[s_ov.n++];
    o->kind = (uint8_t)kind;
    o->player = (uint8_t)p;
    o->colour = s_ov.col[p];
    o->age = (uint8_t)age;
    o->x = x;
    o->h = h;
    o->z = z;
}

/* the recording's births column at W, or NULL when it carries none */
static const RecTable* ov_births(const RecWorld* W)
{
    const RecHeader* h = recr_header(s_rd);
    unsigned i;
    for (i = 0; i < h->ntab && i < W->ntab; i++)
        if (!strcmp(h->tab[i].name, BIRTHS_TABLE) && h->tab[i].stride == sizeof(uint32_t))
            return &W->tab[i];
    return NULL;
}

/* An entry's unit is the displayed one: in a multiplayer recording's own segment, the composed
   lifecycle's birth for the slot, the reference its order lists are held to (mp_authority);
   elsewhere the recording's births column, which `b` holds `rows` rows of. */
static int ov_born(const RecTable* b, unsigned rows, unsigned slot, uint32_t birth)
{
    uint32_t at;
    if (s_mp.on) {
        RecMpLifeState ls;
        return tagpu_recmp_lifecycle_get(s_mp.life, slot, &ls) == 1 && ls.birth == birth;
    }
    if (!b || slot >= rows) return 0;
    memcpy(&at, b->img + (size_t)slot * sizeof at, sizeof at);
    return at == birth;
}

/* THE SELECTION DISPLAY RULE. BOUND: a slot is used only below the world's slot count and
   s_rsel's length, and a births column's rows (ov_born) or the lifecycle's own slots
   (tagpu_recmp_lifecycle_get refuses any other); the reader has already held every entry inside
   its seat's block (rec_sel_entries_ok, tagpu_recmp_pov_put), and a seat is a header seat below
   REC_PLAYERS. */
static void ov_selections(const RecWorld* W)
{
    const RecHeader* h = recr_header(s_rd);
    const RecTable* b = s_mp.on ? NULL : ov_births(W);
    unsigned rows = b ? b->size / (unsigned)sizeof(uint32_t) : 0, i, k;
    for (i = 0; (rows || s_mp.on) && i < h->nseats && i < REC_PLAYERS; i++) {
        unsigned p = h->seats[i].index;
        const RecSelEntry* e;
        if (p >= TAGPU_PK_OV_PLAYERS) continue;
        e = W->sel + (size_t)p * REC_SEL_MAX;
        for (k = 0; k < W->nsel[p] && k < REC_SEL_MAX; k++) {
            unsigned slot = e[k].slot;
            if (slot >= W->slots || slot >= TAGPU_PK_MAX_UNITS || !W->live[slot] ||
                !(img_u32(W, slot, U_STATE) & U_ALIVE) || !ov_born(b, rows, slot, e[k].birth))
                continue;
            if (!s_rsel[slot]) s_rselAt[s_ov.nrsel++] = (uint16_t)slot;
            s_rsel[slot] |= (uint16_t)(1u << p);
        }
    }
}

/* The frame visit's part, every in-play frame, paused ones included; before Follow's, so that
   `pov` reports this visit's counts. */
static void overlay_visit(char* ta)
{
    const RecHeader* h;
    const RecWorld* W;
    unsigned i, now;
    s_ov.n = 0;
    ov_rsel_clear();
    s_ov.live = tagpu_replay_live();
    if (!s_ov.live) return;
    h = recr_header(s_rd);
    W = persp_world();
    seat_colours();
    if (!s_ov.off[TAGPU_OV_CURSORS])
        for (i = 0; i < h->nseats && i < REC_PLAYERS; i++) {
            unsigned p = h->seats[i].index;
            RecPov pv;
            if (p >= TAGPU_PK_OV_PLAYERS ||
                !persp_sample(W->npov[p], W->pov[p], W->pov_tick[p], W->tick, &pv) ||
                !(pv.flags & REC_POV_WORLD))
                continue;
            ov_item(TAGPU_PK_OV_CURSOR, p, 0, pv.cur_x, pv.cur_h, pv.cur_z);
        }
    if (!s_ov.off[TAGPU_OV_CLICKS]) {
        now = *(const unsigned*)(ta + OFF_GAMETIME) + 1u;
        for (i = 0; i < s_ov.rn; i++) {             /* the oldest first: a newer ring on top */
            unsigned at = (s_ov.rnext + OV_RING - s_ov.rn + i) % OV_RING;
            unsigned age = now - s_ov.ring[at].tick;
            const RecClick* c = &s_ov.ring[at].c;
            if (age < REC_CLICK_SHOW) ov_item(TAGPU_PK_OV_CLICK, c->player, age, c->x, c->h, c->z);
        }
    }
    if (!s_ov.off[TAGPU_OV_SELECTIONS]) ov_selections(W);
}

/* The playing ended: nothing is shown until the next one, whose switches are all on. */
static void overlay_close(void)
{
    s_ov.n = 0;
    ov_rsel_clear();
    s_ov.live = 0;
    s_ov.col_ok = 0;
    ov_clicks_clear();
    memset(s_ov.off, 0, sizeof s_ov.off);
    s_chat_hidden = 0;
}

unsigned tagpu_replay_overlays(TAGPU_PK_OVERLAY* out, unsigned cap, unsigned char col[10])
{
    unsigned n = s_ov.live ? s_ov.n : 0;
    if (n > cap) n = cap;
    if (n) memcpy(out, s_ov.item, (size_t)n * sizeof *out);
    if (s_ov.live) memcpy(col, s_ov.col, TAGPU_PK_OV_PLAYERS);
    else memset(col, 0, TAGPU_PK_OV_PLAYERS);
    return n;
}

unsigned tagpu_replay_rsel(unsigned slot)
{
    return s_ov.live && slot < TAGPU_PK_MAX_UNITS ? s_rsel[slot] : 0;
}

int tagpu_replay_paused(void)
{
    const char* ta = ta_main();
    return ta ? (*(const unsigned short*)(ta + OFF_PAUSE) & 1) : 0;
}

/* the Pause key's own read-modify-write of the word (0x4960AA..0x4960BC), without its packet 0x19:
   a playback is a skirmish, mode 2, and has no peer */
void tagpu_replay_pause(int on)
{
    char* ta = ta_main();
    unsigned short w;
    if (!ta || !tagpu_puppet_on() || !s_rd) return;
    w = *(unsigned short*)(ta + OFF_PAUSE);
    *(unsigned short*)(ta + OFF_PAUSE) = (unsigned short)((w & ~1u) | (on ? 1u : 0u));
}

int tagpu_replay_speed(void)
{
    const char* ta = ta_main();
    return ta ? *(const unsigned short*)(ta + OFF_SPEED_CEIL) : 0;
}

/* THE SPEED, written into both the ceiling and the live speed. Only the presets 5, 10, 20, 40 and
   80 (BOUND), and only while the player's speed is saved for the teardown to put back (s_opt.saved,
   THE PLAYER'S SETTINGS): every speed written here has its restore. 40 and 80 are past
   SetGameSpeed's clamp of 20 (0x490DF0), which this does not call; the scheduler has no clamp from
   above, and the engine bounds what a frame can step itself: at most 5 ticks a frame, and the
   live speed drops by 1 after 10 clamped frames in a row (0x4953EF..0x49547B). */
int tagpu_replay_speed_set(unsigned gs)
{
    char* ta = ta_main();
    if (!ta || !tagpu_puppet_on() || !s_rd || !s_opt.saved) return 0;
    if (gs != 5 && gs != 10 && gs != 20 && gs != 40 && gs != 80) return 0;
    *(unsigned short*)(ta + OFF_SPEED_CEIL) = (unsigned short)gs;
    *(unsigned short*)(ta + OFF_SPEED_LIVE) = (unsigned short)gs;
    return 1;
}

/* THE VIEWER'S SEAT, at the level's end. V is record 10, which the playback seats for itself and
   the recording does not describe: viewer_seat activates it, gives it the empty block and sets
   its watcher bit (+0x9B bit 6) and its side. Record 10 goes back whole as the level's first
   seating took it: its buffer pointers were NULL then, which matches the state 0x464A00 has left
   them in (freed and zeroed, 0x464A2D, 0x4801F0 from 0x464A3C, 0x464A76..0x464A7E), and the
   pointer +0x27 to its PlayerInfo never changes. Then, when +0x2A42 or +0x2A43 names record 10,
   both become 0, the players reset's values (0x46451B, 0x46452D), so no ten-entry table can be
   indexed with 10 outside a level that seats record 10. Of the PlayerInfo, the seat activation
   clears only bit 5 (0x464378); the front end's reset 0x4644D0 zeroes every
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
    char* ta = ta_main();
    if (ta && s_vrec.taken) memcpy(player_rec(ta, REC_VIEWER), s_vrec.was, PL_STRIDE);
    s_vrec.taken = 0;
    if (s_vinfo.at) memcpy(s_vinfo.at, s_vinfo.was, REC_INFO_BYTES);
    s_vinfo.at = NULL;
    if (ta && ((unsigned char)ta[OFF_CONTROLLED] == REC_VIEWER ||
               (unsigned char)ta[OFF_VIEWED] == REC_VIEWER))
        ta[OFF_CONTROLLED] = ta[OFF_VIEWED] = 0;
}

/* ---- THE BROWSER'S START -------------------------------------------------------------------- */

/* THE BROWSER'S START (tagpu_replay_watch): the Replays browser's Watch, run inside REPLAYS'
   OnCommand, where SKIRMISH's Start runs inside its own. It takes the engine's route from the main
   menu to a skirmish load without building SINGLE or SKIRMISH: the side effects of the two shell
   transitions it skips -- SINGLE's players reset 0x4644D0 (0x42734F) and the Skirmish request's
   preferences load 0x42F9A0 and SetGameMode(2) 0x434AB0 (0x4273E8..0x4273EF) -- then the Start
   branch 0x47AEDF..0x47B0BC call for call, with the seating and the options where its two sites
   would run them, then the shell state that branch leaves, and an empty GUI stack. The
   recording's seats and map go into the settings the branch reads, after the preferences load,
   which reads the player's own into the same block from the registry (where the player's edits
   on SKIRMISH would go in a stock Start); the branch's settings save is not made, and the player's seats, map and options come back at the teardown (THE PLAYER'S
   SETTINGS). The file and the view are arguments: no request outlives the call, so no later
   stock Start can become a playback.
   ORDERING. Three phases. Checks, which write nothing. Then the undoable steps in the stock order;
   a refusal undoes them before its notice, since the notice 0x4ABD90 can run a nested pump
   (0x4AC035, reached from 0x4ABF81). Then the commit, which nothing refuses: the validation's
   inputs (the settings, the mode, the network bit, the seat count, the sides and colours, the
   header) are disjoint from what the fill writes (PlayerInfo +0x95/+0x96, the records, main
   +0x2A42/+0x2A43, the alliances), so its verdict before the fill is its verdict at the fill. The
   commit leaves what a stock Start leaves -- the seats filled, mode 2, shell group 9 with request
   2 -- and pops every screen, as the load state does (0x497F6A..0x497F80) and LOADGAME does from
   inside its own OnCommand (0x491D70(1) at 0x4929EB). The same IdleTick's shell tick 0x426E80
   takes request 2 (0x426E8B..0x426EC4), group 9's sub-state 2 sets main+0x2A44 bit 2 (0x4281F6),
   and the front end's state 0x496BB0 enters the load state 0x497F40 (0x496C05..0x496C47). No
   screen exists between the commit and the load to take a second click or a key.
   BOUNDS. At most ten seat entries, the settings' own count; the map copied within the engine's
   0x100 (0x47AA12..0x47AA31); the pop loop ends with the stack.
   THE START GUARD. Each range of engine code the route rests on, as this design was made against
   it, is compared by FNV-1a against the retail exe's bytes whenever Watch is offered or pressed.
   A mismatch greys Watch and names the range. The route's own two sites are masked, so the guard
   does not depend on installation order. */

/* the CD check 0x41D6A0(disc), stdcall `ret 4`: AL nonzero when the disc is found */
typedef unsigned char (__stdcall* PFN_CdCheck)(unsigned disc);
#define E_CdCheck      ((PFN_CdCheck)0x0041D6A0u)
/* the string table lookup 0x4C5740(text), stdcall `ret 4`: `text` when the table has none */
typedef const char* (__stdcall* PFN_Translate)(const char* text);
#define E_Translate    ((PFN_Translate)0x004C5740u)
/* the notice 0x4ABD90(gi, text, width, 1, 1), stdcall `ret 0x14`: copies `text` into its own
   frame (0x4ABDD7..0x4ABDF3), 0x100 bytes */
typedef void (__stdcall* PFN_Notice)(void* gi, const char* text, unsigned w, unsigned a, unsigned b);
#define E_Notice       ((PFN_Notice)0x004ABD90u)
typedef void (__stdcall* PFN_Gi)(void* gi);
#define E_ActDone      ((PFN_Gi)0x004AB0A0u)    /* gi+0x60 = -1: the click is answered          */
#define E_GuiPop       ((PFN_Gi)0x004A9660u)    /* the top screen, its -1 handler, then freed   */
typedef void (__cdecl* PFN_Void)(void);
#define E_PlayersReset ((PFN_Void)0x004644D0u)
#define E_Preferences  ((PFN_Void)0x0042F9A0u)  /* the registry's preferences into main, the
                                                   skirmish settings' map and seat entries among
                                                   them (0x430B6B, from 0x430C06); to the registry
                                                   it writes only a missing key's default
                                                   (0x42F9E5..0x42FA07)                         */
#define E_InitHpi      ((PFN_Void)0x0041D4C0u)  /* InitTAHPIAry                                  */
#define E_SeatFill     ((PFN_Void)0x0047A760u)
#define E_FillTail     ((PFN_Void)0x0041DA30u)  /* main+0x391CF = 0x55 x25, +0x391E8 = 0        */
typedef void (__stdcall* PFN_SetMode)(unsigned mode);
#define E_SetGameMode  ((PFN_SetMode)0x00434AB0u)   /* frees and makes GameingState anew        */
#define E_Cursor       ((PFN_SetMode)0x00491C80u)   /* the cursor (0x14, as the Start leaves it) */
typedef int (__thiscall* PFN_OtaRead)(void* gs, const char* map);
#define E_OtaRead      ((PFN_OtaRead)0x00435A20u)   /* 0: the map's terrain is missing          */
typedef int (__thiscall* PFN_MapSeats)(void* gs);
#define E_MapSeats     ((PFN_MapSeats)0x00437300u)  /* the start positions the map offers       */
typedef int (__cdecl* PFN_Int)(void);
#define E_OneAlly      ((PFN_Int)0x00479760u)       /* 1: every active entry in one ally group  */
#define STR_CD         ((const char*)0x005030C0u)   /* the Start's refusals, as it shows them   */
#define STR_TERRAIN    ((const char*)0x0050842Cu)
#define STR_PLAYERS    ((const char*)0x0050838Cu)
#define STR_TOOMANY    ((const char*)0x005083FCu)
#define STR_ALLIES     ((const char*)0x005083C8u)
#define NOTICE_W       0x1E0
#define NOTICE_MAX     0x100        /* the notice's copy of its text                              */
#define OFF_GUI        0x519        /* the GUI info, `gi`                                        */
#define GI_TOP         0x18         /* its top screen, NULL when the stack is empty               */
#define OFF_SHELLGROUP 0x2BBE       /* the shell's screen family ...                              */
#define OFF_SHELLREQ   0x2BC0       /* ... and the sub-state it is asked for (0x426E8B..0x426EC4)  */
#define SHELL_SKIRMISH 9
#define REQ_START      2            /* group 9's sub-state 2: 0x4281F6, `or [main+0x2A44], 4`     */
#define NET_FRONTEND   0x08         /* main+0x2A44 bit 3, as SINGLE's request sets it (0x427164)   */
#define CTRL_HUMAN     1
#define CTRL_AI        2
#define MENU_LOBBY     0x00512C80u  /* nonzero: the main menu goes on to the lobby (0x4270AE..0x4270BA) */

typedef struct {
    const char* name;
    unsigned    a[3], b[3];         /* up to three ranges [a, b)                                   */
    unsigned    ma, mb, na, nb;     /* up to two masked ranges                                     */
    unsigned    fnv;                /* of the retail exe's bytes: pristine/TotalA.exe.pristine     */
} Guard;
static const Guard GUARDS[] = {
    { "the SKIRMISH Start 0x47AEDF..0x47B0F7", { 0x47AEDF }, { 0x47B0F8 },
      0x47B092, 0x47B097, 0x47B09C, 0x47B0A1, 0x297667D1u },
    { "SINGLE's players reset call 0x42734F", { 0x42734F }, { 0x427354 }, 0, 0, 0, 0, 0xC8DF5CDBu },
    { "the Skirmish request 0x4273E8..0x427414", { 0x4273E8 }, { 0x427415 }, 0, 0, 0, 0,
      0xF93C7E6Eu },
    { "group 9's dispatch 0x427594, its table 0x42866C and sub-state 2 0x4281F6",
      { 0x427594, 0x42866C, 0x4281F6 }, { 0x4275B7, 0x42867C, 0x4281FD }, 0, 0, 0, 0, 0xB82BD2E5u },
    { "the shell tick 0x426E80 and its group 9 entry 0x4285C0", { 0x426E80, 0x4285C0 },
      { 0x426EE1, 0x4285C4 }, 0, 0, 0, 0, 0x81441611u },
    { "the front end's state 0x496BB5..0x496C48", { 0x496BB5 }, { 0x496C49 }, 0, 0, 0, 0,
      0x06E340BCu },
    { "GUI_Pop 0x4A9660", { 0x4A9660 }, { 0x4A96CC }, 0, 0, 0, 0, 0x467555C9u },
    { "the main menu's entry 0x427093..0x4270E5", { 0x427093 }, { 0x4270E6 },
      0x4270A9, 0x4270AE, 0, 0, 0xC4FA39B6u },
};

static int s_sitesIn;               /* the four play sites and the save's observer              */
static int s_watchIn;               /* the main menu's return site and the RESTART gate         */
static void (*s_back)(void* gi);    /* the browser, to reopen once the main menu is next built  */

static unsigned guard_fnv(const Guard* g)
{
    unsigned h = 0x811C9DC5u, r, va;
    for (r = 0; r < 3 && g->a[r]; r++)
        for (va = g->a[r]; va < g->b[r]; va++) {
            if ((va >= g->ma && va < g->mb) || (va >= g->na && va < g->nb)) continue;
            h = (h ^ *(const unsigned char*)(size_t)va) * 0x01000193u;
        }
    return h;
}

int tagpu_replay_watch_ready(char* why, unsigned cap)
{
    unsigned i;
    if (!s_sitesIn || !s_watchIn) {
        snprintf(why, cap, "the replay's sites are not in (the log's `replay:` lines say which)");
        return 0;
    }
    for (i = 0; i < sizeof GUARDS / sizeof GUARDS[0]; i++)
        if (guard_fnv(&GUARDS[i]) != GUARDS[i].fnv) {
            snprintf(why, cap, "%s holds other bytes than the route was made against",
                     GUARDS[i].name);
            return 0;
        }
    return 1;
}

/* A WRITER'S FILE IS NEVER PLAYED. The recorder's writer creates each recording CREATE_NEW and
   holds it open, for writing and sharing reads alone, until it has written the end
   (tagpu_recwriter.c); an open that shares reads alone therefore fails with a sharing violation
   exactly while a writer holds the file. The answer cannot go stale the wrong way: a closed
   recording is never opened for writing again, since CREATE_NEW refuses a file that exists, so a
   file found closed stays closed while it is read. The reader's own open does not decide this:
   recr_info shares writing, and recr_open may come to. */
int tagpu_replay_being_written(const char* path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
        return 0;
    }
    return GetLastError() == ERROR_SHARING_VIOLATION;
}

/* A file the browser listed in replays\: a name, never a path out of the folder. A sanity filter
   on a value the browser's own enumeration produced, not a safety argument. */
static int file_ok(const char* file)
{
    size_t n = strlen(file);
    return n && n <= MAX_PATH - 9 && !strpbrk(file, "\\/:") && !strstr(file, "..");
}

/* The recording's seats and map into the settings, the player's kept in s_opt. The entries below
   the count: a seat's controller, side and colour, no ally group (seat_level writes the recorded
   rows); metal and energy stay the player's, which the first image overwrites. A gap's controller
   is 0. Entries at or above the count are left: the fill walks below it (0x47A760). */
static int settings_in(char* ta, const RecHeader* h)
{
    char* set = *(char**)(ta + OFF_SETTINGS);
    unsigned count = 0, i;
    if (!set) return 0;
    memcpy(s_opt.entries, set, sizeof s_opt.entries);
    memcpy(s_opt.map, set + SET_MAP, sizeof s_opt.map);
    s_opt.nseats = *(unsigned*)(ta + OFF_NSEATS);
    s_opt.seats_saved = 1;
    for (i = 0; i < h->nseats; i++)
        if (h->seats[i].index + 1 > count) count = h->seats[i].index + 1;  /* below 10: the reader */
    for (i = 0; i < count; i++) *(unsigned*)(set + i * SET_ENTRY) = 0;
    for (i = 0; i < h->nseats; i++) {
        char* e = set + h->seats[i].index * SET_ENTRY;
        *(unsigned*)e = h->seats[i].controller;
        *(unsigned*)(e + ENT_SIDE) = h->seats[i].side;
        *(unsigned*)(e + ENT_ALLY) = ALLY_NONE;
        *(unsigned*)(e + ENT_COLOUR) = h->seats[i].logo;
    }
    memset(set + SET_MAP, 0, SET_MAPLEN);
    memcpy(set + SET_MAP, start_map(h), strnlen(start_map(h), SET_MAPLEN - 1));
    *(unsigned*)(ta + OFF_NSEATS) = count;
    return 1;
}

/* entries below the seat count with controller `c`, as the Start counts them (0x47AF39..0x47AF56) */
static unsigned controlled_by(const char* ta, unsigned c)
{
    const char* set = *(const char* const*)(ta + OFF_SETTINGS);
    unsigned n = *(const unsigned*)(ta + OFF_NSEATS), i, k = 0;
    for (i = 0; i < n && i < SET_ENTRIES; i++)
        if (*(const unsigned*)(set + i * SET_ENTRY) == c) k++;
    return k;
}

/* the Start's refusal idiom (0x47B0BF..0x47B0F7), after the log line. The notice looks its text
   up itself (0x4ABDC7), so an engine string is passed as the Start passes it; a reason of ours
   reads as one. */
static int watch_notice(void* gi, const char* name, const char* text, int ours)
{
    char t[NOTICE_MAX];
    snprintf(t, sizeof t, ours ? "This recording cannot be played: %s." : "%s", text);
    tagpu_logf("replay: not playing %s: %s", name, text);
    E_Notice(gi, t, NOTICE_W, 1, 1);
    E_ActDone(gi);
    return 0;
}

static int watch_refuse(void* gi, const char* name, const char* why)
{
    return watch_notice(gi, name, why, 1);
}

static int watch_refuse_engine(void* gi, const char* name, const char* text)
{
    return watch_notice(gi, name, text, 0);
}

/* Phase B's undo: the settings and the mode the main menu had (0x427093), the recording closed;
   then the notice, `ours` as watch_notice takes it */
static int watch_undo(char* ta, void* gi, const char* name, const char* text, int ours)
{
    seats_restore(ta);
    E_SetGameMode(0);
    replay_close();
    return watch_notice(gi, name, text, ours);
}

int tagpu_replay_watch(void* gi, const char* file, int view, void (*back)(void* gi))
{
    char* ta = ta_main();
    char name[MAX_PATH], path[MAX_PATH], why[200];
    const RecHeader* h;
    char* set;
    void* gs;
    unsigned v = 0, ais;
    size_t n;
    Refusal r = { 0, 0, 0 };

    /* A: checks; nothing is written */
    snprintf(name, sizeof name, "%s", file);
    n = strlen(name);
    if (n > 7 && !_stricmp(name + n - 7, ".imprec")) name[n - 7] = 0;
    if (!ta) return 0;
    if (!tagpu_replay_watch_ready(why, sizeof why)) return watch_refuse(gi, name, why);
    if (!file_ok(file)) return watch_refuse(gi, name, "the file is not one of replays\\");
    if (ta[OFF_NETWORK] & 1) return watch_refuse(gi, name, "this is a network game");
    if (!E_CdCheck(1)) return watch_refuse_engine(gi, name, E_Translate(STR_CD));
    snprintf(path, sizeof path, "replays\\%s", file);
    if (tagpu_replay_being_written(path))
        return watch_refuse(gi, name, "it is the recording the game is writing");
    replay_close();
    tagpu_puppet_end();
    s_rd = recr_open(path, why, sizeof why);
    if (!s_rd) return watch_refuse(gi, name, why);
    span_read(path);
    h = recr_header(s_rd);
    if (!validate_header(h, why, sizeof why, &v, &r) || !view_ok(h, view, why, sizeof why)) {
        replay_close();
        refused(&r, path);              /* before the notice: the browser is still on top */
        return watch_refuse(gi, name, why);
    }
    /* Watch's Start fills the skirmish menu from the recorded controllers, which knows types 1 and
       2 only; a multiplayer recording plays through tacli replay play, which fills it as human
       and AI */
    if (h->mode == MODE_NETWORK) {
        replay_close();
        return watch_refuse(gi, name, "a multiplayer recording plays through tacli replay play "
                            "only");
    }

    /* B: the stock order; each refusal undoes before its notice */
    E_PlayersReset();
    E_Preferences();
    E_SetGameMode(MODE_SKIRMISH);
    gs = *(void**)(ta + OFF_GSTATE);
    if (!gs) return watch_undo(ta, gi, name, "the game state could not be made", 1);
    if (!settings_in(ta, h)) return watch_undo(ta, gi, name, "there are no skirmish settings", 1);
    E_InitHpi();
    ais = controlled_by(ta, CTRL_AI);
    *(unsigned short*)(ta + OFF_NPLAYERS) = (unsigned short)(ais + 1);
    set = *(char**)(ta + OFF_SETTINGS);
    if (!E_OtaRead(gs, set + SET_MAP))
        return watch_undo(ta, gi, name, E_Translate(STR_TERRAIN), 0);
    if (ais < 1 || controlled_by(ta, CTRL_HUMAN) < 1)
        return watch_undo(ta, gi, name, E_Translate(STR_PLAYERS), 0);
    if (*(unsigned short*)(ta + OFF_NPLAYERS) > (unsigned)E_MapSeats(gs))
        return watch_undo(ta, gi, name, E_Translate(STR_TOOMANY), 0);
    if (E_OneAlly()) return watch_undo(ta, gi, name, E_Translate(STR_ALLIES), 0);
    if (!validate_engine(ta, h, why, sizeof why, 1)) return watch_undo(ta, gi, name, why, 1);

    /* C: the commit; nothing below refuses */
    *(unsigned short*)(ta + OFF_NPLAYERS) = (unsigned short)(ais + controlled_by(ta, CTRL_HUMAN));
    E_SeatFill();
    seat(ta, name, v, view);
    E_FillTail();
    replay_options();
    ta[OFF_NETWORK] |= NET_FRONTEND;
    ta[OFF_SHELLGROUP] = SHELL_SKIRMISH;
    ta[OFF_SHELLREQ] = REQ_START;
    E_Cursor(0x14);
    s_back = back;
    while (*(void**)((char*)gi + GI_TOP)) E_GuiPop(gi);
    return 1;
}

/* The main menu's builder 0x4263B0 has returned at its one call, 0x4270A9: every way out of a
   skirmish level, the in-game quit (0x460630..0x460653 -> 0x425A90) and the results screen's
   MainMenu (0x41EE2D..0x41EE6E), ends in group 2 sub-state 0 and this call. The browser comes
   back once, the first time after a Start it made, unless the menu goes on to the lobby
   (main 0x512C80, read at 0x4270AE): the flag is taken either way, so it cannot outlive the first
   main menu. ORDERING: the builder has staged MAINMENU and flushed the event ring
   (0x426523, 0x426636); this is the shell tick, no pump is in flight, and what follows the call
   touches no screen (0x4270C6..0x4270E1). */
static void __cdecl menu_built(void)
{
    void (*back)(void* gi) = s_back;
    char* ta = ta_main();
    s_back = NULL;
    if (back && ta && !*(const unsigned*)(size_t)MENU_LOBBY) back(ta + OFF_GUI);
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

/* DISASSEMBLED: 0x496920 tests main+0x38A51 bit 0 immediately before the branch
   around scheduler 0x495230 and step 0x495490 (0x496926 jumps to 0x496969, then 0x4969CD and
   0x4969D2: a paused frame still draws and reaches the frame visit). Both the rewind state and
   this callback belong to the game thread. Reasserting pause before that test prevents a physical
   Pause key from advancing the suspended world; doing it at the later draw visit cannot.
   THE HOLD KEEPER: the same holds for a pause THE PAUSE AT T or THE LAUNCH'S HOLD took
   (s_pause_taken). Whatever clears the bit between two frames -- the Pause key at 0x4960AA, the
   in-game menu's OK -- is undone before the next test, so no step runs until `resume`, which
   clears s_pause_taken before the bit. */
static int __cdecl rw_pause_guard(void* entry_esp)
{
    char* ta = ta_main();
    (void)entry_esp;
    if (!ta) return 0;
    if (s_rw.state) *(unsigned short*)(ta + OFF_PAUSE) |= 1;
    else if (s_pause_taken) *(unsigned short*)(ta + OFF_PAUSE) |= 1;
    return 0;
}

static void rw_pause_init(void)
{
    static const unsigned char TEST[6] = { 0x84, 0x98, 0x51, 0x8A, 0x03, 0x00 };
    s_rw_pause_in = tagpu_detour_bytes_ok(0x00496920u, TEST, sizeof TEST) &&
        tagpu_detour_observe(0x00496920u, TEST, sizeof TEST, rw_pause_guard, NULL);
    tagpu_logf("rewind: scheduler pause guard %s", s_rw_pause_in ? "in" : "unavailable");
}

#define TURBO_GAP_MS 250
#define TURBO_RING_BYTES (REC_RING_BYTES / 2)

/* THE TURBO: a test launch's pacing, k ticks every frame. DISASSEMBLED: the scheduler 0x495230
   writes DeltaTime main+0x38A3B from the 30-Hz clock 0x4B6340 and clamps it to 5 (0x4953F5),
   reading back only what it wrote this call; after its return only the count test 0x496932,
   the step 0x495490, which caches it at 0x495497, and a debug sprintf (0x46AA09) read it. So k, bounded to the scheduler's own
   1..5 at attach, written here between that return and that load, makes the frame run k
   unchanged stock iterations and the step's once-a-call tail (0x495674..0x495688) once: the batch
   speed 10k gives a frame that lands on a 30-Hz edge. Nothing an iteration reads is written.
   THE BATCH IS PART OF THE RESULT. The tail -- 0x428BD0, 0x428BE0, 0x428BF0 (each a bare ret),
   the chat expiry 0x463EF0 and the temporary sight records' expiry 0x482130 -- runs once a batch, so where the
   batches fall changes what units see and so what they do. Stock pacing at speed 20 mixes
   batches of 2 with an odd one after a slow frame: of two pairs of takeovers of one recording at
   speed 20 one parted in 1328 unit fields, while both pairs at a fixed k agreed in everything but
   the meteor shower's points, which the CRT rand feeds (MEASURED 2026-10-08, tenseat taken over
   at 931, dumped at 2731). The harness runs k = 1, the batch a player at normal speed gets.
   This observer lies on the skirmish branch past the front-end test 0x496918 and the pause test
   0x496920, so it never runs in a network game, a menu, a paused frame or a rewind (whose pause
   guard holds the pause bit). Bit 1 of main+0x38A51 is the scheduler's lag scale (0x49535C),
   set only when a type-3 player lags 900 ticks: such a frame keeps the stock count.
   THE WRITER'S BACK-PRESSURE. The recorder's ring never blocks the capture (tagpu_recorder.h): a
   level that writes faster than the writer drains ends its recording INCOMPLETE when the ring
   fills. The battle fixtures write ~56 KB of records a tick, and at k = 1 the stock-limits battle
   ran 314 ticks a second and filled the 16 MiB ring by GameTime 1003 (MEASURED 2026-10-08, two
   games at once). So a turbo frame runs no tick while the ring holds more than TURBO_RING_BYTES:
   the count test 0x496932 then skips the step, its tail included, so every batch is still k
   ticks and only when the batches run waits on the writer. A single batch that writes more than
   the rest of the ring still fills it, as it would at stock pacing.
   What turbo does not make tick-exact is what stock pacing does not either: the readers of wall
   time and the CRT rand draws of the frame -- the hover wave in the unit tick (0x48A66A), the
   meteor tick's draws (0x437E37..0x437FB5) against the draw path's from the same stream, the
   camera scroll by wall elapsed (0x41CEA2) -- each a function of the frames and the time between
   two ticks (exe-reverse-engineering.md, the scheduler). */
static void turbo_due(char* ta)
{
    DWORD now;
    if (!s_turbo.live || s_rw.state) return;
    if (*(const unsigned short*)(ta + OFF_PAUSE) & 2) {
        s_turbo.lagged++;
        return;
    }
    if (tagpu_rec_ring_used() > TURBO_RING_BYTES) {
        *(unsigned*)(ta + OFF_DUE_TICKS) = 0;
        s_turbo.held++;
    } else {
        *(unsigned*)(ta + OFF_DUE_TICKS) = s_turbo_k;
        s_turbo.frames++;
    }
    /* the rate's clock: a gap of TURBO_GAP_MS or more between two frames is a pause, which
       reaches no frame here, and is not play */
    now = GetTickCount();
    if (s_turbo.last && now - s_turbo.last < TURBO_GAP_MS) {
        s_turbo.ms += now - s_turbo.last;
        s_turbo.ticks += *(const unsigned*)(ta + OFF_GAMETIME) - s_turbo.last_g;
    }
    s_turbo.last = now;
    s_turbo.last_g = *(const unsigned*)(ta + OFF_GAMETIME);
}

/* DISASSEMBLED: the scheduler returns before 0x49692D; 0x496932 tests its count,
   and 0x495490 loads that count once at 0x495497, then decrements EBX at 0x49566C.
   A future round trip needs the frame after T-1. Bounding the batch before that
   load makes the frame boundary a fact, independent of speed or frame timing.
   Only pacing changes: every simulation iteration still advances by one tick. */
static void rt_pace_bound(char* ta)
{
    unsigned now, due, i;
    if (!tagpu_puppet_on() || !s_rd || s_dead || s_over || s_rw.state || !s_rt.n)
        return;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    due = *(const unsigned*)(ta + OFF_DUE_TICKS);
    if (due > 5) return;
    for (i = 0; i < s_rt.n; i++) {
        unsigned last = s_rt.a[i].T - 1;
        if (last > now && last - now < due) due = last - now;
    }
    *(unsigned*)(ta + OFF_DUE_TICKS) = due;
}

/* THE PAUSE AT T's half of the bound, on any level in play: the batch ends at T, so the frame
   visit after step T is reached with GameTime T, the round trip's argument above. */
static void pause_at_bound(char* ta)
{
    unsigned now, due;
    if (!s_pause_at || s_rw.state) return;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    due = *(const unsigned*)(ta + OFF_DUE_TICKS);
    if (due <= 5 && s_pause_at > now && s_pause_at - now < due)
        *(unsigned*)(ta + OFF_DUE_TICKS) = s_pause_at - now;
}

/* THE LAUNCH'S HOLD's half of the bound: the level's first batch, and every batch until the hold
   is taken, is one tick, so the frame visit after the step that began it sees the level one tick
   on (hold_visit). Like the other bounds it only lowers the count: a frame that runs no step
   (count 0) is left alone and the hold waits for the next. The scheduler leaves the count at
   0..5 (0x4953EF..0x4953FE clamps it, DISASSEMBLED), so the test against 5 bounds a value read
   from engine memory and no frame of the scheduler's fails it. */
static void hold_bound(char* ta)
{
    unsigned due;
    if ((s_lhold.state != LHOLD_ARMED && s_lhold.state != LHOLD_STEP) || s_rw.state ||
        pacing_refusal(ta))
        return;
    due = *(const unsigned*)(ta + OFF_DUE_TICKS);
    if (due < 1 || due > 5) return;
    *(unsigned*)(ta + OFF_DUE_TICKS) = 1;
    if (s_lhold.state == LHOLD_ARMED) {
        s_lhold.g0 = *(const unsigned*)(ta + OFF_GAMETIME);
        s_lhold.state = LHOLD_STEP;
    }
}

/* The turbo first, then the bounds: a bound only lowers the count, so whatever the turbo set,
   an armed round trip still gets the frame after its T-1, a pause its frame after T and the
   launch's hold its one tick. */
static int __cdecl rt_pace_guard(void* entry_esp)
{
    char* ta = ta_main();
    (void)entry_esp;
    if (!ta) return 0;
    turbo_due(ta);
    rt_pace_bound(ta);
    pause_at_bound(ta);
    hold_bound(ta);
    return 0;
}

/* THE PAUSE AT T: a test that drives the game at a GameTime of its own -- a key, an order, a
   scenario -- has it paused there, whatever the pacing, instead of racing a clock it cannot see.
   The bound above ends the batch at T; the frame visit after step T, on the same game thread,
   sets bit 0 of main+0x38A51, the bit the Pause key flips (0x4960AA..0x4960BC, DISASSEMBLED) and
   the frame callback tests at 0x496920 before the scheduler and the step. A GameTime a seek has
   moved past is dropped with a line, never paused at. Nothing is paused without an armed T. */
static void pause_at_arm(unsigned T)
{
    char* ta = ta_main();
    const char* why = pacing_refusal(ta);
    unsigned now;
    if (why) {
        tagpu_logf("pause: at %u refused: %s", T, why);
        return;
    }
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    if (T <= now) {
        tagpu_logf("pause: at %u refused: the level is at GameTime %u", T, now);
        return;
    }
    if (s_pause_at)
        tagpu_logf("pause: at %u replaces the pause at %u", T, s_pause_at);
    s_pause_at = T;
    tagpu_logf("pause: armed for GameTime %u", T);
}

static void pause_at_visit(void)
{
    char* ta = ta_main();
    unsigned now;
    if (!s_pause_at || !ta) return;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    if (now < s_pause_at) return;
    if (now == s_pause_at) {
        *(unsigned short*)(ta + OFF_PAUSE) |= 1;
        s_pause_taken = 1;
        tagpu_logf("pause: paused at GameTime %u", now);
    } else {
        tagpu_logf("pause: at %u not taken: the level is at GameTime %u", s_pause_at, now);
    }
    s_pause_at = 0;
}

/* Clears only the pause THE PAUSE AT T took: a pause the player or a rewind set stays theirs.
   Refused while a live rewind is open: the rewind saved the pause bit when it opened (s_rw.pause)
   and puts it back when it closes, so a pause taken before it would come back with no
   s_pause_taken behind it, a pause no later `resume` could clear. */
static void pause_resume(void)
{
    char* ta = ta_main();
    const char* why = pacing_refusal(ta);
    if (!why && s_rw.state)
        why = "a live rewind is open; it restores the pause it found when it closes";
    if (why || !s_pause_taken) {
        tagpu_logf("resume: refused: %s", why ? why : "no pause at T was taken");
        return;
    }
    s_pause_taken = 0;
    *(unsigned short*)(ta + OFF_PAUSE) &= (unsigned short)~1u;
    tagpu_logf("pause: resumed at GameTime %u", *(const unsigned*)(ta + OFF_GAMETIME));
}

/* THE LAUNCH'S HOLD: a test launch whose tagpu_hold.on is present starts its level held, so a
   test arms what it waits for -- dumps, round trips, pauses, the turbo -- on a level that cannot
   run on until it says `resume`. THE INVARIANT: no GameTime the harness arms can pass before its
   acknowledgement. The level's first batch is bounded to one tick (hold_bound), and the frame
   visit after it sets the pause bit (here), which THE HOLD KEEPER (rw_pause_guard) keeps set until
   `resume`; every arm is read and answered at a frame visit, which paused frames reach too.
   Where each level ends up held:
   - a skirmish, at GameTime 1;
   - a playing, at its first step;
   - a playing with a request's seek (`play <name> at <T>`), at T-1: the seek runs at the frame
     visit after the first step, before this test;
   - a takeover of a held playing, at T-1: its seek runs on a held frame.
   The hold waits while a playing still has its first step's checks or a seek to come, so the
   level it holds is the one the request asked for. */
static void hold_visit(void)
{
    char* ta = ta_main();
    unsigned now;
    if (s_lhold.state != LHOLD_STEP || !ta || s_rw.state) return;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    if (now == s_lhold.g0) return;
    if (tagpu_puppet_on() && s_rd && !s_dead && (s_first || s_seek.pending)) return;
    *(unsigned short*)(ta + OFF_PAUSE) |= 1;
    s_pause_taken = 1;
    s_lhold.state = LHOLD_DONE;
    tagpu_logf("pause: paused at GameTime %u (the launch's hold)", now);
}

/* `turbo on` (the trigger): the turbo until `turbo off` or the level's end, in a test launch
   whose tagpu_turbo.on gave k. The harness sends it while the level is held (THE LAUNCH'S HOLD,
   THE PAUSE AT T), once every dump and round trip of the lane is armed and acknowledged, so the
   turbo's ticks start with its `resume`. */
/* Why the test pacing (`turbo on`, `pause at`, `resume`) may not act now, or NULL. It is a test
   launch's own: the trigger file is read in every launch, and a network game's peers would run on
   while this seat alone paused or batched differently -- the Pause key sends packet 0x19 to them
   (0x4960A4..0x4960BC, DISASSEMBLED) and this pause does not. A pause it takes stays only
   while THE HOLD KEEPER is in, so it needs the pause guard as well as the tick-batch guard. */
static const char* pacing_refusal(const char* ta)
{
    if (!tagpu_regstore_active()) return "not a test launch (tagpu_regstore.h)";
    if (!ta) return "no level";
    if (ta[OFF_NETWORK] & 1) return "a network game";
    if (!s_rt_pace_in) return "the tick-batch guard is unavailable";
    if (!s_rw_pause_in) return "the pause guard is unavailable";
    return NULL;
}

static void turbo_on(void)
{
    char* ta = ta_main();
    const char* why = pacing_refusal(ta);
    if (why || !s_turbo_k) {
        tagpu_logf("turbo: on refused: %s", why ? why : "this launch has no tagpu_turbo.on k");
        return;
    }
    if (!s_turbo.engaged) {
        s_turbo.engaged = 1;
        s_turbo.g0 = *(const unsigned*)(ta + OFF_GAMETIME);
    }
    s_turbo.live = 1;
    tagpu_logf("turbo: on at GameTime %u (k=%u)", *(const unsigned*)(ta + OFF_GAMETIME),
               s_turbo_k);
}

static void turbo_off(void)
{
    char* ta = ta_main();
    if (!s_turbo.live || !ta) {
        tagpu_logf("turbo: off refused: the turbo is not on");
        return;
    }
    s_turbo.live = 0;
    s_turbo.last = 0;               /* the time at the game's own pacing is not the turbo's play */
    tagpu_logf("turbo: off at GameTime %u", *(const unsigned*)(ta + OFF_GAMETIME));
}

/* the level's turbo account; no turbo and no pause carries into the next level */
static void pace_level_end(void)
{
    char* ta = ta_main();
    unsigned g1 = ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0;
    if (s_turbo.engaged)
        tagpu_logf("turbo: from GameTime %u to %u, %u frames at k=%u, %u ticks in %lu ms of play "
                   "(%lu a second), %u left to the lag scale, %u held for the writer",
                   s_turbo.g0, g1, s_turbo.frames, s_turbo_k, s_turbo.ticks,
                   (unsigned long)s_turbo.ms,
                   s_turbo.ms ? (unsigned long)(s_turbo.ticks * 1000ull / s_turbo.ms) : 0ul,
                   s_turbo.lagged, s_turbo.held);
    memset(&s_turbo, 0, sizeof s_turbo);
    s_pause_at = 0;
    s_pause_taken = 0;
}

void tagpu_replay_pacing_init(void)
{
    char b[16] = {0}, *end;
    unsigned long k;
    if (tagpu_sim_lever("tagpu_hold.on")) {
        if (tagpu_regstore_active()) s_lhold.state = LHOLD_ARMED;
        else tagpu_logf("hold: tagpu_hold.on refused: not a test launch (tagpu_regstore.h)");
    }
    if (tagpu_sim_lever_read("tagpu_turbo.on", b, sizeof b) < 0) return;
    k = strtoul(b, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (!*end && strspn(b, " \t\r\n") == strlen(b)) {
        k = 1;                          /* an empty lever: normal speed's batch */
        end = b + strlen(b);
    }
    if (!tagpu_regstore_active())
        tagpu_logf("turbo: tagpu_turbo.on refused: not a test launch (tagpu_regstore.h)");
    else if (end == b || *end || k < 1 || k > 5)
        tagpu_logf("turbo: tagpu_turbo.on refused: \"%s\" is not a k of 1 to 5, the "
                   "scheduler's own range (0x4953F5)", b);
    else
        s_turbo_k = (unsigned)k;
}

static void rt_pace_init(void)
{
    static const unsigned char MAIN[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };
    static const unsigned char COUNT[10] = {
        0x8B, 0x88, 0x3B, 0x8A, 0x03, 0x00, 0x85, 0xC9, 0x74, 0x2D
    };
    s_rt_pace_in = call_is(0x00496928u, 0x00495230u) &&
        tagpu_detour_bytes_ok(0x0049692Du, MAIN, sizeof MAIN) &&
        tagpu_detour_bytes_ok(0x00496932u, COUNT, sizeof COUNT) &&
        call_is(0x0049693Eu, 0x00495490u) &&
        tagpu_detour_observe(0x0049692Du, MAIN, sizeof MAIN, rt_pace_guard, NULL);
    tagpu_logf("replay: tick-batch guard %s", s_rt_pace_in ? "in" : "unavailable");
    if (s_turbo_k)
        tagpu_logf("turbo: k=%u %s", s_turbo_k, s_rt_pace_in ? "at 0x49692D" : "UNAVAILABLE");
    if (s_lhold.state == LHOLD_ARMED) {
        if (!s_rt_pace_in || !s_rw_pause_in) s_lhold.state = LHOLD_OFF;
        tagpu_logf("hold: the launch's hold %s", s_lhold.state ? "armed at 0x49692D and 0x496920"
                   : "UNAVAILABLE: a guard is not in");
    }
}

/* DISASSEMBLED: 0x48587E follows the allocator's complete ten-player sort and
   precedes every owner/block store. Its live stack owns ten pointers at +0x18;
   +0x14 holds the freshly zeroed arena. Reorder only that bounded permutation:
   0x4858BD..0x485925 then initializes player +0x67/+0x6B/+0x6F/+0x71 and unit
   +0x96/+0xFF/+0xAC together, before any constructor can use a block. The
   loader owns this fresh arena/stack. It reads no mutable reader or playback
   state: only the copied plan published before its creation, then atomically
   publishes a result for the first step after load completion. No live unit moves. */
static int __cdecl mp_arena_order(void* entry_esp)
{
    char* ta;
    uint32_t row[RECMP_ACTORS];
    unsigned short slots, each;
    void* arena;
    const RecMpRoster* roster=&s_mp_arena.roster;
    if (InterlockedCompareExchange(&s_mp_arena.state,MP_ARENA_WORKING,MP_ARENA_ARMED)!=MP_ARENA_ARMED)
        return 0;
    ta=ta_main();
    if (!ta) goto refused;
    memcpy(&slots,ta+OFF_NSLOTS,sizeof slots);
    memcpy(&each,ta+OFF_EACH,sizeof each);
    memcpy(&arena,(const char*)entry_esp+0x14,4);
    if (!arena || arena!=*(void**)(ta+OFF_BEGIN) || slots!=roster->slots || each!=roster->each)
        goto refused;
    for (unsigned i=0;i<RECMP_ACTORS;i++) row[i]=(uint32_t)(size_t)player_rec(ta,i);
    if (!tagpu_recmp_arena_order(roster,row,(uint32_t*)((char*)entry_esp+0x18)))
        goto refused;
    InterlockedExchange(&s_mp_arena.state,MP_ARENA_READY);
    tagpu_logf("replay: multiplayer unit blocks ordered: %u actors, %u slots",roster->count,slots);
    return 0;
refused:
    InterlockedExchange(&s_mp_arena.state,MP_ARENA_FAILED);
    tagpu_log("replay: multiplayer allocator ownership refused; no units will be applied");
    return 0;
}

static void mp_arena_init(void)
{
    static const unsigned char FIRST[9]={0x33,0xFF,0xC6,0x85,0xFF,0x00,0x00,0x00,0xFF};
    s_mp_arena_in=tagpu_detour_bytes_ok(0x0048587Eu,FIRST,sizeof FIRST) &&
        tagpu_detour_observe(0x0048587Eu,FIRST,sizeof FIRST,mp_arena_order,NULL);
    tagpu_logf("replay: multiplayer allocator ownership boundary %s",s_mp_arena_in ? "in" : "unavailable");
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

/* THE RESTART GATE. A level's RESTART runs the seat fill itself (0x4997A0: the teardown at
   0x4996AA, the .ota read 0x499820, the fill 0x499825), past both Start sites, from settings the
   teardown has given back to the player: on a puppet level it would start an ordinary skirmish.
   So a puppet level refuses RESTART where the in-game menu takes the button. The restart flag
   main+0x39249 has one setter, 0x460449, in RESTART.GUI's OnCommand 0x460340, which only
   0x4604A0 installs (0x4604CB); 0x4604A0 has one caller, 0x46088F, reached only when the menu's
   name test `0x49FD60(gi, "RESTART")` at 0x460880 answers nonzero. On a puppet level the gate
   answers 0 for that test alone, so the menu takes the click as no button's
   (0x460898..0x46089F answers it) and stays open. The puppet flag is set at the seating and
   cleared by the teardown (tagpu_puppet.h), so it holds for the whole level.
   And the main menu's return site, 0x4270A9 (menu_built). Watch needs both; without them it is
   grey, and a tacli playback goes on as before. */
static void watch_sites_init(void)
{
    enum { MENU_SITE = 0x004270A9u, MENU_BUILD = 0x004263B0u, RESTART_SITE = 0x00460880u,
           NAME_IS = 0x0049FD60u };
    static const unsigned char RESTART_NAME[5] = { 0x68, 0xA8, 0x6C, 0x50, 0x00 };  /* push "RESTART" */
    unsigned char *back, *gate, *p;
    int ok = 1;
    if (!call_is(MENU_SITE, MENU_BUILD) || !call_is(RESTART_SITE, NAME_IS) ||
        memcmp((const void*)(size_t)(RESTART_SITE - 6), RESTART_NAME, sizeof RESTART_NAME)) {
        tagpu_logf("replay: the main menu's builder call 0x4270A9 or the RESTART test 0x460880 "
                   "holds other bytes: no Watch in the browser");
        return;
    }
    back = after_call(MENU_BUILD, menu_built);
    gate = p = tagpu_detour_stub();
    if (!back || !gate) {
        tagpu_detour_stub_free(back);
        tagpu_detour_stub_free(gate);
        tagpu_logf("replay: no memory for the browser's sites: no Watch in the browser");
        return;
    }
    p = tagpu_detour_cmp_flag(p, (volatile unsigned char*)tagpu_puppet_flag());
    *p++ = 0x75; *p++ = 0x05;                           /* jne over the jump   */
    *p++ = 0xE9; tagpu_detour_rel(p, NAME_IS); p += 4;
    *p++ = 0x33; *p++ = 0xC0;                           /* xor eax,eax         */
    *p++ = 0xC2; *p++ = 0x08; *p++ = 0x00;              /* ret 8               */
    call_write(RESTART_SITE, gate, &ok);
    call_write(MENU_SITE, back, &ok);
    if (!ok) {
        tagpu_logf("replay: FAILED a browser site could not be written: no Watch in the browser");
        return;
    }
    s_watchIn = 1;
    tagpu_logf("replay: the main menu's return 0x4270A9 and the RESTART gate 0x460880 in");
}

/* THE PAUSE ICON. DrawGameScreen 0x468CF0 draws the GAF entry `igpaused` (main+0x1481B) while the
   pause bit, bit 0 of main+0x38A51, is set: `test byte [eax+0x38A51],1` at 0x46A107 and `je
   0x46A140` at 0x46A10E, with eax = main from 0x46A102; 0x46A110 draws it and goes on at 0x46A14E,
   as 0x46A140 does after loading the same esi/edi. A recording played with the replay HUD shows no
   icon: the HUD's play button carries the pause (the mockup has none), and the icon sits in the
   middle of the world, under the stats window. The stub takes the icon's branch only when the
   pause bit is set and tagpu_hudinput_on_screen answers 0 -- the frame on the screen shows no HUD,
   its art still loading or its pass down, so the icon is the pause's only sign; it keeps every
   register, and neither continuation reads the flags before setting them. Game thread, inside the
   draw, after the in-play draw's take of the command record. */
static void pause_icon_init(void)
{
    enum { SITE = 0x0046A107u, DRAW = 0x0046A110u, SKIP = 0x0046A140u };
    static const unsigned char STOLEN[9] = { 0xF6, 0x80, 0x51, 0x8A, 0x03, 0x00, 0x01,   /* test   */
                                             0x74, 0x30 };                               /* je +30 */
    static const unsigned char MAIN[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };              /* mov eax,[main] */
    unsigned char *stub, *p;
    if (!tagpu_detour_bytes_ok(SITE, STOLEN, (int)sizeof STOLEN) ||
        memcmp((const void*)(size_t)(SITE - 5), MAIN, sizeof MAIN)) {
        tagpu_logf("replay: the pause icon's test 0x46A107 holds other bytes: a replay shows it");
        return;
    }
    if (!(stub = p = tagpu_detour_stub())) {
        tagpu_logf("replay: no memory for the pause icon's stub: a replay shows it");
        return;
    }
    memcpy(p, STOLEN, 7); p += 7;                       /* test byte [eax+0x38A51],1 */
    *p++ = 0x74; *p++ = 0x10;                           /* jz skip             */
    *p++ = 0x60;                                        /* pushad              */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)tagpu_hudinput_on_screen); p += 4;
    *p++ = 0x85; *p++ = 0xC0;                           /* test eax,eax        */
    *p++ = 0x61;                                        /* popad (ZF kept)     */
    *p++ = 0x75; *p++ = 0x05;                           /* jnz skip            */
    *p++ = 0xE9; tagpu_detour_rel(p, DRAW); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, SKIP); p += 4;     /* skip:               */
    if (!tagpu_detour_land(SITE, stub, (int)sizeof STOLEN)) {
        tagpu_detour_stub_free(stub);
        tagpu_logf("replay: FAILED the pause icon's test 0x46A107 could not be written: a replay "
                   "shows it");
        return;
    }
    tagpu_logf("replay: the pause icon's test 0x46A107 in");
}

void tagpu_replay_init(void)
{
    enum { FILL = 0x0047B092u, PERSIST = 0x0047B09Cu, START1 = 0x00497963u, START2 = 0x00497ACDu,
           PLAYERS = 0x004919C8u, DEATHCUE = 0x004869EBu };
    static const unsigned char SAVE[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };   /* mov eax,[main] */
    unsigned char *fill, *persist, *skip, *cue, *p;
    int ok = 1;
    frame_site_init();
    pause_icon_init();
    rw_pause_init();
    rt_pace_init();
    mp_arena_init();
    if (!call_is(FILL, 0x0047A760u) || !call_is(PERSIST, 0x00430F00u) ||
        !call_is(START1, 0x00496EE0u) || !call_is(START2, 0x00496EE0u) ||
        !call_is(PLAYERS, 0x00464990u) || !call_is(DEATHCUE, 0x00494FF0u)) {
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
    cue = tagpu_detour_stub();
    /* THE PLAYER'S SETTINGS: no site goes in unless the save's observer is in. Without the
       persist's site it stays inert: nothing else puts a recording's options in. */
    if (!fill || !persist || !skip || !cue ||
        !tagpu_detour_observe(0x00430F00u, SAVE, 5, save_before, save_after)) {
        tagpu_detour_stub_free(fill);
        tagpu_detour_stub_free(persist);
        tagpu_detour_stub_free(skip);
        tagpu_detour_stub_free(cue);
        tagpu_logf("replay: no memory for the sites, or the settings save cannot be observed: no "
                   "replays");
        return;
    }
    p = tagpu_detour_cmp_flag(p, (volatile unsigned char*)tagpu_puppet_flag());
    *p++ = 0x75; *p++ = 0x05;                           /* jne over the jump   */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00496EE0u); p += 4;
    *p++ = 0xC2; *p++ = 0x08; *p++ = 0x00;              /* ret 8               */

    /* THE DEATH CUE: the destructor's 0x494FF0(5) at 0x4869EB, stdcall `ret 4` (0x495000), adds 5
       to the music's combat ring when a death's killer byte +0xF4 equals the controlled index
       (0x4869D7..0x4869E3). On a puppet level the controlled record is the viewer's, record 10,
       and +0xF4 holds 10 on most deaths (MEASURED 2026-10-06: 906 of 1448 in the battle fixture,
       every one in the ten-seat fixture), so the playback would lean to the battle music for
       deaths no seat of the viewer's caused. There the stub returns with the argument popped, as
       the cue never fired for a viewer before it had record 10; elsewhere it jumps to the cue.
       The hit cue 0x489DE6 compares owners (+0xFF), which are never 10, and needs no gate. */
    p = cue;
    p = tagpu_detour_cmp_flag(p, (volatile unsigned char*)tagpu_puppet_flag());
    *p++ = 0x75; *p++ = 0x05;                           /* jne over the jump   */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00494FF0u); p += 4;
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;              /* ret 4               */

    /* THE VIEWER'S BUFFERS: the level init's own call, made by the C function itself, which, like
       0x464990, takes no argument and leaves nothing the site reads but the registers C keeps */
    call_write(PLAYERS, (const unsigned char*)(size_t)tagpu_replay_level_players, &ok);
    call_write(START1, skip, &ok);
    call_write(START2, skip, &ok);
    call_write(PERSIST, persist, &ok);
    call_write(FILL, fill, &ok);
    call_write(DEATHCUE, cue, &ok);
    if (!ok) {
        tagpu_logf("replay: FAILED a site could not be written: no request is read");
        return;
    }
    tagpu_logf("replay: 6 sites and the settings save's observer in");
    s_sitesIn = 1;
    watch_sites_init();
}
