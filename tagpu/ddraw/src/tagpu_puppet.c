/* tagpu_puppet.c -- the puppet host's table and its gates (tagpu_puppet.h). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_puppet.h"
#include "tagpu_cob_safe.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "tagpu_simreg.h"

typedef struct PuppetTable {
    volatile unsigned char on;              /* this level plays a recording                 */
    unsigned char playing;                  /* ... and its first step has run: the feature
                                               gates close (tagpu_puppet_play). Set once, on
                                               the game thread, after the loader's last read
                                               of it: the loader's feature calls end before
                                               the load word's bit 1 that the first step
                                               waits on (0x497C5F, 0x498342)                */
    unsigned char seat[PUPPET_PLAYERS];     /* the record is a puppet seat, V included      */
    unsigned char local[PUPPET_PLAYERS];    /* getter 75's answer for its units             */
} PuppetTable;
static PuppetTable s_pup;
SIM_VAR(s_pup, SIM_CONTENT);

int tagpu_puppet_on(void) { return s_pup.on; }

int tagpu_puppet_seat(unsigned player)
{
    return s_pup.on && player < PUPPET_PLAYERS && s_pup.seat[player];
}

int tagpu_puppet_local(unsigned player)
{
    return player < PUPPET_PLAYERS ? s_pup.local[player] : 0;
}

const volatile unsigned char* tagpu_puppet_flag(void) { return &s_pup.on; }

