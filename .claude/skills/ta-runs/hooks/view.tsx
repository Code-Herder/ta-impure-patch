// What the pane, the band and the toasts say about runs. Text first (plain functions the tests
// read), then the trees, drawn with the elements of whichever surface asked.
import type { BoxProps, ButtonProps, ElementConstructor, RenderElement, TextProps } from 'claude-code'

import type { Item, Run, Step } from '../types'
import { echoesChecks, failures } from './journal'

export type Els = {
  Box: ElementConstructor<BoxProps>
  Text: ElementConstructor<TextProps>
  Button: ElementConstructor<ButtonProps>
}

const ENDED = new Set(['done', 'pass', 'fail', 'unrun', 'skipped'])

const GLYPH: Record<string, [string, string | undefined]> = {
  planned: ['·', 'inactive'],
  running: ['▶', 'suggestion'],
  done: ['✓', 'inactive'],
  pass: ['✓', 'success'],
  fail: ['✗', 'error'],
  unrun: ['?', 'warning'],
  skipped: ['–', 'inactive'],
  stopped: ['■', 'warning'],
  died: ['✗', 'error'],
  abandoned: ['?', 'warning'],
  exited: ['■', 'inactive'],
}

export function duration(seconds: number): string {
  const s = Math.max(0, seconds)
  if (s < 90) return `${Math.round(s)}s`
  if (s < 90 * 60) return `${Math.round(s / 60)}min`
  return `${(s / 3600).toFixed(1)}h`
}

