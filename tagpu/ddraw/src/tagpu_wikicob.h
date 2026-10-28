#ifndef TAGPU_WIKICOB_H
#define TAGPU_WIKICOB_H
/* tagpu_wikicob -- the in-game wiki's COB host: every unit's script run by tagpu_cobvm on the
   wiki's own world (research/notes/ingame-wiki-cob-plan.md, "The wiki host").

   ONE HOST FOR BOTH SIMULATIONS. The DLL's tagpu_wikisim.c calls this file in-process; the
   mockup's sim.js calls the same file through tools/wiki/serve.py (ctypes); wikitest.c links it
   for the parity check. What the two simulations implement each is only WHEN they call it -- the
   engine's calls for the actions the wiki performs, at the same points of the same tick -- and
   tagpu_wikicob_hash, which both fold into their state hash, holds them to that. Everything about
   a script itself is decided here: the program a type runs, each unit's four blocks, what GET
   answers, what a SET changes, the generator RAND draws from, a refusal and its quarantine.

   THE RULES ARE IMPURE'S (TAGPU_COBVM_IMPURE): ten records, the raised build's guarded
   interpreter, the getters of tagpu_cob_getter over the wiki's units, as the game the wiki
   teaches runs them.

   ENGINE-FREE, WIN32-FREE, WITHOUT GLOBALS: a WIKICOB is its caller's and one thread uses it at a
   time; two threads may each run their own (tagpu_cobvm.h, SAFE ON SEVERAL THREADS).

   BOUNDED BY CONSTRUCTION: at most WIKICOB_TYPES types, WIKICOB_UNITS units over a host's life,
   WIKICOB_PIECES primitives a model; a spawn past the unit bound gets no handle (0), which the
   simulation treats as a unit with no script, its gates open, so a player's placements cannot
   grow it without bound.

   THE QUARANTINE (the plan's decision 9). A type whose script the validator refuses, or one of
   whose units' VMs faults, is quarantined for the host's life: every unit of it drops its VM,
   stands at rest (the caller draws Create()'s hides, as before the host existed), and reads
   every gate as open (INBUILDSTANCE, ACTIVATION and YARD_OPEN 1). The first refusal of a type is
   kept as its note (tagpu_wikicob_note). A type with no script at all is quarantined the same
   way, with that note. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_cobvm.h"

#define WIKICOB_TYPES   64          /* WIKI_MAX_DEFS: the unit types of one entry              */
#define WIKICOB_UNITS   1024        /* units spawned over one host's life                     */
#define WIKICOB_PIECES  256         /* TAGPU_PBMAXPIECE: the readers' piece bound              */
#define WIKICOB_VALUES  21          /* the retail SET/GET ids 0..20 (file-formats.md 2.5)      */
#define WIKICOB_NOTE    192

/* the unit values a script SETs and a simulation reads (tools/tacob VALUE_IDS) */
enum {
    WIKICOB_ACTIVATION = 1, WIKICOB_STANDINGMOVEORDERS = 2, WIKICOB_STANDINGFIREORDERS = 3,
    WIKICOB_HEALTH = 4, WIKICOB_INBUILDSTANCE = 5, WIKICOB_BUSY = 6, WIKICOB_PIECE_XZ = 7,
    WIKICOB_PIECE_Y = 8, WIKICOB_UNIT_XZ = 9, WIKICOB_UNIT_Y = 10, WIKICOB_UNIT_HEIGHT = 11,
    WIKICOB_XZ_ATAN = 12, WIKICOB_XZ_HYPOT = 13, WIKICOB_ATAN = 14, WIKICOB_HYPOT = 15,
    WIKICOB_GROUND_HEIGHT = 16, WIKICOB_BUILD_PERCENT_LEFT = 17, WIKICOB_YARD_OPEN = 18,
    WIKICOB_BUGGER_OFF = 19, WIKICOB_ARMORED = 20
};

/* How a call enters the core: the engine's entries (tagpu_cobvm.h). NOW is a start with run-now
   1 and no arguments (0x4B0940: Create, StartMoving, StopMoving, MoveRate*); LATER one with
   run-now 0 and no arguments (Activate and Deactivate through 0x4B0940; a mobile builder's
   StopBuilding by index through 0x4B0B00 at 0x43A251); ARGS one with run-now 0 and the arguments
   (0x4B0B00 / 0x4B0A70: StartBuilding, SetSpeed, SetDirection). */