/* ---- THE GATES ------------------------------------------------------------------------------

   On a puppet level the recording is the only writer of what it carries, and the viewer's own
   input reaches nothing the recording drives (demo-recorder-s2-host.md §2, §3.2; the events note
   §2.4). Each gate is a prologue gate on the engine function (tagpu_detour_gate) whose predicate
   refuses the call while the level plays a recording; the function then returns at once with 0.
   The unit gates refuse only a call about a puppet seat's unit: its pointer is a slot of the unit
   array by arithmetic before its owner byte +0xFF is read, and a value that is no slot is let
   through, as the engine's own. On any other level no predicate reads engine memory.

   - The damage apply 0x489CE0(rec), `ret 4`: every hit that lands passes it -- the sender's
     local apply (0x489C89, through tagpu_datakeys.c's local_apply), the receivers' 0x0B case and
     B4's companion (0x455417) -- so none lands on a puppet: area damage's per victim (the fire
     spread, whose projectile is owner 10, 0x423BE0 -> 0x49A120), the destructor's cargo loop
     (0x48680B) and the rest. The victim is the record's u16 at +1, bounded by the array's slots
     as B3's own test at 0x489CED bounds it. The sender's forward to a type-3 owner (0x489C8E)
     then sends nothing: 0x451DF0 sends only in a network game, and a puppet level is none.
   - Send_UnitDeath 0x4864B0(unit, kind), `ret 8`: the unit tick's reaper (0x48AFD1) calls it for
     every owner once +0x110 bit 14 is set, and the recording writes that bit (tagpu_replay.c,
     THE APPLY); a puppet dies through its recorded death, replayed through the destructor
     0x4866D0 itself. Its other callers -- the kill-all's (0x486EB2, 0x486EF8, 0x486FB4), the
     occupant CreateFromNetwork destroys (0x486244) and tooling's kills -- are refused for a puppet
     too. The teardown's walk over every unit (0x485980 at 0x491B95) comes after its wrap's entry
     has ended the mode (tagpu_reclaim.c), so it destroys every unit as ever.
   - COB's engine writes, on the script's own unit [[this+model]+0xC] as the functions read it:
     SET 0x480B20(id, value), `ret 8` (ACTIVATION and ARMORED through the state setter 0x48B090,
     YARD_OPEN through 0x47DAC0, +0x10F bits 0, 1 and 3, and +0xBA bit 2 for every id), gated at
     its second instruction 0x480B26, where this is still in ECX and nothing is pushed: the raised
     limits rewrite the first one's displacement (0x480B22) and check it stays theirs; ATTACH
     0x481340(cargo, piece, flags), `ret 0xC`, and DROP 0x4813B0(cargo), `ret 4`, through
     0x48AAC0. The recording carries each: the state byte as events, +0x10F, +0xBA and the carry
     links as images (exe-reverse-engineering.md, the COB-to-engine writers).
   - The two replies 0x47F780 and 0x47F850(unit, kind, text), `ret 0xC`, on a puppet's unit:
     a replayed state change through 0x48B090 would otherwise reply twice, its own and the
     recorded one after it.
   - The meteor shower 0x437DE0(), `ret`: its stones come from the recording, as shots.
   - The viewer's input: typed commands 0x417B50(line, mask), `ret 8` (every one, D4); the
     debug keys 0x4956C0(key), `ret 4` (its one caller 0x4965E1, which can set bit 14 on the
     unit under the cursor); the resource transfer 0x464C60, `ret 0x10` (Give and the SHARE
     dialog); the save 0x4326B0(path, desc, x), `ret 0xC` (a saved puppet level would load as a
     skirmish of seats nothing drives).

   - The features (the slice note's S3a): the feature tick 0x424050 (`ret`, its one caller
     0x495585), the spawn 0x423C50 (`ret 0x14`), the destroy 0x4246B0 (`ret 8`), FeatureDie
     0x423550, the burn 0x4233A0 and the swap 0x423710 (`ret 0xC` each) and the damage 0x4244B0
     (`ret 0x10`), every call from the level's first step: the recording carries the grid and the
     wreck pool, and the replay plays each feature root and the tick through the same function
     (tagpu_replay.c). Before the first step the loader places the map's features through the spawn
     (0x483A7E, 0x483ADE, 0x483B38, and 0x423160's 0x423254), so the gates stay open until the
     replay's first step (tagpu_puppet_play). The analysis found no other caller in play on a puppet
     host -- a puppet's order handlers do not run (type 3, 0x48AEC4), its shots' impacts and its
     deaths' explosions damage no feature (the owner gate, 0x49A01B..0x49A047), typed commands are
     refused -- and each gate names the first call it refuses, so a path the analysis missed shows
     in the log instead of in the world.

   THE REPLAY'S OWN CALLS open the COB and reply gates while they run (tagpu_puppet_open), on
   the game thread. A recorded event nested in a root is recorded after the root's record when the
   root is recorded at its entry, and the replay plays it then, as its own event: every root but
   the create. The create is recorded at its exit, so what it nests -- its state byte, its Create
   script's first slice with the SETs in it (0x485D40 runs it at once), their replies -- is
   recorded before it and finds no unit when played; the replayed create, the same function with
   the recorded arguments, makes those again. A replayed reply is the other own call. Inside
   every other replayed root the gates stay shut: what a start, a state change or a build-done
   nests, the replay plays as its own events, and what a death nests is about a unit the death
   has destroyed. */

#define TA_MAINPP     0x00511DE8u
#define OFF_UFIRST    0x14357       /* the unit array's first slot ...        */
#define OFF_ULAST     0x1435B       /* ... and its last, inclusive            */
#define UNIT_STRIDE   0x118
#define U_OWNER       0xFF
/* this+COB_MODEL: the block whose +0xC is the script's unit; the raised limits grow the COB
   object and move the field, at every site that reads it (tagpu_patches.c, cob_runtime_sites) */
#ifdef TAGPU_LIMITS_STOCK
#define COB_MODEL     0x540u
#else
#define COB_MODEL     TAGPU_COB_MODEL_OFFSET
#endif
#define COB_UNIT      0x0C

enum { G_HIT, G_DEATH, G_SET, G_ATTACH, G_DROP, G_REPLY0, G_REPLY1, G_METEOR, G_COMMAND,
       G_DEBUGKEY, G_TRANSFER, G_SAVE, G_FTICK, G_FSPAWN, G_FDESTROY, G_FDIE, G_FBURN, G_FDAMAGE,
       G_FSWAP, G_COUNT };

