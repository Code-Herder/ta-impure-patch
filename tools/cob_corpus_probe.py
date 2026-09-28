#!/usr/bin/env python3
"""Audit the effective COBs the native loader selected in an isolated mod game.

The loader, not an assumed archive ordering, resolves overrides. Only metadata,
hashes and analysis results are saved; the in-memory scripts are not written.
Evidence must live outside the repository. This does not prove dynamic occupancy.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import uuid

import cob_getter_probe as probe
import talog

compat = probe.compat
tacob = probe.cob_audit.tacob


def loaded_cob(read, address):
    header = struct.unpack('<11I', read(address, 44))
    version, nscripts, npieces, words, statics = header[:5]
    if version != 4 or max(nscripts, npieces, statics) > 4096 or words > 4 * 1024 * 1024:
        raise ValueError('unbounded or unsupported loaded COB header')

    def table(pointer, count):
        if count and not address <= pointer <= address + 16 * 1024 * 1024 - count * 4:
            raise ValueError('loaded COB table outside parser allocation ceiling')
        return struct.unpack(f'<{count}I', read(pointer, count * 4)) if count else ()

    def name(pointer):
        if not address <= pointer < address + 16 * 1024 * 1024:
            raise ValueError('loaded COB name outside parser allocation ceiling')
        # Read up to the terminator, not past the object's last string.
        result = bytearray()
        for offset in range(256):
            if pointer + offset >= address + 16 * 1024 * 1024:
                raise ValueError('loaded COB name exceeds parser allocation ceiling')
            char = read(pointer + offset, 1)
            if char == b'\0':
                return result.decode('latin-1')
            result.extend(char)
        raise ValueError('unterminated loaded COB name')

    entries = table(header[6], nscripts)
    scripts = [name(p) for p in table(header[7], nscripts)]
    pieces = [name(p) for p in table(header[8], npieces)]
    return tacob.Cob(zip(scripts, entries), pieces, statics, table(header[9], words))


def snapshot(pid):
    # The driver owns this game and issues no load/reload/exit until the capture
    # ends. Paused play retains all unit definitions and loaded script blobs.
    with open(f'/proc/{pid}/mem', 'rb') as memory:
        def read(address, size):
            memory.seek(address)
            data = memory.read(size)
            if len(data) != size:
                raise RuntimeError('short engine observation')
            return data

        def word(address):
            return struct.unpack('<I', read(address, 4))[0]

        main = word(0x511DE8)
        if read(main + 0x38A51, 1) != b'\x01':
            raise RuntimeError('effective corpus capture requires acknowledged pause')
        count, base = word(main + 0x1438F), word(main + 0x1439B)
        if not 0 < count <= 16384:
            raise RuntimeError('unit definition count outside allocation')
        units, programs = [], {}
        for index in range(1, count):
            data = read(base + index * 0x249, 0x249)
            name = data[32:64].split(b'\0')[0].decode('latin-1')
            pointer = struct.unpack_from('<I', data, 0x18E)[0]
            row = dict(index=index, type=name, scriptless=not pointer,
                       available=bool(struct.unpack_from('<I', data, 0x241)[0] & 0x800000))
            if pointer:
                cob = loaded_cob(read, pointer)
                blob = tacob.write_cob(cob)
                digest = hashlib.sha256(blob).hexdigest()
                row['canonical_sha256'] = digest
                if digest not in programs:
                    programs[digest] = probe.cob_audit.audit_blob(blob)
            units.append(row)
        return dict(units=units, programs=programs)


def run(setup_name, out, dll):
    setup = compat.pick_setups([setup_name])[0]
    compat.PREFIX = 'cob-corpus-' + uuid.uuid4().hex[:8] + '-'
    name = compat.inst_name(setup_name)
    server = compat.start_xvfb()
    game = inst = None
    report = dict(setup=setup_name, dll_sha256=hashlib.sha256(dll.read_bytes()).hexdigest())
    try:
        inst = compat.prepare_wine(setup, dll, server.tacompat_display)
        game = compat.start_wine(inst, server.tacompat_display, display_server=server)
        probe.enter_skirmish(name, inst['gamedir'] / 'log/startup-failure.txt')
        probe.command('keys', name, 'shift', 'pause')
        hooks = compat.exe_hooks(game['proc'].pid, inst['gamedir'])
        if hooks.get('why') or compat.hook_evidence(hooks):
            raise RuntimeError('effective corpus requires confirmed Impure-only runtime')
        probe.wait_paused(hooks['pid'])
        corpus = snapshot(hooks['pid'])
        (out / 'corpus.json').write_text(json.dumps(corpus, indent=2))
        log = talog.run_text(inst['gamedir'])
        rejected = [line for line in log.splitlines() if 'cob: disabled unit' in line]
        report.update(definitions=len(corpus['units']), programs=len(corpus['programs']),
                      scriptless=[u['type'] for u in corpus['units'] if u['scriptless']],
                      peak_words=max((p['peak_words'] for p in corpus['programs'].values()), default=0),
                      rejected=rejected, hooks=hooks, ok=not rejected)
    except Exception as exc:
        report.update(ok=False, error=str(exc))
    finally:
        if game:
            shutil.move(compat.stop_wine(game, keep=True), out / 'wine.log')
        else:
            compat.stop_xvfb(server)
        if inst:
            (out / 'tagpu.log').write_text(talog.run_text(inst['gamedir']))
            compat.tacli('rm', name, '--force')
        (out / 'result.json').write_text(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('setups', nargs='+')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--dll', type=Path, default=compat.TREE / 'tagpu/ddraw/ddraw.dll')
    args = parser.parse_args()
    out = args.out.resolve()
    if any(out == root or root in out.parents for root in (compat.TREE, compat.main_checkout())):
        parser.error('evidence must be outside the repository')
    out.mkdir(parents=True, exist_ok=False)
    ok = True
    for setup in args.setups:
        folder = out / setup
        folder.mkdir()
        print(f'Auditing loaded {setup} scripts', flush=True)
        result = run(setup, folder, args.dll.resolve())
        print(json.dumps({k: v for k, v in result.items() if k != 'hooks'}), flush=True)
        ok &= result['ok']
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