enum { WIKICOB_NOW = 0, WIKICOB_LATER = 1, WIKICOB_ARGS = 2 };

/* A type's script and the facts of its model the host needs, all borrowed for the call that
   takes them: the script file's bytes (NULL: the type has none), and the posed model's primitive
   count and, per primitive in the posed order (the COB's piece order after 0x45A950's swaps),
   1 when the model builder leaves it visible (0x45AF1B: three or more vertices). pieces 0: no
   model, which quarantines the type as no script does.

   THE MODEL'S SHAPE, for a piece's origin (tagpu_wikicob_origin) and the nano spray's target box:
   per primitive in the posed order its node's offset from its parent's, three 16.16 words in the
   axes the engine holds a model in (the loader 0x4CB590 negates X and Z), and its parent's
   primitive index, -1 for a root; NULL leaves every offset 0 and every primitive a root. `top` is
   0x4CB5F0's height of the model, 16.16: over the root's sibling chain, the most of 0 and, for
   each node, its vertices' y and its children's height, each plus the node's y offset
   (0x4CB5F0..0x4CB641, DISASSEMBLED), which the def loader stores at UnitDef+0x16E (0x42D7BF). */
typedef struct {
    const void*          cob;
    size_t               ncob;
    int                  pieces;
    const unsigned char* visible;
    const int32_t*       offset;        /* 3 words a primitive */
    const int16_t*       parent;
    int32_t              top;
} WIKICOBTYPE;

/* One unit as GET sees it, which the simulation hands with every call that can run script code.
   `unit` is the handle tagpu_wikicob_spawn gave it; x and z are its centre in 16.16 world units,
   truncated toward zero and held to the int32 range (both simulations truncate the same
   doubles); `left` is the build fraction still to go, as the float the engine's unit+0x104
   holds; `health` its HEALTH in percent. */
typedef struct {
    int32_t unit, owner;
    int32_t x, z;
    float   left;
    int32_t health;
} WIKICOBFACT;

typedef struct WIKICOB WIKICOB;

/* A host for `ntypes` types (index = the simulation's type index), its generator seeded with
   `seed`. Every program is made here; a refused one quarantines its type at once. NULL with a
   message when memory runs out or ntypes is outside 0..WIKICOB_TYPES. The type facts are copied:
   the caller's bytes may go once this returns. */
WIKICOB* tagpu_wikicob_new(int ntypes, const WIKICOBTYPE* types, uint32_t seed, char* err, size_t errn);
void     tagpu_wikicob_free(WIKICOB* w);

/* A unit of type `type` appears (a scene's spawn, a nanoframe placed): its blocks are made and
   Create starts run-now (0x485DE6), with `facts` (its own row included) answering its GETs.
   Returns its handle, 1 .. WIKICOB_UNITS, in spawn order whatever happens to its script; 0 past
   the bound or for a type outside the host. */
int  tagpu_wikicob_spawn(WIKICOB* w, int type, const WIKICOBFACT* facts, int nfacts);

/* The unit leaves the world (a reclaim's end): its VM goes, its handle is never reused. */
void tagpu_wikicob_remove(WIKICOB* w, int unit);

/* One engine call on a unit's script by name, through the entry `how` names (WIKICOB_*), with
   `argc` words of `args` for ARGS. A script the program lacks is a refused start, as the engine's
   0x4B08C0 refuses it, and changes nothing. Returns 1 started, 0 refused or no VM, -1 when the
   VM faulted (the type is quarantined by then). */
int  tagpu_wikicob_call(WIKICOB* w, int unit, const char* script, int how, int argc, const int32_t* args,
                        const WIKICOBFACT* facts, int nfacts);

/* The state setter 0x48B090(unit, 1, on), as the engine calls it on a structure finished (or
   created whole) whose FBI says ActivateWhenBuilt (0x41B9A6, 0x48612F): nothing when ACTIVATION
   already reads `on` (0x48B0D6); else the value changes and Activate or Deactivate starts
   run-later (0x48B106, 0x48B12B). A script's SET ACTIVATION takes the same rule. Returns 1
   started, 0 nothing to start, -1 when the VM faulted. */
