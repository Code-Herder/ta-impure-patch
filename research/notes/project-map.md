# Project Map — Lineage, Features, Repos & Versions

Every project documented in this wiki, how they descend from one another, and where each one
actually lives. All dates verified 2026-08-31.

## The lineage

<div class="tablewrap">
<svg viewBox="0 0 920 660" width="920" height="660" role="img" aria-label="Lineage diagram of Total Annihilation exe-modding projects">
<style>
.bx{fill:var(--panel-2);stroke:var(--edge);stroke-width:1.2}
.bx-core{fill:var(--panel-2);stroke:var(--accent);stroke-width:2}
.bx-dead{fill:none;stroke:var(--edge);stroke-width:1.2;stroke-dasharray:5 4}
.t{font-family:"IBM Plex Sans",sans-serif;font-size:13px;fill:var(--ink)}
.t-b{font-family:"Saira Condensed",sans-serif;font-size:15px;font-weight:600;fill:var(--ink)}
.t-s{font-family:"IBM Plex Mono",monospace;font-size:10.5px;fill:var(--ink-3)}
.t-era{font-family:"Saira Condensed",sans-serif;font-size:11px;font-weight:600;fill:var(--ink-3);letter-spacing:.09em}
.ln{stroke:var(--edge);stroke-width:1.6;fill:none}
.ln-a{stroke:var(--accent);stroke-width:2;fill:none}
</style>
<defs>
<marker id="ah" markerWidth="9" markerHeight="9" refX="7" refY="3" orient="auto">
<path d="M0,0 L7,3 L0,6 z" fill="var(--edge)"/></marker>
<marker id="aha" markerWidth="9" markerHeight="9" refX="7" refY="3" orient="auto">
<path d="M0,0 L7,3 L0,6 z" fill="var(--accent)"/></marker>
</defs>

<text class="t-era" x="8" y="34">1997–98</text>
<rect class="bx-core" x="330" y="14" width="270" height="46" rx="7"/>
<text class="t-b" x="345" y="34">Cavedog TotalA.exe</text>
<text class="t-s" x="345" y="50">1997 · MSVC 5.0 · unpacked · base 0x400000</text>
<rect class="bx-dead" x="640" y="14" width="230" height="46" rx="7"/>
<text class="t" x="654" y="34">Official patch v3.1 (1998)</text>
<text class="t-s" x="654" y="50">line ends here — 28 years ago</text>
<path class="ln" d="M600,37 L638,37" marker-end="url(#ah)"/>

<text class="t-era" x="8" y="126">2002–15</text>
<rect class="bx" x="100" y="104" width="250" height="50" rx="7"/>
<text class="t-b" x="114" y="124">Xpoy — TA Interface Upgrade</text>
<text class="t-s" x="114" y="141">ddraw line · weapon-ID crack, early megamap</text>
<rect class="bx" x="560" y="104" width="250" height="50" rx="7"/>
<text class="t-b" x="574" y="124">Fnordia / SJ / Yeha — TA Demo</text>
<text class="t-s" x="574" y="141">dplayx line · replays, .tad format</text>
<rect class="bx" x="600" y="176" width="180" height="38" rx="7"/>
<text class="t" x="614" y="200">Rime (2015)</text>
<path class="ln" d="M420,62 L225,100" marker-end="url(#ah)"/>
<path class="ln" d="M510,62 L680,100" marker-end="url(#ah)"/>
<path class="ln" d="M685,154 L690,172" marker-end="url(#ah)"/>

<text class="t-era" x="8" y="264">2019–23</text>
<rect class="bx-core" x="280" y="242" width="360" height="50" rx="7"/>
<text class="t-b" x="295" y="262">TADR — tanvanman/TADR</text>
<text class="t-s" x="295" y="279">fork of svn.riouxsvn.com/tadr · MIT · both lines merge</text>
<path class="ln-a" d="M225,154 L390,238" marker-end="url(#aha)"/>
<path class="ln-a" d="M690,214 L540,238" marker-end="url(#aha)"/>

