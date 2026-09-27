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
   keyed on a node pointer needs the LEVEL GENERATION, and specifically the
   frame packet's, which advances at every level end whether or not
   tagpu_reclaim is armed (tagpu_packet.h `level_gen`).

   Split out of tagpu_native.c so the geometry bake (tagpu_posebake.c) reads
   the same numbers as the emitters it replaces rather than a second copy of
   them. Derivation and call sites: research/notes/exe-reverse-engineering.md,
   "The repose" and "The 3DO model tree". */

#include <stddef.h>
#include "tagpu_addr.h"

/* The piece-count bound every reader of an Object3do uses. Nothing in the
   engine bounds a model's piece count. Measured over all 608 models
   of the stock objects3d tree the largest has 36 pieces, so this sits an order
   of magnitude above anything stock content asks for and above any plausible
   mod — which is the point: it stops being a number the design has to reason
   about (gpu-posing.md decision 7). It lives here because the bound is a fact
   about a model, not about the pose bake. */
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
#define F_COLORTAB   0x00      /* the flat colour: 0x4C0330 fills with its low */
                               /* byte (`mov al,[esp+0x14080]` @0x4C0678)      */
#define F_VCOUNT     0x04
#define F_INDICES    0x0C
/* THE EFFECTS DRAWS' FACE WORDS (0x46BAE0 and 0x4211D0, DISASSEMBLED). The
   unit rasteriser does not read +0x1C the same way; these are the two draws
   that decide a face by it. */
#define F_TEXFRAME   0x10      /* GAFFrame* when F_FLAGS bit 1 is clear; when */
                               /* set, the u16 frame index of an anim state   */
                               /* {u16 frame @0, seq @+8} (0x4B7EE0)          */
#define F_TEXSEQ     0x18      /* the anim state's sequence: u16 nframes @0,  */
                               /* {GAFFrame*, dword} x nframes from +0x28     */
#define F_FLAGS      0x1C      /* bit 0 flat fill (any vertex count), else    */
                               /* only a 4-vertex face is drawn, textured;    */
                               /* bit 1 the frame comes from the sequence;    */
                               /* bit 2 (debris only) indexed by the owner's  */
                               /* logo colour through 0x4B7F30                */
#define FF_FLAT      0x1u
#define FF_ANIM      0x2u
#define FF_TEAM      0x4u
/* the sequence layout both frame pickers read */
#define SEQ_NFRAMES  0x00      /* u16                                         */
#define SEQ_FRAMES   0x28      /* {GAFFrame*, dword} x nframes                */
#define SEQ_FSTRIDE  8

/* THE BUILD GHOST'S PIECE ORDER, in one place because two threads must agree
   on it: the render thread poses the ghost in this order (tagpu_native.c,
   ghost_pieces) and the game thread publishes the ghost's hidden-piece mask
   as bits in this order (tagpu_datakeys.c). A second copy of the walk could
   drift from the first and hide the wrong pieces; one function cannot.

   A stack walk of the template tree: a node, then its children in sibling
   order pushed, so the LAST child is visited first. It is not the engine's
   own piece order (0x45AEC0 recurses child before sibling), and nothing may
   assume it is. The root's own siblings are not visited.

   Returns the node count, 0 for a root the range test refuses, and -1 when
   the tree exceeds `max` nodes or the stack's TAGPU_PBMAXPIECE: a DATA bound
   that stops a malformed model running away, and the caller refuses the
   model rather than draw part of it. The range test is a sanity filter on a
   VALUE, the end-of-list test; what makes the reads safe is the caller's
   bound on the type and the template's LEVEL lifetime. */
static __inline int tagpu_m3_ptr_ok(const void* p)
{ return (size_t)p > 0x600000u && (size_t)p <= tagpu_user_top(); }

static __inline int tagpu_model_walk(const char* root, const char** nodes, int max)
{
    const char* stack[TAGPU_PBMAXPIECE];
    int sp = 0, n = 0;
    if (!tagpu_m3_ptr_ok(root)) return 0;
    stack[sp++] = root;
    while (sp > 0) {
        const char* nd = stack[--sp];
        const char* ch;
        if (n >= max) return -1;
        nodes[n++] = nd;
        for (ch = *(const char* const*)(nd + N_CHILD);
             tagpu_m3_ptr_ok(ch);
             ch = *(const char* const*)(ch + N_SIB)) {
            if (sp >= TAGPU_PBMAXPIECE) return -1;
            stack[sp++] = ch;
        }
    }
    return n;
}

#endif
