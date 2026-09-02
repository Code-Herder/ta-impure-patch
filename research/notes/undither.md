<link rel="stylesheet" href="assets/undither/undither.css?v=__ASSETV__">

# Undithering the 8bpp frame

*What Total Annihilation's own frame buffer looks like once the 256-colour dither is
resolved back into the colours it was standing in for. Fifteen 1920×1080 engine
screenshots — one per map environment in TA and The Core Contingency — captured
2026-09-01 with `tools/tacli`, measured with `tools/undither/restore.py`, and replayed
here through a GPU port of the same three stages so each can be switched on and off and
the undither kernel tuned live.*

## Summary

TA composes every frame into an 8bpp surface, so the picture that reaches the screen is
palette-limited no matter what resolution it runs at. Where the artists wanted a colour
the palette could not hold they dithered: neighbouring pixels alternate between two
palette entries and the eye averages them. That average is *recoverable* — an
edge-preserving filter whose colour tolerance sits just above the dither amplitude and
well below real edge contrast reconstructs the intended colour exactly, without inventing
anything. On these fifteen captures it turns visibly speckled sand, water and rock into
smooth surfaces while keeping unit silhouettes and terrain edges intact (edge retention
0.98–1.00 across the set). [VERIFIED]

Three findings worth carrying forward. First, **a single TA frame uses only 119–200
distinct colours** — a third of the palette is unused in any one scene, which is why the
banding is as coarse as it is. Second, **the restoration spec's kernel is too wide for
TA's ground art.** Its σ<sub>s</sub> = 1.8 bilateral runs on a 7×7 disc, twice; a
period-2 dither needs nothing like that reach, and on painted single-pixel speckle —
green-planet grass, desert grit, the Urban tileset — the disc outvotes every fleck and
leaves flat mud. A 5×5 disc (σ<sub>s</sub> = 1.2) resolves the dither just as completely
on the water and rock frames and keeps the grass looking like grass; it is now the
viewer's default, with the spec kernel one click away for comparison. Third, and a
correction to the first draft of this page: **the spec's dither detector was right to
stay silent on ten of the fifteen frames.** Its lag-1 autocorrelation test does not fire
where the high-frequency content is authored texture rather than dither, and those are
exactly the frames the spec kernel damages. The detector is a texture-versus-dither
classifier working as designed; what was wrong was forcing a texture-sized kernel through
it. [VERIFIED]

## Try it

Drag the seam to wipe between the original and the restored frame; drag anywhere else to
pan; the wheel zooms. **Zoom to 2:1 or 4:1 — at fit-to-window the dither is smaller than a
screen pixel and there is nothing to see.** The second row of controls is the undither
kernel: four presets (the spec's 7×7, the tuned 5×5 default, a 3×3 cross, and the 5×5
with a luma/chroma-split colour metric), then the individual knobs behind them —
kernel size, metric, σ<sub>r</sub> as a multiple of q, pass count, and a strength blend
back toward the original. Green planet at 4:1 with **Spec 7×7** against **Tuned 5×5** is
the whole argument of this page in one wipe.

<div id="undither-viewer"></div>

The same viewer runs standalone as `tools/undither/app/index.html`, which is the
prototype this page was built from.

## How the captures were made

`tools/undither/capture.py` drives one `tacli` instance per environment. The front-end
menus drop keys, so it does not fire a canned sequence: it screenshots, classifies the
screen against reference fingerprints of the three menus, sends the key that screen needs
and repeats. Settings that matter:

| Setting | Value | Why |
|---|---|---|
| `--res 1920x1080` | registry `DisplaymodeWidth/Height` | the front end always runs 640×480; the desired mode is applied on game entry (see [Resolution](resolution.html)) |
| `--mapping 0` | "Mapped" | 1 and 2 both read "Unmapped"; without this the frame is mostly fog-black and there is no terrain to look at |
| `--los 0` | "Permanent" | unit visibility only; terrain is already revealed by Mapped |
| camera | untouched | the engine opens centred on the local commander, and nothing scrolls it while no input is sent |

Each run waits for `alive=[1-9]` in `tagpu.log`, lets the game run 20 s at speed +3, then
takes the engine surface shot — TA's own 8bpp buffer, saved as an indexed PNG, which is
exactly the input `restore.py` expects. The skirmish "Current Map" field is cropped from
the setup screen on the way past and kept next to each capture, so the registry map name
can be shown to have taken; all fifteen match. [VERIFIED]

## The three stages

Ordered deliberately: the two that *recover* information the image already encodes run
first, the one that *invents* it is last and optional.

