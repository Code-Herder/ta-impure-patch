# Zoomed-out speed: the plan

The frame rate of a large battle at full zoom-out, on the Vulkan backend. The branch has already moved
the nanoframe wire onto the device and removed the engine's unit draw on a Vulkan launch
([units](modules/units.md), *The engine draws no unit on a Vulkan launch*). This note is the list of
what is left to attack, in order, with the measurement each item rests on.

## How it is measured

The 13 500-unit Total Mayhem battle on Zombie Kingdom (`tools/compat/battle13k.py`), a private display at
1920x1080, the frame profiler with its sampler ([profiler](modules/profiler.md)), windows alternating near
(the scenario's camera) and far (the wheel out to the floor) inside one game. An A/B of a lever that
follows a file live is played inside one game too: two runs of the same battle on a machine whose load
changes between them differ by a factor of two in spans no change touches.

## The items

### 1. The engine's feature, effect and particle draws on a Vulkan launch

`featown` (the feature leaf `0x46A610`) and `fxown` (the projectile pass `0x49BE60`, the explosion pass
`0x420B00`'s leaves, the particle layers' walker `0x471F90`) skip the engine's software draws that the
Vulkan passes replace. Neither is a play default, so a player's launch rasterises every feature, effect
and particle into a frame nothing presents. MEASURED 2026-11-21, the battle, in one game with
`tagpu_fx.on`/`tagpu_sfx.on`/`tagpu_feat.on` flipped to `passive` and back, the spans `sweep` + `markers`:
4 to 8 ms a frame saved (near, busy: 7.7; far, settled: 4.8). The particle layers are the larger half of
it; `markers` is almost all particle layers 8 and 9.

The shape the unit draw took: removed at attach on every Vulkan launch, GDI (the `tagpu_gdi.on` oracle)
keeping every engine draw, no lever.

### 2. The repose's trigonometry, once a piece

What is left of `sweep` once item 1 is in is DrawUnit's repose (96 % far), and four fifths of that is
the rotation `0x4B6CC0`, which calls `0x4B7173` three times a VERTEX, each an x87 `fsincos` of the
piece's angle (only a zero angle is skipped). The angles are the piece's, the same for all its vertices.

The posed vertices are simulation state: SweetSpot `0x43E0B0` takes the box of a piece's posed vertices
as AutoAim's target point (scouted 2026-11-21 from the disassembly; the chain up to the tick
`0x4954ED → 0x48ADDA → 0x49E1E1 → 0x48A2F7 → 0x43E3F4`). So the loops cannot be skipped; they can be
made to give the same bits faster: the three `fsincos` results once a piece, kept at 80 bits (precision
control does not touch `fsincos`, and a tbyte store and load are exact), then the engine's own per-vertex
multiply-add sequence. THE INVARIANT: every vertex the repose writes is bit-identical to the engine's.
The replay test groups are the check.

### 3. The message pump under Wine

`outside`, the time between the engine's frames, is 14 to 23 ms a frame, and 93 % of its samples are
inside one call, WinMain's `PeekMessageA` at `0x49EC47`. Whether that is idle time before the next tick
is due or Wine's own work is not established; in the busy battle, behind schedule at 18 ticks a second,
it is still 15 ms a frame.

### 4. Impure's packet publish

`publish_world` + `publish`, 6 to 8 ms a frame: `fill_world` walks every unit slot each publish,
`fill_pieces` copies every piece of every unit in reach (all of them, zoomed out), and `fx_model` poses
each effect model through `0x4B6CC0`, the trigonometry of item 2 (its fix applies here too).

### 5. The particle update

The `particles` span (the tick's walker, Impure's `particle_walk`), 5 ms a frame settled and 17 busy, and
half of it is the first read of each particle object. A prefetch of the next objects keeps the same calls
in the same order.

## Not on the list

- **The render thread's `gather`** grows from 7 to 18 ms a frame zoomed out; it is not the bottleneck
  while the game thread is, and the in-tree sampler samples only the game thread.
- **GPU time** grows from 1.5 to 11.6 ms a frame (p50) zoomed out at 1080p. The world target is a fixed
  ss=2 size, so it is geometry, not pixels. Not the bottleneck yet; at 4K the target is four times larger.
- **`present`** on the private display is X's software copy of each frame, a cost of the measuring
  display and not of the game.
