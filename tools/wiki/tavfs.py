"""tavfs.py -- the game's files as the running engine finds them: loose first, then the first archive in mount order.

    from tavfs import Vfs, find_install
    vfs = Vfs(find_install(None))
    blob, where = vfs.open("maps\\Crossing Two Rivers.tnt")   # None when nothing holds it
    vfs.feature("lushmetal3")                                  # (section dict, the TDF's path, where) or None

The rules are the engine's (research/notes/ingame-wiki-3d-stage-plan.md, *How the engine opens a file*, and
exe-reverse-engineering.md, *Where the engine looks for archives*):

- **The mount list.** InitTAHPIAry 0x41D4C0 mounts four bare relative patterns from the game's directory, in this
  order: the executable's main-archive pattern (`"rev%s.GP3"` at 0x5028CC formatted with `"31"` at 0x5028D8 on
  retail; a mod's executable renames it, `TAE%s.GP3`, `may%s.GP3`, `ProTA.gp3`, ...), `*.CCX`, `*.UFO`, `*.HPI`.
  The pattern is read out of the executable at those addresses, so a mod's own archive mounts first here as it does
  in the game. Within one pattern the files mount in name order without case [INFERRED: the order FindFirstFile
  lists on NTFS]. The mounter 0x4BE0B0 skips a path already mounted, and OpenHAPIFile 0x4BDD70 refuses, silently, a
  file whose last 36 bytes are not `Copyright <year> Cavedog Entertainment` for any four year bytes: it copies
  "0000" (0x50390C) over the file's year before comparing with the template at 0x4FDBF0, which reads
  "Copyright 0000 ..." (0x4BDE72..0x4BDED4, DISASSEMBLED 2026-10-26; the retail archives carry 1997 to 2000). The
  drive roots' `%c:\\*.hpi` are not mounted here: they are the CD's. A level's list holds every archive by then, so
  `*.HPI`'s ten mounts a call (0x41D5C0) do not bound it here.
- **A path** opens loose first, relative to the game's directory as the engine's `fopen` (0x4E4990) takes it
  relative to the process's current directory, then from the first archive in mount order that holds it as a file
  (0x4BB2E0 walks the list from index 0 and stops at the first file entry, 0x4BB378). Names compare without case
  [INFERRED for 0x4F8A70], and the path splits at `\\` only (0x4BB4EA); `/` is accepted here as a convenience.
- **An enumeration** lists loose files first, then each archive in mount order (0x4BCA30, 0x4BCB50), and **each path
  once, from the first source that holds it**: the copy 0x4BB2E0 opens. The engine does not de-duplicate in the
  enumerators; InitTAHPIAry ends with 0x4BE320, which sets bit 1 of every directory entry a loose file or an earlier
  archive already holds as a file (`or cl,2` at 0x4BE658), and the find-next 0x4BC640 skips an entry with that bit
  (`test byte [edi+8],2` at 0x4BC735). Directories are never shadowed. Within one source the order is the
  directory's: an archive's forwards (0x4BC6FF..0x4BC754), the loose files as the host's `_findfirst` lists them,
  here sorted without case as NTFS lists them [INFERRED for the loose order]. DISASSEMBLED 2026-10-26 (the 3D
  stage's scout answers, A1).
- **A feature section** comes from the first TDF of `0x4BCB50("features", "*.tdf", -1, 1)` that holds it
  (LoadFeature 0x4224B0, cited).

- **A file exists** for the engine when its size is above zero: the open test 0x4BBC40 returns the size (the archive
  entry's, or filelength for a loose file) and every probe tests that value, so a 0-byte file is a missing one.
  ProTA ships 32 0-byte .GUI pages as page stops (MEASURED, the Q1 scout answer).

The readers are the repository's: tools/ta3do's HPI reader and tools/tascene's TDF and TNT readers, and `tdf_sections`
below for the files the engine reads by position.
"""

import fnmatch
import importlib.util
import os
import struct
from importlib.machinery import SourceFileLoader
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent


def _load(name, path):
    loader = SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


tascene = _load("tascene", TOOLS / "tascene")
ta3do = tascene.ta3do

VA_STR_ARCHIVE = 0x5028CC       # "rev%s.GP3" on retail: the first pattern InitTAHPIAry mounts (0x41D4E0)
VA_STR_ARCHIVE_ARG = 0x5028D8   # "31", the argument it is formatted with
TRAILER_HEAD, TRAILER_TAIL = b"Copyright ", b" Cavedog Entertainment"   # a four-byte year between them
TRAILER_LEN = len(TRAILER_HEAD) + 4 + len(TRAILER_TAIL)
EXE_NAME = "TotalA.exe"


