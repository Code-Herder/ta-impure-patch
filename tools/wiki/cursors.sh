#!/bin/bash
# cursors.sh [kit dir]: the wiki stage's three cursor icons, rasterised once by Chromium from stage.js's own cursorShape
# (tools/wiki/cursors.html) and written into the kit as icons -- parts/cursor_{arrow,move,build}.png and their kit.json
# entries -- so the mockup's stage and the DLL draw the same pixels. The kit is art/wiki/kit/ unless given. Run it after
# a change to cursorShape, SCALE or ICON_BOX; the icons are committed, as the kit's other parts are.
set -e
here=$(cd "$(dirname "$0")" && pwd); kit=$(realpath -m "${1:-$here/../../art/wiki/kit}"); tmp=$(mktemp -d)
# the browser refuses file: URLs, so the page is served, from a port nothing holds
port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
python3 "$here/../hudart/serve.py" --port "$port" >/dev/null 2>&1 & srv=$!
trap 'kill $srv 2>/dev/null; rm -rf "$tmp"' EXIT
cd "$tmp"   # playwright-cli writes its console logs into the working directory
P="playwright-cli -s=wikicursors$$"
for t in $(seq 1 50); do curl -sf -o /dev/null "http://127.0.0.1:$port/tools/wiki/cursors.html" && break; sleep 0.1; done
$P open "http://127.0.0.1:$port/tools/wiki/cursors.html" >/dev/null 2>&1
$P eval "() => JSON.stringify(window.CURSORS)" 2>&1 | sed -n '/Result/,/^###/p' | sed -n 2p > cursors.json
$P close >/dev/null 2>&1
python3 - "$kit" cursors.json <<'EOF'
import base64, json, sys
from pathlib import Path
kit, got = Path(sys.argv[1]), json.loads(json.loads(open(sys.argv[2]).read()))
K = json.load(open(kit / 'kit.json'))
for name, url in sorted(got['png'].items()):
    (kit / 'parts' / f'{name}.png').write_bytes(base64.b64decode(url.split(',', 1)[1]))
    K['parts'][name] = {'kind': 'icon', 'scale': got['scale']}
    print(f'{name}: {kit / "parts" / (name + ".png")}')
K['parts'] = dict(sorted(K['parts'].items()))
open(kit / 'kit.json', 'w').write(json.dumps(K, indent=1) + '\n')
EOF
