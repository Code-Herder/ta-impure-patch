#!/usr/bin/env python3
"""Offline tests for ta3do — no game files, no browser, no network.

Every format this tool reads is a pile of absolute offsets that either line up or
quietly produce garbage, so the tests here build each container by hand and read
it back: an obfuscated HPI archive, a GAF frame using all three run types, a 3DO
tree with a sibling chain and a selection quad, a COB whose Create hides a piece.
The renderer half is covered by the parts that decide *what* gets drawn — atlas
packing, triangulation, winding, framing table — not by pixels.

    python3 tools/test_ta3do.py [-v]
"""

import json
import re
import struct
import tempfile
import unittest
import zlib
from importlib.machinery import SourceFileLoader
from pathlib import Path

ta3do = SourceFileLoader("ta3do", str(Path(__file__).with_name("ta3do"))).load_module()


# --------------------------------------------------------------------------- builders


def make_hpi(files: dict, key: int = 0) -> bytes:
    """Write a HAPI archive the way TA does, so the reader has something real to eat.

    Paths with slashes become real sub-directory nodes, which is the part of the
    reader most likely to go wrong silently.
    """
    tree = {}
    for path, data in files.items():
        node = tree
        parts = path.split("/")
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = data

    body = bytearray(b"\0" * 20)          # header written last

    def emit_file(data: bytes) -> int:
        chunks = []
        for start in range(0, max(len(data), 1), 65536):
            piece = data[start:start + 65536]
            packed = zlib.compress(piece)
            chunks.append(struct.pack("<IBBBIII", 0x48535153, 2, 2, 0, len(packed),
                                      len(piece), 0) + packed)
        offset = len(body)
        body.extend(struct.pack(f"<{len(chunks)}I", *[len(c) for c in chunks]))
        body.extend(b"".join(chunks))
        entry = len(body)
        body.extend(struct.pack("<IIB", offset, len(data), 2))
        return entry

    def emit_dir(node) -> int:
        rows = []
        for name, value in node.items():
            if isinstance(value, dict):
                rows.append((name, emit_dir(value), 1))
            else:
                rows.append((name, emit_file(value), 0))
        placed = []
        for name, data_off, flag in rows:
            name_off = len(body)
            body.extend(name.encode() + b"\0")
            placed.append((name_off, data_off, flag))
        here = len(body)
        body.extend(struct.pack("<II", len(placed), here + 8))
        for name_off, data_off, flag in placed:
            body.extend(struct.pack("<IIB", name_off, data_off, flag))
        return here

    root = emit_dir(tree)
    body[0:20] = struct.pack("<4sIIII", b"HAPI", 0x00010000, len(body) - root,
                             key, root)
    if not key:
        return bytes(body)
    mask = ~((key * 4) | (key >> 6)) & 0xFF
    scrambled = bytearray(body[:20])
    for pos in range(20, len(body)):
        scrambled.append(((pos ^ mask) ^ (~body[pos] & 0xFF)) & 0xFF)
    return bytes(scrambled)


def make_gaf(name: str, width: int, height: int, rows) -> bytes:
    """A one-entry GAF whose single frame is RLE'd with the runs `rows` describes."""
    header = struct.pack("<III", 0x00010100, 1, 0)
    entry_off = len(header) + 4
    frame_entry_off = entry_off + 40
    frame_header_off = frame_entry_off + 8
    pixel_off = frame_header_off + 24
    pixels = b"".join(rows)
    blob = (header + struct.pack("<I", entry_off)
            + struct.pack("<HIH32s", 1, 1, 0, name.encode())
            + struct.pack("<II", frame_header_off, 0)
            + struct.pack("<HHhhBBHIII", width, height, 0, 0, 9, 1, 0, 0, pixel_off, 0)
            + pixels)
    return blob


def rle_row(*runs) -> bytes:
    body = b"".join(runs)
    return struct.pack("<H", len(body)) + body


def transparent_run(count):
    return bytes([(count << 1) | 1])


def repeat_run(count, value):
    return bytes([((count - 1) << 2) | 2, value])


def literal_run(values):
    return bytes([(len(values) - 1) << 2]) + bytes(values)


