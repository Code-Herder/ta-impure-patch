---
name: work-landing
description: Own one landing of a grill-to-kanban Epic end to end — cut its child ticket, do the work in the Epic's worktree, keep the documentation current, attach evidence, land it through /git_commit_merge_wt, and close the ticket. Use when asked to work, start, resume or land a landing or gate of a TA Epic, e.g. "work TA-50's next landing", "/work-landing TA-50", "resume TA-63".
---

# work-landing

Take **one landing** of an Epic filed by `grill-to-kanban` from "a row in a table" to "on local
`main`, ticket closed, evidence attached".

A **landing** is this repository's unit (`CLAUDE.md`): one finished piece of work
fast-forwarded onto local `main`, carrying its own verification, documentation pass and review.
Not one commit — commits on the branch are cheap checkpoints.

Load **`agent-kanban`** alongside this file. It owns identity, the `kb` surface, comment kinds,
attachment categories and the completion rules; this file only says what is different when the
ticket is a landing.

## 0. Authorization — read this before the landing gate

**Invoking `/work-landing` authorizes this landing**: the dedicated review and the fast-forward
of local `main` for *this one landing*, and nothing beyond it. The skill calls
`/git_commit_merge_wt` itself (§8) without asking again.

**Run it to `main` without checking in.** Once invoked, this skill is expected to come back with
a landed gate, not with questions. Everything that goes wrong on the way is yours to fix (§9)
except the six escalation reasons in `CLAUDE.md`'s *Land it yourself — what actually requires a
human*. Do not stop at a gate to ask permission to do the obvious thing.

The authorization is **scoped and expiring**:

- **One landing per invocation.** When this landing closes, the authorization is spent. Naming
  the next landing in the report is not permission to start it.
- **It never authorizes publishing.** `/git_publish` is a separate act with the content scan in
  front of it, and it is always the human's.
- **It never authorizes closing the Epic.** That stays the human's call (`agent-kanban` §12).

## 1. Entry

`$ARGUMENTS` is one of:

| argument | meaning |
| --- | --- |
| `TA-50` (an Epic key) | cut and work the **next un-cut landing** of that Epic |
| `TA-50 3` | work **landing 3** of that Epic specifically |
| `TA-63` (a child key) | **resume** that existing child ticket |
| *nothing* | **refuse** — list the live Epics and stop |

**Refuse, with one line saying why, when:**

- No argument was given. Do not guess which work was meant.
- The key is an Epic whose description has no **Landings** table — it was not filed by
  `grill-to-kanban`, and there is no row to work. Say so; do not invent the decomposition.
- Every landing in the table is already cut and `done`. Report that and say the Epic's close is
  the human's call.
- The child ticket handed to you is `done`, `cancelled`, or owned by another agent (`kb t pick`
  returns 409). Surface it; never force-take.

## 2. Project, identity, and the landing row

1. **Project** is `ta_impure_patch`. Pass `--project ta_impure_patch` (or `--epic`/`<KEY>`)
   explicitly on every project-scoped command — `~/.kanban/config.toml`'s `default_project` is
   **not** this project, and inheriting it files the work on another board.
2. **Verify the checkout** is `Code-Herder/ta-impure-patch` before touching anything.
3. **Identify** per `agent-kanban` §1 — `kb identify` lazily, `export KB_TOKEN=…`, never
   `--save`, never the bootstrap token. `kb whoami` must not print the human's handle.
4. **Read the Epic**: `kb epic show <EPIC>` for the rollup and children, and
   `kb t show <EPIC>` for the description with its **Landings** table.
5. **Work out which landing is next.** Children bind to rows by the label `landing:<n>`:
   ```bash
   kb epic show <EPIC>                                  # children grouped by status
   kb t ls --project ta_impure_patch --label=landing:3   # is row 3 cut yet?
   ```
   `kb t ls` has **no `--epic` flag** (verified 2026-09-15: its filters are `--project`,
   `--status`, `--type`, `--assignee`, `--label`, `--q`, `--archived`, `--only-archived`) —
   `kb epic show` is how you list an Epic's children.
   The next landing is the **first row in table order** with no `landing:<n>` child — table
   order, not lowest integer, because a gate that split mid-flight carries sub-numbered rows
   (`3.1`, `3.2`, … — §7) that sort between `3` and `4` only by position. Bug children
   (§9) carry **no** `landing:` label precisely so they do not corrupt this count.

