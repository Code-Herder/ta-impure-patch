#!/usr/bin/env python3
"""Load a save made by the native 32-word COB serializer with the raised runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import time
import traceback
import uuid

import cob_getter_probe as probe
import talog

compat = probe.compat
command = probe.command


def records(pid, words):
    with open(f'/proc/{pid}/mem', 'rb') as mem:
        def read(address, size):
            mem.seek(address)
            data = mem.read(size)
            if len(data) != size:
                raise RuntimeError('short memory read')
            return data

        def word(address):
            return struct.unpack('<I', read(address, 4))[0]

        main = word(0x511DE8)
        if read(main + 0x38A51, 1) != b'\1':
            raise RuntimeError('script observation requires pause acknowledgement')
        base = word(main + 0x14357)
        slots = struct.unpack('<H', read(main + 0x14351, 2))[0]
        result = {}
        defs = word(main + 0x1439B)
        count = word(main + 0x1438F)
        pool = read(base, slots * 0x118)
        for index in range(1, slots):
            unit = pool[index * 0x118:(index + 1) * 0x118]
            if not struct.unpack_from('<I', unit, 0x110)[0] & 0x10000000:
                continue
            typ = struct.unpack_from('<H', unit, 0xA6)[0]
            if typ >= count:
                raise RuntimeError('live unit type exceeds definition count')
            if not word(defs + typ * 0x249 + 0x245) & 0x40000:
                continue
            owner = unit[0xFF]
            if owner in result:
                raise RuntimeError('ambiguous duplicate commander')
            cob = struct.unpack_from('<I', unit, 0x9A)[0]
            if not cob:
                raise RuntimeError(f'live commander {index}, owner {owner}, has no script')
            state = []
            for slot in range(8):
                data = list(struct.unpack(f'<{words + 9}I', read(cob + 28 + slot * (36 + 4 * words), 36 + 4 * words)))
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
            raise RuntimeError(f'expected human and AI commanders, found owners {list(result)}')
        return result


def run(out, dll, stock, load_limit=None, reject_saved=False):
    setup = compat.pick_setups(['retail'])[0]
    compat.PREFIX = 'cob-old-' + uuid.uuid4().hex[:8] + '-'
    name = compat.inst_name('retail')
    server = compat.start_xvfb()
    inst = game = None
    report = dict(instance=name, dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest(),
                  native_serializer_dll_sha256=hashlib.sha256(stock.read_bytes()).hexdigest())
    try:
        inst = compat.prepare_wine(setup, stock, server.tacompat_display)
        game = compat.start_wine(inst, server.tacompat_display, display_server=server)
        print('Creating a retail save with native COB serialization', flush=True)
        probe.enter_skirmish(name, inst['gamedir'] / 'log/startup-failure.txt')
        cmd = compat.commanders(name)
        if not cmd['ok']:
            raise RuntimeError(cmd['why'])
        command('keys', name, 'shift', 'pause')
        hooks = compat.exe_hooks(game['proc'].pid, inst['gamedir'])
        if hooks.get('why'):
            raise RuntimeError(hooks['why'])
        probe.wait_paused(hooks['pid'])
        report['before'] = records(hooks['pid'], 32)
        if not any(report['before'].values()):
            raise RuntimeError('native save has no active COB state to exercise')
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
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--dll', type=Path, default=compat.TREE / 'tagpu/ddraw/ddraw.dll')
    parser.add_argument('--stock', type=Path, default=compat.TREE / 'tagpu/ddraw/ddraw-stocklimits.dll')
    parser.add_argument('--load-limit', type=int, help='override only the loading executable configuration')
    parser.add_argument('--reject-saved', action='store_true', help='require refusal when a saved commander script is malformed')
    args = parser.parse_args()
    out = args.out.resolve()
    if any(out == root or root in out.parents for root in (compat.TREE, compat.main_checkout())):
        parser.error('output must be outside the repository')
    out.mkdir(parents=True, exist_ok=False)
    report = run(out, args.dll.resolve(), args.stock.resolve(), args.load_limit, args.reject_saved)
    print(json.dumps(report, indent=2))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