def make_3do(objects) -> bytes:
    """Assemble a 3DO from dicts: {name, offset, verts, prims, child, sibling}.

    Objects are laid out in the order given; `child`/`sibling` are indices into
    the same list, so a test can build a real first-child/next-sibling tree.
    """
    count = len(objects)
    header_size = 52
    blob = bytearray(b"\0" * (header_size * count))
    slots = []
    for index, obj in enumerate(objects):
        name_off = len(blob)
        blob += obj["name"].encode() + b"\0"
        vert_off = len(blob)
        for vertex in obj["verts"]:
            blob += struct.pack("<3i", *[round(v * 65536) for v in vertex])
        prim_meta = []
        for prim in obj["prims"]:
            idx_off = len(blob)
            blob += struct.pack(f"<{len(prim['indices'])}H", *prim["indices"])
            tex_off = 0
            if prim.get("texture"):
                tex_off = len(blob)
                blob += prim["texture"].encode() + b"\0"
            prim_meta.append((prim, idx_off, tex_off))
        prim_off = len(blob)
        for prim, idx_off, tex_off in prim_meta:
            blob += struct.pack("<8i", prim.get("color", 0), len(prim["indices"]), 0,
                                idx_off, tex_off, 0, 0, 0)
        slots.append((name_off, vert_off, prim_off))

    for index, obj in enumerate(objects):
        name_off, vert_off, prim_off = slots[index]
        offset = obj.get("offset", (0, 0, 0))
        struct.pack_into("<11i", blob, index * header_size,
                         1, len(obj["verts"]), len(obj["prims"]),
                         obj.get("selection", -1),
                         *[round(v * 65536) for v in offset],
                         name_off, 0, vert_off, prim_off)
        struct.pack_into("<ii", blob, index * header_size + 44,
                         header_size * obj["sibling"] if obj.get("sibling") else 0,
                         header_size * obj["child"] if obj.get("child") else 0)
    return bytes(blob)


def make_cob(pieces, scripts) -> bytes:
    """A COB with the given piece names and {name: [code words]} scripts."""
    names = list(scripts)
    code = []
    entries = []
    for name in names:
        entries.append(len(code))
        code.extend(scripts[name])
    head = 44
    blob = bytearray(b"\0" * head)
    entry_off = len(blob)
    blob += struct.pack(f"<{len(entries)}i", *entries)
    sname_ptr_off = len(blob)
    blob += b"\0" * (4 * len(names))
    pname_ptr_off = len(blob)
    blob += b"\0" * (4 * len(pieces))
    sname_offs, pname_offs = [], []
    for name in names:
        sname_offs.append(len(blob))
        blob += name.encode() + b"\0"
    for name in pieces:
        pname_offs.append(len(blob))
        blob += name.encode() + b"\0"
    code_off = len(blob)
    blob += struct.pack(f"<{len(code)}i", *[c - (1 << 32) if c >= (1 << 31) else c
                                            for c in code])
    struct.pack_into(f"<{len(names)}i", blob, sname_ptr_off, *sname_offs)
    struct.pack_into(f"<{len(pieces)}i", blob, pname_ptr_off, *pname_offs)
    struct.pack_into("<11i", blob, 0, 4, len(names), len(pieces), len(code), 0, 0,
                     entry_off, sname_ptr_off, pname_ptr_off, code_off, 0)
    return bytes(blob)


class FakeBank:
    """A texture bank with no GAF behind it: every name resolves, or none does."""

    def __init__(self, frames=None):
        self.frames = frames or {}

    def frame(self, name):
        return self.frames.get(name.upper()) if name else None


def frame_of(width, height, fill=100, transparent=9):
    return ta3do.GafFrame(width, height, bytearray([fill]) * (width * height), transparent)


PALETTE = [(i, (i * 7) % 256, (i * 13) % 256) for i in range(256)]


# --------------------------------------------------------------------------- archives


def backref(offset: int, count: int) -> bytes:
    return struct.pack("<H", (offset << 4) | (count - 2))


