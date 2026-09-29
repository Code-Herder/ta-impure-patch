#!/usr/bin/env python3
"""Assert COB ownership semantics on two real Wine/DirectPlay peers of a mod."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import time
import uuid

import cob_getter_probe as probe
import hpipack
import talog

compat = probe.compat
command = probe.command


def peer_script(pieces):
    cob = probe.cob_audit.tacob.read_cob(probe.probe_cob(pieces))
    op = probe.cob_audit.tacob.OP
    cob.code[1] = 45000
    cob.code[27:27] = [op['CALL_SCRIPT'], 9, 0]
    cob.scripts = [(name, pc + 3 if pc else 0) for name, pc in cob.scripts]
    cob.scripts.append(('Probe975', len(cob.code)))
    code = cob.code
    code += [op['CREATE_LOCAL_VAR'], op['PUSH_CONSTANT'], 1, op['POP_LOCAL_VAR'], 0]
    loop = len(code)
    # Skip absent slots and the caller's own owner. The first other owner is
    # a remote commander; the ordinary lobby guarantees it exists on both peers.
    code += [op['PUSH_CONSTANT'], 72, op['PUSH_LOCAL_VAR'], 0]
    code += [op['PUSH_CONSTANT'], 0] * 3 + [op['GET'], op['PUSH_CONSTANT'], 0, op['LESS']]
    branch = len(code)
    code += [op['JUMP_NOT_EQUAL'], 0]
    code += [op['JUMP'], 0]
    owner_check = len(code)
    code[branch + 1] = owner_check
    code += [op['PUSH_CONSTANT'], 72, op['PUSH_LOCAL_VAR'], 0]
    code += [op['PUSH_CONSTANT'], 0] * 3 + [op['GET']]
    code += [op['PUSH_CONSTANT'], 72, op['PUSH_CONSTANT'], 71, op['GET_UNIT_VALUE']]
    code += [op['PUSH_CONSTANT'], 0] * 3 + [op['GET'], op['EQUAL']]
    branch2 = len(code)
    code += [op['JUMP_NOT_EQUAL'], 0]
    increment = len(code)
    code[branch + 3] = increment
    code += [op['PUSH_LOCAL_VAR'], 0, op['PUSH_CONSTANT'], 1, op['ADD'], op['POP_LOCAL_VAR'], 0]
    code += [op['PUSH_LOCAL_VAR'], 0, op['PUSH_CONSTANT'], 70, op['GET_UNIT_VALUE'], op['LESS_EQUAL']]
    end_branch = len(code)
    code += [op['JUMP_NOT_EQUAL'], 0, op['JUMP'], loop]
    code[end_branch + 1] = len(code)
    code += [op['PUSH_CONSTANT'], -99 & 0xffffffff, op['RETURN']]
    code[branch2 + 1] = len(code)
    code += [op['PUSH_CONSTANT'], 75, op['PUSH_LOCAL_VAR'], 0]
    code += [op['PUSH_CONSTANT'], 0] * 3 + [op['GET'], op['RETURN']]
    return probe.cob_audit.tacob.write_cob(cob)


def fixture(setup, malformed=False):
    archive = hpipack.Archive(compat.FIXTURES / 'escalation-gold-10.2.0/TAESC.gp3')
    unit_dir, solar = 'unitse', 'armsolar'
    if setup['name'] == 'tazero-alpha5':
        archive = hpipack.Archive(next(src for dest, src in compat.overlay(setup)
                                      if dest.lower() == 'taz31.gp3'))
        unit_dir, solar = 'zunits', 'armt1solar'
    fbi, count = re.subn(rb'(?im)(\bUnitName\s*=\s*)[^;]+;',
                         rb'\g<1>' + probe.TYPE.encode() + b';', archive.read(f'{unit_dir}/{solar}.fbi'))
    if count != 1:
        raise RuntimeError('probe FBI has no unique unit name')
    original = probe.cob_audit.tacob.read_cob(archive.read(f'scripts/{solar}.cob'))
    defs = {probe.TYPE + '.fbi': fbi, 'COBFBAD.fbi': fbi.replace(probe.TYPE.encode(), b'COBFBAD')}
    op = probe.cob_audit.tacob.OP
    bad = probe.cob_audit.tacob.Cob([('Create', 0)], original.pieces, 0,
               [op['PUSH_LOCAL_VAR'], 128, op['RETURN']] if malformed else
               [op['PUSH_CONSTANT'], 0, op['RETURN']])
    return hpipack.build({'units': defs, unit_dir: defs,
                          'scripts': {probe.TYPE + '.cob': peer_script(original.pieces),
                                      'COBFBAD.cob': probe.cob_audit.tacob.write_cob(bad)}})


def run(out, setup_name, dll, port, quarantine=None):
    setup = compat.pick_setups([setup_name])[0]
    compat.PREFIX = 'cob-peer-' + uuid.uuid4().hex[:6] + '-'
    peers, games, servers = {}, {}, {}
    report = dict(setup=setup_name, dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest())
    try:
        for role, suffix in (('host', '-h'), ('join', '-j')):
            server = servers[role] = compat.start_xvfb()
            inst = peers[role] = compat.prepare_wine(setup, dll, server.tacompat_display, suffix)
            (inst['gamedir'] / 'cob-peer.ufo').write_bytes(fixture(setup, quarantine in ('both', role)))
            (inst['gamedir'] / 'tagpu_cobtrace.on').write_text(probe.TYPE)
            compat.dpport.set_port(inst['prefix'], port)
        held = compat.free_dplay_port(port, {str(i['prefix']) for i in peers.values()})
        if held:
            raise RuntimeError(f'test DirectPlay port is occupied: {held}')
        for role, inst in peers.items():
            games[role] = compat.start_wine(inst, servers[role].tacompat_display, display_server=servers[role])
        for role, inst in peers.items():
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                ui = compat.tacli('ui', inst['name'], timeout=20)
                if ui.returncode == 0 and 'MAINMENU.GUI' in ui.stdout:
                    break
                if games[role]['proc'].poll() is not None:
                    raise RuntimeError(f'{role}: game exited before the main menu')
                time.sleep(.2)
            else:
                raise RuntimeError(f'{role}: main menu was not acknowledged')
        print(f'{setup_name}: entering the two-peer lobby', flush=True)
        report['commanders'] = compat.wine_lobby(peers['host']['name'], peers['join']['name'], port)
        units = {}
        for role, inst in peers.items():
            content = json.loads(command('units', inst['name'], '--json'))
            if not any(u['name'] == probe.TYPE for u in content['units']):
                raise RuntimeError(f'{role}: generated probe type is absent from mod catalogue')
            scenario = dict(format='ta-scenario/1', seed=7, on_error='abort',
                            setup=dict(clear_existing=False), units=[
                                dict(id='probe', type=probe.TYPE, owner=0,
                                     pos=[2600 if role == 'host' else 4200, 1200])])
            path = out / (role + '-scenario.json')
            path.write_text(json.dumps(scenario, indent=2))
            applied = json.loads(command('scenario', 'apply', inst['name'], str(path), '--json', timeout=180))
            (out / (role + '-apply.json')).write_text(json.dumps(applied, indent=2))
            units[role] = applied['units']['probe']['engine_index']
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            rows = {role: probe.returns(talog.run_text(inst['gamedir'], 'tagpu_cobtrace'))
                    for role, inst in peers.items()}
            if all((units[role], 975) in rows[role] for role in peers):
                break
            for role, inst in peers.items():
                if games[role]['proc'].poll() is not None or (inst['gamedir'] / 'log/startup-failure.txt').exists():
                    raise RuntimeError(f'{role}: game stopped during peer assertions')
            time.sleep(.5)
        report['peers'] = {}
        report['quarantine'] = {}
        for role, inst in peers.items():
            hooks = compat.exe_hooks(games[role]['proc'].pid, inst['gamedir'])
            if hooks.get('why'):
                raise RuntimeError(hooks['why'])
            if compat.hook_evidence(hooks):
                raise RuntimeError(f'{role}: TADR hooks ran beside the port')
            tdraw_file = inst['gamedir'] / 'tdrawlog.txt'
            tdraw = tdraw_file.read_text(errors='replace') if tdraw_file.exists() else None
            if compat.tadr_evidence(None if compat._started_only(tdraw) else tdraw,
                                    compat.other_logs(inst['gamedir'] / 'log', 0)):
                raise RuntimeError(f'{role}: TADR execution evidence in logs')
            with open(f"/proc/{hooks['pid']}/mem", 'rb') as mem:
                mem.seek(0x511DE8)
                main = struct.unpack('<I', mem.read(4))[0]
                mem.seek(main + 0x14351)
                slots = struct.unpack('<H', mem.read(2))[0]
                if quarantine:
                    mem.seek(main + 0x1438F)
                    count = struct.unpack('<I', mem.read(4))[0]
                    mem.seek(main + 0x1439B)
                    defs = struct.unpack('<I', mem.read(4))[0]
                    if count > 16384:
                        raise RuntimeError('definition count exceeds its allocation')
                    available = None
                    for index in range(1, count):
                        mem.seek(defs + index * 0x249)
                        data = mem.read(0x249)
                        if data[32:64].split(b'\0')[0] == b'COBFBAD':
                            available = bool(struct.unpack_from('<I', data, 0x241)[0] & 0x800000)
                    report['quarantine'][role] = dict(present=available is not None, available=available)
                    # The native checksum synchronization removes incompatible
                    # types before model/COB loading. Identical malformed files
                    # remain definitions, but must lose their availability bit.
                    if available or (quarantine == 'both' and available is None):
                        raise RuntimeError(f'{role}: rejected peer type remains available')
            checks = probe.judge(rows[role], {units[role]: 0}, slots, 'implemented')
            checks.append(dict(getter=975, expected=0, actual=rows[role].get((units[role], 975)),
                               ok=rows[role].get((units[role], 975)) == 0))
            report['peers'][role] = dict(checks=checks, hooks=hooks)
        report['ok'] = all(c['ok'] for peer in report['peers'].values() for c in peer['checks'])
    except Exception as exc:
        report.update(ok=False, error=str(exc))
    finally:
        for role, server in servers.items():
            if role in games:
                shutil.move(compat.stop_wine(games[role], keep=True), out / (role + '-wine.log'))
            else:
                compat.stop_xvfb(server)
        for role, inst in peers.items():
            for stream in ('tagpu', 'tagpu_cobtrace'):
                (out / (role + '-' + stream + '.log')).write_text(talog.run_text(inst['gamedir'], stream))
            compat.tacli('rm', inst['name'], '--force')
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('setup')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=47881)
    parser.add_argument('--quarantine', choices=('both', 'host', 'join'), help='require consistent exclusion, including mismatched scripts')
    parser.add_argument('--dll', type=Path, default=compat.TREE / 'tagpu/ddraw/ddraw.dll')
    args = parser.parse_args()
    out = args.out.resolve()
    if any(out == root or root in out.parents for root in (compat.TREE, compat.main_checkout())):
        parser.error('output must be outside the repository')
    out.mkdir(parents=True, exist_ok=False)
    report = run(out, args.setup, args.dll.resolve(), args.port, args.quarantine)
    print(json.dumps({k: v for k, v in report.items() if k != 'peers'}, indent=2))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
