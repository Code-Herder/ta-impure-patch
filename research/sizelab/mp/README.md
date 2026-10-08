# Multiplayer contributions in the file: record the merged world, not the packets

KiB throughout. **M** = MEASURED (scripts and outputs beside this file); **S** = read in our
source (file, function); **[I]** = inferred. Recordings: two-peer `verify/two` (1,578 ticks) and
`chatfix/two` (1,577); five-peer ten-seat `v6/full/ten` (2,973; `verify/ten.imprec` is
byte-identical).

## The answer, plainly

We do record the simulation state: every tick the recording peer stores its whole world. But in a
network game no machine holds the true state of every unit. Each simulates only its own; its
copies of the others' are late, partial replicas. So each Impure peer also sends its own units,
players, orders, camera and chat to the others.

The file stores that second source **as the network packets themselves**, and the playback
assembles the true world from both. That kept the proof that nothing was lost re-checkable. It
also let the "which copy wins" rules change after recording: the state byte, the build finish and
the explored map all did on 2026-10-06.

It costs a lot: every remote unit is stored twice; the recorder's own outgoing packets repeat its
stream (§2); seals and 80-byte headers ride on every packet; the size round's codecs cannot touch
any of it; every seek re-reads the packets from the start.

The alternative is safe by construction. The writer thread already receives both inputs in order
and validates every packet, so it can assemble the world exactly as the playback does and store
that one world, with a small proof footer. What travels on the network does not change.

## 1. What the playback takes from each copy (S: `tagpu_replay.c`, *THE MULTIPLAYER MATERIALIZER*)

| State | Source on the puppet host today | The merged stream carries it as |
|---|---|---|
| Unit existence | the owner's `CREATE_BEGIN/END` and `DEATH_BEGIN/END` roots, through the lifecycle consumer; the primary's create, state and build-done events are not played (`mp_owned_event`) | owner lifecycle events at their application position |
| Unit record and mover: state byte, build finish, water | the latest applied owner image of that birth (`mp_compose`); the level-start image until the first one (ticks 54–57) | unit images: local = the primary's; remote = composed |
| Named starts, replies, sprays | `ROOT` actions in ledger order, the recorder's own included | events |
| Actor player records | the owner's image mapped to recorder rows; `+0xF0` is the recorder's | player records |
| Order lists | the owner's lists whose birth matches the unit's (`mp_authority`) | `RK_ORDERS` |
| Explored map | each actor's bit from its owner, the other bits the recorder's (`mp_world`) | the composed map |
| Camera, clicks, selections | per-origin samples at their application ticks | `RK_POV`, `RK_CLICK`, `RK_SELECT` per seat (these kinds exist) |
| Chat | text entries at their application tick; the audience by phase and recipient | a new event kind (136-byte entries) |
| Features, cells' definitions and heights, wrecks, `FR_CORPSE`, projectiles, shots, globals, tables, unseated players | the primary stream | unchanged |
| Cells' unit slots, yard bit | the host's engine | unchanged rule |
| Contact bits `+0x110` 8–10, 12 | the host's own radar pass (*R14b*); only the comparison expects the recorder's | the recorder's four bits; a unit the recorder has not yet observed is marked and compared without them, as today |

**The clock (S: `mp_capture`, *THE CLOCK*).** At B2 of tick T the game thread does, in order:

1. flushes the post-capture roots of T−1;
2. advances;
3. flushes the pre-capture roots of T;
4. publishes its player images;
5. consumes the remote packets, labelled T;
6. publishes its unit, order and map images.

