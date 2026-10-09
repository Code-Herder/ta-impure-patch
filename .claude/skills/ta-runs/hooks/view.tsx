// What the pane, the band and the toasts say about runs. Text first (plain functions the tests
// read), then the trees, drawn with the elements of whichever surface asked.
import type { BoxProps, ButtonProps, ElementConstructor, RenderElement, TextProps } from 'claude-code'

import type { Item, Run, Step } from '../types'
import { failures } from './journal'

export type Els = {
  Box: ElementConstructor<BoxProps>
  Text: ElementConstructor<TextProps>
  Button: ElementConstructor<ButtonProps>
}

/** Expansion: what the person chose for a run, else open while it runs or after it failed. */
export type Opened = (run: Run) => boolean

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
  if (s < 90) return `${Math.round(s)} s`
  if (s < 90 * 60) return `${Math.round(s / 60)} min`
  return `${(s / 3600).toFixed(1)} h`
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

export function steps(run: Run): Step[] {
  return run.items.flatMap(it => it.steps)
}

/** "~7 min · ≈11:42", "≥3 min" while some step has no history yet, "" with no estimate. */
export function etaText(run: Run, now: number, utcOffset: number): string {
  if (!run.eta || run.live !== 'running') return ''
  const left = Math.max(0, run.eta.seconds - (now / 1000 - run.eta.at))
  if (run.eta.lower_bound) return `≥${duration(left)}`
  return `~${duration(left)} · ≈${clockAt(now + left * 1000, utcOffset)}`
}

/** The run in one line after its title: progress, failures, time. */
export function summary(run: Run, now: number, utcOffset: number): string {
  const live = standing(run)
  if (run.tool === 'tacli') {
    const up = duration((run.ended ?? now / 1000) - run.started)
    return [run.live === 'running' ? `up ${up}` : `${live} after ${up}`,
            run.last ? `last: ${run.last}` : ''].filter(Boolean).join(' · ')
  }
  const all = steps(run)
  const done = all.filter(s => ENDED.has(s.state)).length
  const fails = failures(run).length
  const parts = [`${done}/${all.length}`]
  if (fails) parts.push(`${fails} FAIL`)
  if (run.live === 'running') {
    const eta = etaText(run, now, utcOffset)
    if (eta) parts.push(eta)
  } else {
    parts.push(`${live}, ${duration((run.ended ?? now / 1000) - run.started)}`)
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
  if (!c) return it.state === 'running' ? '' : it.state
  return [`${c.pass} pass`, c.fail.length ? `${c.fail.length} FAIL` : '',
          c.unrun.length ? `${c.unrun.length} could not run` : ''].filter(Boolean).join(' · ')
}

/** The toast when one of this session's runs ends. */
export function endToast(run: Run, now: number): string {
  const f = failures(run)
  const took = duration((run.ended ?? now / 1000) - run.started)
  return `${run.title}: ${standing(run)} after ${took}` +
    (f.length ? ` — ${f.length} FAIL, first: ${f[0]}` : '')
}

// ---- trees -----------------------------------------------------------------------------

type Ctx = {
  els: Els
  runs: Run[]
  now: number
  utcOffset: number
  opened: Opened
  toggle: (run: Run) => void
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

function header(c: Ctx, run: Run, depth: number, label: string): RenderElement {
  const { els } = c
  return (
    <els.Box>
      <els.Text>{'  '.repeat(depth)}</els.Text>
      <els.Button key={`run:${run.id}`} plain onPress={() => c.toggle(run)}>
        <els.Text>{c.opened(run) ? '▾ ' : '▸ '}</els.Text>
        {Glyph(els, standing(run))}
        <els.Text bold> {label}</els.Text>
        <els.Text dimColor> {summary(run, c.now, c.utcOffset)}</els.Text>
      </els.Button>
    </els.Box>
  )
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

function body(c: Ctx, run: Run, depth: number): RenderElement[] {
  const { els } = c
  const rows: RenderElement[] = (run.problems ?? []).map(p =>
    line(els, depth, <els.Text color="error">✗ {p}</els.Text>))
  if (run.items.length === 1 && run.items[0]!.id === '') {
    rows.push(...stepRows(c, run, run.items[0]!, depth))
  } else {
    const planned = run.items.filter(it => it.state === 'planned')
    for (const it of run.items.filter(i => i.state !== 'planned')) {
      rows.push(line(els, depth, Glyph(els, it.state), <els.Text bold> {it.id}</els.Text>,
                     <els.Text dimColor> {itemSummary(it)}</els.Text>))
      if (it.state === 'running') {
        rows.push(...stepRows(c, run, it, depth + 1))
      } else {
        for (const f of failures({ ...run, items: [{ ...it, id: '' }] })) {
          rows.push(line(els, depth + 1, <els.Text color="error">✗ {f}</els.Text>))
        }
      }
    }
    if (planned.length) {
      rows.push(line(els, depth, Glyph(els, 'planned'),
                     <els.Text dimColor> {planned.length} planned: {planned.map(i => i.id)
                       .join(' ')}</els.Text>))
    }
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

function runTree(c: Ctx, run: Run, depth: number, label = run.title): RenderElement[] {
  return [header(c, run, depth, label), ...(c.opened(run) ? body(c, run, depth + 1) : [])]
}

function tops(runs: Run[]): Run[] {
  const ids = new Set(runs.map(r => r.id))
  return runs.filter(r => !r.parent || !ids.has(r.parent.run))
}

export function paneTree(
  els: Els,
  state: { own: Run[]; others: Run[]; now: number; utcOffset: number },
  opened: Opened,
  othersOpen: boolean,
  toggle: (run: Run) => void,
  toggleOthers: () => void,
): RenderElement {
  const { Box, Text, Button } = els
  const own: Ctx = { els, runs: state.own, now: state.now, utcOffset: state.utcOffset, opened,
                     toggle }
  const rows = tops(state.own).flatMap(r => runTree(own, r, 0))
  if (!rows.length) rows.push(<Text dimColor>No runs in this session yet.</Text>)
  const others = tops(state.others)
  if (others.length) {
    rows.push(
      <Box marginTop={1}>
        <Button key="others" plain onPress={toggleOthers}>
          <Text>{othersOpen ? '▾ ' : '▸ '}</Text>
          <Text dimColor>other sessions: {others.length} running</Text>
        </Button>
      </Box>,
    )
    if (othersOpen) {
      rows.push(...others.flatMap(r => runTree({ ...own, runs: state.others }, r, 1,
                                               `${worktreeName(r)} · ${r.title}`)))
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
    const said = others.map(r => `${worktreeName(r)} ${r.tool} ${r.kind} ` +
      summary(r, state.now, state.utcOffset)).join('; ')
    rows.push(<Box><Text dimColor wrap="truncate-end">  others: {said}</Text></Box>)
  }
  return <Box flexDirection="column">{rows.slice(0, Math.max(1, maxRows))}</Box>
}
