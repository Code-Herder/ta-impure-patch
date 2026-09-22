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

## Fixes must be safe by construction, never by timing

**A fix that makes a fault rarer is not a fix.** Every correctness fix here — crash
fixes above all, but any fix — must rest on an invariant that holds by design. If the
argument for it contains "the window is small", "the other thread has almost always
finished by then", or "we check first", it is not the fix; it is the bug with better
odds. The failures this stack produces are silent, so a race that fires once a week
reads as a mystery, not as the change that caused it.

Concretely, the fix must be one of:

- **A bound.** A value read from engine memory is DATA until it has been validated as
  data — an index checked against the engine's own count, a length against the
  allocation, a tag against the enum. `tagpu_native.c`'s `model_root` is the shape:
  the unit's `ModelId` is bounded by `UNITINFOCount` (the count `0x42DB90` itself
  loops to), so an index taken from a recycled unit slot costs one wrong frame and
  cannot address memory the engine does not own.
- **A lifetime.** Cooperate with the engine's own destructor so the memory cannot go
  away while we read it — `tagpu_reclaim`'s deferred reclamation, the whole of
  `research/notes/thread-safe-destruction.md`. Its Mode A / Mode B classification is
  how you decide which objects need it: Mode B (fixed arrays, arenas, pools) is safe
  to read stale **only because** every value taken out of one is then bounded.
- **An ordering.** A handshake or a fence that makes the state a fact rather than a
  hope, as the pass counters do.

**What does not count.** `IsBadReadPtr` (and any probe of the same shape) answers a
question about the past: the page can be unmapped between the check and the read, and
the check itself can swallow a guard page. A range test like `ptr_ok` is a cheap
sanity filter on a VALUE and is fine as one — it is never the safety argument. Neither
is a timeout, a retry, a sleep, or "the render thread will have finished by then".

**If there is genuinely no by-design fix available, stop and ask.** Say what the
invariant would have to be, why it cannot be established, and what the timing-dependent
alternative buys — then let the human decide. A timing-dependent mitigation that the
human has approved is documented as one, in the code and in the note, with the residual
hole named. Silently shipping one is the failure this section exists to prevent.

**This is a standing debt, not just a rule for new work.** `IsBadReadPtr` is used
widely in the older passes as though it were a guarantee. Those are not all wrong — many
sit behind a bound already — but none of them should be cited as the reason a read is
safe, and any of them touched by new work gets the argument above or a note saying why
it cannot.

## Land it yourself — what actually requires a human

**The default is to keep going.** A landing is meant to be driven to `main` without a human in
the loop, and most of what goes wrong on the way is ordinary work: a build that breaks on your
own change, a review finding, a documentation claim the reviewer disproved, a merge conflict
whose two sides you can read. Fix it, commit the fix as a **new** commit, re-run the gate that
failed, and carry on. None of those is an escalation, and stopping to report one costs the human
a round trip to say "yes, fix it".

**Bounded, not infinite.** Give a failing gate **three** fix-and-re-run attempts. If the third
fails, or if two consecutive attempts fail the same way with no new information, stop — that is
no longer iteration, it is thrashing, and the next attempt will not be the one that works.
Report what was tried and what the failure actually says.

**Escalate only for these.** The list is closed; anything not on it, you handle:

1. **A decision that changes what the landing is.** The work or the review shows the *plan* is
   wrong — not the code. Rewriting the goal is the human's call, not a fix.
2. **No by-design fix is available** and only a timing-dependent mitigation is left. This is
   already the rule above (*Fixes must be safe by construction*): say what the invariant would
   have to be, why it cannot be established, and what the mitigation buys. Never ship one
   silently.
3. **The verification cannot be run at all.** Not "it failed" — failed is a bug to fix. This is
   the game refusing to launch, a missing tool, hardware. A landing whose claim was never
   actually run does not meet the bar in the first place, so it cannot proceed past this.
4. **Credentials or secrets** anywhere in the change set.
5. **Someone else's in-progress state**: a conflict that was already in the tree, uncommitted
   work in the main checkout blocking the fast-forward, a ticket another agent owns. Never
   force, stash, or discard it.
6. **Anything outward-facing or irreversible beyond the local landing** — publishing above all.
   `/git_publish` is always the human's, and a landing never implies one.

**Say which one.** An escalation names the numbered reason and what would unblock it. "It didn't
work" is not an escalation, it is a status update.

