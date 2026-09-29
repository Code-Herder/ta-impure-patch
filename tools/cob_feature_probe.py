#!/usr/bin/env python3
"""Exercise shipped mod scripts through normal in-game orders in a private Wine game.

Evidence and copied game assets stay outside the repository. The game and display
are owned by this invocation and cleaned up even when an assertion fails.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
import uuid

import cob_getter_probe as probe
import talog

compat = probe.compat
command = probe.command


def snapshot(pid, indices):
    """Read observations only; all mutations go through native game orders."""
    with open(f'/proc/{pid}/mem', 'rb') as mem:
        def read(address, size):
            mem.seek(address)
            data = mem.read(size)
            if len(data) != size:
                raise RuntimeError('short engine observation')
            return data

        def word(address):
            return struct.unpack('<I', read(address, 4))[0]

        main = word(0x511DE8)
        base = word(main + 0x14357)
        slots = struct.unpack('<H', read(main + 0x14351, 2))[0]
        if 'upgrade' not in indices:
            pool = read(base, slots * 0x118)
            for index in range(1, slots):
                data = pool[index * 0x118:(index + 1) * 0x118]
                if struct.unpack_from('<I', data, 0x110)[0] & 0x10000000:
                    definition = struct.unpack_from('<I', data, 0x92)[0]
                    if read(definition + 32, 32).split(b'\0')[0] == b'CORFUS_UPGRADE':
                        indices['upgrade'] = index
                        break
        result = dict(tick=word(main + 0x38A47), units={})
        for handle, index in indices.items():
            if not 0 < index < slots:
                raise RuntimeError(f'unit {index} outside match pool')
            data = read(base + index * 0x118, 0x118)
            parent = struct.unpack_from('<I', data, 0x86)[0]
            if parent and (parent < base or (parent - base) % 0x118 or
                           (parent - base) // 0x118 >= slots):
                raise RuntimeError('transporter does not name a slot in the unit pool')
            u = dict(index=index, alive=bool(struct.unpack_from('<I', data, 0x110)[0] & 0x10000000),
                     remaining=struct.unpack_from('<f', data, 0x104)[0],
                     transporter=(parent - base) // 0x118 if parent else 0)
            cob = struct.unpack_from('<I', data, 0x9A)[0]
            if u['alive'] and cob:
                script = word(cob + 8)
                count = word(script + 16)
                if count > 4096:
                    raise RuntimeError('unbounded script static count')
                u['statics'] = list(struct.unpack(f'<{count}i', read(word(cob + 16), count * 4))) if count else []
                u['threads'] = [struct.unpack('<9i', read(cob + 28 + slot * 548, 36))
                                for slot in range(8)]
            result['units'][handle] = u
        return result


def save_upgrade(name, pid, indices, out):
    command('keys', name, 'shift', 'pause')
    probe.wait_paused(pid)
    before = snapshot(pid, indices)
    probe.open_options(name)
    command('ui', name, 'click', 'SAVEGAME')
    command('ui', name, 'click', 'GAMENAME')
    filled = json.loads(command('ui', name, 'fill', 'GAMENAME', 'U', '--json'))
    if filled.get('text') != 'U':
        raise RuntimeError('upgrade save name was not acknowledged')
    command('ui', name, 'click', 'LOAD')
    probe.open_options(name)
    command('ui', name, 'click', 'LOADGAME')
    command('ui', name, 'select', 'GAMES', 'U')
    command('ui', name, 'click', 'LOAD', timeout=90)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        if compat.IN_GAME_PANEL.search(command('ui', name).splitlines()[0]):
            break
        time.sleep(.2)
    else:
        raise RuntimeError('upgrade save did not return to the game')
    probe.wait_paused(pid)
    after = snapshot(pid, indices)

    def state(s):
        result = {}
        for handle, unit in s['units'].items():
            unit = dict(unit)
            active = []
            for slot, record in enumerate(unit.pop('threads', [])):
                if record[0]:
                    record = list(record)
                    record[3] = record[8] = 0
                    active.append([slot, record])
            result[handle] = dict(unit, threads=active)
        return result

    evidence = dict(before=before, after=after, ok=state(before) == state(after))
    (out / 'upgrade-save.json').write_text(json.dumps(evidence, indent=2))
    if not evidence['ok']:
        raise RuntimeError('upgrade script state changed across save/load')
    command('keys', name, 'shift', 'pause')
    return True


def run(out, dll, seconds):
    setup = compat.pick_setups(['escalation'])[0]
    compat.PREFIX = 'cob-feature-' + uuid.uuid4().hex[:8] + '-'
    name = compat.inst_name(setup['name'])
    if (compat.INSTANCES / name).exists():
        raise RuntimeError('unique test instance already exists')
    server = compat.start_xvfb()
    display = server.tacompat_display
    inst = game = None
    report = dict(instance=name, dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest())
    try:
        print(f'Preparing {name}', flush=True)
        inst = compat.prepare_wine(setup, dll, display)
        for resource in ('Metal', 'Energy'):
            subprocess.run(['wine', 'reg', 'add',
                            r'HKCU\Software\TA Esc\Total Annihilation\Skirmish',
                            '/v', 'Player0' + resource, '/t', 'REG_DWORD', '/d', '1000000', '/f'],
                           env=dict(os.environ, WINEPREFIX=str(inst['prefix'])),
                           capture_output=True, check=True, timeout=120)
        subprocess.run(['wineserver', '-w'], env=dict(os.environ, WINEPREFIX=str(inst['prefix'])), timeout=60)
        (inst['gamedir'] / 'tagpu_cobtrace.on').write_text('CORFUS,CORFUS_UPGRADE,ARMCRAWL,CORDECI,ARMVCAR')
        game = compat.start_wine(inst, display, display_server=server)
        probe.enter_skirmish(name, inst['gamedir'] / 'log/startup-failure.txt')
        command('units', name, '--json')
        scenario = dict(format='ta-scenario/1', seed=7, on_error='abort',
                        setup=dict(clear_existing=False), units=[
                            dict(id='fusion', type='CORFUS', owner=0, pos=[2600, 1200]),
                            dict(id='crawler', type='ARMCRAWL', owner=0, pos=[2800, 1200]),
                            dict(id='decimator', type='CORDECI', owner=0, pos=[2900, 1200]),
                            dict(id='carrier', type='ARMVCAR', owner=0, pos=[3100, 1200])],
                        camera=dict(center_on='fusion', pin=True))
        path = out / 'scenario.json'
        path.write_text(json.dumps(scenario, indent=2))
        applied = json.loads(command('scenario', 'apply', name, str(path), '--json', timeout=180))
        (out / 'apply.json').write_text(json.dumps(applied, indent=2))
        indices = {handle: u['engine_index'] for handle, u in applied['units'].items()}
        hooks = compat.exe_hooks(game['proc'].pid, inst['gamedir'])
        if hooks.get('why'):
            raise RuntimeError(hooks['why'])
        report['hooks'] = hooks
        report['before'] = snapshot(hooks['pid'], indices)
        print('Selecting the fusion plant and queuing its upgrade from the build menu', flush=True)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            roster = json.loads(command('roster', name, '--json'))
            matches = [u for u in roster['units'] if u['engine_index'] == indices['fusion']]
            if matches:
                x, y = matches[0]['screen']
                command('click', name, str(x), str(y))
                if 'CORFUS1.GUI' in command('ui', name):
                    break
            time.sleep(.5)
        else:
            raise RuntimeError('fusion build menu was not selected')
        command('ui', name, 'click', 'CORFUS_UPGRADE')
        samples = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            failure = inst['gamedir'] / 'log/startup-failure.txt'
            if failure.exists():
                raise RuntimeError(failure.read_text(errors='replace'))
            if game['proc'].poll() is not None:
                raise RuntimeError('game exited before feature completion')
            state = snapshot(hooks['pid'], indices)
            samples.append(state)
            fusion = state['units']['fusion']
            upgrade = state['units'].get('upgrade')
            if upgrade and .1 < upgrade['remaining'] < .9 and not report.get('upgrade_save'):
                print('Saving and loading the upgrade while construction is in progress', flush=True)
                report['upgrade_save'] = save_upgrade(name, hooks['pid'], indices, out)
            if fusion.get('statics', [0, 0])[1] == 1:
                report['upgrade_completed'] = bool(upgrade and upgrade['transporter'] == indices['fusion'])
                break
            time.sleep(1)
        report['samples'] = samples
        peaks, types = {}, {}
        trace = talog.run_text(inst['gamedir'], 'tagpu_cobtrace')
        if 'INCOMPLETE' in trace:
            raise RuntimeError('incomplete COB trace cannot establish stack usage')
        for line in trace.splitlines():
            fields = line.split('\t')
            if fields[0] == 'S':
                types[fields[2]] = fields[3]
            elif fields[0] == 'H':
                typ = types.get(fields[2], fields[2])
                peaks[typ] = max(peaks.get(typ, 0), int(fields[-1]))
        report['stack_peaks'] = peaks
        report['ok'] = bool(report.get('upgrade_completed') and report.get('upgrade_save') and
                            all(peaks.get(t) == 85 for t in ('ARMCRAWL', 'CORDECI')) and
                            not compat.hook_evidence(hooks))
        if not report['ok']:
            report['error'] = 'upgrade, high-stack, or no-TADR assertion failed'
    except Exception as exc:
        report.update(ok=False, error=str(exc))
    finally:
        if game:
            shutil.move(compat.stop_wine(game, keep=True), out / 'wine.log')
        else:
            compat.stop_xvfb(server)
        if inst:
            for stream in ('tagpu', 'tagpu_cobtrace'):
                (out / (stream + '.log')).write_text(talog.run_text(inst['gamedir'], stream))
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        compat.tacli('rm', name, '--force')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--dll', type=Path, default=compat.TREE / 'tagpu/ddraw/ddraw.dll')
    parser.add_argument('--seconds', type=int, default=900)
    args = parser.parse_args()
    out = args.out.resolve()
    if any(out == root or root in out.parents for root in (compat.TREE, compat.main_checkout())):
        parser.error('output must be outside the repository')
    out.mkdir(parents=True, exist_ok=False)
    report = run(out, args.dll.resolve(), args.seconds)
    print(json.dumps({k: v for k, v in report.items() if k not in ('hooks', 'samples', 'before')}, indent=2))
    print(f'Evidence: {out}')
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
