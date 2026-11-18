#!/usr/bin/env python3
"""thinkmap.py -- the ground under thinkview.py's war room: the map's picture from the game's own map file, and the
heights, the metal and the features from the recording's feature grid.

TWO SOURCES, and the page says which a layer is drawn from:

- THE RECORDING holds the engine's feature grid at every tick (tarec.py's World.grid, 13 bytes a 16-px cell:
  `+0x04` the height, `+0x07` the metal, `+0x08` the feature's def, 0xFFFB and up no anchor; research/notes/engine/
  map.md, *The cell*). `Track` keeps the heights and the metal as the first sample holds them, and every feature's
  anchor from the sample it is first seen at to the sample it is gone at. They are the engine's own values, whatever
  the mod; they are SAMPLED, so a wreck made and reclaimed between two samples is never seen.
- THE MAP FILE, `maps/<name>.tnt`, gives the picture (its tiles through `palettes/palette.pal`), the sea level and
  the names of the features' defs: def i below the TNT's own count is the i-th name of its table (LoadMap loads
  them in that order before any other, map.md, *The FeatureDefs' order*), and the name's section is the first
  `features/**.tdf` that holds it. A def past the table was loaded later -- a schema's feature or a wreck -- and is
  shown without a name.

THE ARCHIVES ARE THE RECORDING'S OWN. A recording's header lists the archives its game mounted, in mount order,
each with its size (RT_ARCHIVE), so a file is opened as that game opened it: loose in a game directory first, then
the first listed archive that holds it. An archive is looked for by name in each `--gamedir` and in its
`mods/*/` folders (a managed mod's archives stand there); one of another size is used and named, one not found is
named, and a file is REFUSED when an archive that was not found stands before the one that holds it, since that
archive may have held another copy. A recording that lists none is read by tavfs.py's mount order of the first
game directory. IMPURE'S OWN ARCHIVE, impure-patch.ufo, is left out: the DLL writes it at every start, each build
its own size, and it holds Impure's menu screens alone (`impure/`, `anims/`, `bitmaps/`; tagpu_menu.c, UFO_FILE),
none of the folders read here (`maps/`, `palettes/`, `features/`, `units/`, `textures/`).

THE PLAYERS' COLOURS (team_colours) are the game's own: colour index k is the middle pixel of frame k of the
sequence the engine loads its player colours from (`32xlogos` of `textures/logos.gaf`, loaded at 0x431911;
research/notes/engine/recorder.md, `main+0x148DB`), through `palettes/palette.pal`, as the replay HUD takes a
seat's colour (tagpu_replay.c, `tagpu_replay_logo_colour`). A mod's archives give the mod's colours.

THE PICTURE is the whole map as one image: every 32-px tile reduced by a power of two, then the image resized so
its long side is at most IMAGE_SIDE, and written as WebP at the best quality that keeps it under IMAGE_BYTES. It
is DERIVED FROM THE ORIGINAL GAME: it is written beside the page's data and nowhere else.

THE GAME'S VIEW. The tiles are drawn as the game shows the ground: a point at (x, z) and height h stands at
(x, z - h / 2) on the picture. The page lifts every marker the same way (thinkview.html, `lift`), from the
recording's heights, which `ground` hands it a value every HSTEP cells.

Pillow and numpy are needed for the picture and the layers; without them the page is the plain grid, and says so."""
import base64
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]

CELL_PX, TILE_PX = 16, 32
IMAGE_SIDE = 4096               # the picture's long side at most, px
IMAGE_BYTES = 4_000_000         # ... and its file's size
QUALITIES = (82, 74, 66, 58, 50, 42, 34)
HEIGHT_SIDE = 320               # the heights handed to the page: at most this many values a side
REC_CELL = 13                   # tarec.REC_CELL_BYTES
NO_ANCHOR = 0xFFFB              # a def from here up anchors no feature
OWN_ARCHIVE = "impure-patch.ufo"    # IMPURE'S OWN ARCHIVE
F_EXTRACTS, F_FLOATS, F_FLIES = 1, 2, 4     # a unit type's facts, as the page reads them (D.kinds)


