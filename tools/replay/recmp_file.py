"""Strict persisted contribution ledgers; mirrors tagpu_recmp_file_state.c."""
import struct
import zlib


class Roster:
    """Copied bounded rows; a source roster may order the same identities differently."""
    def __init__(self, raw):
        raw = bytes(raw)
        if len(raw) != 176:
            raise ValueError("contribution seat mapping span")
        version, count, self.slots, self.each = struct.unpack_from("<IIII", raw)
        if version != 1 or not 1 <= count <= 10 or not 1 <= self.each <= 6553 or self.slots != 10*self.each+1:
            raise ValueError("contribution seat mapping shape")
        rows = [struct.unpack_from("<IIII", raw, 16+16*i) for i in range(10)]
        if any(row != (0, 0, 0, 0) for row in rows[count:]):
            raise ValueError("unused contribution seat mapping")
        self.actors = rows[:count]
        origins, seats, blocks = set(), set(), set()
        for origin, seat, first, last in self.actors:
            if (not origin or origin in origins or seat in seats or first in blocks or seat >= 10 or
                    not 1 <= first <= last < self.slots or last-first+1 != self.each or (first-1) % self.each):
                raise ValueError("invalid or aliased contribution origin/seat/block")
            origins.add(origin); seats.add(seat); blocks.add(first)

    def mapping(self, destination):
        source = {a[0]: a for a in self.actors}
        target = {a[0]: a for a in destination.actors}
        if ((self.slots, self.each) != (destination.slots, destination.each) or source.keys() != target.keys() or
                any(a[2:] != target[o][2:] for o, a in source.items())):
            raise ValueError("source roster identity or global blocks changed")
        result = {a[1]: target[o][1] for o, a in source.items()}
        vacant = [row for row in range(10) if row not in result]
        available = [row for row in range(10) if row not in result.values()]
        result.update(zip(vacant, available))
        result[10] = 10
        return result

    def player(self, destination, origin, raw):
        image = bytes(raw)
        actor = next((a for a in self.actors if a[0] == origin), None)
        if (len(image) != 520 or actor is None or image[0x146] != actor[1] or
                struct.unpack_from("<II", image, 0x67) != (actor[2]+1, actor[3]+1)):
            raise ValueError("source player self row or block IDs changed")
        mapping, out = self.mapping(destination), bytearray(image)
        for offset in (0x108, 0x113, 0x11E, 0x129, 0x134):
            for row, target in mapping.items():
                out[offset+target] = image[offset+row]
        out[0x146] = mapping[image[0x146]]
        return bytes(out)

    def unit(self, destination, origin, slot, raw):
        image = bytes(raw)
        actor = next((a for a in self.actors if a[0] == origin and a[2] <= slot <= a[3]), None)
        if (len(image) != 328 or actor is None or struct.unpack_from("<I", image, 0x96)[0] != actor[1]+1 or
                image[0xFF] != actor[1] or image[0xF4] > 10):
            raise ValueError("unit source owner/block or attacker row")
        store = struct.unpack_from("<I", image, 0xEC)[0]
        if store > 11:
            raise ValueError("unit normalized player ID")
        mapping, out = self.mapping(destination), bytearray(image)
        struct.pack_into("<I", out, 0x96, mapping[actor[1]]+1)
        struct.pack_into("<I", out, 0xEC, mapping[store-1]+1 if store else 0)
        out[0xF4], out[0xFF] = mapping[image[0xF4]], mapping[image[0xFF]]
        return bytes(out)


class Seats(Roster):
    """Recorder roster additionally agrees with header roles, seats and allocation."""
    def __init__(self, raw, actors, slots, each, players):
        super().__init__(raw)
        if ((slots, each) != (self.slots, self.each) or
                len(self.actors) != len(actors) or len(self.actors) != len(players)):
            raise ValueError("contribution mapping disagrees with world allocation or seats")
        for i, (origin, seat, _, _) in enumerate(self.actors):
            matches = [p for p in players if p["index"] == seat]
            if (origin != actors[i][0] or len(matches) != 1 or not 1 <= matches[0]["controller"] <= 4 or
                    actors[i][1] != (matches[0]["controller"] in (1, 2))):
                raise ValueError("contribution origin has no matching owned seat")


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
            if magic != b"IMPUREMP" or version != 2 or header != 64 or not 64 <= span <= 4096 or span > len(raw)-at:
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
            elif kind in (1, 2, 6, 7, 8, 9, 10):
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


