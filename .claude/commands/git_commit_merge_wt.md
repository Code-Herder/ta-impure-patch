---
allowed-tools: Bash(git:*), Bash(make:*), Bash(.venv-undither/bin/python:*), Bash(grep:*), Read, Edit, Write, Agent
description: Land this worktree on local main — commit, merge main in, then CHECK every landing gate (build, documentation pass, Opus review) and run whichever is missing before fast-forwarding main
---

# Git Commit + Merge Worktree — the landing, with its gates checked

Argument (optional commit message): `$ARGUMENTS`

**Invoking this command is the human saying "this is ready to land."** That is the trigger the
review gate waits for (CLAUDE.md, "Review engine changes before they land"), so the review may
run from inside this command without asking again. It is also the human saying "I may have
forgotten a gate": every gate below is **checked first and only then done**, so a landing whose
docs or review were already handled costs nothing extra, and one where they were forgotten gets
them here instead of on `main`.

The gates, in order:

1. Commit any local changes in the current worktree.
2. Merge `main` into this branch (fast-forward, or a merge commit if diverged).
3. Build gate — both DLLs compile on the post-merge tree.
4. Documentation gate — the pass CLAUDE.md requires, checked against the diff.
5. Review gate — `medium` (or `high`) review **on Opus**, checked against a record of what was
   reviewed; findings verified and fixed as new commits; build re-run if anything changed.
6. Fast-forward local `main` to this branch. (This repo has no remote — "shipping" means landing
   on the local `main` ref. If a remote is ever added, add the push here.)

If any step produces an unexpected state (conflicts, non-fast-forward, detached HEAD, failing
builds, a review you cannot verify) **STOP** and report the state. Never use `--force`,
`--no-verify`, `reset --hard`, or rewrite history.

## Why there is a bar at all

**Committing and landing are different acts.** Commits on the worktree branch are cheap
checkpoints — make as many as are useful, including work in progress. **Landing** is
fast-forwarding local `main` (Step 6), and that is the act with a bar. Historically this repo has
run at roughly one landing per 1.7 commits, i.e. almost every commit went straight to `main`;
that is what makes the review expensive and what lets half-checked work reach `main`.

**Land one FINISHED unit of work, not one commit.** A gate, a fix, a documentation pass. If the
next thing you do would be "…and now correct what I just landed", it was not finished.

Before Step 6, all of these hold:

1. **It does what it claims**, verified by running it, not by reading it. Both DLLs compiling is
   Step 3's gate, not evidence the change works.
2. **Its claims are checked.** This bites documentation hardest: a build gate cannot catch a
   wrong statement. Do not land a page and then discover a claim in it was too rosy — check the
   claims against the source *first*.
3. **Nothing is left half-done behind it** — no debug instrumentation, no counters added to
   chase a bug, no `.on` file the change depends on but does not create.
4. **The documentation it taught us is written down** — Step 4, in the same landing.
5. **It was reviewed, on Opus, and the findings were acted on** — Step 5.

## Step 1 — Commit local changes

Run these in parallel:
- `git status --porcelain`
- `git diff HEAD`
- `git log --oneline -10` (to match local commit style)

Then:

- **If `git status --porcelain` is empty**: skip to Step 2.
- **If there are merge conflicts** (lines starting with `UU`, `AA`, `DD`, `AU`, `UA`, `DU`, `UD`): **STOP**. Report the conflicted files and ask the user to resolve.
- **Otherwise**:
  - **If `$ARGUMENTS` is non-empty**: use it as the commit title verbatim.
  - **Otherwise**: draft a 1–2 sentence commit message from the diff, matching the style of recent `git log` entries (imperative, capitalized first word, short title).
  - Stage only the files reported by `git status --porcelain` (named explicitly — do NOT use `git add -A` or `git add .`).
  - **Use judgement on what belongs in the commit — a dirty file is not automatically intentional work.** Read the diff of anything that looks machine-written, environment-specific, or unrelated to the branch's stated purpose, and challenge it rather than staging it: build outputs (`*.dll`, `*.o`), captures/recordings, wineprefix state, instance scratch dirs, generated symbol dumps. Leave anything suspicious unstaged and say so in the summary.
  - **Skip files that may contain secrets** (`.env`, `*.pem`, `credentials*`, `*.key`). Warn the user if any are in the change set and wait for explicit authorization.
  - Commit using a HEREDOC, with the standard `Co-Authored-By` trailer this session's harness specifies:
    ```
    git commit -m "$(cat <<'EOF'
    <title>

    <optional body>

    Co-Authored-By: Claude <noreply@anthropic.com>
    EOF
    )"
    ```
  - If a pre-commit hook fails: investigate the root cause, fix it, re-stage, and create a NEW commit. **Do NOT** use `--amend` or `--no-verify`.

