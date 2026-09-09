#ifndef TAGPU_MODEL3DO_H
#define TAGPU_MODEL3DO_H
/* tagpu_model3do.h — the engine's per-unit model structures, by offset.

   THE OBJECT3DO is the per-UNIT instance: how many pieces the unit has, the
   body turn, and one PrimitiveStruct per piece carrying that piece's COB turn
   and move, its flags, and a pointer to the posed vertex buffer the engine
   rewrites in place. It dies with the unit (FreeObjectState 0x45AAA0, which
   tagpu_reclaim defers).

   THE MODEL3DONODE is the per-TYPE template: the rest vertices, the faces and
   the piece tree, shared by every unit of a type and freed with the LEVEL
   (0x42DB90 in the teardown cascade), not with any unit — which is why a cache
   keyed on a node pointer needs tagpu_reclaim_level_gen().

   Split out of tagpu_native.c so the geometry bake (tagpu_posebake.c) reads
   the same numbers as the emitters it replaces rather than a second copy of
   them. Derivation and call sites: research/notes/exe-reverse-engineering.md,
   "The repose" and "The 3DO model tree". */

/* The piece-count bound every reader of an Object3do uses. Nothing in the
   engine bounds a model's piece count; 64 was an array size in tagpu_native.c
   and 48 is the replacement-mesh program's GLSL uniform array (TAGPU_HMAXPIECE,
   a different constraint, deliberately left alone). Measured over all 608 models
   of the stock objects3d tree the largest has 36 pieces, so this sits an order
   of magnitude above anything stock content asks for and above any plausible
   mod — which is the point: it stops being a number the design has to reason
   about (gpu-posing.md decision 7). The `PB` is historical: it arrived with the
   pose bake, and it lives here because the bound is a fact about a model, not
   about that module. */
#define TAGPU_PBMAXPIECE 256

#define O3_NUMPARTS  0x00
#define O3_POSEDIRTY 0x08      /* i32: the posed vertex buffers are stale or */
                               /* being rewritten. Set before the rewrite    */
                               /* (0x45AC89 / 0x45AB6C, and by the COB piece */
                               /* MOVE/TURN setters 0x480C90 / 0x480D22),    */
                               /* cleared only after the compose returns     */
                               /* (0x45AD28 / 0x45AC0A) -- and the rewrite is */
                               /* ENTERED only when it is non-zero, so zero   */
                               /* on both sides of a read means the engine    */
                               /* was not touching the buffer                */
#define O3_THISUNIT  0x0C
#define O3_BTURN     0x18      /* u16[3] the body turn the compose folds into */
                               /* the BASE piece's own turn (0x45B0DB): +0x18 */
                               /* <- unit+0x64 (about Z), +0x1A <- unit+0x66  */
                               /* (the yaw, about Y), +0x1C <- unit+0x68      */
                               /* (about X). The cached copy, up to 8 behind  */
                               /* the live unit -- and it is what was baked   */
#define O3_BASEPRIM  0x1E      /* PrimitiveStruct* the reset walk and the     */
                               /* compose both start from                     */
#define O3_PRIM0     0x22
#define PRIM_STRIDE  0x36
#define P_NODE       0x00
#define P_POS        0x04      /* i32[3] 16.16 COB MOVE delta               */
#define P_TURN       0x10      /* u16[3] COB TURN, 65536 = 360 degrees      */
#define P_VBUF       0x22
#define P_FLAGS      0x28
#define N_VCOUNT     0x04
#define N_FCOUNT     0x08
#define N_SELPRIM    0x0C      /* selection primitive index, -1 = none      */
#define N_OFF        0x10      /* i32[3] 16.16 rest offset from the parent  */
#define N_NAME       0x1C      /* char* piece name                          */
#define N_VERTS      0x24      /* raw model-space verts i32[3] 16.16        */
#define N_FACES      0x28
#define N_SIB        0x2C
#define N_CHILD      0x30
#define FACE_STRIDE  0x20
#define F_COLORTAB   0x00
#define F_VCOUNT     0x04
#define F_INDICES    0x0C

#endif
