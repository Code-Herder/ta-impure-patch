// A run as tools/tarun.py writes it (sessions/<session>/<run>.json), cut to what the viewer
// draws, with `live` added: how the run stands by its owner process, not by its file's age.

export type Checks = { pass: number; fail: string[]; unrun: string[] }

export type Step = {
  id: string
  /** planned, running, done, pass, fail, unrun or skipped */
  state: string
  after?: string[]
  /** The workers it plays on: one of the run's `pools`, or work that waits for none. */
  pool?: string
  instance?: string
  display?: string
  gt?: number
  gt_target?: number | null
  started?: number
  seconds?: number
  detail?: string
}

export type Item = { id: string; state: string; steps: Step[]; checks?: Checks }

/** The tool's estimate when it last wrote: `seconds` left as of `at` (epoch seconds). */
export type Eta = { at: number; seconds: number; lower_bound: boolean }

/**
 * `running`; `ended` (it wrote its end); or its owner gone with no end written: `died` (the
 * tool), `abandoned` (the session that drove it) or `exited` (a tacli instance's game).
 */
export type Live = 'running' | 'ended' | 'died' | 'abandoned' | 'exited'

export type Run = {
  id: string
  session: string
  tool: string
  kind: string
  title: string
  worktree: string | null
  owner: string
  state: string
  live: Live
  started: number
  ended: number | null
  eta: Eta | null
  /** Each pool's worker count; a step on a pool not named here (or of 0) waits for no worker. */
  pools?: Record<string, number>
  items: Item[]
  parent: { run: string; step: string | null } | null
  summary?: string
  last?: string
  problems?: string[]
}

declare module 'claude-code' {
  interface PluginState {
    'ta-runs': {
      /** This session's runs: every open one, and the last few that ended. */
      own: Run[]
      /** Other sessions' runs that are running now. */
      others: Run[]
      /** An expansion the person chose: a run's by its id, its passed items' by `pass:<id>`,
       * the other sessions' by `others`. */
      open: Record<string, boolean>
      /** When the runs were last read, in epoch milliseconds, a multiple of five seconds. */
      now: number
      /** The host's offset from UTC in minutes, for the finish time. */
      utcOffset: number
    }
  }
}