static struct { unsigned refused[G_COUNT]; } s_gateDiag;
SIM_VAR(s_gateDiag, SIM_PRIVATE);
/* The replay's own feature call is running: set around that one engine call (tagpu_replay.c,
   feature_root, the tick, a death) on the game thread inside the replay step, so at every tick
   boundary it is 0, its value from load. */
static unsigned char s_fopen;
SIM_VAR(s_fopen, SIM_TRANSIENT);
/* The draws armed for the replay's own feature call: set and cleared around it as s_fopen is. */
static struct { const PuppetDraw* q; unsigned n, next, off; } s_draws;
SIM_VAR(s_draws, SIM_TRANSIENT);
static int s_gatesIn;               /* every gate landed, at attach */
SIM_VAR(s_gatesIn, SIM_INSTALL);
/* The replay's own create or reply is running: set just before that one engine call and cleared
   just after it (tagpu_replay.c, make and ev_reply), on the game thread inside the replay step, so
   at every tick boundary it is 0, its value from load. */
static unsigned char s_open;
SIM_VAR(s_open, SIM_TRANSIENT);

#define E_ARG(E, n)  (((const unsigned*)(E))[n])
#define E_ECX(E)     (*(const unsigned*)((const unsigned char*)(E) - 0x0C))

/* 1 when `u` is a slot of the unit array whose owner is a puppet seat */
static int puppet_unit(const char* u)
{
    const char* ta = *(const char* const*)(size_t)TA_MAINPP;
    const char* first;
    const char* last;
    if (!ta || !u) return 0;
    first = *(const char* const*)(ta + OFF_UFIRST);
    last = *(const char* const*)(ta + OFF_ULAST);
    if (!first || u < first || u > last || (size_t)(u - first) % UNIT_STRIDE) return 0;
    return tagpu_puppet_seat((unsigned char)u[U_OWNER]);
}

static int refuse(int g)
{
    s_gateDiag.refused[g]++;
    return 1;
}

static int __cdecl gate_hit(void* E)
{
    const unsigned char* rec = (const unsigned char*)(size_t)E_ARG(E, 1);
    const char* ta;
    const char* first;
    const char* last;
    unsigned short v;
    if (!s_pup.on || !rec || !(ta = *(const char* const*)(size_t)TA_MAINPP)) return 0;
    first = *(const char* const*)(ta + OFF_UFIRST);
    last = *(const char* const*)(ta + OFF_ULAST);
    memcpy(&v, rec + 1, 2);
    if (!first || last < first || !v || v > (unsigned)(last - first) / UNIT_STRIDE) return 0;
    return puppet_unit(first + (size_t)v * UNIT_STRIDE) && refuse(G_HIT);
}

static int __cdecl gate_death(void* E)
{
    return s_pup.on && puppet_unit((const char*)(size_t)E_ARG(E, 1)) && refuse(G_DEATH);
}

/* the script's unit, as the COB functions read it */
static int cob_puppet(void* E)
{
    const char* cob = (const char*)(size_t)E_ECX(E);
    const char* owner;
    if (!cob || !(owner = *(const char* const*)(cob + COB_MODEL))) return 0;
    return puppet_unit(*(const char* const*)(owner + COB_UNIT));
}

static int cob_refused(void* E) { return s_pup.on && !s_open && cob_puppet(E); }

static int __cdecl gate_set(void* E) { return cob_refused(E) && refuse(G_SET); }
static int __cdecl gate_attach(void* E) { return cob_refused(E) && refuse(G_ATTACH); }
static int __cdecl gate_drop(void* E) { return cob_refused(E) && refuse(G_DROP); }

static int reply_refused(void* E)
{
    return s_pup.on && !s_open && puppet_unit((const char*)(size_t)E_ARG(E, 1));
}

static int __cdecl gate_reply0(void* E) { return reply_refused(E) && refuse(G_REPLY0); }
static int __cdecl gate_reply1(void* E) { return reply_refused(E) && refuse(G_REPLY1); }
static int __cdecl gate_meteor(void* E) { (void)E; return s_pup.on && refuse(G_METEOR); }
static int __cdecl gate_command(void* E) { (void)E; return s_pup.on && refuse(G_COMMAND); }
static int __cdecl gate_debugkey(void* E) { (void)E; return s_pup.on && refuse(G_DEBUGKEY); }
static int __cdecl gate_transfer(void* E) { (void)E; return s_pup.on && refuse(G_TRANSFER); }
static int __cdecl gate_save(void* E) { (void)E; return s_pup.on && refuse(G_SAVE); }