class MapError(Exception):
    """What could not be read or rendered: the page prints it."""


def _tavfs():
    sys.path.insert(0, str(TREE / "tools" / "wiki"))
    import tavfs
    return tavfs


def atoi(v, default=0) -> int:
    """C's atoi over a TDF value: its leading integer."""
    m = re.match(r"\s*([-+]?\d+)", str(v or ""))
    return int(m.group(1)) if m else default


def number(v) -> float:
    m = re.match(r"\s*([-+]?\d+(?:\.\d+)?)", str(v or ""))
    return float(m.group(1)) if m else 0.0


class Source:
    """A game's files as the recording's game opened them (THE ARCHIVES ARE THE RECORDING'S OWN)."""

    def __init__(self, listed, dirs):
        """`listed`: the header's [(archive's name, size, ...)] in mount order, or None; `dirs`: game directories."""
        self.tavfs = _tavfs()
        self.dirs = [Path(d) for d in dirs if d and Path(d).is_dir()]
        self.archives = []          # (name, HpiArchive or None, why it is not as listed or "")
        self.listed = bool(listed)
        if not self.dirs:
            raise MapError("no game directory to read the map file from (--gamedir)")
        if listed:
            for name, size, *_ in listed:
                if name.lower() == OWN_ARCHIVE:
                    continue
                path = self._find(name, size)
                arc, why = None, "not found"
                if path is not None:
                    try:
                        arc = self.tavfs.ta3do.HpiArchive(path)
                        why = "" if path.stat().st_size == size else f"{path.stat().st_size} bytes, the recording's {size}"
                    except Exception as e:                              # noqa: BLE001
                        why = f"unreadable ({type(e).__name__})"
                self.archives.append((name, arc, why))
        else:
            for d in self.dirs:
                try:
                    vfs = self.tavfs.Vfs(d)
                except Exception:                                       # noqa: BLE001
                    continue
                self.archives = [(name, arc, "") for name, arc in vfs.archives]
                self.dirs = [d]
                break
            else:
                raise MapError("the recording lists no archive and no game directory holds a TotalA.exe to mount by")

    def _find(self, name: str, size: int):
        """The archive file `name`: one of the listed size first, else one of the name."""
        hits = []
        for d in self.dirs:
            for folder in [d] + sorted(p for p in (d / "mods").glob("*") if p.is_dir()):
                try:
                    hits += [p for p in folder.iterdir() if p.name.lower() == name.lower() and p.is_file()]
                except OSError:
                    pass
        for p in hits:
            if p.stat().st_size == size:
                return p
        return hits[0] if hits else None

    def where(self, path: str):
        """(where `path` opens from, the listed archives before it that were not found)."""
        key = self.tavfs._key(path)
        for d in self.dirs:
            if self.tavfs.loose_path(d, path):
                return "loose", []
        missing = []
        for name, arc, _why in self.archives:
            if arc is None:
                missing.append(name)
            elif key in arc.files:
                return name, missing
        return None, missing

    def open(self, path: str) -> tuple:
        """(bytes, where) of `path`; MapError when nothing holds it or an archive not found stands before it."""
        where, missing = self.where(path)
        if where is None:
            raise MapError(f"no {path} in the recording's archives" + (f" (not found: {', '.join(missing)})" if missing else ""))
        if missing:
            raise MapError(f"{path} is in {where}, but {', '.join(missing)} stood before it in the recording's game "
                           "and was not found: it may have held another copy")
        if where == "loose":
            return next(p for p in (self.tavfs.loose_path(d, path) for d in self.dirs) if p).read_bytes(), where
        return next(arc for name, arc, _w in self.archives if name == where).read(self.tavfs._key(path)), where

    def enumerate(self, top: str, ext: str):
        """[(path, read)] of every file under `top` ending `ext`, `read()` its bytes: the loose files first, then
        the archives in mount order, each path from the first source that holds it."""
        prefix, seen, out = self.tavfs._key(top) + "/", set(), []
        for d in self.dirs:
            base = next((c for c in d.iterdir() if c.is_dir() and c.name.lower() == self.tavfs._key(top)), None)
            for f in sorted(base.rglob("*"), key=lambda f: str(f).lower()) if base else []:
                path = prefix + f.relative_to(base).as_posix().lower()
                if f.is_file() and path.endswith(ext) and path not in seen:
                    seen.add(path)
                    out.append((path, f.read_bytes))
        for _name, arc, _why in self.archives:
            if arc is None:
                continue
            for path in arc.files:
                if path.startswith(prefix) and path.endswith(ext) and path not in seen:
                    seen.add(path)
                    out.append((path, lambda arc=arc, path=path: arc.read(path)))
        return out

    def notes(self) -> list:
        """What of the recording's archives is not as it listed them."""
        return [f"{name}: {why}" for name, _arc, why in self.archives if why]


