# Shared brief: reducing the size of Impure's demo recordings

You are one of five researchers working in parallel, each on one area. Two challengers will
examine your report afterwards, so state what you measured and what you inferred, and label each.

## The project

Impure patches Total Annihilation (1997, 32-bit exe) through a `ddraw.dll`. Its demo recorder
captures the engine's simulation state every sim tick (30 ticks a second at normal speed) on the
game thread into a ring; a writer thread packs blocks and writes `replays\*.imprec`. Playback runs
the recording on "puppets" (the engine's units driven by the recording), can seek to any GameTime
(from the nearest snapshot, then the stream forward) and can take over (turn the playback into a
live game from any point, which needs the exact world). Multiplayer recordings add contributions
from other peers over the network.

## Files (read-only; the worktree is the repository)

- Worktree: `<repo>`.
  Run commands from there. NEVER edit, build, commit or launch the game. Write only under your own
  folder in the scratch dir below.
- The format: `tagpu/ddraw/inc/tagpu_recfmt.h` (record kinds, image sizes, delta layouts).
- The capture: `tagpu/ddraw/src/tagpu_recorder.c`; the writer: `tagpu/ddraw/src/tagpu_recwriter.c`
  (lodepng deflate, default settings = 2 KB window); the DLL reader `tagpu_recreader.c`, the
  playback `tagpu_replay.c`, the puppet `tagpu_puppet.c`.
- The Python reader: `tools/replay/tarec.py` (`blocks()`, `records()`, `World.apply()` decode
  every record kind). The size tool: `tools/replay/recsize.py` (read it: it shows how to walk the
  stream and decode deltas).
- What each engine field is: `tagpu/ddraw/replay-layout.tsv` (unit/mover/player/projectile rows
  the recorder treats specially), `tagpu/ddraw/replay-globals.tsv`, and the engine map
  `research/notes/exe-reverse-engineering.md` (search an offset or address). The retail exe for
  disassembly: `"$(git rev-parse --git-common-dir)/../pristine/TotalA.exe.pristine"` (image base
  0x400000), `i686-w64-mingw32-objdump -d -M intel --start-address=0x… --stop-address=0x… <exe>`.
- The design notes: `research/notes/tadr-port/demo-recorder-size.md` (READ FIRST: the
  measurement and the six ranked ideas so far), `demo-recorder.md` (product contract: *Disk
  budget*, open decisions), `demo-recorder-state-evidence.md` §4 (size estimate, the tolerance
  sampler's measurements), `demo-recorder-skirmish-slice.md` *Measured* (the 2.5x scratch
  repacking), `demo-recorder-completeness.md` (what must be recorded, the classes of state).

## The data

Scratch: `$SIZELAB/`

- `size-battle.imprec` — 99.6 MB, ten players on Core Prime, 2,010 units at the start (five ARM vs
  five CORE armies of 200), 7,830 ticks (4 min 21 s); 1,115 alive at tick 952, 557 at 2,804, ~350
  from 6,500 on. 26,782 shots, 2,087 deaths.
- `size.txt` / `size.json` — recsize.py's full output for it (categories, field tables, timeline,
  compressors). Re-running recsize.py takes ~3.5 min; reuse the JSON where you can.
- `tagpu.log` — the run's log (the recorder's own cost lines).
- A smaller sample: `.../scratchpad/gate4/run/tenseat/L/tenseat.imprec` (ten seats, few units).
- Python with numpy and zstandard: `<main checkout>/.venv-undither/bin/python`
  (the system python3 lacks them). `pip install` into that venv is allowed if you need a package
  (e.g. brotli, lz4); say what you installed.

Headline numbers (packed shares): unit deltas 49.9 % (framing 57.6 MB raw > values 50.1 MB),
projectiles 39.1 % (each delta rewrites ~21 of 27 dwords), cells 3.8 %, events 3.1 %, order lists
3.0 %; snapshots 1.6 % of the file. zstd-19 per block = 0.61x the writer, lzma 0.53x, whole-stream
zstd 0.48x.

## Constraints every idea must respect

- **Seek** starts from a snapshot (every 900 ticks today) and plays the stream forward; anything
  that needs the whole stream before a point breaks seek. **Takeover** needs the exact world at
  the takeover point: snapshots stay exact at full precision.
- **The product contract** already decided the reduced stream's precision: moving units at 10 Hz,
  whole world units, 8-bit headings, interpolated on playback; snapshots full precision.
- **Verification:** today every value at every tick is exact and the replay gate compares dumps
  EQUAL. A lossless idea keeps that. A lossy one needs tolerances, which are the owner's decision
  (a loosening): say exactly what would become approximate.
- **Cost:** the capture runs on the game thread (budget 0.5 ms average, 1 ms worst at 1,500 units;
  today 337 µs average on this battle). Packing/compression runs on the writer thread. 32-bit
  MinGW DLL: a compression library must be small and C. Memory: the ring is 16 MB.
- **Multiplayer:** the same records travel as contributions between peers; smaller is better there
  too, but the file is the target here.
- **Owner preferences:** robust by construction (no brittle hand-maintained lists of fields where a
  general mechanism works), no unrequested extras, fixes safe by design not by timing.

## What to deliver

1. **Measure, don't just argue.** Where an idea can be prototyped offline in Python on
   `size-battle.imprec` (re-encode a category, compress it, compare), do it and give the number in
   MB against today's packed size for that category. Check a lossless re-encoding decodes back
   exactly (round-trip the bytes). Keep each experiment's script in your folder.
2. **Use the known tricks** from network-game state replication and data compression: delta
   against the previous value or a prediction (dead reckoning, linear/quadratic extrapolation),
   bit packing, varints/zigzag, column (struct-of-arrays) layout, run-length and sparse bitmaps,
   field-group masks, "same as last time" flags, priority/relevance sampling, error-bounded
   sampling, quantization, keyframe interval, entropy coding (zstd levels, trained dictionaries,
   long-distance matching, rANS/arithmetic coding, context models), whatever fits your area. Name
   the precedent when you use one (Quake 3/Tribes delta snapshots, Overwatch/Halo replication,
   Parquet/columnar, etc.).
3. **Write your report** to `research/<your-area>.md` in the scratch folder (`.../size/research/`)
   — under 1,500 words plus tables: each idea with mechanism, measured or estimated saving (MB on
   this file and the share of the file), CPU and complexity cost, fidelity effect (lossless /
   lossy and what), effect on seek, takeover, verification, multiplayer; risks; prerequisites
   (engine facts to establish). End with your ranked recommendations.
4. Your final reply: the report's path and a 10-line summary of the ranked recommendations with
   their numbers.