class TestLz77(unittest.TestCase):
    """Tag bits are LSB-first, clear = literal. file-formats.md §5 says the
    opposite; the shipped archives say this, so the tests pin the archives."""

    def test_literals_then_backreference(self):
        # bits 0-2 clear: three literals, which land at window positions 1..3;
        # bit 3 set: copy 3 bytes from window position 1
        stream = bytes([0b11111000]) + b"ABC" + backref(1, 3)
        self.assertEqual(ta3do.lz77_decompress(stream, 6), b"ABCABC")

    def test_backreference_can_overlap_what_it_is_writing(self):
        stream = bytes([0b11111110]) + b"A" + backref(1, 4)
        self.assertEqual(ta3do.lz77_decompress(stream, 5), b"AAAAA")

    def test_zero_offset_ends_the_stream(self):
        stream = bytes([0b11111110]) + b"X" + backref(0, 2)
        self.assertEqual(ta3do.lz77_decompress(stream, 99), b"X")

    def test_stops_at_the_wanted_length(self):
        stream = bytes([0b00000000]) + b"12345678" + bytes([0b00000000]) + b"abcdefgh"
        self.assertEqual(ta3do.lz77_decompress(stream, 3), b"123")
        self.assertEqual(ta3do.lz77_decompress(stream, 12), b"12345678abcd")


class TestHpi(unittest.TestCase):
    def archive(self, files, key=0):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test.hpi"
            path.write_bytes(make_hpi(files, key))
            archive = ta3do.HpiArchive(path)
            return archive, {name: archive.read(name) for name in files}

    def test_plain_archive_round_trip(self):
        payload = {"objects3d/thing.3do": b"\x01\x00\x00\x00" + bytes(range(256)) * 4}
        archive, got = self.archive(payload)
        self.assertEqual(got["objects3d/thing.3do"], payload["objects3d/thing.3do"])

    def test_obfuscated_archive_round_trip(self):
        payload = {"units/x.fbi": b"[UNITINFO]{Objectname=X;}" * 40}
        _archive, got = self.archive(payload, key=0x27)
        self.assertEqual(got["units/x.fbi"], payload["units/x.fbi"])

    def test_multi_chunk_file(self):
        payload = {"big.bin": bytes((i * 31) % 251 for i in range(150000))}
        _archive, got = self.archive(payload)
        self.assertEqual(got["big.bin"], payload["big.bin"])

    def test_missing_file_raises(self):
        archive, _ = self.archive({"a.txt": b"hello"})
        with self.assertRaises(ta3do.HpiError):
            archive.read("nope.txt")

    def test_names_are_case_insensitive(self):
        archive, _ = self.archive({"Mixed.TXT": b"hello"})
        self.assertEqual(archive.read("mixed.txt"), b"hello")


# --------------------------------------------------------------------------- GAF


class TestGaf(unittest.TestCase):
    def test_entry_names_are_indexed_upper_case(self):
        blob = make_gaf("Metal1b", 2, 1, [rle_row(literal_run([7, 8]))])
        self.assertEqual(list(ta3do.gaf_entries(blob)), ["METAL1B"])

    def test_rle_runs(self):
        blob = make_gaf("T", 6, 2, [
            rle_row(literal_run([1, 2]), repeat_run(3, 5), transparent_run(1)),
            rle_row(transparent_run(2), literal_run([9]), repeat_run(3, 4)),
        ])
        offset = ta3do.gaf_entries(blob)["T"]
        frame = ta3do.gaf_frame(blob, offset)
        self.assertEqual(frame.width, 6)
        self.assertEqual(list(frame.pixels[:6]), [1, 2, 5, 5, 5, 9])
        self.assertEqual(list(frame.pixels[6:]), [9, 9, 9, 4, 4, 4])

    def test_transparent_index_is_the_fill(self):
        blob = make_gaf("T", 4, 1, [rle_row(transparent_run(4))])
        frame = ta3do.gaf_frame(blob, ta3do.gaf_entries(blob)["T"])
        self.assertEqual(set(frame.pixels), {9})


# --------------------------------------------------------------------------- 3DO


def quad(indices=(0, 1, 2, 3), **kw):
    return dict(indices=list(indices), **kw)