static int feature_refused(void* E, int g)
{
    static const char* const what[] = { "the feature tick", "the spawn", "the destroy",
                                        "FeatureDie", "the burn", "the damage", "the swap" };
    if (!s_pup.on || !s_pup.playing || s_fopen) return 0;
    if (!s_gateDiag.refused[g])
        tagpu_logf("puppet: refused %s, called from 0x%06X: the recording is its only writer",
                   what[g - G_FTICK], E_ARG(E, 0) - 5);
    return refuse(g);
}

static int __cdecl gate_ftick(void* E) { return feature_refused(E, G_FTICK); }
static int __cdecl gate_fspawn(void* E) { return feature_refused(E, G_FSPAWN); }
static int __cdecl gate_fdestroy(void* E) { return feature_refused(E, G_FDESTROY); }
static int __cdecl gate_fdie(void* E) { return feature_refused(E, G_FDIE); }
static int __cdecl gate_fburn(void* E) { return feature_refused(E, G_FBURN); }
static int __cdecl gate_fdamage(void* E) { return feature_refused(E, G_FDAMAGE); }
static int __cdecl gate_fswap(void* E) { return feature_refused(E, G_FSWAP); }

typedef struct Gate {
    unsigned va;
    unsigned char n, retn, stolen[8];
    tagpu_detour_before_fn pred;
} Gate;
static const Gate GATES[G_COUNT] = {
    { 0x00489CE0u, 8, 0x04, { 0x53, 0x56, 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 }, gate_hit },
    { 0x004864B0u, 5, 0x08, { 0x83, 0xEC, 0x10, 0x53, 0x55 }, gate_death },
    { 0x00480B26u, 5, 0x08, { 0x8B, 0x4C, 0x24, 0x04, 0x56 }, gate_set },
    { 0x00481340u, 5, 0x0C, { 0x8B, 0x44, 0x24, 0x04, 0x56 }, gate_attach },
    { 0x004813B0u, 5, 0x04, { 0x8B, 0x44, 0x24, 0x04, 0x56 }, gate_drop },
    { 0x0047F780u, 6, 0x0C, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, gate_reply0 },
    { 0x0047F850u, 5, 0x0C, { 0x57, 0x8B, 0x7C, 0x24, 0x08 }, gate_reply1 },
    { 0x00437DE0u, 6, 0x00, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, gate_meteor },
    { 0x00417B50u, 6, 0x08, { 0x81, 0xEC, 0xD4, 0x00, 0x00, 0x00 }, gate_command },
    { 0x004956C0u, 5, 0x04, { 0x8B, 0x44, 0x24, 0x04, 0x53 }, gate_debugkey },
    { 0x00464C60u, 5, 0x10, { 0x8B, 0x44, 0x24, 0x04, 0x53 }, gate_transfer },
    { 0x004326B0u, 6, 0x0C, { 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00 }, gate_save },
    { 0x00424050u, 5, 0x00, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, gate_ftick },
    { 0x00423C50u, 5, 0x14, { 0x83, 0xEC, 0x28, 0x53, 0x55 }, gate_fspawn },
    { 0x004246B0u, 7, 0x08, { 0x53, 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, gate_fdestroy },
    { 0x00423550u, 5, 0x0C, { 0x83, 0xEC, 0x08, 0x53, 0x55 }, gate_fdie },
    { 0x004233A0u, 8, 0x0C, { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04 }, gate_fburn },
    { 0x004244B0u, 6, 0x10, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, gate_fdamage },
    { 0x00423710u, 8, 0x0C, { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04 }, gate_fswap },
};

void tagpu_puppet_init(void)
{
    unsigned i;
    for (i = 0; i < G_COUNT; i++)
        if (tagpu_detour_landed(GATES[i].va, NULL, NULL) ||
            memcmp((const void*)(size_t)GATES[i].va, GATES[i].stolen, GATES[i].n)) {
            tagpu_logf("puppet: the gate site 0x%06X holds other bytes: no gates, and no "
                       "recording will play", GATES[i].va);
            return;
        }
    for (i = 0; i < G_COUNT; i++)
        if (!tagpu_detour_gate(GATES[i].va, GATES[i].stolen, GATES[i].n, GATES[i].pred,
                               GATES[i].retn)) {
            tagpu_logf("puppet: FAILED the gate at 0x%06X after %u others: those stay inert, "
                       "and no recording will play", GATES[i].va, i);
            return;
        }
    s_gatesIn = 1;
    tagpu_logf("puppet: %u gates in", (unsigned)G_COUNT);
}

int tagpu_puppet_gates_in(void) { return s_gatesIn; }

void tagpu_puppet_open(int open) { s_open = open ? 1 : 0; }

/* ---- THE FOOTPRINT WRITERS ------------------------------------------------------------------

   On a puppet level the recording is the only writer of the grid's unit slots and yard bit too
   (a cell's +0x00 and +0x02, and +0x0C bit 1): the cell images carry them (tagpu_replay.c, THE
   FEATURES). Three engine functions write them, each (unit) `ret 4`: the stamp 0x47CC30, the clear
   0x47D0E0 and the re-claim 0x47C790. On a puppet host they run for the replay's placements (the
   set-position 0x48A9F0 calls the clear and the stamp, 0x48AA6B and 0x48AA9A), its creates and
   deaths, and the host's own unit tick, and none can be refused: the stamp also files the unit in
   its sort bucket (+0x82, +0x8E and the bucket's head, 0x47CCA9..0x47CD25), which the image does
   not write, and the stamp and the clear recompute the cells' height extremes (0x483210, the same
   on both machines). Left alone they write the host's timing into the grid: in play an aircraft's
   slot follows its position by an iteration, while the replay's placement stamps it at the
   image's position, an iteration early (MEASURED 2026-09-30, the battle: at GameTime 1280 one
   cell's slot B held the host's unit 858 where the recording's held 854).

   So each call's grid writes are undone at its return. The recorder's observer at each entry hands
   the unit over (tagpu_puppet.h); this saves the unit's footprint -- +0x76, +0x78 by +0x7E, +0x80,
   i16 each, a cell wider each way, clipped to the grid -- and puts the slots and the yard bit back
   when the function returns. Each of the three writes only inside that footprint (0x47CD31..
   0x47D03A, 0x47D105..0x47D23B, 0x47C7C9..0x47CA8E). The clear nests the re-claim: for a unit
   with +0x110 bit 26 it walks the buckets around (0x47D2C4, the walk 0x47E5C0) with the callback
   0x47ED30 stored at 0x4FD660, which re-claims each unit it is handed (0x47ED35). Nothing else
   nests: no other code path from the three reaches one of them, none of the three is stored as a
   pointer, and the callback is passed only by the clear and the saved game's 0x47DB20 (and by
   0x47CAC0, which nothing calls). So the frames are a LIFO, two deep in play; an entry past
   FOOT_DEPTH is counted, the first named, and not undone. A frame's undo puts back the cells it
   saved, so an outer frame also undoes what the calls it nests wrote inside its rectangle. A
   footprint wider than FOOT_SAVE either way is data the save cannot hold: counted, the first
   named, its writes stay -- and the harness names the cells. The passability refresh 0x440A40
   sees the call's own writes; the recording does not carry passability (the slice note's S3a). */

#define OFF_GRID     0x14287        /* the cells, 13 bytes each, row-major ... */
#define OFF_GRID_W   0x14233        /* ... W columns ...                       */
#define OFF_GRID_H   0x14237        /* ... by H rows (0x481550)                */
#define CELL_BYTES   13
#define CELL_SLOTS   4              /* slot A +0x00 and slot B +0x02           */
#define CELL_FLAGS   0x0C
#define CELL_YARD    0x02
#define U_FOOT_X     0x76           /* the footprint's cell, i16 each ...      */
#define U_FOOT_Z     0x78
#define U_FOOT_W     0x7E           /* ... and its size (the def's +0x14A, 0x485AA4) */
#define U_FOOT_H     0x80
#define FOOT_SAVE    64
#define FOOT_DEPTH   4

typedef struct FootFrame {
    unsigned ret;                   /* the writer's return address              */
    unsigned char* first;           /* the saved rectangle's first cell         */
    int cols, rows, pitch;          /* its columns and rows, the grid's row bytes */
    unsigned char cell[(FOOT_SAVE + 2) * (FOOT_SAVE + 2)][CELL_SLOTS + 1];
} FootFrame;
/* the open calls, innermost last: 0 deep at every tick boundary, each call opening and closing
   inside one engine call on the game thread */
static struct { unsigned depth; FootFrame f[FOOT_DEPTH]; } s_foot;
SIM_VAR(s_foot, SIM_TRANSIENT);
static struct { unsigned undone, nested, wide; } s_footDiag;
SIM_VAR(s_footDiag, SIM_PRIVATE);

int tagpu_puppet_foot_enter(const char* unit, unsigned ret)
{
    const char* ta;
    const char* first;
    const char* last;
    unsigned char* grid;
    FootFrame* fr;
    int W, H, x0, z0, x1, z1, w, h, x, z, k;
    if (!s_pup.on || !(ta = *(const char* const*)(size_t)TA_MAINPP)) return 0;
    if (s_foot.depth >= FOOT_DEPTH) {
        if (!s_footDiag.nested++)
            tagpu_logf("puppet: a footprint writer called from 0x%06X inside %d open ones, the "
                       "innermost called from 0x%06X: its grid writes stay", ret - 5, FOOT_DEPTH,
                       s_foot.f[FOOT_DEPTH - 1].ret - 5);
        return 0;
    }
    first = *(const char* const*)(ta + OFF_UFIRST);
    last = *(const char* const*)(ta + OFF_ULAST);
    if (!first || !unit || unit < first || unit > last || (size_t)(unit - first) % UNIT_STRIDE)
        return 0;
    grid = *(unsigned char* const*)(ta + OFF_GRID);
    W = *(const int*)(ta + OFF_GRID_W);
    H = *(const int*)(ta + OFF_GRID_H);
    w = *(const short*)(unit + U_FOOT_W);
    h = *(const short*)(unit + U_FOOT_H);
    if (!grid || W <= 0 || H <= 0 || w <= 0 || h <= 0) return 0;
    if (w > FOOT_SAVE || h > FOOT_SAVE) {
        if (!s_footDiag.wide++)
            tagpu_logf("puppet: a footprint writer called from 0x%06X on a %d x %d footprint, "
                       "wider than the %d cells its undo holds: its grid writes stay", ret - 5, w,
                       h, FOOT_SAVE);
        return 0;
    }
    x0 = *(const short*)(unit + U_FOOT_X) - 1;
    z0 = *(const short*)(unit + U_FOOT_Z) - 1;
    x1 = x0 + w + 2;
    z1 = z0 + h + 2;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 > W) x1 = W;
    if (z1 > H) z1 = H;
    if (x0 >= x1 || z0 >= z1) return 0;
    fr = &s_foot.f[s_foot.depth];
    fr->first = grid + ((size_t)z0 * (size_t)W + (size_t)x0) * CELL_BYTES;
    fr->cols = x1 - x0;
    fr->rows = z1 - z0;
    fr->pitch = W * CELL_BYTES;
    for (z = k = 0; z < fr->rows; z++)
        for (x = 0; x < fr->cols; x++, k++) {
            const unsigned char* c = fr->first + (size_t)z * fr->pitch + (size_t)x * CELL_BYTES;
            memcpy(fr->cell[k], c, CELL_SLOTS);
            fr->cell[k][CELL_SLOTS] = c[CELL_FLAGS];
        }
    fr->ret = ret;
    s_foot.depth++;
    return 1;
}

