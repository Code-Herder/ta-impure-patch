---
allowed-tools: Bash(git:*), Bash(bash:*), Bash(gh:*), Bash(*/_local/*:*), Read
description: Publish local main to GitHub — verify the identity and transport setup, run the FULL content scan (every revision, every worktree, every blob against the original-game manifest), dry-run, then push main and the review notes. Landing (/git_commit_merge_wt) never pushes; this is the only command that does.
---

# Git Publish — the push, with its gates checked

Landing (`/git_commit_merge_wt`) ends at the local `main` fast-forward. This command is the
separate, deliberate act of publishing it, and it is the only thing in this repository that
pushes. What it checks is spelled out in `CLAUDE.md` *Publishing* and in `CLAUDE.local.md`;
the command exists so that none of it can be forgotten.

Everything runs against the **main checkout** — `_local/` and `.githooks/` exist only there,
never in a worktree:

```
MAIN="$(cd "$(git rev-parse --git-common-dir)/.." && pwd)"
```

Every command below is `git -C "$MAIN" …` or `"$MAIN/_local/…"`. If any step fails **STOP** and
report the state. Never `--force`, never `--no-verify`, never rewrite history to get past a
finding: anything already pushed is permanent.

## Step 1 — Preconditions

- `git -C "$MAIN" status --porcelain` is empty and `git -C "$MAIN" branch --show-current` is
  `main`. Otherwise STOP: the main checkout has uncommitted work or is on another branch.
- `[ -f "$MAIN/_local/CLAUDE.local.md" ]`. If not, STOP: this checkout does not carry the
  publishing rules and nothing may be pushed from it.
- Read `$MAIN/_local/CLAUDE.local.md` — the *Before every publish* section — even if the
  session already loaded it.

## Step 2 — Setup verification

```
bash "$MAIN/_local/verify-setup.sh"
```

Must end `0 failed`. It checks the pinned identity, the HTTPS remote under the right account,
that `gh` is logged in as that account, the absolute hooks path, that `_local/` is untracked
and `.publish-allow` is tracked, and that the original-game manifest is present.

## Step 3 — Full content scan

```
"$MAIN/_local/sanitize-scan.sh"
```

No `--quick`: the history surface is the point of this pass. Exit 0 is clean. On findings:
fix the content, commit it as a new commit, land it through `/git_commit_merge_wt`, and start
this command again. A finding in history that has **not** been pushed yet is handled by
`CLAUDE.local.md` *Rewriting local history*; one that **has** been pushed is permanent — say so
plainly and fix forward.

## Step 4 — What is about to leave

```
git -C "$MAIN" fetch origin main
git -C "$MAIN" log --oneline origin/main..main
git -C "$MAIN" log --format='%h %an <%ae> | %cn <%ce>' origin/main..main \
  | grep -v 'Code-Herder <5179440+Code-Herder@users.noreply.github.com> | Code-Herder <5179440+Code-Herder@users.noreply.github.com>'
git -C "$MAIN" push --dry-run origin main
```

The identity line must print nothing. On the very first publish there is no `origin/main` yet:
use `main` alone as the range. Record the commit count and the range for the summary.

## Step 5 — Push

```
git -C "$MAIN" push origin main
git -C "$MAIN" push origin refs/notes/commits
```

The pre-push hook re-checks the remote and every commit's identity; a refusal is a STOP, not
something to route around. The notes ref carries the `landing-review:` records, which are part
of the repository's history and contain nothing else.

## Summary

- Setup check: passed, or what failed.
- Scan: clean, or the findings and what was done about them.
- The range pushed (`<old>..<new>`, n commits) and whether the notes ref went with it.

Under 10 lines.
