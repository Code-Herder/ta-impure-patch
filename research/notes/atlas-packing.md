# The GAF sprite atlas — 48 % full and out of room

At full zoom-out the feature atlas refused **455 sprite quads a frame** and rebuilt itself
**37,140 times** in one 4K session. It was not too small. Every frame the map can show fits in
**48 % of one 2048 square** — the packer stranded the rest, and a latch threw away what was
left. The fix turned out to be an *order*, not a bigger page.

This page is the visual argument: what was wrong, what the obvious answers cost, and what
shipped on 2026-09-10. The code-level reference lives in
[features](features.html) §5; the engine addresses behind the pass are in
[the engine map](exe-reverse-engineering.html).

## Summary — the six things to know

1. **Nothing sorts the frames.** `gaf_insert` is a shelf packer fed in the order the gather
   walks the map, so cell heights arrive shuffled. That is the whole mechanism and the whole
   problem.
2. **One tall frame retroactively raises a whole shelf.** Heights on Town & Country run 10 to
   320, median 54. A 322-tall cliff face landing on a shelf of trees strands everything already
   on it under a band of nothing.
3. **A latch turned a miss into a shutdown.** The first cell that would not fit set `full`,
   which is read at the *top* of `gaf_insert` — so every later frame was refused without being
   measured, including an 18×16 pebble with 29,696 free texels in front of it.
4. **The reset was not a recovery.** The next frame rebuilt the atlas from nothing, in the same
   order, into the same geometry, and hit the same wall — 37,140 times, each one re-decoding
   ~200 frames and clearing the Classic++ restore queue before the restorer could land a batch.
5. **Both obvious answers were measured and rejected.** A 4096 square fits everything by making
   the waste affordable rather than removing it (51 % → 55 % stranded, ×4 memory). Multipage is
   genuinely better *and* reaches into every consumer's vertex format and shader.
6. **What shipped is a repack.** When the atlas fills it re-lays what it holds **tallest cell
   first** and reserves the rects; each frame re-decodes into its new rect on demand. Live:
   `atlas-fail` 455 → **0**, resets 37,140 → **0**, one repack for the session.

---

## 1. How a frame reaches the atlas

A GAF frame reaches the atlas the first time something asks to draw it. `gaf_insert` puts its
cell — the frame plus one texel of replicated border on every side — at the shelf cursor,
advances the cursor by the cell's width, and raises the shelf to the cell's height if it is
taller. When the cursor runs past the right edge, the shelf ends and a new one starts below it.

Nothing sorts the frames. They arrive in the order the gather walks the map, which is row by
row across the terrain, so their heights arrive shuffled.

<div class="tablewrap ap">
<style>
.ap{color:var(--ink-2)}
.ap svg{display:block;width:100%;height:auto}
.ap svg text{font-family:"IBM Plex Sans",system-ui,sans-serif;fill:var(--ink);font-size:12px}
.ap-plate{background:var(--code-bg)}
.ap-cap{font-family:"IBM Plex Mono",monospace;font-size:11px;color:var(--ink-3);
        letter-spacing:.03em;margin:0 0 .45rem}
