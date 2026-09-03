# Project instructions

## Commit freely, land deliberately

Commits on a worktree branch are cheap checkpoints — make as many as are useful, including work
in progress. **Landing** — fast-forwarding local `main` — is the act with a bar:

- **Land one finished unit of work, not one commit.** If the next thing you would do is "…and
  now correct what I just landed", it was not finished. Batch the commits and land once.
- **Verified by running it**, not by reading it. Both DLLs compiling is not evidence the change
  works. For documentation, check the claims against the source *before* landing — no build gate
  catches a wrong statement.
- **Nothing half-done left behind** — no debug instrumentation, no counters added to chase a bug.

## Review engine changes before they land

Once per landing (not per commit), on the accumulated branch diff, run **`/code-review medium`**
when the landing touches **`tagpu/ddraw/**`**, **`tagpu/src/**`** or **`tools/tacli`** — `high`
if it writes engine or user state, adds or moves a byte patch, or is sim-adjacent. Verify each
finding against the code before acting on it, and never use `--fix`.

Skip it for docs, scenarios, comments, or a few lines with no new state, no new engine patch and
no new GL object — a review costs ~100k tokens and is not worth that for a typo. Batching
landings is what keeps this cheap: three commits landed together cost one review, not three.

Full conditions and rationale: `.claude/commands/git_commit_merge_wt.md` **Step 0**, which is
where both rules are enforced. These lines exist so they still apply when landing by hand.

**Why:** this stack patches a 1997 binary at absolute addresses, so mistakes are silent — they do
not throw, they render slightly wrong or corrupt state days later. The reviews have paid for
themselves every time: 6/6 real findings on the G13d diff, and on G13e two HIGH findings that
were both real bugs about to ship (a dropped button *release* that left the engine holding the
button for the rest of the session, and `ScrollSpeed` being written back to the player's registry
where it would compound across launches).