## Review engine changes before they land

Once per landing (not per commit), on the accumulated branch diff, review at
**`medium`** when the landing touches **`tagpu/ddraw/**`**, **`tagpu/src/**`** or
**`tools/tacli`** — `high` if it writes engine or user state, adds or moves a byte patch, is
sim-adjacent, **or changes the synchronisation between the game thread and the render thread**.

That last one is not covered by the others and is the reason this line was amended. A change to a
lock, a handshake, a published counter, or the lifetime of anything one thread writes and the other
reads is the class this stack fails at silently: it does not throw, it produces a dead render
thread or a wrong frame days later, on one launch in twenty, and the reproduction is a story about
timing rather than a test. `tagpu_reclaim` exists because of one; `tagpu_fog_at`'s guard exists
because of another whose root cause was never found; G13s was a producer gated on a value the
other thread published. A diff that adds three lines to a critical section is small in the
reviewer's sense and large in this one. Verify each finding against the code before acting on it, and never apply findings
blindly.

**Reviews run in a dedicated, read-only review context, not as the implementation agent grading
its own continuation.** Use the current harness's native review mechanism when it has one. In
Codex, that is `/review` against `main`, or `codex review --base main` for a non-interactive run;
in a harness without a dedicated reviewer, launch a fresh read-only agent. Do not pin this gate
to one vendor or model. Give the reviewer the brief in
`.claude/commands/git_commit_merge_wt.md` **Step 5**: the worktree path, the range
`main...HEAD`, what the change does in engine terms, the binary and the `objdump` command to
verify addresses against, the risky spots, and the report format. Record it afterwards as a
`landing-review:` git note on the reviewed commit — that note is how the landing command knows
the gate was met.

Skip it for docs, scenarios, comments, or a few lines with no new state, no new engine patch and
no new GL object — a review costs ~100k tokens and is not worth that for a typo. Batching
landings is what keeps this cheap: three commits landed together cost one review, not three.

**Why the review itself:** this stack patches a 1997 binary at absolute addresses, so mistakes are
silent — they do not throw, they render slightly wrong or corrupt state days later. The reviews
have paid for themselves every time: 6/6 real findings on the G13d diff, on G13e two HIGH findings
that were both real bugs about to ship (a dropped button *release* that left the engine holding
the button for the rest of the session, and `ScrollSpeed` being written back to the player's
registry where it would compound across launches), and on G13g a HIGH that was a fog-grid rebuild
firing every frame after any zoom-out from a map edge.

## Document what the landing learned — before the review, not after

Once per landing, on the same diff the review reads: if it touched engine code, the documentation
pass lands with it.

- **The engine map — `research/notes/exe-reverse-engineering.md` — gets every address the work
  touched *or merely read*,** not only the ones we patched: what it is, its call sites, the
  fields it owns, and how each fact was established (disassembly, or a live measurement with
  the numbers). **Negative results count** — a function with no callers, one loop that
  bounds-tests and one that does not. Update it as fully as the work allows; these addresses
  are the expensive part of this project and get re-derived every time they are not written down.
- **Then the module docs**: `gpu-status.md`'s hook map and its *fields we write* table,
  `roadmap.md`'s gate row and entry, and the `ta-*` skill if how you drive or measure the game
  changed — by that skill's own *Maintaining this skill* rules: state the present, replace
  rather than annotate, and keep the landing's story in the note.
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

**Commit the docs BEFORE running the review,** so the claims are in the diff it reads — the
reviewer disassembles, and it earns this: on the G13g landing it caught a documentation overclaim
(the scroll target is *not* "a copy taken after every clamp call") that three files repeated.
A docs-only landing still skips the review; docs riding with code do not.

**Why:** a wrong line costs more than a missing one. G13g's own probe went astray for several
rounds against a skill note claiming edge scroll "does not fire under injected input" — it does,
on the exact edge pixel — and the session's most reused new page was the functions we only
*read*: the camera stepper, the scroll poll, a clamp with no callers at all.

Full conditions and rationale: `.claude/commands/git_commit_merge_wt.md`, which **checks each
gate and runs whichever is missing** — commit, merge main in, build, the documentation pass
(Step 4), the dedicated review (Step 5), then the fast-forward. These lines exist so the rules
still apply when landing by hand.

## Build the gate in parallel