unsigned tagpu_puppet_foot_leave(void)
{
    FootFrame* fr = &s_foot.f[--s_foot.depth];
    int x, z, k;
    for (z = k = 0; z < fr->rows; z++)
        for (x = 0; x < fr->cols; x++, k++) {
            unsigned char* c = fr->first + (size_t)z * fr->pitch + (size_t)x * CELL_BYTES;
            memcpy(c, fr->cell[k], CELL_SLOTS);
            c[CELL_FLAGS] = (unsigned char)((c[CELL_FLAGS] & ~CELL_YARD) |
                                            (fr->cell[k][CELL_SLOTS] & CELL_YARD));
        }
    s_footDiag.undone++;
    return fr->ret;
}

void tagpu_puppet_refusals(char* out, size_t cap)
{
    const unsigned* r = s_gateDiag.refused;
    snprintf(out, cap, "hits %u, deaths %u, COB sets %u, attaches %u, drops %u, replies %u, "
             "meteor showers %u, typed commands %u, debug keys %u, transfers %u, saves %u, "
             "feature ticks %u, feature calls %u", r[G_HIT], r[G_DEATH], r[G_SET], r[G_ATTACH],
             r[G_DROP], r[G_REPLY0] + r[G_REPLY1], r[G_METEOR], r[G_COMMAND], r[G_DEBUGKEY],
             r[G_TRANSFER], r[G_SAVE], r[G_FTICK], r[G_FSPAWN] + r[G_FDESTROY] + r[G_FDIE] +
             r[G_FBURN] + r[G_FDAMAGE] + r[G_FSWAP]);
}

