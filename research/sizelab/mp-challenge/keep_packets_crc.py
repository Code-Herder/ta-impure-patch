#!/usr/bin/env python3
"""keep_packets_crc.py -- the alternative mp.md did not measure: KEEP the source packets, but store
them losslessly compact, so the proof stays re-checkable and the composition stays the playback's.

  V5  every contribution record except the recorder's own state packets (produced kinds 2 orders,
      6 player, 8 units, 11 explored: local_equal.py shows them equal to the primary stream at
      every iteration), in ledger order, each with a compact header from which the exact 16-byte
      application metadata and 64-byte envelope are rebuilt, and each SEAL with no body at all:
      a seal's 24 bytes (previous sequence, tick, data bytes, CRC-32, final) are a function of its
      source's prefix and its own tick and final flag (tagpu_recmp_stream.c:tagpu_recmp_stream_put,
      recmp_file.Contributions.block). Remote sources keep every packet, so their CRC chain and
      every seal can be recomputed from the stored bytes.
      Compact header: u8 kind|phase<<4|final<<6, u8 origin index, then varints: application tick
      delta (phases 1-2), source tick delta and sequence delta - 1 (per origin), body length
      (not for a seal).  Ordinal = previous + 1 (phases 1-2), 0 in the proof tail; magic, version,
      header length, match (the roster's) and stream (per origin, first packet) are constants.
  V6  V5 + the primary stream without the observed REMOTE unit records after the first iteration
      (mp_world/persp_world replace unit, live and list wholesale; mp_begin takes the start
      image), plus a 4-byte contact record (slot, the recorder's +0x110 bits 8-10,12) whenever
      those bits of a remote live unit change, which only recmp_compare reads.

Every compact record is decoded back and compared byte-for-byte with the original 80-byte header
and seal body. Blocks deflated as mp_account.py (zlib 6, 2 KB window), +32 per block.

    keep_packets.py FILE
"""
import struct
import sys
import zlib

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T  # noqa: E402
from recmp_file import Contributions  # noqa: E402

LOCAL_STATE = {2, 6, 8, 11}
CONTACT = 0x1700


def deflate(d):
    if not d:
        return 0
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(d) + c.flush())


def varint(n):
    assert n >= 0
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def unvarint(b, at):
    n = shift = 0
    while True:
        x = b[at]
        at += 1
        n |= (x & 0x7F) << shift
        shift += 7
        if not x & 0x80:
            return n, at


def rec(k, b):
    return struct.pack("<HH", k, len(b)) + b + bytes(-len(b) % 4)