SIMPLE_TREE = [
    dict(name="body", offset=(0, 0, 0), child=1,
         verts=[(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)],
         prims=[quad(color=42), quad(color=7)], selection=1),
    dict(name="turret", offset=(0, 2, 0), sibling=2,
         verts=[(-1, 0, -1), (1, 0, -1), (1, 0, 1)],
         prims=[quad(indices=(0, 1, 2), texture="metal1b")]),
    dict(name="flare", offset=(0, 3, -5),
         verts=[(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)],
         prims=[quad(color=13)]),
]


class TestParse3do(unittest.TestCase):
    def setUp(self):
        self.root = ta3do.parse_3do(make_3do(SIMPLE_TREE))

    def test_tree_shape_and_names(self):
        names = [piece.name for piece, _ in self.root.walk()]
        self.assertEqual(names, ["body", "turret", "flare"])
        self.assertEqual(len(self.root.children), 2)

    def test_fixed_point_scale(self):
        self.assertEqual(self.root.children[0].offset, (0.0, 2.0, 0.0))
        self.assertEqual(self.root.vertices[1], (1.0, 0.0, -1.0))

    def test_selection_primitive_is_flagged_on_the_root_only(self):
        self.assertFalse(self.root.primitives[0].is_selection)
        self.assertTrue(self.root.primitives[1].is_selection)
        turret = self.root.children[0]
        self.assertFalse(any(p.is_selection for p in turret.primitives))

    def test_texture_name_or_none(self):
        self.assertIsNone(self.root.primitives[0].texture)
        self.assertEqual(self.root.children[0].primitives[0].texture, "metal1b")

    def test_rejects_a_bad_version_word(self):
        blob = bytearray(make_3do(SIMPLE_TREE))
        struct.pack_into("<i", blob, 0, 3)
        with self.assertRaises(ValueError):
            ta3do.parse_3do(bytes(blob))


# --------------------------------------------------------------------------- geometry


class TestGeometry(unittest.TestCase):
    def test_fan_triangulation(self):
        self.assertEqual(ta3do.fan(3), [(0, 1, 2)])
        self.assertEqual(ta3do.fan(4), [(0, 1, 2), (0, 2, 3)])
        self.assertEqual(ta3do.fan(6), [(0, 1, 2), (0, 2, 3), (0, 3, 4), (0, 4, 5)])

    def test_newell_survives_collinear_leading_vertices(self):
        loop = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (2, 0, 2), (0, 0, 2)]
        normal = ta3do.newell_normal(loop)
        self.assertAlmostEqual(abs(normal[1]), 1.0, places=6)
        self.assertAlmostEqual(normal[0], 0.0, places=6)

    def test_degenerate_triangles_are_detected(self):
        self.assertTrue(ta3do._degenerate((0, 0, 0), (1, 0, 0), (2, 0, 0)))
        self.assertFalse(ta3do._degenerate((0, 0, 0), (1, 0, 0), (0, 1, 0)))

    def test_quad_uvs_are_the_rect_corners(self):
        points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        uvs = ta3do.face_uvs(points, 4, (0.25, 0.5, 0.75, 1.0))
        self.assertEqual(uvs, [(0.25, 0.5), (0.75, 0.5), (0.75, 1.0), (0.25, 1.0)])

    def test_triangle_uses_three_corners(self):
        uvs = ta3do.face_uvs([(0, 0, 0), (1, 0, 0), (1, 1, 0)], 3, (0.0, 0.0, 1.0, 1.0))
        self.assertEqual(uvs, [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0)])

    def test_ngon_uvs_stay_inside_the_rect(self):
        points = [(0, 0, 0), (2, 0, 0), (3, 0, 1), (2, 0, 2), (0, 0, 2), (-1, 0, 1)]
        uvs = ta3do.face_uvs(points, 6, (0.1, 0.2, 0.6, 0.9))
        for u, v in uvs:
            self.assertGreaterEqual(round(u, 6), 0.1)
            self.assertLessEqual(round(u, 6), 0.6)
            self.assertGreaterEqual(round(v, 6), 0.2)
            self.assertLessEqual(round(v, 6), 0.9)


# --------------------------------------------------------------------------- atlas


