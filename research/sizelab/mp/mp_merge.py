#!/usr/bin/env python3
"""mp_merge.py -- the merged-world file, prototyped offline on a mode-3 recording.

    mp_merge.py FILE OUTDIR

Reads the recording (tarec.blocks), feeds every contribution record in ledger order through the
independent Python history (recmp_file.History), and composes, at each recorder iteration T, what
the play-time materializer composes (tagpu_replay.c, THE MULTIPLAYER MATERIALIZER / THE CLOCK):

  - a remote owner's unit: its level-start image until its owner's first applied image, then the
    latest applied owner image (mapped to recorder rows), absent after an applied absence or
    DEATH_END, held (not live) after CREATE_BEGIN; every remote envelope with application tick <= T
    (a consumed envelope is never a post-capture root batch, so the split of THE CLOCK does not
    move it);
  - a remote actor's player record: its owner's latest mapped image, +0xF0 the recorder's;
  - a remote unit's order lists: the owner's complete transactions, at their application tick.

It then writes the merged stream: today's stream blocks with the remote units' and remote players'
observed records replaced by the composed ones, the events playback never plays (state, build
done, create, network create) dropped, and the owner-only records added as stream records (roots,
remote order lists, text, perspective). Explored maps are left as recorded ([INFERRED] the composed
map differs from the recorder's in a few entries: one in the measured sample). Each block is
deflated with the writer's settings. Also checks that each LOCAL owner image (produced kind 8)
equals the primary stream's image of the same slot at its application tick.

Writes OUTDIR/merge.json and, for frame2's codec, three npz files in extract.py's format:
today.npz (the observed stream, every unit), merged.npz (local observed + remote composed) and
owner_<origin>.npz (each source's own unit sequence in source ticks, the contributions' content).
"""
import collections
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

REPO = Path(str(__import__("pathlib").Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(REPO / "tools/replay"))
import tarec as T  # noqa: E402
from recmp_file import History  # noqa: E402

RK_OWNER_ROOT = 200        # the prototype's record kinds for the owner-only records
RK_OWNER_TEXT = 201
CREATE_BEGIN = 0x100
NDW = T.REC_UNIT_DWORDS


def deflate(data):
    if not data:
        return 0
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(data) + c.flush())


def rec(kind, body):
    return struct.pack("<HH", kind, len(body)) + body + bytes(-len(body) % 4)


def delta_body(slot, old, new, ndw, nbits, head):
    o = struct.unpack(f"<{ndw}I", old)
    n = struct.unpack(f"<{ndw}I", new)
    bits = bytearray(nbits)
    words = []
    for d in range(ndw):
        if o[d] != n[d]:
            bits[d >> 3] |= 1 << (d & 7)
            words.append(n[d])
    if not words:
        return None, None
    hb = struct.pack("<H", slot) + bytes(bits)
    hb += bytes(head - len(hb))
    return hb + struct.pack(f"<{len(words)}I", *words), bytes(bits)


def contributions(bl, hist):
    """Every contribution record in ledger order, with what it applies (None for a proof)."""
    out = []
    for kind, _, _, raw in bl:
        if kind != T.RB_CONTRIBUTIONS:
            continue
        at = 0
        while at < len(raw):
            ordinal, app, phase = struct.unpack_from("<QII", raw, at)
            span, origin, tick, seq, rk = struct.unpack_from("<IIIII", raw, at + 16 + 12)
            r = bytes(raw[at:at + 16 + span])
            body = r[80:]
            hist.block(r)
            a = dict(ordinal=ordinal, app=app, phase=phase, origin=origin, tick=tick, seq=seq,
                     kind=rk, body=body)
            if phase != 3:
                if rk == 8:
                    n = struct.unpack_from("<I", body)[0]
                    p, ups = 4, []
                    for _ in range(n):
                        slot, live, full, birth, mask, _r = struct.unpack_from("<HBBI11sB", body, p)
                        w = bin(int.from_bytes(mask, "little")).count("1")
                        raw_img = bytes(body[p + 20:p + 20 + 4 * w])
                        p += 20 + 4 * w
                        st = hist.units[slot]
                        ups.append(dict(slot=slot, live=live, full=full, birth=birth, mask=mask,
                                        words=raw_img, image=st["image"], source=hist.source_units[slot]["image"]))
                    a["units"] = ups
                elif rk == 6:
                    a["player"] = hist.players[origin]["image"]
                elif rk == 2:
                    slot, which = struct.unpack_from("<HB", body)
                    o = hist.orders.get((slot, which))
                    if o is not None and o["sequence"] == seq and o["applied_tick"] == app:
                        a["orders"] = (slot, which, o["count"], o["image"])
                elif rk == 10:
                    n = struct.unpack_from("<I", body)[0]
                    p, ents = 4, []
                    for _ in range(n):
                        root, sp, before, _res, birth = struct.unpack_from("<HHB3sI", body, p)
                        ents.append(dict(root=root, before=before, birth=birth,
                                         payload=bytes(body[p + 12:p + 12 + sp])))
                        p += 12 + sp
                    a["roots"] = ents
            out.append(a)
            at += 16 + span
    return out