## Step 2 — Merge main into this branch

Before the docs and the review, so that both look at the tree that will actually land.

- If the current branch IS `main`, **STOP** — this flow is for worktree/feature branches.
- Run `git log HEAD..main --oneline`.
- **If the output is empty**: `main` has nothing new for this branch — skip to Step 3.
- Otherwise, before merging, check whether any incoming commits touch files that are about to conflict. Not required, but surfaces overlap early:
  ```
  git log HEAD..main --name-only --pretty=format:
  ```
- Run `git merge main --no-edit`.
  - **On fast-forward**: continue to Step 3.
  - **On conflict**: **STOP**. Report the conflicted files and exit. Do NOT run `git merge --abort` automatically — leave the state for the user to resolve.
  - **If a non-fast-forward merge commit would be created** (i.e., this branch has diverged from main): accept the merge commit that `--no-edit` produces, but report it explicitly so the user knows a merge commit was created.

## Step 3 — Build gate

Always run the gate on the post-commit, post-merge tree so a broken tree never reaches `main`.
There is no test suite; the gate is that both DLLs compile cleanly.

- **Skip entirely only if nothing changed**: if Step 1 committed nothing AND Step 2 merged nothing AND `git log main..HEAD` is empty, there is nothing new to gate.
- **Per-target skip (optimization, not a shortcut):** a target may be skipped only when the landing changes **zero** files it covers — check `git diff --name-only main...HEAD` against `tagpu/ddraw/**` (ddraw.dll) and `tagpu/src/**` + `tagpu/ddraw/inc/tagpu.h` (tagpu.dll). Call out any skip in the summary.

Run each build in the foreground, sequentially:

- **ddraw.dll** — `make -C tagpu/ddraw`
- **tagpu.dll** — `make -C tagpu`

Treat new warnings from changed files as worth reporting even when the build succeeds.

**On failure — STOP before touching `main`.** Investigate, fix, commit a **NEW** commit (never `--amend`/`--no-verify`), and re-run the gate.

## Step 4 — Documentation gate: check it, then do what is missing

**The landing's diff is `git diff main...HEAD`** (Step 1 committed everything, Step 2 merged
main in). Decide from it:

- **Does the landing touch engine code?** `tagpu/ddraw/src/**`, `tagpu/ddraw/inc/**`,
  `tagpu/src/**`, or `tools/tacli`. If not, the gate passes — say so and go to Step 5.
