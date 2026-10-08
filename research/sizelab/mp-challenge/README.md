# Challenge: "record the merged world, not the packets" (mp.md)

M = MEASURED here, S = read in the source, [I] = inferred.

**Verdict: do not adopt the merged world as written.** Its ordering invariant I1 is off by one
iteration. A lossless design that keeps the packets saves as much on the two-peer files and keeps
everything that mp.md §4 gives up.

## 1. Equivalence

- **The composition needs no engine state: CONFIRMED (S).** `mp_compose` (the seek's compose
  mode) touches no engine. `mp_authority` reads only the reader's history, and `mp_world` reads
  only the primary world's `PL_MAPBIT`. The writer already validates every record
  (`tagpu_recwriter.c:contribution` calls `tagpu_recmp_history_record`).
- **Labels and transactions: CONFIRMED (S).** A record's label is its application tick, so late
  and bunched packets are already resolved by it. Chunked transactions publish at their last
  chunk.
- **I1 is WRONG (S): some records labelled T are published *after* `RK_TICK T`.**
  - Clicks are published between B2s, at `s_mp_tick` (`mp_click`).
  - Post-capture roots and chat text are flushed at the next B2, step 1, before the advance
    (`mp_events_flush(s_mp_tick)`).
  - `RK_TICK T+1` is published at the end of capture T+1 (`ring_publish` after `cap_pov`).
  - `mp_step_image(T)` applies every record labelled ≤ T+1 up to the first post-capture root
    batch (`mp_apply_held`). That includes clicks labelled T+1 and, in a tick with no post-capture
    roots, text labelled T+1. Neither exists yet at `RK_TICK T+1`.
- **What does hold:** every record labelled ≤ T is published before `advance(T+1)`, so before
  `RK_TICK T+1`. Compose **image T** at `RK_TICK T+1`, and the last image after
  `tagpu_recmp_record_done`.
  - A writer built to the stated I1 would refuse a late click, which ends the file, or put clicks
    and chat one iteration late.
  - `recmp_compare --chat` accepts a stamp of T or T−1 (`chat_check`), so it would not catch this.
  - Units, players, orders, the map, cameras and selections are published in steps 3–6, so they
    are unaffected.
- **Seek and takeover: CONFIRMED (S).** Both use `mp_world` after `mp_rebuild` and `mp_make_held`.
  Snapshots also need the owners' births: `ov_born` reads the lifecycle (D3).
- **The playback still runs the owner-time engine actions (S)**: creates, `DEATH_END`, roots,
  `books_put`, and the chat audience. `BOOTSTRAP` needs a marker in the merged stream, which mp.md
  omits.
- **Leavers:** no rule yet; merged files would freeze today's "units stay alive".
- **Verification (a) needs another oracle.** `recmp_compare`'s `compose(hist.at(T))` has no
  lifecycle consumer, no held creates and no post-capture split. The check "equal at every
  iteration" needs the production compose mode (`mp_rebuild`) as its oracle.

## 2. The measurements

- **Shares 25.9 / 27.5 / 49.0 %: CONFIRMED (M).** I re-ran `mp_account.py` and got identical
  output.
- **Merged −19 / −20 / −49 %: CONFIRMED on the report's basis.** Two caveats:
  - The disk basis compares the writer's deflate (today) with zlib (merged), and zlib is 2.5 %
    larger on the same stream (3,521 against 3,437 KiB). Like-for-like the savings are −19.0 /
    −21.4 / −46.2 %.
  - The ten-seat −49 % excludes the snapshots, which are 12 % of that file.
- **"Own packets repeat the stream": CONFIRMED and extended (M, `local_equal.py`).**
  - I compared every local slot at every iteration, not only where a packet changed it: units
    38,682 / 38,933 / 9,714 equal; order lists 77,364 / 77,866 / 19,428 equal; players and
    explored-map bit equal at every iteration. There were no differences.
  - It also holds by construction: nothing between `mp_capture`'s step 6 and `cap_units` writes
    engine state (`ai_finish` only logs).
