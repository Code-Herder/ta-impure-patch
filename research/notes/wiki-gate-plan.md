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
| **L2** — one gate, in parallel | `wiki_regression.py gate [--jobs N]`: takes the run lock once, runs the offline checks in a process pool and every game lane at once -- each mode, and each of the six setups, on its own instance and its own private X server -- and files itself with the runs viewer, one step a lane (`tools/tarun.py`), as `tareplay.py landing` does | every lane's verdict equal to the serial run's; the whole under the longest lane |
| **L3** — frames and ticks | A trigger the DLL answers after N frames with how many drew the image, in place of waiting for two whole 300-frame log lines; a test-only wiki pacing, K demo ticks every frame (`tagpu_wiki.turbo`), and a pause at tick T, so a check stops at its tick instead of racing a pause to it | the captures and the d4 pauses at exactly their ticks; no wait on a log line's period |
| **L4** — the timing checks by construction | (d2): the pace judged on the game's own clock -- the ticks stepped against the clamped seconds the page accumulated, the rule `page_fill` paces by -- not against the harness's wall clock. (u6)/(m): the stage's cost judged by its work -- the ground built once a stage, the bake's entries kept, nothing rebuilt or re-baked frame to frame -- with the milliseconds logged, not asserted | the checks pass under a fully loaded machine, and fail on a change that breaks the rule they hold |

## Gaps open

- (u6)/(m) have no work-based form yet: which counters the producer and the bake keep, and which
  of them a slow frame would show, is L4's research.
- How many lanes the reference setup carries at once -- Wine games, Xvfb servers and the headless
  GPU renders of `stage3dcheck.py` together -- is measured in L2, not assumed.
