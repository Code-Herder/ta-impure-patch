# Demo recorder research tools

These inspect the 2026-09-28 transport experiment and measure a disposable block container.
They do not record a game, implement replay, establish a checkpoint, or measure game seeking.
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
