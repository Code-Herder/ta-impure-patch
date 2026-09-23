/* tagpu_lerp.h -- option A of research/notes/smooth-motion.md: interpolate a
   unit's COB-driven piece pose between the two most recent SIM TICKS, so the
   model animates at render rate instead of snapping at 30 Hz.

   THE WHOLE FEATURE IS TWO FIELD READS. `posed_pose` composes each piece's
   4x3 from exactly `P_POS` (i32[3] 16.16) and `P_TURN` (u16[3] TAang); this
   module hands it a blended copy of those two triples and nothing else
   changes. That is only this small because the pose arrives as per-piece
   FIELDS rather than already baked into vertices.

   THE POSE HISTORY IS THE EXCHANGE ITSELF (frame packet exchange). The two
   packets the consumer holds ARE the two banks: `read` is the later tick,
   `prev` the earlier one, the acquire guarantees they differ (tagpu_packet.c's
   rotation), and units are matched between them by the STABLE ID, never by
   table position. Nothing here reads the engine's PrimitiveStructs.

   THE INVARIANTS (smooth-motion.md section 3), all load-bearing:

     1. READ-ONLY. Nothing is written back to PrimitiveStruct. The sim reads
        those fields (`get PIECE_XZ`, QueryPrimary/AimFromPrimary hand the
        engine weapon muzzle origins out of them) and TA has NO runtime desync
        detection, so a framerate-dependent, per-machine blend written there
        would diverge two machines silently. Kept inside posed_pose's output it
        is invisible to the simulation -- and this module holds no engine
        pointer at all, so the invariant is a property of the code
        rather than a rule to keep.
     2. ONE RENDERER. Degradation is a blend weight of 1.0, never a second code
        path -- and here that is literal: every refusal returns 0 and the
        caller then reads the packet's own CURRENT triples with the code it
        always had, so the off case is bit-identical BY CONSTRUCTION rather
        than by a float lerp that happens to land on the endpoint
        (`a + (b-a)*1.0f` is NOT `b`).
     3. Blend the FIELDS, not the composed matrices.
     4. TAang WRAPS -- take the short way round, as the engine's own TURN does.
     5. Visibility never blends: P_FLAGS is not touched here at all.

   The sub-tick weight is timing-derived, which smooth-motion.md section 6
   allows ONLY because it is a visual weight clamped to [0,1] that never
   reaches a pointer, an index or engine state: a wrong estimate costs one
   slightly wrong frame and corrupts nothing. It is not precedent for a
   timing-based correctness argument anywhere else in this stack. */
#ifndef TAGPU_LERP_H
#define TAGPU_LERP_H

struct TAGPU_PACKET;
struct TAGPU_PK_UNIT;
struct TAGPU_PK_PIECE;

/* Once per render frame, before the unit gather: re-reads the lever and
   latches this frame's pair and its sub-tick weight. `prev` may be NULL (no
   pair this frame: every unit then draws its packet pose unblended). */
void tagpu_lerp_frame(unsigned frameCounter,
                      const struct TAGPU_PACKET* pk,
                      const struct TAGPU_PACKET* prev);

/* One unit's blended pose fields, or 0 for "use the packet's own triples".
   `cur` is this unit's PK_PIECE run out of the packet `tagpu_lerp_frame` was
   given. On 1, *pos is int[nparts*3] and *turn is unsigned short[nparts*3],
   valid until the next call on this thread (render thread only). */
int tagpu_lerp_unit(const struct TAGPU_PK_UNIT* u,
                    const struct TAGPU_PK_PIECE* cur, int nparts,
                    const int** pos, const unsigned short** turn);

#endif
