#!/usr/bin/env python3
"""Measure independent blocks of local scene-probe packets; not a replay decoder.

The input is the private 32-bit TAGPU_PACKET layout measured at a044167. The
unit/piece projection zeros two runtime keys solely to compare compression. It
does not resolve asset handles or make the remaining fields a portable format.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import struct
import time

import zstandard as zstd


def projection(data):
    if len(data) < 2312 or len(data) > 8 * 1024 * 1024:
        raise ValueError('packet size')
    tick = struct.unpack_from('<I', data, 16)[0]
    nu, ou, np, op = struct.unpack_from('<4I', data, 1608)
    if struct.unpack_from('<I', data, 2300)[0]:
        raise ValueError('truncated packet')
    parts = []
    for count, offset, stride in ((nu, ou, 100), (np, op, 24)):
        if count and (offset < 2312 or offset + count * stride > len(data)):
            raise ValueError('table bounds')
        part = bytearray(data[offset:offset + count * stride])
        for i in range(count):
            part[i * stride + 20:i * stride + 24] = b'\0' * 4
        parts.append(part)
    return struct.pack('<3I', tick, nu, np) + b''.join(parts)


def benchmark(frames, group_size):
    raw = [b''.join(struct.pack('<I', len(f)) + f for f in frames[i:i + group_size])
           for i in range(0, len(frames), group_size)]
    compressor = zstd.ZstdCompressor(level=3)
    decoder = zstd.ZstdDecompressor()
    blocks = [compressor.compress(r) for r in raw]
    for r, b in zip(raw, blocks):
        if decoder.decompress(b, max_output_size=len(r), allow_extra_data=False) != r:
            raise AssertionError('round trip')
    encode, decode = [], []
    for _ in range(25):
        for r, b in zip(raw, blocks):
            start = time.perf_counter_ns()
            compressor.compress(r)
            encode.append((time.perf_counter_ns() - start) / 1e6)
            start = time.perf_counter_ns()
            decoder.decompress(b, max_output_size=len(r), allow_extra_data=False)
            decode.append((time.perf_counter_ns() - start) / 1e6)
    return dict(frames_per_block=group_size, blocks=len(blocks),
                raw_bytes=sum(map(len, raw)), compressed_bytes=sum(map(len, blocks)),
                largest_raw_block=max(map(len, raw)),
                encode_ms_median=statistics.median(encode), encode_ms_max=max(encode),
                decode_ms_median=statistics.median(decode), decode_ms_max=max(decode),
                exact_round_trip=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    paths = sorted(args.directory.glob('*.bin'))
    if not paths:
        parser.error('no packets')
    frames = [p.read_bytes() for p in paths]
    projected = [projection(f) for f in frames]
    print(json.dumps(dict(frames=len(frames),
                          input_sha256=hashlib.sha256(b''.join(frames)).hexdigest(),
                          full_packet=[benchmark(frames, n) for n in (1, 10, 50, 100)],
                          unit_piece_projection=[benchmark(projected, n) for n in (1, 10, 50, 100)]),
                     indent=2))


if __name__ == '__main__':
    main()