`make -C tagpu/ddraw -j$(nproc)` — **43.6 s serial, 4.8 s parallel** on the reference setup
(measured 2026-09-17). Use it; the serial form is a habit, not a requirement.

It is parallel-safe by construction rather than by luck: `inc/git.h` is written at **parse**
time by `$(shell …)` and not by a recipe, `thread-split-check.sh` and `spirv-check.sh` are
**order-only** prerequisites of the link (`$(TARGET): $(OBJS) | thread-split spirv`), and no two
targets share an intermediate. CI has been building this way since the workflow was added
(`.github/workflows/build.yml`).

**Byte-identity is not the test, and do not reach for it as one.** Two *serial* builds of the
same tree already differ (a timestamp reaches the PE header), so a differing md5 says nothing
about `-j`. The assurances are the two above.

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

## Publishing

This repository is public on GitHub as `Code-Herder/ta-impure-patch`. Everything in it is
committed as **`Code-Herder <5179440+Code-Herder@users.noreply.github.com>`** and pushed over
**HTTPS through the `gh` credential helper**, never over SSH. Both facts are pinned by
per-machine hooks rather than assumed:

- `pre-commit` refuses a commit whose `user.name`/`user.email` differ from the pin; anything
  staged under `_local/`; anything `.gitignore` refuses (it asks `git check-ignore`, so every
  ignore rule is enforced for free); any file over 8 MB; any blob byte-identical to a file of
  the original game; and any binary whose path matches no glob in `.publish-allow`.
- `pre-push` refuses any remote that is not this repository's HTTPS URL, re-checks the author
  and committer of every commit in the range, refuses if any `_local/` path is tracked, and then
  runs the full content scan — every revision, every worktree, every blob against the manifest —
  so a plain `git push` cannot skip it.

The hooks live in `.githooks/` (untracked, per machine), enabled through an **absolute**
`core.hooksPath` so that every worktree runs them. A hook failure is a real problem with the
change: fix it and commit again, never `--no-verify`.

### Nothing from the original game, ever

`.gitignore` bans the game's file formats and its archives; that is the outer layer. The inner
one is content: `_local/original-manifest.tsv` holds the hash of every file of the retail
install, including everything inside its archives, and both the hook and the publish scan refuse
a match whatever the file is called. Derived work — screenshots of our renderer, figures we
generated, models we built or trained — is allowed only **by class**, listed in
`.publish-allow`. Adding a class is a one-line diff a review can see; a binary outside the list
does not commit.

### Landing is local; publishing is `/git_publish`

`/git_commit_merge_wt` ends at the local `main` fast-forward and never pushes. `/git_publish` is
the separate act: it runs the setup check, then the full content scan (every revision, every
worktree, every blob against the manifest), and pushes `main` only when both are clean. Anything
already pushed is permanent — fix forward.

### Releases: a `v*` tag, and notes written for players

Pushing a `v*` tag is what cuts a release — GitHub Actions builds `ddraw.dll`, packages it and
publishes the zip. The workflow writes only a one-line placeholder note, so the body is set
afterwards with `gh release edit <tag> --notes-file <file>`.

**Release notes are for the person downloading the zip, not a lab report.** Say what changed and
what it means for them, lead with anything that made the previous release not work, and stop
there. **Never carry the engineering apparatus into them** — no "what was measured", no
"not covered", no coverage caveats, no percentage tables, no methodology notes. All of that
belongs in `research/notes/` and in the commit messages, where the next person working on the
code will look for it; in a release note it reads as hedging and buries the one line a player
actually needs. Ask before choosing a version number that is not the obvious next one: a patch
release on top of `v0.2` is `v0.2.1`, and `v0.21` sorts *above* `v0.3` under semver.

### The local half: `CLAUDE.local.md`

The rules that name what must never be published — the strings, the paths, the scan and its
allow-list — live in `_local/CLAUDE.local.md`, reached through a gitignored symlink at the
repository root so that it loads together with this file. **Read it at the start of a session
and before every publish.** It loads in every worktree under `.claude/worktrees/` as well
(measured 2026-09-07: memory files are read from the working directory and every directory above
it, and the symlink is followed). If it is not present on your machine you do not have the
rules: work freely, commit freely, do not publish.

In tracked content, paths are repo-relative or go through a documented variable, and facts true
of one desktop only (display, hardware, what is installed) are attributed to *the reference
setup*, never to a named or implied host.