def iterations(bl):
    """[(block index, tick, [(kind, body)])] of the primary stream, and the snapshots."""
    its, snaps = [], {}
    for bi, (kind, first, last, payload) in enumerate(bl):
        if kind == T.RB_SNAPSHOT:
            snaps[last] = (bi, list(T.records(payload)))
        if kind != T.RB_STREAM:
            continue
        cur = None
        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                cur = (bi, struct.unpack_from("<I", b)[0], [])
                its.append(cur)
            cur[2].append((rk, bytes(b)))
    return its, snaps


class UnitOps:
    """extract.py's op lists, built from (tick, block, creates, deltas, gones) iterations."""

    def __init__(self):
        self.ops, self.images, self.dslot, self.dmask, self.dvoff, self.vals = [], [], [], [], [], []
        self.nvals = 0

    def tick(self, block, gt):
        self.ops.append((0, block, gt, 0))

    def unit(self, block, slot, img):
        self.ops.append((1, block, slot, len(self.images)))
        self.images.append(np.frombuffer(img, "<u4"))

    def delta(self, block, slot, mask, words):
        self.ops.append((2, block, slot, len(self.dslot)))
        self.dslot.append(slot)
        self.dmask.append(mask)
        self.dvoff.append(self.nvals)
        self.vals.append(words)
        self.nvals += len(words) // 4

    def gone(self, block, slot):
        self.ops.append((3, block, slot, 0))

    def save(self, path, snap_ticks):
        dvoff = self.dvoff + [self.nvals]
        np.savez(path, ops=np.array(self.ops, np.int64).reshape(-1, 4),
                 dslot=np.array(self.dslot, np.int32),
                 dmask=np.frombuffer(b"".join(self.dmask), np.uint8).reshape(-1, T.REC_UNIT_BITS),
                 dvoff=np.array(dvoff, np.int64),
                 vals=np.frombuffer(b"".join(self.vals), "<u4"),
                 images=np.array(self.images, np.uint32).reshape(-1, NDW),
                 snap_ticks=np.array(sorted(snap_ticks), np.int64))


