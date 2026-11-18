#!/usr/bin/env python3
"""thinklib.py -- the thought viewer's library: recordings kept by the label of the experiment that made them.

    tools/impai/thinkview.py add REC... --label L [--note TEXT] [--move]
    tools/impai/thinkview.py labels
    tools/impai/thinkview.py drop --label L

THE LIBRARY is a folder a label, `<library>/<label>/`, by default `.data/impai/library/` of the main checkout
(library_dir: found through `git rev-parse --git-common-dir`, so every worktree shares one). A label is the
experiment or the reason -- `item19 try1`, `tier @39562add`, `played` -- and its folder holds, a recording:

    <name>.imprec       the recording, a HARD LINK to the file it was added from: no byte is copied, and the
                        source may be moved or deleted afterwards. Where a link cannot be made (another
                        filesystem) it is a copy, and `add` says so.
    <name>.json         THE SIDECAR: {"library": 1, "label", "added": the time, "source": the path it was added
                        from, "how": "link" | "copy" | "move", "tool", "command", "run", "game": the harness's
                        id of the game, "result": the harness's result of it, whole (bar.py's <id>.json, which
                        stood beside the source), "note"}
    <name>.tagpu.log    the game's log, linked the same way when one stood beside the source (think.py why
                        reads it)

There is no index: the labels are the folders and a label's recordings its `.imprec` files, read when asked
for, so nothing can go stale. `.cache/` in the library is the viewer's (thinkserve.py), no label.

A LABEL IS ONE PATH COMPONENT (label_name): a separator, a control character or a leading dot never reaches the
filesystem, and what a request or a command names is looked up among the folders that exist, never joined into
a path.

DROP REMOVES NAMES, NEVER THE LAST ONE. `drop` unlinks a label's recordings whose bytes another name still
holds (a hard link's count above one), or whose source still stands at the same size (a copy); a recording the
label holds the only copy of -- moved in, or its source since deleted -- is kept and named, and the folder stays
for it.

PLAYED GAMES are not in the library and are never copied: played() lists every
`tagpu/instances/<instance>/gamedir/replays/*.imprec` of the main checkout, read-only. BEING RECORDED
(being_recorded): a recording another process holds open -- its file or the `.lock` beside it, by the process
table's own open files -- or whose size or time changed since it was last looked at. The `.lock` FILE ITSELF says
nothing: it stays beside every recording by design (tagpu_recfile.h, THE PATH LOCK).

THE RUN TOOLS' --label (add_kept) is the one function session.py, batch.py, bar.py play, diag.py run and
gauntlet.py run call at the end of a run: it adds the recordings the run kept in its folders under the label.
A failure is a printed warning, never the run's."""
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
CACHE_NAME = ".cache"
LABEL_MAX = 100
SIDECAR = 1                     # THE SIDECAR's format


class LibraryError(Exception):
    pass


