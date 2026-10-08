# The recording size lab

The prototypes, outputs and reports of the demo recorder's size research round (2026-10-07). The
conclusions, the plan and the owner's decisions are in
[making recordings smaller](../notes/tadr-port/demo-recorder-size.md); this folder keeps the
evidence and the code to re-run or extend it.

| Folder | Area | Its main finding |
|---|---|---|
| `units/` | unit images between snapshots | a per-field frame predicted by the engine's rules: 46.7 → 4.5 MiB, lossless (`frame2.py`) |
| `weapons/` | the projectile pool and the weapon slots | the pool coded by identity: 36.5 → 2.5 MiB, lossless (`codec*.py`, `pool.py`) |
| `coding/` | framing, container and compressors | zstd as a drop-in; the sectioned container; `bench/bench.c` is the 32-bit CPU benchmark |
| `discrete/` | cells, orders, events, globals, snapshots | 10.2 → 3.6 MiB, lossless; the snapshot wreck base |
| `fidelity/` | quantization, sampling rate, what must stay exact | truncate, never round; size-against-error curves |
| `mp/` | multiplayer contributions | recording the composed world instead of the packets |
| `mp-challenge/` | the multiplayer challenger | the timing rule re-indexed; variant K (the remote packets kept compactly) measured |
| `challenge-a/` | the first challenger: the numbers and the engine facts | re-runs and disassembly checks |
| `challenge-b/` | the second challenger: the combined plan | the non-double-counted stack, the landing order |
| `l0/` | L0: every codec on Town & Country, naval and an air war | lossless everywhere; aircraft cost ~10× a ground unit; the guided-projectile rule; the search cap |

Each folder's `README.md` is that area's report; `BRIEF.md` is the brief every researcher worked
from. Outputs (`*.out`, `*.jsonl`, `*.json`) are the runs the reports cite.

## Running

- **The data folder.** The scripts read and write under `$SIZELAB`: put the recording there as
  `size-battle.imprec`. It is made by recording `scenarios/size-battle-10p.json` with the recorder
  armed (`tacli arm <instance> recorder.on`, then `tacli scenario load <instance>
  scenarios/size-battle-10p.json --defaults --los 0 --unit-limit 250`, ended at GameTime 7,200 with
  `scenarios/replay-end.json`); the measured one is 99.6 MiB, 7,830 ticks, build `337246d4`.
  `tools/replay/recsize.py` gives its breakdown. Several scripts cache large intermediates
  (`*.npy`, `*.npz`, pickles, gigabytes for `units/` and `fidelity/`) beside themselves; they are
  regenerated on demand and are not committed.
- **Other maps and unit limits**: the discrete scripts read the map's grid from the header;
  the unit scripts take the slot count from `$SIZELAB_SLOTS` (2501 at unit limit 250, 5001 at
  500). `discrete/run.sh` and `units/runall.sh` run each area's pipeline on one recording.
- **Python**: the shared venv (`.venv-undither/bin/python` at the main checkout), which has numpy,
  zstandard, brotli, lz4 and numba. The shell scripts take it as `$PY`.
- **The exe**: `units/sintab.py` reads the engine's sine table from the pristine exe named by
  `$TA_PRISTINE` (`"$(git rev-parse --git-common-dir)/../pristine/TotalA.exe.pristine"`).
- **Other recordings** the reports name (the two-peer and ten-seat multiplayer files, the Town &
  Country and naval battles) are not kept; record them again with their fixtures
  (`tools/replay/fixtures.json`) or the multiplayer tests.

The scripts were written as one-off experiments: paths, argument conventions and file names vary
between folders, and the reports name the command lines they ran.
