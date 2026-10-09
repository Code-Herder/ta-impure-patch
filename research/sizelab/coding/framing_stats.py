"""Framing census of the stream: per kind, records, body bytes, header+pad bytes, fixed or variable
length, pad bytes all zero?, unit/proj delta: slot ascending within an iteration?, mask reuse."""
import sys, struct, collections
from common import *
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
NAMES = {v: n for n, v in vars(T).items() if n.startswith("RK_") and isinstance(v, int)}
bl = load(sys.argv[1] if len(sys.argv) > 1 else "battle")
cnt = collections.Counter(); body = collections.Counter(); pad = collections.Counter(); lens = collections.defaultdict(collections.Counter)
nonzero_pad = 0
asc = collections.Counter(); desc = collections.Counter()
mask_same_slot = collections.Counter(); mask_same_prev = collections.Counter(); masks = collections.defaultdict(collections.Counter)
zero_after = collections.Counter()
for k, f, l, p, pk in bl:
    if k != RB_STREAM: continue
    at = 0; n = len(p)
    last_slot = {}; last_mask = {}; prev_mask = {}
    while at + 4 <= n:
        kind, ln = struct.unpack_from("<HH", p, at)
        b = p[at + 4:at + 4 + ln]
        step = (4 + ln + 3) & ~3
        padb = p[at + 4 + ln:at + step]
        if any(padb): nonzero_pad += 1
        cnt[kind] += 1; body[kind] += ln; pad[kind] += step - 4 - ln; lens[kind][ln] += 1
        if kind == T.RK_TICK:
            last_slot = {}; prev_mask = {}
        if kind in (T.RK_UNIT_DELTA, T.RK_PROJ_DELTA):
            slot = struct.unpack_from("<H", b)[0]
            nb = T.REC_UNIT_BITS if kind == T.RK_UNIT_DELTA else T.REC_PROJ_BITS
            m = bytes(b[2:2 + nb])
            hd = T.REC_UNIT_DELTA_HEAD if kind == T.RK_UNIT_DELTA else T.REC_PROJ_DELTA_HEAD
            if any(b[2 + nb:hd]): zero_after[kind] += 1
            if kind in last_slot:
                (asc if slot > last_slot[kind] else desc)[kind] += 1
            last_slot[kind] = slot
            key = (kind, slot)
            if last_mask.get(key) == m: mask_same_slot[kind] += 1
            if prev_mask.get(kind) == m: mask_same_prev[kind] += 1
            last_mask[key] = m; prev_mask[kind] = m
            masks[kind][m] += 1
        at += step
tot_frame = 0
print(f"{'kind':<22}{'records':>10}{'body MB':>10}{'hdr+pad MB':>12}{'distinct len':>14}  top lens")
for kind in sorted(cnt, key=lambda k: -(body[k] + 4 * cnt[k] + pad[k])):
    fr = 4 * cnt[kind] + pad[kind]; tot_frame += fr
    print(f"{NAMES.get(kind, kind):<22}{cnt[kind]:>10}{body[kind]/2**20:>10.2f}{fr/2**20:>12.2f}{len(lens[kind]):>14}  {lens[kind].most_common(4)}")
print("total header+pad MB", tot_frame / 2**20, "nonzero pads", nonzero_pad, "nonzero reserved", dict(zero_after))
for kind in (T.RK_UNIT_DELTA, T.RK_PROJ_DELTA):
    print(NAMES[kind], "slot ascending", asc[kind], "not", desc[kind], "mask same as slot's last", mask_same_slot[kind], "same as prev record", mask_same_prev[kind], "of", cnt[kind], "distinct masks", len(masks[kind]), "top10 cover", sum(c for _, c in masks[kind].most_common(10)) / cnt[kind], "top255 cover", sum(c for _, c in masks[kind].most_common(255)) / cnt[kind])
