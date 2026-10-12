// The viewer against a run folder held in memory: what the pane and the band say, the toasts
// a run's end and its first failure raise, and the reading of /proc liveness.
import { describe, expect, mock, test } from 'claude-code/testing'
import type { On } from 'claude-code'
import type { Engine } from 'claude-code/testing'

import { failures, shown, startTicks } from '../hooks/journal'
import { bar, clockAt, duration, progress, summary } from '../hooks/view'
import type { Run } from '../types'

const ROOT = '/runs/sessions'
const SID = 'S'
const T0 = 1_800_000_000_000                        // epoch ms the clock starts at
const PROC_TICKS = 4242

function stat(pid: number, ticks: number): string {
  // fields 3.. after the name; the name holds a space and a ')', as a process's may
  const after = Array.from({ length: 50 }, (_, k) => (k === 0 ? 'S' : k === 19 ? String(ticks) : '0'))
  return `${pid} (wine64 (x)) ${after.join(' ')}`
}

type Files = Map<string, string>

function doc(id: string, over: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    v: 1, id, session: SID, tool: 'tareplay', kind: 'landing', title: `tareplay landing ${id}`,
    worktree: '/w/mod_feature', owner: 'process', pid: 77, pid_start: PROC_TICKS, state: 'running',
    started: T0 / 1000 - 60, ended: null, parent: null, pools: { game: 2 },
    eta: { at: T0 / 1000, seconds: 420, lower_bound: false },
    items: [{
      id: 'battle', state: 'running',
      steps: [
        { id: 'L', state: 'done', pool: 'game', seconds: 412 },
        { id: 'P', state: 'running', pool: 'game', instance: 'tareplay-2', gt: 3120,
          gt_target: 5300, started: T0 / 1000 - 30 },
        { id: 'checks', state: 'planned', pool: 'offline', after: ['L', 'P'] },
      ],
    }],
    ...over,
  }
}

