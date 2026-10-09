#!/bin/bash
# compare.sh <page base url> <query> <out prefix>: the button states of one look at native 4K, for compose.py.
# <page base url> is the mockup (http://127.0.0.1:<port>/tools/hudart/mockup/index.html) or the exploration
# prototype; for the prototype, <query> picks the look and the button set (look=c&btn=3). Writes <prefix>_tp (transport plate, play held), _foot (stats plate, stats on),
# _chips (the chip trays, UNITS selected) and _hov (a row hovered, JUMP held).
set -e
base=$1; q=$2; pre=$(realpath -m "$3"); cfg=$(mktemp -d)
echo '{ "browser": { "contextOptions": { "deviceScaleFactor": 2, "viewport": { "width": 2100, "height": 1400 } } } }' > "$cfg/dpr2.json"
cd "$cfg"   # playwright-cli writes its console logs into the working directory
P="playwright-cli -s=hudcmp$$ --config=$cfg/dpr2.json"; ev() { $P eval "$1" 2>&1 | sed -n '/Result/,/^###/p' | sed -n 2p | tr -d '"'; }
shot() { $P run-code "async page => { await page.screenshot({ path: '$1', scale: 'device', clip: (([x, y, width, height]) => ({ x, y, width, height }))([$2]) }); }" 2>&1 | grep -i error || true; }
rect() { ev "() => { const r = document.querySelector('$1').getBoundingClientRect(); return [r.x - 4, r.y - 4, r.width + 8, r.height + 8].map(Math.round).join(','); }"; }
mid() { ev "() => { const r = document.querySelector('$1').getBoundingClientRect(); return Math.round(r.x + r.width / 2) + ' ' + Math.round(r.y + r.height / 2); }"; }
$P open "$base?paused&stat=1&res=3840x2160&hud=150&view=live&e1v=V1&nt=2&np=10&$q#E1" >/dev/null 2>&1; sleep 3
for t in $(seq 1 30); do r=$(ev "() => { const g = [...document.querySelectorAll('.stage [data-gp], .stage [data-tile]')]; return g.length + ':' + g.filter(e => e.style.backgroundImage.startsWith('url')).length; }"); [ "${r%%:*}" = "${r##*:}" ] && break; sleep 0.5; done
set -- $(mid '[data-act=play]'); $P mousemove $1 $2 >/dev/null; $P mousedown >/dev/null; sleep 0.4
shot "${pre}_tp.png" "$(rect '[data-gp=tp_one]')"; $P mouseup >/dev/null; sleep 0.3
shot "${pre}_foot.png" "$(rect '[data-gp=foot_one]')"
shot "${pre}_chips.png" "$(ev "() => { const a = document.querySelector('#statwin [data-val=metal]').getBoundingClientRect(), b = document.querySelector('#statwin [data-val=losses]').getBoundingClientRect(), t = document.querySelector('#statwin [data-val=team]').getBoundingClientRect(); return [a.x - 112, a.y - 6, t.right - a.x + 118, b.bottom - a.y + 12].map(Math.round).join(','); }")"
set -- $(ev "() => { const r = document.querySelectorAll('.stage .row1, .stage .row')[2].getBoundingClientRect(); return Math.round(r.x + 40) + ' ' + Math.round(r.y + r.height / 2); }"); $P mousemove $1 $2 >/dev/null; sleep 0.4
set -- $(mid '.stage .row1:hover [data-act=jump], .stage .row:hover [data-act=jump]'); $P mousemove $1 $2 >/dev/null; $P mousedown >/dev/null; sleep 0.4
shot "${pre}_hov.png" "$(ev "() => { const r = document.querySelector('.stage .row1:hover, .stage .row:hover').getBoundingClientRect(); return [r.x - 4, r.y - 4, r.width + 8, r.height + 8].map(Math.round).join(','); }")"; $P mouseup >/dev/null
echo "$q: missing art $(ev "() => document.querySelectorAll('.stage .noart, #statwin .noart').length")"
$P close >/dev/null 2>&1; rm -rf "$cfg"
