#!/bin/bash
# capture.sh <page url> <out.png>: the prototype page's HUD stage at device scale, once every one-piece part
# has been composed. Give the page a 3840x2160 stage (res=3840x2160) and it lands at native 4K pixels.
# The page lays out at device scale factor 1, one CSS pixel to a screen pixel, as the game draws: Blink rounds a
# face's ascent and descent to whole CSS pixels, so at a factor of 2 a line's baseline would fall on a two-pixel
# grid the game does not have (tagpu_hudlay_baseline). The screenshot is taken with run-code at scale 'device',
# never playwright-cli's own, which is CSS-scale; and a capture taken before the kit has loaded shows a HUD with
# no art, hence the wait.
set -e
url=$1; out=$(realpath -m "$2"); cfg=$(mktemp -d)
echo '{ "browser": { "contextOptions": { "deviceScaleFactor": 1, "viewport": { "width": 3900, "height": 2400 } } } }' > "$cfg/dpr1.json"
cd "$cfg"   # playwright-cli writes its console logs into the working directory
P="playwright-cli -s=hudcap$$ --config=$cfg/dpr1.json"; ev() { $P eval "$1" 2>&1 | sed -n '/Result/,/^###/p' | sed -n 2p | tr -d '"'; }
$P open "$url" >/dev/null 2>&1; sleep 3
for t in $(seq 1 30); do r=$(ev "() => { const g = [...document.querySelectorAll('.stage [data-gp], .stage [data-tile]')]; return g.length + ':' + g.filter(e => e.style.backgroundImage.startsWith('url')).length; }"); [ "${r%%:*}" = "${r##*:}" ] && break; sleep 0.5; done
bb=$(ev "() => { const r = document.querySelector('.stage').getBoundingClientRect(); return [r.x, r.y, r.width, r.height].map(Math.round).join(','); }")
$P run-code "async page => { await page.screenshot({ path: '$out', scale: 'device', clip: (([x, y, width, height]) => ({ x, y, width, height }))([$bb]) }); }" 2>&1 | grep -i error || true
echo "composed $r, missing art $(ev "() => document.querySelectorAll('.stage .noart, #statwin .noart').length")"
$P console 2>&1 | grep -iE "error" | head -2 || true; $P close >/dev/null 2>&1; rm -rf "$cfg"