class History:
    """Independent semantic source validation and application-only authority."""
    def __init__(self, header):
        self.header = {key: header[key] for key in ("multiplayer", "mp_seats", "slots", "units_each")}
        self.header["players"] = [p.copy() for p in header["players"]]
        self.ledger = Contributions(self.header["multiplayer"])
        self.roster = Seats(self.header["mp_seats"], self.ledger.actors,
                            self.header["slots"], self.header["units_each"], self.header["players"])
        self.source_players, self.players = {}, {}
        self.source_maps, self.maps = {}, {}
        self.source_units, self.units = {}, {}
        self.source_named, self.named = {}, {}
        self.source_roots, self.roots = {}, {}
        self.source_orders, self.orders, self.stages = {}, {}, {}
        self.applications = []
        self.failed = False

    def _semantic(self, origin, tick, sequence, kind, stream, body, application_tick, proof):
        actor = next(a for a in self.roster.actors if a[0] == origin)
        if kind == 7:
            if sequence != 1 or origin in self.source_maps:
                raise ValueError("late or repeated source roster")
            roster = Roster(body)
            roster.mapping(self.roster)
            state = dict(roster=roster, stream=bytes(stream))
            self.source_maps[origin] = state
            if not proof:
                self.maps[origin] = state.copy()
        elif kind == 6:
            source_map = self.source_maps.get(origin)
            if not source_map or source_map["stream"] != stream or not proof and origin not in self.maps:
                raise ValueError("player precedes its source roster")
            if not 32 <= len(body) <= 552:
                raise ValueError("player delta span")
            player, controller, full = struct.unpack_from("<III", body)
            bits = int.from_bytes(body[12:29], "little")
            previous = self.source_players.get(origin)
            if (player != origin or controller not in (1, 2) or full not in (0, 1) or
                    any(body[29:32]) or bits >> 130 or full and bits != (1 << 130)-1 or
                    not full and previous is None):
                raise ValueError("player delta fields/mask/baseline")
            if previous and (sequence <= previous["sequence"] or tick < previous["source_tick"] or
                             application_tick < previous["applied_tick"] or stream != previous["stream"]):
                raise ValueError("player delta provenance")
            image = bytearray(520) if full else bytearray(previous["image"])
            pos = 32
            for word in range(130):
                if bits >> word & 1:
                    if pos+4 > len(body):
                        raise ValueError("truncated player delta")
                    image[4*word:4*word+4] = body[pos:pos+4]
                    pos += 4
            if pos != len(body) or struct.unpack_from("<II", image, 0x67) != (actor[2]+1, actor[3]+1):
                raise ValueError("player delta block IDs or trailing bytes")
            state = dict(image=bytes(image), controller=controller, source_tick=tick, sequence=sequence,
                         applied_tick=application_tick, stream=bytes(stream))
            mapped = source_map["roster"].player(self.roster, origin, image)
            self.source_players[origin] = state
            if not proof:
                self.players[origin] = dict(state, image=mapped)
        elif kind == 8:
            source_map = self.source_maps.get(origin)
            if not source_map or source_map["stream"] != stream or not proof and origin not in self.maps:
                raise ValueError("units precede their source roster")
            if len(body) < 4:
                raise ValueError("unit batch span")
            count = struct.unpack_from("<I", body)[0]
            if not 1 <= count <= 201:
                raise ValueError("unit batch count")
            at, updates, mapped_updates = 4, {}, {}
            for _ in range(count):
                if at+20 > len(body):
                    raise ValueError("unit entry span")
                slot, live, full, birth, mask, reserved = struct.unpack_from("<HBBI11sB", body, at)
                at += 20
                bits = int.from_bytes(mask, "little")
                previous = self.source_units.get(slot)
                if (not actor[2] <= slot <= actor[3] or slot in updates or live > 1 or full > 1 or reserved or
                        birth >> 31 or bits >> 82 or not live and (not full or bits) or
                        live and full and bits != (1 << 82)-1):
                    raise ValueError("unit entry ownership/flags/mask")
                if previous and (previous["origin"] != origin or previous["stream"] != stream or
                                 sequence <= previous["sequence"] or tick < previous["source_tick"] or
                                 application_tick < previous["applied_tick"]):
                    raise ValueError("unit clocks/stream")
                if live and not full and (not previous or not previous["live"] or previous["birth"] != birth):
                    raise ValueError("unit delta has no matching live incarnation")
                image = bytearray(previous["image"]) if live and not full else bytearray(328)
                for word in range(82):
                    if bits >> word & 1:
                        if at+4 > len(body):
                            raise ValueError("unit delta word span")
                        image[word*4:word*4+4] = body[at:at+4]
                        at += 4
                if live:
                    if (not struct.unpack_from("<I", image, 0x110)[0] & (1 << 28) or
                            not full and image[0x92:0x96] != previous["image"][0x92:0x96]):
                        raise ValueError("unit live bit or delta type change")
                    mapped = source_map["roster"].unit(self.roster, origin, slot, image)
                else:
                    mapped = bytes(image)
                state = dict(image=bytes(image), live=live, origin=origin, birth=birth,
                             source_tick=tick, sequence=sequence, applied_tick=application_tick, stream=bytes(stream))
                updates[slot], mapped_updates[slot] = state, dict(state, image=mapped)
            if at != len(body):
                raise ValueError("unit batch trailing bytes")
            self.source_units.update(updates)
            if not proof:
                self.units.update(mapped_updates)
        elif kind == 9:
            source_map = self.source_maps.get(origin)
            if not source_map or source_map["stream"] != stream or not proof and origin not in self.maps:
                raise ValueError("named roots precede their source roster")
            if not 4 <= len(body) <= 4032:
                raise ValueError("named root batch span")
            count = struct.unpack_from("<I", body)[0]
            if not 1 <= count <= 122:
                raise ValueError("named root batch count")
            previous = self.source_named.get(origin)
            if previous and (sequence <= previous["sequence"] or tick < previous["source_tick"] or
                             application_tick < previous["applied_tick"] or stream != previous["stream"]):
                raise ValueError("named root provenance")
            at, events = 4, []
            for _ in range(count):
                if at+8 > len(body):
                    raise ValueError("named root prefix span")
                span, before, reserved, birth = struct.unpack_from("<HBBI", body, at)
                at += 8
                if not 25 <= span <= 56 or at+span > len(body) or reserved or before > 1 or birth >> 31:
                    raise ValueError("named root prefix fields")
                unit, argc, now, reserved1, reserved2 = struct.unpack_from("<IBBBB", body, at)
                name = body[at+24:at+span]
                if (not actor[2]+1 <= unit <= actor[3]+1 or argc > 4 or now > 1 or
                        reserved1 or reserved2 or b"\0" in name):
                    raise ValueError("named root ownership/arguments/name")
                events.append(dict(unit=unit, birth=birth, before_capture=before, argc=argc,
                                   now=now, args=body[at+8:at+24], name=bytes(name)))
                at += span
            if at != len(body):
                raise ValueError("named root trailing bytes")
            state = dict(body=bytes(body), events=events, source_tick=tick, sequence=sequence,
                         applied_tick=application_tick, stream=bytes(stream))
            self.source_named[origin] = state
            if not proof:
                self.named[origin] = state.copy()
        elif kind == 10:
            from tarec import RK_EV_CREATE, RK_EV_START_NAMED, RK_EV_REPLY, RK_EV_SPRAY
            source_map = self.source_maps.get(origin)
            if not source_map or source_map["stream"] != stream or not proof and origin not in self.maps:
                raise ValueError("owner roots precede their source roster")
            if not 4 <= len(body) <= 4032:
                raise ValueError("owner root batch span")
            count = struct.unpack_from("<I", body)[0]
            if not 1 <= count <= 167:
                raise ValueError("owner root batch count")
            previous = self.source_roots.get(origin)
            if previous and (sequence <= previous["sequence"] or tick < previous["source_tick"] or
                             application_tick < previous["applied_tick"] or stream != previous["stream"]):
                raise ValueError("owner root provenance")
            at, events = 4, []
            for _ in range(count):
                if at+12 > len(body):
                    raise ValueError("owner root prefix span")
                root, span, before, reserved, birth = struct.unpack_from("<HHB3sI", body, at)
                at += 12
                if not 12 <= span <= 140 or at+span > len(body) or any(reserved) or before > 1 or birth >> 31:
                    raise ValueError("owner root prefix fields")
                image = bytes(body[at:at+span])
                unit = struct.unpack_from("<I", image)[0]
                if not actor[2]+1 <= unit <= actor[3]+1:
                    raise ValueError("owner root unit block")
                if root == RK_EV_CREATE:
                    if (span != 28 or not struct.unpack_from("<H", image, 4)[0] or
                            image[6] >= 10 or image[7] > 3 or image[24] > 4 or any(image[25:])):
                        raise ValueError("owner create type/owner/layer/water/reserved")
                    if source_map["roster"].mapping(self.roster)[image[6]] != actor[1]:
                        raise ValueError("owner create source row")
                elif root == RK_EV_START_NAMED:
                    if (not 25 <= span <= 56 or image[4] > 4 or image[5] > 1 or
                            image[6] or image[7] or b"\0" in image[24:]):
                        raise ValueError("owner script arguments/name")
                elif root == RK_EV_REPLY:
                    if (struct.unpack_from("<I", image, 4)[0] >= 24 or image[8] > 1 or image[9] > 1 or
                            image[10] or image[11] or not image[9] and span != 12 or b"\0" in image[12:]):
                        raise ValueError("owner reply kind/flags/text")
                elif root == RK_EV_SPRAY:
                    if (span != 48 or struct.unpack_from("<I", image, 4)[0] > 1 or
                            not 0 <= struct.unpack_from("<h", image, 44)[0] < 10):
                        raise ValueError("owner spray direction/layer")
                else:
                    raise ValueError("unknown owner root kind")
                events.append(dict(kind=root, unit=unit, birth=birth, before_capture=before, image=image))
                at += span
            if at != len(body):
                raise ValueError("owner root trailing bytes")
            state = dict(body=bytes(body), events=events, source_tick=tick, sequence=sequence,
                         applied_tick=application_tick, stream=bytes(stream))
            self.source_roots[origin] = state
            if not proof:
                self.roots[origin] = state.copy()
        elif kind == 2:
            source_map = self.source_maps.get(origin)
            if not source_map or source_map["stream"] != stream or not proof and origin not in self.maps:
                raise ValueError("orders precede their source roster")
            if len(body) < 16:
                raise ValueError("order chunk span")
            slot, which, reserved, birth, total, first, count, reserved2 = struct.unpack_from("<HBBIHHHH", body)
            if (not actor[2] <= slot <= actor[3] or which > 1 or reserved or reserved2 or
                    total > 4096 or count > 45 or first > total or count > total-first or
                    len(body) != 16+88*count or not count and (total or first)):
                raise ValueError("order chunk bound/ownership/span")
            stage = self.stages.get(origin)
            if stage and (stream != stage["stream"] or sequence <= stage["sequence"] or tick < stage["tick"]):
                raise ValueError("order chunk provenance")
            if first == 0:
                if stage and stage["active"]:
                    raise ValueError("interrupted order transaction")
                stage = dict(stream=bytes(stream), tick=tick, slot=slot, which=which, birth=birth,
                             total=total, next=0, active=True, image=bytearray())
                self.stages[origin] = stage
            elif (not stage or not stage["active"] or stage["sequence"]+1 != sequence or
                  (stage["tick"], stage["slot"], stage["which"], stage["birth"], stage["total"], stage["next"]) !=
                  (tick, slot, which, birth, total, first)):
                raise ValueError("order transaction discontinuity")
            stage["image"] += body[16:]
            stage["next"] += count
            stage["sequence"] = sequence
            if stage["next"] == total:
                used = sum(s["count"] for s in self.source_orders.values())
                old = self.source_orders.get((slot, which), {}).get("count", 0)
                if total > 131072-used+old:
                    raise ValueError("order pool exhausted")
                state = dict(origin=origin, birth=birth, source_tick=tick, sequence=sequence,
                             applied_tick=application_tick, count=total, stream=bytes(stream),
                             image=bytes(stage["image"]))
                self.source_orders[slot, which] = state
                if not proof:
                    self.orders[slot, which] = state.copy()
                stage["active"] = False
        elif kind == 4:
            if self.stages.get(origin, {}).get("active"):
                raise ValueError("seal closes an incomplete order transaction")
        # Legacy kind 1 contributes no complete player or order authority.

    def block(self, raw):
        if self.failed:
            raise ValueError("history already invalid")
        try:
            raw = bytes(raw)
            self.ledger.block(raw)
            at = 0
            while at < len(raw):
                start = at
                ordinal, application_tick, phase = struct.unpack_from("<QII", raw, at)
                at += 16
                span, origin, tick, sequence, kind = struct.unpack_from("<IIIII", raw, at+12)
                stream = raw[at+48:at+64]
                proof = phase == 3
                self._semantic(origin, tick, sequence, kind, stream, raw[at+64:at+span],
                               self.ledger.tick if proof else application_tick, proof)
                at += span
                if not proof:
                    self.applications.append((application_tick, memoryview(raw)[start:at]))
        except (ValueError, struct.error):
            self.failed = True
            raise

    def footer(self, raw):
        self.ledger.footer(raw)

    def at(self, tick):
        """Rebuild a semantic application prefix at a world tick, excluding proof tails."""
        if self.failed:
            raise ValueError("history already invalid")
        result = History(self.header)
        for applied, raw in self.applications:
            if applied > tick:
                break
            result.block(raw)
        return result
