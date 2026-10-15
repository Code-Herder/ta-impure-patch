---
store: .data/pixel-forge
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
2. **The look is a crop of the chosen painting**, passed as a style reference: the part being made,
   cut out of the full-dock painting the owner picked, upscaled 2x. Use `pixel-forge crop`.
3. **Render each layout with its crops**: `pixel-forge gen --ref <layout>.png:layout --ref
   <crop>.png:style`. Pixel-forge pads a long layout to an aspect the model accepts and crops it
   back, so the result is the layout's full canvas, ready for the cutters.
4. **Cut and file** with the repository's tools: `gridslice.py` for one-piece parts,
   `PRESSED_DARK=1.5 cut.py` for sheets, `iconcut.py` for icons, then `file.py <kit dir> <parts>`.
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
