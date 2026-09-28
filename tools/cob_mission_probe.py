#!/usr/bin/env python3
"""Require a clear refusal for a malformed placed unit in a retail campaign."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import time
import uuid

import cob_getter_probe as probe
import talog

compat = probe.compat


def run(out, dll, control=False):
    setup = compat.pick_setups(['retail'])[0]
    compat.PREFIX = 'cob-mission-' + uuid.uuid4().hex[:8] + '-'
    server = compat.start_xvfb()
    inst = game = None
    report = dict(ok=False, control=control, dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest(), screens=[])
    try:
        inst = compat.prepare_wine(setup, dll, server.tacompat_display)
        scripts = inst['gamedir'] / 'scripts'
        scripts.mkdir(exist_ok=True)
        op = probe.cob_audit.tacob.OP
        invalid = probe.cob_audit.tacob.Cob([('Create', 0)], ['base'], 0,
                                         [op['PUSH_LOCAL_VAR'], 128, op['RETURN']])
        if not control:
            (scripts / 'ARMFAV.COB').write_bytes(probe.cob_audit.tacob.write_cob(invalid))
        game = compat.start_wine(inst, server.tacompat_display, display_server=server)
        name = inst['name']
        failure = inst['gamedir'] / 'log/startup-failure.txt'
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            if failure.exists():
                report['refusal'] = failure.read_text(errors='replace')
                report['ok'] = bool(not control and report.get('campaign_selected') and all(
                    s in report['refusal'] for s in ('required or saved unit', 'ARMFAV', 'local index')))
                break
            if game['proc'].poll() is not None:
                raise RuntimeError('campaign exited without its expected refusal')
            ui = compat.tacli('ui', name, timeout=20)
            if ui.returncode:
                time.sleep(.2)
                continue
            screen = ui.stdout
            report['screens'].append(screen)
            if 'MAINMENU.GUI' in screen:
                gadget = 'SINGLE'
            elif 'SINGLE.GUI' in screen:
                gadget = 'NewCamp'
            elif any(gui in screen for gui in ('NEWGAME.GUI', 'NEWCAMP.GUI', 'SELCAMP.GUI')):
                report['campaign_selected'] = True
                gadget = 'Start'
            elif 'SELCAMPX.GUI' in screen:
                report['campaign_selected'] = True
                gadget = 'SELECT'
            elif 'BRIEFX.GUI' in screen:
                gadget = 'BEGIN'
            elif 'BRIEF.GUI' in screen or 'MSNBRIEF.GUI' in screen or 'SELSIDE.GUI' in screen:
                gadget = 'Start'
            elif 'BRIEFING.GUI' in screen:
                gadget = 'OK'
            elif compat.IN_GAME_PANEL.search(screen.splitlines()[0]):
                if control:
                    units = set(re.findall(r'\barmfav\s+own=0 idx=(\d+)', talog.run_text(inst['gamedir'])))
                    if len(units) == 3:
                        report.update(ok=True, placed_jeffys=sorted(map(int, units)))
                        break
                    time.sleep(.2)
                    continue
                raise RuntimeError('campaign entered play with its malformed placed unit')
            else:
                raise RuntimeError('unrecognized campaign screen: ' + screen)
            print('Campaign screen: ' + screen.splitlines()[0] + ' -> ' + gadget, flush=True)
            # A refused load cannot provide tacli with a replacement GUI.
            compat.tacli('ui', name, 'click', gadget, timeout=45)
        else:
            raise RuntimeError('campaign did not acknowledge refusal before the deadline')
    except Exception as exc:
        report.update(ok=False, error=str(exc))
    finally:
        if game:
            shutil.move(compat.stop_wine(game, keep=True), out / 'wine.log')
        else:
            compat.stop_xvfb(server)
        if inst:
            (out / 'tagpu.log').write_text(talog.run_text(inst['gamedir']))
            compat.tacli('rm', inst['name'], '--force')
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--dll', type=Path, default=compat.TREE / 'tagpu/ddraw/ddraw.dll')
    parser.add_argument('--control', action='store_true', help='leave scripts intact; require all three placed Jeffys')
    args = parser.parse_args()
    out = args.out.resolve()
    if any(out == root or root in out.parents for root in (compat.TREE, compat.main_checkout())):
        parser.error('output must be outside the repository')
    out.mkdir(parents=True, exist_ok=False)
    report = run(out, args.dll.resolve(), args.control)
    print(json.dumps({k: v for k, v in report.items() if k != 'screens'}, indent=2))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
