#!/usr/bin/env python3
"""Check native eight-slot import or a full expanded COB save/load round trip."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import struct
import tempfile
import time
import traceback
import uuid

import cob_getter_probe as probe
import talog

compat = probe.compat
command = probe.command
TYPE = 'COBOLD'


def fixture(gamedir, slots):
    archive = probe.hpipack.Archive(gamedir / 'totala1.hpi')
    fbi = archive.read('units/armsolar.fbi').decode('latin-1')
    fbi, n = re.subn(r'(?i)(\bUnitName\s*=\s*)[^;]+;', rf'\g<1>{TYPE};', fbi, count=1)
    if n != 1:
        raise RuntimeError('native save fixture lacks UnitName')
    starts = ''.join(f'start-script Worker({100 + i});' for i in range(slots - 1))
    source = ('piece base; Create() {' + starts + 'sleep 600000; return(0);}'
              'Worker(value) {var local; local=9876; sleep 600000; return(local);}')
    script = probe.cob_audit.tacob.write_cob(probe.cob_audit.tacob.compile_bos(source))
    tree = {}
    probe.hpipack.insert(tree, 'units/cobold.fbi', fbi.encode('latin-1'))
    probe.hpipack.insert(tree, 'scripts/cobold.cob', script)
    (gamedir / 'cob-old-probe.ufo').write_bytes(probe.hpipack.build(tree, method=2))


def records(pid, words):
    with open(f'/proc/{pid}/mem', 'rb') as mem:
        def read(address, size):
            mem.seek(address)
            data = mem.read(size)
            if len(data) != size:
                raise RuntimeError('short memory read')
            return data

        return record_state(read, words)


def record_state(read, words, indices=None):
    def word(address):
        return struct.unpack('<I', read(address, 4))[0]

    main = word(0x511DE8)
    if not read(main + 0x38A51, 1)[0] & 1:
        raise RuntimeError('script observation requires pause acknowledgement')
    base = word(main + 0x14357)
    slots = struct.unpack('<H', read(main + 0x14351, 2))[0]
    result = {}
    defs = word(main + 0x1439B)
    count = word(main + 0x1438F)
    for index in range(1, slots) if indices is None else indices:
        if not 0 < index < slots:
            raise RuntimeError('save probe index exceeds the unit allocation')
        unit = read(base + index * 0x118, 0x118)
        if not struct.unpack_from('<I', unit, 0x110)[0] & 0x10000000:
            continue
        typ = struct.unpack_from('<H', unit, 0xA6)[0]
        if typ >= count:
            raise RuntimeError('live unit type exceeds definition count')
        if read(defs + typ * 0x249 + 0x20, 32).split(b'\0')[0].upper() != TYPE.encode():
            continue
        owner = unit[0xFF]
        if owner in result:
            raise RuntimeError('ambiguous duplicate save probe')
        cob = struct.unpack_from('<I', unit, 0x9A)[0]
        if not cob:
            raise RuntimeError(f'live save probe {index}, owner {owner}, has no script')
        state = []
        record_count = 8 if words == 32 else probe.COB_THREADS
        stride = 36 + 4 * words
        pool = read(cob + 28, record_count * stride)
        for slot in range(record_count):
            data = list(struct.unpack_from(f'<{words + 9}I', pool, slot * stride))
            if data[0]:
                # Native records leave inactive wait fields and unused stack
                # words unspecified. Compare only live state and stack cells.
                depth = struct.unpack('<i', struct.pack('<I', data[2]))[0] + 1
                if not 0 <= depth <= words:
                    raise RuntimeError('native record exceeds its stack')
                state.append(dict(slot=slot, status=data[0], pc=data[1], depth=depth,
                                  mask=data[7], stack=data[9:9 + depth]))
        result[owner] = state
    if not {0, 1} <= set(result):
        raise RuntimeError(f'expected human and AI save probes, found owners {list(result)}')
    return result


def preflight(name, gamedir):
    """Exercise the native loader on the replay worker's exact build before recording."""
    api = runpy.run_path(str(compat.TACLI), run_name='cob_save_preflight')
    inst = api['Instance'](name)
    if inst.gamedir.resolve() != gamedir.resolve():
        raise RuntimeError('save preflight game folder differs from its instance')
    recorder = gamedir / 'tagpu_recorder.on'
    previous = recorder.read_bytes() if recorder.exists() else None
    archive = gamedir / 'cob-old-probe.ufo'
    if archive.exists():
        raise RuntimeError('save preflight would replace existing probe content')
    from resource_regression import Check
    c = Check.__new__(Check)
    c.name = name
    try:
        c.call('stop')
        recorder.unlink(missing_ok=True)
        fixture(gamedir, probe.COB_THREADS)
        c.call('launch', '--defaults', '--keep-dll')
        c.until(lambda: c.call('ui', '--json')['gui'] == 'MAINMENU.GUI',
                'save preflight did not reach the main menu', seconds=30)
        names = {u['name'].upper() for u in c.call('units', '--json')['units']}
        if TYPE not in names:
            raise RuntimeError('save preflight unit did not reach the native catalogue')
        probe.enter_skirmish(name, gamedir / 'log/startup-failure.txt')
        with tempfile.TemporaryDirectory(prefix='cob-save-preflight-') as temp:
            scenario = Path(temp) / 'native-save.json'
            scenario.write_text(json.dumps(dict(format='ta-scenario/1', seed=7, on_error='abort',
                setup=dict(clear_existing=False),
                units=[dict(id='human', type=TYPE, owner=0, pos=[1000, 1200]),
                       dict(id='ai', type=TYPE, owner=1, pos=[3000, 1200])])) + '\n')
            applied = json.loads(command('scenario', 'apply', name, str(scenario), '--json'))
        c.call('keys', 'shift', 'pause')
        c.until(lambda: c.values('*0x511DE8+0x38A51:1')[0] & 1,
                'save preflight did not acknowledge pause')
        def snapshot():
            def read(address, size):
                specs = [f'0x{address + offset:x}:x{min(256, size - offset):x}'
                         for offset in range(0, size, 256)]
                return b''.join(bytes.fromhex(p['value']) for p in
                                c.call('peek', *specs, '--json')['peeks'])
            indices = [u['engine_index'] for u in applied['units'].values()]
            return record_state(read, 128, indices)
        before = snapshot()
        if any(len(rows) != probe.COB_THREADS for rows in before.values()):
            raise RuntimeError('save preflight did not fill every script record')
        print('COB native save preflight: full active pool observed', flush=True)
        probe.open_options(name)
        command('ui', name, 'click', 'SAVEGAME')
        command('ui', name, 'click', 'GAMENAME')
        filled = json.loads(command('ui', name, 'fill', 'GAMENAME', 'L', '--json'))
        if filled.get('text') != 'L':
            raise RuntimeError('save preflight filename was not acknowledged')
        command('ui', name, 'click', 'LOAD')
        probe.open_options(name)
        command('ui', name, 'click', 'LOADGAME')
        command('ui', name, 'select', 'GAMES', 'L')
        command('ui', name, 'click', 'LOAD', timeout=90)
        print('COB native save preflight: native load acknowledged', flush=True)
        c.until(lambda: compat.IN_GAME_PANEL.search(command('ui', name).splitlines()[0]),
                'save preflight did not return to play', seconds=90)
        if not c.values('*0x511DE8+0x38A51:1')[0] & 1:
            c.call('keys', 'shift', 'pause')
        c.until(lambda: c.values('*0x511DE8+0x38A51:1')[0] & 1,
                'loaded save preflight did not acknowledge pause')
        if before != snapshot():
            raise RuntimeError('native save/load changed active script records')
        target = gamedir / 'replays/cover-resource-save.bin'
        target.unlink(missing_ok=True)
        trigger = gamedir / 'tagpu_replay.trigger'
        pending = trigger.with_suffix('.tmp')
        pending.write_text('cover resource-save\n')
        pending.replace(trigger)
        c.until(lambda: target.exists() or 'cover: resource-save refused: not a coverage build'
                in talog.run_text(gamedir), 'native save coverage request was not acknowledged')
        print(f'COB native save preflight: all {probe.COB_THREADS} active slots preserved '
              'on human and AI probes', flush=True)
    finally:
        try:
            c.call('stop')
        finally:
            archive.unlink(missing_ok=True)
            if previous is None:
                recorder.unlink(missing_ok=True)
            else:
                recorder.write_bytes(previous)


