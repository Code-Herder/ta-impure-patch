/* tagpu_lerp.h -- option A of research/notes/smooth-motion.md: interpolate a
   unit's COB-driven piece pose between the two most recent SIM TICKS, so the
   model animates at render rate instead of snapping at 30 Hz.

   THE WHOLE FEATURE IS TWO FIELD READS. `posed_pose` composes each piece's
   4x3 from exactly `P_POS` (i32[3] 16.16) and `P_TURN` (u16[3] TAang); this
   module hands it a blended copy of those two triples and nothing else
   changes. That is only this small because G16 turned the pose back into
   per-piece FIELDS -- before it, the pose arrived already baked into vertices.

   THE INVARIANTS (smooth-motion.md section 3), all load-bearing:

     1. READ-ONLY. Nothing is written back to PrimitiveStruct. The sim reads
        those fields (`get PIECE_XZ`, QueryPrimary/AimFromPrimary hand the
        engine weapon muzzle origins out of them) and TA has NO runtime desync
        detection, so a framerate-dependent, per-machine blend written there
        would diverge two machines silently. Kept inside posed_pose's output it
        is invisible to the simulation.
     2. ONE RENDERER. Degradation is a blend weight of 1.0, never a second code
        path -- and here that is literal: every refusal returns 0 and the
        caller then reads the live fields with the code it always had, so the
        off case is bit-identical BY CONSTRUCTION rather than by a float lerp
        that happens to land on the endpoint (`a + (b-a)*1.0f` is NOT `b`).
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

/* Once per render frame, before the unit gather: re-reads the lever, latches
   the sim tick and the sub-tick phase, and ages the history table. */
void tagpu_lerp_frame(unsigned frameCounter);

/* One unit's blended pose fields, or 0 for "use the live fields".
   `pr[nparts]` are the unit's PrimitiveStructs, as posed_pose already built
   them. On 1, *pos is int[nparts*3] and *turn is unsigned short[nparts*3],
   valid until the next call on this thread (render thread only). */
int tagpu_lerp_unit(const char* o3, int nparts, const char* const* pr,
                    const int** pos, const unsigned short** turn);

/* " lerp=<blended>/<snapped>[ big=<n>][ full=<n>]" for the native: line, or ""
   when the lever is off. Resets the window's counters. */
void tagpu_lerp_stats(char* buf, unsigned cap);

#endif
