"""Strict persisted contribution ledgers; mirrors tagpu_recmp_file_state.c."""
import struct
import zlib


class Contributions:
    def __init__(self, raw):
        if len(raw) != 112:
            raise ValueError("contribution roster span")
        version, count, recorder, reserved = struct.unpack_from("<IIII", raw)
        self.match = raw[16:32]
        if version != 1 or not 1 <= count <= 10 or reserved or not any(self.match):
            raise ValueError("contribution roster fields")
        actors = [struct.unpack_from("<II", raw, 32+8*i) for i in range(10)]
        if any(a != (0, 0) for a in actors[count:]) or any(not o or l > 1 for o, l in actors[:count]):
            raise ValueError("contribution roster actors")
        self.actors = actors[:count]
        if len({o for o, _ in self.actors}) != count or (recorder, 1) not in self.actors:
            raise ValueError("contribution roster identity")
        self.source = [{"sequence": 0, "tick": 0, "crc": 0, "bytes": 0,
                        "sealed_tick": None, "sealed_sequence": 0, "ended": False,
                        "stream": None} for _ in self.actors]
        self.cuts = [(0,)*5+(bytes(16),) if local else (origin, 0, 0, 0, 0, bytes(16))
                     for origin, local in self.actors]
        self.records = self.consumed = self.tick = 0
        self.proof = False

    def block(self, raw):
        if not raw:
            raise ValueError("empty contribution block")
        at = 0
        while at < len(raw):
            if len(raw)-at < 80:
                raise ValueError("truncated contribution record")
            ordinal, application_tick, phase = struct.unpack_from("<QII", raw, at)
            at += 16
            magic, version, header, span, origin, tick, sequence, kind = struct.unpack_from("<8sHHIIIII", raw, at)
            match, stream = raw[at+32:at+48], raw[at+48:at+64]
            if magic != b"IMPUREMP" or version != 1 or header != 64 or not 64 <= span <= 4096 or span > len(raw)-at:
                raise ValueError("contribution envelope span")
            if match != self.match or not any(stream):
                raise ValueError("contribution match/stream")
            index = next((i for i, a in enumerate(self.actors) if a[0] == origin), None)
            if index is None:
                raise ValueError("unknown contribution origin")
            local, source = self.actors[index][1], self.source[index]
            if phase == 3:
                if local or ordinal or application_tick:
                    raise ValueError("proof tail has application metadata")
                self.proof = True
            else:
                if self.proof or ordinal != self.records+1 or application_tick < self.tick or phase != (1 if local else 2):
                    raise ValueError("contribution application order/role")
                self.records, self.tick = ordinal, application_tick
                if not local:
                    self.consumed += 1
                    self.cuts[index] = (origin, sequence, tick, application_tick, self.consumed, stream)
            if source["ended"] or sequence != source["sequence"]+1 or tick < source["tick"] or source["stream"] not in (None, stream):
                raise ValueError("contribution source order/stream")
            body = raw[at+64:at+span]
            if kind == 4:
                if len(body) != 24:
                    raise ValueError("contribution seal span")
                previous, seal_tick, data_bytes, crc, final = struct.unpack("<IIQII", body)
                sealed = source["sealed_tick"]
                if final > 1 or (previous, seal_tick, data_bytes, crc) != (source["sequence"], tick, source["bytes"], source["crc"]) or (sealed is not None and (tick < sealed or (not final and tick == sealed))):
                    raise ValueError("contribution prefix seal")
                source.update(sealed_tick=tick, sealed_sequence=sequence, ended=bool(final))
            elif kind in (1, 2):
                if source["sealed_tick"] is not None and tick <= source["sealed_tick"]:
                    raise ValueError("contribution data in closed tick")
                source["bytes"] += span
                if source["bytes"] > 0xFFFFFFFFFFFFFFFF:
                    raise ValueError("contribution byte count overflow")
                source["crc"] = zlib.crc32(raw[at:at+span], source["crc"])
            else:
                raise ValueError("non-source contribution record")
            source.update(sequence=sequence, tick=tick, stream=stream)
            at += span

    def footer(self, raw):
        if len(raw) != 416:
            raise ValueError("contribution footer span")
        version, count, records = struct.unpack_from("<IIQ", raw)
        cuts = [struct.unpack_from("<IIIIQ16s", raw, 16+40*i) for i in range(10)]
        if version != 1 or count != len(self.actors) or records != self.records or cuts[:count] != self.cuts or any(c != (0, 0, 0, 0, 0, bytes(16)) for c in cuts[count:]):
            raise ValueError("contribution application cut mismatch")
        for i, (_, local) in enumerate(self.actors):
            source = self.source[i]
            if (local and not source["ended"]) or (not local and (source["sealed_tick"] is None or source["sealed_sequence"] < cuts[i][1])):
                raise ValueError("contribution prefix does not cover application cut")