def main_checkout() -> Path:
    """The main checkout this tree is a worktree of (itself, when it is the main one or git does not say)."""
    try:
        got = subprocess.run(["git", "-C", str(TREE), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                             capture_output=True, text=True, timeout=30)
        common = Path(got.stdout.strip())
        if got.returncode == 0 and common.name == ".git":
            return common.parent
    except (OSError, subprocess.SubprocessError):
        pass
    return TREE


def library_dir(given=None) -> Path:
    return Path(given) if given else main_checkout() / ".data" / "impai" / "library"


def label_name(label) -> str:
    """A LABEL IS ONE PATH COMPONENT: `label` with its runs of white space a space each, every separator and
    control character a dash, cut to LABEL_MAX; LibraryError for one that is empty or begins with a dot."""
    name = re.sub(r"[/\\\x00-\x1f\x7f]", "-", re.sub(r"\s+", " ", str(label)))
    name = name.strip()[:LABEL_MAX].strip()
    if not name or name.startswith(".") or os.sep in name or (os.altsep and os.altsep in name):
        raise LibraryError(f"no label {label!r}: a label is a name, not a path, and begins with no dot")
    return name


def recordings(folder: Path) -> list:
    """A folder's recordings, by name."""
    try:
        return sorted(p for p in folder.iterdir() if p.suffix == ".imprec" and p.is_file())
    except OSError:
        return []


def sidecar(rec: Path) -> dict:
    """A library recording's SIDECAR; {} for none, or for a file that is not one (a harness's own result)."""
    try:
        doc = json.loads(rec.with_suffix(".json").read_text())
        return doc if isinstance(doc, dict) and doc.get("library") == SIDECAR else {}
    except (OSError, ValueError):
        return {}


def _place(src: Path, dest: Path, move: bool) -> str:
    """`src` at `dest`: moved, hard-linked, or copied where no link can be made; how."""
    if move:
        shutil.move(str(src), str(dest))
        return "move"
    try:
        os.link(src, dest)
        return "link"
    except OSError:
        shutil.copy2(src, dest)
        return "copy"


def add(recs, label, note="", move=False, tool=None, command=None, run=None, library=None, say=print) -> list:
    """Each of `recs` added to the library under `label`: [the recording's path there, ...]. A recording the
    label already holds (the same file, or a file of the same name and size) is left as it is and listed; another
    of its name gets the next free `<name>-N`. The log and the harness's result that stand beside a source come
    with it (THE LIBRARY)."""
    folder = library_dir(library) / label_name(label)
    out = []
    for src in (Path(r) for r in recs):
        if not src.is_file():
            raise LibraryError(f"{src}: no such recording")
        folder.mkdir(parents=True, exist_ok=True)
        dest, n = folder / src.name, 1
        while dest.exists():
            if dest.samefile(src) or dest.stat().st_size == src.stat().st_size:
                break
            n += 1
            dest = folder / f"{src.stem}-{n}{src.suffix}"
        if dest.exists():
            say(f"thinkview: {label_name(label)}: {dest.name} is already there")
            out.append(dest)
            continue
        log, result = src.with_suffix(".tagpu.log"), None
        try:
            result = json.loads(src.with_suffix(".json").read_text())
        except (OSError, ValueError):
            pass
        source = str(src.resolve())
        how = _place(src, dest, move)
        if how == "copy":
            say(f"thinkview: {dest.name}: no hard link could be made from {src.parent} (another filesystem?): copied")
        if log.is_file():
            try:
                _place(log, dest.with_suffix(".tagpu.log"), move)
            except OSError as e:
                say(f"thinkview: {dest.name}: its log was not added ({e.strerror or e})")
        if move and result is not None:
            try:
                src.with_suffix(".json").unlink()
            except OSError:
                pass
        doc = {"library": SIDECAR, "label": label_name(label), "added": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
               "source": source, "how": how, "tool": tool, "command": command, "run": run,
               "game": result.get("id") if isinstance(result, dict) else None,
               "result": result if isinstance(result, dict) else None, "note": note or ""}
        part = dest.with_suffix(f".json.{os.getpid()}.part")
        part.write_text(json.dumps(doc, indent=1))
        os.replace(part, dest.with_suffix(".json"))
        out.append(dest)
    return out


def labels(library=None) -> list:
    """[{"label", "n": its recordings, "bytes", "newest": the latest time one was added or written}, ...], the
    newest first: the library's folders, as they are now."""
    out = []
    try:
        folders = [p for p in library_dir(library).iterdir() if p.is_dir() and not p.name.startswith(".")]
    except OSError:
        return []
    for folder in folders:
        recs = recordings(folder)
        stats = [r.stat() for r in recs]
        out.append({"label": folder.name, "n": len(recs), "bytes": sum(s.st_size for s in stats),
                    "newest": max([s.st_mtime for s in stats] + [folder.stat().st_mtime])})
    return sorted(out, key=lambda row: -row["newest"])


def drop(label, library=None, say=print) -> tuple:
    """DROP REMOVES NAMES, NEVER THE LAST ONE: (the recordings removed, those kept) of `label`, each by name."""
    folder = library_dir(library) / label_name(label)
    if not folder.is_dir():
        raise LibraryError(f"no label {label_name(label)!r} in {library_dir(library)}")
    gone, kept = [], []
    for rec in recordings(folder):
        src = sidecar(rec).get("source")
        st = rec.stat()
        try:
            elsewhere = st.st_nlink > 1 or (src and Path(src).is_file() and Path(src).stat().st_size == st.st_size
                                            and not Path(src).samefile(rec))
        except OSError:
            elsewhere = False
        if not elsewhere:
            kept.append(rec.name)
            say(f"thinkview: {folder.name}: {rec.name} is the only copy of its recording: kept")
            continue
        for f in (rec, rec.with_suffix(".json"), rec.with_suffix(".tagpu.log")):
            try:
                f.unlink()
            except FileNotFoundError:
                pass
        gone.append(rec.name)
        say(f"thinkview: {folder.name}: removed {rec.name}")
    try:
        folder.rmdir()
    except OSError:
        pass                    # it still holds a recording that was kept, or a file that is no recording's
    return gone, kept


def played(main=None) -> dict:
    """PLAYED GAMES: {instance: [recording, ...]} of the main checkout's instances that hold one."""
    out = {}
    root = (Path(main) if main else main_checkout()) / "tagpu" / "instances"
    try:
        instances = sorted(p for p in root.iterdir() if p.is_dir())
    except OSError:
        return out
    for inst in instances:
        recs = recordings(inst / "gamedir" / "replays")
        if recs:
            out[inst.name] = recs
    return out


def open_files(*under) -> set:
    """The files under each of `under` that a process other than this one holds open, by the process table
    (/proc); empty where there is none to read."""
    out, me, prefix = set(), str(os.getpid()), tuple(str(Path(u).resolve()) + os.sep for u in under)
    try:
        pids = [p for p in os.listdir("/proc") if p.isdigit() and p != me]
    except OSError:
        return out
    for pid in pids:
        try:
            for fd in os.listdir(f"/proc/{pid}/fd"):
                try:
                    target = os.readlink(f"/proc/{pid}/fd/{fd}")
                except OSError:
                    continue
                if target.startswith(prefix):
                    out.add(target)
        except OSError:
            continue
    return out


def being_recorded(rec: Path, held: set, seen: dict) -> bool:
    """BEING RECORDED: `rec` or its `.lock` is in `held` (open_files' answer), or its size or time is not what
    `seen` ({path: (size, mtime_ns)}, updated here) held of it at the last look."""
    key = str(rec.resolve())
    try:
        st = rec.stat()
    except OSError:
        return True
    now = (st.st_size, st.st_mtime_ns)
    before = seen.get(key)
    seen[key] = now
    return key in held or key + ".lock" in held or (before is not None and before != now)


def add_kept(folders, label, tool=None, command=None, run=None, library=None, say=print) -> list:
    """THE RUN TOOLS' --label: every recording a run kept in `folders` (each searched whole) added under
    `label`; what was added. Whatever goes wrong is a printed warning: a run's verdict is never the library's."""
    try:
        recs = sorted({p for folder in folders for p in Path(folder).rglob("*.imprec") if p.is_file()})
        if not recs:
            say(f"thinkview: --label {label}: the run kept no recording: nothing added")
            return []
        if command is None:
            command = " ".join(sys.argv)
        out = add(recs, label, tool=tool, command=command, run=run, library=library, say=lambda _line: None)
        say(f"thinkview: {len(out)} recording{'s' if len(out) != 1 else ''} in the library under "
            f"{label_name(label)!r}: tools/impai/thinkview.py serve")
        return out
    except Exception as e:                                      # noqa: BLE001
        say(f"thinkview: WARNING: --label {label}: nothing added to the library ({type(e).__name__}: {e})")
        return []


def label_argument(parser) -> None:
    """THE RUN TOOLS' --label, the one flag each of them takes."""
    parser.add_argument("--label", default=None, metavar="L",
                        help="add the recordings this run keeps to the thought viewer's library under the label L "
                             "(tools/impai/thinkview.py serve shows them); without it nothing is added")


def size_says(n: int) -> str:
    return f"{n / 1e6:.1f} MB" if n >= 1e6 else f"{n / 1e3:.0f} kB"


def main(argv) -> int:
    """`add`, `labels` and `drop`, as thinkview.py hands them on."""
    import argparse
    p = argparse.ArgumentParser(prog="thinkview.py", description="the thought viewer's library of recordings")
    sub = p.add_subparsers(dest="verb", required=True)
    a = sub.add_parser("add", help="add recordings to the library under a label, as hard links")
    a.add_argument("rec", type=Path, nargs="+")
    a.add_argument("--label", required=True)
    a.add_argument("--note", default="")
    a.add_argument("--move", action="store_true", help="move the recordings in, where a link would leave them")
    ls = sub.add_parser("labels", help="the library's labels, with their counts and sizes")
    d = sub.add_parser("drop", help="remove a label's recordings from the library, never a recording's last copy")
    d.add_argument("--label", required=True)
    for x in (a, ls, d):
        x.add_argument("--library", default=None, metavar="DIR", help="the library (default .data/impai/library)")
    args = p.parse_args(argv)
    try:
        if args.verb == "add":
            got = add(args.rec, args.label, args.note, args.move, tool="thinkview.py add", library=args.library)
            print(f"thinkview: {len(got)} recording{'s' if len(got) != 1 else ''} under {label_name(args.label)!r} "
                  f"in {library_dir(args.library)}")
        elif args.verb == "labels":
            rows = labels(args.library)
            for row in rows:
                print(f"{row['n']:5d}  {size_says(row['bytes']):>9}  {time.strftime('%Y-%m-%d %H:%M', time.localtime(row['newest']))}  {row['label']}")
            print(f"thinkview: {len(rows)} label{'s' if len(rows) != 1 else ''} in {library_dir(args.library)}")
        else:
            gone, kept = drop(args.label, args.library)
            print(f"thinkview: {label_name(args.label)!r}: {len(gone)} removed, {len(kept)} kept")
        return 0
    except LibraryError as e:
        print(f"thinkview: COULD NOT RUN: {e}")
        return 2
    except OSError as e:
        print(f"thinkview: COULD NOT RUN: {e.filename or ''}: {e.strerror or e}")
        return 2
