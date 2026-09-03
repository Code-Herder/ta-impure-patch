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
- **The documentation pass is part of the landing**, not a follow-up — see below.

## Review engine changes before they land

Once per landing (not per commit), on the accumulated branch diff, run **`/code-review medium`**
when the landing touches **`tagpu/ddraw/**`**, **`tagpu/src/**`** or **`tools/tacli`** — `high`
if it writes engine or user state, adds or moves a byte patch, or is sim-adjacent. Verify each
finding against the code before acting on it, and never use `--fix`.

Skip it for docs, scenarios, comments, or a few lines with no new state, no new engine patch and
no new GL object — a review costs ~100k tokens and is not worth that for a typo. Batching
landings is what keeps this cheap: three commits landed together cost one review, not three.

**Why:** this stack patches a 1997 binary at absolute addresses, so mistakes are silent — they do
not throw, they render slightly wrong or corrupt state days later. The reviews have paid for
themselves every time: 6/6 real findings on the G13d diff, on G13e two HIGH findings that were
both real bugs about to ship (a dropped button *release* that left the engine holding the button
for the rest of the session, and `ScrollSpeed` being written back to the player's registry where
it would compound across launches), and on G13g a HIGH that was a fog-grid rebuild firing every
frame after any zoom-out from a map edge.

## Document what the landing learned — before the review, not after

Same trigger as the review, once per landing: if it touched engine code, the documentation pass
lands with it.

- **The engine map — `research/notes/exe-reverse-engineering.md` — gets every address the work
  touched *or merely read*,** not only the ones we patched: what it is, its call sites, the
  fields it owns, and how each fact was established (disassembly, or a live measurement with
  the numbers). **Negative results count** — a function with no callers, one loop that
  bounds-tests and one that does not. Update it as fully as the work allows; these addresses
  are the expensive part of this project and get re-derived every time they are not written down.
- **Then the module docs**: `gpu-status.md`'s hook map and its *fields we write* table,
  `roadmap.md`'s gate row and entry, and the `ta-*` skill if how you drive or measure the game
  changed.
- **Correct what the work proved wrong.** A stale note is worse than no note.
- **State the gaps the landing did not close** instead of writing as though it closed them.
- Mark inferred names `[INFERRED]`; check every claim against the source, never from memory.
- **Regenerate the wiki and look at what you wrote.** A note that does not render is not
  documentation either, and the notes are the wiki's source:

  ```
  .venv-undither/bin/python research/build_wiki.py     # from the main checkout OR any worktree
  ```

  It must end `built N pages + index`; then grep the generated `research/site/*.html` for the
  section you touched. `research/site/` is gitignored, so the only cost is the run.

**Commit the docs BEFORE running `/code-review`,** so the claims are in the diff it reads — the
reviewer disassembles, and it earns this: on the G13g landing it caught a documentation overclaim
(the scroll target is *not* "a copy taken after every clamp call") that three files repeated.
A docs-only landing still skips the review; docs riding with code do not.

**Why:** a wrong line costs more than a missing one. G13g's own probe went astray for several
rounds against a skill note claiming edge scroll "does not fire under injected input" — it does,
on the exact edge pixel — and the session's most reused new page was the functions we only
*read*: the camera stepper, the scroll poll, a clamp with no callers at all.

Full conditions and rationale: `.claude/commands/git_commit_merge_wt.md` **Step 0**, which is
where all three rules are enforced. These lines exist so they still apply when landing by hand.

## Python tooling: install what you need, into the shared venv

**`.venv-undither/` at the main checkout root is the project's Python environment**, and you may
`pip install` into it whenever a tool needs a package — that is what it is for. It already carries
`markdown` (the wiki builder) and the undither inference stack (torch/onnx, which
`build_wiki.py` shells out to for the learned bakes). `.gitignore` has `.venv*/`, so it is local
state and nothing you install reaches the repo.

**One venv, at the main checkout, shared by every worktree** — do not make a per-worktree copy.
`build_wiki.py:find_python()` already resolves it through `git rev-parse --git-common-dir` for
exactly this reason, and duplicating a CUDA torch install per ephemeral worktree is pure waste.

The system `python3` is PEP 668 externally-managed, so it will refuse installs and has none of
this — **a bare `python3 research/build_wiki.py` fails at `import markdown`**. Name the venv's
interpreter, as above.
