---
allowed-tools: Bash(git:*), Bash(make:*)
description: Commit worktree changes, merge main into this branch, run the build gate, and fast-forward local main to include this branch
---

# Git Commit + Merge Worktree

Argument (optional commit message): `$ARGUMENTS`

Chains together the common "ship this worktree" flow:

1. Commit any local changes in the current worktree.
2. Merge `main` into this branch (fast-forward, or a merge commit if diverged).
3. Run the build gate.
4. Fast-forward local `main` to this branch. (This repo has no remote — "shipping" means landing on the local `main` ref. If a remote is ever added, revisit this flow.)

If any step produces an unexpected state (conflicts, non-fast-forward, detached HEAD, failing builds, etc.) **STOP** and report the state. Never use `--force`, `--no-verify`, `reset --hard`, or rewrite history.

## Step 0 — Is this ready to LAND? (the documentation pass, and the review that decides)

**Committing and landing are different acts.** Commits on the worktree branch are cheap
checkpoints — make as many as are useful, including work in progress. **Landing** is
fast-forwarding local `main` (Step 4), and that is the act with a bar. Historically this repo has
run at roughly one landing per 1.7 commits, i.e. almost every commit went straight to `main`;
that is what makes the review below expensive and what lets half-checked work reach `main`.

**Land one FINISHED unit of work, not one commit.** A gate, a fix, a documentation pass. If the
next thing you do would be "…and now correct what I just landed", it was not finished.

### The bar — all of these, before Step 4

1. **It does what it claims**, verified by running it, not by reading it. Both DLLs compiling is
   Step 3's gate, not evidence the change works.
2. **Its claims are checked.** This bites documentation hardest: a build gate cannot catch a
   wrong statement. Do not land a page and then discover a claim in it was too rosy — check the
   claims against the source *first*.
3. **Nothing is left half-done behind it** — no debug instrumentation, no counters added to
   chase a bug, no `.on` file the change depends on but does not create.
4. **The documentation it taught us is written down** — the pass below, in the same landing.

### The documentation pass

**Same trigger as the review, once per landing.** If the landing touches engine code, its
documentation is part of the finished unit of work — not a follow-up someone gets to later.
Do it *before* the review (see why below).

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

**Commit the docs BEFORE running `/code-review`.** The reviewer reads the accumulated branch
diff and it disassembles, so the claims get fact-checked for free — on the G13g landing it
returned a MEDIUM against a documentation overclaim ("the second eye pair is a copy taken right
after every clamp call, so it follows too") that the header comment, `gpu-status.md` and
`roadmap.md` all repeated, and it was wrong: three sites clamp that pair inline and never call
the patched function. The "skip the review for docs-only landings" rule below is unchanged —
docs *riding with code* are simply in the diff.

**Why this is a step and not a nicety:** the addresses are the expensive part of this project.
They get re-derived every time they are not written down, and a *wrong* line costs more than a
missing one — G13g spent several probes chasing a skill note that claimed edge scroll "does not
fire under injected input" (it does; the trigger is an exact equality on the outermost pixel).
The most reused output of that landing was the page for functions we only *read* — the camera
stepper `0x41CA30`, the scroll poll `0x41CF10`, the dead clamp `0x41C450`.

### The review

**Run `/code-review medium` on the accumulated branch diff (`main...HEAD` plus anything still
uncommitted) once per landing** — not once per commit — when the landing touches:

- `tagpu/ddraw/src/**` or `tagpu/ddraw/inc/**` (the fork and our modules), or
- `tagpu/src/**` (tagpu.dll), or
- `tools/tacli` (it drives every session; a bug here costs hours).

**Skip it** — and say so in the final summary — when the landing is only docs
(`research/notes/**`, `*.md`), scenarios (`scenarios/*.json`), comments, or a handful of lines
with no new state, no new engine patch and no new GL object. A review costs roughly 100k tokens;
it is worth that for a real change and not for a typo. Batching landings is what keeps this
cheap: three commits that land together cost one review, not three.

**Use `high` instead of `medium`** when the landing writes engine or user state, adds or moves a
byte patch, or touches anything sim-adjacent. That is the class that ships silently: the review
that caught `ScrollSpeed` being persisted into the player's registry — where it would have
compounded across launches — was exactly this case.

Findings become another commit on the branch before Step 4; that is what the branch is for.

**Verify every finding against the code (and the decompile) before acting on it.** The hit rate
on this codebase is high but not perfect — on the G13e diff, 2 of 11 findings were HIGH and real
(a dropped button *release* left the engine and the shield's virtual key state holding the button
forever; `ScrollSpeed` corrupting the registry), several MEDIUMs were real, and about a third
were overstated or described existing intentional behaviour as a bug. Fix what is real, say what
you rejected and why. **Do not use `--fix`** — it applies the wrong ones too.

Re-run the review only if the fixes were themselves substantial.

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

## Step 3 — Run the build gate

Always run the gate on the post-commit, post-merge tree so a broken tree never reaches `main`. There is no test suite; the gate is that both DLLs compile cleanly.

- **Skip entirely only if nothing changed**: if Step 1 committed nothing AND Step 2 merged nothing, there is nothing new to gate — skip to Step 4.
- **Per-target skip (optimization, not a shortcut):** a target may be skipped only when the branch changes **zero** files it covers — check `git diff --name-only main...HEAD` (before the Step 2 merge moved things) or the combined commit range against `tagpu/ddraw/**` (ddraw.dll) and `tagpu/src/**` + `tagpu/ddraw/inc/tagpu.h` (tagpu.dll). Call out any skip in the summary.

Run each build in the foreground, sequentially:

- **ddraw.dll** — `make -C tagpu/ddraw`
- **tagpu.dll** — `make -C tagpu`

Treat new warnings from changed files as worth reporting even when the build succeeds.

**On failure — STOP before touching `main`.** Investigate, fix, commit a **NEW** commit (never `--amend`/`--no-verify`), and re-run the gate.

## Step 4 — Fast-forward local `main` to this branch

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
- The range `main` advanced by in Step 4 (e.g., `50b2272..a1b2c3d`), and where it was updated (worktree path or headless ref).

Keep the final report under 12 lines.
