#!/usr/bin/env python3
"""Compare the sample's final live rosters with an isolated two-player replay.

This checks only surviving slots, type, integer X/Y, and receiver-side height.
It does not assert full-world fidelity or compare controlling-peer simulation state.
"""
import argparse
import json
from pathlib import Path
import re

ROW = re.compile(r'^  u(\d+) (\w+)\s+own=(\d+) idx=(\d+) '
                 r'world=\((-?\d+),(-?\d+),(-?\d+)\).*nano=([\d.]+)$')


def last_roster(path):
    blocks, current = [], []
    for line in path.read_text().splitlines():
        match = ROW.match(line)
        if not match:
            if current:
                blocks.append(current)
                current = []
            continue
        row = match.groups()
        if int(row[0]) != len(current) + 1:
            raise ValueError('non-contiguous roster dump')
        current.append(dict(engine_index=int(row[3]), type=row[1],
                            world=[int(row[4]), int(row[5])], height=int(row[6])))
    if current:
        raise ValueError('log ends inside roster; retain its following header')
    if not blocks:
        raise ValueError('no roster dump')
    result = {u['engine_index']: u for u in blocks[-1]}
    if len(result) != len(blocks[-1]):
        raise ValueError('duplicate unit slot')
    return result


def compare(host, join, replay, unit_limit):
    actual = {u['engine_index']: u for u in replay['units']}
    report = {}
    for name, expected in [('host', host), ('join', join)]:
        common = expected.keys() & actual.keys()
        report[name] = dict(original=len(expected), replay=len(actual),
                            missing=sorted(expected.keys() - actual.keys()),
                            extra=sorted(actual.keys() - expected.keys()),
                            differences={k: sorted(i for i in common
                                                   if expected[i][k] != actual[i][k])
                                         for k in ('type', 'world', 'height')})
    if host.keys() != join.keys():
        raise ValueError('original peers have different final survivor slots')
    # The capture assigns host units 1..unit_limit and join units in the next block.
    if any(i < 1 or i > 2 * unit_limit for i in host):
        raise ValueError('unit outside the two recorded ranges')
    report['receiver_height_differences'] = sorted(
        i for i in host.keys() & actual.keys()
        if actual[i]['height'] != (join if i <= unit_limit else host)[i]['height'])
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('host_log', type=Path)
    p.add_argument('join_log', type=Path)
    p.add_argument('replay_roster', type=Path)
    p.add_argument('--unit-limit', type=int, default=1500)
    args = p.parse_args()
    result = compare(last_roster(args.host_log), last_roster(args.join_log),
                     json.loads(args.replay_roster.read_text()), args.unit_limit)
    print(json.dumps(result, indent=2))
    failed = result['receiver_height_differences'] or any(
        result[p]['missing'] or result[p]['extra'] or result[p]['differences']['type']
        or result[p]['differences']['world'] for p in ('host', 'join'))
    raise SystemExit(bool(failed))


if __name__ == '__main__':
    main()