The playback applies everything labelled ≤ T before image T, except the envelopes from the first
produced post-capture root batch on, which it plays after step T's primary events. A late arrival
is a later label; a transaction (order, map, selection) publishes at its last chunk; proof tails
never apply; source ticks are provenance. So the composed world is a pure function of the two
ordered inputs (already the seek's invariant), and a writer running the same code reproduces it.

**Seek (S).** Today a seek is linear in the file: `mp_rebuild` re-reads every envelope from
ordinal 0, and `mp_read` rebuilds both histories from the segment's first contributions block
(note: a fresh seek to 1,958 took 407 ms, 402 reading the stream and composing). A merged snapshot
holds the composed world, so a seek is a skirmish seek plus the held creates (`mp_make_held`'s
arguments) and the births the selection display reads (`ov_born`, D3). The roster stays in the
header.

**Takeover** already runs on the composed world (`tk_bound`, `tk_orphans`); nothing it needs is
only in the packets.

## 2. The observed copy

- **The recorder's own packets repeat its stream exactly (M):** 24,288/24,288 local unit images
  and 153/153 player images equal at their tick (two-peer); 2,711/2,711 and 206/206 (ten-seat).
  Its own order lists and map were not compared.
- **The observed remote copy gives the playback only** the level-start images (until each
  owner's first image), the comparison's contact bits, and research audits.
- **No unit is without an owner:** a peer without Impure leaves the recording incomplete
  (*stage 6 residuals*); supporting one would take that seat's units from the observed copy.

## 3. Sizes

**Disk** basis: the real files against my prototype merged file (`mp_merge.py`: units, players
and lists composed, the map left as recorded; deflated as the writer does). **Category** basis:
each category of the stream and the contributions deflated alone per block (the sectioned
container; snapshots, minimap and header excluded).

| Today, on disk | two-peer | chatfix | ten-seat |
|---|---:|---:|---:|
| file | 4,744 | 4,192 | 1,402 |
| contributions | 1,228 (25.9 %) | 1,152 (27.5 %) | 687 (49.0 %) |

| Contributions by kind (category basis, M) | two: own | two: remote | ten: own | ten: remote |
|---|---:|---:|---:|---:|
| units (kind 8) | 356.6 | 260.4 | 64.4 | 182.4 |
| seals (kind 4) | 64.4 | 65.8 | 53.6 | 136.1 |
| order lists | 38.1 | 39.4 | 13.3 | 40.0 |
| roots | 36.7 | 28.7 | 5.0 | 14.8 |
| explored, players, perspective, chat, roster | 16.9 | 18.4 | 14.8 | 33.8 |
| **total** | **512.7** | **412.7** | **151.1** | **407.1** |

The 80-byte headers are 38 % of the two-peer contributions before compression.

| Whole recording (category basis, M) | two-peer | chatfix | ten-seat |
|---|---:|---:|---:|
| today | 3,987 | 3,487 | 1,056 |
| V1: without the own-state packets | 3,576 (−10 %) | 3,110 | 965 (−9 %) |
| V3: also without seals, headers cut to 12 bytes | 3,380 (−15 %) | 2,917 | 687 (−35 %) |
| **merged world** | **3,226 (−19 %)** | **2,787 (−20 %)** | **544 (−49 %)** |
| merged world, disk basis (prototype) | 3,944 (−17 %) | 3,385 (−19 %) | 780 (−44 %) |

**Units through the unit codec** (M; `frame2.py` with the engine rules; every iteration decoded
back with 0 mismatches; unit records alone per block, created images and gones included):

| | two: records | two: codec | chatfix: codec | ten: records | ten: codec |
|---|---:|---:|---:|---:|---:|
| the observed stream, every unit | 452.0 | 84.3 | 61.8 | 103.3 | 14.3 |
| the packets' unit sequences, each on its owner's clock | 512.3 | 84.1 | 67.1 | 126.6 | 16.5 |
| **merged units** | 566.7 | **91.6** | **72.5** | 124.8 | **22.7** |

- **Through the codec, today's two copies come to 168 (two-peer) and 30.8 (ten-seat); the merged
  world to 92 and 22.7.**
- **Composed remote units cost 20–50 % more than their owners' own sequences:** 43.3 against 35.7
  (two-peer), 32.1/26.8 (chatfix), 18.6/12.4 (ten-seat) [I: the local part subtracted]. Arrival
  bunching defeats the integrator rules [I]; a per-origin source tick could restore them.

**After the size round's stack** [I: the battle's measured ratios, projectiles × 0.070 and the
discrete categories × 0.36, applied to each file's shared part]:

| | today, packets as they are | merged world |
|---|---:|---:|
| two-peer | ≈ 1,400 (66 % packets) | ≈ 510–550 |
| ten-seat | ≈ 716 (78 % packets) | ≈ 175–190 |

## 4. The proof footer

**What it keeps, per source** (≈ 0.7 KiB in all):

- the origin and stream GUID;
- the consumption cut: sequence, source tick, application tick, ordinal;
- the covering seal (24 bytes) and its sequence;
- the local final seals, the receipt count and the application-record count.

The writer keeps today's refusal: no complete end block without the proof.

**What is lost:** (1) a reader's re-check of sequence continuity and the CRC-32 chain (integrity
only: no key or signature is persisted, S `tagpu_recmp_file.h`); (2) re-composing an old file
under new rules; (3) the independent Python composition (`recmp_compare.py`) that checks the C
materializer; (4) audits of source clocks and replica against owner.

## 5. The wire

**The merged file changes nothing that is sent.** The recorder needs every owner's contributions
as input, and a peer that does not save still contributes.

**A chained codec is safe: order is guaranteed by construction (S).** The receive worker refuses
any sequence but the last plus one (`tagpu_recmp_stream_put`) and publishes only a contiguous
prefix per source (`tagpu_recmp.c`, *THE ORDERING*); the session refuses
`DPSESSION_NOPRESERVEORDER`. A gap fails the channel, never decodes wrong. Today's deltas already
chain.

**Measured on the wire** (M, two-peer, per-tick frames, uncompressed as on the wire, decoded back),
per actor over 52.6 s:

| | per actor |
|---|---:|
| unit bodies today | 263–484 KB |
| unit bodies through the codec | 51–117 KB (4.1–5.2× less) |
| 64-byte envelopes | ≈ 96 KB |
| seals (one a tick, 88 bytes) | ≈ 133 KB |

After the codec the seals outweigh the units: stage 6's matter.

## 6. What the merged design rests on

**I1, an ordering.** One game thread fills both single-producer rings in program order.
`mp_capture(T+1)` publishes every journal record labelled ≤ T+1 (pre-capture) and T's
post-capture roots, and only then does `tagpu_rec_capture` publish iteration T's events and
`RK_TICK T+1`. So once the writer has acquired `RK_TICK T+1`, the journal holds everything the
materializer's step T applies. The writer applies each record by its label and the split
(`mp_post_capture`), exactly as `mp_apply_held` does.

**I2, a bound.** The writer takes a journal record only when its label is due (peek, then take),
so it holds back at most one B2's records. Every ID stays bounded by the existing validation
(frozen blocks, `ndefs`).

**I3, one composition.** The lifecycle consumer and the rules of `mp_compose`, `mp_authority` and
`mp_world` move into one engine-free module, run by the writer over the world copy it already keeps
(it already validates every journal record). A merged segment's playback composes nothing.

**I4, snapshots.** A snapshot at T holds the composed world and the held creates as of image T
(`mp_rebuild`'s state), never the iteration's tail.

**Cost:** writer only; the capture is unchanged [I].

**Verification** (each run ≤ 10 min):

- (a) a game-free transcode of the three recordings; the merged world must equal
  `recmp_compare`'s composed world at **every** iteration;
- (b) their playing: forward, fresh seeks, round trips and a takeover fork, all EQUAL;
- (c) a native two-peer recording, then (b) on it;
- (d) `secondary-orders --cover`.

## 7. Risks

- **A writer bug becomes permanent data.** The safeguard is (a). A composition refusal ends the
  file at its last complete block instead of failing at play time.
- **The tools change:** `tarec check`'s transition rules; `recmp_compare.py` (a plain compare
  with the mode-3 holds); `recmp_file.py`; the browser (it reads the footer alone).
- **Existing mode-3 files stop playing** unless the old reader stays. The recorder is unreleased.

## 8. Recommendation and owner decisions

**Recommended: the merged world, composed in the writer, landing with or after the size round's
L1** (the container and decode-before-write), so the merged units get the unit codec.

V1 is the only trim that keeps a proof re-checkable, and only the remote sources'. It saves
9–10 %.

**Decisions for you:**

- **D1 — escalation (reason 1: the plan changes).** Replace the slice's file design (the packets
  in `RB_CONTRIBUTIONS`, the persisted ledgers) with the merged world and a footer.
- **D2 — weaker verification.** Accept losing §4's four items? Keep any packet copy, even
  test-only?
- **D3 — births.** Should the recorded births column (`s_hitStamp`) carry the owners' births for
  remote slots, rather than the recorder's replica tokens? [I: what a takeover restores from it was
  not checked.]
- **D4 — the wire codec, and seals on the wire.** This is stage 6 (traffic) and needs a wire
  version bump.
- **D5 — order.** Go to the merged world directly, or take V1 now as a stopgap?

No loosening is involved (no exclusion, tolerance or expected-absent row); the comparison keeps
its approved holds.
