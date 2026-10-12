// tools/tarun.py's run files as the viewer holds them, and what it works out from them.
import type { Item, Live, Run, Step } from '../types'

/** Finished top-level runs of this session kept in view, newest first. */
export const KEEP_ENDED = 5

export type Doc = Omit<Run, 'live'> & { pid?: number | null; pid_start?: number | null }

/** The start time of a process in clock ticks since boot (/proc/<pid>/stat field 22), or null
 * when it is gone. Fields are counted from the last ')': the command name may hold either. */
export function startTicks(stat: string): number | null {
  const fields = stat.slice(stat.lastIndexOf(')') + 1).trim().split(/\s+/)
  const ticks = Number(fields[19])
  return Number.isFinite(ticks) ? ticks : null
}

export function cut(doc: Doc, live: Live): Run {
  return {
    id: doc.id,
    session: doc.session,
    tool: doc.tool,
    kind: doc.kind,
    title: doc.title,
    worktree: doc.worktree,
    owner: doc.owner,
    state: doc.state,
    live,
    started: doc.started,
    ended: doc.ended,
    eta: doc.eta,
    ...(doc.pools ? { pools: doc.pools } : {}),
    items: doc.items.map(it => ({
      id: it.id,
      state: it.state,
      ...(it.checks ? { checks: it.checks } : {}),
      steps: it.steps.map(s => ({
        id: s.id,
        state: s.state,
        ...(s.after ? { after: s.after } : {}),
        ...(s.pool ? { pool: s.pool } : {}),
        ...(s.instance ? { instance: s.instance } : {}),
        ...(s.gt != null ? { gt: s.gt } : {}),
        ...(s.gt_target != null ? { gt_target: s.gt_target } : {}),
        ...(s.started ? { started: s.started } : {}),
        ...(s.seconds != null ? { seconds: s.seconds } : {}),
        ...(s.detail ? { detail: s.detail } : {}),
      })),
    })),
    parent: doc.parent,
    ...(doc.summary ? { summary: doc.summary } : {}),
    ...(doc.last ? { last: doc.last } : {}),
    ...(doc.problems?.length ? { problems: doc.problems } : {}),
  }
}

/** Whether a step's end only repeats its item's checks: the checks step ends `fail` (`unrun`)
 * with no detail of its own when the checks it ran are listed failed (unrun) by name. */
export function echoesChecks(it: Item, s: Step): boolean {
  const named = s.state === 'fail' ? it.checks?.fail : s.state === 'unrun' ? it.checks?.unrun
    : undefined
  return !s.detail && (named?.length ?? 0) > 0
}

/** A run's failures so far: failed steps and failed checks. */
export function failures(run: Run): string[] {
  return run.items.flatMap(it => [
    ...it.steps
      .filter(s => s.state === 'fail' && !echoesChecks(it, s))
      .map(s => `${it.id ? `${it.id} ` : ''}${s.id}: ${s.detail ?? 'failed'}`),
    ...(it.checks?.fail ?? []).map(f => `${it.id ? `${it.id} ` : ''}${f}`),
  ])
}

/** The run and every run filed under it, at any depth. */
export function family(runs: Run[], top: Run): Run[] {
  const kids = runs.filter(r => r.parent?.run === top.id)
  return [top, ...kids.flatMap(k => family(runs, k))]
}

/** What to show of a session's runs: the open ones and the last KEEP_ENDED ended, each with the
 * runs filed under it. */
export function shown(all: Run[]): Run[] {
  const ids = new Set(all.map(r => r.id))
  const tops = all.filter(r => !r.parent || !ids.has(r.parent.run))
  const open = tops.filter(r => r.live === 'running')
  const ended = tops
    .filter(r => r.live !== 'running')
    .sort((a, b) => (b.ended ?? b.started) - (a.ended ?? a.started))
    .slice(0, KEEP_ENDED)
  return [...open, ...ended].flatMap(r => family(all, r))
}
