#ifndef TAGPU_COBSCAN_H
#define TAGPU_COBSCAN_H
/* What a unit's script hides (and marks dont-cache) when the unit is created, read from the
   script and not run, and which pieces of a model that marks. Engine-free and thread-free: no
   engine header, no Win32, no state; every function reads only its arguments. The game's build
   ghost (tagpu_datakeys.c) and the in-game wiki's models both take their hidden pieces from here,
   so the wiki hides exactly what the game's ghost and finished building hide; the wiki's slant
   shadows take the dont-caches from here too.

   A COB block is given as its bytes, its length and a BASE: the value its header and its two
   name tables hold for the block's first byte. The engine's loader 0x4B2450 turns those
   offsets into pointers in place (0x4B24A7..0x4B2527, DISASSEMBLED), so a script the engine
   loaded has base = its own address; a script read from its file has base 0. Every offset is
   the stored value minus the base, in 32-bit arithmetic, and is DATA until bounded by the
   length: the header, each table whole, each name to its NUL. */
#include <stdint.h>

/* the piece bits a script can carry, an order of magnitude above stock (the largest stock model
   has 36 pieces); a script past it gets no mask rather than part of one */
#define TAGPU_COB_MAXPIECES 1024
#define TAGPU_COB_HIDWORDS  (TAGPU_COB_MAXPIECES / 32)

/* Create()'s hides as COB piece bits: from its entry point, through the opcodes of certain
   length, HIDE sets a piece's bit and SHOW clears it; the walk stops at the first opcode it
   cannot size. Conservative by construction: a hide behind a branch, a call or a sleep is not
   seen, and that piece shows.

   Returns the script's piece count with `hid` filled (bits past it clear), 0 for a script with
   no Create(), -1 for a script the block's bounds refuse. `hid` is written only when the
   return is positive. */
int tagpu_cobscan_hides(const char* cob, unsigned size, uint32_t base,
                        uint32_t hid[TAGPU_COB_HIDWORDS]);

/* THE SAME WALK FOR EITHER PAIR OF OPCODES Create() can mark a piece with:
     TAGPU_COB_HIDE       HIDE sets the bit, SHOW clears it (what tagpu_cobscan_hides reads);
     TAGPU_COB_DONTCACHE  DONT_CACHE sets it, CACHE clears it: the pieces whose `cached` flag
                          (prim+0x28 bit 1, on at creation) the script clears, which the
                          structure shadow's raster 0x45A610 then leaves out (build-state.md; a
                          wind generator's rotor, ARMMEX's arms). The wiki's slant shadows read it.
   The same returns as tagpu_cobscan_hides. */
#define TAGPU_COB_HIDE      0
#define TAGPU_COB_DONTCACHE 1
int tagpu_cobscan_create(const char* cob, unsigned size, uint32_t base, int what,
                         uint32_t hid[TAGPU_COB_HIDWORDS]);

/* COB piece k's name and the bytes it has before the block ends; NULL when k is not a piece
   or the name table or the name lies outside the block. The name need not end inside `room`. */
const char* tagpu_cobscan_piece(const char* cob, unsigned size, uint32_t base, int k,
                                unsigned* room);

/* The pieces Create() hides, as bits over the caller's walk of a model: walk[g] is set in
   `bits` (OR-ed in; the caller clears it) when the engine would hide that node.

   `prim` is the model's nodes in the engine's own piece order for a new unit (0x45AEC0: a
   node, its child's whole subtree, then its sibling's; tagpu_model_engine_order), `nprim` their
   count or a negative count for a tree the caller's walk refused, and `pname` each node's name,
   NULL for a name the caller's range test refused. 0x45A950 then, for each COB piece i below
   the primitive count, swaps into position i the FIRST primitive at an index >= i whose name
   _stricmp matches COB piece i's (0x45A9E3..0x45AA70; the relink 0x45AF90 follows), and COB
   piece i drives primitive i whether or not a name matched. DISASSEMBLED. Only positions up to
   the last hidden piece are settled, since a later swap never moves a position below its own
   index. `prim` and `pname` are the caller's scratch and are reordered by the swaps.

   Nodes are compared by address only, never followed. Returns 1 with bits set, 0 when nothing
   is hidden (no script, no Create(), no hide), -1 when the script or the model is refused (a
   bound, a negative or zero `nprim`, a name compare that cannot be answered), -2 for a script
   whose length is 0 (not recorded). `bits` must hold `np` bits. */
int tagpu_cobscan_mask(const char* cob, unsigned size, uint32_t base,
                       const char** prim, const char** pname, int nprim,
                       const char* const* walk, int np, uint32_t* bits);

/* THE POSED ORDER: prim and pname (as above, in the engine's order for a new unit) put in place
   by 0x45A950's swaps for every COB piece below the primitive count, the order whose position i
   COB piece i drives (exe-reverse-engineering.md, *The posed model the opcodes write into*).
   Returns nprim, or -1 when the block's bounds or a name refuse it (prim and pname then hold a
   partial swap). */
int tagpu_cobscan_posed(const char* cob, unsigned size, uint32_t base,
                        const char** prim, const char** pname, int nprim);

/* The mask of either pair (TAGPU_COB_*), by the same swap and the same returns:
   tagpu_cobscan_mask is this with TAGPU_COB_HIDE. */
int tagpu_cobscan_create_mask(const char* cob, unsigned size, uint32_t base, int what,
                              const char** prim, const char** pname, int nprim,
                              const char* const* walk, int np, uint32_t* bits);

#endif
