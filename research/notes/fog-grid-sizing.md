# Sizing the wide fog grid

The wide fog grid ([terrain & depth](terrain-depth.html) §8) is the lattice `tagpu_fogwide.c`
builds so that fog covers a zoomed-out view instead of stopping at the edge of the 1× viewport.
This page is about **how big it is**, which is currently answered by two constants that disagree
with each other and with the screen.

Everything below is computed from the code's own arithmetic — `fogw_window` in
`tagpu_fogwide.c` and the `cols > 512 || rows > 512` test in `tagpu_fog_at`
(`tagpu_fx.c`) — evaluated over every eye residue, not estimated.

<div class="tablewrap fg">
<style>
.fg{color:var(--ink-2)}
.fg svg{display:block;width:100%;height:auto}
.fg svg text{font-family:"IBM Plex Sans",system-ui,sans-serif;fill:var(--ink);font-size:12px}
.fg-cap{font-family:"IBM Plex Mono",monospace;font-size:11px;color:var(--ink-3);
        letter-spacing:.03em;margin:0 0 .45rem}
.fg-box{fill:var(--code-bg);stroke:var(--edge);stroke-width:1.4}
.fg-dead{fill:var(--panel-2)}
.fg-ghost{fill:none;stroke:var(--edge);stroke-width:1;stroke-dasharray:4 4}
.fg-ok{fill:var(--ok-bg);stroke:var(--ok);stroke-width:1.2}
.fg-bad{fill:var(--warn-bg);stroke:var(--warn);stroke-width:1.3}
.fg-badtx{fill:var(--warn)}
.fg-oktx{fill:var(--ok)}
.fg-sig{stroke:var(--accent-ink);fill:none;stroke-width:1.5}
.fg-sigtx{fill:var(--accent-ink)}
.fg-lab{font-size:10.5px;fill:var(--ink-3);letter-spacing:.04em}
.fg-thin{stroke:var(--ink-2);stroke-width:1;fill:none}
.fg-grid{stroke:var(--edge);stroke-width:.6;fill:none}
.fg-use{fill:#8a9a63}
.fg-hold{fill:#6d8098}
.fg-pub{fill:#b8975f}
</style>

<p class="fg-cap">the two bounds, on one axis of screen width</p>
<svg class="fg-dia" viewBox="0 0 700 190" role="img" aria-label="A number line of screen widths from 1024 to 7680 pixels. The CPU-side gate refuses grids wider than 512 cells, which happens past 4064 pixels of screen width; the producer's own cap of 1024 cells does not bite until 8160. Between those two figures the producer builds a grid the consumer refuses.">
  <line class="fg-thin" x1="60" y1="120" x2="660" y2="120"/>
  <rect class="fg-ok"  x="60"  y="96" width="265" height="24"/>
  <rect class="fg-bad" x="325" y="96" width="315" height="24"/>
  <rect class="fg-ghost" x="640" y="96" width="20" height="24"/>

  <text class="fg-oktx"  x="192" y="112" text-anchor="middle" font-size="11">producer and consumer agree</text>
  <text class="fg-badtx" x="482" y="112" text-anchor="middle" font-size="11">producer builds it — consumer refuses it</text>

  <line class="fg-sig" x1="325" y1="60" x2="325" y2="96"/>
  <text class="fg-sigtx" x="325" y="52" text-anchor="middle" font-size="11">4064 px wide</text>
  <text class="fg-lab"   x="325" y="38" text-anchor="middle">cols &gt; 512 — tagpu_fog_at refuses</text>

  <line class="fg-ghost" x1="640" y1="60" x2="640" y2="96"/>
  <text class="fg-lab" x="640" y="52" text-anchor="middle">8160 px</text>
  <text class="fg-lab" x="640" y="38" text-anchor="middle">cols &gt; 1024</text>

  <g class="fg-lab" text-anchor="middle">
    <line class="fg-thin" x1="60"  y1="120" x2="60"  y2="127"/><text x="60"  y="140">1024</text>
    <line class="fg-thin" x1="138" y1="120" x2="138" y2="127"/><text x="138" y="140">1920</text>
    <line class="fg-thin" x1="194" y1="120" x2="194" y2="127"/><text x="194" y="140">2560</text>
    <line class="fg-thin" x1="271" y1="120" x2="271" y2="127"/><text x="271" y="140">3440</text>
    <line class="fg-thin" x1="305" y1="120" x2="305" y2="127"/><text x="305" y="152">3840</text>
    <line class="fg-thin" x1="417" y1="120" x2="417" y2="127"/><text x="417" y="140">5120</text>
    <line class="fg-thin" x1="640" y1="120" x2="640" y2="127"/><text x="640" y="140">7680</text>
  </g>
  <text class="fg-lab" x="60" y="172">screen width, px</text>
  <text class="fg-lab" x="305" y="166" text-anchor="middle">4K sits 27 cells inside the bound</text>
</svg>
</div>

**The gap is real, not theoretical arithmetic.** `FOGW_MAXDIM` lets `tagpu_fogwide` build a grid
up to 1024 cells a side; `tagpu_fog_at` refuses one over 512. Between a screen 4064 px wide and
one 8160 px wide the producer builds a grid the CPU-side consumer will not read. A 4K screen
(485 cells) clears the bound with 27 cells to spare. A 5K screen (645) does not.

## 1. What "the window" is, and why it is not the viewport

The engine's own fog grid spans the 1× viewport and about two cells more — it is allocated once
per map load from the viewport size and can never be made bigger ([engine
map](exe-reverse-engineering.html) §"The screen fog grid"). Ours spans **the widest view the zoom
levers can reach**, because the level the game thread can see is the one the render thread
published on the previous frame, so a window cut to the current level would be a frame behind
every outward ease.

<div class="tablewrap fg">
<p class="fg-cap">1920×1080: the engine's grid, our window, and the rect actually drawn at 0.25×</p>
<svg class="fg-dia" viewBox="0 0 700 330" role="img" aria-label="Three nested rectangles drawn to scale. The outer one is the wide window, 245 by 148 cells. Inside it is the world rect drawn at quarter zoom. Inside that, much smaller, is the engine's own fog grid at 58 by 34 cells, which is the 1x viewport plus two cells.">
  <rect class="fg-box" x="60" y="30" width="490" height="296"/>
  <text class="fg-lab" x="60" y="22">wide window — 245 × 148 cells (7840 × 4736 world px)</text>

  <rect class="fg-ghost" x="92" y="46" width="426" height="264"/>
  <text class="fg-lab" x="100" y="60">drawn at 0.25× — 7168 × 4064 px</text>

  <rect class="fg-use" x="248" y="150" width="116" height="66" opacity="0.85"/>
  <rect class="fg-thin" x="248" y="150" width="116" height="66" fill="none"/>
  <text class="fg-lab" x="306" y="244" text-anchor="middle">engine's grid — 58 × 34 cells</text>
  <line class="fg-thin" x1="306" y1="218" x2="306" y2="234"/>

  <text x="306" y="188" text-anchor="middle" font-size="11" fill="var(--ink)">1× viewport</text>

  <line class="fg-sig" x1="60" y1="316" x2="92" y2="316"/>
  <text class="fg-sigtx" x="76" y="308" text-anchor="middle" font-size="10">margin</text>
  <text class="fg-lab"   x="76" y="296" text-anchor="middle">256 px</text>

  <text class="fg-sigtx" x="560" y="118" font-size="11">cols ≈ vw/8 + 21</text>
  <text class="fg-sigtx" x="560" y="134" font-size="11">rows ≈ vh/8 + 21</text>
  <text class="fg-lab" x="560" y="158">vw = screenW − 128</text>
  <text class="fg-lab" x="560" y="174">vh = screenH − 64</text>
  <text class="fg-lab" x="560" y="200">at the zoom floor,</text>
  <text class="fg-lab" x="560" y="216">plus a 256 px margin</text>
  <text class="fg-lab" x="560" y="232">and two spare cells</text>
</svg>
</div>

`vw/0.25` is where the factor of four comes from, and `/32` turns world pixels into cells: a cell
is 32 world px. The two spare cells are not slack — the builder fills entry `gx` from map cells
`col0+gx` **and** `col0+gx+1`, so the last column of any grid is short its right-hand corners and
must be kept outside anything the view can show ([terrain & depth](terrain-depth.html) §8a).

**So the window is a pure function of the screen.** It does not depend on the map, the zoom in
force, or anything the player does — only on the viewport and the zoom floor. That is what makes
sizing it dynamically possible at all.

## 2. What tripping the 512 bound actually does

`tagpu_fog_at` is the **CPU-side** fog gate. It is not what paints fog — the shaders do that from
the uploaded texture, and they are unaffected by this bound. It is what decides whether a thing is
*drawn at all*:

| caller | what it gates |
| --- | --- |
| `tagpu_native.c:2348` | a unit's anchor tile — unexplored means skip, grey means skip unless it is ours |
| `tagpu_native.c:2581` | a wreck's anchor tile — hidden only where the map is unexplored |
| `tagpu_fx_tile_visible` | every projectile, explosion, particle and feature |

On refusal it calls `fog_alarm` — which logs, and pops a **modal message box at the player**, once
per process — and then returns **0, meaning "no fog"**. Every one of those gates reads 0 as
*nothing is hidden here*.

<div class="tablewrap fg">
<p class="fg-cap">a 5120×2880 screen, zoomed out, with the bound tripped</p>
<svg class="fg-dia" viewBox="0 0 700 250" role="img" aria-label="Two frames side by side. On the left, correct behaviour: unexplored ground is black and nothing is drawn on it. On the right, the bound tripped: the terrain is still black because the shader is unaffected, but enemy units, wreckage, trees and an explosion are drawn on top of the black, and a modal dialog is up.">
  <rect class="fg-box" x="40" y="34" width="290" height="180"/>
  <rect x="40" y="34" width="290" height="180" fill="#11161a"/>
  <text class="fg-lab" x="40" y="26">correct — the shader blacks it, the gates hide it</text>
  <circle cx="140" cy="120" r="34" fill="#3d4a35"/>
  <rect class="fg-use" x="128" y="110" width="9" height="9"/>
  <rect class="fg-use" x="148" y="126" width="9" height="9"/>
  <text class="fg-lab" x="185" y="200" text-anchor="middle">only our own LOS circle has anything in it</text>

  <rect class="fg-box" x="370" y="34" width="290" height="180"/>
  <rect x="370" y="34" width="290" height="180" fill="#11161a"/>
  <text class="fg-badtx" x="370" y="26" font-size="11">tripped — terrain still black, everything else drawn</text>
  <circle cx="470" cy="120" r="34" fill="#3d4a35"/>
  <rect class="fg-use" x="458" y="110" width="9" height="9"/>
  <rect class="fg-use" x="478" y="126" width="9" height="9"/>
  <g class="fg-bad">
    <rect x="560" y="70"  width="10" height="10"/>
    <rect x="600" y="96"  width="10" height="10"/>
    <rect x="576" y="140" width="10" height="10"/>
    <rect x="620" y="170" width="10" height="10"/>
    <rect x="530" y="180" width="10" height="10"/>
  </g>
  <circle cx="606" cy="128" r="9" fill="var(--warn)" opacity="0.75"/>
  <text class="fg-badtx" x="515" y="200" text-anchor="middle" font-size="11">enemy units, wrecks, trees, explosions</text>
  <text class="fg-lab"   x="515" y="212" text-anchor="middle">on ground the player has never seen</text>

  <rect class="fg-bad" x="470" y="222" width="190" height="20"/>
  <text class="fg-badtx" x="565" y="236" text-anchor="middle" font-size="10">▲ modal dialog, once per process</text>
</svg>
</div>

**This is the leak G13r closed, coming back through a different door.** The reason the wide grid
exists at all is that the border-cell smear was drawing an enemy Solar Collector on ground with no
LOS. A refused grid puts that back — worse, because it applies over the whole frame rather than
the outer ring, and it silently affects every sprite class at once.

It is unreachable on every screen this project has ever run. It is reachable on a 5K monitor,
which is a thing that exists.

## 3. The square that is not a window

The second problem is independent of the bound, and it costs something on **every** screen.
`fogw_alloc` takes three buffers of `FOGW_MAXDIM × FOGW_MAXDIM` — a **square** — while the window
is `cols × rows`, which is nothing like square. At 1920×1080 the window is 245 × 148.

<div class="tablewrap fg">
<p class="fg-cap">one buffer at 1920×1080: allocated against used</p>
<svg class="fg-dia" viewBox="0 0 700 300" role="img" aria-label="A 1024 by 1024 square representing one allocated buffer, with a small 245 by 148 rectangle in its top left corner representing the part actually written. The rectangle is 3.5 percent of the square.">
  <rect class="fg-dead" x="150" y="30" width="240" height="240"/>
  <rect class="fg-box"  x="150" y="30" width="240" height="240" fill="none"/>
  <rect class="fg-use"  x="150" y="30" width="57.4" height="34.7"/>
  <text class="fg-lab" x="150" y="22">1024 × 1024 cells allocated — 2 MB, ×3 buffers = 6 MB</text>
  <line class="fg-sig" x1="207" y1="40" x2="300" y2="90"/>
  <text class="fg-sigtx" x="306" y="88" font-size="11">245 × 148 — the window</text>
  <text class="fg-lab"   x="306" y="102">36,260 cells, 71 KB</text>
  <text class="fg-lab"   x="306" y="116">3.5 % of the buffer</text>
  <text class="fg-lab"   x="306" y="140">the other 96.5 % is never</text>
  <text class="fg-lab"   x="306" y="154">written, read or uploaded</text>
  <text class="fg-lab"   x="150" y="288">at 1024×768 it is 1.4 %; at 3840×2160, 13.1 %</text>
</svg>
</div>

Sizing to the window rather than to a square is a bigger win than raising the cap, and it wins at
every resolution — **including 8K**, where three window-shaped buffers still come to half what the
fixed square costs today.

| mode | viewport | window | 3 buffers | today |
| --- | --- | --- | --- | --- |
| 1024×768 | 896×704 | 133 × 109 | **85 KB** | 6 MB (72× more) |
| 1280×1024 | 1152×960 | 165 × 141 | **136 KB** | 6 MB (45×) |
| 1920×1080 | 1792×1016 | 245 × 148 | **212 KB** | 6 MB (29×) |
| 2560×1440 | 2432×1376 | 325 × 193 | **368 KB** | 6 MB (17×) |
| 3440×1440 | 3312×1376 | 435 × 193 | **492 KB** | 6 MB (13×) |
| 3840×2160 | 3712×2096 | 485 × 283 | **804 KB** | 6 MB (7.6×) |
| 5120×2880 | 4992×2816 | 645 × 373 | **1.4 MB** | 6 MB (4.4×) — *refused today* |
| 7680×4320 | 7552×4256 | 965 × 553 | **3.1 MB** | 6 MB (2.0×) — *refused today* |

## 4. Why it cannot simply be `realloc`'d

This is the part that makes "just size it dynamically" a design question rather than a one-line
change. The grid crosses a thread boundary, and it crosses it **by pointer**.

<div class="tablewrap fg">
<p class="fg-cap">the hand-over: three buffers, one owner each, and a pointer held across a whole frame</p>
<svg class="fg-dia" viewBox="0 0 700 320" role="img" aria-label="A timeline. The game thread builds into s_build and swaps it with s_pub inside a critical section. The render thread enters the same critical section, swaps s_pub with s_hold, leaves the section, and then reads s_hold for the rest of the frame — outside any lock. Reallocating the buffers at that moment would free the block the render thread is still reading.">
  <text class="fg-lab" x="20" y="30">game thread</text>
  <line class="fg-thin" x1="20" y1="40" x2="660" y2="40"/>
  <rect class="fg-use" x="60" y="30" width="120" height="20" opacity="0.85"/>
  <text x="120" y="45" text-anchor="middle" font-size="10">fogw_build → s_build</text>

  <rect class="fg-sig" x="196" y="24" width="54" height="32" fill="var(--panel-2)"/>
  <text class="fg-sigtx" x="223" y="18" text-anchor="middle" font-size="10">s_cs</text>
  <text x="223" y="45" text-anchor="middle" font-size="10">swap</text>

  <text class="fg-lab" x="20" y="120">render thread</text>
  <line class="fg-thin" x1="20" y1="130" x2="660" y2="130"/>
  <rect class="fg-sig" x="300" y="114" width="54" height="32" fill="var(--panel-2)"/>
  <text class="fg-sigtx" x="327" y="108" text-anchor="middle" font-size="10">s_cs</text>
  <text x="327" y="135" text-anchor="middle" font-size="10">swap</text>

  <rect class="fg-hold" x="354" y="120" width="266" height="20" opacity="0.85"/>
  <text x="487" y="134" text-anchor="middle" font-size="10.5" fill="#eef3f6">reads s_hold — no lock held</text>
  <line class="fg-thin" x1="354" y1="150" x2="620" y2="150"/>
  <text class="fg-lab" x="487" y="164" text-anchor="middle">the whole rest of the frame: upload, four passes, every CPU gate</text>

  <line class="fg-sig" x1="223" y1="56" x2="327" y2="114" stroke-dasharray="4 3"/>
  <text class="fg-lab" x="240" y="92">s_pub carries it across</text>

  <rect class="fg-bad" x="380" y="206" width="240" height="46"/>
  <text class="fg-badtx" x="500" y="224" text-anchor="middle" font-size="11">free(s_hold) here</text>
  <text class="fg-badtx" x="500" y="240" text-anchor="middle" font-size="11">= use-after-free, mid-frame</text>
  <line class="fg-sig" x1="500" y1="206" x2="500" y2="142" stroke-dasharray="4 3"/>

  <text class="fg-lab" x="20" y="284">A grow that runs on the game thread cannot see whether the render thread is</text>
  <text class="fg-lab" x="20" y="300">inside that blue band. Nothing in the current design tells it.</text>
</svg>
</div>

That is why the source says *"ALLOCATED ONCE AND NEVER GROWN, and that is a lifetime argument,
not a convenience"*. Any dynamic scheme has to answer it, because **the size a process needs can
grow inside that process**: stock TA cannot change resolution mid-game
([resolution](resolution.html) §6), but the front-end and the battleroom write a *desired* mode
and the switch happens at the next game entry — and this fork runs game → shell → game cycles.
So a session can legitimately go 1080p, back to the shell, then 4K.

## 5. The four ways to grow

<div class="tablewrap fg">
<p class="fg-cap">what happens to the block the render thread is holding</p>
<svg class="fg-dia" viewBox="0 0 700 320" role="img" aria-label="Four options compared. Grow and never free leaves the old block mapped so a stale pointer still reads. A quiescence handshake defers the free until the render thread has passed a fence. Sizing once from the engine's mode list avoids growth but is a fixed cap again. Sizing once and clamping brings the border smear back for the larger game.">
  <g>
    <rect class="fg-ok" x="24" y="34" width="150" height="120"/>
    <text x="99" y="52" text-anchor="middle" font-size="11">grow, never free</text>
    <rect class="fg-hold" x="40" y="64" width="52" height="26" opacity="0.85"/>
    <text class="fg-lab" x="66" y="102" text-anchor="middle">old</text>
    <rect class="fg-use" x="104" y="64" width="54" height="40" opacity="0.85"/>
    <text class="fg-lab" x="131" y="116" text-anchor="middle">new</text>
    <text class="fg-lab" x="99" y="134" text-anchor="middle">stale pointer still</text>
    <text class="fg-lab" x="99" y="146" text-anchor="middle">reads — one stale frame</text>
  </g>
  <g>
    <rect class="fg-box" x="188" y="34" width="150" height="120"/>
    <text x="263" y="52" text-anchor="middle" font-size="11">quiescence handshake</text>
    <rect class="fg-hold" x="204" y="64" width="52" height="26" opacity="0.45"/>
    <line class="fg-sig" x1="264" y1="60" x2="264" y2="96"/>
    <text class="fg-sigtx" x="270" y="76" font-size="10">fence</text>
    <rect class="fg-use" x="272" y="64" width="52" height="40" opacity="0.85"/>
    <text class="fg-lab" x="263" y="122" text-anchor="middle">old block freed, later</text>
    <text class="fg-lab" x="263" y="134" text-anchor="middle">+ epoch, ring, overflow</text>
    <text class="fg-lab" x="263" y="146" text-anchor="middle">counter that must read 0</text>
  </g>
  <g>
    <rect class="fg-box" x="352" y="34" width="150" height="120"/>
    <text x="427" y="52" text-anchor="middle" font-size="11">size from the mode list</text>
    <rect class="fg-use" x="368" y="64" width="118" height="40" opacity="0.85"/>
    <text class="fg-lab" x="427" y="122" text-anchor="middle">no growth path at all</text>
    <text class="fg-lab" x="427" y="134" text-anchor="middle">but a fixed cap again —</text>
    <text class="fg-lab" x="427" y="146" text-anchor="middle">the fork can force a mode</text>
  </g>
  <g>
    <rect class="fg-bad" x="516" y="34" width="160" height="120"/>
    <text x="596" y="52" text-anchor="middle" font-size="11">size once, clamp after</text>
    <rect class="fg-use" x="532" y="64" width="60" height="30" opacity="0.85"/>
    <rect class="fg-ghost" x="532" y="64" width="128" height="46"/>
    <text class="fg-badtx" x="596" y="128" text-anchor="middle" font-size="10.5">the smear returns for</text>
    <text class="fg-badtx" x="596" y="140" text-anchor="middle" font-size="10.5">the second, bigger game</text>
  </g>

  <text class="fg-lab" x="24" y="196">The blue block is the one the render thread may still be reading when the grow runs.</text>
  <text class="fg-lab" x="24" y="212">Only the first two keep it readable; the fourth never grows, so it never has the problem —</text>
  <text class="fg-lab" x="24" y="228">it just gives the player the bug this whole module exists to remove.</text>

  <rect class="fg-ok" x="24" y="246" width="652" height="58"/>
  <text class="fg-oktx" x="40" y="266" font-size="11">Recommended: grow, never free.</text>
  <text class="fg-lab" x="40" y="282">A mode change happens a handful of times a session, and each abandoned set is a few</text>
  <text class="fg-lab" x="40" y="296">hundred KB — against the 6 MB this frees on the very first in-game tick.</text>
</svg>
</div>

**Why "never free" is not a leak worth worrying about here.** The abandoned set is bounded by the
number of *distinct, increasing* window sizes a session visits, which is bounded by the number of
video modes the player switches between — a handful, and only ever upward, since a smaller window
reuses the block it already has. The largest set anyone can strand is one 8K set, 3.1 MB, and
reaching it means having already allocated the 8K one that replaced it. Against today's baseline
of 6 MB committed on the first in-game tick in **every** session, every path here is a net
reduction.

## 6. What is actually being decided

Three separable questions, in dependency order:

1. **Does the buffer track the window, or stay square?** Tracking the window is where the memory
   win is (72× at 1024×768, 29× at 1080p) and it is a precondition for the rest.
2. **How does it grow when a later game in the same process needs more?** The four options above.
3. **What replaces the `512` in `tagpu_fog_at`?** It has to become the same expression the
   producer sizes from, not a second number — that is the defect this page exists to describe,
   and re-typing a different literal reproduces it a year from now.

There is a fourth question hiding behind (3): `tagpu_native.c` bounds the **engine's** grid
descriptor at `cols <= 1024 && rows <= 1024`, which is a sanity check on a struct read out of
engine memory rather than a statement about our own window. That one should stay a fixed,
generous bound — it is guarding against a corrupted descriptor, and the engine's own grid is
`viewW/32 + 2` cells, so it cannot legitimately approach 1024 until the viewport is 32,000 px
wide.
