#!/usr/bin/env python3
"""PROTOTYPE — throwaway. Generates five radically different cuts of the tacli promo.

The question: **what should the promo actually look like?** We built one cut from
the brief. This makes four rivals that disagree with it on one axis each, so the
choice is made by watching rather than by imagining.

Each variant is a whole-file override of `promo/tacli-promo.json` — no inheritance
in the renderer, just plain JSON written out, so `tools/tamontage` eats them
unchanged and any variant can be promoted by copying it over the base.

Delete this directory once a cut is chosen; fold the answer into NOTES.md first.
"""

import copy
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE = json.loads((HERE.parent / "tacli-promo.json").read_text())


def cap(t0, t1, text, sub=None, **kw):
    c = {"t0": t0, "t1": t1, "text": text}
    if sub:
        c["sub"] = sub
    c.update(kw)
    return c


TITLE_SUB = "github.com/Code-Herder/ta-impure-patch"


# ---------------------------------------------------------------- A: ladder
def variant_ladder(d):
    """The control: the cut built from the brief, unchanged."""
    return d


# ------------------------------------------------------------ B: relentless
def variant_relentless(d):
    """One unbroken zoom-out, no hold beats, all four terminals up front.

    Asks: does the film work as a single continuous move? The brief's ladder has
    three stops in it; this argues the stops are what make it feel long.
    """
    d["duration"] = 32.0
    opens = [(0.3, 1.0, 3.0, 3.6), (4.2, 4.6, 7.2, 7.8),
             (4.7, 5.1, 7.7, 8.3), (5.2, 5.6, 8.2, 8.8)]
    for w, (o, ty, en, ga) in zip(d["windows"], opens):
        w["t_open"], w["t_type"], w["t_enter"], w["t_game"] = o, ty, en, ga
    d["camera"] = [
        {"t": 0.0,  "cols": 1.34, "cx": 512,  "cy": 384},
        {"t": 6.0,  "cols": 2.20, "cx": 1056, "cy": 800, "ease": "linear"},
        {"t": 32.0, "cols": 44.0, "cx": 1056, "cy": 800, "ease": "linear"},
    ]
    d["fill"]["reveal"] = {"not_before": 9.0, "lead": 0.8, "wave": 1.0, "fade": 0.2}
    d["captions"] = [
        cap(1.2, 4.2, "One command.", "no menu, no map setup, no clicking"),
        cap(10.0, 14.5, "Every fight is a file.", "scenarios/big-battle.json"),
        cap(27.0, 32.0, "tacli", TITLE_SUB, big=True),
    ]
    return d


# ------------------------------------------------------------------ C: card
def variant_card(d):
    """Full-frame typographic cards instead of lower-thirds.

    Asks: does the pitch land harder when the words own the frame? Costs motion —
    the picture darkens behind every card — so it is a real trade, not a free win.
    """
    d["captions"] = [
        cap(1.4, 5.0, "One command.", "no menu, no map setup, no clicking",
            full=True, dim=170),
        cap(15.2, 19.8, "Every fight is a file.",
            "edit it, commit it, re-run it", full=True, dim=185),
        cap(27.0, 31.5, "Run one.", "or as many as the machine will hold",
            full=True, dim=185),
        cap(36.5, 41.5, "A test harness that happens to look like a war.",
            full=True, dim=195),
        cap(48.5, 54.0, "tacli", TITLE_SUB, big=True),
    ]
    return d


# ---------------------------------------------------------------- D: silent
def variant_silent(d):
    """No captions at all until the closing title.

    Asks: do the pictures carry the pitch on their own? If they do, the film needs
    no translation and no reading — which matters more than any single line of copy.
    """
    d["captions"] = [cap(48.0, 54.0, "tacli", TITLE_SUB, big=True)]
    return d


# ----------------------------------------------------------------- E: close
def variant_close(d):
    """Stops at 12 columns instead of 44 — the grid stays legible as games.

    Asks the awkward question: is 40-wide actually better? At 44 columns a tile is
    ~45 px and the wall reads as texture; at 12 it reads as a dozen distinct games
    you could still point at. Scale versus legibility, and only watching settles it.
    """
    d["duration"] = 46.0
    d["camera"] = [
        {"t": 0.0,  "cols": 1.34, "cx": 512,  "cy": 384},
        {"t": 9.0,  "cols": 1.28, "cx": 512,  "cy": 384, "ease": "linear"},
        {"t": 12.5, "cols": 3.05, "cx": 1056, "cy": 800, "ease": "out"},
        {"t": 22.0, "cols": 2.95, "cx": 1056, "cy": 800, "ease": "linear"},
        {"t": 40.0, "cols": 12.0, "cx": 1056, "cy": 800, "ease": "linear"},
        {"t": 46.0, "cols": 13.0, "cx": 1056, "cy": 800, "ease": "out"},
    ]
    d["captions"] = [
        cap(1.2, 4.8, "One command.", "no menu, no map setup, no clicking"),
        cap(5.4, 9.2, "A 1997 engine, mid-battle, in seconds.",
            "silent  ·  windowed  ·  no intro movies"),
        cap(15.0, 19.6, "Every fight is a file.",
            "scenarios/big-battle.json  —  edit it, commit it, re-run it"),
        cap(20.2, 24.0, "Instances share nothing.",
            "own game dir  ·  own config  ·  own window"),
        cap(27.0, 31.0, "Run one.", "or as many as the machine will hold"),
        cap(33.0, 38.0, "A test harness that happens to look like a war."),
        cap(40.5, 46.0, "tacli", TITLE_SUB, big=True),
    ]
    return d


VARIANTS = [
    ("a-ladder", "Ladder (the brief)", variant_ladder,
     "Three stops: one terminal, four, then the wall. Lower-third captions."),
    ("b-relentless", "Relentless", variant_relentless,
     "32s, one unbroken zoom, no holds, three captions. Does it work as one move?"),
    ("c-card", "Card", variant_card,
     "Full-frame typographic cards instead of subtitles. Words own the frame."),
    ("d-silent", "Silent", variant_silent,
     "No captions until the title. Do the pictures carry it alone?"),
    ("e-close", "Close", variant_close,
     "Stops at 12 columns, not 44. Legibility instead of scale."),
]


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(os.environ.get("TMPDIR", "/tmp")) / "tacli-promo-cuts"
    out.mkdir(parents=True, exist_ok=True)
    manifest = []
    for vid, title, fn, blurb in VARIANTS:
        d = fn(copy.deepcopy(BASE))
        d["name"] = f"tacli-promo-{vid}"
        (out / f"{vid}.json").write_text(json.dumps(d, indent=2, ensure_ascii=False) + "\n")
        manifest.append({
            "id": vid, "title": title, "blurb": blurb,
            "duration": d["duration"],
            "captions": len(d["captions"]),
            "camera": [[k["t"], k["cols"]] for k in d["camera"]],
            "end_cols": d["camera"][-1]["cols"],
            "first_caption": (d["captions"][0]["text"] if d["captions"] else "—"),
        })
        print(f"  {vid:14s} {d['duration']:5.1f}s  "
              f"{len(d['captions'])} captions  ends at {d['camera'][-1]['cols']:.0f} cols")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (out / "index.html").write_text(page(manifest))
    print(f"\n  -> {out}")


def page(manifest):
    """The compare page: template beside this file, data substituted in."""
    tmpl = (HERE / "page.html.tmpl").read_text()
    return tmpl.replace("__DATA__", json.dumps(manifest))


if __name__ == "__main__":
    main()
