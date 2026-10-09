// ta-runs: what this session's gates and games are doing, read from tools/tarun.py's run files.
// A viewer only: nothing here writes a file a tool reads, starts a process a run owns, or acts on
// a run. The pane is the tree (/runs), the band the running runs, a toast a run's end or its
// first failure.
import { atom, read, update } from 'claude-code'
import type { EngineInterface, Register } from 'claude-code'

import type { Live, Run } from '../types'
import type { Doc } from './journal'
import { cut, failures, family, shown, startTicks } from './journal'
import { bandRows, endToast, paneTree, standing } from './view'

const PANE = 'ta-runs'
const POLL_MS = 2000

const own = atom({ plugin: 'ta-runs', key: 'own' } as const, [] as Run[])
const others = atom({ plugin: 'ta-runs', key: 'others' } as const, [] as Run[])
const open = atom({ plugin: 'ta-runs', key: 'open' } as const, {} as Record<string, boolean>)
const now = atom({ plugin: 'ta-runs', key: 'now' } as const, 0)
const utcOffset = atom({ plugin: 'ta-runs', key: 'utcOffset' } as const, 0)

// ---- reading the run files: here, since $ is followed into no other file ----

/** A session folder nobody wrote to for this long holds nothing running worth a look. */
const OTHERS_FRESH_MS = 24 * 3600 * 1000

// Each file read once per change: tarun replaces a run file whole, so its mtime moves on every
// write, and a file listed but replaced before the read is read again on the next poll.
const cache = new Map<string, { mtimeMs: number; doc: Doc }>()

async function dataRoot($: EngineInterface): Promise<string> {
  const home = (await $.env.get('HOME')) ?? ''
  const named = await $.env.get('TARUN_DIR')
  if (named) return named.replace(/^~(?=\/|$)/, home)
  const xdg = await $.env.get('XDG_DATA_HOME')
  return `${xdg || `${home}/.local/share`}/ta-runs`
}

async function readRuns($: EngineInterface, dir: string): Promise<Doc[]> {
  let entries
  try {
    entries = await $.fs.list(dir)
  } catch {
    return []
  }
  const out: Doc[] = []
  const listed = new Set(entries.map(entry => `${dir}/${entry.name}`))
  for (const path of cache.keys()) {
    if (path.startsWith(`${dir}/`) && !listed.has(path)) cache.delete(path)
  }
  for (const entry of entries) {
    if (entry.kind !== 'file' || !entry.name.endsWith('.json') || entry.name.startsWith('.')) {
      continue
    }
    const path = `${dir}/${entry.name}`
    const hit = cache.get(path)
    if (hit && hit.mtimeMs === entry.mtimeMs) {
      out.push(hit.doc)
      continue
    }
    try {
      const doc = JSON.parse((await $.fs.read(path)) as string) as Doc
      cache.set(path, { mtimeMs: entry.mtimeMs, doc })
      out.push(doc)
    } catch {
      cache.delete(path)
    }
  }
  return out
}

async function liveOf($: EngineInterface, doc: Doc): Promise<Live> {
  if (doc.ended) return 'ended'
  if (doc.pid && doc.pid_start != null) {
    const stat = await ($.fs.read(`/proc/${doc.pid}/stat`) as Promise<string>).catch(() => null)
    if (stat !== null && startTicks(stat) === doc.pid_start) return 'running'
  }
  return doc.owner === 'session' ? 'abandoned' : doc.owner === 'instance' ? 'exited' : 'died'
}

async function poll(
  $: EngineInterface,
  now: number,
): Promise<{ all: Run[]; own: Run[]; others: Run[] }> {
  const root = `${await dataRoot($)}/sessions`
  const sid = await $.session.id()
  const view = async (docs: Doc[]) => Promise.all(docs.map(async d => cut(d, await liveOf($, d))))
  const all = await view(await readRuns($, `${root}/${sid}`))
  const others: Run[] = []
  const sessions = await $.fs.list(root).catch(() => [])
  for (const s of sessions) {
    if (s.kind !== 'dir' || s.name === sid) continue
    const stat = await $.fs.stat(`${root}/${s.name}`).catch(() => null)
    if (!stat || now - stat.mtimeMs > OTHERS_FRESH_MS) continue
    const runs = await view(await readRuns($, `${root}/${s.name}`))
    const ids = new Set(runs.map(r => r.id))
    for (const top of runs.filter(r => r.live === 'running' &&
                                       (!r.parent || !ids.has(r.parent.run)))) {
      others.push(...family(runs, top))
    }
  }
  return { all, own: shown(all), others }
}

