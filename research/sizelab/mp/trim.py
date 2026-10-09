import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
R = [
("""The playback applies everything labelled ≤ T before image T. The exception is the envelopes from
the first produced post-capture root batch on: those it plays after step T's primary events.

- A late arrival is simply a later label.
- An order, map or selection transaction publishes only at its last chunk.
- Proof tails never apply.
- Source ticks are provenance only.

The composed world is therefore a pure function of the two ordered inputs. That is already the
seek's invariant (`mp_rebuild` composes what the forward playing holds), and a writer running the
same code reproduces it exactly.""",
"""The playback applies everything labelled ≤ T before image T, except the envelopes from the first
produced post-capture root batch on, which it plays after step T's primary events. A late arrival
is a later label; a transaction (order, map, selection) publishes at its last chunk; proof tails
never apply; source ticks are provenance. So the composed world is a pure function of the two
ordered inputs (already the seek's invariant), and a writer running the same code reproduces it."""),
("""**Seek (S).** Today a seek is linear in the file:

- `mp_rebuild` re-reads every envelope from ordinal 0;
- the reader's `mp_read` rebuilds both histories from the segment's first contributions block
  whenever the tick goes back;
- measured (note): a fresh seek to 1,958 took 407 ms, 402 of them reading the stream and composing
  the contributions.

A merged snapshot holds the composed world, so a seek becomes a skirmish seek plus two items:

- the held creates (`mp_make_held`'s 28-byte arguments and water);
- the births that the selection display reads (`ov_born`, D3).

The roster stays in the header.""",
"""**Seek (S).** Today a seek is linear in the file: `mp_rebuild` re-reads every envelope from
ordinal 0, and `mp_read` rebuilds both histories from the segment's first contributions block
(note: a fresh seek to 1,958 took 407 ms, 402 reading the stream and composing). A merged snapshot
holds the composed world, so a seek is a skirmish seek plus the held creates (`mp_make_held`'s
arguments) and the births the selection display reads (`ov_born`, D3). The roster stays in the
header."""),
("""- **No unit is without an owner.** A peer without Impure leaves the channel unbound and the
  recording incomplete (the slice note, *stage 6 residuals*). Supporting such a peer would need a
  per-seat rule taking that seat's units from the observed copy.""",
"""- **No unit is without an owner:** a peer without Impure leaves the recording incomplete
  (*stage 6 residuals*); supporting one would take that seat's units from the observed copy."""),
("""Two bases:

- **Disk:** the real files, against my prototype merged file (`mp_merge.py`: today's blocks with
  the records replaced, deflated as the writer does).
- **Category:** each category deflated alone per block (recsize's attribution, which is the size
  round's sectioned container).""",
"""**Disk** basis: the real files against my prototype merged file (`mp_merge.py`, deflated as the
writer does). **Category** basis: each category deflated alone per block (the sectioned
container)."""),
("""**What is lost:**

1. **A reader's re-check** of sequence continuity and of the CRC-32 chain against the seals. This
   is integrity only: no key or signature is persisted (S: `tagpu_recmp_file.h`).
2. **Re-composing an old file under new rules.** A rule change means re-recording.
3. **The independent Python composition** (`recmp_compare.py`, `History`) that checks the C
   materializer.
4. **Audits on packet files:** source clocks, replica against owner.""",
"""**What is lost:** (1) a reader's re-check of sequence continuity and the CRC-32 chain (integrity
only: no key or signature is persisted, S `tagpu_recmp_file.h`); (2) re-composing an old file
under new rules; (3) the independent Python composition (`recmp_compare.py`) that checks the C
materializer; (4) audits of source clocks and replica against owner."""),
("""**A chained codec is safe, because order is guaranteed by construction (S):**

- The receive worker passes each packet through `tagpu_recmp_stream_put`, which refuses any
  sequence but the last plus one.
- It publishes only a contiguous prefix per source (`tagpu_recmp.c`, *THE ORDERING*).
- The session refuses `DPSESSION_NOPRESERVEORDER` and sends `DPSEND_GUARANTEED`.

A gap fails the channel, so nothing is ever decoded against the wrong state. Today's unit deltas
already chain this way.""",
"""**A chained codec is safe: order is guaranteed by construction (S).** The receive worker refuses
any sequence but the last plus one (`tagpu_recmp_stream_put`) and publishes only a contiguous
prefix per source (`tagpu_recmp.c`, *THE ORDERING*); the session refuses
`DPSESSION_NOPRESERVEORDER`. A gap fails the channel, never decodes wrong. Today's deltas already
chain."""),
("""**I3, one composition.**

- The lifecycle consumer and the rules of `mp_compose`, `mp_authority` and `mp_world` move out of
  `tagpu_replay.c` into one engine-free module.
- The writer runs it over the world copy it already keeps; it already runs
  `tagpu_recmp_history_record` on every journal record.
- The playback of a merged segment composes nothing. It uses the skirmish image appliers, and
  plays owner events under today's mode-3 event rules.""",
"""**I3, one composition.** The lifecycle consumer and the rules of `mp_compose`, `mp_authority` and
`mp_world` move into one engine-free module, run by the writer over the world copy it already keeps
(it already validates every journal record). A merged segment's playback composes nothing."""),
("""- **D2 — weaker verification.** Do you accept losing the reader's re-check, the re-composition of
  old files under new rules, and the Python cross-check (which moves to the transcode test)? Is any
  copy of the packets kept, even for tests only?
- **D3 — births.** Should the recorded births column (`s_hitStamp`) carry the owners' births for
  remote slots, to match the composed units, rather than the recorder's replica tokens (B4's
  exact, lower-bound or unknown)? [I: I did not check what a takeover restores from that column.]""",
"""- **D2 — weaker verification.** Accept losing §4's four items? Keep any packet copy, even
  test-only?
- **D3 — births.** Should the recorded births column (`s_hitStamp`) carry the owners' births for
  remote slots, rather than the recorder's replica tokens? [I: what a takeover restores from it was
  not checked.]"""),
("""It costs a lot:

- every remote unit is stored twice;
- the recorder's own outgoing packets sit beside its stream, byte-identical to it;
- seals and 80-byte headers ride on every packet;
- the size round's codecs cannot touch any of it;
- every seek re-reads the packets from the start.""",
"""It costs a lot: every remote unit is stored twice; the recorder's own outgoing packets sit
beside its stream, byte-identical to it; seals and 80-byte headers ride on every packet; the size
round's codecs cannot touch any of it; every seek re-reads the packets from the start."""),
]
for a, b in R:
    assert a in s, a[:60]
    s = s.replace(a, b)
p.write_text(s)