class TestAtlas(unittest.TestCase):
    def test_regions_do_not_overlap_and_carry_padding(self):
        atlas = ta3do.Atlas(PALETTE)
        for i, (w, h) in enumerate([(32, 32), (16, 16), (64, 8), (8, 64)]):
            atlas.request_texture(f"t{i}", frame_of(w, h))
        atlas.request_color(5)
        atlas.pack()
        boxes = list(atlas.rects.values())
        for i, (x1, y1, w1, h1) in enumerate(boxes):
            self.assertGreaterEqual(x1, ta3do.PAD)
            self.assertLessEqual(x1 + w1 + ta3do.PAD, atlas.size)
            for x2, y2, w2, h2 in boxes[i + 1:]:
                apart = (x1 + w1 <= x2 or x2 + w2 <= x1
                         or y1 + h1 <= y2 or y2 + h2 <= y1)
                self.assertTrue(apart, "atlas regions overlap")

    def test_colour_swatch_paints_the_palette_entry(self):
        atlas = ta3do.Atlas(PALETTE)
        key = atlas.request_color(77)
        atlas.pack()
        x, y, _, _ = atlas.rects[key]
        offset = (y * atlas.size + x) * 4
        self.assertEqual(tuple(atlas.rgba[offset:offset + 4]), PALETTE[77] + (255,))

    def test_transparent_index_becomes_alpha_zero(self):
        atlas = ta3do.Atlas(PALETTE)
        key = atlas.request_texture("clear", frame_of(4, 4, fill=9, transparent=9))
        atlas.pack()
        x, y, _, _ = atlas.rects[key]
        offset = (y * atlas.size + x) * 4
        self.assertEqual(atlas.rgba[offset + 3], 0)

    def test_uv_rect_is_normalised(self):
        atlas = ta3do.Atlas(PALETTE)
        key = atlas.request_texture("t", frame_of(16, 16))
        atlas.pack()
        u0, v0, u1, v1 = atlas.uv_rect(key)
        self.assertAlmostEqual(u1 - u0, 16 / atlas.size)
        self.assertTrue(0.0 <= u0 < u1 <= 1.0 and 0.0 <= v0 < v1 <= 1.0)


# --------------------------------------------------------------------------- model


class TestBuildModel(unittest.TestCase):
    def build(self, **kw):
        root = ta3do.parse_3do(make_3do(SIMPLE_TREE))
        bank = FakeBank({"METAL1B": frame_of(8, 8)})
        return ta3do.build_model("test", root, bank, PALETTE, **kw)

    def test_selection_primitive_is_dropped_by_default(self):
        model = self.build()
        self.assertEqual(model.stats["skipped_selection"], 1)
        self.assertEqual(model.stats["triangles"], 2 + 1 + 2)  # quad, tri, quad

    def test_selection_primitive_can_be_kept(self):
        model = self.build(keep_selection=True)
        self.assertEqual(model.stats["skipped_selection"], 0)
        self.assertEqual(model.stats["triangles"], 2 + 2 + 1 + 2)

    def test_hidden_pieces_lose_faces_but_keep_their_node(self):
        model = self.build(hidden={"flare"})
        names = [mesh.name for mesh in model.meshes]
        self.assertIn("flare", names)
        flare = next(m for m in model.meshes if m.name == "flare")
        self.assertEqual(flare.triangles, 0)
        self.assertEqual(model.stats["skipped_hidden"], 1)

    def test_z_flip_and_winding_reversal_preserve_the_facing(self):
        """Mirroring Z reverses a face; reversing the vertex order reverses it
        back. The normal must therefore come out as the file's own normal with
        its Z negated — the same physical direction, in the new axes."""
        model = self.build()
        raw = ta3do.newell_normal([(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)])
        want = (round(raw[0], 6), round(raw[1], 6), round(-raw[2], 6))
        got = tuple(round(v, 6) for v in model.root.normals[:3])
        self.assertEqual(got, want)

        flare_raw = ta3do.newell_normal([(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)])
        flare = next(m for m in model.meshes if m.name == "flare")
        rebuilt = ta3do.build_model("t", ta3do.parse_3do(make_3do(SIMPLE_TREE)),
                                    FakeBank(), PALETTE)
        flare = next(m for m in rebuilt.meshes if m.name == "flare")
        self.assertEqual(tuple(round(v, 6) for v in flare.normals[:3]),
                         (round(flare_raw[0], 6), round(flare_raw[1], 6),
                          round(-flare_raw[2], 6)))

    def test_piece_offsets_follow_the_same_z_flip(self):
        model = self.build()
        self.assertEqual(model.root.translation, (0.0, 0.0, 0.0))
        flare = next(m for m in model.meshes if m.name == "flare")
        self.assertEqual(flare.translation, (0.0, 3.0, 5.0))

    def test_missing_texture_falls_back_to_the_flat_colour(self):
        root = ta3do.parse_3do(make_3do(SIMPLE_TREE))
        model = ta3do.build_model("test", root, FakeBank(), PALETTE)
        self.assertEqual(model.stats["missing_textures"], ["metal1b"])
        self.assertIn(("col", 0), model.atlas.rects)

    def test_bounds_cover_the_posed_tree(self):
        model = self.build()
        lo, hi = model.bounds()
        self.assertAlmostEqual(hi[1], 3.0 + 1.0)     # flare sits at y=3, one unit tall
        self.assertAlmostEqual(hi[2], 5.0)           # its z=-5 offset flips to +5
        self.assertAlmostEqual(lo[0], -1.0)


