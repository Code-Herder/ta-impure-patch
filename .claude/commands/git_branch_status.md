---
allowed-tools: Bash(git:*)
description: Check uncommitted changes in a local worktree or branch
---

# Git Branch Status

Argument (optional branch or worktree name): `$ARGUMENTS`

## Instructions

1. **Resolve the target worktree directory:**
   - If the argument is empty, use the current working directory.
   - Otherwise, run `git worktree list` and find the worktree matching the argument (match against both the branch name and path).
   - If no matching worktree is found, say "No worktree found for: <argument>" and stop.

2. **Get all uncommitted changes** by running:
   ```
   git -C <resolved_path> status --porcelain
   ```
   This covers untracked, modified, deleted, staged, and renamed files — everything not excluded by `.gitignore`.

3. **If no output**, respond with: **No changes detected.**

4. **If there are changes**, list up to 20 files grouped by category:
   - **Staged** (lines starting with `A`, `M`, `D`, `R` in the first column)
   - **Modified** (` M` — second column M)
   - **Deleted** (` D` — second column D)
   - **Untracked** (`??`)
   - **Conflicts** (`UU`, `AA`, `DD`, etc.)

   Format as a concise summary with the worktree branch name as a header.

5. **Branch vs main comparison** (skip this step if the branch IS main):
   - Run `git log main..<branch> --oneline` to find commits on this branch not in main (ahead).
   - Run `git log <branch>..main --oneline` to find commits on main not in this branch (behind).
   - Run `git diff main...<branch> --stat` to summarize file-level differences.
   - Report:
     - **Ahead of main by N commits** (if any), with a one-line list of those commits
     - **Behind main by N commits** (if any)
     - **Diverged files**: short `--stat` summary of what's different
   - If the branch has no commits ahead and no commits behind, say: **In sync with main.**