void tagpu_puppet_footprints(char* out, size_t cap)
{
    snprintf(out, cap, "%u undone, %u not: %u entered inside %d open ones, %u wider than %d "
             "cells", s_footDiag.undone, s_footDiag.nested + s_footDiag.wide, s_footDiag.nested,
             FOOT_DEPTH, s_footDiag.wide, FOOT_SAVE);
}

void tagpu_puppet_play(void) { s_pup.playing = 1; }

int tagpu_puppet_features(int open)
{
    int was = s_fopen;
    s_fopen = open ? 1 : 0;
    return was;
}

void tagpu_puppet_draws(const PuppetDraw* q, unsigned n)
{
    s_draws.q = q;
    s_draws.n = n;
    s_draws.next = 0;
    s_draws.off = 0;
}

int tagpu_puppet_draw(unsigned site, unsigned n, unsigned* v)
{
    const PuppetDraw* d;
    if (!s_draws.q || s_draws.off) return 0;
    if (s_draws.next >= s_draws.n) {
        s_draws.off = 1;
        return 0;
    }
    d = &s_draws.q[s_draws.next];
    if (d->site != site || d->n != n) {
        s_draws.off = 1;
        return 0;
    }
    s_draws.next++;
    *v = d->v;
    return 1;
}

unsigned tagpu_puppet_draws_end(void)
{
    unsigned left = s_draws.n - s_draws.next + s_draws.off;
    s_draws.q = NULL;
    s_draws.n = s_draws.next = s_draws.off = 0;
    return left;
}

void tagpu_puppet_begin(const unsigned char seat[PUPPET_PLAYERS],
                        const unsigned char local[PUPPET_PLAYERS])
{
    unsigned i;
    for (i = 0; i < PUPPET_PLAYERS; i++) {
        s_pup.seat[i] = seat[i] ? 1 : 0;
        s_pup.local[i] = local[i] ? 1 : 0;
    }
    memset(&s_gateDiag, 0, sizeof s_gateDiag);
    memset(&s_footDiag, 0, sizeof s_footDiag);
    s_open = 0;
    s_fopen = 0;
    s_pup.playing = 0;
    s_pup.on = 1;
}

void tagpu_puppet_end(void)
{
    s_pup.on = 0;
    s_pup.playing = 0;
    s_open = 0;
    s_fopen = 0;
    memset(s_pup.seat, 0, sizeof s_pup.seat);
    memset(s_pup.local, 0, sizeof s_pup.local);
}
