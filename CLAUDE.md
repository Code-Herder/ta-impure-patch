# Project instructions

## Review engine changes before they land

Before committing a change that touches **`tagpu/ddraw/**`**, **`tagpu/src/**`** or
**`tools/tacli`**, run **`/code-review medium`** on the working tree (`high` if the change writes
engine or user state, adds or moves a byte patch, or is sim-adjacent). Verify each finding
against the code before acting on it, and never use `--fix`.

Skip the review for docs, scenarios, comments, or a few lines with no new state, no new engine
patch and no new GL object — a review costs ~100k tokens and is not worth that for a typo.

Full conditions and the rationale: `.claude/commands/git_commit_merge_wt.md` **Step 0**, which is
where the rule is enforced when landing on `main`. This line exists so it still applies when the
commit is made by hand.

**Why this rule exists:** this stack patches a 1997 binary at absolute addresses, so mistakes are
silent — they do not throw, they render slightly wrong or corrupt state days later. The reviews
have paid for themselves every time: 6/6 real findings on the G13d diff, and on G13e two HIGH
findings that were both real bugs about to ship (a dropped button *release* that left the engine
holding the button for the rest of the session, and `ScrollSpeed` being written back to the
player's registry where it would compound across launches).