class VfsError(Exception):
    pass


def find_install(arg):
    """--gamedir, else TA3DO_GAMEDIR, else Steam's default library, else ta3do's gamedir."""
    for cand in (arg, os.environ.get("TA3DO_GAMEDIR"),
                 Path.home() / ".local/share/Steam/steamapps/common/Total Annihilation"):
        if cand and Path(cand).expanduser().is_dir():
            return Path(cand).expanduser()
    return ta3do.game_dir()


def exe_cstr(blob: bytes, va: int, limit: int = 64) -> str:
    """The NUL-terminated string at virtual address `va` of a PE image, through its section table."""
    if blob[:2] != b"MZ" or len(blob) < 0x40:
        raise VfsError("not a PE executable")
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if pe + 24 > len(blob) or blob[pe:pe + 4] != b"PE\0\0":
        raise VfsError("no PE header")
    nsec, opt_size = struct.unpack_from("<H12xH", blob, pe + 6)
    opt = pe + 24
    if opt + opt_size > len(blob):
        raise VfsError("PE optional header past the file")
    base = struct.unpack_from("<I", blob, opt + 28)[0]
    rva = va - base
    for i in range(nsec):
        sec = opt + opt_size + i * 40
        if sec + 40 > len(blob):
            break
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", blob, sec + 8)
        if vaddr <= rva < vaddr + max(vsize, rsize):
            off = raddr + rva - vaddr
            if not raddr <= off < min(len(blob), raddr + rsize):
                break
            raw = blob[off:off + limit]
            end = raw.find(b"\0")
            if end < 0:
                raise VfsError(f"no string end within {limit} bytes at 0x{va:X}")
            return raw[:end].decode("latin-1")
    raise VfsError(f"0x{va:X} is in no section of the executable")


def mount_patterns(exe: bytes, first: str = None):
    """InitTAHPIAry's four patterns, the first as this executable spells it unless `first` names it: a mod's loader
    can rewrite that string in memory before the engine reads it, which the executable's file does not show."""
    if first is None:
        first = exe_cstr(exe, VA_STR_ARCHIVE)
        if "%s" in first:
            first = first.replace("%s", exe_cstr(exe, VA_STR_ARCHIVE_ARG), 1)
    return [first, "*.CCX", "*.UFO", "*.HPI"]


# --------------------------------------------------------------------------- the engine's TDF grammar, by position

class TdfError(Exception):
    """A file the engine's parser refuses: in the game this is the "Parse error in .TDF File!" box and exit(1)."""


_BLANK = b" \t\r\n"


def _strip_comments(t: bytearray):
    """0x4C33A0, once over the whole text before parsing: `//` blanks up to (not including) the next LF; `/*` blanks
    up to and including the first `*/` (not nested; unterminated, it blanks to the end except the last byte).
    Comments are recognised anywhere, inside a value too."""
    n, i = len(t), 0
    while i < n:
        if t[i] == 0x2F and i + 1 < n and t[i + 1] == 0x2F:
            while i < n and t[i] != 0x0A:
                t[i] = 0x20
                i += 1
            continue
        if t[i] == 0x2F and i + 1 < n and t[i + 1] == 0x2A:
            t[i] = t[i + 1] = 0x20
            i += 2
            # the closing test looks at the byte before i, which the opener's own '*' never is: it is blanked
            while i < n:
                if t[i - 1] == 0x2A and t[i] == 0x2F:
                    t[i - 1] = t[i] = 0x20
                    i += 1
                    break
                t[i - 1] = 0x20
                i += 1
            continue
        i += 1


def _trim(b: bytes) -> str:
    return b.strip(_BLANK).decode("latin-1")


