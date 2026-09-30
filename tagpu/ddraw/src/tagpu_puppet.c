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
       G_DEBUGKEY, G_TRANSFER, G_SAVE, G_COUNT };

static struct { unsigned refused[G_COUNT]; } s_gateDiag;
SIM_VAR(s_gateDiag, SIM_PRIVATE);
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

void tagpu_puppet_refusals(char* out, size_t cap)
{
    const unsigned* r = s_gateDiag.refused;
    snprintf(out, cap, "hits %u, deaths %u, COB sets %u, attaches %u, drops %u, replies %u, "
             "meteor showers %u, typed commands %u, debug keys %u, transfers %u, saves %u",
             r[G_HIT], r[G_DEATH], r[G_SET], r[G_ATTACH], r[G_DROP], r[G_REPLY0] + r[G_REPLY1],
             r[G_METEOR], r[G_COMMAND], r[G_DEBUGKEY], r[G_TRANSFER], r[G_SAVE]);
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
    s_open = 0;
    s_pup.on = 1;
}

void tagpu_puppet_end(void)
{
    s_pup.on = 0;
    s_open = 0;
    memset(s_pup.seat, 0, sizeof s_pup.seat);
    memset(s_pup.local, 0, sizeof s_pup.local);
}
