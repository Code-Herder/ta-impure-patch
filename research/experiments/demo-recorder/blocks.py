#!/usr/bin/env python3
"""Benchmark independent compressed event blocks on a measured transport trace.

This is a disposable container experiment, NOT the replay format or a world seek.
Install zstandard into the shared project venv. All times below are codec-only.
"""
import argparse
import json
from pathlib import Path
import statistics
import struct
import time
import zlib

import zstandard as zstd
from trace import events, read_trace

HEADER = struct.Struct('<4s6I')
RECORD = struct.Struct('<IIIHH')
LIMIT = 8 * 1024 * 1024


def encode(raw, first, last, count, level):
    if len(raw) > LIMIT:
        raise ValueError('raw block budget')
    compressed = zstd.ZstdCompressor(level=level).compress(raw)
    if len(compressed) > LIMIT:
        raise ValueError('stored block budget')
    header = HEADER.pack(b'DXB1', len(compressed), len(raw), first, last, count, zlib.crc32(raw))
    return header + struct.pack('<I', zlib.crc32(header)) + compressed


def read_block(data, offset):
    if len(data) - offset < 32:
        raise ValueError('short header')
    header = bytes(data[offset:offset + 28])
    if zlib.crc32(header) != struct.unpack_from('<I', data, offset + 28)[0]:
        raise ValueError('header checksum')
    magic, stored, size, first, last, count, checksum = HEADER.unpack(header)
    if magic != b'DXB1' or stored > LIMIT or size > LIMIT or first > last:
        raise ValueError('header bounds')
    end = offset + 32 + stored
    if end > len(data):
        raise ValueError('short body')
    compressed = bytes(data[offset + 32:end])
    try:
        if zstd.frame_content_size(compressed) != size:
            raise ValueError('frame size')
        raw = zstd.ZstdDecompressor().decompress(compressed, max_output_size=size,
                                               allow_extra_data=False)
    except zstd.ZstdError as error:
        raise ValueError('invalid compression') from error
    if len(raw) != size or zlib.crc32(raw) != checksum:
        raise ValueError('body checksum')
    pos, records, previous = 0, [], first
    while pos < len(raw):
        if len(raw) - pos < RECORD.size:
            raise ValueError('short event header')
        ms, sender, recipient, length, direction = RECORD.unpack_from(raw, pos)
        pos += RECORD.size
        if ms < previous or ms > last or direction not in (1, 2) or length > len(raw) - pos:
            raise ValueError('event bounds')
        records.append((ms, sender, recipient, direction, raw[pos:pos + length]))
        pos += length
        previous = ms
    if len(records) != count:
        raise ValueError('event count')
    return end, records


def recover(data):
    """Return the verified prefix; do not skip corruption and invent continuity."""
    offset, blocks = 0, []
    while offset < len(data):
        try:
            end, records = read_block(data, offset)
        except ValueError:
            break
        blocks.append(records)
        offset = end
    return offset, blocks


def make_blocks(messages, interval_ms, level):
    groups = {}
    origin = messages[0].ms
    for m in messages:
        groups.setdefault(((m.ms - origin) & 0xFFFFFFFF) // interval_ms, []).append(m)
    blocks, raws, index = [], [], []
    offset = 0
    for group in groups.values():
        relative = [((m.ms - origin) & 0xFFFFFFFF, m.sender, m.recipient, m.direction, m.payload)
                    for m in group]
        raw = b''.join(RECORD.pack(ms, sender, recipient, len(payload), direction) + payload
                       for ms, sender, recipient, direction, payload in relative)
        block = encode(raw, relative[0][0], relative[-1][0], len(group), level)
        _, decoded = read_block(block, 0)
        if decoded != relative:
            raise AssertionError('event round trip differs')
        index.append((relative[0][0], offset))
        blocks.append(block)
        raws.append((raw, relative[0][0], relative[-1][0], len(group)))
        offset += len(block)
    return blocks, raws, index


def percentile(values, fraction):
    return sorted(values)[min(len(values) - 1, int(len(values) * fraction))]


def benchmark(messages, interval_ms, level):
    blocks, raws, index = make_blocks(messages, interval_ms, level)
    compression, decode_times = [], []
    for _ in range(10):
        start = time.perf_counter_ns()
        for raw, first, last, count in raws:
            encode(raw, first, last, count, level)
        compression.append((time.perf_counter_ns() - start) / 1e6)
    for _ in range(10):
        for block in blocks:
            start = time.perf_counter_ns()
            read_block(block, 0)
            decode_times.append((time.perf_counter_ns() - start) / 1e6)
    return dict(interval_ms=interval_ms, zstd_level=level, events=len(messages), blocks=len(blocks),
                raw_bytes=sum(len(raw) for raw, *_ in raws), file_bytes=sum(map(len, blocks)),
                index_bytes=len(index) * 12, compression_total_ms_median=statistics.median(compression),
                block_read_parse_ms_p50=statistics.median(decode_times),
                block_read_parse_ms_p99=percentile(decode_times, .99),
                block_read_parse_ms_max=max(decode_times), round_trip_equal=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--start-ms', type=int, required=True, help='absolute probe time, inclusive')
    parser.add_argument('--end-ms', type=int, required=True, help='absolute probe time, exclusive')
    args = parser.parse_args()
    if args.end_ms <= args.start_ms:
        parser.error('end must be after start')
    records, info = read_trace(args.trace)
    if info['drops']:
        parser.error('trace has dropped records')
    messages = [m for m in events(records) if args.start_ms <= m.ms < args.end_ms]
    if not messages:
        parser.error('no events in interval')
    # Reservation order and sampling order can differ across transport threads.
    # Wall-time sorting is solely a compression experiment, not a playback scheduler.
    messages.sort(key=lambda m: m.ms)
    result = dict(window_ms=args.end_ms - args.start_ms, event_payload_bytes=sum(len(m.payload) for m in messages),
                  results=[benchmark(messages, interval, level) for interval in (100, 1000, 5000)
                           for level in (1, 3, 6)])
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
