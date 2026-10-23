---
store: .data/pixel-forge
model: gemini-3-pro-image
size: 4K
budget_usd: 5
cutout: "key #FF00FF"
---
# pixel-forge in this repository

The art made here is the replay HUD's kit: plates, housings, buttons and icons, painted at high
resolution, cut into 9-slice and one-piece parts, and filed into `art/hud/`, which the mockup
(`tools/hudart/mockup/`) and the DLL draw from. **Read `research/notes/hud-art.md` before any
render**: it holds the style guide, the method, the tools and what each one measures.

## The method

1. **Layouts come from this repository's tools**, never drawn by hand:
   - a one-piece part (a plate with recesses) from `tools/hudart/whitebox.py <spec> <out>`, which
     writes `<out>.png` and `<out>.json` (the zones `gridslice.py` measures against);
   - a sheet of 9-slice parts from `PLAIN=1 tools/hudart/sheets.py <spec> <out dir>`, which writes
     `parts_<sheet>.png` and `sheets.json`.
   The specs are in `tools/hudart/specs/`; a one-off spec may live beside the job's inputs.
   A recess drawn into a 9-slice's whitebox sits where `art/hud/layout.json` puts what the game draws in it:
   the picker's list area starts below its title band (`pad` + `title.h`, 20 units), and a list area
   painted higher puts the title on its edge.
2. **The look is a piece of a finished part, placed on the layout itself** beside the whiteboxes and
   named as the reference in the prompt ("keep it exactly as it is"): a crop of the dock from
   `art/hud/parts/`. Passed as a separate style reference instead, its content is copied into the
   painting (an extra dock plate appeared above the parts) and the old paint is kept.
3. **Every layout is drawn at an aspect the model takes exactly** (21:9, 16:9, 3:2, ...), so
   nothing is stretched to fit and back: a sheet holds several parts at their true aspect, each
   whitebox at one scale, and is laid out to fill that aspect. Render with `pixel-forge gen --ref
   <layout>.png:layout`. The model is **Nano Banana Pro**: it keeps a layout's recess widths (Nano
   Banana 2 and 2.1 paint them all equal), at the cost of 21:9 as its widest aspect. Test at
   `--size 1K`; render the kit at 4K, and look at each 4K render before cutting it: one in four
   reframed its sheet (the parts painted several times taller, the reference dropped), and a reroll
   kept the layout.
   **A part longer than 21:9 is never stretched to fit**: squashing the painting back leaves its wear
   several times wider than tall. Up to about 420 units it fits whole on a 21:9 sheet. Longer, paint
   it as windows of its whitebox, each cut at exactly 21:9 and
   stacked on one sheet (its left end, a stretch of its middle, its right end), with the open sides
   running off the sheet's edge; then assemble the windows on the whitebox's edges and cover the gap
   with the middle window and its mirror image, joined by `seamband.py`'s seams
   (`.data/hud-art/work/m3kit/compose_housing.py` does this for the timeline housing).
   **Paint a part at about the length it is drawn.** A 9-slice drawn far longer than any sheet cell
   (the live strip, 787 units at HUD 150) is painted 420 units long, so the period `seamband.py`
   learns from its middle is long enough that its wear does not read as a rhythm; one drawn at a
   fixed size (the hover strip, 137 units) is painted at that size and does not repeat.
   `.data/hud-art/work/m3all/` is the whole job: `make_sheets.py` draws the layouts, `cut_all.py`
   cuts, repeats and pins slices.
4. **Before cutting**: key out a sheet's labels (everything outside its cells: `.data/hud-art/work/m3kit/unlabel.py`), and scale each painting
   to 10 px per HUD unit, or the one-piece parts outgrow the 8 MB commit limit (the stats window goes in
   at 7: at 10 it is 13 MB). A flat recess painted
   with uneven tone is levelled by column (`.data/hud-art/work/m3kit/level.py`).
5. **Cut and file** with the repository's tools: `gridslice.py` for one-piece parts,
   `PRESSED_DARK=1.5 cut.py` for sheets, `iconcut.py` for icons, then, for every part drawn wider than it was painted, `seamband.py <kit dir> <parts>`
   so its middle repeats instead of stretching, then `file.py <kit dir> <parts>`.
   A 9-slice's fixed ends must fit the shortest size it is drawn at: `cut.py` measures them off the
   wear, so pin them where they do not (the scroll thumb is drawn as short as 10 units).
   Check `tools/hudart/laycheck.py` still ends "laycheck: the two agree" and capture the mockup.

## Prompts

- Name the part and its material concretely, as the style guide does; the chosen look today is
  **worn painted steel**: grey military paint over steel, chipped along every edge and bevel to
  bright bare steel (never rust), short scratches in groups, oily grime in corners and along recess
  rims, crisp bevels lit from the top-left, a thin near-black outline. Buttons a shade lighter than
  plates. Neutral greys, no cyan or teal; never marble, stone, concrete, cracks or veins.
- Every part is **empty**: no text, labels, numbers, icons, badges, fills or playheads. The game
  draws all of those.
- **No screws, rivets, bolts, lamps or glow.** Plain edges: no dark blocks or notches in corners.
- Parts that stretch: "spread the wear evenly along its length, no single large feature".
- A sheet: "exactly N parts, each in its cell"; pressed parts are sunk, with a darker face and an
  inner shadow along the top and left.
- Pure flat `#FF00FF` outside every part (the cutters key it out).

## What must never be committed

- **Total Annihilation's own interface art** (crops of the game's screens, its palette samples):
  it may be passed as a reference, but it is original game art. It lives only under `.data/`.
- The paintings, crops and sessions: the store is `.data/pixel-forge/` in the main checkout, which
  git ignores. Only the cut parts filed into `art/hud/` are committed.