**1 — Undither** (`undither_bilateral`, `undither_split`). Two passes of a bilateral
filter, σ<sub>r</sub> = 1.3 q, where q is the measured dither amplitude. The colour
tolerance is the constant the method lives on: below q the filter treats the dither
checkerboard as an edge and preserves it (the "it does nothing" failure); above real edge
contrast it melts silhouettes. At 1.3 q a dither difference gets weight ≈0.7 and a
three-step edge ≈0.07. Palette-limited art almost always has multi-step real edges, which
is why the gap exists. The *spatial* extent is the constant the spec got wrong for this
material: it asks for σ<sub>s</sub> = 1.8, which OpenCV turns into a 7×7 disc, and the
section below shows why that is twice what a period-2 dither needs. `restore.py` now
carries the kernel as named presets (`--preset spec|tuned|tight|split`, each field
overridable) and the viewer defaults to `tuned`.

**2 — Deband** (`deband`). Where the artist did not dither, a gradient became a staircase
of one-palette-step contours. Blurring across them with σ = 6 px reconstructs the ramp;
anything stronger than 1.5 × step is protected as a real edge and dilated 5×5 so the blur
cannot leak into it. Note the step is measured *after* undithering — on ten of the fifteen
frames it drops below the 8-unit floor once the dither is resolved, and deband correctly
becomes a no-op.

**3 — Enhance** (`enhance_procedural`). Local contrast plus saturation-aware vibrance. It
is the deterministic stand-in for a learned restoration model, and it keeps the two parts
of one that are reproducible while omitting the third — invented tonal drift — which is
exactly what would make two tiles of the same wall stop matching. Expect a 2–3 unit mean
colour shift; that is the vibrance working, not a bug. **No generative or diffusion model
belongs in this pipeline.**

## Why the spec kernel muddies TA grass

The first cut of this page ran the spec kernel on all fifteen frames and called the result
good. Green planet was not good: at 1:1 the grass had gone from speckled to a flat green
smear, and Urban, Desert and Metal had lost their grit the same way, while Crystal and
Water World looked excellent. The difference is what the high-frequency content *is*.

<a href="assets/undither/figs/kernels.png"><img src="assets/undither/figs/kernels.png" alt="Green planet, Urban and Water World at 4:1 — original, spec 7×7, tuned 5×5, luma/chroma 5×5" style="width:100%"></a>

*Green planet, Urban and Water World at 4:1: original, spec 7×7, tuned 5×5, and the 5×5
with the luma/chroma-split metric. Rendered by `restore.py` itself, not the viewer.*

Counting palette indices in every 3×3 window of the terrain says it directly. A true
two-tone dither — the thing the spec is built for — shows up as windows holding exactly
two indices; painted speckle shows up as windows holding four or more:

| Frame | 2-index windows | 4+-index windows | Result with the spec kernel |
|---|---|---|---|
| Water World | 41 % | 1 % | excellent — real dither, fully resolved |
| Lush | 21 % | 46 % | good |
| Wet Desert | 19 % | 59 % | good |
| Archipelago | 14 % | 66 % | water good, rock faces smeared |
| Acid | 12 % | 58 % | good |
| Crystal | 1 % | 95 % | excellent — streaks are coherent, cracks are high-contrast |
| Urban | 2 % | 87 % | grass flattened |
| Green planet | 1 % | 90 % | grass flattened |
| Metal | 1 % | 94 % | panel grain flattened |
| Desert | 0.5 % | 97 % | grit flattened |

Strict checkerboards are below 0.3 % everywhere: TA's dither is error-diffusion-like, not
ordered. So the population splits into frames where the noise is two colours alternating
(dither — average them and the intended colour comes back) and frames where it is many
colours in single-pixel flecks (texture — average them and the texture is gone). Crystal
sits in the second group and still looks good because its texture is *coherent*: streaks
several pixels long keep each other alive under the bilateral, and the cracks are far above
σ<sub>r</sub>. Isotropic speckle has no such protection.

The mechanism is spatial reach, not colour tolerance. σ<sub>s</sub> = 1.8 gives OpenCV a
disc of radius 3, and two passes reach ~6 px. A single orange fleck in green sits at
weight 0.26 against ~28 green neighbours, so it is outvoted no matter what σ<sub>r</sub>
says. A period-2 dither, on the other hand, is fully resolved by any kernel that spans
one period. Measured on the terrain area, Nyquist-band energy (the checkerboard component)
and how much of the 3–8 px band survives:

| Frame | original | spec 7×7 | tuned 5×5 | tight 3×3 | luma ÷ chroma 5×5 |
|---|---|---|---|---|---|
| Green planet | 60 | 7 · keeps 53 % | 10 · keeps 72 % | 14 · keeps 87 % | 28 · keeps 83 % |
| Urban | 59 | 10 · 56 % | 12 · 74 % | 17 · 88 % | 25 · 83 % |
| Desert | 205 | 107 · 78 % | 125 · 89 % | 146 · 96 % | 161 · 93 % |
| Crystal | 63 | 17 · 83 % | 22 · 91 % | 27 · 96 % | 45 · 93 % |
| Water World | 53 | 1.4 · 38 % | 2.5 · 66 % | 4.7 · 88 % | 17 · 89 % |

