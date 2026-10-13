# Runs viewer — gates and games in progress

What a session's gates and games are doing: the plan, what ran, what passed, what is left and how
long it will take. `/runs` opens it in Claude Code; the same run files are readable without it
(`tools/tarun.py ls`).

Three pieces:

| piece | what it is |
|---|---|
| `tools/tarun.py` | the journal: one file per run, replaced whole on every change; the finished steps' times; the estimate; a command line for runs driven from a shell |
| the producers | `tareplay.py` (`run`, `landing`, `broad`, `measure`, `takeover`), `tacompat.py` (`wine`, `windows`), `tacli` (one instance's game), and the landing command through the command line |
| `.claude/skills/ta-runs/` | a Claude Code mod that reads the files and draws a pane, a band above the prompt and toasts |

A plugin in the project's `.claude/skills/` folder loads in every Claude Code session started in
the repository and reloads when its files change, so the viewer needs no install; a worktree has
it once it carries the folder.

## A run

A run is planned up front as **items** (test groups, setups) of **steps** (tests, stages). A run with
no items of its own (a landing, a tacli game) has one item named `""`.

| step state | meaning |
|---|---|
| `planned` | not started; `after` names the steps of its item it waits for |
| `running` | started; carries `instance`, `display`, and a game test's `gt` / `gt_target` |
| `done` | ended with no verdict of its own: a test whose checks run later |
| `pass` / `fail` | ended with a verdict |
| `unrun` | could not run, or never started because the run stopped |
| `skipped` | not needed, with the reason in `detail` |

An item carries its **checks**: how many passed, and by name every one that failed or could not
run.

**Liveness is the owner process, not the file's age.** A run records the process whose life is its
own, and that process's start time from `/proc/<pid>/stat` (field 22), so a reused PID is not
taken for it:

| owner | the process | gone with no end written reads as |
|---|---|---|
| `process` | the tool itself (tareplay, tacompat) | died |
| `instance` | the game (`TotalA.exe` under Wine) of a tacli run | exited |
| `session` | Claude Code (`CLAUDE_PID`), else the calling shell, for a run the command line drives, and a remote instance's game | abandoned |

**Nesting.** A gate passes its id to every process it starts (`TARUN_RUN`); a run started under it
is its child, and tacli under it writes nothing at all, since the gate's own step already names the
instance it plays on. A run started while this session and worktree have a run open that the
command line drives (`adopts`: the landing) is filed under that run's running step: the replay
gate's tareplay run shows inside the landing's `replay` step.

## The files

Under `TARUN_DIR`, default `$XDG_DATA_HOME/ta-runs` (`~/.local/share/ta-runs`), outside the
repository:

```
sessions/<session>/<run>.json   the run as it stands: written beside and renamed into place, so a
                                reader never sees half a file
sessions/<session>/.lock        the lock a command-line edit takes (a gate writes its own file alone)
history.jsonl                   one line per finished step: tool, item, step, variant, seconds
```

`<session>` is `CLAUDE_CODE_SESSION_ID`, which Claude Code exports to every shell it starts —
its subagents' included, so a `ta-runner`'s gate is the session's own run. A run started outside
Claude Code is filed under `terminal`. A session folder nobody wrote to for 14 days is deleted
when a run begins; `history.jsonl` is never pruned.

The journal never fails the tool that writes it: every write is guarded, an error switches the
journal off for the rest of that run with one line on stderr, and a journal that cannot open is a
null object whose calls do nothing. No file a tool reads is written by it.

## What each producer reports

**tareplay** (`run`, `landing`, `broad`, `measure`, `takeover`; `forks` and `rewind` write none).
Items are the test groups; steps are each test group's tests
([*Terms*](tadr-port/demo-recorder-completeness.md#terms)) under the one-letter ids tareplay's code
still writes, which the pane shows as the tests' names: record (`L`) first, every other test after
it, follow (`F`) after takeover (`K`), on the `game` pool of `-j` workers, ordered as a free worker
takes them (`LANE_ORDER`, then the test groups' record order), and a `checks` step after all of
them on the `offline` pool, which never waits for a game (`measure` has no checks). A test ends
`done`, or `fail` (a `Failed`) or `unrun` (`CouldNotRun`, or anything else) with the first line of
its reason; the tests it would have queued then end `unrun` too (record queues all the others,
takeover its follow). A test group's checks
end `fail` with every failing check's label and first line, `unrun` when none ran, else `pass`.
`landing`'s `UNCOVERED` lines and `broad`'s unexercised features, sites and tables are the run's
`problems`. The run ends `pass`, `fail` or `unrun` by the exit status, `stopped` after SIGTERM or
Ctrl-C.

GameTime is reported **only where the harness already learns it**: each pause it takes
(`wait_paused`) and each `Worker.gametime()` peek it makes. No peek is added for the viewer, so the
gate's pacing is what it was. A test's `gt_target` is the test group's end for `L` (and for `P` under
`measure`), `Tlast` for `P`, `Tlast + CONTINUE` for `R`, `T1` for `Q`; `K`, `F`, `X`, `U` and `W`
have none.

**tacompat wine.** Items are the setups; steps `prepare` (the `prep` pool, `--prep-jobs`), `run`
(the `game` pool, `-j`) and, with `--mp` on a setup `mp_eligible` admits, `mp` (the `mp` pool,
`--mp-jobs`). `mp` is `skipped` when Impure did not reach the menu. Steps end `done` with the
outcome and verdict, `unrun` when the run itself threw, and `mp` `fail` when the network game
failed. The setup's final verdict is its one check: *meets goal* and *known gap* pass, `UNEXPECTED`
fails by each way it differs, no result could not run. **tacompat windows**: `run`, and `smoke`
with `--smoke`, one setup at a time.

**tacli.** `launch` (and every verb that launches through it) outside a gate opens a run owned by
the game process. Any other verb naming the instance writes its command line (less the instance's
name) as the run's `last`; `stop` and `rm` end it, a remote instance's as well. A relaunch of the same name ends the
previous run `unrun`.

**The landing.** `/git_commit_merge_wt` begins a `landing` run with the steps `commit merge build
docs replay review ff` and marks each as it goes:

```bash
tools/tarun.py begin landing --title "landing $(git branch --show-current) -> main" \
    commit merge build docs replay review ff
tools/tarun.py step landing replay start
tools/tarun.py step landing replay pass        # fail, unrun, or skipped --detail "why"
tools/tarun.py end landing pass
```

A gate run again after a fix is started again, and drops what its last attempt said. The same
commands serve any script: `begin TOOL [--jobs N] STEP...` (the steps in order, or `N` at
once), `step RUN STEP start|done|pass|fail|unrun|skipped`, `end RUN pass|fail|unrun|stopped`, where
`RUN` is a run id or the newest open run of that tool in this session and worktree.

## The estimate

Each step's expected seconds are the **median of its last five** finished times with the same
tool, item, step and **variant** (tareplay: `gs<speed>-turbo<k>`, `-cover` on the coverage build;
tacompat: its watch, battle and network seconds). A running step is left the longer of its
expected time less its elapsed time and, where it has two GameTime readings, the time their rate
takes to its target: history knows a test's tail after its last GameTime (dumps, collection, the
exit), the rate knows a test running slower than it ever has. The steps left are then laid onto
the run's pools in the tool's queue order, each after the steps it waits for; the estimate is when
the last one ends. A step with no history of its own (a new test group, the first run of a variant)
borrows the median of the same step over the tool's other items in that variant, each item
counted once; a step with none at all counts as zero and the estimate becomes a lower bound (`≥`);
with nothing known there is none.

MEASURED 2026-10-09, three runs of `tareplay.py run secondary-orders c2-weapon-keys -j 2` (two
tests at once, about two minutes): with the second and third run's history, the estimate read at
15, 30, 45, 61, 76 and 91 s into the third was 76, 86, 71, 56, 54 and 32 s against 114, 99, 84,
68, 53 and 38 s actually left. It reads low early by the workers' start-up (each worker creates
its instance and starts its Xvfb before its first test, about 20 s here), which is no step, and
converges as the tests end.

MEASURED 2026-10-09, the 39-test-group `tareplay.py landing` at `-j 8` on the coverage build: planned
from scratch with the history its own run left, the estimate is 832 s against the 842 s it took.
With one test group's history removed (`battle-los`), its tests borrow 154 s against its own 199 s.

## The viewer

The mod reads its session's folder every two seconds, re-reading a file only when its mtime moved,
and the folders of the sessions written in the last 24 hours for their running runs. A run's
liveness is read from `/proc`, as above. Nothing in it writes a file a tool reads or acts on a run.

- **The pane** (`/runs`): two sections, **IN PROGRESS** (this session's running runs) and **DONE**
  (its last five ended ones, newest first), each run with the runs filed under it, then the other
  sessions' running runs under one folded line, `OTHER SESSIONS (N running)`, labelled by
  worktree. A running run is open, an ended one folded unless it did not pass (fail, unrun,
  stopped, died, abandoned, exited); a press on a run's line opens or folds it. A section's titles
  are padded alike, so a DONE row reads as a column: outcome, time taken, when it ended, then
  how far it got when that was short of the plan, and its failures. Inside a running run: its
  problems, a bar and the count of **items with every step ended** (a test group counts once its
  checks have a verdict; a run with no items of its own counts steps), the failures and the
  estimate. Its items are grouped:
    - **running** — an item playing a step on one of the run's worker pools, one row each (so no
      more rows than workers): every ended test as its name and glyph, a running test with its
      GameTime against its target, or its time;
    - **checking** and **waiting** — an item whose only running steps wait for no worker (tareplay's
      `checks` on `offline`), and one between tests waiting for a worker: each one line of names;
    - **done** — the count passed and failed, each item that did not pass by name with every failed
      or unrun step and check, and the passed ones behind their own folded line (`▸ ✓ N passed`),
      each with its checks and wall time when opened;
    - **planned** — one line of names.

    An ended run keeps the same groups, with what it stopped in as **cut short** and what it never
    reached as **not started**, and adds the tool's report. A `checks` step that failed (or could not
    run) with no reason of its own is the verdict of the checks its item lists by name, so it is
    neither listed nor counted beside them. The pane opens by itself for a new run of this session
    only where it docks beside the transcript (the fullscreen layout); a tacli game never opens it.

- **The band** above the prompt, while anything runs: a row per running run of this session, with
  the progress of a child run that is running after it (`landing … › tareplay landing …`), and one
  row for the other sessions. It sits under whatever else draws there.

- **Toasts**: a run of this session ended (its state, time and first failure), and its first
  failure while it runs. A tacli game stopped with `tacli stop` raises none; one that exited on its
  own does.

## Gaps

- **No GameTime for an ad-hoc tacli game.** Nothing polls it, and the viewer must not: `tacli
  peek` has one trigger file per instance, so a poll would race the session's own peeks and add a
  log line each time. A DLL heartbeat file is what would close it.
- A test's GameTime moves only at its pauses and the harness's own peeks; between them the
  estimate counts down.
- The estimate leaves out the workers' start-up, and `-j` is not part of the variant: a run at
  another `-j` uses the same history. A borrowed test time is the median over test groups whose
  tests differ in length (a record test ran 58 to 141 s on the coverage build).
- Another session's run is shown only while it runs.
