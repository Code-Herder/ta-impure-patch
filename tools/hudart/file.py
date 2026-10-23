"""Files cut parts into art/hud/, the replay HUD's shared source: each part's PNG, lossless, and its entry
in art/hud/kit.json. The mockup (tools/hudart/mockup/) draws from there, and the DLL packs it with tools/hudpak.py.
usage: file.py <kit dir> [--look <l1>,<l2>,...] <part>[=<name>] ...

<kit dir> is a cutter's output (gridslice.py, cut.py, iconcut.py, tile.py) or a lookmerge.py kit. A part is
taken from <part>@<look>.png for the first look in --look that has it, else from <part>.png; `=<name>`
files it under another name (track_fill_green=track_fill). A part filed again replaces its old entry.

Each entry carries what drawing the part needs and nothing about how it was made:
  grid   a one-piece part: `design` size in HUD units, stretch bands `xs` / `ys`, measured `wells`, and
         `text`, the wells that are text zones on the plate rather than recesses;
  nine   a 9-slice: `slice` [top, right, bottom, left] in the PNG's pixels;
  xtile  on a grid or a 9-slice: its x stretch band (a grid's one `xs` band, a 9-slice's middle) is one
         period of a seamless tile (seamband.py) and repeats along x instead of stretching;
  tile   a strip that repeats along x;
  whole  drawn stretched to its box;
  icon   drawn whole inside its box, keeping its aspect, centred.
`scale` is the PNG's pixels per HUD unit. The PNG is rewritten with zlib's best compression and checked
pixel for pixel against the source."""
import json, os, sys
from pathlib import Path
import numpy as np
from PIL import Image

OUT = Path(__file__).resolve().parents[2] / 'art' / 'hud'


def entry(e, name):
    if e.get('grid'):
        d = {'kind': 'grid', 'scale': e['scale'], 'design': e['design'], 'xs': e['xs'], 'ys': e['ys'], 'wells': e['wells']}
        if e.get('text'): d['text'] = e['text']
        if e.get('xtile'): d['xtile'] = True
        return d
    if e.get('tile'): return {'kind': 'tile', 'scale': e['scale']}
    if e.get('fixed'): return {'kind': 'icon' if name.startswith('i_') else 'whole', 'scale': e['scale']}
    if e.get('core'): sys.exit(f'{name}: a core-only fill; nothing in the chosen HUD draws one, so file.py has no kind for it')
    return {'kind': 'nine', 'scale': e['scale'], 'slice': e['slice'], **({'xtile': True} if e.get('xtile') else {})}


def main(a):
    kit, looks = Path(a[0]), []
    if len(a) > 2 and a[1] == '--look': looks, a = a[2].split(','), a[:1] + a[3:]
    K = json.load(open(kit / 'kit.json'))
    (OUT / 'parts').mkdir(parents=True, exist_ok=True)
    kp = OUT / 'kit.json'
    H = json.load(open(kp)) if kp.exists() else {'format': 1, 'parts': {}}
    for spec in a[1:]:
        part, _, name = spec.partition('=')
        name = name or part
        src = next((f'{part}@{l}' for l in looks if f'{part}@{l}' in K), part)
        if src not in K: sys.exit(f'{part}: not in {kit}/kit.json')
        im = Image.open(kit / f'{src}.png'); im.load()
        dst = OUT / 'parts' / f'{name}.png'
        im.save(dst, optimize=True, compress_level=9)
        if not np.array_equal(np.asarray(Image.open(dst).convert('RGBA')), np.asarray(im.convert('RGBA'))): sys.exit(f'{name}: the rewritten PNG differs')
        H['parts'][name] = entry(K[src], name)
        print(f'{name:22s} <- {src:24s} {H["parts"][name]["kind"]:5s} {os.path.getsize(dst) / 1e3:7.0f} KB')
    H['parts'] = dict(sorted(H['parts'].items()))
    json.dump(H, open(kp, 'w'), indent=1)


if __name__ == '__main__':
    main(sys.argv[1:])