/** Open by default while it runs, and once ended only when it went wrong. */
function openByDefault(run: Run): boolean {
  return run.live === 'running' || !['pass', 'done'].includes(standing(run))
}

// Whether this surface docks a pane beside the transcript, as its last drawing said: a pane
// the person did not ask for opens only where it is a sidebar, never inline over the prompt.
let isFullscreen: boolean | undefined
// Each run's standing and failure count at the last poll; the first poll only learns them,
// so runs that ended before this session loaded the mod raise nothing.
const seen = new Map<string, { live: string; fails: number }>()
let isPrimed = false
let isPolling = false
let drawn = ''

async function tick($: EngineInterface) {
  if (isPolling) return
  isPolling = true
  try {
    const t = await $.clock.now()
    const got = await poll($, t)
    let opensPane = false
    for (const run of got.all) {
      const fails = failures(run).length
      const before = seen.get(run.id)
      seen.set(run.id, { live: run.live, fails })
      if (!isPrimed) continue
      if (!before) {
        opensPane ||= run.live === 'running' && run.tool !== 'tacli'
      } else if (before.live === 'running' && run.live !== 'running') {
        // a tacli game stopped by tacli is the expected end; one that went away is not
        if (run.tool !== 'tacli' || run.live === 'exited') $.ui.toast(endToast(run, t))
      } else if (run.live === 'running' && before.fails === 0 && fails > 0) {
        $.ui.toast(`${run.title}: first failure — ${failures(run)[0]}`)
      }
    }
    isPrimed = true
    const at = Math.floor(t / 5000) * 5000
    const next = JSON.stringify([got.own, got.others, at])
    if (next !== drawn) {
      drawn = next
      await update($, own, () => got.own)
      await update($, others, () => got.others)
      await update($, now, () => at)
    }
    if (opensPane && isFullscreen === true) {
      await $.ui.open({ id: PANE, title: 'Runs' })
    }
  } catch (e) {
    $.ui.log(`ta-runs: ${e instanceof Error ? e.message : String(e)}`, { to: 'debug' })
  } finally {
    isPolling = false
  }
}

async function shared($: EngineInterface) {
  const [o, x, t, z] = await Promise.all([read($, own), read($, others), read($, now),
                                          read($, utcOffset)])
  return { own: o, others: x, now: t, utcOffset: z }
}

export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    await $.command.register({
      name: 'runs',
      description: 'Show the gates and games of this session: planned, running, passed, failed, ' +
        'time left',
    })
    const started = await next(e)
    // the host's own offset (`date +%z`, e.g. -0400): the environment this module runs in
    // need not share the host's time zone
    const zone = await $.process.run(['date', '+%z']).catch(() => null)
    const m = zone?.stdout.trim().match(/^([+-])(\d\d)(\d\d)$/)
    if (m) {
      const minutes = (m[1] === '-' ? -1 : 1) * (Number(m[2]) * 60 + Number(m[3]))
      await update($, utcOffset, () => minutes)
    }
    void tick($)
    $.clock.every(POLL_MS, () => void tick($))
    return started
  })

  on('command.run', { command: 'runs' }, async $ => {
    const opened = await $.ui.open({ id: PANE, title: 'Runs' })
    return { text: opened.isPlaced ? 'Runs pane opened.' : 'The runs pane waits for room.' }
  })

  on('ui.render', { component: 'Pane', requestId: PANE }, async ($, e) => {
    if (e.viewport?.isFullscreen !== undefined) isFullscreen = e.viewport.isFullscreen
    const els = $.ui.resolve(e)
    const [state, chosen] = await Promise.all([shared($), read($, open)])
    const isOther = new Set(state.others.map(r => r.id))
    return paneTree(
      els,
      state,
      run => chosen[run.id] ?? (!isOther.has(run.id) && openByDefault(run)),
      chosen.others ?? false,
      run => void update($, open, m => ({ ...m, [run.id]: !(m[run.id] ?? openByDefault(run)) })),
      () => void update($, open, m => ({ ...m, others: !(m.others ?? false) })),
    )
  })

  on('ui.render', { component: 'AbovePrompt' }, async ($, e, next) => {
    if (e.viewport?.isFullscreen !== undefined) isFullscreen = e.viewport.isFullscreen
    if (e.props.hasSurvey) return next(e)
    const rows = bandRows($.ui.resolve(e), await shared($), e.props.maxRows)
    if (!rows) return next(e)
    const { Box } = $.ui.resolve(e)
    return <Box flexDirection="column">{await next(e)}{rows}</Box>
  })
}