def tdf_sections(blob):
    """The top-level sections of a TDF-grammar file in FILE ORDER, as the engine's parser 0x4C2F60 builds them, each
    `{"_name": str, "keys": {lowercased key: str}, "sub": [nested sections, in order]}`.

    Not tascene.parse_tdf and not tagpu_tdf: those merge sections that share a name, and the engine's .GUI reader
    0x4AEAC0 walks the sections by index (0x4C3490(i)), one gadget each, so retail ARMCK1.GUI's repeated GADGET2..6
    are distinct gadgets. The grammar is the engine's (DISASSEMBLED, the Q1 scout answer):

    - the text ends at its first NUL byte (0x4C30A8 makes the file a C string); comments as `_strip_comments`;
    - blanks are space, TAB, CR and LF only;
    - `[name]` searches for `]` through the rest of the file; the name is trimmed and may be empty or repeated; the
      next non-blank byte must be `{`;
    - `}` closes a section; at the top level a stray `}` ends the whole parse silently;
    - anything else is a key: `=` and then `;` are searched through the rest of the file, so a value missing its `;`
      runs on to the next `;` anywhere later (retail ARMGEN.GUI's `attribs=256` swallows `colorf=15`);
    - a key given twice keeps the LAST value (0x4C3F98..0x4C403C replaces it), matched without case;
    - end of text inside a section, a missing `]`, `{`, `=` or `;` raise TdfError (the engine exits).

    A 0-byte file is a failed load for the engine (0x4C2F9A), not an empty one: TdfError too."""
    if isinstance(blob, str):
        blob = blob.encode("latin-1")
    if not blob:
        raise TdfError("an empty file, which the engine does not load")
    end = blob.find(b"\0")
    t = bytearray(blob if end < 0 else blob[:end])
    _strip_comments(t)
    s, n, pos = bytes(t), len(t), 0
    root = {"_name": "root", "keys": {}, "sub": []}
    stack = [root]
    while True:
        while pos < n and s[pos] in _BLANK:
            pos += 1
        cur = stack[-1]
        if pos >= n:
            if len(stack) > 1:
                raise TdfError(f"end of file inside [{cur['_name']}]")
            break
        c = s[pos]
        if c == 0x5B:                                   # '['
            j = s.find(b"]", pos)
            if j < 0:
                raise TdfError("a section's closing ']' is not found")
            name = _trim(s[pos + 1:j])
            k = j + 1
            while k < n and s[k] in _BLANK:
                k += 1
            if k >= n or s[k] != 0x7B:
                raise TdfError(f"no '{{' after [{name}]")
            sec = {"_name": name, "keys": {}, "sub": []}
            cur["sub"].append(sec)
            stack.append(sec)
            pos = k + 1
            continue
        if c == 0x7D:                                   # '}'
            if len(stack) == 1:
                break
            stack.pop()
            pos += 1
            continue
        j = s.find(b"=", pos)
        if j < 0:
            raise TdfError(f"a key in [{cur['_name']}] has no '='")
        k = s.find(b";", j + 1)
        if k < 0:
            raise TdfError(f"a value in [{cur['_name']}] has no ';'")
        cur["keys"][_trim(s[pos:j]).lower()] = _trim(s[j + 1:k])
        pos = k + 1
    return root["sub"]


def gui_sections(blob):
    """A .GUI file's gadgets as the reader 0x4AEAC0 takes them: `tdf_sections`, one gadget per top-level section,
    whatever the names. The engine's gadget count is the section count: it overwrites HEADER's `totalgadgets` with
    count − 1 (0x4AED48), so the number a file states is not read. A gadget's `[COMMON]` is `gadget_common`."""
    return tdf_sections(blob)


def gadget_common(sec):
    """The section's first nested section named COMMON without case (0x4AD355 → 0x4C3410), or None: the gadget's
    record then stays zero, which is id 0."""
    return next((s for s in sec["sub"] if s["_name"].lower() == "common"), None)