def palette(blob: bytes) -> list:
    """palette.pal's 256 colours, [(r, g, b)]: four bytes a colour."""
    if len(blob) < 1024:
        raise MapError("palette.pal is shorter than 256 colours")
    return [tuple(blob[i * 4:i * 4 + 3]) for i in range(256)]


def render(tnt, pal, side=IMAGE_SIDE):
    """The whole map as a Pillow image, and the power of two each tile was reduced by before the resize."""
    import numpy as np
    from PIL import Image
    long = max(tnt.mw, tnt.mh) * TILE_PX
    n = 1
    while n < TILE_PX and long // (n * 2) >= side:
        n *= 2
    t = TILE_PX // n
    gfx = np.frombuffer(tnt.blob, np.uint8, tnt.tiles * TILE_PX * TILE_PX, tnt.p_tilegfx).reshape(tnt.tiles, TILE_PX, TILE_PX)
    rgb = np.asarray(pal, np.float32)[gfx]
    small = (rgb.reshape(tnt.tiles, t, n, t, n, 3).mean(axis=(2, 4)) + .5).astype(np.uint8)
    small = np.concatenate([small, np.zeros((1, t, t, 3), np.uint8)])       # a tile the map names and the file lacks
    tm = np.frombuffer(tnt.blob, "<u2", tnt.mw * tnt.mh, tnt.p_mapdata).reshape(tnt.mh, tnt.mw)
    tm = np.where(tm < tnt.tiles, tm, tnt.tiles)
    img = Image.fromarray(small[tm].transpose(0, 2, 1, 3, 4).reshape(tnt.mh * t, tnt.mw * t, 3), "RGB")
    if max(img.size) > side:
        k = side / max(img.size)
        img = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
    return img, n


