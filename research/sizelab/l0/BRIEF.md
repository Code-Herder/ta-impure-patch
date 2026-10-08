# L0: do the size codecs generalize beyond the battle they were built on?

The size research round (2026-10-07) prototyped lossless re-encodings of Impure's demo recordings
on ONE recording: `size-battle.imprec`, ten players, 2,010 ground units on flat Core Prime, no
aircraft, no hills (pitch, roll and vertical velocity never changed). The owner has adopted the
plan (`research/notes/tadr-port/demo-recorder-size.md`, read *The research round* and
*Decisions*). L0 runs each codec, unchanged in its rules, on other recordings, before any DLL work.

## Where things are

- Worktree (run from it; read-only — never edit, build, commit or launch the game):
  `<repo>`.
- The prototypes and each area's report: `research/sizelab/<area>/` (README.md = the report;
  `research/sizelab/README.md` = how to run). Scripts take their data folder from `$SIZELAB` or
  arguments; when a script hard-codes `size-battle.imprec`, pass/point it at the new file (a
  wrapper or env var in YOUR folder — do not edit the committed scripts; copy a script into your
  folder and modify the copy if you must, and say so).
- Python: `<main checkout>/.venv-undither/bin/python` (numpy,
  zstandard, numba). Format reader: `tools/replay/tarec.py`; size breakdown: `tools/replay/recsize.py`.
- Recordings, in `L0=$SIZELAB/l0/`:
  - `battle.imprec` — Town & Country (hilly), four seats, ~1,440 ground units in four armies,
    bases, factories, 12 aircraft (ARMFIG, CORVENG), 4,021 ticks, 83 MB.
  - `naval-push.imprec` — Anteer Strait, naval units, 3,841 ticks, 10.6 MB.
  - `air-war.imprec` — Two Continents, 200 aircraft over flak bases. BEING RECORDED NOW; you will
    be told when it exists. Do not wait or poll for it: finish the other two first.
  - The original `size-battle.imprec` is in `L0/..` (the `size/` folder) as the reference.
- Write ONLY under `L0/<your-area>/` (caches may be large; disk has ~280 GB free; delete caches
  of your own that exceed 5 GB when you are done with them).

## The question per recording

1. **Lossless:** does every codec decode back exactly (0 mismatches, every iteration)? A mismatch
   is the main finding — find its cause.
2. **Size:** today's packed size of the category (writer = per-block deflate as recsize.py
   measures it) against the codec's, and the ratio, next to the battle's ratio.
3. **Assumptions:** which engine-rule predictors still hold and how often (e.g. the sine-table
   velocity rule on aircraft, hovers, ships; position ← velocity on slopes), which fields change
   here that never changed on the battle, and what that costs. A rule that stops predicting is
   not a bug (the per-unit switching falls back), but say what it costs.
4. **Cost:** anything that grows badly (an encoder's search, memory, time).

Label MEASURED / [INFERRED]. Report `L0/<area>/l0-<area>.md`, under 800 words plus tables; final
reply: its path and a 10-line summary.