def _has_trailer(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            if f.tell() < TRAILER_LEN:
                return False
            f.seek(-TRAILER_LEN, os.SEEK_END)
            tail = f.read()
            return tail.startswith(TRAILER_HEAD) and tail.endswith(TRAILER_TAIL)
    except OSError:
        return False


def _key(path: str) -> str:
    return path.replace("\\", "/").lower().strip("/")


class Vfs:
    """The archives of one install in the engine's mount order, and its loose files."""

    def __init__(self, root: Path, exe: Path = None, archive: str = None):
        """`archive` replaces the first mount pattern the executable spells (`mount_patterns`)."""
        self.root = Path(root)
        if not self.root.is_dir():
            raise VfsError(f"no game directory at {self.root}")
        exe = Path(exe) if exe else self._loose_path(EXE_NAME)
        if not exe or not exe.is_file():
            raise VfsError(f"no {EXE_NAME} in {self.root} to read the archive patterns from (--exe)")
        self.patterns = mount_patterns(exe.read_bytes(), archive)
        self.archives = []                      # (file name, ta3do.HpiArchive), in mount order
        mounted = set()
        names = sorted((p for p in self.root.iterdir() if p.is_file()), key=lambda p: p.name.lower())
        for pattern in self.patterns:
            for path in names:
                if not fnmatch.fnmatch(path.name.lower(), pattern.lower()):
                    continue
                full = str(path.resolve()).lower()
                if full in mounted or not _has_trailer(path):
                    continue
                try:
                    arc = ta3do.HpiArchive(path)
                except (ta3do.HpiError, struct.error, IndexError):
                    continue
                mounted.add(full)
                self.archives.append((path.name, arc))
        self._features = None

    # -- loose files ----------------------------------------------------------

    def _loose_path(self, path: str):
        """The loose file at `path` under the game's directory, each component matched without case, or None."""
        cur = self.root
        parts = [p for p in _key(path).split("/") if p]
        for i, part in enumerate(parts):
            try:
                hit = next((c for c in cur.iterdir() if c.name.lower() == part), None)
            except OSError:
                return None
            if hit is None:
                return None
            if i < len(parts) - 1 and not hit.is_dir():
                return None
            cur = hit
        return cur if parts and cur.is_file() else None

    def _loose_walk(self, top: str, recursive: bool):
        base = self._loose_path_dir(top)
        if base is None:
            return
        entries = sorted(base.iterdir(), key=lambda p: p.name.lower())
        for e in entries:
            rel = f"{_key(top)}/{e.name.lower()}"
            if e.is_file():
                yield rel, e
        if recursive:
            for e in entries:
                if e.is_dir():
                    yield from self._loose_walk(f"{_key(top)}/{e.name.lower()}", True)

    def _loose_path_dir(self, path: str):
        cur = self.root
        for part in [p for p in _key(path).split("/") if p]:
            hit = next((c for c in cur.iterdir() if c.name.lower() == part and c.is_dir()), None)
            if hit is None:
                return None
            cur = hit
        return cur

    # -- the engine's questions -------------------------------------------------

    def where(self, path: str):
        """Where `path` opens from: "loose" or the archive's file name; None when nothing holds it."""
        if self._loose_path(path):
            return "loose"
        key = _key(path)
        for name, arc in self.archives:
            if key in arc.files:
                return name
        return None

    def size(self, path: str) -> int:
        """What the open test 0x4BBC40 answers for `path`: the size of the copy the engine opens, 0 when nothing holds
        it. A 0-byte file is as absent as a missing one to every probe that tests this value."""
        loose = self._loose_path(path)
        if loose:
            return loose.stat().st_size
        key = _key(path)
        for _name, arc in self.archives:
            if key in arc.files:
                return arc.files[key][1]
        return 0

    def open(self, path: str):
        """(bytes, where) for `path` by the engine's precedence, or None."""
        loose = self._loose_path(path)
        if loose:
            return loose.read_bytes(), "loose"
        key = _key(path)
        for name, arc in self.archives:
            if key in arc.files:
                return arc.read(key), name
        return None

    def enumerate(self, top: str, pattern: str, recursive: bool):
        """[(path, where)] of every file under `top` matching `pattern`, loose first, then the archives in mount
        order, each path once, from the first source that holds it (the module's header: the shadow bit)."""
        pat = pattern.lower()
        out = [(rel, "loose") for rel, _p in self._loose_walk(top, recursive)
               if fnmatch.fnmatch(rel.rsplit("/", 1)[-1], pat)]
        seen = {rel for rel, _w in out}
        prefix = _key(top) + "/"
        for name, arc in self.archives:
            for path in arc.files:
                if not path.startswith(prefix) or path in seen:
                    continue
                rest = path[len(prefix):]
                if "/" in rest and not recursive:
                    continue
                if fnmatch.fnmatch(rest.rsplit("/", 1)[-1], pat):
                    out.append((path, name))
                    seen.add(path)
        return out

    def read_from(self, path: str, where: str) -> bytes:
        """The copy of `path` that `where` (as enumerate names it) holds."""
        if where == "loose":
            return self._loose_path(path).read_bytes()
        for name, arc in self.archives:
            if name == where:
                return arc.read(_key(path))
        raise VfsError(f"no archive {where}")

    def feature(self, name: str):
        """(section, tdf path, where) of the feature section `name` from the first TDF that holds it, or None."""
        return self.features().get(name.lower())

    def features(self):
        """{lowercased section name: (section, tdf path, where)} of every feature section, each from the first TDF that
        holds it, in the order the TDFs enumerate."""
        if self._features is None:
            self._features = {}
            for path, where in self.enumerate("features", "*.tdf", True):
                try:
                    text = self.read_from(path, where).decode("latin-1")
                except (ta3do.HpiError, VfsError, struct.error, OSError):
                    continue
                for sec, body in tascene.parse_tdf(text).items():
                    if isinstance(body, dict) and body:
                        self._features.setdefault(sec, (body, path, where))
        return self._features