- **Headers 38 %: CONFIRMED.** 20,326 records × 80 B against 4,209 KiB raw.
- **Wire units 4.1–5.2×: CONFIRMED (M).** `mp_wire.py` gives 4.05–5.16×, with every tick decoded
  back. Today's side omits the absent entries (59 of 927 KB), so the ratio is slightly
  understated.
- **Order refusal and gap-free delivery: CONFIRMED (S).** `tagpu_recmp_net.c:288` refuses
  `DPSESSION_NOPRESERVEORDER`; the receive worker requires +1, and a failed publication latches
  consumption off.
- **The 407 ms seek** includes the stream read from the 1801 snapshot; composition's share is
  unmeasured.

## 3. The alternative mp.md did not measure: keep the packets, store them losslessly (M)

**Variant K** (`keep_packets_nolocalseal.py`):

- **Drop the recorder's own copies:** its state packets and its own seals. The playback then takes
  local state from the primary, which §2 shows equal.
- **Keep the remote packets with a 5–6-byte header.** The exact 16-byte metadata and 64-byte
  envelope are rebuilt from it: the ordinal is the previous one plus 1, sequences are gap-free,
  and the GUIDs are constant.
- **Store each seal as its tick, its final flag and the attested CRC.** The rest of a seal is a
  function of the prefix (`tagpu_recmp_stream_put`).
- **Drop the observed remote unit records after the first iteration** (`mp_world` replaces them
  wholesale), and add 90 / 81 / 15 contact-bit records for `recmp_compare`.

All 11,320 / 11,785 / 17,955 kept records rebuilt **byte-exact**, and every attested CRC matched
the chain recomputed from the stored packets.

| zlib, KiB | two-peer | chatfix | ten-seat |
|---|---:|---:|---:|
| today | 4,872 | 4,304 | 1,451 |
| merged (the mp.md prototype) | 3,944 (−19.0 %) | 3,385 (−21.4 %) | 781 (−46.2 %) |
| **K** | **3,799 (−22.0 %)** | **3,282 (−23.7 %)** | **861 (−40.6 %)** |

K keeps the re-checkable proof, re-composition, `History`, `recmp_compare`, the native tests
(behind one decode-to-bytes layer) and today's materializer.

The ten-seat gap of 80 KiB is made of:

- **Remote unit packets: 120.6 KiB alone, against 88.6 KiB composed.** mp.md's own codec runs put
  the owners' sequences below the composed ones (35.7 against 43.3 KiB, 12.4 against 18.6 KiB
  [I]), so after L1 this part should reverse.
- **Seals: 33.1 KiB.** The covering seal implies every earlier one, so the intermediate seals can
  become stubs [I].

K's seek stays linear. Composition checkpoints in the snapshots would fix that [I].

## 4. What mp.md's loss list leaves out

- **The native tests that read the persisted packets:** `recmp_lifecycle_test.py` (the production
  consumer against a Python audit), `recmp_create_begin_test.py` (persisted causality) and
  `recmp_compare --fork`.
- **A writer refusal truncates a player's file permanently.** Today a refusal stops one playing,
  and a later build can still play the file.
- **Re-composition is in use now (S, the slice note).**
  - The state-byte rule was re-applied to the existing four-actor recording, and the explored-map
    rule to the existing reproduction recording.
  - The build finish was verified on a fresh recording.
  - Leavers and an owner build-done root are the next rules, and under the merged design each
    needs new native multi-peer games.

## 5. Verdict and questions for the owner

I recommend K, plus snapshot checkpoints for the seek, and a revisit of the merged world once the
composition rules stop changing. If the merged world is chosen anyway, I1 must be re-indexed as in
§1.

- **Q1 (escalation reason 1): K or the merged world?** Either one replaces the slice's file
  design.
- **Q2: drop the observed remote copy**, losing the replica-against-owner audits? Both designs do.
- **Q3 (merged only): accept losing the independent Python oracle**, or keep a test-only packet
  lever? This is a weaker verification, not one of the listed loosenings, but it is the owner's
  call.
