#!/usr/bin/env python3
"""Copy the transcripts a shoot captured into the montage script.

    promo/transcripts.py promo/tacli-promo.json <shootdir> [--check]

`promo/shoot.sh` writes `<shootdir>/<clip-id>.txt` — exactly what
`tacli scenario load` printed for that clip. This puts those lines into the
matching window's `output`, so the terminal in the film prints what the tool
really prints and keeps doing so after a re-shoot.

`--check` reports what would change and exits non-zero if anything would, which
is the form to run before a release: a montage whose terminal has drifted from
the tool's real output is the kind of thing nobody notices until someone else does.

The instance name in each window's `command` has to be the instance the clip was
shot on, or the transcript contradicts the command right above it — shoot
`big-battle` on `front1` if the film says `scenario load front1 big-battle`.
"""

import json
import re
import sys
from pathlib import Path


def transcript_for(shootdir: Path, clip: str) -> list[str] | None:
    p = shootdir / f"{clip}.txt"
    if not p.exists():
        return None
    lines = [l.rstrip("\n") for l in p.read_text().splitlines()]
    # `--restart` is an artefact of re-shooting, not of the command the film
    # shows; a first run on a stopped instance never prints it.
    return [l for l in lines if "(--restart)" not in l and l.strip()]


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    script, shootdir = Path(argv[0]), Path(argv[1])
    check = "--check" in argv[2:]

    doc = json.loads(script.read_text())
    changed, missing = [], []
    for win in doc.get("windows", []):
        clip = win.get("clip")
        if not clip or "command" not in win:
            continue
        lines = transcript_for(shootdir, clip)
        if lines is None:
            missing.append(clip)
            continue
        # the command has to name the instance the clip was shot on
        m = re.search(r"load\s+(\S+)", win["command"])
        said = m.group(1) if m else None
        shot = None
        for l in lines:
            m2 = re.match(r"\S+: loaded on (\S+)", l)
            if m2:
                shot = m2.group(1)
                break
        if said and shot and said != shot:
            print(f"{clip}: the film says `load {said}` but the clip was shot on "
                  f"{shot} — re-shoot it on {said}, or the transcript contradicts "
                  f"the command above it", file=sys.stderr)
            return 1
        if win.get("output") != lines:
            changed.append((clip, len(win.get("output", [])), len(lines)))
            win["output"] = lines

    for clip, was, now in changed:
        print(f"{'would update' if check else 'updated'} {clip}: "
              f"{was} -> {now} lines")
    if missing:
        print(f"no transcript captured for: {', '.join(missing)}")
    if not changed:
        print("every window already carries its captured transcript")
        return 0
    if check:
        return 1
    script.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
