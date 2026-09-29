#!/usr/bin/env python3
"""Exercise COB rejection/runtime guards in private Escalation instances.

Generated scripts use a copied mod solar definition/model. Evidence and copied
assets stay outside the repository. A diagnostic refusal, not a crash, is the
required outcome of each deliberate runtime fault.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import time
import uuid

import cob_getter_probe as probe

compat = probe.compat
tacob = probe.cob_audit.tacob
TYPE = 'COBSAFE'


def fixture(archive, case):
    original = tacob.read_cob(archive.read('scripts/armsolar.cob'))
    fbi, changed = re.subn(r'(?im)(\bUnitName\s*=\s*)[^;]+;', rf'\g<1>{TYPE};',
                          archive.read('unitse/armsolar.fbi').decode('latin-1'))
    if changed != 1:
        raise ValueError('expected one solar UnitName')
    op = tacob.OP
    pieces = original.pieces
    scripts = [('Create', 0)]
    if case == 'divide':
        code = [op['PUSH_CONSTANT'], 123, op['PUSH_CONSTANT'], 0, op['DIV'], op['RETURN']]
    elif case == 'piece':
        pieces = pieces + [f'unused{i}' for i in range(4096 - len(pieces))]
        code = [op['SHOW'], 4095, op['PUSH_CONSTANT'], 0, op['RETURN']]
    elif case == 'sweetspot':
        code = [op['PUSH_CONSTANT'], 0, op['RETURN']]
        scripts.append(('SweetSpot', len(code)))
        code += [op['CREATE_LOCAL_VAR'], op['PUSH_CONSTANT'], 0x100000,
                 op['POP_LOCAL_VAR'], 0, op['PUSH_CONSTANT'], 0, op['RETURN']]
    elif case == 'native-ops':
        code = [op['CALL_SCRIPT'], 1, 0, op['PUSH_CONSTANT'], 0, op['RETURN']]
        scripts.append(('Probe980', len(code)))
        code += [op['PUSH_CONSTANT'], 0, op['PUSH_CONSTANT'], 0, 0x10009000, 0,
                 0x1000a000, 0, op['PUSH_CONSTANT'], 9876, op['RETURN']]
    else:
        raise ValueError(case)
    return probe.hpipack.build({'unitse': {TYPE + '.fbi': fbi.encode('latin-1')},
                                'scripts': {TYPE + '.cob': tacob.write_cob(
                                    tacob.Cob(scripts, pieces, 0, code))}})


def run(case, out, dll):
    compat.PREFIX = 'cobsafe-' + uuid.uuid4().hex[:8] + '-'
    setup = compat.pick_setups(['escalation'])[0]
    name = compat.inst_name('escalation')
    server = compat.start_xvfb()
    inst = game = None
    report = dict(case=case, dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest())
    try:
        inst = compat.prepare_wine(setup, dll, server.tacompat_display)
        archive = probe.hpipack.Archive(compat.FIXTURES / 'escalation-gold-10.2.0/TAESC.gp3')
        (inst['gamedir'] / 'cob-safety-probe.ufo').write_bytes(fixture(archive, case))
        (inst['gamedir'] / 'tagpu_cobtrace.on').write_text(TYPE)
        game = compat.start_wine(inst, server.tacompat_display, display_server=server)
        failure_path = inst['gamedir'] / 'log/startup-failure.txt'
        probe.enter_skirmish(name, failure_path)
        hooks = compat.exe_hooks(game['proc'].pid, inst['gamedir'])
        if hooks.get('why') or compat.hook_evidence(hooks):
            raise RuntimeError('unconfirmed Impure-only runtime')
        report['hooks'] = hooks
        if case != 'piece':
            scenario = dict(format='ta-scenario/1', seed=7, on_error='abort',
                            setup=dict(clear_existing=False), units=[
                                dict(id='probe', type=TYPE, owner=1, pos=[2600, 1200]),
                                dict(id='tower', type='ARMLLT', owner=0, pos=[2750, 1200])])
            path = out / 'scenario.json'
            path.write_text(json.dumps(scenario))
            applied = compat.tacli('scenario', 'apply', name, str(path), '--json', timeout=90)
            (out / 'apply.txt').write_text(applied.stdout + applied.stderr)
            if applied.returncode and not failure_path.exists():
                raise RuntimeError('fixture was not applied: ' + applied.stderr)
        expected = {'divide': 'integer division by zero',
                    'sweetspot': 'SweetSpot piece exceeds the posed model allocation',
                    'piece': 'piece index exceeds model allocation'}
        end = time.monotonic() + 45
        while True:
            log = probe.talog.run_text(inst['gamedir'])
            trace = probe.talog.run_text(inst['gamedir'], 'tagpu_cobtrace')
            failure = failure_path.read_text(errors='replace') if failure_path.exists() else ''
            if case == 'piece':
                passed = all(s in log for s in (f'disabled unit {TYPE}', expected[case],
                                                f'cob: entry message: COB: {TYPE}'))
            elif case == 'native-ops':
                passed = 9876 in probe.returns(trace).values()
            else:
                passed = TYPE in failure and expected[case] in failure
            if passed or failure or time.monotonic() >= end:
                break
            time.sleep(.2)
        error = inst['gamedir'] / 'ErrorLog.txt'
        report.update(ok=passed and not error.exists(), refusal=failure,
                      crash=error.read_text(errors='replace') if error.exists() else None)
        if 'INCOMPLETE' in trace:
            report.update(ok=False, error='incomplete trace')
        (out / 'cobtrace.log').write_text(trace)
    except Exception as exc:
        report.update(ok=False, error=str(exc))
    finally:
        if game:
            shutil.move(compat.stop_wine(game, keep=True), out / 'wine.log')
        else:
            compat.stop_xvfb(server)
        if inst:
            (out / 'tagpu.log').write_text(probe.talog.run_text(inst['gamedir']))
            compat.tacli('rm', name, '--force')
        (out / 'result.json').write_text(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cases', nargs='+', choices=('divide', 'piece', 'sweetspot', 'native-ops'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--dll', type=Path, default=compat.TREE / 'tagpu/ddraw/ddraw.dll')
    args = parser.parse_args()
    out = args.out.resolve()
    if any(out == root or root in out.parents for root in (compat.TREE, compat.main_checkout())):
        parser.error('evidence must be outside the repository')
    out.mkdir(parents=True, exist_ok=False)
    ok = True
    for case in args.cases:
        folder = out / case
        folder.mkdir()
        print('Exercising ' + case, flush=True)
        result = run(case, folder, args.dll.resolve())
        print(json.dumps({k: v for k, v in result.items() if k != 'hooks'}), flush=True)
        ok &= result['ok']
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