<text class="t-era" x="8" y="356">2023–26</text>
<rect class="bx-core" x="215" y="334" width="180" height="48" rx="7"/>
<text class="t-b" x="229" y="354">tdraw.dll</text>
<text class="t-s" x="229" y="370">C++ · the engine patches</text>
<rect class="bx-core" x="420" y="334" width="180" height="48" rx="7"/>
<text class="t-b" x="434" y="354">tplayx.dll</text>
<text class="t-s" x="434" y="370">Delphi · net + recorder</text>
<rect class="bx" x="650" y="326" width="240" height="64" rx="7"/>
<text class="t-b" x="664" y="346">FunkyFr3sh toolchain</text>
<text class="t-s" x="664" y="362">Patch Loader (dplayx proxy)</text>
<text class="t-s" x="664" y="378">cnc-ddraw · petool</text>
<path class="ln-a" d="M400,292 L320,330" marker-end="url(#aha)"/>
<path class="ln-a" d="M520,292 L510,330" marker-end="url(#aha)"/>
<path class="ln" d="M600,358 L646,358" marker-end="url(#ah)" stroke-dasharray="4 3"/>

<rect class="bx" x="280" y="424" width="360" height="42" rx="7"/>
<text class="t" x="294" y="450">One DLL, six build configs — features gated per mod</text>
<path class="ln-a" d="M305,382 L400,420" marker-end="url(#aha)"/>
<path class="ln-a" d="M510,382 L510,420" marker-end="url(#aha)"/>

<text class="t-era" x="8" y="530">TODAY</text>
<rect class="bx" x="40" y="508" width="125" height="40" rx="6"/><text class="t" x="54" y="533">OTA · 6/23</text>
<rect class="bx" x="180" y="508" width="125" height="40" rx="6"/><text class="t" x="194" y="533">ProTA · 19/23</text>
<rect class="bx" x="320" y="508" width="145" height="40" rx="6"/><text class="t" x="334" y="533">Escalation · 22/23</text>
<rect class="bx" x="480" y="508" width="135" height="40" rx="6"/><text class="t" x="494" y="533">Mayhem · 20/23</text>
<rect class="bx" x="630" y="508" width="125" height="40" rx="6"/><text class="t" x="644" y="533">TA:Zero · 15/23</text>
<rect class="bx" x="770" y="508" width="110" height="40" rx="6"/><text class="t" x="784" y="533">BTA · 15/23</text>
<path class="ln" d="M400,466 L110,504" marker-end="url(#ah)"/>
<path class="ln" d="M440,466 L240,504" marker-end="url(#ah)"/>
<path class="ln" d="M480,466 L392,504" marker-end="url(#ah)"/>
<path class="ln" d="M520,466 L545,504" marker-end="url(#ah)"/>
<path class="ln" d="M560,466 L690,504" marker-end="url(#ah)"/>
<path class="ln" d="M600,466 L820,504" marker-end="url(#ah)"/>

<rect class="bx-dead" x="290" y="592" width="340" height="46" rx="7"/>
<text class="t" x="304" y="612">TA Forever — lobby, replays, matchmaking</text>
<text class="t-s" x="304" y="628">consumes the stack · patches no bytes</text>
<path class="ln" d="M420,548 L440,588" marker-end="url(#ah)" stroke-dasharray="4 3"/>
</svg>
</div>

**Reading the diagram:** amber boxes and lines are the load-bearing path — everything modern flows
through `TADR`. Dashed boxes are dead ends or non-patching consumers. The two ancestor branches are
genuinely separate codebases (C++ and Delphi) that merged into one repository but never into one
program; they still ship as two DLLs.

## Projects, repos and current versions

