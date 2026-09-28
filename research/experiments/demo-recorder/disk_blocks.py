#!/usr/bin/env python3
"""Disposable indexed disk container; payloads are opaque research state samples.

This measures IO and verified-prefix recovery, not playable game recovery. A fast
open validates the index; each accessed block is independently verified. Only a
full recovery scan verifies the entire prefix. No shipping format is defined here.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import struct
import time
import zlib

import zstandard as zstd

LIMIT = 8 * 1024 * 1024
FILE = struct.Struct('<8sII')
BLOCK = struct.Struct('<4sIIIQI')
FOOTER = struct.Struct('<8sQII')
ENTRY = struct.Struct('<QQ')
DATA, INDEX = 1, 2


def checked_header(prefix):
    return prefix + struct.pack('<I', zlib.crc32(prefix))


def file_header():
    return checked_header(struct.pack('<8sI', b'DRIO001\0', 1))


def block(raw, sequence, kind=DATA):
    if not raw or len(raw) > LIMIT or kind not in (DATA, INDEX):
        raise ValueError('block input')
    packed = zstd.ZstdCompressor(level=3).compress(raw)
    if len(packed) > LIMIT:
        raise ValueError('stored budget')
    return checked_header(BLOCK.pack(b'DISK', len(packed), len(raw),
                                     zlib.crc32(raw), sequence, kind)) + packed


def exact(f, length):
    data = f.read(length)
    if len(data) != length:
        raise ValueError('short read')
    return data


def check_file(f):
    f.seek(0)
    if exact(f, FILE.size) != file_header():
        raise ValueError('file header')


def read_block(f, offset, boundary):
    if not FILE.size <= offset <= boundary - BLOCK.size - 4:
        raise ValueError('block offset')
    f.seek(offset)
    header = exact(f, BLOCK.size)
    checksum, = struct.unpack('<I', exact(f, 4))
    if checksum != zlib.crc32(header):
        raise ValueError('header checksum')
    magic, stored, size, crc, sequence, kind = BLOCK.unpack(header)
    if (magic != b'DISK' or not 0 < stored <= LIMIT or not 0 < size <= LIMIT
            or kind not in (DATA, INDEX)):
        raise ValueError('block bounds')
    end = offset + BLOCK.size + 4 + stored
    if end > boundary:
        raise ValueError('body bounds')
    payload = exact(f, stored)
    try:
        if zstd.frame_content_size(payload) != size:
            raise ValueError('frame size')
        raw = zstd.ZstdDecompressor().decompress(payload, max_output_size=size,
                                               allow_extra_data=False)
    except zstd.ZstdError as error:
        raise ValueError('compression') from error
    if len(raw) != size or zlib.crc32(raw) != crc:
        raise ValueError('body checksum')
    return end, sequence, kind, raw


def index_tail(entries, offset):
    raw = b''.join(ENTRY.pack(*entry) for entry in entries)
    encoded = block(raw, len(entries), INDEX)
    footer = checked_header(struct.pack('<8sQI', b'DRIOEND\0', offset, len(entries)))
    return encoded, footer


def open_index(f):
    """Validate bounded metadata only; this does not verify untouched data blocks."""
    check_file(f)
    size = os.fstat(f.fileno()).st_size
    if size < FILE.size + FOOTER.size:
        raise ValueError('no footer')
    f.seek(size - FOOTER.size)
    footer = exact(f, FOOTER.size)
    magic, offset, count, crc = FOOTER.unpack(footer)
    if (magic != b'DRIOEND\0' or crc != zlib.crc32(footer[:-4])
            or not 0 < count <= LIMIT // ENTRY.size):
        raise ValueError('footer bounds')
    end, sequence, kind, raw = read_block(f, offset, size - FOOTER.size)
    if (end != size - FOOTER.size or kind != INDEX or sequence != count
            or len(raw) != count * ENTRY.size):
        raise ValueError('index shape')
    entries = list(ENTRY.iter_unpack(raw))
    previous = FILE.size - 1
    for i, (seq, pos) in enumerate(entries):
        if seq != i or not previous < pos < offset or (i == 0 and pos != FILE.size):
            raise ValueError('index ordering')
        previous = pos
    return entries, offset


def indexed_read(f, entries, index_offset, sequence):
    if not 0 <= sequence < len(entries):
        raise ValueError('sequence range')
    boundary = entries[sequence + 1][1] if sequence + 1 < len(entries) else index_offset
    end, seq, kind, raw = read_block(f, entries[sequence][1], boundary)
    if end != boundary or seq != sequence or kind != DATA:
        raise ValueError('indexed block identity')
    return raw


def recover(f):
    """Scan contiguous verified blocks; corruption never permits skipping ahead."""
    try:
        check_file(f)
    except ValueError:
        return dict(blocks=0, verified_end=0, complete=False, sha256=None)
    size = os.fstat(f.fileno()).st_size
    offset, entries, digest = FILE.size, [], hashlib.sha256()
    while offset < size:
        try:
            end, seq, kind, raw = read_block(f, offset, size)
            if kind != DATA or seq != len(entries):
                break
        except ValueError:
            break
        entries.append((seq, offset))
        digest.update(raw)
        offset = end
    try:
        index, index_offset = open_index(f)
        complete = index == entries and index_offset == offset
    except ValueError:
        complete = False
    return dict(blocks=len(entries), verified_end=offset, complete=complete,
                sha256=digest.hexdigest())


def write_all(fd, data):
    view = memoryview(data)
    while view:
        n = os.write(fd, view)
        if n <= 0:
            raise OSError('write made no progress')
        view = view[n:]


def write_file(path, raw_blocks, repetitions):
    entries, offset = [], FILE.size
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    started = time.perf_counter_ns()
    try:
        write_all(fd, file_header())
        for i in range(repetitions * len(raw_blocks)):
            encoded = block(raw_blocks[i % len(raw_blocks)], i)
            entries.append((i, offset))
            write_all(fd, encoded)
            offset += len(encoded)
        index, footer = index_tail(entries, offset)
        write_all(fd, index)
        before_sync = time.perf_counter_ns()
        os.fsync(fd)
        after_sync = time.perf_counter_ns()
        write_all(fd, footer)
        os.fsync(fd)
        ended = time.perf_counter_ns()
    finally:
        os.close(fd)
    # The directory entry is part of the local POSIX durability experiment.
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return dict(blocks=len(entries), bytes=path.stat().st_size,
                encode_write_ms=(before_sync - started) / 1e6,
                data_index_fsync_ms=(after_sync - before_sync) / 1e6,
                footer_write_fsync_ms=(ended - after_sync) / 1e6)


def timed(call):
    start = time.perf_counter_ns()
    value = call()
    return (time.perf_counter_ns() - start) / 1e6, value


def distribution(values):
    values = sorted(values)
    return dict(n=len(values), median_ms=statistics.median(values),
                p99_ms=values[min(len(values)-1, int(len(values)*.99))],
                max_ms=values[-1])


def benchmark(path, raw_blocks, repetitions):
    written = write_file(path, raw_blocks, repetitions)
    opens, reads, evicted = [], [], []
    with path.open('rb', buffering=0) as f:
        for _ in range(25):
            elapsed, (entries, index_offset) = timed(lambda: open_index(f))
            opens.append(elapsed)
        targets = [0, len(entries)-1, len(entries)//9, len(entries)//2] * 25
        for sequence in targets:
            elapsed, raw = timed(lambda: indexed_read(f, entries, index_offset, sequence))
            if raw != raw_blocks[sequence % len(raw_blocks)]:
                raise AssertionError('indexed state differs')
            reads.append(elapsed)
        if hasattr(os, 'posix_fadvise'):
            for sequence in targets[:20]:
                os.posix_fadvise(f.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                elapsed, raw = timed(lambda: indexed_read(f, entries, index_offset, sequence))
                if raw != raw_blocks[sequence % len(raw_blocks)]:
                    raise AssertionError('advised state differs')
                evicted.append(elapsed)
        elapsed, recovered = timed(lambda: recover(f))
    expected = hashlib.sha256()
    for i in range(len(entries)):
        expected.update(raw_blocks[i % len(raw_blocks)])
    if not recovered['complete'] or recovered['sha256'] != expected.hexdigest():
        raise AssertionError('full scan differs')
    return dict(write=written, metadata_open=distribution(opens),
                cached_indexed_read=distribution(reads),
                advisory_eviction_read=distribution(evicted) if evicted else None,
                verified_full_scan_ms=elapsed, recovered=recovered)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('frames', type=Path)
    parser.add_argument('output', type=Path, help='new file, never overwritten')
    parser.add_argument('--repetitions', type=int, default=270)
    args = parser.parse_args()
    paths = sorted(args.frames.glob('*.bin'))
    if not paths or not 1 <= args.repetitions <= 1000:
        parser.error('frames and 1..1000 repetitions required')
    frames = []
    for path in paths:
        if not 0 < path.stat().st_size <= LIMIT:
            parser.error('frame budget')
        frames.append(path.read_bytes())
    raw = [b''.join(struct.pack('<I', len(f)) + f for f in frames[i:i+10])
           for i in range(0, len(frames), 10)]
    print(json.dumps(benchmark(args.output, raw, args.repetitions), indent=2))


if __name__ == '__main__':
    main()