def main(path, outdir):
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    stem = Path(path).parent.parent.name + "_" + Path(path).parent.name + "_" + Path(path).stem
    sizes = []
    bl, stop = T.blocks(path, sizes)
    assert stop is None, stop
    segs = T.segments(bl)
    h = segs[0]["header"]
    hist = History(h)
    local_origins = {o for o, l in hist.ledger.actors if l}
    seats = hist.roster.actors
    remote_slot, local_slot, remote_seat, origin_seat = set(), set(), {}, {}
    for origin, seat, first, last in seats:
        origin_seat[origin] = seat
        for s in range(first, last + 1):
            (local_slot if origin in local_origins else remote_slot).add(s)
        if origin not in local_origins:
            remote_seat[seat] = origin
    apps = contributions(bl, hist)
    its, snaps = iterations(bl)

    # the materializer's clock: per application tick, the split at the first produced root batch
    # whose first entry is post-capture (THE CLOCK)
    by_tick = collections.defaultdict(list)
    for a in apps:
        if a["phase"] != 3:
            by_tick[a["app"]].append(a)
    pre, post = {}, {}
    for t, lst in by_tick.items():
        cut = next((i for i, a in enumerate(lst) if a["phase"] == 1 and a["kind"] == 10 and
                    a["roots"] and not a["roots"][0]["before"]), len(lst))
        pre[t], post[t] = lst[:cut], lst[cut:]

    # composition state
    comp, comp_live, emitted = {}, {}, {}
    cplayer, pemitted = {}, {}
    prim_units, prim_players = {}, {}
    local_owner = {}
    local_player = {}
    checks = collections.Counter()
    diff_dwords = collections.Counter()
    started = False

    def owner_records(lst):
        """the owner-only stream records of a list of applications"""
        rs = []
        for a in lst:
            k = a["kind"]
            if k == 10:
                for e in a["roots"]:
                    rs.append(rec(RK_OWNER_ROOT, struct.pack("<HBBI", e["root"], e["before"], 0, e["birth"])
                                  + e["payload"]))
            elif k == 12:
                n = struct.unpack_from("<I", a["body"])[0]
                for i in range(n):
                    rs.append(rec(RK_OWNER_TEXT, a["body"][4 + 136 * i:4 + 136 * (i + 1)]))
            elif k == 13:
                rs.append(rec(T.RK_POV, a["body"]))
            elif k == 14:
                rs.append(rec(T.RK_CLICK, a["body"]))
            elif k == 15:
                rs.append(rec(T.RK_SELECT, a["body"]))
            elif k == 2 and a["origin"] not in local_origins and "orders" in a:
                slot, which, count, img = a["orders"]
                first = 0
                while True:
                    n = min(512, count - first)
                    rs.append(rec(T.RK_ORDERS, struct.pack("<HBxHH", slot, which, count, first)
                                  + img[88 * first:88 * (first + n)]))
                    first += n
                    if first >= count:
                        break
        return rs

    def apply(lst):
        for a in lst:
            k, o = a["kind"], a["origin"]
            if k == 8:
                for u in a["units"]:
                    if o in local_origins:
                        local_owner[u["slot"]] = u["image"] if u["live"] else None
                    else:
                        comp_live[u["slot"]] = bool(u["live"])
                        if u["live"]:
                            comp[u["slot"]] = u["image"]
            elif k == 10 and o not in local_origins:
                for e in a["roots"]:
                    slot = struct.unpack_from("<I", e["payload"])[0] - 1
                    if e["root"] in (CREATE_BEGIN, T.RK_EV_DEATH_END):
                        comp_live[slot] = False
            elif k == 6 and o not in local_origins:
                cplayer[origin_seat[o]] = bytearray(a["player"])
            elif k == 6:
                local_player[origin_seat[o]] = a["player"]

    merged_blocks = collections.defaultdict(list)
    today_ops, merged_ops = UnitOps(), UnitOps()
    removed = collections.Counter()
    added = collections.Counter()
    applied_upto = 0
    for n, (bi, tick, records) in enumerate(its):
        # primary world (observed), for the local check and the level-start images
        for rk, b in records:
            if rk == T.RK_UNIT:
                prim_units[struct.unpack_from("<H", b)[0]] = bytearray(b[4:4 + T.REC_UNIT_IMAGE])
            elif rk == T.RK_UNIT_DELTA:
                s = struct.unpack_from("<H", b)[0]
                T.World._delta(prim_units[s], b[2:2 + T.REC_UNIT_BITS], NDW, b[T.REC_UNIT_DELTA_HEAD:])
            elif rk == T.RK_UNIT_GONE:
                prim_units.pop(struct.unpack_from("<H", b)[0], None)
            elif rk == T.RK_PLAYER:
                prim_players[struct.unpack_from("<H", b)[0]] = bytearray(b[4:4 + T.REC_PLAYER_IMAGE])
            elif rk == T.RK_PLAYER_DELTA:
                i = struct.unpack_from("<H", b)[0]
                T.World._delta(prim_players[i], b[2:2 + T.REC_PLAYER_BITS], T.REC_PLAYER_DWORDS,
                               b[T.REC_PLAYER_DELTA_HEAD:])
        if not started:
            for s, img in prim_units.items():
                if s in remote_slot:
                    comp[s], comp_live[s] = bytes(img), True
            for seat in remote_seat:
                cplayer[seat] = bytearray(prim_players[seat])
            started = True
        # every application through tick: those of earlier ticks whole, this tick's pre-capture part
        while applied_upto < tick:
            applied_upto += 1
            if applied_upto < tick:
                apply(pre.get(applied_upto, []) + post.get(applied_upto, []))
            else:
                apply(pre.get(applied_upto, []))
        # the local check: produced owner images equal the primary's
        for s, img in local_owner.items():
            p = prim_units.get(s)
            if img is None:
                checks["local absent: primary absent" if p is None else "local absent: PRIMARY LIVE"] += 1
            elif p is None:
                checks["local live: PRIMARY ABSENT"] += 1
            elif bytes(p) == img:
                checks["local live: equal"] += 1
            else:
                checks["local live: DIFFERENT"] += 1
                for d in range(NDW):
                    if p[4 * d:4 * d + 4] != img[4 * d:4 * d + 4]:
                        diff_dwords[f"+0x{4 * d:03X}"] += 1
        local_owner.clear()
        for seat, img in local_player.items():
            p = prim_players.get(seat)
            if p is not None and bytes(p) == img:
                checks["local player: equal"] += 1
            else:
                checks["local player: DIFFERENT"] += 1
                for d in range(T.REC_PLAYER_DWORDS):
                    if p is None or p[4 * d:4 * d + 4] != img[4 * d:4 * d + 4]:
                        diff_dwords[f"player +0x{4 * d:03X}"] += 1
        local_player.clear()
        # the merged records of this iteration
        mrec = []
        today_ops.tick(bi, tick)
        merged_ops.tick(bi, tick)
        for rk, b in records:
            slot = struct.unpack_from("<H", b)[0] if len(b) >= 2 else None
            if rk in (T.RK_UNIT, T.RK_UNIT_DELTA, T.RK_UNIT_GONE):
                if rk == T.RK_UNIT:
                    today_ops.unit(bi, slot, b[4:4 + T.REC_UNIT_IMAGE])
                elif rk == T.RK_UNIT_DELTA:
                    today_ops.delta(bi, slot, b[2:2 + T.REC_UNIT_BITS], b[T.REC_UNIT_DELTA_HEAD:])
                else:
                    today_ops.gone(bi, slot)
                if slot in remote_slot:
                    removed["remote unit records"] += len(rec(rk, b))
                    continue
                if rk == T.RK_UNIT:
                    merged_ops.unit(bi, slot, b[4:4 + T.REC_UNIT_IMAGE])
                elif rk == T.RK_UNIT_DELTA:
                    merged_ops.delta(bi, slot, b[2:2 + T.REC_UNIT_BITS], b[T.REC_UNIT_DELTA_HEAD:])
                else:
                    merged_ops.gone(bi, slot)
            elif rk in (T.RK_PLAYER, T.RK_PLAYER_DELTA) and slot in remote_seat:
                removed["remote player records"] += len(rec(rk, b))
                continue
            elif rk in (T.RK_EV_STATE, T.RK_EV_BUILT, T.RK_EV_CREATE, T.RK_EV_NET_CREATE):
                removed["events playback does not play"] += len(rec(rk, b))
                continue
            mrec.append(rec(rk, b))
            if rk == T.RK_TICK:
                # the composed remote units and players of image T, after the TICK
                for s in sorted(set(comp_live) | set(emitted)):
                    live = comp_live.get(s, False)
                    was = emitted.get(s)
                    if live and was is None:
                        r = rec(T.RK_UNIT, struct.pack("<HH", s, 0) + comp[s])
                        merged_ops.unit(bi, s, comp[s])
                        emitted[s] = comp[s]
                    elif live:
                        body, mask = delta_body(s, was, comp[s], NDW, T.REC_UNIT_BITS, T.REC_UNIT_DELTA_HEAD)
                        if body is None:
                            continue
                        r = rec(T.RK_UNIT_DELTA, body)
                        merged_ops.delta(bi, s, mask, body[T.REC_UNIT_DELTA_HEAD:])
                        emitted[s] = comp[s]
                    elif was is not None:
                        r = rec(T.RK_UNIT_GONE, struct.pack("<HH", s, 0))
                        merged_ops.gone(bi, s)
                        del emitted[s]
                    else:
                        continue
                    mrec.append(r)
                    added["composed remote units"] += len(r)
                for seat, img in cplayer.items():
                    img[0xF0:0xF4] = prim_players[seat][0xF0:0xF4]
                    was = pemitted.get(seat)
                    if was is None:
                        r = rec(T.RK_PLAYER, struct.pack("<HH", seat, 0) + bytes(img))
                    else:
                        body, _ = delta_body(seat, was, bytes(img), T.REC_PLAYER_DWORDS, T.REC_PLAYER_BITS,
                                             T.REC_PLAYER_DELTA_HEAD)
                        if body is None:
                            continue
                        r = rec(T.RK_PLAYER_DELTA, body)
                    pemitted[seat] = bytes(img)
                    mrec.append(r)
                    added["composed remote players"] += len(r)
        # owner-only records: this tick's pre-capture applications (before image T, so they end
        # iteration T-1: the placement does not matter to a size) and its post-capture ones after
        # the iteration's events
        for r in owner_records(pre.get(tick, [])) + owner_records(post.get(tick, [])):
            mrec.append(r)
            added["owner-only records"] += len(r)
        merged_blocks[bi].extend(mrec)

    # the file: today, and merged
    other = sum(pk + 32 for k, _, _, _, pk in sizes if k not in (T.RB_STREAM, T.RB_CONTRIBUTIONS))
    today_stream = sum(pk + 32 for k, _, _, _, pk in sizes if k == T.RB_STREAM)
    today_contrib = sum(pk + 32 for k, _, _, _, pk in sizes if k == T.RB_CONTRIBUTIONS)
    merged_stream = sum(deflate(b"".join(v)) + 32 for v in merged_blocks.values())
    # the merged stream by category, each deflated alone per block (recsize's attribution)
    def category(r):
        rk, n = struct.unpack_from("<HH", r)
        slot = struct.unpack_from("<H", r, 4)[0] if n >= 2 else None
        if rk in (T.RK_UNIT, T.RK_UNIT_DELTA, T.RK_UNIT_GONE):
            return "units, remote (composed)" if slot in remote_slot else "units, local"
        if rk in (T.RK_PLAYER, T.RK_PLAYER_DELTA):
            return "players, remote (composed)" if slot in remote_seat else "players, others"
        if rk == T.RK_ORDERS:
            return "order lists, remote (owners')" if slot in remote_slot else "order lists, local"
        if rk == RK_OWNER_ROOT:
            return "owner roots (lifecycle, starts, replies, sprays)"
        if rk == RK_OWNER_TEXT:
            return "chat"
        if rk in (T.RK_POV, T.RK_CLICK, T.RK_SELECT):
            return "perspective"
        if rk in (T.RK_PROJ_HEAD, T.RK_PROJ, T.RK_PROJ_DELTA):
            return "projectiles"
        if rk == T.RK_CELLS:
            return "cells"
        return "other shared"
    cat_alone, cat_raw = collections.Counter(), collections.Counter()
    for v in merged_blocks.values():
        parts = collections.defaultdict(bytearray)
        for r in v:
            parts[category(r)] += r
        for c, d in parts.items():
            cat_alone[c] += deflate(bytes(d))
            cat_raw[c] += len(d)
    recompressed = sum(deflate(bytes(bl[bi][3])) + 32 for bi in merged_blocks)
    footer = 16 + 10 * 40 + 10 * 24          # per source: cut row and its last seal
    res = dict(file=str(path), bytes=Path(path).stat().st_size, iterations=len(its),
               today=dict(stream=today_stream, contributions=today_contrib, other=other,
                          total=today_stream + today_contrib + other),
               merged=dict(stream=merged_stream, footer=footer, other=other,
                           total=merged_stream + footer + other),
               stream_recompressed_alike=recompressed,
               removed_raw=dict(removed), added_raw=dict(added),
               local_check=dict(checks), local_diff_dwords=dict(diff_dwords.most_common(12)),
               merged_categories={c: dict(raw=cat_raw[c], alone=cat_alone[c])
                                  for c in sorted(cat_alone, key=lambda c: -cat_alone[c])})
    (out / f"merge_{stem}.json").write_text(json.dumps(res, indent=1))
    print(json.dumps(res, indent=1))

    # the npz files for the unit codec
    snap_ticks = list(snaps)
    today_ops.save(out / f"today_{stem}.npz", snap_ticks)
    merged_ops.save(out / f"merged_{stem}.npz", snap_ticks)
    # each source's own sequence, one iteration per source tick, blocks by the stream's tick ranges
    bounds = sorted((first, bi) for bi, (k, first, last, _) in enumerate(bl) if k == T.RB_STREAM)
    starts = [f for f, _ in bounds]

    def block_of(t):
        i = max(0, np.searchsorted(starts, t, side="right") - 1)
        return bounds[i][1]
    for origin, _l in hist.ledger.actors:
        ops = UnitOps()
        cur = {}
        seq = [a for a in apps if a["origin"] == origin and a["kind"] == 8 and a["phase"] != 3]
        if not seq:
            continue
        by = collections.defaultdict(list)
        for a in seq:
            by[a["tick"]].extend(a["units"])
        for t in range(min(by), max(by) + 1):
            b = block_of(t)
            ops.tick(b, t)
            for u in by.get(t, []):
                s = u["slot"]
                img = u["source"]
                if u["live"] and (u["full"] or s not in cur):
                    ops.unit(b, s, img)
                    cur[s] = img
                elif u["live"]:
                    body, mask = delta_body(s, cur[s], img, NDW, T.REC_UNIT_BITS, T.REC_UNIT_DELTA_HEAD)
                    if body is not None:
                        ops.delta(b, s, mask, body[T.REC_UNIT_DELTA_HEAD:])
                    cur[s] = img
                elif s in cur:
                    ops.gone(b, s)
                    del cur[s]
        ops.save(out / f"owner_{stem}_{origin}.npz", snap_ticks)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