/** The world beneath the plugin: a run folder, /proc, the session id and the clock. */
function world(on: On, files: Files, procs: Map<number, number>, toasts: string[] = []) {
  mock.env(on, { HOME: '/nowhere', TARUN_DIR: '/runs' })
  const clock = mock.clock(on, { now: T0 })
  on('session.start', ($, e) => ({ cwd: e.cwd }))
  on('command.register', ($, e) => ({ value: { command: e.name } }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.log', () => ({ value: undefined }))
  on('ui.toast', ($, e) => {
    toasts.push(e.text)
    return { value: undefined }
  })
  on('ui.render', { component: 'AbovePrompt' }, () => ({ type: 'Box', props: {}, children: [] }))
  on('session.id', () => ({ value: SID }))
  on('process.run', () => ({ value: { exitCode: 0, stdout: '-0400\n', stderr: '',
                                       isStdoutTruncated: false, isStderrTruncated: false } }))
  on('fs.list', ($, e) => {
    const prefix = `${e.path}/`
    const names = new Set<string>()
    for (const p of files.keys()) {
      if (p.startsWith(prefix)) names.add(p.slice(prefix.length).split('/')[0]!)
    }
    if (!names.size) return { deny: `ENOENT ${e.path}` }
    return { value: [...names].map(name => {
      const isDir = [...files.keys()].some(p => p.startsWith(`${prefix}${name}/`))
      return { name, kind: isDir ? 'dir' : 'file', size: 0, mtimeMs: isDir ? 0 : clock.now(),
               isLink: false }
    }) }
  })
  on('fs.stat', () => ({ value: { kind: 'dir', size: 0, mtimeMs: clock.now(), isLink: false } }))
  on('fs.read', ($, e) => {
    const m = /^\/proc\/(\d+)\/stat$/.exec(e.path)
    if (m) {
      const ticks = procs.get(Number(m[1]))
      return ticks === undefined ? { deny: 'ENOENT' } : { value: stat(Number(m[1]), ticks) }
    }
    const text = files.get(e.path)
    return text === undefined ? { deny: `ENOENT ${e.path}` } : { value: text }
  })
  return clock
}

const PANE_PROPS = {
  title: 'Runs', isFocused: false, bodyColumns: 100, placement: 'dock' as const,
  scroll: { offset: 0, bodyRows: 40 }, view: {},
}
const BAND_PROPS = {
  hasSurvey: false, isWorking: false, maxRows: 6, bodyColumns: 100,
  scroll: { offset: 0, bodyRows: 6 }, view: {},
}

async function start($: Engine) {
  await $.session.start({ cwd: '/w/mod_feature', surface: 'terminal', isInteractive: true })
}

describe('reading', () => {
  test('a process start time is field 22, whatever its name holds', () => {
    expect(startTicks(stat(77, 123456))).toBe(123456)
  })

  test('durations and the host clock', () => {
    expect(duration(59)).toBe('59 s')
    expect(duration(420)).toBe('7 min')
    expect(duration(7200)).toBe('2.0 h')
    expect(clockAt(Date.UTC(2026, 9, 9, 15, 42), -240)).toBe('11:42')
  })

  test('the last five ended runs stay, with what is filed under them', () => {
    const run = (id: string, ended: number | null, parent: string | null = null): Run => ({
      id, session: SID, tool: 't', kind: '', title: id, worktree: null, owner: 'process',
      state: ended ? 'pass' : 'running', live: ended ? 'ended' : 'running', started: 0, ended,
      eta: null, items: [], parent: parent ? { run: parent, step: null } : null,
    })
    const all = [run('open', null), ...[1, 2, 3, 4, 5, 6, 7].map(k => run(`e${k}`, k)),
                 run('kid', 7, 'e7'), run('old-kid', 1, 'e1')]
    expect(shown(all).map(r => r.id)).toEqual(['open', 'e7', 'kid', 'e6', 'e5', 'e4', 'e3'])
  })
})

describe('the pane', () => {
  test('the plan, the lane on its instance, the time left', async ($, on) => {
    const files: Files = new Map([[`${ROOT}/${SID}/a.json`, JSON.stringify(doc('a'))]])
    const clock = world(on, files, new Map([[77, PROC_TICKS]]))
    await start($)
    await clock.advance(2000)
    const ui = await $.ui.mount({ plugin: 'ta-runs', surface: 'terminal', component: 'Pane',
                                  props: PANE_PROPS, requestId: 'ta-runs' })
    expect(await ui.find({ text: /tareplay landing a/ })).toBeDefined()
    expect(await ui.find({ text: /── IN PROGRESS \(1\)/ })).toBeDefined()
    expect(await ui.find({ text: /░{22} {2}0\/1 fixtures · ~7 min · ≈/ })).toBeDefined()
    expect(await ui.find({ text: /running \(1\)/ })).toBeDefined()
    expect(await ui.find({ text: /▶ battle {2}L✓ {2}P ▶ GT 3120\/5300$/ })).toBeDefined()
    expect(await ui.find({ text: /── DONE/ })).toBeUndefined()
    await ui.unmount()
  })

  test('a run whose process is gone with no end written is said to have died', async ($, on) => {
    const files: Files = new Map([[`${ROOT}/${SID}/a.json`, JSON.stringify(doc('a'))]])
    const procs = new Map([[77, PROC_TICKS + 1]])          // the PID taken by another process
    const clock = world(on, files, procs)
    await start($)
    await clock.advance(2000)
    const ui = await $.ui.mount({ plugin: 'ta-runs', surface: 'terminal', component: 'Pane',
                                  props: PANE_PROPS, requestId: 'ta-runs' })
    expect(await ui.find({ text: /✗ tareplay landing a died · 60 s · 0\/1 fixtures$/ })).toBeDefined()
    await ui.unmount()
  })

  test('another session\'s running run is one line, folded', async ($, on) => {
    const files: Files = new Map([
      [`${ROOT}/other/b.json`, JSON.stringify(doc('b', { session: 'other',
                                                         worktree: '/w/demo_recorder' }))],
      [`${ROOT}/${SID}/.lock`, ''],
    ])
    const clock = world(on, files, new Map([[77, PROC_TICKS]]))
    await start($)
    await clock.advance(2000)
    const ui = await $.ui.mount({ plugin: 'ta-runs', surface: 'terminal', component: 'Pane',
                                  props: PANE_PROPS, requestId: 'ta-runs' })
    expect(await ui.find({ text: /OTHER SESSIONS \(1 running\)/ })).toBeDefined()
    expect(await ui.find({ text: /demo_recorder · tareplay landing b/ })).toBeUndefined()
    await ui.press({ key: 'others' })
    expect(await ui.find({ text: /demo_recorder · tareplay landing b/ })).toBeDefined()
    await ui.unmount()
  })
})

describe('the band and the toasts', () => {
  test('a row while a run goes, nothing once it has ended', async ($, on) => {
    const files: Files = new Map([[`${ROOT}/${SID}/a.json`, JSON.stringify(doc('a'))]])
    const toasts: string[] = []
    const clock = world(on, files, new Map([[77, PROC_TICKS]]), toasts)
    await start($)
    await clock.advance(2000)
    const band = await $.ui.mount({ plugin: 'ta-runs', surface: 'terminal',
                                    component: 'AbovePrompt', props: BAND_PROPS })
    expect(await band.find({ text: /tareplay landing a/ })).toBeDefined()
    await band.unmount()

    const failed = doc('a')
    ;(failed.items as { checks?: unknown }[])[0]!.checks = {
      pass: 40, fail: ['P compare --puppet: unit 412'], unrun: [],
    }
    files.set(`${ROOT}/${SID}/a.json`, JSON.stringify(failed))
    await clock.advance(2000)
    expect(toasts.some(t => t.includes('first failure — battle P compare --puppet'))).toBe(true)

    files.set(`${ROOT}/${SID}/a.json`, JSON.stringify({ ...failed, state: 'fail',
                                                        ended: T0 / 1000 + 4 }))
    await clock.advance(2000)
    expect(toasts.some(t => /tareplay landing a: fail after 64 s — 1 FAIL/.test(t))).toBe(true)
    const after = await $.ui.mount({ plugin: 'ta-runs', surface: 'terminal',
                                     component: 'AbovePrompt', props: BAND_PROPS })
    expect(await after.find({ text: /tareplay landing a/ })).toBeUndefined()
    await after.unmount()
  })
})

describe('an ended run', () => {
  test('names each lane that could not run, and why', async ($, on) => {
    const ended = doc('a', { state: 'unrun', ended: T0 / 1000 - 5, items: [{
      id: 'c2-weapon-keys', state: 'unrun', checks: { pass: 0, fail: [], unrun: [] },
      steps: [{ id: 'L', state: 'unrun', detail: 'the twin: mouse event [512, 384, 512]' },
              { id: 'P', state: 'unrun' }],
    }] })
    const files: Files = new Map([[`${ROOT}/${SID}/a.json`, JSON.stringify(ended)]])
    const clock = world(on, files, new Map())
    await start($)
    await clock.advance(2000)
    const ui = await $.ui.mount({ plugin: 'ta-runs', surface: 'terminal', component: 'Pane',
                                  props: PANE_PROPS, requestId: 'ta-runs' })
    expect(await ui.find({ text: /c2-weapon-keys unrun$/ })).toBeDefined()
    expect(await ui.find({ text: /\? L: the twin: mouse event \[512, 384, 512\]/ })).toBeDefined()
    expect(await ui.find({ text: /\? P: did not run/ })).toBeDefined()
    await ui.unmount()
  })
})

test('failures name the item, the step and the check', () => {
  const run = { items: [{ id: 'battle', state: 'fail', checks: { pass: 1, fail: ['x: y'], unrun: [] },
                          steps: [{ id: 'K', state: 'fail', detail: 'judge' }] }] } as unknown as Run
  expect(failures(run)).toEqual(['battle K: judge', 'battle x: y'])
  const verdict = { items: [{ id: 'c2', state: 'fail', checks: { pass: 3, fail: ['R: hp'], unrun: [] },
                              steps: [{ id: 'checks', state: 'fail' }] }] } as unknown as Run
  expect(failures(verdict)).toEqual(['c2 R: hp'])
  expect(summary({ ...run, tool: 'tareplay', live: 'running', eta: null, started: 0,
                   ended: null } as Run, 1000, 0)).toBe('1/1 fixtures · 2 FAIL')
})

describe('in progress and done', () => {
  test('a running run above, an ended one below, folded to its outcome', async ($, on) => {
    const files: Files = new Map([
      [`${ROOT}/${SID}/a.json`, JSON.stringify(doc('a'))],
      [`${ROOT}/${SID}/b.json`, JSON.stringify(doc('b', {
        state: 'pass', ended: T0 / 1000 - 10, started: T0 / 1000 - 370,
        items: [{ id: 'battle', state: 'pass', checks: { pass: 4, fail: [], unrun: [] },
                  steps: [{ id: 'L', state: 'done', seconds: 300 },
                          { id: 'checks', state: 'pass', seconds: 20 }] }],
      }))],
    ])
    const clock = world(on, files, new Map([[77, PROC_TICKS]]))
    await start($)
    await clock.advance(2000)
    const ui = await $.ui.mount({ plugin: 'ta-runs', surface: 'terminal', component: 'Pane',
                                  props: PANE_PROPS, requestId: 'ta-runs' })
    expect(await ui.find({ text: /── IN PROGRESS \(1\)/ })).toBeDefined()
    expect(await ui.find({ text: /── DONE \(1\)/ })).toBeDefined()
    const at = clockAt(T0 - 10_000, -240)
    expect(await ui.find({ text: new RegExp(`▸ ✓ tareplay landing b pass · 6 min · ${at}$`) }))
      .toBeDefined()
    expect(await ui.find({ text: /passed/ })).toBeUndefined()
    await ui.unmount()
  })

  test('a run\'s fixtures: running, done with the failures named and the passes folded, planned',
       async ($, on) => {
    const ok = (id: string, t: number) => ({
      id, state: 'pass', checks: { pass: 4, fail: [], unrun: [] },
      steps: [{ id: 'L', state: 'done', started: t, seconds: 200 },
              { id: 'checks', state: 'pass', started: t + 200, seconds: 100 }],
    })
    const files: Files = new Map([[`${ROOT}/${SID}/a.json`, JSON.stringify(doc('a', { items: [
      (doc('a').items as unknown[])[0],
      ok('b7-flash', 1000), ok('c2-stack', 2000),
      { id: 'c2-airwar', state: 'fail', checks: { pass: 3, fail: ['R: unit 412 hp'], unrun: [] },
        steps: [{ id: 'L', state: 'done', seconds: 100 },
                { id: 'checks', state: 'fail', seconds: 10 }] },
      { id: 'b7-radar', state: 'planned', steps: [{ id: 'L', state: 'planned' }] },
      { id: 'c3-vet', state: 'running',
        steps: [{ id: 'L', state: 'done', pool: 'game' },
                { id: 'checks', state: 'running', pool: 'offline' }] },
      { id: 'c4-kill', state: 'running',
        steps: [{ id: 'L', state: 'done', pool: 'game' }, { id: 'P', state: 'planned', pool: 'game' }] },
    ] }))]])
    const clock = world(on, files, new Map([[77, PROC_TICKS]]))
    await start($)
    await clock.advance(2000)
    const ui = await $.ui.mount({ plugin: 'ta-runs', surface: 'terminal', component: 'Pane',
                                  props: PANE_PROPS, requestId: 'ta-runs' })
    expect(await ui.find({ text: /3\/7 fixtures · 1 FAIL · ~7 min/ })).toBeDefined()
    expect(await ui.find({ text: /checks: / })).toBeUndefined()
    expect(await ui.find({ text: /^ *running \(1\)$/ })).toBeDefined()
    expect(await ui.find({ text: /^ *done \(3\) {2}2 ✓ · 1 ✗$/ })).toBeDefined()
    expect(await ui.find({ text: /✗ c2-airwar 3 pass · 1 FAIL$/ })).toBeDefined()
    expect(await ui.find({ text: /✗ R: unit 412 hp$/ })).toBeDefined()
    expect(await ui.find({ text: /▸ ✓ 2 passed$/ })).toBeDefined()
    expect(await ui.find({ text: /b7-flash/ })).toBeUndefined()
    expect(await ui.find({ text: /^ *planned \(1\) {2}b7-radar$/ })).toBeDefined()
    expect(await ui.find({ text: /^ *checking \(1\) {2}c3-vet$/ })).toBeDefined()
    expect(await ui.find({ text: /^ *waiting \(1\) {2}c4-kill$/ })).toBeDefined()
    await ui.press({ key: 'pass:a' })
    expect(await ui.find({ text: /▾ ✓ 2 passed$/ })).toBeDefined()
    expect(await ui.find({ text: /✓ b7-flash {2}4 pass · 5 min$/ })).toBeDefined()
    expect(await ui.find({ text: /✓ c2-stack {2}4 pass · 5 min$/ })).toBeDefined()
    await ui.unmount()
  })
})

test('progress counts fixtures with a verdict, the bar its share', () => {
  const run = { tool: 'tareplay', items: [
    { id: 'a', state: 'pass', steps: [{ id: 'L', state: 'done' }, { id: 'checks', state: 'pass' }] },
    { id: 'b', state: 'running', steps: [{ id: 'L', state: 'done' }, { id: 'checks', state: 'planned' }] },
  ] } as unknown as Run
  expect(progress(run)).toEqual({ done: 1, total: 2, noun: 'fixtures' })
  expect(progress({ tool: 'landing', items: [{ id: '', state: 'running', steps: [
    { id: 'commit', state: 'pass' }, { id: 'build', state: 'running' }] }] } as unknown as Run))
    .toEqual({ done: 1, total: 2, noun: 'steps' })
  expect(bar(9, 41)).toBe('█'.repeat(5) + '░'.repeat(17))
  expect(bar(0, 0)).toBe('░'.repeat(22))
})