| Project | Role | Website | Repo | Latest | Date | Licence |
|---|---|---|---|---|---|---|
| **TADR** (`tdraw.dll` + `tplayx.dll`) | The patch core — all engine features | *none* | `github.com/tanvanman/TADR` | `v2026.8.6` (stable) · `dev-13d71dd` | 2026-08-06 · 2026-08-31 | MIT |
| **TA Patch Loader** | `dplayx.dll` proxy; removes the need for a hex-edited exe | *none* | `github.com/FunkyFr3sh/Total-Annihilation-Patch-Loader` | `v1.3.0.0` | 2024-05-02 | MIT |
| **cnc-ddraw** | DirectDraw reimplementation — windowed, upscaling, shaders | *none* | `github.com/FunkyFr3sh/cnc-ddraw` | rolling | 2026-08-30 | open source |
| **petool** | Extends a 32-bit PE so new code can be added | *none* | `github.com/FunkyFr3sh/petool` | rolling | 2024-12-28 | MIT (`res/` BSD0) |
| **Unofficial Patch 3.9.02** | The end-user distribution of the above | tauniverse.com (forum, gated) | `github.com/gammata/TA-Unofficial-Patch-Install` | `3.9.02` | 2023-08 | — |
| **TA: Escalation** | Mod — consumes the patch, authors none of it | `taesc.tauniverse.com` | *none* | `GOLD 10.2.0` | 2026-08-08 | — |
| **Total Mayhem** | Mod — 160 INI patch sections | `mayhem.tauniverse.com` | patches in Patch Loader `res/mayhem.ini` | `11.3.0` | 2024-06 | — |
| **ProTA** | Mod — 69 INI patch sections, targeting changes | — | patches in Patch Loader `res/prota.ini` | `4.5` | 2023-04 | — |
| **TA Zero** | Mod — COB-scripted shields, `zdraw.dll` = the patch | `zero.tauniverse.com` | *none* | `Alpha 5` | 2024-12 | — |
| **TA Forever** | Lobby, matchmaking, live replays — patches nothing | `taforever.com` | `github.com/ta-forever` | client `v2026.8.23` | 2026-08-23 | MIT / GPL-3 |
| **totala-re** | Standalone RE notes (radare2, 391 functions) | *none* | `github.com/ioma8/totala-re` | — | 2025-10 | — |
| **jtaext** | Historical: 86-byte NASM stub + plugin DLL | *none* | *none* (`jtaext_full.zip`) | — | 2006 | — |

Only two projects here are under active development: **TADR** (weekly-ish) and **TA Forever**.
The FunkyFr3sh toolchain is stable-and-maintained. Everything else last shipped between 2023 and 2024.

## Feature totals

Counted from the `v2026.8.6` release-notes matrix — 23 gated features across four groups. A config
scores a point where the feature is enabled at all, so the numeric rows (mex-snap radius, off-map
tiles) count as present even at their stock value. Full breakdown in [[release-matrix]].

| Config | Build/placement (5) | HUD/display (8) | Sim/netcode (4) | Combat (6) | **Total** |
|---|---|---|---|---|---|
| **Escalation** | 4 | 8 | 4 | 6 | **22 / 23** |
| **Mayhem** | 5 | 8 | 3 | 4 | **20 / 23** |
| **ProTA** | 5 | 8 | 3 | 3 | **19 / 23** |
| **TA:Zero** | 5 | 6 | 3 | 1 | **15 / 23** |
| **BTA** | 5 | 7 | 2 | 1 | **15 / 23** |
| **OTA** (stock) | 0 | 3 | 2 | 1 | **6 / 23** |

Escalation is the most-served config and authors none of it — share guard, repair-rate fix, falling
air wrecks and extended weapon IDs are all Escalation-only, implemented upstream at Wotan's request.
Stock OTA is deliberately the leanest: only competitively neutral features are on.

## Separate measures of scale

Feature counts describe the shared DLL. The mods' own byte-patch sets are a different axis:

| Measure | Value |
|---|---|
| TADR annotated code addresses | ~470 (~809 distinct across `src/DDraw/`) |
| `tamem.h` struct map | ~1,980 lines, `static_assert`-checked offsets |
| Total Mayhem INI patch sections | 160 |
| ProTA INI patch sections | 69 (67 shared with Mayhem, 64 byte-identical) |
| Patch Loader named patches | 14 |
| Engine command table entries | 83 (43 NORMAL / 10 CHEAT / 30 DEBUG) |

The ProTA/Mayhem overlap is the key structural fact: these are not independent reverse-engineering
efforts but one shared corpus of community-known offsets. See [[deep-prota]].