## 3. Cut the child ticket

```bash
kb t create ta_impure_patch --type=<inferred> --epic=<EPIC> \
  --labels=landing:<n> --title="<the landing's name, verbatim from the table>" -f desc.md
kb t pick <KEY>
```

- **Title is the row's own name**, unchanged. The `landing:<n>` label is the machine key, so a
  later retitle during a scope change cannot break the join. `<n>` is the row's `#` cell
  verbatim, sub-number included (`landing:3.2`), and it is never reassigned afterwards — §7.
- **Description** states the landing's goal in one or two sentences and then, on its own line,
  copies the row's exit condition verbatim:

  ```
  **Verified by:** <the thing you run, and what it must show>
  ```

  The ticket must carry its own exit criterion. A landing whose ticket does not say how it will
  be verified is a landing that will be declared finished by reading it.
- `--type` inferred per `agent-kanban` §3a (`feature` / `bug` / `task` / `chore` / `spike`).
  **Never `epic`.**

Then set the worktree fields (§4) and post the **cut and picked** comment (§5).

## 4. Worktree

The convention on disk is `.claude/worktrees/<slug>` with branch `worktree-<slug>`, one tree per
**topic**, reused across landings.

- **Invoked from the main checkout** → use the Epic's designated worktree. Read it from the
  Epic's `branch` / `worktree` fields; if it has none, create
  `.claude/worktrees/<slug>` + `worktree-<slug>` (slug from the Epic) and write it onto the
  Epic with `kb t set-branch <EPIC> --branch=… --worktree=…`.
- **Invoked from inside a worktree** → use that one. Record it on the child, and if it differs
  from the Epic's recorded tree, **say so in chat** and leave a `decision` comment. Landing in a
  tree the human did not expect is the failure this rule exists to prevent.

Never create a per-landing worktree. After a landing, `/git_commit_merge_wt` has merged `main`
in and fast-forwarded it, so the same tree is a clean base for the next row.

## 5. Comments — the fixed minimum

`agent-kanban` §6's judgement applies on top of these, but these four are **mandatory**, because
judgement is exactly what skips the boring structural ones:

1. **Cut and picked** — what this landing is, and the *Verified by* criterion it must meet.
2. **Verification passed** — the criterion actually run, and what it showed, with the evidence
   attached in the same breath (§6). Not "looks right": the numbers, or the frame.
3. **Every gate stop inside `/git_commit_merge_wt`** — build failure, documentation gap, or
   review findings: what stopped it, what was found, and what changed. Review findings are the
   important one. This repository's reviews land real bugs (6/6 on G13d; two HIGH on G13e that
   were both about to ship), and that trajectory is what a cold reader needs six weeks later.
4. **Landed** — the commit `main` was fast-forwarded to.

**Not a timer.** The ~60-minute rule stays `agent-kanban` §6's fallback, not this skill's
cadence. Do not paste logs into a comment — attach them as `--category=log` and cite them in one
prose sentence.

## 6. Evidence

**Attach as you produce it**, into `tmp/` first, never the repo root or `src/` (`agent-kanban`
§8). Screenshots at meaningful *visual* milestones — changed states, not a clock.

`ta-capture` covers the mechanics (`tacli shot`, `tacli glshot`, ffmpeg `x11grab -window_id`);
`ta-drive` launches and drives the instance.

### The closing evidence

**If the landing has a visible on-screen result**, record **one coherent demonstration from
application startup, through loading and the relevant animation, to the finished feature in
action** — self-contained, so a viewer with no context can follow it.

- **Verify the clip decodes end to end** before attaching. A truncated capture nobody can play
  is worse than no evidence, because the ticket claims it has some.
- **Encode to mp4/h264**, not x11grab's raw mkv, so it plays in the board's lightbox instead of
  downloading as a dead file.
- Attach as `--category=testing-evidence` with a caption naming the scenario and the criterion
  it shows.
- Load `ta-video-montage` **only** if the evidence genuinely needs cuts, captions, or several
  clips. Not by default.

**If the landing has no visible runtime behaviour** — a thread-safety fix, a documentation pass,
a build change — do **not** film it. Attach the closest useful evidence (the measurement, the
log, the rendered page) and add one line saying why a video would prove nothing.

## 7. When the shape changes — record the split, don't re-plan

