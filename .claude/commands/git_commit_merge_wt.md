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

## Step 0 — Review before you commit (engine / DLL changes only)

**Run `/code-review medium` on the working tree BEFORE Step 1**, when the change touches:

- `tagpu/ddraw/src/**` or `tagpu/ddraw/inc/**` (the fork and our modules), or
- `tagpu/src/**` (tagpu.dll), or
- `tools/tacli` (it drives every session; a bug here costs hours).

**Skip it** — and say so in the final summary — when the diff is only docs
(`research/notes/**`, `*.md`), scenarios (`scenarios/*.json`), comments, or a handful of lines
with no new state, no new engine patch and no new GL object. A review costs roughly 100k tokens;
it is worth that for a real change and not for a typo.

**Use `high` instead of `medium`** when the change writes engine or user state, adds or moves a
byte patch, or touches anything sim-adjacent. That is the class that ships silently: the review
that caught `ScrollSpeed` being persisted into the player's registry — where it would have
compounded across launches — was exactly this case.

**Before the commit, not after it**, so the fixes fold into the same commit instead of becoming
follow-ups.

**Verify every finding against the code (and the decompile) before acting on it.** The hit rate
on this codebase is high but not perfect — on the G13e diff, 2 of 11 findings were HIGH and real
(a dropped button *release* left the engine and the shield's virtual key state holding the button
forever; `ScrollSpeed` corrupting the registry), several MEDIUMs were real, and about a third
were overstated or described existing intentional behaviour as a bug. Fix what is real, say what
you rejected and why. **Do not use `--fix`** — it applies the wrong ones too.

Re-run the review only if the fixes were themselves substantial. One review per landing, not per
commit.

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