The 5×5 disc removes 80–95 % of the Nyquist energy that the 7×7 does, and keeps 20–30
points more of the mid band. Edge retention is 0.98–1.00 under every kernel (table below).
That is the trade the tuned preset makes; the 3×3 cross keeps still more texture but
leaves visible grain on the water frames, and is offered as a preset rather than the
default for that reason. The split metric is the other end of the same trade: it keeps
the most texture and removes the least dither — on Water World a third of the Nyquist
energy is still there — so it is the preset for textured frames, not a general default.

Approaches that were tried and did not earn a preset: lowering σ<sub>r</sub> to 0.8 q
(barely filters — the fleck contrast is well above q anyway), non-local means (plastic
mush, worse than the spec), a texture gate on distinct-index count (correctly leaves the
grass alone, which means it does nothing to it), and a per-pixel σ<sub>r</sub> from the
nearest-neighbour colour distance (converges to the tuned kernel). One did: **splitting
the range weight into luma and chroma** (`undither_split`) — luma tolerance 0.31 σ<sub>r</sub>,
chroma tolerance 1.15 σ<sub>r</sub>. Off-hue dither flecks are mostly chroma; light/dark
grass structure is mostly luma. It keeps the most texture of any 5×5 variant and is the
"Luma ÷ chroma" preset. Its caveat: it leaves a band step of 8–17 on the textured frames,
which trips the deband gate, and deband's σ = 6 blur then takes some of that texture back.
Toggle Deband off to see the kernel alone. The gate is measuring texture contrast as if it
were a palette staircase; a deband gate that looked at run length rather than neighbour
distance would not fire there. [LEAD]

One more approach is on the table and deliberately not done here: a small learned
deterministic 1× restorer trained on true-colour art quantised to TA's palette with
error-diffusion dither. The spec allows it (section 5.3) and it is the only way to tell
"fleck the artist painted" from "fleck the quantiser made"; everything above is a
heuristic standing in for that. [LEAD]

## What the frames measure

Per-frame analysis from `tools/undither/prep.py`, which calls `restore.py`'s own
`analyze()` and `band_step()`. `q` is the dither amplitude in RGB distance (0–441 scale);
`ac1` is the lag-1 autocorrelation of the high-frequency residual in flat regions; the step
columns are the median non-zero neighbour distance before undithering and after each
kernel preset (that number gates deband). Edge retention is the worst case across presets.

| Environment | Map | Set | Colours | q | ac1 | step | after spec | after tuned | after split | detector | edge ret. |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Green planet | Plains and Passes | CC | 176 | 35.8 | −0.18 | 46.1 | 4.1 | 6.0 | 11.9 | no | 0.98 |
| Desert | Painted Desert | TA | 189 | 39.8 | −0.07 | 67.5 | 17.5 | 22.1 | 28.6 | no | 0.99 |
| Archipelago | East Indeez | CC | 200 | 31.2 | −0.15 | 49.5 | 6.4 | 7.7 | 10.8 | no | 1.00 |
| Lava | Red Hot Lava | TA | 171 | 17.1 | −0.09 | 23.0 | 4.1 | 4.9 | 5.8 | no | 1.00 |
| Metal | Core Prime Industrial Area | CC | 197 | 18.1 | +0.02 | 39.8 | 22.0 | 22.5 | 24.9 | no | 1.00 |
| Lunar | Moon Quartet | CC | 154 | 21.2 | −0.06 | 27.7 | 15.6 | 13.9 | 15.6 | no | 1.00 |
| Ice | Polar Range | CC | 161 | 19.6 | −0.01 | 21.5 | 2.8 | 3.6 | 5.0 | no | 0.99 |
| Red Planet | Red River | CC | 178 | 35.8 | −0.32 | 54.0 | 5.4 | 5.8 | 16.4 | **yes** | 0.99 |
| Slate | Temblorian Mist | CC | 169 | 41.2 | −0.24 | 46.3 | 2.2 | 3.6 | 4.6 | **yes** | 0.99 |
| Lush | Lusch Puppy | CC | 200 | 12.8 | −0.05 | 21.5 | 5.4 | 6.0 | 9.6 | no | 1.00 |
| Crystal | Crystal Maze | CC | 167 | 28.6 | −0.19 | 46.3 | 9.5 | 11.5 | 17.4 | no | 0.99 |
| Acid | Acid Pools | CC | 173 | 17.9 | −0.37 | 26.5 | 3.3 | 4.2 | 4.6 | **yes** | 0.99 |
| Water World | Brain Coral | CC | 119 | 33.3 | −0.26 | 44.9 | 2.8 | 3.6 | 7.1 | **yes** | 0.99 |
| Wet Desert | Lake Shore | CC | 183 | 20.4 | −0.09 | 47.3 | 20.4 | 14.5 | 19.7 | no | 1.00 |
| Urban | Assault on Suburbia | CC | 178 | 38.1 | −0.21 | 46.1 | 3.2 | 5.0 | 8.4 | **yes** | 0.98 |