A gate is filed as one row and usually stays one. But the work is how you find out what a gate is
made of, and a landing sometimes shows that the row in front of you is really several: parts with
their own seam, their own thing to run, their own result. **When that happens, say so where the
plan lives — in the landing that revealed it, not in a tidy-up afterwards.** A row that still
reads as one undivided thing, three landings in, is out of date in the way that costs the most:
the next reader takes it for nearly done.

Two surfaces, and they have to agree:

1. **`research/notes/roadmap.md`'s gate row** — `landing N of M`, one sentence per part, and a
   closing *"Not covered by landings 1–N"* naming what is still open, **the gate's own exit
   condition included** (a part's bar is not the gate's bar). This rides in the §8 documentation
   pass — `/git_commit_merge_wt` Step 4 states the same rule and checks for it, so skipping it
   here only moves the work to the gate.
2. **The Epic's Landings table** — `kb t edit <EPIC> -f desc.md`, so a board reader and a roadmap
   reader see the same plan, followed by a `decision` comment on the Epic saying what the work
   showed and why the row became several. Without this half the board keeps counting the Epic
   against a landing list that no longer exists.

**Never renumber a row that already has a child.** The `landing:<n>` label is the join (§2), and
renumbering desynchronises every child already open — the same reason `grill-to-kanban` refuses
to rewrite an Epic's table wholesale. Split **in place** instead: row `n` becomes `n.1`, `n.2`, …
with the work already done as `n.1`, rows after it keep their numbers, and the label follows the
cell verbatim (`landing:3.2`). §2's "next landing" then means the first row in **table order**
with no child, not the lowest integer.

**The split does not extend this invocation's authorization** (§0). When the row that split is
the one you are working, it becomes `n.1` and you finish that; the parts you have just named are
in the table for the next invocation, not started in this one.

### The bar for a part — the guard against over-splitting

A part is a landing, so it meets a landing's bar: **something you can run that shows a result**,
with its own *Verified by* line in its row. Beyond that:

- **Split along a seam the work already has** — a pass, an op, a layer, a surface, a thread
  hand-over. Not along a calendar, and not along "what I finished today".
- **Never open a part for a fix, a review round, a re-measurement or a documentation pass.**
  Those belong to the landing that produced them; that is what the landing's own gates are for.
  G19f landing 2 absorbed six review findings, a re-review that disproved two of its own fixes,
  and a re-measurement after each round, and it is still *one* landing.
- **The calibration is the one already in the tree, and it is about right.** G19e ran one part
  per world pass — six for the whole of it; G19f is six for the UI layer and the present. A gate
  that comes out as ten-plus parts has been sliced rather than split. A gate that finishes in one
  or two landings is not split at all, and writing "landing 1 of 2" on it buys nothing.
- **`M` is allowed to move.** G19f was written up as "landing 1 of 5" when the UI op stream turned
  out to hand over in pieces, then "landings 1–2 of 6" one landing later, when the string op
  showed that landing 1 alone composited nothing in real play. A count that grew is the plan
  catching up with the work. Do not keep a wrong number to look decisive, and do not re-split a
  gate every landing to keep the shape tidy.

**This records what the work showed; it does not re-plan the Epic.** Splitting a row into the
parts the work turned out to have needs no turn. If the *exit condition* has to change — the gate
is asking for the wrong thing, or the goal moved — stop: that is **escalation reason 1**, and the
plan is the human's.

## 8. The landing

**Do the documentation pass yourself, and commit it, before invoking the landing command.**
`CLAUDE.md` requires the docs to be in the diff the review reads, and this skill is the only
party that knows what the work *learned* — an address it merely read, a function with no callers,
a note the work proved wrong. `/git_commit_merge_wt` Step 4 reconstructing that from a diff is
strictly worse; it will then find the gate already met and cost nothing.

In order:

1. `research/notes/exe-reverse-engineering.md` — every address the work **touched or merely
   read**: what it is, its call sites, the fields it owns, and how each fact was established.
   **Negative results count.** Mark inferred names `[INFERRED]`. Check every claim against the
   source, never from memory.
2. The module docs — `gpu-status.md`'s hook map and *fields we write* table, `roadmap.md`'s gate
   row, and the `ta-*` skills if how you drive or measure the game changed. Correct what the
   work proved wrong; state the gaps it did **not** close. If this landing showed the gate to be
   several, the row carries the split and the Epic's table matches it — §7, both surfaces.