# --------------------------------------------------------------------------- glTF


class TestGltf(unittest.TestCase):
    def setUp(self):
        root = ta3do.parse_3do(make_3do(SIMPLE_TREE))
        self.model = ta3do.build_model("test", root, FakeBank({"METAL1B": frame_of(8, 8)}),
                                       PALETTE, hidden={"flare"})
        self.doc, self.blob = ta3do.model_to_gltf(self.model)
        self.glb = ta3do.write_glb(self.doc, self.blob)

    def test_document_is_a_valid_shape(self):
        self.assertEqual(self.doc["asset"]["version"], "2.0")
        self.assertEqual(self.doc["scenes"][0]["nodes"], [0])
        self.assertEqual(len(self.doc["nodes"]), 3)
        self.assertEqual(self.doc["nodes"][0]["children"], [1, 2])
        self.assertNotIn("mesh", self.doc["nodes"][2])   # the hidden flare
        self.assertEqual(len(self.doc["materials"]), 1)
        self.assertEqual(self.doc["images"][0]["mimeType"], "image/png")

    def test_accessors_are_in_range_and_have_bounds(self):
        for accessor in self.doc["accessors"]:
            view = self.doc["bufferViews"][accessor["bufferView"]]
            self.assertLessEqual(view["byteOffset"] + view["byteLength"],
                                 self.doc["buffers"][0]["byteLength"])
            if accessor["type"] == "VEC3" and view.get("target") == 34962:
                self.assertEqual(len(accessor["min"]), 3)

    def test_positions_normals_and_uvs_have_the_same_count(self):
        for mesh in self.doc["meshes"]:
            attrs = mesh["primitives"][0]["attributes"]
            counts = {self.doc["accessors"][i]["count"] for i in attrs.values()}
            self.assertEqual(len(counts), 1)
            self.assertEqual(counts.pop() % 3, 0)

    def test_glb_container(self):
        magic, version, length = struct.unpack_from("<III", self.glb, 0)
        self.assertEqual(magic, ta3do.GLB_MAGIC)
        self.assertEqual(version, 2)
        self.assertEqual(length, len(self.glb))
        json_len, json_tag = struct.unpack_from("<II", self.glb, 12)
        self.assertEqual(json_tag, ta3do.CHUNK_JSON)
        self.assertEqual(json_len % 4, 0)
        doc = json.loads(self.glb[20:20 + json_len])
        bin_len, bin_tag = struct.unpack_from("<II", self.glb, 20 + json_len)
        self.assertEqual(bin_tag, ta3do.CHUNK_BIN)
        self.assertEqual(bin_len % 4, 0)
        self.assertGreaterEqual(bin_len, doc["buffers"][0]["byteLength"])

    def test_hidden_pieces_are_recorded_in_extras(self):
        self.assertEqual(self.doc["asset"]["extras"]["hiddenPieces"], ["flare"])


# --------------------------------------------------------------------------- COB


