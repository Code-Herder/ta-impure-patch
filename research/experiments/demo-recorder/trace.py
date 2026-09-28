#!/usr/bin/env python3
"""Inspect the temporary DPROBE1 transport observer's output, never a replay file.

The decoder follows vendor/TADR/src/packet_old.pas (the recorder's actual import),
with explicit bounds and checksum failures. System messages contain process-local
pointers and are counted without decoding. See the exploration note for coverage.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
import json
import mmap
from pathlib import Path
import struct

HEADER = struct.Struct('<10I')
SIZES = {
    0x02: 13, 0x03: 7, 0x05: 65, 0x06: 1, 0x07: 1, 0x08: 1,
    0x09: 23, 0x0A: 7, 0x0B: 9, 0x0C: 11, 0x0D: 36, 0x0E: 14,
    0x0F: 6, 0x10: 22, 0x11: 4, 0x12: 5, 0x14: 24, 0x15: 1,
    0x16: 17, 0x17: 2, 0x18: 2, 0x19: 3, 0x1A: 14, 0x1B: 6,
    0x1E: 2, 0x1F: 5, 0x20: 192, 0x21: 10, 0x22: 6, 0x23: 14,
    0x24: 6, 0x26: 41, 0x28: 58, 0x29: 3, 0x2A: 2, 0x2E: 9,
}


@dataclass(frozen=True)
class Record:
    ms: int
    thread: int
    direction: int
    sender: int
    recipient: int
    copy_ticks: int
    qpc: int
    payload: bytes


@dataclass(frozen=True)
class Event:
    ms: int
    direction: int
    sender: int
    recipient: int
    payload: bytes


def read_trace(path):
    """Read a stopped capture; a partially published tail is not accepted as complete."""
    with open(path, 'rb') as source:
        if source.seek(0, 2) < 64:
            raise ValueError('short trace header')
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            if data[:8] != b'DPROBE1\0':
                raise ValueError('not a DPROBE1 trace')
            count, drops = struct.unpack_from('<II', data, 8)
            frequency, = struct.unpack_from('<Q', data, 16)
            if not frequency:
                raise ValueError('zero timer frequency')
            records, pos = [], 64
            while pos + HEADER.size <= len(data):
                size, ms, tid, direction, sender, recipient, length, cost, lo, hi = HEADER.unpack_from(data, pos)
                if not size:
                    break
                if (size < HEADER.size or size % 4 or size > len(data) - pos
                        or length > size - HEADER.size or direction not in (1, 2)):
                    raise ValueError(f'invalid record at {pos}')
                records.append(Record(ms, tid, direction, sender, recipient, cost,
                                      lo | hi << 32, bytes(data[pos + HEADER.size:pos + HEADER.size + length])))
                pos += size
            if len(records) != count:
                raise ValueError('trace changed during read, or has an unpublished record')
    return records, dict(records=count, drops=drops, used_bytes=pos, qpc_hz=frequency)


def decode(payload):
    if len(payload) < 7 or payload[0] not in (3, 4):
        raise ValueError('unsupported transport header')
    if sum(payload[3:-3]) & 0xFFFF != int.from_bytes(payload[1:3], 'little'):
        raise ValueError('transport checksum')
    data = bytearray(payload)
    for i in range(3, len(data) - 3):
        data[i] ^= i & 255
    if data[0] == 4:
        out, pos, ended = data[:3], 3, False
        while pos < len(data) and not ended:
            mask, pos = data[pos], pos + 1
            for bit in range(8):
                if pos == len(data):
                    break
                if mask >> bit & 1:
                    if pos + 2 > len(data):
                        raise ValueError('short compression reference')
                    value = int.from_bytes(data[pos:pos + 2], 'little')
                    pos += 2
                    offset = value >> 4
                    if not offset:
                        ended = True
                        break
                    for j in range((value & 15) + 2):
                        if offset + 2 + j >= len(out):
                            raise ValueError('compression reference beyond output')
                        out.append(out[offset + 2 + j])
                else:
                    out.append(data[pos])
                    pos += 1
                if len(out) > 1024 * 1024:
                    raise ValueError('decompression budget exceeded')
        data = out
    if len(data) < 7:
        raise ValueError('short decompressed header')
    return bytes(data[7:])


def split(data):
    """Only the measured Impure message dialect; reject unknown lengths."""
    pos = 0
    while pos < len(data):
        code = data[pos]
        length = SIZES.get(code)
        if code == 0x2C:
            if pos + 3 > len(data):
                raise ValueError('short unit update length')
            length = int.from_bytes(data[pos + 1:pos + 3], 'little')
            if length < 11:
                raise ValueError('unit update shorter than header')
        if not length or length > len(data) - pos:
            raise ValueError(f'unsupported/truncated message {code:02x} at {pos}')
        yield data[pos:pos + length]
        pos += length


def events(records):
    for record in records:
        if record.sender == 0:
            continue
        # Materialize first: a bad final message must not yield a valid-looking prefix.
        parts = list(split(decode(record.payload)))
        for payload in parts:
            yield Event(record.ms, record.direction, record.sender, record.recipient, payload)


def summary(path):
    records, result = read_trace(path)
    messages = list(events(records))
    counts = Counter(f'{m.direction}:{m.payload[0]:02x}' for m in messages)
    tags = Counter(f'{m.direction}:{m.payload[2]:02x}' for m in messages
                   if m.payload[:2] == b'\x05\0' and m.payload[2] in range(0x49, 0x4D))
    costs = sorted(r.copy_ticks * 1e6 / result['qpc_hz'] for r in records)
    result.update(system_records=sum(r.sender == 0 for r in records), messages=counts,
                  tags=tags, transport_bytes=sum(len(r.payload) for r in records),
                  elapsed_ms=(records[-1].ms - records[0].ms) & 0xFFFFFFFF if records else 0,
                  copy_us={name: costs[min(len(costs)-1, int(len(costs)*p))] if costs else 0
                           for name, p in [('p50', .5), ('p95', .95), ('p99', .99), ('max', 1)]})
    return result


def compare(paths):
    streams = [list(events(read_trace(path)[0])) for path in paths]
    result = []
    for source, target in ((0, 1), (1, 0)):
        for tag in range(0x4A, 0x4D):
            prefix = bytes([5, 0, tag])
            sent = [m.payload for m in streams[source] if m.direction == 1 and m.payload.startswith(prefix)]
            received = [m.payload for m in streams[target] if m.direction == 2 and m.payload.startswith(prefix)]
            result.append(dict(source=source, target=target, tag=f'{tag:02x}', sent=len(sent),
                               received=len(received), ordered_equal=sent == received))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('traces', nargs='+', type=Path)
    args = parser.parse_args()
    result = {'traces': [summary(path) for path in args.traces]}
    if len(args.traces) == 2:
        result['comparison'] = compare(args.traces)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