.ap-atlas{fill:var(--code-bg);stroke:var(--edge);stroke-width:1.4}
.ap-dead,.ap-shelfband{fill:var(--panel-2)}
.ap-free{fill:none;stroke:var(--edge);stroke-width:1;stroke-dasharray:4 4}
.ap-cell0,.ap-f0{fill:#8a9a63} .ap-cell1,.ap-f1{fill:#b8975f}
.ap-cell2,.ap-f2{fill:#6d8098} .ap-cell3,.ap-f3{fill:#41505f}
.ap-bad{fill:var(--warn-bg);stroke:var(--warn);stroke-width:1.3}
.ap-badtx,.ap-cursor{fill:var(--warn)}
.ap-sigtx{fill:var(--accent-ink)} .ap-sig{stroke:var(--accent-ink);fill:none;stroke-width:1.4}
.ap-lab,.ap-atlaslabel{font-size:10.5px;fill:var(--ink-3);letter-spacing:.04em}
.ap-thin{stroke:var(--ink-2);stroke-width:1;fill:none}
</style>
<svg class="ap-dia" viewBox="0 0 700 300" role="img" aria-label="Four short sprite cells packed left to right on the first shelf of an atlas, with the shelf cursor and shelf height marked">
   <rect class="ap-atlas" x="60" y="20" width="512" height="256"/>
   <text class="ap-lab" x="60" y="14">0</text>
   <text class="ap-lab" x="572" y="14" text-anchor="end">512 texels</text>
   <rect class="ap-cell1" x="60" y="20" width="44" height="60"/>
   <rect class="ap-cell0" x="104" y="20" width="62" height="24"/>
   <rect class="ap-cell0" x="166" y="20" width="40" height="36"/>
   <rect class="ap-cell0" x="206" y="20" width="30" height="28"/>
   <text x="82" y="96" text-anchor="middle" font-size="10">tree1</text>
   <text x="135" y="96" text-anchor="middle" font-size="10">tree1s</text>
   <text x="186" y="96" text-anchor="middle" font-size="10">rock2</text>
   <text x="221" y="108" text-anchor="middle" font-size="10">bush3</text>
   <line class="ap-thin" x1="82" y1="86" x2="82" y2="90"/>
   <line class="ap-thin" x1="135" y1="86" x2="135" y2="90"/>
   <line class="ap-thin" x1="186" y1="86" x2="186" y2="90"/>
   <line class="ap-thin" x1="221" y1="86" x2="221" y2="102"/>
   <line class="ap-cursor" x1="236" y1="14" x2="236" y2="86"/>
   <text class="ap-sigtx" x="243" y="14" font-size="11">shelfX</text>
   <line class="ap-thin" x1="48" y1="20" x2="48" y2="80"/>
   <line class="ap-thin" x1="44" y1="20" x2="52" y2="20"/>
   <line class="ap-thin" x1="44" y1="80" x2="52" y2="80"/>
   <text x="40" y="54" text-anchor="end" font-size="11">shelfH</text>
   <text class="ap-lab" x="40" y="68" text-anchor="end">= 60</text>
   <line class="ap-thin" x1="60" y1="80" x2="572" y2="80" stroke-dasharray="3 3"/>
   <text class="ap-lab" x="588" y="52">shelf 0</text>
   <text class="ap-lab" x="588" y="66">y = 0</text>
  </svg>
</div>

**Reading the diagram:** the top 256 rows of a 512-texel atlas, drawn 1:1 so the cells are
legible; the real one is 2048 square. Four cells on the first shelf. The shelf is as tall as the
tallest cell that has landed on it — here 60, set by `tree1`. Everything shorter carries dead
space above it already.

## 2. The failure, 1 of 2 — one tall frame raises the whole shelf

Frame heights on this map run from **10 to 320, median 54**. A cliff face is six times the
height of a tree, and if it lands on a shelf of trees the shelf becomes 322 texels tall —
retroactively, for everything already on it.

<div class="tablewrap ap">
<svg class="ap-dia" viewBox="0 0 700 420" role="img" aria-label="A tall cliff face cell lands on a shelf of short cells, raising the shelf height to 322 and stranding dead space above every short cell">
   <rect class="ap-atlas" x="60" y="20" width="512" height="380"/>
   <rect class="ap-dead" x="60" y="20" width="274" height="322"/>
   <rect class="ap-cell1" x="60" y="282" width="44" height="60"/>
   <rect class="ap-cell0" x="104" y="318" width="62" height="24"/>
   <rect class="ap-cell0" x="166" y="306" width="40" height="36"/>
   <rect class="ap-cell0" x="206" y="314" width="30" height="28"/>
   <rect class="ap-cell2" x="236" y="230" width="98" height="112"/>
   <rect class="ap-cell3" x="334" y="20" width="212" height="322"/>
   <text x="440" y="188" text-anchor="middle" font-size="11" fill="#fff">cliff_face</text>
   <text x="440" y="204" text-anchor="middle" font-size="10" fill="#fff">210 &times; 320</text>
   <text x="285" y="292" text-anchor="middle" font-size="10" fill="#fff">hive</text>
   <text x="197" y="176" text-anchor="middle" font-size="11" class="ap-sigtx">dead space</text>
   <text x="197" y="192" text-anchor="middle" font-size="10" class="ap-lab">70 844 texels stranded</text>
   <text x="197" y="206" text-anchor="middle" font-size="10" class="ap-lab">above five short cells</text>
   <line class="ap-thin" x1="48" y1="20" x2="48" y2="342"/>
   <line class="ap-thin" x1="44" y1="20" x2="52" y2="20"/>
   <line class="ap-thin" x1="44" y1="342" x2="52" y2="342"/>
   <text x="40" y="176" text-anchor="end" font-size="11">shelfH</text>
   <text class="ap-lab" x="40" y="190" text-anchor="end">60 &rarr; 322</text>
   <line class="ap-thin" x1="60" y1="342" x2="572" y2="342" stroke-dasharray="3 3"/>
   <rect class="ap-free" x="60" y="342" width="512" height="58"/>
   <text class="ap-sigtx" x="316" y="376" text-anchor="middle" font-size="11">58 rows left below</text>
  </svg>
</div>

**Reading the diagram:** the same four cells, plus `hive` and `cliff_face`. The shelf grew to
322 under the tall cell, so the five short ones now sit at the bottom of a 322-tall band with
**88,200 texels of nothing above them**. The packer cannot go back: the cursor has already
passed. Only 58 rows are left for every frame still to come.

## 3. The failure, 2 of 2 — then one miss closes the door

The first frame that will not fit sets `full`. That flag is checked at the *top* of
`gaf_insert`, so every later frame is refused **without being measured** — including an 18×16
pebble with 58 free rows and the entire width in front of it.

The next gather sees `full` and resets the atlas whole. Frames re-decode on demand in the same
order, hit the same wall, and set the flag again. Every reset also clears the Classic++ twin,
throwing away restored art a sliced GPU job had already paid for.

<div class="tablewrap ap">
<svg class="ap-dia" viewBox="0 0 700 340" role="img" aria-label="A boulder cell too tall for the remaining rows sets the full flag, after which a small pebble that would have fitted is refused, and the atlas resets in a loop">
   <rect class="ap-atlas" x="30" y="20" width="330" height="248"/>
   <rect class="ap-dead" x="30" y="20" width="176" height="208"/>
   <rect class="ap-cell1" x="30" y="188" width="28" height="40"/>
   <rect class="ap-cell0" x="58" y="212" width="40" height="16"/>
   <rect class="ap-cell0" x="98" y="204" width="26" height="24"/>
   <rect class="ap-cell0" x="124" y="210" width="20" height="18"/>
   <rect class="ap-cell2" x="144" y="156" width="62" height="72"/>
   <rect class="ap-cell3" x="206" y="20" width="136" height="208"/>
   <rect class="ap-free" x="30" y="228" width="330" height="40"/>
   <text class="ap-sigtx" x="195" y="253" text-anchor="middle" font-size="11">58 rows &times; 512 free</text>
   <text class="ap-lab" x="30" y="14">the atlas, at the moment of the miss</text>

   <g transform="translate(410,26)">
     <text class="ap-lab" x="0" y="0">arriving next</text>
     <rect class="ap-bad" x="0" y="12" width="94" height="60"/>
     <text class="ap-badtx" x="8" y="34" font-size="11">boulder</text>
     <text class="ap-badtx" x="8" y="50" font-size="10">150 &times; 200</text>
     <text class="ap-badtx" x="8" y="64" font-size="10">taller than 58 &rarr; miss</text>
     <text class="ap-sigtx" x="104" y="46" font-size="11">&rarr;</text>
     <rect class="ap-bad" x="120" y="12" width="140" height="60"/>
     <text class="ap-badtx" x="128" y="34" font-size="11">full = 1</text>
     <text class="ap-badtx" x="128" y="50" font-size="10">checked at the top of</text>
     <text class="ap-badtx" x="128" y="64" font-size="10">every later insert</text>

     <rect class="ap-bad" x="0" y="96" width="260" height="52"/>
     <text class="ap-badtx" x="8" y="118" font-size="11">smallrock 18 &times; 16 &mdash; refused</text>
     <text class="ap-badtx" x="8" y="134" font-size="10">never measured; it would have fitted</text>

     <path class="ap-sig" d="M130 156 v22 h-100" marker-end="url(#ar2)"/>
     <text class="ap-sigtx" x="136" y="176" font-size="11">next gather: reset the whole atlas</text>
     <text class="ap-lab" x="136" y="192">&mdash; and the restored twin with it</text>
     <text class="ap-lab" x="136" y="208">&mdash; refill in the same order</text>
     <text class="ap-lab" x="136" y="224">&mdash; hit the same wall</text>
   </g>
   <defs><marker id="ar2" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="6" markerHeight="6" orient="auto">
     <polygon points="0,0 10,5 0,10" fill="currentColor"/></marker></defs>
  </svg>
</div>

**Reading the diagram:** the latch, and the loop it drove. The pebble is refused with **29,696
free texels in front of it**, because `full` is read before the cell is measured. The reset that
follows is not a recovery — the same arrival order produces the same wall, which is why the live
log showed thousands of generations in one session.

## 4. Rejected — a bigger atlas buys the waste, not the fit

Doubling the edge to 4096 does make everything fit — by making the stranded space *affordable*
rather than by removing it. The proportion wasted inside the region the packer consumes is
unchanged: **51 % at 2048, 55 % at 4096**. You pay four times the memory to keep throwing away
the same half.

<div class="tablewrap ap" style="display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:0">
<div style="padding:.5rem"><div class="ap-cap">2048 &mdash; what shipped: 197 / 229 placed</div>
<svg class="ap-plate" viewBox="0 0 520 520" role="img" aria-label="The 2048 atlas packed with 229 real feature frames">
<rect x="0" y="0.00" width="509.08" height="58.14" class="ap-shelfband"/>
<rect x="0" y="58.14" width="506.04" height="81.76" class="ap-shelfband"/>
<rect x="0" y="139.90" width="515.43" height="59.41" class="ap-shelfband"/>
<rect x="0" y="199.32" width="483.69" height="57.38" class="ap-shelfband"/>
<rect x="0" y="256.70" width="497.40" height="53.07" class="ap-shelfband"/>
<rect x="0" y="309.77" width="517.21" height="63.48" class="ap-shelfband"/>
<rect x="0" y="373.24" width="512.13" height="71.86" class="ap-shelfband"/>
<rect x="0.00" y="0.00" width="39.36" height="46.46" class="ap-f3"/>
<rect x="39.36" y="0.00" width="18.79" height="22.09" class="ap-f2"/>
<rect x="58.14" y="0.00" width="16.00" height="43.42" class="ap-f3"/>
<rect x="74.14" y="0.00" width="25.39" height="20.31" class="ap-f2"/>
<rect x="99.53" y="0.00" width="20.31" height="19.04" class="ap-f1"/>
<rect x="119.84" y="0.00" width="8.12" height="11.93" class="ap-f1"/>
<rect x="127.97" y="0.00" width="6.86" height="4.32" class="ap-f0"/>
<rect x="134.82" y="0.00" width="12.44" height="11.68" class="ap-f1"/>
<rect x="147.27" y="0.00" width="3.81" height="5.59" class="ap-f0"/>
<rect x="151.07" y="0.00" width="4.57" height="9.65" class="ap-f0"/>
<rect x="155.64" y="0.00" width="9.90" height="14.47" class="ap-f1"/>
<rect x="165.55" y="0.00" width="12.70" height="13.96" class="ap-f1"/>
<rect x="178.24" y="0.00" width="7.62" height="4.06" class="ap-f0"/>
<rect x="185.86" y="0.00" width="38.59" height="51.54" class="ap-f3"/>
<rect x="224.45" y="0.00" width="16.25" height="7.36" class="ap-f0"/>
<rect x="240.70" y="0.00" width="8.38" height="13.96" class="ap-f1"/>
<rect x="249.08" y="0.00" width="4.82" height="7.62" class="ap-f0"/>
<rect x="253.91" y="0.00" width="41.13" height="58.14" class="ap-f3"/>
<rect x="295.04" y="0.00" width="40.88" height="37.07" class="ap-f2"/>
<rect x="335.92" y="0.00" width="12.95" height="13.46" class="ap-f1"/>
<rect x="348.87" y="0.00" width="5.08" height="4.06" class="ap-f0"/>
<rect x="353.95" y="0.00" width="47.73" height="37.07" class="ap-f2"/>
<rect x="401.68" y="0.00" width="6.35" height="7.11" class="ap-f0"/>
<rect x="408.03" y="0.00" width="11.17" height="8.12" class="ap-f0"/>
<rect x="419.20" y="0.00" width="36.31" height="51.29" class="ap-f3"/>
<rect x="455.51" y="0.00" width="10.66" height="12.95" class="ap-f1"/>
<rect x="466.17" y="0.00" width="5.84" height="3.81" class="ap-f0"/>
<rect x="472.01" y="0.00" width="4.57" height="5.33" class="ap-f0"/>
<rect x="476.58" y="0.00" width="19.80" height="22.34" class="ap-f2"/>
<rect x="496.39" y="0.00" width="12.70" height="18.54" class="ap-f1"/>
<rect x="0.00" y="58.14" width="12.95" height="11.43" class="ap-f1"/>
<rect x="12.95" y="58.14" width="7.36" height="13.46" class="ap-f1"/>
<rect x="20.31" y="58.14" width="5.59" height="4.06" class="ap-f0"/>
<rect x="25.90" y="58.14" width="3.81" height="4.06" class="ap-f0"/>
<rect x="29.71" y="58.14" width="28.18" height="42.66" class="ap-f3"/>
<rect x="57.89" y="58.14" width="12.95" height="12.19" class="ap-f1"/>
<rect x="70.84" y="58.14" width="21.84" height="30.98" class="ap-f2"/>
<rect x="92.68" y="58.14" width="4.57" height="3.81" class="ap-f0"/>
<rect x="97.25" y="58.14" width="4.32" height="7.11" class="ap-f0"/>
<rect x="101.56" y="58.14" width="31.48" height="41.64" class="ap-f3"/>
<rect x="133.05" y="58.14" width="14.47" height="15.23" class="ap-f1"/>
<rect x="147.52" y="58.14" width="2.79" height="7.36" class="ap-f0"/>
<rect x="150.31" y="58.14" width="5.08" height="3.55" class="ap-f0"/>
<rect x="155.39" y="58.14" width="53.32" height="44.69" class="ap-f3"/>
<rect x="208.71" y="58.14" width="49.51" height="81.76" class="ap-f3"/>
<rect x="258.22" y="58.14" width="3.05" height="3.05" class="ap-f0"/>
<rect x="261.27" y="58.14" width="18.79" height="12.70" class="ap-f1"/>
<rect x="280.06" y="58.14" width="35.80" height="45.96" class="ap-f3"/>
<rect x="315.86" y="58.14" width="5.08" height="7.36" class="ap-f0"/>
<rect x="320.94" y="58.14" width="17.27" height="48.50" class="ap-f3"/>
<rect x="338.20" y="58.14" width="10.41" height="14.98" class="ap-f1"/>
<rect x="348.61" y="58.14" width="7.62" height="4.32" class="ap-f0"/>
<rect x="356.23" y="58.14" width="12.95" height="26.41" class="ap-f2"/>
<rect x="369.18" y="58.14" width="13.46" height="12.19" class="ap-f1"/>
<rect x="382.64" y="58.14" width="7.11" height="4.06" class="ap-f0"/>
<rect x="389.75" y="58.14" width="13.71" height="15.23" class="ap-f1"/>
<rect x="403.46" y="58.14" width="27.68" height="40.37" class="ap-f2"/>
<rect x="431.13" y="58.14" width="6.86" height="6.86" class="ap-f0"/>
<rect x="437.99" y="58.14" width="18.79" height="45.70" class="ap-f3"/>
<rect x="456.78" y="58.14" width="11.68" height="8.63" class="ap-f0"/>
<rect x="468.46" y="58.14" width="11.68" height="8.63" class="ap-f0"/>
<rect x="480.14" y="58.14" width="9.39" height="18.03" class="ap-f1"/>
<rect x="489.53" y="58.14" width="8.63" height="19.04" class="ap-f1"/>
<rect x="498.16" y="58.14" width="7.87" height="5.33" class="ap-f0"/>
<rect x="0.00" y="139.90" width="23.11" height="19.80" class="ap-f1"/>
<rect x="23.11" y="139.90" width="13.20" height="11.68" class="ap-f1"/>
<rect x="36.31" y="139.90" width="8.89" height="14.22" class="ap-f1"/>
<rect x="45.20" y="139.90" width="11.93" height="12.44" class="ap-f1"/>
<rect x="57.13" y="139.90" width="12.44" height="17.52" class="ap-f1"/>
<rect x="69.57" y="139.90" width="39.86" height="59.41" class="ap-f3"/>
<rect x="109.43" y="139.90" width="4.82" height="9.39" class="ap-f0"/>
<rect x="114.26" y="139.90" width="29.45" height="26.15" class="ap-f2"/>
<rect x="143.71" y="139.90" width="10.16" height="4.57" class="ap-f0"/>
<rect x="153.87" y="139.90" width="6.60" height="7.11" class="ap-f0"/>
<rect x="160.47" y="139.90" width="5.84" height="8.12" class="ap-f0"/>
<rect x="166.31" y="139.90" width="13.20" height="12.44" class="ap-f1"/>
<rect x="179.51" y="139.90" width="8.63" height="11.17" class="ap-f1"/>
<rect x="188.14" y="139.90" width="19.30" height="19.80" class="ap-f1"/>
<rect x="207.44" y="139.90" width="5.08" height="9.14" class="ap-f0"/>
<rect x="212.52" y="139.90" width="20.31" height="45.70" class="ap-f3"/>
<rect x="232.83" y="139.90" width="46.21" height="20.06" class="ap-f1"/>
<rect x="279.04" y="139.90" width="5.59" height="4.06" class="ap-f0"/>
<rect x="284.63" y="139.90" width="12.44" height="10.92" class="ap-f1"/>
<rect x="297.07" y="139.90" width="46.72" height="24.63" class="ap-f2"/>
<rect x="343.79" y="139.90" width="5.84" height="12.19" class="ap-f1"/>
<rect x="349.63" y="139.90" width="4.57" height="4.06" class="ap-f0"/>
<rect x="354.20" y="139.90" width="17.01" height="26.91" class="ap-f2"/>
<rect x="371.21" y="139.90" width="47.99" height="47.99" class="ap-f3"/>
<rect x="419.20" y="139.90" width="19.55" height="11.43" class="ap-f1"/>
<rect x="438.75" y="139.90" width="4.32" height="5.08" class="ap-f0"/>
<rect x="443.07" y="139.90" width="17.01" height="8.89" class="ap-f0"/>
<rect x="460.08" y="139.90" width="28.69" height="42.15" class="ap-f3"/>
<rect x="488.77" y="139.90" width="26.66" height="28.69" class="ap-f2"/>
<rect x="0.00" y="199.32" width="16.00" height="23.36" class="ap-f2"/>
<rect x="16.00" y="199.32" width="16.76" height="28.95" class="ap-f2"/>
<rect x="32.75" y="199.32" width="9.65" height="11.93" class="ap-f1"/>
<rect x="42.40" y="199.32" width="45.96" height="31.48" class="ap-f2"/>
<rect x="88.36" y="199.32" width="24.12" height="25.14" class="ap-f2"/>
<rect x="112.48" y="199.32" width="19.55" height="31.99" class="ap-f2"/>
<rect x="132.03" y="199.32" width="31.23" height="20.31" class="ap-f2"/>
<rect x="163.26" y="199.32" width="29.20" height="26.66" class="ap-f2"/>
<rect x="192.46" y="199.32" width="53.32" height="43.67" class="ap-f3"/>
<rect x="245.78" y="199.32" width="14.73" height="27.68" class="ap-f2"/>
<rect x="260.51" y="199.32" width="34.53" height="20.06" class="ap-f1"/>
<rect x="295.04" y="199.32" width="4.32" height="5.33" class="ap-f0"/>
<rect x="299.36" y="199.32" width="11.43" height="18.79" class="ap-f1"/>
<rect x="310.78" y="199.32" width="9.14" height="7.11" class="ap-f0"/>
<rect x="319.92" y="199.32" width="10.41" height="4.57" class="ap-f0"/>
<rect x="330.33" y="199.32" width="10.66" height="6.35" class="ap-f0"/>
<rect x="341.00" y="199.32" width="10.16" height="12.44" class="ap-f1"/>
<rect x="351.15" y="199.32" width="22.09" height="38.85" class="ap-f2"/>
<rect x="373.24" y="199.32" width="22.60" height="18.79" class="ap-f1"/>
<rect x="395.84" y="199.32" width="9.14" height="16.76" class="ap-f1"/>
<rect x="404.98" y="199.32" width="5.84" height="12.19" class="ap-f1"/>
<rect x="410.82" y="199.32" width="34.28" height="57.38" class="ap-f3"/>
<rect x="445.10" y="199.32" width="21.58" height="29.45" class="ap-f2"/>
<rect x="466.68" y="199.32" width="17.01" height="43.42" class="ap-f3"/>
<rect x="0.00" y="256.70" width="53.57" height="45.20" class="ap-f3"/>
<rect x="53.57" y="256.70" width="46.21" height="20.06" class="ap-f1"/>
<rect x="99.79" y="256.70" width="11.43" height="12.95" class="ap-f1"/>
<rect x="111.21" y="256.70" width="49.51" height="49.26" class="ap-f3"/>
<rect x="160.72" y="256.70" width="10.16" height="19.55" class="ap-f1"/>
<rect x="170.88" y="256.70" width="19.55" height="12.44" class="ap-f1"/>
<rect x="190.43" y="256.70" width="13.20" height="12.44" class="ap-f1"/>
<rect x="203.63" y="256.70" width="31.74" height="39.61" class="ap-f2"/>
<rect x="235.37" y="256.70" width="19.80" height="20.31" class="ap-f2"/>
<rect x="255.18" y="256.70" width="13.46" height="10.16" class="ap-f1"/>
<rect x="268.63" y="256.70" width="15.74" height="13.46" class="ap-f1"/>
<rect x="284.38" y="256.70" width="6.60" height="7.11" class="ap-f0"/>
<rect x="290.98" y="256.70" width="47.23" height="49.77" class="ap-f3"/>
<rect x="338.20" y="256.70" width="11.17" height="8.38" class="ap-f0"/>
<rect x="349.38" y="256.70" width="16.00" height="41.39" class="ap-f3"/>
<rect x="365.37" y="256.70" width="12.70" height="12.44" class="ap-f1"/>
<rect x="378.07" y="256.70" width="6.86" height="6.60" class="ap-f0"/>
<rect x="384.92" y="256.70" width="7.87" height="4.32" class="ap-f0"/>
<rect x="392.79" y="256.70" width="3.55" height="4.06" class="ap-f0"/>
<rect x="396.35" y="256.70" width="23.11" height="37.83" class="ap-f2"/>
<rect x="419.45" y="256.70" width="20.06" height="16.76" class="ap-f1"/>
<rect x="439.51" y="256.70" width="57.89" height="53.07" class="ap-f3"/>
<rect x="0.00" y="309.77" width="25.39" height="42.15" class="ap-f3"/>
<rect x="25.39" y="309.77" width="6.60" height="6.60" class="ap-f0"/>
<rect x="31.99" y="309.77" width="26.91" height="37.07" class="ap-f2"/>
<rect x="58.91" y="309.77" width="2.29" height="4.32" class="ap-f0"/>
<rect x="61.19" y="309.77" width="4.82" height="9.39" class="ap-f0"/>
<rect x="66.02" y="309.77" width="5.08" height="3.81" class="ap-f0"/>
<rect x="71.09" y="309.77" width="28.69" height="43.93" class="ap-f3"/>
<rect x="99.79" y="309.77" width="3.81" height="4.06" class="ap-f0"/>
<rect x="103.59" y="309.77" width="18.28" height="9.90" class="ap-f0"/>
<rect x="121.88" y="309.77" width="20.31" height="43.67" class="ap-f3"/>
<rect x="142.19" y="309.77" width="7.62" height="7.87" class="ap-f0"/>
<rect x="149.80" y="309.77" width="9.90" height="15.74" class="ap-f1"/>
<rect x="159.71" y="309.77" width="46.72" height="24.63" class="ap-f2"/>
<rect x="206.43" y="309.77" width="18.28" height="9.39" class="ap-f0"/>
<rect x="224.71" y="309.77" width="9.39" height="17.27" class="ap-f1"/>
<rect x="234.10" y="309.77" width="6.09" height="5.59" class="ap-f0"/>
<rect x="240.20" y="309.77" width="2.54" height="6.09" class="ap-f0"/>
<rect x="242.73" y="309.77" width="24.12" height="22.09" class="ap-f2"/>
<rect x="266.86" y="309.77" width="59.16" height="49.77" class="ap-f3"/>
<rect x="326.02" y="309.77" width="20.57" height="45.70" class="ap-f3"/>
<rect x="346.58" y="309.77" width="28.69" height="51.29" class="ap-f3"/>
<rect x="375.27" y="309.77" width="9.90" height="11.93" class="ap-f1"/>
<rect x="385.18" y="309.77" width="6.09" height="5.84" class="ap-f0"/>
<rect x="391.27" y="309.77" width="9.39" height="4.32" class="ap-f0"/>
<rect x="400.66" y="309.77" width="41.13" height="63.48" class="ap-f3"/>
<rect x="441.80" y="309.77" width="37.07" height="54.34" class="ap-f3"/>
<rect x="478.87" y="309.77" width="9.90" height="18.03" class="ap-f1"/>
<rect x="488.77" y="309.77" width="28.44" height="40.62" class="ap-f3"/>
<rect x="0.00" y="373.24" width="4.32" height="5.59" class="ap-f0"/>
<rect x="4.32" y="373.24" width="2.29" height="7.11" class="ap-f0"/>
<rect x="6.60" y="373.24" width="2.79" height="8.12" class="ap-f0"/>
<rect x="9.39" y="373.24" width="13.20" height="12.19" class="ap-f1"/>
<rect x="22.60" y="373.24" width="16.00" height="38.85" class="ap-f2"/>
<rect x="38.59" y="373.24" width="59.41" height="71.86" class="ap-f3"/>
<rect x="98.01" y="373.24" width="4.32" height="6.09" class="ap-f0"/>
<rect x="102.32" y="373.24" width="3.55" height="4.82" class="ap-f0"/>
<rect x="105.88" y="373.24" width="37.07" height="48.24" class="ap-f3"/>
<rect x="142.95" y="373.24" width="35.29" height="24.63" class="ap-f2"/>
<rect x="178.24" y="373.24" width="9.14" height="4.82" class="ap-f0"/>
<rect x="187.38" y="373.24" width="28.69" height="34.02" class="ap-f2"/>
<rect x="216.07" y="373.24" width="5.08" height="7.87" class="ap-f0"/>
<rect x="221.15" y="373.24" width="27.68" height="34.28" class="ap-f2"/>
<rect x="248.83" y="373.24" width="10.92" height="14.98" class="ap-f1"/>
<rect x="259.75" y="373.24" width="22.60" height="24.63" class="ap-f2"/>
<rect x="282.34" y="373.24" width="40.12" height="50.78" class="ap-f3"/>
<rect x="322.46" y="373.24" width="39.61" height="20.06" class="ap-f1"/>
<rect x="362.07" y="373.24" width="11.43" height="12.95" class="ap-f1"/>
<rect x="373.50" y="373.24" width="10.66" height="13.20" class="ap-f1"/>
<rect x="384.16" y="373.24" width="15.23" height="20.82" class="ap-f2"/>
<rect x="399.39" y="373.24" width="17.27" height="30.47" class="ap-f2"/>
<rect x="416.66" y="373.24" width="16.00" height="43.42" class="ap-f3"/>
<rect x="432.66" y="373.24" width="12.44" height="18.03" class="ap-f1"/>
<rect x="445.10" y="373.24" width="9.65" height="23.11" class="ap-f2"/>
<rect x="454.75" y="373.24" width="3.30" height="5.84" class="ap-f0"/>
<rect x="458.05" y="373.24" width="7.62" height="16.76" class="ap-f1"/>
<rect x="465.66" y="373.24" width="13.20" height="12.44" class="ap-f1"/>
<rect x="478.87" y="373.24" width="27.68" height="42.66" class="ap-f3"/>
<rect x="506.54" y="373.24" width="5.59" height="4.06" class="ap-f0"/>
</svg></div>
<div style="padding:.5rem"><div class="ap-cap">4096 &mdash; four times the memory: 229 / 229 placed</div>
<svg class="ap-plate" viewBox="0 0 520 520" role="img" aria-label="The 4096 atlas packed with 229 real feature frames">
<rect x="0" y="0.00" width="519.11" height="40.88" class="ap-shelfband"/>
<rect x="0" y="40.88" width="514.79" height="29.71" class="ap-shelfband"/>
<rect x="0" y="70.59" width="499.81" height="31.74" class="ap-shelfband"/>
<rect x="0" y="102.32" width="511.24" height="40.62" class="ap-shelfband"/>
<rect x="0.00" y="0.00" width="19.68" height="23.23" class="ap-f3"/>
<rect x="19.68" y="0.00" width="9.39" height="11.04" class="ap-f2"/>
<rect x="29.07" y="0.00" width="8.00" height="21.71" class="ap-f3"/>
<rect x="37.07" y="0.00" width="12.70" height="10.16" class="ap-f2"/>
<rect x="49.77" y="0.00" width="10.16" height="9.52" class="ap-f1"/>
<rect x="59.92" y="0.00" width="4.06" height="5.97" class="ap-f1"/>
<rect x="63.98" y="0.00" width="3.43" height="2.16" class="ap-f0"/>
<rect x="67.41" y="0.00" width="6.22" height="5.84" class="ap-f1"/>
<rect x="73.63" y="0.00" width="1.90" height="2.79" class="ap-f0"/>
<rect x="75.54" y="0.00" width="2.29" height="4.82" class="ap-f0"/>
<rect x="77.82" y="0.00" width="4.95" height="7.24" class="ap-f1"/>
<rect x="82.77" y="0.00" width="6.35" height="6.98" class="ap-f1"/>
<rect x="89.12" y="0.00" width="3.81" height="2.03" class="ap-f0"/>
<rect x="92.93" y="0.00" width="19.30" height="25.77" class="ap-f3"/>
<rect x="112.23" y="0.00" width="8.12" height="3.68" class="ap-f0"/>
<rect x="120.35" y="0.00" width="4.19" height="6.98" class="ap-f1"/>
<rect x="124.54" y="0.00" width="2.41" height="3.81" class="ap-f0"/>
<rect x="126.95" y="0.00" width="20.57" height="29.07" class="ap-f3"/>
<rect x="147.52" y="0.00" width="20.44" height="18.54" class="ap-f2"/>
<rect x="167.96" y="0.00" width="6.47" height="6.73" class="ap-f1"/>
<rect x="174.43" y="0.00" width="2.54" height="2.03" class="ap-f0"/>
<rect x="176.97" y="0.00" width="23.87" height="18.54" class="ap-f2"/>
<rect x="200.84" y="0.00" width="3.17" height="3.55" class="ap-f0"/>
<rect x="204.01" y="0.00" width="5.59" height="4.06" class="ap-f0"/>
<rect x="209.60" y="0.00" width="18.15" height="25.64" class="ap-f3"/>
<rect x="227.75" y="0.00" width="5.33" height="6.47" class="ap-f1"/>
<rect x="233.09" y="0.00" width="2.92" height="1.90" class="ap-f0"/>
<rect x="236.01" y="0.00" width="2.29" height="2.67" class="ap-f0"/>
<rect x="238.29" y="0.00" width="9.90" height="11.17" class="ap-f2"/>
<rect x="248.19" y="0.00" width="6.35" height="9.27" class="ap-f1"/>
<rect x="254.54" y="0.00" width="6.47" height="5.71" class="ap-f1"/>
<rect x="261.02" y="0.00" width="3.68" height="6.73" class="ap-f1"/>
<rect x="264.70" y="0.00" width="2.79" height="2.03" class="ap-f0"/>
<rect x="267.49" y="0.00" width="1.90" height="2.03" class="ap-f0"/>
<rect x="269.39" y="0.00" width="14.09" height="21.33" class="ap-f3"/>
<rect x="283.49" y="0.00" width="6.47" height="6.09" class="ap-f1"/>
<rect x="289.96" y="0.00" width="10.92" height="15.49" class="ap-f2"/>
<rect x="300.88" y="0.00" width="2.29" height="1.90" class="ap-f0"/>
<rect x="303.16" y="0.00" width="2.16" height="3.55" class="ap-f0"/>
<rect x="305.32" y="0.00" width="15.74" height="20.82" class="ap-f3"/>
<rect x="321.06" y="0.00" width="7.24" height="7.62" class="ap-f1"/>
<rect x="328.30" y="0.00" width="1.40" height="3.68" class="ap-f0"/>
<rect x="329.70" y="0.00" width="2.54" height="1.78" class="ap-f0"/>
<rect x="332.24" y="0.00" width="26.66" height="22.34" class="ap-f3"/>
<rect x="358.90" y="0.00" width="24.76" height="40.88" class="ap-f3"/>
<rect x="383.65" y="0.00" width="1.52" height="1.52" class="ap-f0"/>
<rect x="385.18" y="0.00" width="9.39" height="6.35" class="ap-f1"/>
<rect x="394.57" y="0.00" width="17.90" height="22.98" class="ap-f3"/>
<rect x="412.47" y="0.00" width="2.54" height="3.68" class="ap-f0"/>
<rect x="415.01" y="0.00" width="8.63" height="24.25" class="ap-f3"/>
<rect x="423.64" y="0.00" width="5.21" height="7.49" class="ap-f1"/>
<rect x="428.85" y="0.00" width="3.81" height="2.16" class="ap-f0"/>
<rect x="432.66" y="0.00" width="6.47" height="13.20" class="ap-f2"/>
<rect x="439.13" y="0.00" width="6.73" height="6.09" class="ap-f1"/>
<rect x="445.86" y="0.00" width="3.55" height="2.03" class="ap-f0"/>
<rect x="449.41" y="0.00" width="6.86" height="7.62" class="ap-f1"/>
<rect x="456.27" y="0.00" width="13.84" height="20.19" class="ap-f2"/>
<rect x="470.11" y="0.00" width="3.43" height="3.43" class="ap-f0"/>
<rect x="473.54" y="0.00" width="9.39" height="22.85" class="ap-f3"/>
<rect x="482.93" y="0.00" width="5.84" height="4.32" class="ap-f0"/>
<rect x="488.77" y="0.00" width="5.84" height="4.32" class="ap-f0"/>
<rect x="494.61" y="0.00" width="4.70" height="9.01" class="ap-f1"/>
<rect x="499.31" y="0.00" width="4.32" height="9.52" class="ap-f1"/>
<rect x="503.62" y="0.00" width="3.94" height="2.67" class="ap-f0"/>
<rect x="507.56" y="0.00" width="11.55" height="9.90" class="ap-f1"/>
<rect x="0.00" y="40.88" width="6.60" height="5.84" class="ap-f1"/>
<rect x="6.60" y="40.88" width="4.44" height="7.11" class="ap-f1"/>
<rect x="11.04" y="40.88" width="5.97" height="6.22" class="ap-f1"/>
<rect x="17.01" y="40.88" width="6.22" height="8.76" class="ap-f1"/>
<rect x="23.23" y="40.88" width="19.93" height="29.71" class="ap-f3"/>
<rect x="43.16" y="40.88" width="2.41" height="4.70" class="ap-f0"/>
<rect x="45.58" y="40.88" width="14.73" height="13.08" class="ap-f2"/>
<rect x="60.30" y="40.88" width="5.08" height="2.29" class="ap-f0"/>
<rect x="65.38" y="40.88" width="3.30" height="3.55" class="ap-f0"/>
<rect x="68.68" y="40.88" width="2.92" height="4.06" class="ap-f0"/>
<rect x="71.60" y="40.88" width="6.60" height="6.22" class="ap-f1"/>
<rect x="78.20" y="40.88" width="4.32" height="5.59" class="ap-f1"/>
<rect x="82.52" y="40.88" width="9.65" height="9.90" class="ap-f1"/>
<rect x="92.17" y="40.88" width="2.54" height="4.57" class="ap-f0"/>
<rect x="94.71" y="40.88" width="10.16" height="22.85" class="ap-f3"/>
<rect x="104.86" y="40.88" width="23.11" height="10.03" class="ap-f1"/>
<rect x="127.97" y="40.88" width="2.79" height="2.03" class="ap-f0"/>
<rect x="130.76" y="40.88" width="6.22" height="5.46" class="ap-f1"/>
<rect x="136.98" y="40.88" width="23.36" height="12.31" class="ap-f2"/>
<rect x="160.34" y="40.88" width="2.92" height="6.09" class="ap-f1"/>
<rect x="163.26" y="40.88" width="2.29" height="2.03" class="ap-f0"/>
<rect x="165.55" y="40.88" width="8.51" height="13.46" class="ap-f2"/>
<rect x="174.05" y="40.88" width="23.99" height="23.99" class="ap-f3"/>
<rect x="198.05" y="40.88" width="9.78" height="5.71" class="ap-f1"/>
<rect x="207.82" y="40.88" width="2.16" height="2.54" class="ap-f0"/>
<rect x="209.98" y="40.88" width="8.51" height="4.44" class="ap-f0"/>
<rect x="218.49" y="40.88" width="14.35" height="21.07" class="ap-f3"/>
<rect x="232.83" y="40.88" width="13.33" height="14.35" class="ap-f2"/>
<rect x="246.16" y="40.88" width="8.00" height="11.68" class="ap-f2"/>
<rect x="254.16" y="40.88" width="8.38" height="14.47" class="ap-f2"/>
<rect x="262.54" y="40.88" width="4.82" height="5.97" class="ap-f1"/>
<rect x="267.36" y="40.88" width="22.98" height="15.74" class="ap-f2"/>
<rect x="290.34" y="40.88" width="12.06" height="12.57" class="ap-f2"/>
<rect x="302.40" y="40.88" width="9.78" height="16.00" class="ap-f2"/>
<rect x="312.18" y="40.88" width="15.62" height="10.16" class="ap-f2"/>
<rect x="327.79" y="40.88" width="14.60" height="13.33" class="ap-f2"/>
<rect x="342.39" y="40.88" width="26.66" height="21.84" class="ap-f3"/>
<rect x="369.05" y="40.88" width="7.36" height="13.84" class="ap-f2"/>
<rect x="376.42" y="40.88" width="17.27" height="10.03" class="ap-f1"/>
<rect x="393.68" y="40.88" width="2.16" height="2.67" class="ap-f0"/>
<rect x="395.84" y="40.88" width="5.71" height="9.39" class="ap-f1"/>
<rect x="401.55" y="40.88" width="4.57" height="3.55" class="ap-f0"/>
<rect x="406.12" y="40.88" width="5.21" height="2.29" class="ap-f0"/>
<rect x="411.33" y="40.88" width="5.33" height="3.17" class="ap-f0"/>
<rect x="416.66" y="40.88" width="5.08" height="6.22" class="ap-f1"/>
<rect x="421.74" y="40.88" width="11.04" height="19.42" class="ap-f2"/>
<rect x="432.78" y="40.88" width="11.30" height="9.39" class="ap-f1"/>
<rect x="444.08" y="40.88" width="4.57" height="8.38" class="ap-f1"/>
<rect x="448.65" y="40.88" width="2.92" height="6.09" class="ap-f1"/>
<rect x="451.57" y="40.88" width="17.14" height="28.69" class="ap-f3"/>
<rect x="468.71" y="40.88" width="10.79" height="14.73" class="ap-f2"/>
<rect x="479.50" y="40.88" width="8.51" height="21.71" class="ap-f3"/>
<rect x="488.01" y="40.88" width="26.79" height="22.60" class="ap-f3"/>
<rect x="0.00" y="70.59" width="23.11" height="10.03" class="ap-f1"/>
<rect x="23.11" y="70.59" width="5.71" height="6.47" class="ap-f1"/>
<rect x="28.82" y="70.59" width="24.76" height="24.63" class="ap-f3"/>
<rect x="53.57" y="70.59" width="5.08" height="9.78" class="ap-f1"/>
<rect x="58.65" y="70.59" width="9.78" height="6.22" class="ap-f1"/>
<rect x="68.43" y="70.59" width="6.60" height="6.22" class="ap-f1"/>
<rect x="75.03" y="70.59" width="15.87" height="19.80" class="ap-f2"/>
<rect x="90.90" y="70.59" width="9.90" height="10.16" class="ap-f2"/>
<rect x="100.80" y="70.59" width="6.73" height="5.08" class="ap-f1"/>
<rect x="107.53" y="70.59" width="7.87" height="6.73" class="ap-f1"/>
<rect x="115.40" y="70.59" width="3.30" height="3.55" class="ap-f0"/>
<rect x="118.70" y="70.59" width="23.61" height="24.88" class="ap-f3"/>
<rect x="142.31" y="70.59" width="5.59" height="4.19" class="ap-f0"/>
<rect x="147.90" y="70.59" width="8.00" height="20.69" class="ap-f3"/>
<rect x="155.90" y="70.59" width="6.35" height="6.22" class="ap-f1"/>
<rect x="162.25" y="70.59" width="3.43" height="3.30" class="ap-f0"/>
<rect x="165.67" y="70.59" width="3.94" height="2.16" class="ap-f0"/>
<rect x="169.61" y="70.59" width="1.78" height="2.03" class="ap-f0"/>
<rect x="171.39" y="70.59" width="11.55" height="18.92" class="ap-f2"/>
<rect x="182.94" y="70.59" width="10.03" height="8.38" class="ap-f1"/>
<rect x="192.97" y="70.59" width="28.95" height="26.53" class="ap-f3"/>
<rect x="221.91" y="70.59" width="12.70" height="21.07" class="ap-f3"/>
<rect x="234.61" y="70.59" width="3.30" height="3.30" class="ap-f0"/>
<rect x="237.91" y="70.59" width="13.46" height="18.54" class="ap-f2"/>
<rect x="251.37" y="70.59" width="1.14" height="2.16" class="ap-f0"/>
<rect x="252.51" y="70.59" width="2.41" height="4.70" class="ap-f0"/>
<rect x="254.92" y="70.59" width="2.54" height="1.90" class="ap-f0"/>
<rect x="257.46" y="70.59" width="14.35" height="21.96" class="ap-f3"/>
<rect x="271.81" y="70.59" width="1.90" height="2.03" class="ap-f0"/>
<rect x="273.71" y="70.59" width="9.14" height="4.95" class="ap-f0"/>
<rect x="282.85" y="70.59" width="10.16" height="21.84" class="ap-f3"/>
<rect x="293.01" y="70.59" width="3.81" height="3.94" class="ap-f0"/>
<rect x="296.82" y="70.59" width="4.95" height="7.87" class="ap-f1"/>
<rect x="301.77" y="70.59" width="23.36" height="12.31" class="ap-f2"/>
<rect x="325.13" y="70.59" width="9.14" height="4.70" class="ap-f0"/>
<rect x="334.27" y="70.59" width="4.70" height="8.63" class="ap-f1"/>
<rect x="338.96" y="70.59" width="3.05" height="2.79" class="ap-f0"/>
<rect x="342.01" y="70.59" width="1.27" height="3.05" class="ap-f0"/>
<rect x="343.28" y="70.59" width="12.06" height="11.04" class="ap-f2"/>
<rect x="355.34" y="70.59" width="29.58" height="24.88" class="ap-f3"/>
<rect x="384.92" y="70.59" width="10.28" height="22.85" class="ap-f3"/>
<rect x="395.21" y="70.59" width="14.35" height="25.64" class="ap-f3"/>
<rect x="409.55" y="70.59" width="4.95" height="5.97" class="ap-f1"/>
<rect x="414.50" y="70.59" width="3.05" height="2.92" class="ap-f0"/>
<rect x="417.55" y="70.59" width="4.70" height="2.16" class="ap-f0"/>
<rect x="422.25" y="70.59" width="20.57" height="31.74" class="ap-f3"/>
<rect x="442.81" y="70.59" width="18.54" height="27.17" class="ap-f3"/>
<rect x="461.35" y="70.59" width="4.95" height="9.01" class="ap-f1"/>
<rect x="466.30" y="70.59" width="14.22" height="20.31" class="ap-f3"/>
<rect x="480.52" y="70.59" width="2.16" height="2.79" class="ap-f0"/>
<rect x="482.68" y="70.59" width="1.14" height="3.55" class="ap-f0"/>
<rect x="483.82" y="70.59" width="1.40" height="4.06" class="ap-f0"/>
<rect x="485.21" y="70.59" width="6.60" height="6.09" class="ap-f1"/>
<rect x="491.82" y="70.59" width="8.00" height="19.42" class="ap-f2"/>
<rect x="0.00" y="102.32" width="29.71" height="35.93" class="ap-f3"/>
<rect x="29.71" y="102.32" width="2.16" height="3.05" class="ap-f0"/>
<rect x="31.87" y="102.32" width="1.78" height="2.41" class="ap-f0"/>
<rect x="33.64" y="102.32" width="18.54" height="24.12" class="ap-f3"/>
<rect x="52.18" y="102.32" width="17.65" height="12.31" class="ap-f2"/>
<rect x="69.82" y="102.32" width="4.57" height="2.41" class="ap-f0"/>
<rect x="74.39" y="102.32" width="14.35" height="17.01" class="ap-f2"/>
<rect x="88.74" y="102.32" width="2.54" height="3.94" class="ap-f0"/>
<rect x="91.28" y="102.32" width="13.84" height="17.14" class="ap-f2"/>
<rect x="105.12" y="102.32" width="5.46" height="7.49" class="ap-f1"/>
<rect x="110.58" y="102.32" width="11.30" height="12.31" class="ap-f2"/>
<rect x="121.88" y="102.32" width="20.06" height="25.39" class="ap-f3"/>
<rect x="141.93" y="102.32" width="19.80" height="10.03" class="ap-f1"/>
<rect x="161.74" y="102.32" width="5.71" height="6.47" class="ap-f1"/>
<rect x="167.45" y="102.32" width="5.33" height="6.60" class="ap-f1"/>
<rect x="172.78" y="102.32" width="7.62" height="10.41" class="ap-f2"/>
<rect x="180.40" y="102.32" width="8.63" height="15.23" class="ap-f2"/>
<rect x="189.03" y="102.32" width="8.00" height="21.71" class="ap-f3"/>
<rect x="197.03" y="102.32" width="6.22" height="9.01" class="ap-f1"/>
<rect x="203.25" y="102.32" width="4.82" height="11.55" class="ap-f2"/>
<rect x="208.08" y="102.32" width="1.65" height="2.92" class="ap-f0"/>
<rect x="209.73" y="102.32" width="3.81" height="8.38" class="ap-f1"/>
<rect x="213.54" y="102.32" width="6.60" height="6.22" class="ap-f1"/>
<rect x="220.14" y="102.32" width="13.84" height="21.33" class="ap-f3"/>
<rect x="233.97" y="102.32" width="2.79" height="2.03" class="ap-f0"/>
<rect x="236.77" y="102.32" width="21.46" height="39.48" class="ap-f3"/>
<rect x="258.22" y="102.32" width="2.67" height="2.29" class="ap-f0"/>
<rect x="260.89" y="102.32" width="11.04" height="24.76" class="ap-f3"/>
<rect x="271.93" y="102.32" width="1.40" height="1.78" class="ap-f0"/>
<rect x="273.33" y="102.32" width="26.28" height="40.62" class="ap-f3"/>
<rect x="299.61" y="102.32" width="2.92" height="7.24" class="ap-f1"/>
<rect x="302.53" y="102.32" width="5.46" height="6.22" class="ap-f1"/>
<rect x="307.99" y="102.32" width="6.86" height="9.27" class="ap-f1"/>
<rect x="314.84" y="102.32" width="6.09" height="5.46" class="ap-f1"/>
<rect x="320.94" y="102.32" width="9.14" height="6.73" class="ap-f1"/>
<rect x="330.08" y="102.32" width="8.89" height="4.70" class="ap-f0"/>
<rect x="338.96" y="102.32" width="4.95" height="6.73" class="ap-f1"/>
<rect x="343.92" y="102.32" width="8.25" height="5.21" class="ap-f1"/>
<rect x="352.17" y="102.32" width="7.11" height="4.57" class="ap-f0"/>
<rect x="359.28" y="102.32" width="2.79" height="5.71" class="ap-f1"/>
<rect x="362.07" y="102.32" width="2.03" height="2.79" class="ap-f0"/>
<rect x="364.10" y="102.32" width="8.12" height="9.14" class="ap-f1"/>
<rect x="372.23" y="102.32" width="16.12" height="12.57" class="ap-f2"/>
<rect x="388.35" y="102.32" width="2.41" height="2.79" class="ap-f0"/>
<rect x="390.76" y="102.32" width="5.59" height="13.08" class="ap-f2"/>
<rect x="396.35" y="102.32" width="7.11" height="8.38" class="ap-f1"/>
<rect x="403.46" y="102.32" width="5.21" height="4.19" class="ap-f0"/>
<rect x="408.66" y="102.32" width="6.73" height="6.60" class="ap-f1"/>
<rect x="415.39" y="102.32" width="2.92" height="2.79" class="ap-f0"/>
<rect x="418.31" y="102.32" width="19.30" height="25.64" class="ap-f3"/>
<rect x="437.61" y="102.32" width="6.60" height="6.73" class="ap-f1"/>
<rect x="444.21" y="102.32" width="3.81" height="8.38" class="ap-f1"/>
<rect x="448.02" y="102.32" width="4.95" height="6.98" class="ap-f1"/>
<rect x="452.97" y="102.32" width="23.36" height="12.31" class="ap-f2"/>
<rect x="476.33" y="102.32" width="13.84" height="21.33" class="ap-f3"/>
<rect x="490.17" y="102.32" width="2.03" height="2.79" class="ap-f0"/>
<rect x="492.20" y="102.32" width="19.04" height="35.29" class="ap-f3"/>
</svg></div>
</div>

**Reading the plates:** both are the real 229 feature frames of Town & Country, packed by a
line-for-line model of `gaf_insert` in the same arrival order, drawn at the same on-screen size.
Grey is shelf the packer consumed; the colour inside it is the only part that holds art — olive
under 40 texels tall, tan 40–80, slate 80–160, dark slate over 160. The 4096 plate is the
identical picture at half the scale: the grey does not get denser, there is simply more of it to
spare.

| what a bigger atlas costs | 2048 | 4096 | Δ |
|---|---|---|---|
| R8 index texture | 4.2 MB | 16.8 MB | ×4 |
| Classic++ RGBA8 twin | 16.8 MB | 67.1 MB | ×4 |
| per atlas, both | **21.0 MB** | **83.9 MB** | ×4 |
| restore repaint on every resize | — | the whole sliced GPU job, again | |
| cells stranded inside consumed shelves | 51 % | 55 % | **unchanged** |

And a resize is a reset with extra steps: GL cannot resize a texture in place, so growing means
allocate-new and re-upload — exactly what the reset already did — and it moves every entry's
`u0..v1`, so `gen` bumps and anything that baked a UV into a vertex buffer drops its cache. Same
disruption, more memory.

## 5. Rejected — pages are the better idea, and still the wrong one here

Paging is genuinely different from growing, and the difference is the good part: **a new page
does not move the UVs already in the old one.** `gen` holds, no cache drops, no restored art is
thrown away. That is a real advantage and it is worth remembering for the day a working set
truly exceeds a page.

The cost is that a page index has to reach the fragment shader, and it can only get there
through every consumer at once.

<div class="tablewrap ap">
<svg class="ap-dia" viewBox="0 0 700 360" role="img" aria-label="Growing the atlas moves every UV and bumps the generation counter; paging keeps UVs stable but adds a page field to three vertex streams and splits one draw call into one per page">
   <text class="ap-lab" x="20" y="16">GROW</text>
   <line class="ap-thin" x1="20" y1="24" x2="330" y2="24"/>
   <rect class="ap-atlas" x="20" y="40" width="120" height="96"/>
   <rect class="ap-cell0" x="26" y="46" width="34" height="26"/>
   <rect class="ap-cell1" x="60" y="46" width="26" height="38"/>
   <rect class="ap-cell2" x="86" y="46" width="44" height="20"/>
   <path class="ap-sig" d="M152 88 h26" marker-end="url(#ar25)"/>
   <rect class="ap-atlas" x="190" y="40" width="140" height="112"/>
   <rect class="ap-cell1" x="196" y="46" width="26" height="38"/>
   <rect class="ap-cell2" x="222" y="46" width="44" height="20"/>
   <rect class="ap-cell0" x="266" y="46" width="34" height="26"/>
   <text class="ap-badtx" x="20" y="172" font-size="11">every u0..v1 moved</text>
   <text class="ap-badtx" x="20" y="188" font-size="11">gen++ &rarr; baked-UV caches drop</text>
   <text class="ap-badtx" x="20" y="204" font-size="11">twin repainted from zero</text>

   <text class="ap-lab" x="370" y="16">PAGE</text>
   <line class="ap-thin" x1="370" y1="24" x2="680" y2="24"/>
   <rect class="ap-atlas" x="370" y="40" width="120" height="96"/>
   <rect class="ap-cell0" x="376" y="46" width="34" height="26"/>
   <rect class="ap-cell1" x="410" y="46" width="26" height="38"/>
   <rect class="ap-cell2" x="436" y="46" width="44" height="20"/>
   <text class="ap-lab" x="370" y="150">page 0 &mdash; untouched</text>
   <rect class="ap-atlas" x="510" y="40" width="120" height="96"/>
   <rect class="ap-cell3" x="516" y="46" width="52" height="44"/>
   <text class="ap-lab" x="510" y="150">page 1 &mdash; new</text>
   <text class="ap-sigtx" x="370" y="172" font-size="11">UVs hold. gen holds. nothing drops.</text>

   <line class="ap-thin" x1="20" y1="230" x2="680" y2="230"/>
   <text class="ap-lab" x="20" y="250">WHAT IT COSTS, EVERY CONSUMER</text>
   <rect class="ap-bad" x="20" y="262" width="300" height="80"/>
   <text class="ap-badtx" x="30" y="282" font-size="11">the vertex record gains a field</text>
   <text x="30" y="300" font-size="10" class="ap-badtx">x y enc u v ck mode wx wz lam &rarr; + page</text>
   <text x="30" y="318" font-size="10" class="ap-badtx">feature stream &middot; effects stream &middot; unit stream</text>
   <text x="30" y="334" font-size="10" class="ap-badtx">sampler2D &rarr; sampler2DArray in each shader</text>
   <rect class="ap-bad" x="340" y="262" width="150" height="80"/>
   <text class="ap-badtx" x="350" y="282" font-size="11">or: one draw</text>
   <text class="ap-badtx" x="350" y="300" font-size="11">becomes one per page</text>
   <text class="ap-badtx" x="350" y="318" font-size="10">quads sorted by page first</text>
   <text class="ap-badtx" x="350" y="334" font-size="10">breaks one-draw-per-bucket</text>
   <rect class="ap-bad" x="510" y="262" width="170" height="80"/>
   <text class="ap-badtx" x="520" y="282" font-size="11">the restorer paints</text>
   <text class="ap-badtx" x="520" y="300" font-size="11">into the atlas</text>
   <text class="ap-badtx" x="520" y="318" font-size="10">as a render target &rarr; layered</text>
   <text class="ap-badtx" x="520" y="334" font-size="10">targets, or a job per page</text>
   <defs><marker id="ar25" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="6" markerHeight="6" orient="auto">
     <polygon points="0,0 10,5 0,10" fill="var(--signal)"/></marker></defs>
  </svg>
</div>

**Reading the diagram:** left, growing — the shelf restarts at the origin, so every entry lands
somewhere new and `gen` bumps. Right, paging — page 0 is never touched, which is the whole point
of it. Below, the bill: **three vertex streams, every sprite shader, and the restorer's render
target**, to buy capacity for a working set that fits in half of one page.

## 6. The fix — sort at the one moment you already know everything

The first proposal here was several open shelves with the latch removed. Simulated against the
real frames it was **worse than what shipped** — 172 placed against 197, and as low as 73 on an
unlucky order — because keeping shelves open burns vertical space opening them. It is recorded
because the correction is the point: the packer was never the thing to improve. It only ever
fails because of **the order it is fed in**.

A reset is the one moment that order is knowable. The atlas has just observed the exact working
set, and `tagpu_gaf_atlas_reset` never cleared `ents`, so every entry's width and height survives
it. So the recycle became a **repack**: the surviving entries are re-laid tallest cell first — a
counting sort over the cell height, which needs no comparator and no allocation and therefore
runs inside a frame — and because the tallest cell on a shelf is now always the one that opened
it, the shelf never grows under a later arrival. That is the entire waste mechanism, removed by
choosing an order rather than by adding a data structure.

<div class="tablewrap ap">
<svg class="ap-dia" viewBox="0 0 700 470" role="img" aria-label="Twenty fictive feature frames packed twice into the same 512-texel atlas: in arrival order they span 182 rows, tallest-first they span 142">
   <rect class="ap-atlas" x="60" y="40" width="512" height="182"/>
   <rect class="ap-atlas" x="60" y="300" width="512" height="142"/>
<text class="ap-lab" x="60" y="32"></text>
<rect class="ap-dead" x="60" y="40" width="512" height="112"/>
<rect class="ap-dead" x="60" y="152" width="512" height="70"/>
<rect class="ap-cell2" x="60" y="40" width="44" height="60"/>
<rect class="ap-cell1" x="104" y="40" width="62" height="24"/>
<rect class="ap-cell1" x="166" y="40" width="40" height="36"/>
<rect class="ap-cell1" x="206" y="40" width="30" height="28"/>
<rect class="ap-cell0" x="236" y="40" width="20" height="18"/>
<rect class="ap-cell3" x="256" y="40" width="98" height="112"/>
<rect class="ap-cell2" x="354" y="40" width="56" height="52"/>
<rect class="ap-cell0" x="410" y="40" width="16" height="14"/>
<rect class="ap-cell1" x="426" y="40" width="26" height="30"/>
<rect class="ap-cell2" x="452" y="40" width="48" height="66"/>
<rect class="ap-cell0" x="500" y="40" width="70" height="20"/>
<rect class="ap-cell0" x="60" y="152" width="84" height="16"/>
<rect class="ap-cell1" x="144" y="152" width="34" height="34"/>
<rect class="ap-cell2" x="178" y="152" width="40" height="58"/>
<rect class="ap-cell1" x="218" y="152" width="30" height="44"/>
<rect class="ap-cell1" x="248" y="152" width="22" height="26"/>
<rect class="ap-cell0" x="270" y="152" width="60" height="22"/>
<rect class="ap-cell2" x="330" y="152" width="76" height="70"/>
<rect class="ap-cell0" x="406" y="152" width="18" height="20"/>
<rect class="ap-cell2" x="424" y="152" width="46" height="62"/>
<text class="ap-lab" x="60" y="292"></text>
<rect class="ap-dead" x="60" y="300" width="512" height="112"/>
<rect class="ap-dead" x="60" y="412" width="512" height="30"/>
<rect class="ap-cell3" x="60" y="300" width="98" height="112"/>
<rect class="ap-cell2" x="158" y="300" width="76" height="70"/>
<rect class="ap-cell2" x="234" y="300" width="48" height="66"/>
<rect class="ap-cell2" x="282" y="300" width="46" height="62"/>
<rect class="ap-cell2" x="328" y="300" width="44" height="60"/>
<rect class="ap-cell2" x="372" y="300" width="40" height="58"/>
<rect class="ap-cell2" x="412" y="300" width="56" height="52"/>
<rect class="ap-cell1" x="468" y="300" width="30" height="44"/>
<rect class="ap-cell1" x="498" y="300" width="40" height="36"/>
<rect class="ap-cell1" x="538" y="300" width="34" height="34"/>
<rect class="ap-cell1" x="60" y="412" width="26" height="30"/>
<rect class="ap-cell1" x="86" y="412" width="30" height="28"/>
<rect class="ap-cell1" x="116" y="412" width="22" height="26"/>
<rect class="ap-cell1" x="138" y="412" width="62" height="24"/>
<rect class="ap-cell0" x="200" y="412" width="60" height="22"/>
<rect class="ap-cell0" x="260" y="412" width="70" height="20"/>
<rect class="ap-cell0" x="330" y="412" width="18" height="20"/>
<rect class="ap-cell0" x="348" y="412" width="20" height="18"/>
<rect class="ap-cell0" x="368" y="412" width="84" height="16"/>
<rect class="ap-cell0" x="452" y="412" width="16" height="14"/>
   <text class="ap-lab" x="52" y="46" text-anchor="end">arrival</text>
   <text class="ap-lab" x="52" y="60" text-anchor="end">order</text>
   <text class="ap-lab" x="52" y="306" text-anchor="end">tallest</text>
   <text class="ap-lab" x="52" y="320" text-anchor="end">first</text>
   <text class="ap-badtx" x="584" y="46" font-size="11">span 182 rows</text>
   <text class="ap-lab" x="584" y="60">46&nbsp;% of it is art</text>
   <text class="ap-sigtx" x="584" y="306" font-size="11">span 142 rows</text>
   <text class="ap-lab" x="584" y="320">59&nbsp;% of it is art</text>
   <line class="ap-thin" x1="360" y1="20" x2="360" y2="40" stroke-dasharray="2 3"/>
   <text class="ap-badtx" x="366" y="30" font-size="10">hive arrives 6th and raises the whole shelf to 112</text>
   <line class="ap-thin" x1="120" y1="280" x2="120" y2="300" stroke-dasharray="2 3"/>
   <text class="ap-sigtx" x="126" y="292" font-size="10">hive opens the shelf instead &mdash; nothing shorter has to live under it</text>
   <text class="ap-lab" x="60" y="460">the same twenty frames, the same packer, the same atlas &mdash; only the order differs</text>
  </svg>
</div>

**Reading the diagram:** twenty fictive feature frames — `tree1`, `hive`, `crater`, `scar`,
`pebble` and the rest — packed twice into the same 512-texel atlas by the same `gaf_insert`
geometry. Above, map order: `hive` arrives sixth and lifts shelf 0 from 60 rows to 112,
stranding the five short cells already on it under 50 rows of nothing. Below, tallest first:
**40 rows saved on 20 frames**, and the short cells end up in a band 30 rows deep instead of
scattered under tall ones.

## 7. The mechanism — nothing moves; the rects are reserved

"Repack" usually means moving texels. This one cannot: an entry records the frame's address, its
size and its rect, and **never its decoded pixels**. There is nowhere to copy from. GL 3.3 core
also has no `glCopyImageSubData`, so shuffling texels inside the texture would need a scratch
surface and a blit per frame.

It does not need one. The repack only assigns rects and marks each entry `resv` with `ok = 0`,
which keeps `tagpu_gaf_atlas_find` refusing it — there is nothing there to sample yet. The next
time the pass asks for that frame it decodes it from RLE exactly as it did the first time, and
`atlas_paint` uploads it into the rect already waiting. **That is the work one old reset did,
done once instead of sixty times a second.**

<div class="tablewrap ap">
<svg class="ap-dia" viewBox="0 0 700 240" role="img" aria-label="An entry through a repack: painted at its old rect, then reserved at a new rect with no upload, then painted at the new rect by the next atlas_get">
   <rect class="ap-atlas" x="20" y="46" width="180" height="150"/>
   <rect class="ap-cell2" x="96" y="70" width="44" height="60"/>
   <text x="118" y="146" text-anchor="middle" font-size="10">tree1</text>
   <text class="ap-lab" x="110" y="38" text-anchor="middle">1 &middot; atlas full</text>
   <text class="ap-lab" x="110" y="212" text-anchor="middle">painted at rect A</text>

   <rect class="ap-atlas" x="260" y="46" width="180" height="150"/>
   <rect x="270" y="56" width="44" height="60" fill="none" stroke="currentColor"
         stroke-width="1.2" stroke-dasharray="4 3"/>
   <text class="ap-lab" x="292" y="132" text-anchor="middle">tree1</text>
   <text class="ap-lab" x="350" y="38" text-anchor="middle">2 &middot; repack</text>
   <text class="ap-sigtx" x="350" y="212" text-anchor="middle" font-size="11">reserved at rect B</text>
   <text class="ap-lab" x="350" y="226" text-anchor="middle">no upload, no decode</text>

   <rect class="ap-atlas" x="500" y="46" width="180" height="150"/>
   <rect class="ap-cell2" x="510" y="56" width="44" height="60"/>
   <text x="532" y="132" text-anchor="middle" font-size="10">tree1</text>
   <text class="ap-lab" x="590" y="38" text-anchor="middle">3 &middot; next frame asks</text>
   <text class="ap-lab" x="590" y="212" text-anchor="middle">painted at rect B</text>

   <line class="ap-thin" x1="206" y1="112" x2="252" y2="112" marker-end="url(#ar7)"/>
   <text class="ap-lab" x="229" y="100" text-anchor="middle">sort by h</text>
   <text class="ap-lab" x="229" y="132" text-anchor="middle">w, h only</text>
   <line class="ap-thin" x1="446" y1="112" x2="492" y2="112" marker-end="url(#ar7)"/>
   <text class="ap-lab" x="469" y="100" text-anchor="middle">RLE decode</text>
   <text class="ap-lab" x="469" y="132" text-anchor="middle">TexSubImage</text>
   <defs><marker id="ar7" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6"
     orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  </svg>
</div>

**Reading the diagram:** one entry through a repack. Step 2 reads **only `w` and `h`** out of the
entry — both bounded to 1…`TAGPU_GAF_DECMAX` before the entry existed — and dereferences no
pointer it stores, so a stale entry cannot make the geometry address outside the atlas. The
Classic++ twin is cleared once here rather than once per frame, which is what finally lets the
restorer converge while zoomed out.

## 8. The same 229 frames, before and after

<div class="tablewrap ap" style="display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:0">
<div style="padding:.5rem"><div class="ap-cap">Arrival order &mdash; 197 / 229, shelves span 86&nbsp;%</div>
<svg class="ap-plate" viewBox="0 0 520 520" role="img" aria-label="The real 229 feature frames in arrival order: 197 placed, shelves spanning 86 percent of the ap-atlas">
  <rect x="0" y="0.00" width="520" height="58.14" class="ap-shelfband"/>
<rect x="0" y="58.14" width="520" height="81.76" class="ap-shelfband"/>
<rect x="0" y="139.90" width="520" height="59.41" class="ap-shelfband"/>
<rect x="0" y="199.32" width="520" height="57.38" class="ap-shelfband"/>
<rect x="0" y="256.70" width="520" height="53.07" class="ap-shelfband"/>
<rect x="0" y="309.77" width="520" height="63.48" class="ap-shelfband"/>
<rect x="0" y="373.24" width="520" height="71.86" class="ap-shelfband"/>
<rect x="0.00" y="0.00" width="39.36" height="46.46" class="ap-f3"/>
<rect x="39.36" y="0.00" width="18.79" height="22.09" class="ap-f2"/>
<rect x="58.14" y="0.00" width="16.00" height="43.42" class="ap-f3"/>
<rect x="74.14" y="0.00" width="25.39" height="20.31" class="ap-f2"/>
<rect x="99.53" y="0.00" width="20.31" height="19.04" class="ap-f1"/>
<rect x="119.84" y="0.00" width="8.12" height="11.93" class="ap-f1"/>
<rect x="127.97" y="0.00" width="6.86" height="4.32" class="ap-f0"/>
<rect x="134.82" y="0.00" width="12.44" height="11.68" class="ap-f1"/>
<rect x="147.27" y="0.00" width="3.81" height="5.59" class="ap-f0"/>
<rect x="151.07" y="0.00" width="4.57" height="9.65" class="ap-f0"/>
<rect x="155.64" y="0.00" width="9.90" height="14.47" class="ap-f1"/>
<rect x="165.55" y="0.00" width="12.70" height="13.96" class="ap-f1"/>
<rect x="178.24" y="0.00" width="7.62" height="4.06" class="ap-f0"/>
<rect x="185.86" y="0.00" width="38.59" height="51.54" class="ap-f3"/>
<rect x="224.45" y="0.00" width="16.25" height="7.36" class="ap-f0"/>
<rect x="240.70" y="0.00" width="8.38" height="13.96" class="ap-f1"/>
<rect x="249.08" y="0.00" width="4.82" height="7.62" class="ap-f0"/>
<rect x="253.91" y="0.00" width="41.13" height="58.14" class="ap-f3"/>
<rect x="295.04" y="0.00" width="40.88" height="37.07" class="ap-f2"/>
<rect x="335.92" y="0.00" width="12.95" height="13.46" class="ap-f1"/>
<rect x="348.87" y="0.00" width="5.08" height="4.06" class="ap-f0"/>
<rect x="353.95" y="0.00" width="47.73" height="37.07" class="ap-f2"/>
<rect x="401.68" y="0.00" width="6.35" height="7.11" class="ap-f0"/>
<rect x="408.03" y="0.00" width="11.17" height="8.12" class="ap-f0"/>
<rect x="419.20" y="0.00" width="36.31" height="51.29" class="ap-f3"/>
<rect x="455.51" y="0.00" width="10.66" height="12.95" class="ap-f1"/>
<rect x="466.17" y="0.00" width="5.84" height="3.81" class="ap-f0"/>
<rect x="472.01" y="0.00" width="4.57" height="5.33" class="ap-f0"/>
<rect x="476.58" y="0.00" width="19.80" height="22.34" class="ap-f2"/>
<rect x="496.39" y="0.00" width="12.70" height="18.54" class="ap-f1"/>
<rect x="0.00" y="58.14" width="12.95" height="11.43" class="ap-f1"/>
<rect x="12.95" y="58.14" width="7.36" height="13.46" class="ap-f1"/>
<rect x="20.31" y="58.14" width="5.59" height="4.06" class="ap-f0"/>
<rect x="25.90" y="58.14" width="3.81" height="4.06" class="ap-f0"/>
<rect x="29.71" y="58.14" width="28.18" height="42.66" class="ap-f3"/>
<rect x="57.89" y="58.14" width="12.95" height="12.19" class="ap-f1"/>
<rect x="70.84" y="58.14" width="21.84" height="30.98" class="ap-f2"/>
<rect x="92.68" y="58.14" width="4.57" height="3.81" class="ap-f0"/>
<rect x="97.25" y="58.14" width="4.32" height="7.11" class="ap-f0"/>
<rect x="101.56" y="58.14" width="31.48" height="41.64" class="ap-f3"/>
<rect x="133.05" y="58.14" width="14.47" height="15.23" class="ap-f1"/>
<rect x="147.52" y="58.14" width="2.79" height="7.36" class="ap-f0"/>
<rect x="150.31" y="58.14" width="5.08" height="3.55" class="ap-f0"/>
<rect x="155.39" y="58.14" width="53.32" height="44.69" class="ap-f3"/>
<rect x="208.71" y="58.14" width="49.51" height="81.76" class="ap-f3"/>
<rect x="258.22" y="58.14" width="3.05" height="3.05" class="ap-f0"/>
<rect x="261.27" y="58.14" width="18.79" height="12.70" class="ap-f1"/>
<rect x="280.06" y="58.14" width="35.80" height="45.96" class="ap-f3"/>
<rect x="315.86" y="58.14" width="5.08" height="7.36" class="ap-f0"/>
<rect x="320.94" y="58.14" width="17.27" height="48.50" class="ap-f3"/>
<rect x="338.20" y="58.14" width="10.41" height="14.98" class="ap-f1"/>
<rect x="348.61" y="58.14" width="7.62" height="4.32" class="ap-f0"/>
<rect x="356.23" y="58.14" width="12.95" height="26.41" class="ap-f2"/>
<rect x="369.18" y="58.14" width="13.46" height="12.19" class="ap-f1"/>
<rect x="382.64" y="58.14" width="7.11" height="4.06" class="ap-f0"/>
<rect x="389.75" y="58.14" width="13.71" height="15.23" class="ap-f1"/>
<rect x="403.46" y="58.14" width="27.68" height="40.37" class="ap-f2"/>
<rect x="431.13" y="58.14" width="6.86" height="6.86" class="ap-f0"/>
<rect x="437.99" y="58.14" width="18.79" height="45.70" class="ap-f3"/>
<rect x="456.78" y="58.14" width="11.68" height="8.63" class="ap-f0"/>
<rect x="468.46" y="58.14" width="11.68" height="8.63" class="ap-f0"/>
<rect x="480.14" y="58.14" width="9.39" height="18.03" class="ap-f1"/>
<rect x="489.53" y="58.14" width="8.63" height="19.04" class="ap-f1"/>
<rect x="498.16" y="58.14" width="7.87" height="5.33" class="ap-f0"/>
<rect x="0.00" y="139.90" width="23.11" height="19.80" class="ap-f1"/>
<rect x="23.11" y="139.90" width="13.20" height="11.68" class="ap-f1"/>
<rect x="36.31" y="139.90" width="8.89" height="14.22" class="ap-f1"/>
<rect x="45.20" y="139.90" width="11.93" height="12.44" class="ap-f1"/>
<rect x="57.13" y="139.90" width="12.44" height="17.52" class="ap-f1"/>
<rect x="69.57" y="139.90" width="39.86" height="59.41" class="ap-f3"/>
<rect x="109.43" y="139.90" width="4.82" height="9.39" class="ap-f0"/>
<rect x="114.26" y="139.90" width="29.45" height="26.15" class="ap-f2"/>
<rect x="143.71" y="139.90" width="10.16" height="4.57" class="ap-f0"/>
<rect x="153.87" y="139.90" width="6.60" height="7.11" class="ap-f0"/>
<rect x="160.47" y="139.90" width="5.84" height="8.12" class="ap-f0"/>
<rect x="166.31" y="139.90" width="13.20" height="12.44" class="ap-f1"/>
<rect x="179.51" y="139.90" width="8.63" height="11.17" class="ap-f1"/>
<rect x="188.14" y="139.90" width="19.30" height="19.80" class="ap-f1"/>
<rect x="207.44" y="139.90" width="5.08" height="9.14" class="ap-f0"/>
<rect x="212.52" y="139.90" width="20.31" height="45.70" class="ap-f3"/>
<rect x="232.83" y="139.90" width="46.21" height="20.06" class="ap-f1"/>
<rect x="279.04" y="139.90" width="5.59" height="4.06" class="ap-f0"/>
<rect x="284.63" y="139.90" width="12.44" height="10.92" class="ap-f1"/>
<rect x="297.07" y="139.90" width="46.72" height="24.63" class="ap-f2"/>
<rect x="343.79" y="139.90" width="5.84" height="12.19" class="ap-f1"/>
<rect x="349.63" y="139.90" width="4.57" height="4.06" class="ap-f0"/>
<rect x="354.20" y="139.90" width="17.01" height="26.91" class="ap-f2"/>
<rect x="371.21" y="139.90" width="47.99" height="47.99" class="ap-f3"/>
<rect x="419.20" y="139.90" width="19.55" height="11.43" class="ap-f1"/>
<rect x="438.75" y="139.90" width="4.32" height="5.08" class="ap-f0"/>
<rect x="443.07" y="139.90" width="17.01" height="8.89" class="ap-f0"/>
<rect x="460.08" y="139.90" width="28.69" height="42.15" class="ap-f3"/>
<rect x="488.77" y="139.90" width="26.66" height="28.69" class="ap-f2"/>
<rect x="0.00" y="199.32" width="16.00" height="23.36" class="ap-f2"/>
<rect x="16.00" y="199.32" width="16.76" height="28.95" class="ap-f2"/>
<rect x="32.75" y="199.32" width="9.65" height="11.93" class="ap-f1"/>
<rect x="42.40" y="199.32" width="45.96" height="31.48" class="ap-f2"/>
<rect x="88.36" y="199.32" width="24.12" height="25.14" class="ap-f2"/>
<rect x="112.48" y="199.32" width="19.55" height="31.99" class="ap-f2"/>
<rect x="132.03" y="199.32" width="31.23" height="20.31" class="ap-f2"/>
<rect x="163.26" y="199.32" width="29.20" height="26.66" class="ap-f2"/>
<rect x="192.46" y="199.32" width="53.32" height="43.67" class="ap-f3"/>
<rect x="245.78" y="199.32" width="14.73" height="27.68" class="ap-f2"/>
<rect x="260.51" y="199.32" width="34.53" height="20.06" class="ap-f1"/>
<rect x="295.04" y="199.32" width="4.32" height="5.33" class="ap-f0"/>
<rect x="299.36" y="199.32" width="11.43" height="18.79" class="ap-f1"/>
<rect x="310.78" y="199.32" width="9.14" height="7.11" class="ap-f0"/>
<rect x="319.92" y="199.32" width="10.41" height="4.57" class="ap-f0"/>
<rect x="330.33" y="199.32" width="10.66" height="6.35" class="ap-f0"/>
<rect x="341.00" y="199.32" width="10.16" height="12.44" class="ap-f1"/>
<rect x="351.15" y="199.32" width="22.09" height="38.85" class="ap-f2"/>
<rect x="373.24" y="199.32" width="22.60" height="18.79" class="ap-f1"/>
<rect x="395.84" y="199.32" width="9.14" height="16.76" class="ap-f1"/>
<rect x="404.98" y="199.32" width="5.84" height="12.19" class="ap-f1"/>
<rect x="410.82" y="199.32" width="34.28" height="57.38" class="ap-f3"/>
<rect x="445.10" y="199.32" width="21.58" height="29.45" class="ap-f2"/>
<rect x="466.68" y="199.32" width="17.01" height="43.42" class="ap-f3"/>
<rect x="0.00" y="256.70" width="53.57" height="45.20" class="ap-f3"/>
<rect x="53.57" y="256.70" width="46.21" height="20.06" class="ap-f1"/>
<rect x="99.79" y="256.70" width="11.43" height="12.95" class="ap-f1"/>
<rect x="111.21" y="256.70" width="49.51" height="49.26" class="ap-f3"/>
<rect x="160.72" y="256.70" width="10.16" height="19.55" class="ap-f1"/>
<rect x="170.88" y="256.70" width="19.55" height="12.44" class="ap-f1"/>
<rect x="190.43" y="256.70" width="13.20" height="12.44" class="ap-f1"/>
<rect x="203.63" y="256.70" width="31.74" height="39.61" class="ap-f2"/>
<rect x="235.37" y="256.70" width="19.80" height="20.31" class="ap-f2"/>
<rect x="255.18" y="256.70" width="13.46" height="10.16" class="ap-f1"/>
<rect x="268.63" y="256.70" width="15.74" height="13.46" class="ap-f1"/>
<rect x="284.38" y="256.70" width="6.60" height="7.11" class="ap-f0"/>
<rect x="290.98" y="256.70" width="47.23" height="49.77" class="ap-f3"/>
<rect x="338.20" y="256.70" width="11.17" height="8.38" class="ap-f0"/>
<rect x="349.38" y="256.70" width="16.00" height="41.39" class="ap-f3"/>
<rect x="365.37" y="256.70" width="12.70" height="12.44" class="ap-f1"/>
<rect x="378.07" y="256.70" width="6.86" height="6.60" class="ap-f0"/>
<rect x="384.92" y="256.70" width="7.87" height="4.32" class="ap-f0"/>
<rect x="392.79" y="256.70" width="3.55" height="4.06" class="ap-f0"/>
<rect x="396.35" y="256.70" width="23.11" height="37.83" class="ap-f2"/>
<rect x="419.45" y="256.70" width="20.06" height="16.76" class="ap-f1"/>
<rect x="439.51" y="256.70" width="57.89" height="53.07" class="ap-f3"/>
<rect x="0.00" y="309.77" width="25.39" height="42.15" class="ap-f3"/>
<rect x="25.39" y="309.77" width="6.60" height="6.60" class="ap-f0"/>
<rect x="31.99" y="309.77" width="26.91" height="37.07" class="ap-f2"/>
<rect x="58.91" y="309.77" width="2.29" height="4.32" class="ap-f0"/>
<rect x="61.19" y="309.77" width="4.82" height="9.39" class="ap-f0"/>
<rect x="66.02" y="309.77" width="5.08" height="3.81" class="ap-f0"/>
<rect x="71.09" y="309.77" width="28.69" height="43.93" class="ap-f3"/>
<rect x="99.79" y="309.77" width="3.81" height="4.06" class="ap-f0"/>
<rect x="103.59" y="309.77" width="18.28" height="9.90" class="ap-f0"/>
<rect x="121.88" y="309.77" width="20.31" height="43.67" class="ap-f3"/>
<rect x="142.19" y="309.77" width="7.62" height="7.87" class="ap-f0"/>
<rect x="149.80" y="309.77" width="9.90" height="15.74" class="ap-f1"/>
<rect x="159.71" y="309.77" width="46.72" height="24.63" class="ap-f2"/>
<rect x="206.43" y="309.77" width="18.28" height="9.39" class="ap-f0"/>
<rect x="224.71" y="309.77" width="9.39" height="17.27" class="ap-f1"/>
<rect x="234.10" y="309.77" width="6.09" height="5.59" class="ap-f0"/>
<rect x="240.20" y="309.77" width="2.54" height="6.09" class="ap-f0"/>
<rect x="242.73" y="309.77" width="24.12" height="22.09" class="ap-f2"/>
<rect x="266.86" y="309.77" width="59.16" height="49.77" class="ap-f3"/>
<rect x="326.02" y="309.77" width="20.57" height="45.70" class="ap-f3"/>
<rect x="346.58" y="309.77" width="28.69" height="51.29" class="ap-f3"/>
<rect x="375.27" y="309.77" width="9.90" height="11.93" class="ap-f1"/>
<rect x="385.18" y="309.77" width="6.09" height="5.84" class="ap-f0"/>
<rect x="391.27" y="309.77" width="9.39" height="4.32" class="ap-f0"/>
<rect x="400.66" y="309.77" width="41.13" height="63.48" class="ap-f3"/>
<rect x="441.80" y="309.77" width="37.07" height="54.34" class="ap-f3"/>
<rect x="478.87" y="309.77" width="9.90" height="18.03" class="ap-f1"/>
<rect x="488.77" y="309.77" width="28.44" height="40.62" class="ap-f3"/>
<rect x="0.00" y="373.24" width="4.32" height="5.59" class="ap-f0"/>
<rect x="4.32" y="373.24" width="2.29" height="7.11" class="ap-f0"/>
<rect x="6.60" y="373.24" width="2.79" height="8.12" class="ap-f0"/>
<rect x="9.39" y="373.24" width="13.20" height="12.19" class="ap-f1"/>
<rect x="22.60" y="373.24" width="16.00" height="38.85" class="ap-f2"/>
<rect x="38.59" y="373.24" width="59.41" height="71.86" class="ap-f3"/>
<rect x="98.01" y="373.24" width="4.32" height="6.09" class="ap-f0"/>
<rect x="102.32" y="373.24" width="3.55" height="4.82" class="ap-f0"/>
<rect x="105.88" y="373.24" width="37.07" height="48.24" class="ap-f3"/>
<rect x="142.95" y="373.24" width="35.29" height="24.63" class="ap-f2"/>
<rect x="178.24" y="373.24" width="9.14" height="4.82" class="ap-f0"/>
<rect x="187.38" y="373.24" width="28.69" height="34.02" class="ap-f2"/>
<rect x="216.07" y="373.24" width="5.08" height="7.87" class="ap-f0"/>
<rect x="221.15" y="373.24" width="27.68" height="34.28" class="ap-f2"/>
<rect x="248.83" y="373.24" width="10.92" height="14.98" class="ap-f1"/>
<rect x="259.75" y="373.24" width="22.60" height="24.63" class="ap-f2"/>
<rect x="282.34" y="373.24" width="40.12" height="50.78" class="ap-f3"/>
<rect x="322.46" y="373.24" width="39.61" height="20.06" class="ap-f1"/>
<rect x="362.07" y="373.24" width="11.43" height="12.95" class="ap-f1"/>
<rect x="373.50" y="373.24" width="10.66" height="13.20" class="ap-f1"/>
<rect x="384.16" y="373.24" width="15.23" height="20.82" class="ap-f2"/>
<rect x="399.39" y="373.24" width="17.27" height="30.47" class="ap-f2"/>
<rect x="416.66" y="373.24" width="16.00" height="43.42" class="ap-f3"/>
<rect x="432.66" y="373.24" width="12.44" height="18.03" class="ap-f1"/>
<rect x="445.10" y="373.24" width="9.65" height="23.11" class="ap-f2"/>
<rect x="454.75" y="373.24" width="3.30" height="5.84" class="ap-f0"/>
<rect x="458.05" y="373.24" width="7.62" height="16.76" class="ap-f1"/>
<rect x="465.66" y="373.24" width="13.20" height="12.44" class="ap-f1"/>
<rect x="478.87" y="373.24" width="27.68" height="42.66" class="ap-f3"/>
<rect x="506.54" y="373.24" width="5.59" height="4.06" class="ap-f0"/>
 </svg></div>
<div style="padding:.5rem"><div class="ap-cap">Tallest-first &mdash; 229 / 229, shelves span 58&nbsp;%</div>
<svg class="ap-plate" viewBox="0 0 520 520" role="img" aria-label="The same 229 frames re-laid tallest first: all 229 placed, shelves spanning 58 percent of the ap-atlas">
  <rect x="0" y="0.00" width="520" height="81.76" class="ap-shelfband"/>
<rect x="0" y="81.76" width="520" height="51.54" class="ap-shelfband"/>
<rect x="0" y="133.30" width="520" height="45.96" class="ap-shelfband"/>
<rect x="0" y="179.26" width="520" height="41.64" class="ap-shelfband"/>
<rect x="0" y="220.90" width="520" height="28.69" class="ap-shelfband"/>
<rect x="0" y="249.59" width="520" height="20.31" class="ap-shelfband"/>
<rect x="0" y="269.90" width="520" height="15.74" class="ap-shelfband"/>
<rect x="0" y="285.64" width="520" height="11.17" class="ap-shelfband"/>
<rect x="0" y="296.82" width="520" height="4.06" class="ap-shelfband"/>
<rect x="0.00" y="0.00" width="49.51" height="81.76" class="ap-f3"/>
<rect x="49.51" y="0.00" width="52.56" height="81.25" class="ap-f3"/>
<rect x="102.07" y="0.00" width="42.91" height="78.96" class="ap-f3"/>
<rect x="144.98" y="0.00" width="59.41" height="71.86" class="ap-f3"/>
<rect x="204.39" y="0.00" width="38.09" height="70.59" class="ap-f3"/>
<rect x="242.48" y="0.00" width="41.13" height="63.48" class="ap-f3"/>
<rect x="283.61" y="0.00" width="39.86" height="59.41" class="ap-f3"/>
<rect x="323.48" y="0.00" width="41.13" height="58.14" class="ap-f3"/>
<rect x="364.61" y="0.00" width="34.28" height="57.38" class="ap-f3"/>
<rect x="398.89" y="0.00" width="37.07" height="54.34" class="ap-f3"/>
<rect x="435.96" y="0.00" width="57.89" height="53.07" class="ap-f3"/>
<rect x="0.00" y="81.76" width="38.59" height="51.54" class="ap-f3"/>
<rect x="38.59" y="81.76" width="36.31" height="51.29" class="ap-f3"/>
<rect x="74.90" y="81.76" width="28.69" height="51.29" class="ap-f3"/>
<rect x="103.59" y="81.76" width="38.59" height="51.29" class="ap-f3"/>
<rect x="142.19" y="81.76" width="40.12" height="50.78" class="ap-f3"/>
<rect x="182.30" y="81.76" width="47.23" height="49.77" class="ap-f3"/>
<rect x="229.53" y="81.76" width="59.16" height="49.77" class="ap-f3"/>
<rect x="288.69" y="81.76" width="22.09" height="49.51" class="ap-f3"/>
<rect x="310.78" y="81.76" width="49.51" height="49.26" class="ap-f3"/>
<rect x="360.29" y="81.76" width="17.27" height="48.50" class="ap-f3"/>
<rect x="377.56" y="81.76" width="37.07" height="48.24" class="ap-f3"/>
<rect x="414.63" y="81.76" width="47.99" height="47.99" class="ap-f3"/>
<rect x="462.62" y="81.76" width="39.36" height="46.46" class="ap-f3"/>
<rect x="0.00" y="133.30" width="35.80" height="45.96" class="ap-f3"/>
<rect x="35.80" y="133.30" width="18.79" height="45.70" class="ap-f3"/>
<rect x="54.59" y="133.30" width="20.31" height="45.70" class="ap-f3"/>
<rect x="74.90" y="133.30" width="20.57" height="45.70" class="ap-f3"/>
<rect x="95.47" y="133.30" width="53.57" height="45.20" class="ap-f3"/>
<rect x="149.04" y="133.30" width="53.32" height="44.69" class="ap-f3"/>
<rect x="202.36" y="133.30" width="28.69" height="43.93" class="ap-f3"/>
<rect x="231.05" y="133.30" width="53.32" height="43.67" class="ap-f3"/>
<rect x="284.38" y="133.30" width="20.31" height="43.67" class="ap-f3"/>
<rect x="304.69" y="133.30" width="16.00" height="43.42" class="ap-f3"/>
<rect x="320.68" y="133.30" width="17.01" height="43.42" class="ap-f3"/>
<rect x="337.70" y="133.30" width="16.00" height="43.42" class="ap-f3"/>
<rect x="353.69" y="133.30" width="28.18" height="42.66" class="ap-f3"/>
<rect x="381.88" y="133.30" width="27.68" height="42.66" class="ap-f3"/>
<rect x="409.55" y="133.30" width="27.68" height="42.66" class="ap-f3"/>
<rect x="437.23" y="133.30" width="28.69" height="42.15" class="ap-f3"/>
<rect x="465.92" y="133.30" width="25.39" height="42.15" class="ap-f3"/>
<rect x="0.00" y="179.26" width="31.48" height="41.64" class="ap-f3"/>
<rect x="31.48" y="179.26" width="16.00" height="41.39" class="ap-f3"/>
<rect x="47.48" y="179.26" width="28.44" height="40.62" class="ap-f3"/>
<rect x="75.92" y="179.26" width="27.68" height="40.37" class="ap-f2"/>
<rect x="103.59" y="179.26" width="31.74" height="39.61" class="ap-f2"/>
<rect x="135.33" y="179.26" width="22.09" height="38.85" class="ap-f2"/>
<rect x="157.42" y="179.26" width="16.00" height="38.85" class="ap-f2"/>
<rect x="173.42" y="179.26" width="23.11" height="37.83" class="ap-f2"/>
<rect x="196.52" y="179.26" width="40.88" height="37.07" class="ap-f2"/>
<rect x="237.40" y="179.26" width="47.73" height="37.07" class="ap-f2"/>
<rect x="285.14" y="179.26" width="26.91" height="37.07" class="ap-f2"/>
<rect x="312.05" y="179.26" width="27.68" height="34.28" class="ap-f2"/>
<rect x="339.73" y="179.26" width="28.69" height="34.02" class="ap-f2"/>
<rect x="368.42" y="179.26" width="19.55" height="31.99" class="ap-f2"/>
<rect x="387.97" y="179.26" width="45.96" height="31.48" class="ap-f2"/>
<rect x="433.93" y="179.26" width="21.84" height="30.98" class="ap-f2"/>
<rect x="455.76" y="179.26" width="17.27" height="30.47" class="ap-f2"/>
<rect x="473.03" y="179.26" width="21.58" height="29.45" class="ap-f2"/>
<rect x="494.61" y="179.26" width="16.76" height="28.95" class="ap-f2"/>
<rect x="0.00" y="220.90" width="26.66" height="28.69" class="ap-f2"/>
<rect x="26.66" y="220.90" width="14.73" height="27.68" class="ap-f2"/>
<rect x="41.39" y="220.90" width="17.01" height="26.91" class="ap-f2"/>
<rect x="58.40" y="220.90" width="29.20" height="26.66" class="ap-f2"/>
<rect x="87.60" y="220.90" width="12.95" height="26.41" class="ap-f2"/>
<rect x="100.55" y="220.90" width="29.45" height="26.15" class="ap-f2"/>
<rect x="130.00" y="220.90" width="11.17" height="26.15" class="ap-f2"/>
<rect x="141.17" y="220.90" width="24.12" height="25.14" class="ap-f2"/>
<rect x="165.29" y="220.90" width="32.25" height="25.14" class="ap-f2"/>
<rect x="197.54" y="220.90" width="46.72" height="24.63" class="ap-f2"/>
<rect x="244.26" y="220.90" width="46.72" height="24.63" class="ap-f2"/>
<rect x="290.98" y="220.90" width="35.29" height="24.63" class="ap-f2"/>
<rect x="326.27" y="220.90" width="22.60" height="24.63" class="ap-f2"/>
<rect x="348.87" y="220.90" width="46.72" height="24.63" class="ap-f2"/>
<rect x="395.59" y="220.90" width="16.00" height="23.36" class="ap-f2"/>
<rect x="411.58" y="220.90" width="9.65" height="23.11" class="ap-f2"/>
<rect x="421.23" y="220.90" width="19.80" height="22.34" class="ap-f2"/>
<rect x="441.04" y="220.90" width="18.79" height="22.09" class="ap-f2"/>
<rect x="459.82" y="220.90" width="24.12" height="22.09" class="ap-f2"/>
<rect x="483.95" y="220.90" width="15.23" height="20.82" class="ap-f2"/>
<rect x="0.00" y="249.59" width="25.39" height="20.31" class="ap-f2"/>
<rect x="25.39" y="249.59" width="31.23" height="20.31" class="ap-f2"/>
<rect x="56.62" y="249.59" width="19.80" height="20.31" class="ap-f2"/>
<rect x="76.43" y="249.59" width="46.21" height="20.06" class="ap-f1"/>
<rect x="122.64" y="249.59" width="34.53" height="20.06" class="ap-f1"/>
<rect x="157.17" y="249.59" width="46.21" height="20.06" class="ap-f1"/>
<rect x="203.38" y="249.59" width="39.61" height="20.06" class="ap-f1"/>
<rect x="242.99" y="249.59" width="23.11" height="19.80" class="ap-f1"/>
<rect x="266.09" y="249.59" width="19.30" height="19.80" class="ap-f1"/>
<rect x="285.39" y="249.59" width="10.16" height="19.55" class="ap-f1"/>
<rect x="295.55" y="249.59" width="20.31" height="19.04" class="ap-f1"/>
<rect x="315.86" y="249.59" width="8.63" height="19.04" class="ap-f1"/>
<rect x="324.49" y="249.59" width="11.43" height="18.79" class="ap-f1"/>
<rect x="335.92" y="249.59" width="22.60" height="18.79" class="ap-f1"/>
<rect x="358.52" y="249.59" width="12.70" height="18.54" class="ap-f1"/>
<rect x="371.21" y="249.59" width="13.71" height="18.54" class="ap-f1"/>
<rect x="384.92" y="249.59" width="16.25" height="18.28" class="ap-f1"/>
<rect x="401.17" y="249.59" width="9.39" height="18.03" class="ap-f1"/>
<rect x="410.57" y="249.59" width="9.90" height="18.03" class="ap-f1"/>
<rect x="420.47" y="249.59" width="12.44" height="18.03" class="ap-f1"/>
<rect x="432.91" y="249.59" width="12.44" height="17.52" class="ap-f1"/>
<rect x="445.35" y="249.59" width="9.39" height="17.27" class="ap-f1"/>
<rect x="454.75" y="249.59" width="9.14" height="16.76" class="ap-f1"/>
<rect x="463.89" y="249.59" width="20.06" height="16.76" class="ap-f1"/>
<rect x="483.95" y="249.59" width="7.62" height="16.76" class="ap-f1"/>
<rect x="491.56" y="249.59" width="14.22" height="16.76" class="ap-f1"/>
<rect x="505.78" y="249.59" width="7.62" height="16.76" class="ap-f1"/>
<rect x="0.00" y="269.90" width="9.90" height="15.74" class="ap-f1"/>
<rect x="9.90" y="269.90" width="14.47" height="15.23" class="ap-f1"/>
<rect x="24.38" y="269.90" width="13.71" height="15.23" class="ap-f1"/>
<rect x="38.09" y="269.90" width="10.41" height="14.98" class="ap-f1"/>
<rect x="48.50" y="269.90" width="10.92" height="14.98" class="ap-f1"/>
<rect x="59.41" y="269.90" width="9.90" height="14.47" class="ap-f1"/>
<rect x="69.32" y="269.90" width="5.84" height="14.47" class="ap-f1"/>
<rect x="75.16" y="269.90" width="8.89" height="14.22" class="ap-f1"/>
<rect x="84.04" y="269.90" width="12.70" height="13.96" class="ap-f1"/>
<rect x="96.74" y="269.90" width="8.38" height="13.96" class="ap-f1"/>
<rect x="105.12" y="269.90" width="9.90" height="13.96" class="ap-f1"/>
<rect x="115.02" y="269.90" width="12.95" height="13.46" class="ap-f1"/>
<rect x="127.97" y="269.90" width="7.36" height="13.46" class="ap-f1"/>
<rect x="135.33" y="269.90" width="15.74" height="13.46" class="ap-f1"/>
<rect x="151.07" y="269.90" width="18.28" height="13.46" class="ap-f1"/>
<rect x="169.36" y="269.90" width="9.90" height="13.46" class="ap-f1"/>
<rect x="179.26" y="269.90" width="13.20" height="13.46" class="ap-f1"/>
<rect x="192.46" y="269.90" width="10.66" height="13.20" class="ap-f1"/>
<rect x="203.12" y="269.90" width="13.46" height="13.20" class="ap-f1"/>
<rect x="216.58" y="269.90" width="10.66" height="12.95" class="ap-f1"/>
<rect x="227.25" y="269.90" width="11.43" height="12.95" class="ap-f1"/>
<rect x="238.67" y="269.90" width="11.43" height="12.95" class="ap-f1"/>
<rect x="250.10" y="269.90" width="18.79" height="12.70" class="ap-f1"/>
<rect x="268.89" y="269.90" width="11.93" height="12.44" class="ap-f1"/>
<rect x="280.82" y="269.90" width="13.20" height="12.44" class="ap-f1"/>
<rect x="294.02" y="269.90" width="10.16" height="12.44" class="ap-f1"/>
<rect x="304.18" y="269.90" width="19.55" height="12.44" class="ap-f1"/>
<rect x="323.73" y="269.90" width="13.20" height="12.44" class="ap-f1"/>
<rect x="336.93" y="269.90" width="12.70" height="12.44" class="ap-f1"/>
<rect x="349.63" y="269.90" width="13.20" height="12.44" class="ap-f1"/>
<rect x="362.83" y="269.90" width="10.92" height="12.44" class="ap-f1"/>
<rect x="373.75" y="269.90" width="12.95" height="12.19" class="ap-f1"/>
<rect x="386.70" y="269.90" width="13.46" height="12.19" class="ap-f1"/>
<rect x="400.16" y="269.90" width="5.84" height="12.19" class="ap-f1"/>
<rect x="406.00" y="269.90" width="5.84" height="12.19" class="ap-f1"/>
<rect x="411.84" y="269.90" width="13.20" height="12.19" class="ap-f1"/>
<rect x="425.04" y="269.90" width="8.12" height="11.93" class="ap-f1"/>
<rect x="433.16" y="269.90" width="9.65" height="11.93" class="ap-f1"/>
<rect x="442.81" y="269.90" width="9.90" height="11.93" class="ap-f1"/>
<rect x="452.71" y="269.90" width="12.44" height="11.68" class="ap-f1"/>
<rect x="465.16" y="269.90" width="13.20" height="11.68" class="ap-f1"/>
<rect x="478.36" y="269.90" width="12.95" height="11.43" class="ap-f1"/>
<rect x="491.31" y="269.90" width="19.55" height="11.43" class="ap-f1"/>
<rect x="510.86" y="269.90" width="5.59" height="11.43" class="ap-f1"/>
<rect x="0.00" y="285.64" width="8.63" height="11.17" class="ap-f1"/>
<rect x="8.63" y="285.64" width="12.44" height="10.92" class="ap-f1"/>
<rect x="21.07" y="285.64" width="12.19" height="10.92" class="ap-f1"/>
<rect x="33.26" y="285.64" width="16.50" height="10.41" class="ap-f1"/>
<rect x="49.77" y="285.64" width="13.46" height="10.16" class="ap-f1"/>
<rect x="63.22" y="285.64" width="18.28" height="9.90" class="ap-f0"/>
<rect x="81.50" y="285.64" width="4.57" height="9.65" class="ap-f0"/>
<rect x="86.07" y="285.64" width="4.82" height="9.39" class="ap-f0"/>
<rect x="90.90" y="285.64" width="4.82" height="9.39" class="ap-f0"/>
<rect x="95.72" y="285.64" width="18.28" height="9.39" class="ap-f0"/>
<rect x="114.00" y="285.64" width="17.77" height="9.39" class="ap-f0"/>
<rect x="131.78" y="285.64" width="5.08" height="9.14" class="ap-f0"/>
<rect x="136.86" y="285.64" width="14.22" height="9.14" class="ap-f0"/>
<rect x="151.07" y="285.64" width="17.01" height="8.89" class="ap-f0"/>
<rect x="168.09" y="285.64" width="11.68" height="8.63" class="ap-f0"/>
<rect x="179.77" y="285.64" width="11.68" height="8.63" class="ap-f0"/>
<rect x="191.45" y="285.64" width="11.17" height="8.38" class="ap-f0"/>
<rect x="202.62" y="285.64" width="10.41" height="8.38" class="ap-f0"/>
<rect x="213.03" y="285.64" width="11.17" height="8.12" class="ap-f0"/>
<rect x="224.20" y="285.64" width="5.84" height="8.12" class="ap-f0"/>
<rect x="230.04" y="285.64" width="2.79" height="8.12" class="ap-f0"/>
<rect x="232.83" y="285.64" width="7.62" height="7.87" class="ap-f0"/>
<rect x="240.45" y="285.64" width="5.08" height="7.87" class="ap-f0"/>
<rect x="245.53" y="285.64" width="4.82" height="7.62" class="ap-f0"/>
<rect x="250.35" y="285.64" width="16.25" height="7.36" class="ap-f0"/>
<rect x="266.60" y="285.64" width="2.79" height="7.36" class="ap-f0"/>
<rect x="269.39" y="285.64" width="5.08" height="7.36" class="ap-f0"/>
<rect x="274.47" y="285.64" width="6.35" height="7.11" class="ap-f0"/>
<rect x="280.82" y="285.64" width="4.32" height="7.11" class="ap-f0"/>
<rect x="285.14" y="285.64" width="6.60" height="7.11" class="ap-f0"/>
<rect x="291.74" y="285.64" width="9.14" height="7.11" class="ap-f0"/>
<rect x="300.88" y="285.64" width="6.60" height="7.11" class="ap-f0"/>
<rect x="307.48" y="285.64" width="2.29" height="7.11" class="ap-f0"/>
<rect x="309.77" y="285.64" width="6.86" height="6.86" class="ap-f0"/>
<rect x="316.62" y="285.64" width="6.86" height="6.60" class="ap-f0"/>
<rect x="323.48" y="285.64" width="6.60" height="6.60" class="ap-f0"/>
<rect x="330.08" y="285.64" width="10.66" height="6.35" class="ap-f0"/>
<rect x="340.74" y="285.64" width="2.54" height="6.09" class="ap-f0"/>
<rect x="343.28" y="285.64" width="4.32" height="6.09" class="ap-f0"/>
<rect x="347.60" y="285.64" width="6.09" height="5.84" class="ap-f0"/>
<rect x="353.69" y="285.64" width="3.30" height="5.84" class="ap-f0"/>
<rect x="356.99" y="285.64" width="3.81" height="5.59" class="ap-f0"/>
<rect x="360.80" y="285.64" width="6.09" height="5.59" class="ap-f0"/>
<rect x="366.89" y="285.64" width="4.32" height="5.59" class="ap-f0"/>
<rect x="371.21" y="285.64" width="4.06" height="5.59" class="ap-f0"/>
<rect x="375.27" y="285.64" width="4.82" height="5.59" class="ap-f0"/>
<rect x="380.10" y="285.64" width="5.84" height="5.59" class="ap-f0"/>
<rect x="385.94" y="285.64" width="4.06" height="5.59" class="ap-f0"/>
<rect x="390.00" y="285.64" width="4.57" height="5.33" class="ap-f0"/>
<rect x="394.57" y="285.64" width="7.87" height="5.33" class="ap-f0"/>
<rect x="402.44" y="285.64" width="4.32" height="5.33" class="ap-f0"/>
<rect x="406.76" y="285.64" width="4.32" height="5.08" class="ap-f0"/>
<rect x="411.07" y="285.64" width="3.55" height="4.82" class="ap-f0"/>
<rect x="414.63" y="285.64" width="9.14" height="4.82" class="ap-f0"/>
<rect x="423.77" y="285.64" width="10.16" height="4.57" class="ap-f0"/>
<rect x="433.93" y="285.64" width="10.41" height="4.57" class="ap-f0"/>
<rect x="444.34" y="285.64" width="5.33" height="4.57" class="ap-f0"/>
<rect x="449.67" y="285.64" width="6.86" height="4.32" class="ap-f0"/>
<rect x="456.52" y="285.64" width="7.62" height="4.32" class="ap-f0"/>
<rect x="464.14" y="285.64" width="7.87" height="4.32" class="ap-f0"/>
<rect x="472.01" y="285.64" width="2.29" height="4.32" class="ap-f0"/>
<rect x="474.30" y="285.64" width="9.39" height="4.32" class="ap-f0"/>
<rect x="483.69" y="285.64" width="7.62" height="4.06" class="ap-f0"/>
<rect x="491.31" y="285.64" width="5.08" height="4.06" class="ap-f0"/>
<rect x="496.39" y="285.64" width="5.59" height="4.06" class="ap-f0"/>
<rect x="501.97" y="285.64" width="3.81" height="4.06" class="ap-f0"/>
<rect x="505.78" y="285.64" width="7.11" height="4.06" class="ap-f0"/>
<rect x="512.89" y="285.64" width="5.59" height="4.06" class="ap-f0"/>
<rect x="0.00" y="296.82" width="4.57" height="4.06" class="ap-f0"/>
<rect x="4.57" y="296.82" width="3.55" height="4.06" class="ap-f0"/>
<rect x="8.12" y="296.82" width="3.81" height="4.06" class="ap-f0"/>
<rect x="11.93" y="296.82" width="5.59" height="4.06" class="ap-f0"/>
<rect x="17.52" y="296.82" width="5.84" height="3.81" class="ap-f0"/>
<rect x="23.36" y="296.82" width="4.57" height="3.81" class="ap-f0"/>
<rect x="27.93" y="296.82" width="5.08" height="3.81" class="ap-f0"/>
<rect x="33.01" y="296.82" width="5.08" height="3.55" class="ap-f0"/>
<rect x="38.09" y="296.82" width="2.79" height="3.55" class="ap-f0"/>
<rect x="40.88" y="296.82" width="3.05" height="3.05" class="ap-f0"/>
 </svg></div>
</div>

**Reading the plates:** left, arrival order — 197 of 229 placed, 41 % of the page is art, the
shelves consume 86 %, then the latch. Right, tallest-first — all 229 placed, 48 % is art, the
shelves consume 58 %, and the bottom 42 % of the page is never touched.

A real 2D packer was measured before being rejected: **skyline bottom-left**, fed the same sorted
order, gets the span to 53 % against tallest-first's 58 %. That is four times the code for room
the page does not need.

## 9. Measured — same map, same resolution, same zoom, only the DLL differs

Town & Country at 3840×2160, zoom 0.25, `scenarios/static-terrain.json` — two immobile towers in
opposite corners and the camera on empty ground, so nothing in the frame moves except what the
change moves.

| from the pass's own log line | before | after |
|---|---|---|
| frames in the atlas `atlas=` | 204 | **234** |
| sprites dropped per frame `atlas-fail=` | 455 | **0** — no `DROPPED` line at all |
| whole-atlas rebuilds, one session | 37,140 | **0** |
| repacks, ever | — | **1** |
| atlas generation reached | 37,143 | **4** |
| anchors / flat / tall / body / shadow | identical — the gather did not change, only what the atlas does with it | |

```
feat: atlas repacked — 205 frames re-laid tallest-first, 50% of the 2048 square, generation 4 (repack 1)
```

That is the whole of the atlas's work for the session, on one line. Confirmed visually at 4K on
the reference setup's real GL, not only in the counters.

## 10. The back branch — the wall

A repack is futile by construction once it cannot beat the last one: the same entries, sorted the
same way, produce the same layout. So `repackWall` latches when a repack places fewer frames than
it was handed, or no more than the previous one did — and past it the atlas **holds what it has**
rather than dropping a working set for a rebuild that would place fewer. That is the opposite of
the old behaviour, and deliberately so: a stable 229 beats a thrashing 197.

It is also the honest place to stop. Once the page is packed as tightly as a sort can pack it and
frames are still missing, no amount of cleverness inside one 2048 square adds room. Only a second
page does, and the log says exactly that rather than failing quietly.

<div class="tablewrap ap">
<svg class="ap-dia" viewBox="0 0 700 300" role="img" aria-label="The full-atlas branch: repack, and if the repack cannot beat its predecessor, latch the wall and hold the layout instead of rebuilding">
   <rect x="20" y="26" width="126" height="42" rx="3" fill="var(--lead-bg)" stroke="var(--accent-ink)" stroke-width="1.2"/>
   <text class="ap-sigtx" x="83" y="44" text-anchor="middle" font-size="11">atlas latches</text>
   <text class="ap-sigtx" x="83" y="58" text-anchor="middle" font-size="11">full</text>

   <line class="ap-thin" x1="146" y1="47" x2="196" y2="47" marker-end="url(#ar210)"/>
   <rect x="200" y="26" width="150" height="42" rx="3" fill="none" stroke="currentColor" stroke-width="1.2"/>
   <text x="275" y="44" text-anchor="middle" font-size="11">re-lay tallest-first</text>
   <text class="ap-lab" x="275" y="58" text-anchor="middle">reserve every rect</text>

   <line class="ap-thin" x1="350" y1="47" x2="400" y2="47" marker-end="url(#ar210)"/>
   <path d="M475,20 L545,47 L475,74 L405,47 z" fill="none" stroke="currentColor" stroke-width="1.2"/>
   <text x="475" y="44" text-anchor="middle" font-size="10">placed more</text>
   <text x="475" y="56" text-anchor="middle" font-size="10">than last time?</text>

   <line class="ap-thin" x1="545" y1="47" x2="600" y2="47" marker-end="url(#ar210)"/>
   <text class="ap-lab" x="572" y="38" text-anchor="middle">yes</text>
   <text class="ap-sigtx" x="604" y="44" font-size="11">clear full,</text>
   <text class="ap-sigtx" x="604" y="58" font-size="11">carry on</text>

   <line class="ap-thin" x1="475" y1="74" x2="475" y2="126" marker-end="url(#ar210)"/>
   <text class="ap-lab" x="483" y="102">no</text>
   <rect class="ap-bad" x="330" y="130" width="290" height="62" rx="3"/>
   <text class="ap-badtx" x="344" y="150" font-size="11">WALL &mdash; hold this layout</text>
   <text class="ap-badtx" x="344" y="166" font-size="10">stop repacking; keep the frames that fit;</text>
   <text class="ap-badtx" x="344" y="180" font-size="10">never rebuild from nothing again</text>

   <line class="ap-thin" x1="330" y1="161" x2="284" y2="161" marker-end="url(#ar210)"/>
   <rect x="20" y="130" width="260" height="62" rx="3" fill="none" stroke="currentColor" stroke-width="1.2" stroke-dasharray="4 3"/>
   <text class="ap-lab" x="34" y="150">the log line, and the only thing</text>
   <text class="ap-lab" x="34" y="164">that adds room past it:</text>
   <text class="ap-sigtx" x="34" y="182" font-size="11">&ldquo;only a second page adds room&rdquo;</text>

   <text class="ap-lab" x="20" y="232">Not reached by anything measured. Of the four 2048 atlases only the feature one has ever</text>
   <text class="ap-lab" x="20" y="248">filled &mdash; fx 168 entries and no resets, unit none, gui two re-arms &mdash; its high-water mark is 231</text>
   <text class="ap-lab" x="20" y="264">entries against a 4096-entry table, and after the repack 42&nbsp;% of the page is still untouched.</text>
   <text class="ap-lab" x="20" y="280">A map would need roughly four times Town &amp; Country&rsquo;s feature variety to be tight.</text>
   <defs><marker id="ar210" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6"
     orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  </svg>
</div>

**Reading the diagram:** the **yes** path is the one every measurement takes, once, and then never
again. The **no** path exists so that the day a map outgrows one page, the renderer says so in a
log line instead of silently dropping a different 3.7 % of its sprites every frame — which is
precisely what it used to do.

Nothing measured reaches it. Of the four 2048-square atlases only the feature one has ever filled
— `fx` 168 entries and no resets, `unit` none, `gui` two re-arms — its high-water mark is 231
entries against a 4096-entry table, and after the repack **42 % of the page is still untouched**.
A map would need roughly four times Town & Country's feature variety to be tight.

## 11. If that day comes — the three ways to add a page

This is the work the wall defers, priced. None of it is inside `tagpu_gaf.c`: a sprite's UV is
`(u, v)` into *one bound texture*, so a second page changes how every consumer addresses the
atlas, not just how the atlas fills.

| approach | what it changes | the catch |
|---|---|---|
| **Texture array** *(the usual answer)* | `sampler2D` → `sampler2DArray` in every sprite shader, and a layer index added to every vertex format: `tagpu_feat.c`'s body and shadow buckets, `tagpu_fx.c`, and `tagpu_posebake.c`, which *bakes* UVs into a vertex stream keyed on `atlasGen` | `glTexStorage3D` fixes the layer count at creation, so "grow" means recreate and re-upload every page — the repack again, with more moving parts |
| **A draw per page** *(cheapest to write)* | Nothing in the shaders. The gather sorts quads by page and issues one draw per page instead of one per bucket | The page becomes a third sort key on top of shadow/body, and the single batched draw this pass was built around becomes N |
| **Bindless** | — | Not in GL 3.3 core, which is the floor this renderer targets |

Every approach pays the same memory bill per page, which is the part that is easy to miss:
**4.2 MB of `GL_R8` plus 16.8 MB of RGBA8** for that page's Classic++ restored twin, and a second
restorer job competing in the same sliced budget as the terrain, the units and the UI.

**The trigger is a log line, not a judgement call.** Build it when a real map prints `WALL`. Until
then the branch is four lines of state and a message, and the page it would add would be 21 MB of
memory holding nothing.

## 12. Residual — what this does not settle

**Never evicting has a cost, and it is a stale entry rather than a stale pixel.** The repack pins
every entry it holds, including one whose GAF frame the engine has since freed. The lookup key is
`(frame, pix, w, h)` — a value test, not a lifetime guarantee — so an address re-allocated with
the same pixel pointer and the same size would draw the old art. That hazard predates this
change, but the per-frame reset used to scrub it by accident and now nothing does: **nothing tells
the feature atlas when the map changes.** It is why `repack` is opt-in and set only on the atlas
whose contract is "fills once per map and stays"; the effects atlas, which frees and re-allocates
sequences constantly, leaves it clear and still resets.

**The working set is somewhat above 229.** Seven of the map's 123 feature types resolved to no
definition in the offline scan, and animating types contribute more than one frame each — the live
atlas settles at 234. The 48 % figure has slack, not unlimited headroom, which is exactly why the
wall exists rather than an assumption that one page is always enough.

**One map, one screen.** Every live number here is Town & Country at 3840×2160. A map whose
feature variety is much higher, or a 200-versus-200 battle whose ground fills with scar and smudge
defs, is the case that would find the wall — and neither has been run.

---

*Frame sizes read from the shipped archives — 123 feature types on Town & Country, 229 body and
shadow frames, cells at one texel of border. Packers are line-for-line models of `gaf_insert` and
`atlas_repack` in `tagpu_gaf.c`; live counters from `feat:` and `native:` at 3840×2160, zoom 0.25.
The repack, the reserve and the wall are implemented and measured. The second page is not.*