3. Regenerate and look at it:
   ```bash
   .venv-undither/bin/python research/build_wiki.py
   ```
   It must end `built N pages + index`. Then grep `research/site/*.html` for the section you
   touched. A note that does not render is not documentation.
4. Commit the docs.

Then:

```bash
kb t review <KEY>          # the board shows "at the gate", not a stale in_progress
```

and invoke **`/git_commit_merge_wt`**. If the harness cannot invoke repository commands, read
`.claude/commands/git_commit_merge_wt.md` and execute it exactly.

**Do not duplicate or weaken its gates here.** It owns the commit, the merge, the build, the
documentation check, the review and its findings, and the fast-forward. This skill's job at a
gate is to record the outcome (§5.3), not to re-implement the gate. Never `--force`,
`--no-verify`, or `reset --hard`.

## 9. Trouble — the default is to keep going

`CLAUDE.md`'s *Land it yourself — what actually requires a human* governs. **Drive the landing to
`main`.** Things going wrong on the way is the normal shape of this work, not a reason to hand it
back: a build you broke, a review finding, a wrong docs claim, a test that fails, a conflict you
can read — fix it, commit the fix, re-run the gate, carry on. Three fix-and-re-run attempts per
gate, and stop if the third fails or two in a row fail identically.

| situation | what to do |
| --- | --- |
| **In-scope failure** — own build break, review findings on this diff, a docs claim the reviewer caught, the verification failing | ordinary work. The landing command owns the fix loop; you comment on what was found and what changed (§5.3). **Not** a block. Verify each finding against the code before acting on it; never apply findings blindly. |
| **Out-of-scope defect found mid-work** | file one concise `bug` child under the same Epic with a repro or evidence, **no `landing:` label**, link it from a comment on the current ticket, and carry on. Do not derail this landing; do not swallow it either. |
| **One of the six escalation reasons** | `kb t block <KEY> -m "Blocked on: <reason N — cause>. Unblocked when: <condition>."`, report in chat naming the number, **stop**. |

**"Failing" is not "stuck".** A verification that runs and shows the wrong result is a bug you
fix; a verification that **cannot be run at all** — the game will not launch, a tool is missing —
is reason 3, and it stops the landing rather than being papered over, because a landing whose
claim was never run does not meet this repository's bar in the first place. Do not close a ticket
with "could not verify" and a green board.

Never silently abandon a ticket. If work pauses, `kb t unassign` or `kb t block` with a comment
— the board reflects reality or it is worthless.

## 10. Close

`agent-kanban` §9's preconditions hold before `kb t done`, and the landing itself has already
passed, so close directly — no announce-and-wait turn. Preconditions:

- **The work is on local `main`** (the fast-forward completed). Not "committed on the branch".
- **A `testing_evidence` attachment exists** — the clip, or the §6 substitute with its
  one-line justification.
- **The description reflects the final state.** If scope shifted, `kb t edit` it first.

```bash
kb t done <KEY> -m "<what changed | what is verified | what is left or follow-up>"
```

Then report in chat, in at most six lines: the key, the `main` commit, how it was verified, any
follow-up ticket, and **the next landing's number and name** so re-invoking is one line.

**When the last landing closes, announce — do not act:**

> All landings of `TA-50` are done. Close the Epic?

## Don'ts

- **Don't do two landings in one invocation.** The authorization is spent when this one closes.
- **Don't `/git_publish`.** Landing is local; publishing is always the human's.
- **Don't pick, work, or close the Epic.** You work its children.
- **Don't label a bug child `landing:<n>`** — it corrupts the next-landing count.
- **Don't renumber a landing row that already has a child.** Split it in place (§7); the label is
  the join.
- **Don't split a gate into a part per fix, per review round or per documentation pass** (§7).
  Those belong to the landing that produced them, and a ten-part gate has been sliced, not split.
- **Don't leave a split in the roadmap row and not on the board, or the reverse.** Both surfaces
  or neither.
- **Don't inherit `default_project`.** Pass `ta_impure_patch` explicitly.
- **Don't leave the documentation pass to Step 4.** Write it while you still know what the work
  learned.
- **Don't attach a clip you have not played back.**
- **Don't declare a landing finished by reading it.** The ticket names the thing to run; run it.
- **Don't weaken, duplicate, or route around a gate of `/git_commit_merge_wt`.**
