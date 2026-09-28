#!/usr/bin/env python3
"""Headless two-player packet replay prototype using an isolated native DirectPlay bridge.

This is a research backend, not a shipping recorder. Input is one stopped DPROBE1 trace.
It borrows the handshake from TADR Server/tasv.pas and lobby.pas, preserving binary gameplay.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import threading
import time

from trace import read_trace, events, decode, split


def wire(payload):
    data = bytearray(b'\x03\0\0\xff\xff\xff\xff' + payload)
    for i in range(3, len(data) - 3):
        data[i] ^= i & 255
    struct.pack_into('<H', data, 1, sum(data[3:-3]) & 65535)
    return data


class ReplayHost:
    def __init__(self, args):
        records, info = read_trace(args.trace)
        if info['drops']:
            raise ValueError('capture has drops')
        self.order_file = args.order_file
        self.source = list(events(records))
        self.original = sorted({m.sender for m in self.source})
        if len(self.original) != 2:
            raise ValueError('prototype requires exactly two recorded players')
        self.start = next(m.ms for m in self.source if m.payload[:3] == b'\x05\0J')
        self.status = [bytearray([m.payload for m in self.source
                                 if m.sender == pid and m.ms < self.start and m.payload[0] == 0x20][-1])
                       for pid in self.original]
        self.unit_limit = struct.unpack_from('<H', self.status[0], 166)[0]
        self.units = {}
        for m in self.source:
            if m.sender == self.original[0] and m.payload[:2] == b'\x1a\x03':
                self.units[struct.unpack_from('<I', m.payload, 6)[0]] = m.payload
        self.game = [m for m in self.source if m.ms >= self.start and m.payload[0] not in
                     (2, 6, 7, 8, 0x15, 0x17, 0x18, 0x1a, 0x1e, 0x20, 0x21, 0x22, 0x26, 0x2a)]
        # Only wall-time paced prototype playback. No seeking/clock-fidelity claim.
        self.game.sort(key=lambda m: m.ms)
        self.viewer = 0
        self.phase = 'lobby'
        self.synced = False
        self.unit_sent = False
        self.ready = False
        self.acked = set()
        self.received = 0
        self.cursor = 0
        self.rx_counts = Counter()
        self.log = args.log.open('w')
        self.rx = queue.Queue(maxsize=10000)
        self.io_error = []
        env = dict(os.environ, WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG='-all',
                   WINEDLLOVERRIDES='dplayx,dpmodemx,dpnet,dpwsockx,dplaysvr.exe,dpnsvr.exe=n;dpnhpast,dpnhupnp=d',
                   DISPLAY=args.display)
        self.stderr = args.log.with_suffix('.wine.txt').open('w')
        self.process = subprocess.Popen(['wine', str(args.bridge.resolve())], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.stderr, env=env,
                                        text=True, bufsize=1)
        self.reader = threading.Thread(target=self.read_output, daemon=True)
        self.reader.start()
        self.drones = []
        self.send_count = 0
        self.tick = time.monotonic()
        self.deadline = self.tick + args.timeout
        self.speed = args.speed
        self.last_periodic = 0
        self.play_start = 0
        self.eof = False

    def note(self, kind, **data):
        event = dict(elapsed=round(time.monotonic() - self.tick, 3), kind=kind, **data)
        self.log.write(json.dumps(event) + '\n')
        self.log.flush()
        if kind != 'rx':
            print(json.dumps(event), flush=True)

    def read_output(self):
        try:
            for line in self.process.stdout:
                self.rx.put(line.strip(), timeout=5)
        except Exception as error:
            self.io_error.append(repr(error))
        finally:
            try:
                self.rx.put(None, timeout=5)
            except queue.Full:
                self.io_error.append('output queue full at exit')

    def command(self, text):
        self.process.stdin.write(text + '\n')
        self.process.stdin.flush()

    def send(self, index, payload, recipient=None):
        if not self.viewer:
            return
        self.command(f'SEND {self.drones[index]} {self.viewer if recipient is None else recipient} {wire(payload).hex()}')
        self.send_count += 1

    def identity(self):
        self.send(0, b'\x26' + struct.pack('<10I', *self.drones, self.viewer, *([0] * 7)))
        for i, pid in enumerate([*self.drones, self.viewer]):
            self.send(0, b'\x22' + struct.pack('<IB', pid, i + 1))
        for i, pid in enumerate(self.drones):
            st = self.status[i].copy()
            st[1:140] = self.status[0][1:140]
            struct.pack_into('<I', st, 145, pid)
            struct.pack_into('<I', st, 187, pid)
            st[156] = (st[156] | 0xA0) & ~0x40
            st[181] = 0
            self.send(i, st)

    def unit_sync(self):
        if self.unit_sent:
            return
        self.send(0, b'\x18\x02')
        self.send(0, b'\x1a' + bytes(13))
        rows = []
        for key in self.units:
            rows.append(b'\x1a\x03' + bytes(4) + struct.pack('<IHH', key, 1, 65535))
        for i in range(0, len(rows), 32):
            self.send(0, b''.join(rows[i:i + 32]))
        self.unit_sent = True
        self.note('unit-list', types=len(rows))

    def unit_enable(self):
        rows = list(self.units.values())
        for i in range(0, len(rows), 32):
            self.send(0, b''.join(rows[i:i + 32]))
        self.synced = True
        self.note('unit-sync-complete', acknowledged=len(self.acked))

    def launch(self):
        self.identity()
        # Pascal's launch changes the third byte of dwUser1 to ASCII '2'.
        user1 = (1753284736 & ~0xFF0000) | (ord('2') << 16)
        user4 = (16974324 & ~65535) | self.unit_limit
        self.command(f'STATE {user1} 4 655370 {user4}')
        for i in range(2):
            self.send(i, b'\x08')
        self.phase = 'loading'
        self.note('launch')

    def receive(self, sender, recipient, data):
        if sender == 0:
            # Discover the viewer from its application traffic, not from queued
            # create/destroy notifications for the bridge's temporary players.
            return
        if sender in self.drones:
            return
        if self.viewer and sender != self.viewer:
            raise ValueError('unexpected additional viewer')
        if not self.viewer:
            self.note('viewer-identity-check', sender=sender, drones=self.drones)
            if sender <= max(self.drones) and self.order_file is None:
                raise ValueError(f'viewer identity {sender} must follow drone IDs {self.drones}')
            self.viewer = sender
            self.note('viewer-from-message', dpid=sender)
        parts = list(split(decode(data)))
        index = self.drones.index(recipient) if recipient in self.drones else 0
        for payload in parts:
            code = payload[0]
            self.rx_counts[f'{code:02x}'] += 1
            self.note('rx', sender=sender, recipient=recipient, payload=payload.hex())
            if code == 2:
                pong = bytearray(payload)
                struct.pack_into('<I', pong, 5, 1000000)
                self.send(index, pong)
                if self.phase == 'lobby':
                    self.unit_sync()
            elif code == 0x20:
                self.ready = bool(payload[156] & 0x20)
            elif code == 0x1a and payload[1] == 2:
                self.acked.add(struct.unpack_from('<I', payload, 6)[0])
            elif code == 0x1a and payload[1] == 4:
                self.received = struct.unpack_from('<H', payload, 10)[0]
            elif code == 0x2c and self.phase == 'loading':
                self.phase = 'playing'
                self.play_start = time.monotonic()
                self.note('playback-start', viewer_sequence=struct.unpack_from('<I', payload, 3)[0])

    def periodic(self):
        now = time.monotonic()
        if self.phase == 'playing':
            elapsed = (now - self.play_start) * 1000 * self.speed
            # Bounded bursts keep receive draining while the capture has dense creates.
            sent = 0
            while self.cursor < len(self.game) and sent < 100:
                m = self.game[self.cursor]
                if m.ms - self.start > elapsed:
                    break
                payload = bytearray(m.payload)
                if payload[:3] == b'\x05\0L':
                    killer = struct.unpack_from('<I', payload, 6)[0]
                    if killer in self.original:
                        struct.pack_into('<I', payload, 6, self.drones[self.original.index(killer)])
                self.send(self.original.index(m.sender), payload)
                self.cursor += 1
                sent += 1
            if self.cursor == len(self.game) and not self.eof:
                self.eof = True
                self.note('playback-eof', events=self.cursor)
        if not self.viewer or now - self.last_periodic < .25:
            return
        self.last_periodic = now
        if self.phase == 'lobby':
            self.identity()
            if self.unit_sent and not self.synced and len(self.acked) >= len(self.units):
                self.unit_enable()
            if self.synced and self.ready:
                self.launch()
        elif self.phase == 'loading':
            for i in range(2):
                self.send(i, b'\x2a\x64')
                self.send(i, b'\x15')
            self.send(0, b'\x1e\x03')

    def run(self):
        try:
            while time.monotonic() < self.deadline:
                if self.io_error:
                    raise RuntimeError(self.io_error)
                try:
                    line = self.rx.get(timeout=.005)
                except queue.Empty:
                    line = ''
                if line is None:
                    raise RuntimeError(f'bridge exited: {self.process.poll()}')
                if line.startswith('READY '):
                    self.drones = sorted(map(int, line.split()[1:]))
                    if self.order_file:
                        self.order_file.write_text(' '.join(map(str, self.drones)) + '\n')
                    user4 = (16974324 & ~65535) | self.unit_limit
                    self.command(f'STATE 1753284736 4 655370 {user4}')
                    self.note('host-ready', drones=self.drones, original=self.original,
                              unit_limit=self.unit_limit, unit_types=len(self.units))
                elif line.startswith('RX '):
                    _, sender, recipient, hexdata = line.split()
                    self.receive(int(sender), int(recipient), bytes.fromhex(hexdata))
                elif line:
                    raise RuntimeError(line)
                if self.drones:
                    self.periodic()
            self.note('timeout', phase=self.phase, events=self.cursor, rx_counts=self.rx_counts)
        finally:
            if self.process.poll() is None:
                try:
                    self.command('STOP')
                    self.process.wait(timeout=10)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    self.process.kill()
                    self.process.wait()
            self.log.close()
            self.stderr.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace', type=Path)
    p.add_argument('--bridge', type=Path, required=True)
    p.add_argument('--prefix', type=Path, required=True)
    p.add_argument('--log', type=Path, required=True)
    p.add_argument('--order-file', type=Path, help='isolated viewer allocation-order probe input')
    p.add_argument('--display', default=':0')
    p.add_argument('--timeout', type=float, default=600)
    p.add_argument('--speed', type=float, default=1)
    ReplayHost(p.parse_args()).run()


if __name__ == '__main__':
    main()