def run(out, dll, stock, load_limit=None, reject_saved=False, expanded=False):
    setup = compat.pick_setups(['retail'])[0]
    compat.PREFIX = 'cob-old-' + uuid.uuid4().hex[:8] + '-'
    name = compat.inst_name('retail')
    server = compat.start_xvfb()
    inst = game = None
    serializer = dll if expanded else stock
    words, slots = (128, probe.COB_THREADS) if expanded else (32, 8)
    report = dict(instance=name, dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest(),
                  serializer_dll_sha256=hashlib.sha256(serializer.read_bytes()).hexdigest(),
                  serialized_words=words, serialized_slots=slots)
    try:
        inst = compat.prepare_wine(setup, serializer, server.tacompat_display)
        fixture(inst['gamedir'], slots)
        game = compat.start_wine(inst, server.tacompat_display, display_server=server)
        print(f'Creating a retail save with {slots}-slot COB serialization', flush=True)
        probe.enter_skirmish(name, inst['gamedir'] / 'log/startup-failure.txt')
        cmd = compat.commanders(name)
        if not cmd['ok']:
            raise RuntimeError(cmd['why'])
        command('units', name, '--json')
        scenario = out / 'native-probes.json'
        scenario.write_text(json.dumps(dict(format='ta-scenario/1', seed=7, on_error='abort',
            setup=dict(clear_existing=False),
            units=[dict(id='human', type=TYPE, owner=0, pos=[1000, 1200]),
                   dict(id='ai', type=TYPE, owner=1, pos=[3000, 1200])])) + '\n')
        command('scenario', 'apply', name, str(scenario), '--json')
        command('keys', name, 'shift', 'pause')
        hooks = compat.exe_hooks(game['proc'].pid, inst['gamedir'])
        if hooks.get('why'):
            raise RuntimeError(hooks['why'])
        probe.wait_paused(hooks['pid'])
        report['before'] = records(hooks['pid'], words)
        if any(len(rows) != slots for rows in report['before'].values()):
            raise RuntimeError('save did not exercise every script record')
        probe.open_options(name)
        command('ui', name, 'click', 'SAVEGAME')
        command('ui', name, 'click', 'GAMENAME')
        filled = json.loads(command('ui', name, 'fill', 'GAMENAME', 'L', '--json'))
        if filled.get('text') != 'L':
            raise RuntimeError(f'save filename was not acknowledged: {filled}')
        command('ui', name, 'click', 'LOAD')
        shutil.move(compat.stop_wine(game, keep=True), out / 'native-wine.log')
        game = None
        (out / 'native-tagpu.log').write_text(talog.run_text(inst['gamedir']))
        for saved in inst['gamedir'].rglob('*'):
            if saved.is_file() and saved.suffix.lower() == '.sav':
                target = out / 'native-save' / saved.relative_to(inst['gamedir'])
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(saved, target)
        shutil.copy2(dll, inst['gamedir'] / 'ddraw.dll')
        if reject_saved:
            op = probe.cob_audit.tacob.OP
            invalid = probe.cob_audit.tacob.Cob([('Create', 0)], [], 0,
                       [op['PUSH_LOCAL_VAR'], 128, op['RETURN']])
            scripts = inst['gamedir'] / 'scripts'
            scripts.mkdir(exist_ok=True)
            (scripts / 'ARMCOM.COB').write_bytes(probe.cob_audit.tacob.write_cob(invalid))
        if load_limit is not None:
            ini = next(p for p in inst['gamedir'].iterdir() if p.name.lower() == 'totala.ini')
            content, count = re.subn(r'(?i)(UnitLimit\s*=\s*)\d+',
                                    lambda m: m[1] + str(load_limit), ini.read_text())
            if count == 0:
                content, count = re.subn(r'(?im)^\[Preferences\]\s*$',
                                        lambda m: m[0] + f'\nUnitLimit={load_limit}', content)
            if count != 1:
                raise RuntimeError('expected exactly one Preferences section or UnitLimit')
            ini.write_text(content)
            report['load_limit'] = load_limit
        server = compat.start_xvfb()
        game = compat.start_wine(inst, server.tacompat_display, display_server=server)
        print('Loading the native save with the guarded expanded runtime', flush=True)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            ui = compat.tacli('ui', name, timeout=20)
            if ui.returncode == 0 and 'MAINMENU.GUI' in ui.stdout:
                break
            if game['proc'].poll() is not None:
                raise RuntimeError('raised runtime exited before the main menu')
            time.sleep(.2)
        else:
            raise RuntimeError('raised runtime did not acknowledge its main menu')
        command('ui', name, 'click', 'SINGLE')
        command('ui', name, 'click', 'LOADGAME')
        command('ui', name, 'select', 'GAMES', 'L')
        # A refused load has no replacement GUI for tacli's post-click snapshot.
        loaded = compat.tacli('ui', name, 'click', 'LOAD', timeout=90)
        if loaded.returncode and not reject_saved:
            raise RuntimeError(loaded.stderr or loaded.stdout)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            failure = inst['gamedir'] / 'log/startup-failure.txt'
            if failure.exists():
                text = failure.read_text(errors='replace')
                if reject_saved:
                    report['refusal'] = text
                    report['ok'] = all(s in text for s in
                        ('required or saved unit', 'ARMCOM', 'Script:', 'local index'))
                    return report
                raise RuntimeError(text)
            if compat.IN_GAME_PANEL.search(command('ui', name).splitlines()[0]):
                break
            time.sleep(.2)
        else:
            raise RuntimeError('native save never reached play')
        if reject_saved:
            raise RuntimeError('save containing a rejected required unit reached play')
        hooks = compat.exe_hooks(game['proc'].pid, inst['gamedir'])
        if hooks.get('why'):
            raise RuntimeError(hooks['why'])
        # Local loads deliberately resume play. The sleeping probe's PC and
        # locals are stable, so pause through native input before measuring;
        # the old implicit LOADGAME pause is not a saved-state contract.
        command('keys', name, 'shift', 'pause')
        probe.wait_paused(hooks['pid'])
        cmd = compat.commanders(name)
        if not cmd['ok']:
            raise RuntimeError(cmd['why'])
        report['after'] = records(hooks['pid'], 128)
        report['state_preserved'] = report['before'] == report['after']
        command('keys', name, 'shift', 'pause')
        cmd = compat.commanders(name)
        report['commanders'] = cmd
        report['ok'] = report['state_preserved'] and cmd['ok']
    except Exception as exc:
        report.update(ok=False, error=str(exc), traceback=traceback.format_exc())
    finally:
        if game:
            shutil.move(compat.stop_wine(game, keep=True), out / 'raised-wine.log')
        else:
            compat.stop_xvfb(server)
        if inst:
            (out / 'raised-tagpu.log').write_text(talog.run_text(inst['gamedir']))
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        compat.tacli('rm', name, '--force')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--preflight', metavar='INSTANCE')
    parser.add_argument('--gamedir', type=Path)
    parser.add_argument('--dll', type=Path, default=compat.TREE / 'tagpu/ddraw/ddraw.dll')
    parser.add_argument('--stock', type=Path, default=compat.TREE / 'tagpu/ddraw/ddraw-stocklimits.dll')
    parser.add_argument('--load-limit', type=int, help='override only the loading executable configuration')
    parser.add_argument('--reject-saved', action='store_true', help='require refusal when a saved commander script is malformed')
    parser.add_argument('--expanded', action='store_true', help='save and restore all expanded slots with the guarded runtime')
    args = parser.parse_args()
    if args.preflight:
        if not args.gamedir or args.out:
            parser.error('--preflight requires --gamedir and does not use --out')
        preflight(args.preflight, args.gamedir)
        return 0
    if not args.out:
        parser.error('--out is required without --preflight')
    out = args.out.resolve()
    if any(out == root or root in out.parents for root in (compat.TREE, compat.main_checkout())):
        parser.error('output must be outside the repository')
    out.mkdir(parents=True, exist_ok=False)
    report = run(out, args.dll.resolve(), args.stock.resolve(), args.load_limit, args.reject_saved, args.expanded)
    print(json.dumps(report, indent=2))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
