#!/usr/bin/env python3
"""Kill an owned writer at acknowledged IO boundaries, then verify its disk prefix.

POSIX or Win32-under-Wine process-crash experiment. The OS remains running; this
cannot establish power-loss safety, native Windows durability, or game rendering.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys

from disk_blocks import (BLOCK, FILE, FOOTER, block, file_header, index_tail,
                         recover, write_all)


def fixture(raws):
    image = bytearray(file_header())
    phases = [('partial_file_header', 8, False), ('file_header', len(image), False)]
    entries, ends, hashes = [], [], []
    digest = hashlib.sha256()
    for i, raw in enumerate(raws):
        offset = len(image)
        encoded = block(raw, i)
        entries.append((i, offset))
        image += encoded
        phases.extend([(f'block_{i}_partial_header', offset + 7, False),
                       (f'block_{i}_header', offset + BLOCK.size + 4, False),
                       (f'block_{i}_partial_body', offset + (len(encoded)+32)//2, False),
                       (f'block_{i}_complete', len(image), False)])
        ends.append(len(image))
        digest.update(raw)
        hashes.append(digest.hexdigest())
    offset = len(image)
    index, footer = index_tail(entries, offset)
    image += index
    phases.extend([('partial_index', offset + len(index)//2, False),
                   ('index_complete', len(image), False),
                   ('data_index_synced', len(image), True)])
    image += footer
    phases.extend([('partial_footer', len(image)-FOOTER.size//2, False),
                   ('footer_complete', len(image), False),
                   ('footer_synced', len(image), True)])
    return bytes(image), phases, ends, hashes


def worker(directory, target):
    image = (directory/'fixture.bin').read_bytes()
    phases = json.loads((directory/'phases.json').read_text())
    fd = os.open(target, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    offset = 0
    try:
        for name, end, sync in phases:
            write_all(fd, image[offset:end])
            offset = end
            if sync:
                os.fsync(fd)
            print(name, flush=True)
            # The parent only kills after this acknowledgement; no later write
            # can race the observation because it requires a parent token.
            if sys.stdin.buffer.read(1) != b'+':
                return
    finally:
        os.close(fd)


def experiment(raws, directory, windows_exe=None, prefix=None):
    image, phases, ends, hashes = fixture(raws)
    (directory/'fixture.bin').write_bytes(image)
    (directory/'phases.json').write_text(json.dumps(phases))
    (directory/'phases.txt').write_text(''.join(f'{n} {end} {int(sync)}\n'
                                             for n, end, sync in phases))
    results = []
    for stop, (name, length, synced) in enumerate(phases):
        target = directory/f'crash-{stop}.bin'
        command = [sys.executable, str(Path(__file__).resolve()),
                   '--worker', str(directory), str(target)]
        environment = None
        if windows_exe:
            def winpath(path):
                return 'Z:' + str(path.resolve()).replace('/', '\\')
            command = ['wine', str(windows_exe.resolve()),
                       winpath(directory/'fixture.bin'), winpath(directory/'phases.txt'),
                       winpath(target)]
            environment = dict(os.environ, WINEPREFIX=str(prefix.resolve()), WINEDEBUG='-all')
        child = subprocess.Popen(command, env=environment,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        try:
            selector = selectors.DefaultSelector()
            selector.register(child.stdout, selectors.EVENT_READ)
            try:
                for i in range(stop+1):
                    if not selector.select(10):
                        raise RuntimeError('writer acknowledgement timeout')
                    ack = child.stdout.readline().decode().strip()
                    if ack != phases[i][0]:
                        raise RuntimeError(f'writer acknowledgement {ack!r}')
                    if i != stop:
                        child.stdin.write(b'+')
                        child.stdin.flush()
            finally:
                selector.close()
            if windows_exe:
                child.stdin.write(b'K')
                child.stdin.flush()
            else:
                child.kill()
            child.wait(timeout=10)
            if child.returncode != (99 if windows_exe else -9):
                raise AssertionError('writer was not killed')
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
            child.stdin.close()
            child.stdout.close()
            child.stderr.close()
        with target.open('rb', buffering=0) as f:
            actual = recover(f)
        count = sum(end <= length for end in ends)
        expected_end = ends[count-1] if count else (FILE.size if length >= FILE.size else 0)
        expected_hash = hashes[count-1] if count else (
            hashlib.sha256().hexdigest() if length >= FILE.size else None)
        expected = dict(blocks=count, verified_end=expected_end,
                        complete=length == len(image), sha256=expected_hash)
        if actual != expected or target.stat().st_size != length:
            raise AssertionError((name, actual, expected))
        results.append(dict(phase=name, file_bytes=length, recovered=actual,
                            killed_after_ack=True, exact_prefix=True,
                            worker='Win32-under-Wine' if windows_exe else 'POSIX'))
    return results


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--worker':
        worker(Path(sys.argv[2]), Path(sys.argv[3]))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('frames', type=Path)
    parser.add_argument('directory', type=Path, help='new private evidence directory')
    parser.add_argument('--windows-exe', type=Path)
    parser.add_argument('--prefix', type=Path, help='owned idle Wine prefix with Z: mapping to /')
    args = parser.parse_args()
    paths = sorted(args.frames.glob('*.bin'))[:3]
    if len(paths) != 3:
        parser.error('at least three frames required')
    if bool(args.windows_exe) != bool(args.prefix):
        parser.error('--windows-exe and --prefix must be supplied together')
    if args.prefix and (args.prefix/'dosdevices/z:').resolve() != Path('/'):
        parser.error('prefix Z: mapping must resolve to /')
    args.directory.mkdir(exist_ok=False)
    print(json.dumps(experiment([p.read_bytes() for p in paths], args.directory,
                               args.windows_exe, args.prefix), indent=2))


if __name__ == '__main__':
    main()