int  tagpu_wikicob_activate(WIKICOB* w, int unit, int on);

/* A query (0x4B0BC0 by name, entered through 0x4B0C40): `script` runs inside the call with the
   four words of `io` on its stack, run-now, and the four come back in `io` -- QueryNanoPiece's
   first word is the piece its spray starts at (0x43E400). A script the program lacks, or a unit
   with no VM, is a refused query: `io` is left as it was, as the engine leaves the caller's
   preset. Returns 1 answered, 0 refused, -1 when the VM faulted (the type is quarantined by then). */
int  tagpu_wikicob_query(WIKICOB* w, int unit, const char* script, int32_t io[4],
                         const WIKICOBFACT* facts, int nfacts);

/* A PIECE'S ORIGIN, 0x43DEF0's arithmetic on the unit's posed model as its script left it (a unit
   with no VM: its rest pose), in 16.16 in the model axes WIKICOBTYPE's offsets are in: the
   piece's offset plus its MOVE, carried up its parents -- each turned by the parent's TURN
   through 0x4B6CC0 and moved by the parent's offset plus MOVE -- the piece's own TURN never
   applied. The root's offset plus MOVE is `base`, which the body turn does not move, and the
   rest, turned by the root's TURN, is `turned`, which the engine turns by the body's angles
   folded into the root's (0x43DF86): the caller turns it by the unit's facing, which the host
   does not hold, and adds `base`. The engine then negates z (0x43E00A); here z stays in the model
   axes. Each pair rotation is 0x4B7173's -- skipped for a zero angle, else rounded to the nearest
   integer, ties to even, as its fistp rounds -- with sine and cosine from + - * / alone, so that
   every compiler gives the same words; they can differ from the engine's x87 by one unit at a
   rounding boundary. A piece outside the model (DATA: a script's answer, bounded by the
   primitive count) or a type with no model answers 0 and both all zero, the unit's own point, as
   0x43DF16..0x43E01D answers. Returns 1 for a piece of the model. */
int  tagpu_wikicob_origin(const WIKICOB* w, int unit, int piece, int32_t turned[3], int32_t base[3]);

/* The model's height of type `type` (WIKICOBTYPE.top), 0 for a type outside the host. */
int32_t tagpu_wikicob_top(const WIKICOB* w, int type);

/* The slice: DoScriptsNow(1) for every live unit with a VM, in spawn order (0x48ADEB). */
void tagpu_wikicob_slice(WIKICOB* w, const WIKICOBFACT* facts, int nfacts);

/* The value `id` (WIKICOB_*) of a unit as its script last SET it, 0 for one it never set; 1 for
   the three gates of a unit without a VM (quarantined, scriptless, past the bound). 0 for an
   unknown handle or id. */
int32_t tagpu_wikicob_value(const WIKICOB* w, int unit, int id);

/* 1 when the unit runs no script (its type quarantined, or past the bound), else 0. */
int  tagpu_wikicob_resting(const WIKICOB* w, int unit);

/* The unit's posed model block, TAGPU_COBVM_MODEL_BYTES(*nprim) bytes, its PrimitiveStructs in
   the posed order; NULL for a resting unit. Valid until the next call on `w`. */
const unsigned char* tagpu_wikicob_model(const WIKICOB* w, int unit, int* nprim);

/* The first refusal of type `type`, "" when it has none: "<what>: <reason>", `what` the call the
   refusal came in ("the slice", a script's name, "the program", "no script", "no model"). The
   core's run record carries no pc for a fault, so the note names none. */
const char* tagpu_wikicob_note(const WIKICOB* w, int type);

/* 64-bit FNV-1a over everything the host holds that a call can change, continued from `h`: the
   generator, each type's quarantine, and each unit in spawn order -- live or not, its values, and
   with a VM its object's records and count, its statics, its animation block and its model. */
uint64_t tagpu_wikicob_hash(const WIKICOB* w, uint64_t h);

#endif