def encode(img, limit=IMAGE_BYTES) -> tuple:
    """(the image as WebP bytes under `limit`, the quality it took, its size in px): the best of QUALITIES that
    fits, the image reduced by a fifth and tried again when none does."""
    import io
    from PIL import Image
    while True:
        for q in QUALITIES:
            buf = io.BytesIO()
            img.save(buf, "WEBP", quality=q, method=4)
            if buf.tell() <= limit:
                return buf.getvalue(), q, img.size
        if max(img.size) < 64:
            raise MapError(f"the picture does not fit {limit} bytes at any size")
        img = img.resize((max(1, img.width * 4 // 5), max(1, img.height * 4 // 5)), Image.LANCZOS)


def image_px(info: dict, x: float, z: float) -> tuple:
    """Where the picture shows the world's (x, z) px at height 0, in the image's px: the page's own transform
    (thinkview.html stretches the image over the map file's `px`)."""
    return x * info["w"] / info["px"][0], z * info["h"] / info["px"][1]


def feature_kind(body: dict) -> str:
    """A feature's kind as the page draws it: `vent` a geothermal vent, `metal` and `energy` what a reclaim gives
    the most of, `block` it blocks building and cannot be reclaimed, `decor` neither (a deposit's art, a scar).
    A key the section leaves out reads 0 [INFERRED: the engine's default for a missing key is not established]."""
    if atoi(body.get("geothermal")) == 1:
        return "vent"
    if atoi(body.get("reclaimable")) == 1:
        m, e = number(body.get("metal")), number(body.get("energy"))
        return "metal" if m >= e and m > 0 else "energy" if e > 0 else "metal"
    return "block" if atoi(body.get("blocking")) == 1 else "decor"


def feature_table(src: Source, names: list) -> tuple:
    """({def: [name, kind, footprint x, footprint z, metal, energy, what its section calls it]} of the TNT's table,
    the names no features/**.tdf holds)."""
    tascene = src.tavfs.tascene
    want = {n.lower() for n in names}
    found = {}
    for _path, read in src.enumerate("features", ".tdf"):
        if not want - set(found):
            break
        try:
            text = read().decode("latin-1")
        except Exception:                                               # noqa: BLE001
            continue
        if not any(n in text.lower() for n in want - set(found)):
            continue
        for sec, body in tascene.parse_tdf(text).items():
            if sec in want and isinstance(body, dict) and body:
                found.setdefault(sec, body)
    table, lost = {}, []
    for i, name in enumerate(names):
        body = found.get(name.lower())
        if body is None:
            lost.append(name)
            table[str(i)] = [name, "block", 1, 1, 0, 0, ""]
            continue
        table[str(i)] = [name, feature_kind(body), atoi(body.get("footprintx"), 1) or 1, atoi(body.get("footprintz"), 1) or 1,
                         round(number(body.get("metal"))), round(number(body.get("energy"))),
                         str(body.get("description", "")).strip()[:40]]
    return table, lost


def unit_kinds(src: Source, names: dict) -> dict:
    """{type index: its facts} of the types `names` ({index: UnitName}) whose `units/<name>.fbi` the archives
    hold: F_EXTRACTS (`ExtractsMetal` above 0), F_FLOATS (`Floater` or `canhover`: it stands on the water),
    F_FLIES (`canfly`). The facts are the mod's own, read from the recording's archives."""
    out = {}
    for i, name in names.items():
        try:
            blob, _where = src.open(f"units/{name}.fbi")
        except MapError:
            continue
        keys = src.tavfs.tascene.parse_tdf(blob.decode("latin-1")).get("unitinfo")
        if not isinstance(keys, dict):
            continue
        out[i] = (F_EXTRACTS if number(keys.get("extractsmetal")) > 0 else 0) \
            | (F_FLOATS if atoi(keys.get("floater")) == 1 or atoi(keys.get("canhover")) == 1 else 0) \
            | (F_FLIES if atoi(keys.get("canfly")) == 1 else 0)
    return out


LOGOS = ("textures/logos.gaf", "32xlogos")        # the file and the sequence the engine's player colours are frames of
LOGO_SIDE = 64                                    # a frame larger than this gives no colour (tagpu_replay_logo_colour)


def team_colours(src: Source) -> tuple:
    """([(r, g, b) or None, ...] a colour index, the archive they are read from): THE PLAYERS' COLOURS. None
    for a frame that is empty, larger than LOGO_SIDE a side or unreadable; MapError when the archives hold no
    such sequence or no palette."""
    pal = palette(src.open("palettes/palette.pal")[0])
    blob, where = src.open(LOGOS[0])
    ta3do = src.tavfs.ta3do
    try:
        sig, count = struct.unpack_from("<II", blob, 0)
        if sig != 0x00010100:
            raise ValueError
        frames = None
        for off in struct.unpack_from(f"<{count}I", blob, 12):
            if blob[off + 8:off + 40].split(b"\0")[0].decode("latin-1").lower() == LOGOS[1]:
                frames = [struct.unpack_from("<I", blob, off + 40 + 8 * i)[0] for i in range(struct.unpack_from("<H", blob, off)[0])]
                break
    except (struct.error, ValueError):
        raise MapError(f"{LOGOS[0]} in {where} is no GAF") from None
    if frames is None:
        raise MapError(f"{LOGOS[0]} in {where} holds no sequence {LOGOS[1]}")
    out = []
    for off in frames:
        try:
            fr = ta3do.gaf_frame(blob, off)
            ok = 0 < fr.width <= LOGO_SIDE and 0 < fr.height <= LOGO_SIDE
            out.append(tuple(pal[fr.pixels[(fr.height // 2) * fr.width + fr.width // 2]]) if ok else None)
        except Exception:                                               # noqa: BLE001
            out.append(None)
    return out, where


class Track:
    """The recording's feature grid over the samples (THE RECORDING): feed it tarec.py's World at each sampled
    tick, in tick order."""

    def __init__(self, gw: int, gh: int):
        import numpy as np
        self.np, self.gw, self.gh = np, gw, gh
        self.height = self.metal = self.defs = None
        self.first = None
        self.open = {}              # cell -> index into lives
        self.lives = []             # [cell, def, the sample it is first seen at, the sample it is gone at or None]

    def take(self, tick: int, grid) -> None:
        np = self.np
        g = np.frombuffer(grid, np.uint8).reshape(-1, REC_CELL)
        defs = g[:, 8].astype(np.uint16) | (g[:, 9].astype(np.uint16) << 8)
        if self.defs is None:
            self.first = tick
            self.height, self.metal = g[:, 4].copy(), g[:, 7].copy()
            changed = np.flatnonzero(defs < NO_ANCHOR)
            was = None
        else:
            changed = np.flatnonzero(defs != self.defs)
            was = self.defs
        for c in changed.tolist():
            if was is not None and c in self.open:
                self.lives[self.open.pop(c)][3] = tick
            d = int(defs[c])
            if d < NO_ANCHOR:
                self.open[c] = len(self.lives)
                self.lives.append([c, d, 0 if was is None else tick, None])
        self.defs = defs

    def ground(self, sea=None) -> dict:
        """The page's `ground`: the grid's size, the heights every `hstep` cells (a block's mean, base64 of a byte
        each), the metal as runs [value, cells, ...] in row order with the value most cells hold (`base`) and the
        most (`max`), the features' lives, and the sample all of it but the lives is read at."""
        np = self.np
        if self.defs is None:
            return {}
        k = 1
        while max(self.gw, self.gh) / k > HEIGHT_SIDE:
            k *= 2
        h = self.height.reshape(self.gh, self.gw).astype(np.float32)
        pad = np.pad(h, ((0, -self.gh % k), (0, -self.gw % k)), mode="edge")
        hs = pad.reshape(pad.shape[0] // k, k, pad.shape[1] // k, k).mean(axis=(1, 3))
        m = self.metal
        edges = np.flatnonzero(np.diff(m)) + 1
        starts = np.concatenate([[0], edges])
        lens = np.diff(np.concatenate([starts, [len(m)]]))
        rle = np.stack([m[starts].astype(np.int64), lens]).T.reshape(-1).tolist()
        counts = np.bincount(m, minlength=256)
        return {"gw": self.gw, "gh": self.gh, "at": self.first, "hstep": k, "hw": hs.shape[1], "hh": hs.shape[0],
                "h": base64.b64encode((hs + .5).astype(np.uint8).tobytes()).decode(), "sea": sea,
                "metal": {"rle": rle, "base": int(counts.argmax()), "max": int(m.max()) if len(m) else 0},
                "lives": self.lives}


def tnt_of(src: Source, name: str):
    """(the map's Tnt, where its file opened from)."""
    path = f"maps/{name}.tnt"
    blob, where = src.open(path)
    try:
        return src.tavfs.tascene.Tnt(blob, path), where
    except ValueError as e:
        raise MapError(f"{path} from {where}: {e}") from None


def checked(tnt, track: Track) -> list:
    """What of the map file is not the recording's ground; MapError when its size is another. The heights are
    compared cell for cell: equal, the file is the map the game played."""
    if (tnt.w16, tnt.h16) != (track.gw, track.gh):
        raise MapError(f"{tnt.name} is {tnt.w16} x {tnt.h16} cells, the recording's grid {track.gw} x {track.gh}: "
                       "not the map this game played")
    np = track.np
    differ = int((np.frombuffer(tnt.heights(), np.uint8) != track.height).sum()) if track.height is not None else 0
    return [f"{differ} of its {track.gw * track.gh} cells' heights are not the recording's"] if differ else []