Readings from those numbers:

- **q clusters at 13–41**, i.e. the dither spans a third to a tenth of the palette's
  dynamic range. σ<sub>r</sub> = 1.3 q therefore lands between 17 and 54 — comfortably
  under the contrast of a unit silhouette against terrain, which is what the 0.98–1.00
  edge retention confirms under every kernel.
- **`step` before undithering is 21–68; after the tuned kernel it is 4–23.** On ten frames
  the dither *was* the step: resolve it and there is no staircase left to deband. Desert,
  Metal, Lunar, Crystal and Wet Desert keep a real one under the spec and tuned kernels;
  the split kernel adds Green planet, Archipelago, Lush, Red Planet and Urban to the list,
  and on those it is texture contrast being mistaken for a band (see above).
- **The detector's `ac1 < −0.2` gate is a texture-versus-dither test, and it agrees with
  the index counts.** The five frames it passes are five of the six with the most two-index
  windows; Lush is the miss. What it cannot do is say that a smaller kernel would be safe
  on the frames it rejects — that is what the tuned preset adds. A gate on `q` and the
  fraction of two-index windows would make the same call more directly, and could pick the
  kernel size as well as the on/off decision. [LEAD]
- **No frame is tileable** (opposite borders differ), so padding is `reflect` throughout —
  correct for screenshots, and the reason none of the wrap-mode machinery is exercised
  here. It matters for textures, not for frames.

## The environments

Fifteen captures, one per environment type: eight from the base game's tilesets and seven
introduced or dominated by The Core Contingency. Click for full size.

<div id="undither-gallery"></div>

## Reproducing

```bash
pip install opencv-contrib-python pillow numpy          # cv2 is not a repo dependency

python3 tools/undither/capture.py                       # 15 instances, ~1 min each
python3 tools/undither/prep.py                          # measure -> shots.json + thumbs
python3 -m http.server 8777                             # then open
#   http://127.0.0.1:8777/tools/undither/app/index.html

# the reference pipeline on its own, with sidecars and QA per image
python3 tools/undither/restore.py --in research/notes/assets/undither/shots \
                                  --out /tmp/restored --enhance procedural

# what the viewer shows by default: the tuned kernel, forced on every frame
python3 tools/undither/restore.py --in research/notes/assets/undither/shots \
                                  --out /tmp/restored --preset tuned --force-undither
# any preset field can be overridden: --sigma-s 1.2 --passes 3 --k 1.0 --metric split
```

`restore.py` is the reference implementation and the source of truth for every parameter;
its defaults are still the spec's, and the kernel presets are opt-in there. `assets/undither/undither.js`
is a stage-for-stage WebGL2 port of it, run on the GPU so the toggles are instant; it
reproduces OpenCV's L1 colour metric in the bilateral filter, its `radius = round(1.5σ)`
disc, its `ksize = round(σ·8+1)|1` gaussian radii and its `BORDER_REFLECT_101` edges, and
it carries 8-bit render targets between stages because `restore.py` hands `uint8` from one
stage to the next. Every filter parameter is measured by `prep.py` on the CPU and handed
to the shader; the only thing JavaScript decides is which preset's measurements apply to a
custom kernel (nearest kernel size), and the readout says when it is doing that.

The port is checked numerically, not by eye: the GPU framebuffer is read back in Chrome
and compared with `restore.py`'s output on 32×24 crops of Green planet, Water World and
Crystal under the tuned, tight and split presets. Undither alone differs by a mean of
0.04–0.08 levels (max 2); undither plus deband by 0.19–0.38 (max 2). The spec kernel was
checked the same way when the page was first built (mean 0.06, max 1). [VERIFIED]

## Where this fits

This is not part of the renderer. It is the evidence for one line of the
[GPU renderer roadmap](roadmap.html): the 8bpp composite path is palette-locked, and these
frames show how much colour that ceiling is costing — a third of the palette unused per
scene, gradients quantised to 20–70 unit steps, and the difference resolved out of the
dither being large enough to see at a glance. The same three stages are directly useful
for the [3DO/GAF texture](file-formats.html) work, where the input really is 256-colour
tiling texture art and the wrap-padding, colourkey and BC7 parts of the spec all come into
play.

<script src="assets/undither/undither.js?v=__ASSETV__"></script>
<script src="assets/undither/undither-ui.js?v=__ASSETV__"></script>
<script src="assets/undither/undither-page.js?v=__ASSETV__"></script>