- **Which engine addresses did it touch or read?** Every VA on an *added* line of the code half
  of the diff (context and removed lines are the previous landing's business):
  ```
  git diff main...HEAD -- tagpu tools | grep -E '^\+' | grep -oE '\b0x0*[45][0-9A-Fa-f]{5}\b' | sed -E 's/^0x0*/0x/' | sort -u
  ```
  and the `main+0x…` / `+0x…` field offsets the comments name.
- **Are they in the notes?** Each of those must already be in
  `research/notes/exe-reverse-engineering.md` (the docs commit is part of `main...HEAD`):
  ```
  for a in <the list>; do grep -q -i "$a" research/notes/exe-reverse-engineering.md || echo "MISSING $a"; done
  ```
  A `MISSING` line means the engine map was not updated for this landing. It is also how a
  comment that cites an address the note spells differently gets caught (on G13k the code said
  `0x45A658`, the `je`, where the note had the `test` at `0x45A655`) — make them agree.
- **Did the module docs move?** For engine code the landing must also touch at least
  `research/notes/gpu-status.md` (hook map / *fields we write*) and `research/notes/roadmap.md`
  (capability row and gate entry). `.claude/skills/ta-*/SKILL.md` only if how you drive or
  measure the game changed.
- **Does the wiki build?**
  ```
  .venv-undither/bin/python research/build_wiki.py
  ```
  It must end `built N pages + index`; then grep `research/site/*.html` for the section you
  touched. `research/site/` is gitignored, so the only cost is the run. Name the venv's
  interpreter — the system `python3` has no `markdown`.

**If anything above is missing, do the pass now** and commit it as its own commit ("docs: …")
before Step 5, so the reviewer reads the claims. The pass itself:

**The engine map first: `research/notes/exe-reverse-engineering.md`.** Update it as fully as the
work allows. It takes every address the work **touched or merely read**, not only the ones we
patched:

- what the function is, in one line, and **its call sites** (an `E8`/`E9` scan of `.text` for the
  target is cheap and it is the fact nobody has when they need it);
- the **fields it owns**, with the layout gotchas — strides that are not what they look like,
  bits whose meaning is known, names that are guessed;
- **how each fact was established**: disassembly of the pristine build, or a live measurement,
  and then quote the numbers. Section-mark our own work so it is not confused with the vendor
  corpora, which are `[VERIFIED]` against TADR's asserted layouts.
- **Negative results count and are often the most valuable.** A function with no callers at all;
  a per-cell loop that bounds-tests with unsigned compares next to one that does not check at
  all; a trigger that is an equality rather than a band. These are what stop the next person
  re-deriving, or assuming symmetry that is not there.

**Then the module docs**, each in its own place:

- `research/notes/gpu-status.md` — the §2.x hook map (VA, what it is, mechanism) and the
  **"State we read, and the fields we write"** table. Anything newly written, or newly written
  *from a second thread*, belongs in that table.
- `research/notes/roadmap.md` — the capability row and the gate entry, **including the gaps the
  landing did not close.**
- `.claude/skills/ta-*/SKILL.md` — only if how you *drive or measure* the game changed. A
  procedure that cost you an hour to work out is the thing to write here.
- And **correct whatever the work proved wrong.** A stale note is worse than a missing one.

**The bar for the prose is the same as rule 2 above**: every claim traceable to disassembly or a
live measurement, never to memory or to an agent's report you did not check. Mark guessed names
`[INFERRED]`. Say what is *not* covered rather than writing as though it were.

**Why the docs are committed BEFORE the review.** The reviewer reads the accumulated branch diff
and it disassembles, so the claims get fact-checked for free — on the G13g landing it returned a
MEDIUM against a documentation overclaim ("the second eye pair is a copy taken right after every
clamp call, so it follows too") that the header comment, `gpu-status.md` and `roadmap.md` all
repeated, and it was wrong: three sites clamp that pair inline and never call the patched
function.

**Why this is a gate and not a nicety:** the addresses are the expensive part of this project.
They get re-derived every time they are not written down, and a *wrong* line costs more than a
missing one — G13g spent several probes chasing a skill note that claimed edge scroll "does not
fire under injected input" (it does; the trigger is an exact equality on the outermost pixel).
The most reused output of that landing was the page for functions we only *read* — the camera
stepper `0x41CA30`, the scroll poll `0x41CF10`, the dead clamp `0x41C450`.

## Step 5 — Review gate: check it, then run it if it is missing

### Does this landing need a review?

**Yes** when `git diff --name-only main...HEAD` touches:

- `tagpu/ddraw/src/**` or `tagpu/ddraw/inc/**` (the fork and our modules), or
- `tagpu/src/**` (tagpu.dll), or
- `tools/tacli` (it drives every session; a bug here costs hours).

**No** — say so in the summary and go to Step 6 — when the landing is only docs
(`research/notes/**`, `*.md`), scenarios (`scenarios/*.json`), comments, or a handful of lines
with no new state, no new engine patch and no new GL object. A review costs roughly 100k tokens;
it is worth that for a real change and not for a typo. Batching landings is what keeps this
cheap: three commits that land together cost one review, not three.

**Effort: `medium`**, or **`high`** when the landing writes engine or user state, adds or moves
a byte patch, or touches anything sim-adjacent. That is the class that ships silently: the review
that caught `ScrollSpeed` being persisted into the player's registry — where it would have
compounded across launches — was exactly this case.

### Was it already reviewed?

Reviews are recorded as **git notes** on the reviewed commit (they live in the repo, are shared
by every worktree, and never touch the tree). Check:

```
git log --format='%h %s%n%N' main..HEAD | grep -B1 'landing-review:'
```

- **No `landing-review:` note anywhere in `main..HEAD`** → the review is missing. Run it (below).
- **A note exists on some commit `R`.** List what came after it: `git log R..HEAD --oneline` and
  `git diff --stat R HEAD`.
  - Nothing after it → the gate passes.
  - Only the commits that acted on that review's findings, or docs → passes; say so.
  - Anything else in the review paths (new code, a further fix that is not one of the findings)
    → the reviewed diff is not the diff that lands. Re-run the review on `main...HEAD`.

### Running it — on Opus, not on the session model

**The built-in `/code-review` cannot be used for this gate.** It launches as a *fork* of the
session, and a fork always runs on the session's model — the `model` override is ignored
(measured 2026-09-03: `/code-review medium` started as `@code-review`, "forked execution", on
the session's Fable model and had to be stopped). The review is therefore a **general-purpose
Agent with `model: "opus"`** (Opus 5), given the brief below. One agent at `medium`; at `high`,
two in parallel with the same brief and the second told to focus on state written, patches and
sim-adjacent code, then merge their lists.

The brief must contain, in the agent's own prompt (it starts with no context):

1. **Where and what**: the worktree path (it must run every command from there and never `cd`
   elsewhere); the scope `git diff main...HEAD`; a one-paragraph description of what the change
   does and why, in engine terms — the addresses patched, the state written, the GL objects added.
2. **The effort contract**: `medium` = report only findings verified against the code,
   correctness first, then clear reuse/simplification wins — fewer, high-confidence findings
   beat a long list; `high` = broader coverage, uncertain findings allowed but labelled.
   **Read-only: it must not modify files and must not run the game.**
3. **How to verify**: the binary at
   `"~/.local/share/Steam/steamapps/common/Total Annihilation/TotalA.exe"` (image base
   `0x400000`, VAs map directly) and the disassembly command
   `i686-w64-mingw32-objdump -d -M intel --start-address=0x… --stop-address=0x… "<exe>"`;
   that patch-site bytes, branch targets and the registers the targets test must be checked
   against it; which notes hold the static context (`research/notes/exe-reverse-engineering.md`
   and the module note the change cites).
4. **The docs are part of the review**: a stated address or claim in the diff's notes that the
   disassembly or the code disproves is a finding.
5. **What to look at specifically**: list the risky spots you know of — every byte patch, every
   new engine-state write, static arrays reused across frames (a field not written on one path is
   last frame's), index bounds on new vertex ranges, option-bit gating, behaviour for the units or
   modes the change was *not* about (partial arms, non-structure units, zoom ≠ 1).
6. **The report format**: a ranked list, most severe first; per finding the file and line, a
   one-sentence defect, the concrete failure scenario (inputs/state → wrong output), and
   `CONFIRMED` (verified in code or disassembly) or `PLAUSIBLE`; an explicit "nothing survived
   verification" if that is the answer; under 600 words.

### Acting on it

**Verify every finding against the code (and the disassembly) before acting on it.** The hit rate
on this codebase is high but not perfect — on the G13e diff, 2 of 11 findings were HIGH and real
(a dropped button *release* left the engine and the shield's virtual key state holding the button
forever; `ScrollSpeed` corrupting the registry), several MEDIUMs were real, and about a third
were overstated or described existing intentional behaviour as a bug. Fix what is real, say what
you rejected and why. **Never apply findings blindly** (no `--fix`-style bulk application).

Findings become **new commits on the branch** before Step 6; that is what the branch is for.
Then **record the review** on the commit whose diff it read, and re-run Step 3 if any fix
touched a build target:

```
git notes add -m "landing-review: model=opus effort=<medium|high> range=main..<sha> findings=<n> acted=<k> rejected=<m> date=<YYYY-MM-DD>" <sha>
```

(`git notes add -f` only if that commit already carries a note — never delete someone else's.)
Re-run the review only if the fixes were themselves substantial (the check above will say so).

## Step 6 — Fast-forward local `main` to this branch

Because Step 2 merged `main` into this branch, `main → HEAD` is now a pure fast-forward.

- Run `git log main..HEAD --oneline`. **If empty**: nothing to land — report "main already contains this branch." and exit.
- Run `git worktree list --porcelain` and find the line `branch refs/heads/main` — record the `worktree <path>` above it.
- **If `main` is checked out in a worktree** (normally the repo root): run `git -C <that-path> merge --ff-only <this-branch>`.
  - Fast-forward merges only touch files that actually changed; if that worktree has unrelated dirty files it will usually succeed, but if git refuses (conflict with uncommitted work in the main worktree), **STOP** and report. Do NOT force, stash, or discard anything in that worktree.
- **If `main` is not checked out anywhere** (headless): run `git update-ref refs/heads/main HEAD`. Safe — no working tree affected.

## Final summary

After a successful run, report:
- Commit hash created (if any) and its one-line title.
- Whether Step 2 was a fast-forward, a no-op, or produced a merge commit.
- Anything left deliberately unstaged in Step 1 (build artifacts, suspicious files) and why.
- Build-gate result per target — ddraw.dll, tagpu.dll — passed, skipped (with the reason), or what failed.
- **Each gate's verdict**: docs — already done / done now (what was added) / not needed;
  review — already recorded on `<sha>` / run now on Opus at `<effort>` (`n` findings, `k` acted
  on, `m` rejected and why) / skipped and why.
- The range `main` advanced by in Step 6 (e.g., `50b2272..a1b2c3d`), and where it was updated (worktree path or headless ref).

Keep the final report under 15 lines.
