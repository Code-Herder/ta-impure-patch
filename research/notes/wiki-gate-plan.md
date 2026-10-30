# The wiki gate in parallel: plan

The in-game wiki's landing gate, `tools/wiki_regression.py`'s offline tier, its eight game modes
and its six-setup `mods` run, made fast by the rules the replay harness already follows
(`tools/replay/tareplay.py`): one run lock taken once, the games played as parallel lanes, pacing
counted in frames and ticks, and no check decided by the wall clock.

## The baseline (MEASURED 2026-10-29, the reclaim entry's L4 landing, the reference setup)

Every part run one after another, one launch per mode, about 105 minutes in all:

| part | wall time | where it goes |
|---|---|---|
| `- offline` | 1218 s | about fifteen independent checks, one after another |
| `demos` | 1676 s | four d4 mockup grabs of 359 s each (the defect below); the checks themselves about 4 min |
| `stage3d`, `units` | 542 s, 440–743 s | the captures, the mockup's renders (100–144 s), two whole 300-frame log lines per wait |
| `mods` | 1305 s | six setups in turn, about 218 s each: a Wine launch, a skirmish, three captures each waited on for two 300-frame lines |
| `sp`, `page`, `try`, `cursor`, `asset` | 160, 33, 159, 148, 24 s | five launches |

`demos`' d1 plays all 112 demo contexts in the game in 119 ms, and one `simparity.mjs --hash`
pass takes 2 s: the mode's time was its waits, not its work.

## The owner's decisions (2026-10-29)

- **The timing checks are fixed by construction**, not moved to a quiet lane or reported only:
  (d2) the demo's pace and (u6)/(m) the stage's render-thread cost each get a form that a loaded
  machine cannot fail.
- **The wiki gate first.** `tacompat.py wine` (already parallel, about 10 minutes) and
  `tareplay.py` stay their own commands.

## The landings

| | What | Gate |
|---|---|---|
| **L1** — the harness's own defects | The d4 mockup grab asked `WikiGfx.ready` for the bare entry id where the stages are keyed `<entry>.<scene>` since named scenes (2026-10-28), so its ready test never held and every grab polled its 300-try bound out -- 300 `playwright-cli` processes, 359 s -- and then shot anyway. The wait now runs inside the page in one call, asks with `WikiPage.stageId()` and fails the grab at its bound; `wikilaycheck.py`'s ready loop, which also fell through its bound, does the same | MEASURED 2026-10-29: one game-look grab 359 s → 6.4 s; `demos` 1676 s → 250 s, passing, its four d4 pairs matching; `wikilaycheck.py` passing in 106 s |
| **L2** — one gate, in parallel | `wiki_regression.py gate [--jobs N]`: takes the run lock once, runs the offline checks in a process pool and every game lane at once -- each mode, and each of the six setups, on its own instance and its own private X server -- and files itself with the runs viewer, one step a lane (`tools/tarun.py`), as `tareplay.py landing` does | MEASURED 2026-10-29 at `-j 6`: 30 lanes (16 offline checks, 8 modes, 6 setups) in 958 s, against some 105 min serially; the longest lane, `units`, 863 s (750 s alone). Re-run 2026-10-30 on the landed form, every instance on private hives: 30 of 30 in 1085 s, `units` 830 s and `simparity --wine` 560 s the longest, the shared `wineprefix/system.reg` untouched by the run. The timing checks held under that load: (d2) and (u6) passed. Its first run failed `stage3d` on a race in `tacli ls`, which read every instance's `instance.json` after scanning for them and crashed on one the retail `mods` lane had just removed: `ls` and the instance constructor now read the file once and take a missing one as no instance |
| **L3** — frames and ticks | The stage's frame count, `tagpu_wiki3d.count N`: the next N drawn frames from the first *at rest* (nothing built, baked or dropped since the drawn frame before, no record the bake refused), said in one line with the work over them and the milliseconds -- in place of two whole 300-frame log lines a wait, in (s4), (u5), (u6), (w3) and (m). A test-only pacing, `tagpu_wiki.turbo K` (a playing demo steps K ≤ 8 ticks a fill), and an exact pause, `tagpu_wiki.pause ENTRY DEMO TICK` (the demo picked afresh and played to stop on that tick), for (d3)'s end and (d4)'s four grabs. Found on the way, the slowest wait of all: `tacli ls --json` asked `pgrep` once per instance folder, 22.4 s over 634 of them, and every window grab lists -- now one process listing, 0.19 s. And the rest of the harness's serial waits: `stage3dcheck.py` runs while the game goes on to its next steps, (u7)'s reclaim plays at TA's top speed and its reads end with the solar, and `simparity` runs as four shards a variant (`--shard K/N`, each building its C side in its own folder) on eight offline workers | MEASURED 2026-10-30 at `-j 6`, landed with L4: 36 of 36 lanes in 361 s, against 1085 s for L2's; `units` 282 s → 162 s alone once its reads ended with the solar, `stage3d` 579 → 227 s, `demos` 293 → 147 s, each `mods` setup 230–279 → 87–130 s, the longest offline lane `simparity --wine` shard 1 at 277 s (the cases on it) against 560 s whole. (d4) paused on 150, 150, 190 and 812 exactly |
| **L4** — the timing checks by construction | (d2): the state dump gives the clock's own account since the demo started (`clock_s`, its clamped seconds, and `clipped`, the ticks the 8-tick bound took off); a fill leaves under one tick owed, so between two dumps of a demo playing at one speed on the clock the ticks played equal 30 × the speed × the seconds less those taken off, to within one -- held at 1x and 2x, under any load. (s5), (u6), (m): the stage judged by its work over the frame count -- no ground or type records built, nothing baked or dropped, no refusal -- with the milliseconds said, never asserted. (u7): bounded on GameTime, 12000 ticks, with a stood-still clock the only wall-clock failure | MEASURED 2026-10-30 under the whole gate at `-j 6`: (d2) 106 ticks in 3.524 s of the clock at 1x and 104 in 1.723 s at 2x; (s5) and (u6) at rest over 60 frames, nothing built, baked or dropped |

## Gaps open

- **A stage that only got slower passes.** (s5), (u6) and (m) catch work repeated frame to frame,
  not a producer that is merely slow at the work it should do; the milliseconds are said for a
  reader to compare, and nothing judges them.
- **The presented-frame grabs still wait on the wall clock**: a fixed sleep of 0.5 to 1 s after a
  shot or a pause before `import -window`, in (d4), (u2), (s2) and (c1). The dump's `drawn` is a flag,
  not a frame number, so no frame-ordered wait exists yet; (c1) judges its grab, the others keep theirs
  beside a check. A frame number on the hit map would make the wait an ordering.
- **(m) is not in the gate.** It needs the compatibility suite's two-player start and runs as `mp`;
  its work-based form is written as (u6)'s and was not run with this landing.
- How many lanes the reference setup carries at once was measured only at `-j 6` with eight offline
  workers.