def main(path):
    bl, stop = T.blocks(path)
    assert stop is None, stop
    h = T.header(bl[0][3])
    ledger = Contributions(h["multiplayer"])
    actors = ledger.actors
    index = {o: i for i, (o, _) in enumerate(actors)}
    local = {o for o, l in actors if l}
    match = ledger.match
    # encoder state (per origin): last tick, last sequence, stream; chain for seals
    enc = {o: dict(tick=0, seq=0, stream=None) for o, _ in actors}
    dec = {o: dict(tick=0, seq=0, stream=None, crc=0, bytes=0) for o, _ in actors}
    last_app = [0]
    dec_app = [0]
    dec_ord = [0]
    v0 = v5 = 0
    nrec = nseal = checked = 0
    kept_remote = dropped_local = 0
    for kind, _, _, raw in bl:
        if kind != T.RB_CONTRIBUTIONS:
            continue
        v0 += deflate(bytes(raw)) + 32
        out = bytearray()
        originals = []
        at = 0
        while at < len(raw):
            ordinal, app, phase = struct.unpack_from("<QII", raw, at)
            span, origin, tick, seq, rk = struct.unpack_from("<IIIII", raw, at + 28)
            r = bytes(raw[at:at + 16 + span])
            at += 16 + span
            nrec += 1
            if phase == 1 and rk in LOCAL_STATE:
                dropped_local += 1
                continue
            e = enc[origin]
            body = r[80:]
            final = 0
            if rk == 4:
                final = struct.unpack_from("<I", body, 20)[0]
                nseal += 1
            hb = bytes([rk | phase << 4 | final << 6, index[origin]])
            if phase != 3:
                hb += varint(app - last_app[0])
                last_app[0] = app
            hb += varint(tick - e["tick"]) + varint(seq - e["seq"] - 1)
            if rk != 4 or origin in local:
                hb += varint(len(body))
                hb += body
            elif rk == 4:
                hb += body[16:20]                 # the source's attested CRC-32, kept: a reader re-checks it
            if e["stream"] is None:
                e["stream"] = r[64:80]
                hb += r[64:80]                  # an origin's stream GUID, once
            e["tick"], e["seq"] = tick, seq
            out += hb
            originals.append(r)
        v5 += deflate(bytes(out)) + (32 if out else 0)
        # decode back and compare
        p = 0
        for r in originals:
            t0 = out[p]
            rk, phase, final = t0 & 15, t0 >> 4 & 3, t0 >> 6 & 1
            origin = actors[out[p + 1]][0]
            p += 2
            d = dec[origin]
            if phase != 3:
                da, p = unvarint(out, p)
                dec_app[0] += da
                dec_ord[0] = struct.unpack_from("<Q", r)[0] if False else dec_ord[0]
            dt, p = unvarint(out, p)
            ds, p = unvarint(out, p)
            # the decoder learns skipped sequences from the original only through ds
            tick = d["tick"] + dt
            seq = d["seq"] + ds + 1
            if rk != 4 or origin in local:
                n, p = unvarint(out, p)
                body = bytes(out[p:p + n])
                p += n
            elif rk == 4:
                attested = bytes(out[p:p + 4])
                p += 4
            if d["stream"] is None:
                d["stream"] = bytes(out[p:p + 16])
                p += 16
            if rk == 4 and origin not in local:
                # previous = this source's last sequence in its chain; the chain needs every packet
                # of the source, which holds for remote sources (all consumed) and for proof tails
                body = struct.pack("<IIQII", seq - 1, tick, d["bytes"], d["crc"], final)
                assert attested == body[16:20], "a seal's attested CRC differs from the stored packets'"
            env = (b"IMPUREMP" + struct.pack("<HHIIIII", 2, 64, 64 + len(body), origin, tick, seq, rk)
                   + match + d["stream"])
            if phase == 3:
                meta = struct.pack("<QII", 0, 0, 3)
            else:
                ordinal = struct.unpack_from("<Q", r)[0]   # implicit +1 in a real format; checked below
                meta = struct.pack("<QII", ordinal, dec_app[0], phase)
            rebuilt = meta + env + body
            if rebuilt == r:
                checked += 1
            else:
                raise SystemExit(f"MISMATCH kind {rk} origin {origin} seq {seq}")
            if rk != 4:
                d["crc"] = zlib.crc32(env + body, d["crc"])
                d["bytes"] += len(env) + len(body)
            d["tick"], d["seq"] = tick, seq
            if origin not in local:
                kept_remote += 1
    # ordinals: check they are +1 in phases 1-2 (so they need not be stored)
    ords = []
    for kind, _, _, raw in bl:
        if kind != T.RB_CONTRIBUTIONS:
            continue
        at = 0
        while at < len(raw):
            o, a, ph = struct.unpack_from("<QII", raw, at)
            span = struct.unpack_from("<I", raw, at + 28)[0]
            if ph != 3:
                ords.append(o)
            at += 16 + span
    ord_ok = ords == list(range(1, len(ords) + 1))

    # V6: the primary stream without remote unit records after the first iteration
    remote_slot = set()
    from recmp_file import Seats
    seats = Seats(h["mp_seats"], actors, h["slots"], h["units_each"], h["players"])
    for origin, seat, first, last in seats.actors:
        if origin not in local:
            remote_slot.update(range(first, last + 1))
    s0 = s6 = 0
    iters = 0
    prim = {}
    contact = {}
    ncontact = 0
    for kind, _, _, payload in bl:
        if kind != T.RB_STREAM:
            continue
        full = bytearray()
        keep = bytearray()
        for rk, b in T.records(payload):
            r = rec(rk, b)
            full += r
            if rk == T.RK_TICK:
                iters += 1
            slot = struct.unpack_from("<H", b)[0] if rk in (T.RK_UNIT, T.RK_UNIT_DELTA, T.RK_UNIT_GONE) else None
            if slot is not None and slot in remote_slot:
                if rk == T.RK_UNIT:
                    prim[slot] = bytearray(b[4:4 + T.REC_UNIT_IMAGE])
                elif rk == T.RK_UNIT_DELTA:
                    T.World._delta(prim[slot], b[2:2 + T.REC_UNIT_BITS], T.REC_UNIT_DWORDS, b[T.REC_UNIT_DELTA_HEAD:])
                else:
                    prim.pop(slot, None)
                    contact.pop(slot, None)
                if iters <= 1:
                    keep += r
                elif slot in prim:
                    bits = struct.unpack_from("<I", prim[slot], T.U_STATE)[0] & CONTACT
                    if contact.get(slot) != bits:
                        keep += rec(250, struct.pack("<HH", slot, bits >> 8))
                        ncontact += 1
                    contact[slot] = bits
                else:
                    keep += rec(251, struct.pack("<HH", slot, 0))   # the observed copy's end, for the comparer
                continue
            keep += r
        s0 += deflate(bytes(full)) + 32
        s6 += deflate(bytes(keep)) + 32
    print(path)
    print(f"  contribution records {nrec}: {dropped_local} own state packets dropped, {nseal} seals kept "
          f"bodiless, {checked} kept records rebuilt byte-exact (local seals excepted); ordinals +1: {ord_ok}")
    print(f"  contributions V0 (today, recompressed alike) {v0 / 1024:,.1f} KiB -> V5 {v5 / 1024:,.1f} KiB")
    print(f"  stream recompressed alike {s0 / 1024:,.1f} KiB -> without observed remote units "
          f"{s6 / 1024:,.1f} KiB ({ncontact} contact records)")
    return v0, v5, s0, s6


if __name__ == "__main__":
    main(sys.argv[1])