class TestCob(unittest.TestCase):
    def test_create_prologue_hides_are_collected(self):
        code = [ta3do.OP_HIDE, 1, ta3do.OP_HIDE, 3,
                0x10021001, 3000, 0x10023004, 0,      # push constant, pop static
                0x10061000, 0, 0]                     # START — the scan must stop here
        blob = make_cob(["base", "flare", "turret", "flare2"], {"Create": code})
        self.assertEqual(ta3do.hidden_at_create(blob), {"flare", "flare2"})

    def test_show_cancels_an_earlier_hide(self):
        code = [ta3do.OP_HIDE, 1, ta3do.OP_SHOW, 1, 0x10065000]
        blob = make_cob(["base", "flare"], {"Create": code})
        self.assertEqual(ta3do.hidden_at_create(blob), set())

    def test_scan_stops_at_an_unknown_opcode(self):
        code = [0x10043000, ta3do.OP_HIDE, 1]         # GET first: we cannot skip it
        blob = make_cob(["base", "flare"], {"Create": code})
        self.assertEqual(ta3do.hidden_at_create(blob), set())

    def test_no_create_script_means_nothing_hidden(self):
        blob = make_cob(["base"], {"Killed": [0x10065000]})
        self.assertEqual(ta3do.hidden_at_create(blob), set())

    def test_garbage_is_not_fatal(self):
        self.assertEqual(ta3do.hidden_at_create(b"not a cob at all"), set())


# --------------------------------------------------------------------------- PNG


class TestPng(unittest.TestCase):
    def test_round_trip(self):
        pixels = bytes(bytearray(range(256)) * 4)[:8 * 8 * 4]
        blob = ta3do.write_png(8, 8, pixels)
        width, height, channels, back = ta3do.read_png(blob)
        self.assertEqual((width, height, channels), (8, 8, 4))
        self.assertEqual(back, pixels)

    def test_coverage_counts_only_opaque_pixels(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty = Path(tmp) / "empty.png"
            empty.write_bytes(ta3do.write_png(4, 4, bytes(4 * 4 * 4)))
            self.assertEqual(ta3do.png_coverage(empty), 0.0)
            half = bytearray(4 * 4 * 4)
            for i in range(8):
                half[i * 4 + 3] = 255
            painted = Path(tmp) / "half.png"
            painted.write_bytes(ta3do.write_png(4, 4, bytes(half)))
            self.assertAlmostEqual(ta3do.png_coverage(painted), 0.5)

    def test_contact_sheet_lays_tiles_side_by_side(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = []
            for i in range(3):
                path = Path(tmp) / f"{i}.png"
                path.write_bytes(ta3do.write_png(10, 6, bytes([200, 0, 0, 255] * 60)))
                paths.append(path)
            width, height, channels, _ = ta3do.read_png(ta3do.contact_sheet(paths, gap=2))
            self.assertEqual((width, height), (3 * 10 + 4 * 2, 6 + 2 * 2))
            self.assertEqual(channels, 4)


# --------------------------------------------------------------------------- standard


class TestViewStandard(unittest.TestCase):
    def test_default_set_is_the_five_named_views(self):
        self.assertEqual(ta3do.DEFAULT_VIEWS,
                         ["front", "side", "top", "back", "quarter"])
        for name in ta3do.DEFAULT_VIEWS:
            self.assertIn(name, ta3do.VIEWS)

    def test_side_is_the_right_hand_side(self):
        self.assertEqual(ta3do.VIEWS["side"], ta3do.VIEWS["right"])

    def test_front_and_back_are_opposite(self):
        self.assertEqual(abs(ta3do.VIEWS["front"][0] - ta3do.VIEWS["back"][0]), 180.0)

    def test_the_viewer_page_carries_the_same_table(self):
        """The HTML repeats the table so it works standalone; drift would be silent."""
        html = ta3do.VIEWER_HTML.read_text()
        block = re.search(r"const VIEWS = \{(.+?)\};", html, re.S).group(1)
        found = {}
        for name, az, el in re.findall(
                r"(\w+):\s*\{\s*az:\s*(-?[\d.]+),\s*el:\s*(-?[\d.]+)", block):
            found[name] = (float(az), float(el))
        self.assertEqual(found, {k: (float(v[0]), float(v[1]))
                                 for k, v in ta3do.VIEWS.items()})


if __name__ == "__main__":
    unittest.main()