/** hh:mm of an epoch time on the host's clock, `utcOffset` minutes east of UTC. */
export function clockAt(ms: number, utcOffset: number): string {
  const d = new Date(ms + utcOffset * 60000)
  return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`
}

/** How a run stands: its owner's fate when it ended unwritten, else what it wrote. */
export function standing(run: Run): string {
  return run.live === 'ended' ? run.state : run.live
}

/** "~7min left · end time ≈11:42", "≥3min left" while some step has no history yet, "" with
 * no estimate. */
export function etaText(run: Run, now: number, utcOffset: number): string {
  if (!run.eta || run.live !== 'running') return ''
  const left = Math.max(0, run.eta.seconds - (now / 1000 - run.eta.at))
  if (run.eta.lower_bound) return `≥${duration(left)} left`
  return `~${duration(left)} left · end time ≈${clockAt(now + left * 1000, utcOffset)}`
}

/** What a run counts its progress in: its items, or the steps of a run with none of its own. */
const NOUN: Record<string, string> = { tareplay: 'test groups', tacompat: 'setups' }

/** Where an item stands by its steps: none started, every one ended, or in between. A failed
 * step leaves the item in between while its other tests still play. */
export function phase(it: Item): 'planned' | 'running' | 'ended' {
  if (it.steps.every(s => s.state === 'planned')) return 'planned'
  return it.steps.every(s => ENDED.has(s.state)) ? 'ended' : 'running'
}

/** Where an item in between stands: `playing` a step on one of the run's worker pools (so at
 * most the pools' worker count of items at once), `checking` with only steps that wait for no
 * worker running, or `waiting` for a worker with nothing running. A run that names no pools
 * plays every step. */
export function stage(run: Run, it: Item): 'playing' | 'checking' | 'waiting' {
  const pools = run.pools ?? {}
  const workers = Object.keys(pools).length > 0
  const going = it.steps.filter(s => s.state === 'running')
  if (!going.length) return 'waiting'
  return going.some(s => !workers || (pools[s.pool ?? 'main'] ?? 0) > 0) ? 'playing' : 'checking'
}

/** How far a run has got: its items with every step ended (a test group counts once its checks
 * have a verdict), or for a run with no items of its own, its ended steps. */
export function progress(run: Run): { done: number; total: number; noun: string } {
  if (run.items.length === 1 && run.items[0]!.id === '') {
    const all = run.items[0]!.steps
    return { done: all.filter(s => ENDED.has(s.state)).length, total: all.length, noun: 'steps' }
  }
  return { done: run.items.filter(it => phase(it) === 'ended').length, total: run.items.length,
           noun: NOUN[run.tool] ?? 'items' }
}

/** A bar of `width` cells for `done` of `total`. */
export function bar(done: number, total: number, width = 22): string {
  const full = total ? Math.round((width * done) / total) : 0
  return '█'.repeat(full) + '░'.repeat(width - full)
}

/** The run in one line after its title. Running: progress, failures, time left. Ended: how it
 * stands, how long, when it ended (so a section's outcomes line up), then how far it got when
 * that was not the whole plan, and failures. */
export function summary(run: Run, now: number, utcOffset: number): string {
  const live = standing(run)
  const took = duration((run.ended ?? now / 1000) - run.started)
  const ended = run.live === 'running' ? []
    : [live, took, ...(run.ended ? [clockAt(run.ended * 1000, utcOffset)] : [])]
  if (run.tool === 'tacli') {
    return [...(run.live === 'running' ? [`up ${took}`] : ended),
            ...(run.last ? [`last: ${run.last}`] : [])].join(' · ')
  }
  const { done, total, noun } = progress(run)
  const fails = failures(run).length
  const parts = [...ended]
  if (run.live === 'running' || done < total) parts.push(`${done}/${total} ${noun}`)
  if (fails) parts.push(`${fails} FAIL`)
  if (run.live === 'running') {
    const eta = etaText(run, now, utcOffset)
    if (eta) parts.push(eta)
  }
  return parts.join(' · ')
}

function stepDetail(s: Step, now: number): string {
  switch (s.state) {
    case 'running': {
      const gt = s.gt != null ? `GT ${s.gt}${s.gt_target ? `/${s.gt_target}` : ''}` : ''
      const t = s.started ? duration(now / 1000 - s.started) : ''
      return [s.instance ?? '', gt, t].filter(Boolean).join('  ')
    }
    case 'planned':
      return s.after?.length ? `after ${s.after.join(', ')}` : 'planned'
    case 'unrun':
      return s.detail ?? 'did not run'
    default:
      return [s.seconds != null ? duration(s.seconds) : '', s.detail ?? ''].filter(Boolean)
        .join(' · ')
  }
}

function itemSummary(it: Item): string {
  const c = it.checks
  const said = c ? [c.pass ? `${c.pass} pass` : '', c.fail.length ? `${c.fail.length} FAIL` : '',
                    c.unrun.length ? `${c.unrun.length} could not run` : ''] : []
  const text = said.filter(Boolean).join(' · ')
  return text || (it.state === 'running' ? '' : it.state)
}

/** What went wrong in an ended item, one line each: its failed and unrun steps with why, then
 * its failed and unrun checks. */
function wrong(it: Item): [string, string][] {
  return [
    ...it.steps.filter(s => (s.state === 'fail' || s.state === 'unrun') && !echoesChecks(it, s))
      .map((s): [string, string] => [s.state, `${s.id}: ${s.detail ??
        (s.state === 'fail' ? 'failed' : 'did not run')}`]),
    ...(it.checks?.fail ?? []).map((f): [string, string] => ['fail', f]),
    ...(it.checks?.unrun ?? []).map((f): [string, string] => ['unrun', f]),
  ]
}

/** The toast when one of this session's runs ends. */
export function endToast(run: Run, now: number): string {
  const f = failures(run)
  const took = duration((run.ended ?? now / 1000) - run.started)
  return `${run.title}: ${standing(run)} after ${took}` +
    (f.length ? ` — ${f.length} FAIL, first: ${f[0]}` : '')
}

// ---- trees -----------------------------------------------------------------------------

/** Open by default while it runs, and once ended only when it went wrong. */
export function openByDefault(run: Run): boolean {
  return run.live === 'running' || !['pass', 'done'].includes(standing(run))
}

/** The pane's expansions: what the person chose by key (a run's id, `pass:<run id>` for its
 * passed items, `others`), and how to flip one from what it showed. */
export type Expansion = {
  chosen: Record<string, boolean>
  flip: (key: string, wasOpen: boolean) => void
}

type Ctx = {
  els: Els
  runs: Run[]
  now: number
  utcOffset: number
  ex: Expansion
  /** Another session's runs open only when asked. */
  isOther: boolean
}

function isOpen(c: Ctx, run: Run): boolean {
  return c.ex.chosen[run.id] ?? (!c.isOther && openByDefault(run))
}

function Glyph(els: Els, state: string): RenderElement {
  const [g, color] = GLYPH[state] ?? ['?', undefined]
  return <els.Text color={color}>{g}</els.Text>
}

function line(els: Els, depth: number, ...parts: (RenderElement | string)[]): RenderElement {
  return (
    <els.Box>
      <els.Text wrap="truncate-end">{'  '.repeat(depth)}{parts}</els.Text>
    </els.Box>
  )
}

/** A run's header: its title padded to `pad` so a section's summaries line up, then its summary,
 * which an open running run carries on its bar line instead. */
function header(c: Ctx, run: Run, depth: number, label: string, pad = 0): RenderElement {
  const { els } = c
  const open = isOpen(c, run)
  const said = open && hasBar(run) ? '' : summary(run, c.now, c.utcOffset)
  return (
    <els.Box>
      <els.Text>{'  '.repeat(depth)}</els.Text>
      <els.Button key={`run:${run.id}`} plain onPress={() => c.ex.flip(run.id, open)}>
        <els.Text>{open ? '▾ ' : '▸ '}</els.Text>
        {Glyph(els, standing(run))}
        <els.Text bold> {label.padEnd(pad)}</els.Text>
        <els.Text dimColor> {said}</els.Text>
      </els.Button>
    </els.Box>
  )
}

function hasBar(run: Run): boolean {
  return run.live === 'running' && run.tool !== 'tacli' && progress(run).total > 0
}

function children(c: Ctx, run: Run, step: string | null, depth: number): RenderElement[] {
  return c.runs.filter(r => r.parent?.run === run.id && (r.parent.step ?? null) === step)
    .flatMap(r => runTree(c, r, depth))
}

function stepRows(c: Ctx, run: Run, it: Item, depth: number): RenderElement[] {
  return it.steps.flatMap(s => [
    line(c.els, depth, Glyph(c.els, s.state), ` ${s.id}  `,
         <c.els.Text dimColor={s.state !== 'fail'}>{stepDetail(s, c.now)}</c.els.Text>),
    ...(it.id === '' ? children(c, run, s.id, depth + 1) : []),
  ])
}

/** A running item's tests in one line: each ended test as its name and glyph, each running one
 * with its progress. */
function testLine(c: Ctx, it: Item): RenderElement[] {
  const { els } = c
  const out: RenderElement[] = []
  for (const s of it.steps.filter(s => s.state !== 'planned' && s.state !== 'skipped')) {
    if (s.state !== 'running') {
      out.push(<els.Text>{out.length ? ' ' : ''}{s.id}</els.Text>, Glyph(els, s.state))
      continue
    }
    const said = s.gt != null ? `GT ${s.gt}${s.gt_target ? `/${s.gt_target}` : ''}`
      : s.started ? duration(c.now / 1000 - s.started) : ''
    out.push(<els.Text>{out.length ? '  ' : ''}{s.id} </els.Text>, Glyph(els, s.state),
             <els.Text dimColor>{said ? ` ${said}` : ''}</els.Text>)
  }
  return out
}

/** An ended item's wall time: its first test's start to its last test's end. */
function itemSeconds(it: Item): number | null {
  const timed = it.steps.filter(s => s.started && s.seconds != null)
  if (!timed.length) return null
  return Math.max(...timed.map(s => s.started! + s.seconds!)) -
    Math.min(...timed.map(s => s.started!))
}

const PASSED = new Set(['pass', 'done'])

/** A run of items, grouped: those playing a game (one row each, bounded by the workers),
 * those checking or waiting for a worker (one line each), those ended (what went wrong by name,
 * the passes behind one toggle), those not started. */
function itemRows(c: Ctx, run: Run, depth: number): RenderElement[] {
  const { els } = c
  const rows: RenderElement[] = []
  const between = run.items.filter(it => phase(it) === 'running')
  const ended = run.items.filter(it => phase(it) === 'ended')
  const planned = run.items.filter(it => phase(it) === 'planned')
  const isLive = run.live === 'running'
  const group = (label: string, n: number, after: RenderElement | string = '') =>
    line(els, depth, <els.Text bold>{`${label} (${n})`}</els.Text>, after)
  const names = (items: Item[]) =>
    <els.Text dimColor>{`  ${items.map(i => i.id).join(' ')}`}</els.Text>

  // A run that ended mid-way shows what it cut short as one group, its tests as they stopped.
  const playing = isLive ? between.filter(it => stage(run, it) === 'playing') : between
  if (playing.length) {
    rows.push(group(isLive ? 'running' : 'cut short', playing.length))
    const pad = Math.max(...playing.map(it => it.id.length))
    for (const it of playing) {
      const fails = it.steps.some(s => s.state === 'fail')
      rows.push(line(els, depth + 1, Glyph(els, isLive ? (fails ? 'fail' : 'running') : 'unrun'),
                     ` ${it.id.padEnd(pad)}  `, ...testLine(c, it)))
    }
  }
  if (isLive) {
    for (const label of ['checking', 'waiting'] as const) {
      const these = between.filter(it => stage(run, it) === label)
      if (these.length) rows.push(group(label, these.length, names(these)))
    }
  }
  if (ended.length) {
    const passed = ended.filter(it => PASSED.has(it.state))
    const other = ended.filter(it => !PASSED.has(it.state))
    rows.push(group('done', ended.length,
                    <els.Text dimColor>{`  ${passed.length} ✓${other.length
                      ? ` · ${other.length} ✗` : ''}`}</els.Text>))
    for (const it of other) {
      rows.push(line(els, depth + 1, Glyph(els, it.state), <els.Text bold> {it.id}</els.Text>,
                     <els.Text dimColor> {itemSummary(it)}</els.Text>))
      for (const [state, why] of wrong(it)) {
        rows.push(line(els, depth + 2, Glyph(els, state),
                       <els.Text color={state === 'fail' ? 'error' : undefined}> {why}</els.Text>))
      }
    }
    if (passed.length) {
      const key = `pass:${run.id}`
      const open = c.ex.chosen[key] ?? false
      rows.push(
        <els.Box>
          <els.Text>{'  '.repeat(depth + 1)}</els.Text>
          <els.Button key={key} plain onPress={() => c.ex.flip(key, open)}>
            <els.Text>{open ? '▾ ' : '▸ '}</els.Text>
            {Glyph(els, 'pass')}
            <els.Text> {passed.length} passed</els.Text>
          </els.Button>
        </els.Box>,
      )
      if (open) {
        const pad = Math.max(...passed.map(it => it.id.length))
        for (const it of passed) {
          const took = itemSeconds(it)
          const said = [itemSummary(it), took != null ? duration(took) : ''].filter(Boolean)
          rows.push(line(els, depth + 2, Glyph(els, it.state), ` ${it.id.padEnd(pad)}  `,
                         <els.Text dimColor>{said.join(' · ')}</els.Text>))
        }
      }
    }
  }
  if (planned.length) {
    rows.push(group(isLive ? 'planned' : 'not started', planned.length, names(planned)))
  }
  return rows
}

function body(c: Ctx, run: Run, depth: number): RenderElement[] {
  const { els } = c
  const rows: RenderElement[] = (run.problems ?? []).map(p =>
    line(els, depth, <els.Text color="error">✗ {p}</els.Text>))
  if (hasBar(run)) {
    const { done, total } = progress(run)
    rows.push(line(els, depth, <els.Text color="suggestion">{bar(done, total)}</els.Text>,
                   <els.Text dimColor>  {summary(run, c.now, c.utcOffset)}</els.Text>))
  }
  if (run.items.length === 1 && run.items[0]!.id === '') {
    rows.push(...stepRows(c, run, run.items[0]!, depth))
  } else {
    rows.push(...itemRows(c, run, depth))
  }
  rows.push(...children(c, run, null, depth))
  if (run.live !== 'running' && run.summary) {
    rows.push(line(els, depth, <els.Text dimColor>report: {run.summary}</els.Text>))
  }
  return rows
}

function worktreeName(run: Run): string {
  return run.worktree?.split('/').pop() ?? run.session.slice(0, 8)
}

function runTree(c: Ctx, run: Run, depth: number, label = run.title, pad = 0): RenderElement[] {
  return [header(c, run, depth, label, pad), ...(isOpen(c, run) ? body(c, run, depth + 1) : [])]
}

function tops(runs: Run[]): Run[] {
  const ids = new Set(runs.map(r => r.id))
  return runs.filter(r => !r.parent || !ids.has(r.parent.run))
}

/** Titles are padded to the longest in their section, up to this, so summaries line up. */
const PAD_MAX = 44

function heading(els: Els, label: string, gap = true): RenderElement {
  return (
    <els.Box marginTop={gap ? 1 : 0}>
      <els.Text bold wrap="truncate-end">── {label} {'─'.repeat(200)}</els.Text>
    </els.Box>
  )
}

/** The pane: this session's runs in two sections, IN PROGRESS and DONE (newest first), then the
 * other sessions' running runs behind one toggle. */
export function paneTree(
  els: Els,
  state: { own: Run[]; others: Run[]; now: number; utcOffset: number },
  ex: Expansion,
): RenderElement {
  const { Box, Text, Button } = els
  const own: Ctx = { els, runs: state.own, now: state.now, utcOffset: state.utcOffset, ex,
                     isOther: false }
  const mine = tops(state.own)
  const rows: RenderElement[] = []
  if (!mine.length) {
    rows.push(<Text dimColor>No runs in this session yet.</Text>)
  } else {
    const section = (label: string, runs: Run[], gap: boolean) => {
      rows.push(heading(els, `${label} (${runs.length})`, gap))
      if (!runs.length) rows.push(<Text dimColor>  nothing running</Text>)
      const pad = Math.min(PAD_MAX, Math.max(0, ...runs.map(r => r.title.length)))
      rows.push(...runs.flatMap(r => runTree(own, r, 0, r.title, pad)))
    }
    section('IN PROGRESS', mine.filter(r => r.live === 'running'), false)
    const done = mine.filter(r => r.live !== 'running')
    if (done.length) section('DONE', done, true)
  }
  const others = tops(state.others)
  if (others.length) {
    const open = ex.chosen.others ?? false
    rows.push(
      <Box marginTop={1}>
        <Button key="others" plain onPress={() => ex.flip('others', open)}>
          <Text>{open ? '▾ ' : '▸ '}</Text>
          <Text bold>OTHER SESSIONS</Text>
          <Text dimColor> ({others.length} running)</Text>
        </Button>
      </Box>,
    )
    if (open) {
      const theirs: Ctx = { ...own, runs: state.others, isOther: true }
      rows.push(...others.flatMap(r => runTree(theirs, r, 1, `${worktreeName(r)} · ${r.title}`)))
    }
  }
  return <Box flexDirection="column">{rows}</Box>
}

/** The band: a row per running run of this session (its running child's progress after it),
 * and one for the other sessions; null when nothing runs. */
export function bandRows(
  els: Els,
  state: { own: Run[]; others: Run[]; now: number; utcOffset: number },
  maxRows: number,
): RenderElement | null {
  const { Box, Text } = els
  const running = tops(state.own).filter(r => r.live === 'running')
  const others = tops(state.others)
  if (!running.length && !others.length) return null
  const rows = running.map(r => {
    const kid = state.own.find(k => k.parent?.run === r.id && k.live === 'running')
    const fails = failures(r).length + (kid ? failures(kid).length : 0)
    return (
      <Box>
        <Text wrap="truncate-end">
          {Glyph(els, fails ? 'fail' : 'running')}
          <Text bold> {r.title}</Text>
          <Text dimColor> {summary(r, state.now, state.utcOffset)}</Text>
          {kid ? <Text dimColor> › {kid.title} {summary(kid, state.now, state.utcOffset)}</Text>
               : ''}
        </Text>
      </Box>
    )
  })
  if (others.length) {
    const said = others.map(r => [worktreeName(r), r.tool, r.kind,
                                  summary(r, state.now, state.utcOffset)].filter(Boolean).join(' '))
      .join('; ')
    rows.push(<Box><Text dimColor wrap="truncate-end">  others: {said}</Text></Box>)
  }
  return <Box flexDirection="column">{rows.slice(0, Math.max(1, maxRows))}</Box>
}
