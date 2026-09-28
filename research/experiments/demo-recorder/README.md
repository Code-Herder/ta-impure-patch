# Demo recorder research tools

These inspect the 2026-09-28 transport experiment and measure a disposable block container.
The offline tools do not establish game seeking. The separate research replay host can feed
one captured two-player match into an isolated Impure spectator; it is not a shipping recorder
or a complete-state checkpoint implementation.
The source and evidence report is `research/notes/tadr-port/demo-recorder-exploration.md`.
`results.json` contains the reference setup, counts, ordered payload comparisons, and timings.

Use the **shared main-checkout venv**, with `zstandard` installed there. Set `REPO` to the main
checkout and `WORKTREE` to the checkout containing these scripts:

```bash
"$REPO/.venv-undither/bin/python" "$WORKTREE/research/experiments/demo-recorder/trace.py" \
  "$REPO/_local/demo-recorder-exploration/demoxh.dprobe" \
  "$REPO/_local/demo-recorder-exploration/demoxj.dprobe"
"$REPO/.venv-undither/bin/python" "$WORKTREE/research/experiments/demo-recorder/blocks.py" \
  "$REPO/_local/demo-recorder-exploration/demoxh.dprobe" \
  --start-ms 80316576 --end-ms 80414000
"$REPO/.venv-undither/bin/python" -m unittest discover \
  -s "$WORKTREE/research/experiments/demo-recorder" -p 'test_*.py' -v
```

Captures are local research inputs, deliberately untracked. The stopped instances were removed.
Their compact used prefixes have SHA-256:

- Host: `c8cb886ec8c89e92132ffe85183efdacc9f53d52636a1df3bcb0ca2a07d628a9`
- Joiner: `ecb6c6684469bd16b0ceaeffc1c3b4fdd2868acff8b01227fb0ec9a03392e1d0`

The temporary observer source and DLL are retained beside those inputs locally. It was inserted
in `tagpu_patches.c` for this run, then removed and the ordinary DLL rebuilt. No persistent hook
or production recording API is introduced by this directory. To reproduce capture, the report
specifies the three byte-checked DirectPlay wrapper boundaries and two existing scenarios.
The observer's send records are attempts; received records exist only for successful receives.

`DPROBE1` is the temporary probe layout: a 64-byte little-endian header starts with `DPROBE1\0`,
u32 published-record count at +8, u32 reported drop count at +12, u64 QPC frequency at +16.
Each record is ten u32 fields followed by payload and padding to a four-byte boundary:

1. Published total record bytes (zero means not published).
2. `timeGetTime` milliseconds.
3. Producer thread ID.
4. Direction (1 send, 2 successful receive).
5. Sender DirectPlay ID.
6. Recipient DirectPlay ID.
7. Payload byte count.
8. Timed copy-region QPC ticks.
9. Start QPC low word.
10. Start QPC high word.

Read a stopped trace. Record counts must agree; an in-progress publication is refused.
`trace.py` rejects unsupported message lengths and invalid checksums instead of silently
producing partial statistics. This decoder only covers the measured message dialect. System
messages are not decoded: their bytes include process-local pointer values. The observer's
65,536-byte payload cap means this experiment does not establish oversized-packet coverage.

`blocks.py` measures independently compressed 100 ms / 1 s / 5 s blocks at Zstandard levels
1 / 3 / 6, with exact event round trips. A 12-byte-per-block index is an estimated size, not a
serialized/benchmarked index. It sorts by observed wall time solely to group this compression
workload; production replay needs an explicit causal application order and clock policy.
The times exclude disk and game/renderer work. Its 32-byte header, 16-byte event framing, and
8 MiB decode limit are experimental choices, not the shipping replay format.

The truncation/corruption tests use synthetic data written by the tests. They prove bounded
parsing and prefix recovery in this container, not file durability or playable crash recovery.


## Isolated engine playback prototype

`dplay_bridge.c` is a bounded stdin/stdout native DirectPlay transport bridge; `replay_host.py`
implements the two-player TA lobby/launch handshake and preserves the captured binary gameplay
messages. Compile the bridge with:

```bash
i686-w64-mingw32-gcc -O2 -Wall -Wextra dplay_bridge.c -lole32 -luuid -o bridge.exe
```
 Use only an owned isolated native-DirectPlay prefix, with a unique
port also assigned to the viewer. The Python driver's `--help` lists the paths.

The viewer must be a spectator on matching content. Its unit-block ordering must match the
recorded players independently of new DirectPlay IDs. Without `--order-file` the driver rejects
a viewer whose ID would sort ahead of either drone. With `--order-file` it writes immutable
recorded-player ranks for the **separate temporary allocation-order probe**: verify the viewer's
arming log and resulting slot ranges before trusting any comparison. That probe is retained
locally, not installed by these scripts and not part of the production patch. An order file
alone cannot change the engine.

`--speed` scales wall-time event submission, not simulation ticks. `playback-eof` means the
Python driver submitted its final command, not that the engine has applied it. Observe the
engine's pause/state separately. The driver omits lobby/control messages after start, translates
Impure carried-death killer IDs, and does not implement the final content fingerprint, tick
alignment, crash recovery, ten-player/AI support, perspectives, or seeking. See §6a of the
exploration note for exact passed fields and unresolved fidelity limits.

The native bridge batches at most 128 queued sends and 128 receives per loop. The FIFO is bounded;
all DirectPlay calls stay on one thread. It does not silently drop queue entries to improve a
benchmark. Isolated save/load probes are independent of this bridge and failed to restore a
world; no saved-game file is a validated checkpoint here.


`compare_replay.py HOST_LOG JOIN_LOG REPLAY_ROSTER_JSON` checks the final roster fields
reported in §6a against the original peers. It uses the original receiver's height for each
unit, because the observed owner-local heights differ. `--unit-limit` defaults to the sample's
1500. `playback-results.json` retains the measured runs and explicit unverified scope.

## Renderer-state experiment

`scene_blocks.py DIRECTORY` measures independent Zstandard blocks of the temporary scene
probe's `.bin` captures. Use the shared venv. The measured moving input is the local
`_local/demo-recorder-exploration/scene/samples-moving-10hz/` directory; its ordered combined
SHA-256 is stored in `scene-results.json`. The parser intentionally accepts only the measured
32-bit packet layout and packets no larger than 8 MiB. It refuses truncation and out-of-range
tables. This is an offline compression workload, not a general packet parser.

The unit/piece projection removes runtime address entropy for the size comparison but retains
other renderer-specific fields. It is neither a portable replay format nor the separate
fresh-process unit importer. Ten frames correspond to about one second only in the measured
10 Hz collection; the tool groups by count, not elapsed time. Codec timings exclude storage
IO and game work. See §6b for visible scene-switch latency, fresh-process asset relocation,
the incomplete state inventory and why these are not complete-world seek measurements.

`perspective-results.json` records the normal-DLL two-peer LOS/resource experiment (§6c).
The local evidence directory `perspectives/` contains the bounded `tacli peek` reader and
paused grids. Compare players by DPID rather than each peer's local seat. Counter-byte
differences and zero/nonzero sight differences are distinct measurements; only the latter
is the current-sight predicate used for the bit-mask compression numbers. Radar, sonar and
allied sharing are not covered by those masks.
