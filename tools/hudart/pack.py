"""Pack a kit for the page: every part as a data URI in one JSON, and the parts' measurements into the page.
usage: pack.py <kit dir> <out.json> [<page.html> <version>]
<kit dir> holds <part>.png and kit.json (what cut.py, gridslice.py, iconcut.py and lookmerge.py write). Given a
page, the kit's measurements replace that version's entry of the page's `const E1KIT = /*E1KIT*/{...};`.
One JSON per kit because an artifact holds at most 511 files; a part over 60 KB goes as WebP at quality 92
with lossless alpha, because the distressed paintings are noise PNG cannot compress and an artifact file
holds at most 16 MB (measured: 25 MB as PNG, 5.3 MB this way, the rendered HUD within 1/255 on average)."""
import base64, io, json, os, re, sys
from PIL import Image
KEYS = ('w', 'h', 'scale', 'slice', 'fixed', 'core', 'grid', 'tile', 'design', 'wells', 'xs', 'ys')

def uri(path):
    raw = open(path, 'rb').read()
    if len(raw) <= 60_000: return 'data:image/png;base64,' + base64.b64encode(raw).decode()
    buf = io.BytesIO(); Image.open(path).save(buf, 'WEBP', quality=92, method=6, alpha_quality=100)
    return 'data:image/webp;base64,' + base64.b64encode(buf.getvalue()).decode()

kit, out = sys.argv[1:3]
K = json.load(open(os.path.join(kit, 'kit.json')))
json.dump({k: uri(os.path.join(kit, k + '.png')) for k in sorted(K)}, open(out, 'w'), separators=(',', ':'))
print(f'{len(K)} parts, {os.path.getsize(out) / 1e6:.1f} MB')
if len(sys.argv) > 3:
    page, version = sys.argv[3:5]; s = open(page).read()
    m = re.search(r'const E1KIT = /\*E1KIT\*/(\{.*?\});\n', s, flags=re.S); assert m, 'no E1KIT in the page'
    kits = json.loads(m.group(1)); kits[version] = {k: {kk: e[kk] for kk in KEYS if kk in e} for k, e in K.items()}
    s = s[:m.start(1)] + json.dumps(kits, separators=(',', ':')) + s[m.end(1):]
    open(page, 'w').write(s); print('page', {v: len(k) for v, k in kits.items()})
