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
   interpreter, the getters of tagpu_cob_getter over this host's units, as the game the wiki
   teaches runs them.

   ENGINE-FREE, WIN32-FREE, WITHOUT GLOBALS: a WIKICOB is its caller's and one thread uses it at a
   time; two threads may each run their own (tagpu_cobvm.h, SAFE ON SEVERAL THREADS).

   BOUNDED BY CONSTRUCTION: at most WIKICOB_TYPES types, WIKICOB_UNITS units over a host's life,
   WIKICOB_PIECES primitives a model; a spawn past the unit bound gets no script (its gates read
   open, as a quarantined type's do), so a player's placements cannot grow it without bound.

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

/* How a call enters the core: the engine's entries (tagpu_cobvm.h). NOW is 0x4B0940 with
   run-now 1 (Create, StartMoving, StopMoving, MoveRate*), LATER 0x4B0940 with run-now 0
   (Activate, Deactivate, StopBuilding), ARGS 0x4B0B00 / 0x4B0A70 with run-now 0 and the
   arguments (StartBuilding, SetSpeed, SetDirection). */
enum { WIKICOB_NOW = 0, WIKICOB_LATER = 1, WIKICOB_ARGS = 2 };

/* A type's script and the facts of its model the host needs, all borrowed for the call that
   takes them: the script file's bytes (NULL: the type has none), and the posed model's primitive
   count and, per primitive in the posed order (the COB's piece order after 0x45A950's swaps),
   1 when the model builder leaves it visible (0x45AF1B: three or more vertices). pieces 0: no
   model, which quarantines the type as no script does. */
typedef struct {
    const void*          cob;
    size_t               ncob;
    int                  pieces;
    const unsigned char* visible;
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

/* The first refusal of type `type`, "" when it has none: "<script> pc <pc>: <reason>". */
const char* tagpu_wikicob_note(const WIKICOB* w, int type);

/* 64-bit FNV-1a over everything the host holds that a call can change, continued from `h`: the
   generator, each type's quarantine, and each unit in spawn order -- live or not, its values, and
   with a VM its object's records and count, its statics, its animation block and its model. */
uint64_t tagpu_wikicob_hash(const WIKICOB* w, uint64_t h);

#endif
