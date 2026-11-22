# Zoomed-out speed: the plan

The frame rate of a large battle at full zoom-out, on the Vulkan backend. The branch has already moved
the nanoframe wire onto the device and removed the engine's unit draw on a Vulkan launch
([units](modules/units.md), *The engine draws no unit on a Vulkan launch*). This note is the list of
what is left to attack, in order, with the measurement each item rests on.

## How it is measured

The 13 500-unit Total Mayhem battle on Zombie Kingdom (`tools/battle13k.py`), the frame profiler with its
sampler ([profiler](modules/profiler.md)), windows alternating near (the scenario's camera) and far (the
wheel out to the floor) inside one game. An A/B of a lever that follows a file live is played inside one
game too: two runs of the same battle on a machine whose load changes between them differ by a factor
of two in spans no change touches.

- **The private display inflates `outside`.** Its present is X's software copy of each frame (45 to
  88 ms), and the game thread's `PeekMessageA` at `0x49EC47` waits behind it: 15 to 26 % of the game
  thread's time there, 0.2 % on the reference setup's live display (MEASURED 2026-11-21 with a probe in
  `fake_PeekMessageA`; `outside` 13 to 16 ms a frame on the private display, 0.9 to 2.5 live, same
  battle). Every other span compares across the two.
- **The owner's resolution doubles the engine's draw.** Live at 3840x2160 fullscreen, `--defaults`:
  `sweep` 17.5 ms a frame far and settled (9.5 at 1080p on the private display), 39.8 far and busy
  (20.4), `markers` up by half. Items 1 and 3 are worth about twice their 1080p figures.
- **A lane refusal on a shared card says nothing about the change.** `vkAllocateMemory` answering -2 with
  our `gpumem` line far under its cap is the card full of another program's memory; `nvidia-smi` first.
- **The profiler's game lane installs at attach**: its lever exists before the launch or the game lane
  records nothing. `tacli launch` refreshes `ddraw.dll` from the tree, so a scratch DLL is tested by
  copying it there and putting the tree's back afterwards.

## The items, in order

### 1. The engine's feature, effect and particle draws on a Vulkan launch

`featown` (the feature leaf `0x46A610`) and `fxown` (the projectile pass `0x49BE60`, the explosion pass
`0x420B00`'s leaves, the particle layers' walker `0x471F90`) skip the engine's software draws that the
Vulkan passes replace. Neither is a play default, so a player's launch rasterises every feature, effect
and particle into a frame nothing presents. MEASURED 2026-11-21, the battle at 1080p, in one game with
`tagpu_fx.on`/`tagpu_sfx.on`/`tagpu_feat.on` flipped to `passive` and back, the spans `sweep` + `markers`:
4 to 8 ms a frame saved (near, busy: 7.7; far, settled: 4.8). The particle layers are the larger half of
it; `markers` is almost all particle layers 8 and 9.

BUILT in that shape, the unit draw's: removed at attach on every Vulkan launch, GDI (the `tagpu_gdi.on`
oracle) keeping every engine draw, no lever (`tagpu_featown.c`, `tagpu_fxown.c`). MEASURED 2026-11-21,
the battle at 1080p, the build before and the build after back to back (load 15 and 12.5, so the frame
rates are loose): `sweep` 7.9 → 1.3, 23.7 → 11.2, 5.8 → 0.9 and 10.7 → 5.6 ms a frame (near busy, far
busy, near thin, far thin), `markers` 1.0 to 2.5 → 0.0 to 0.1; game frames 4.6 → 5.1, 6.8 → 10.1,
12.2 → 19.7 and 13.1 → 15.4 a second.

### 2. One world target, not one a frame slot

`tagpu_vk_world.c` keeps a colour and a depth-stencil image per frame slot (`SLOT s_slot[TAGPU_VK_SLOTS]`,
4 slots on the reference setup's live display). At 3840x2160 and ss=2 the target is 7680x4320: 127 MiB of
colour and 253 MiB of depth-stencil (D32_SFLOAT_S8_UINT, laid out at 8 bytes a pixel), so about 1.5 GiB for
the four where one pair is about 380 MiB. Since this branch a lane that is refused its memory ends the
session, so the footprint is the margin a shared card leaves the game (MEASURED 2026-11-21: the battle at
4K refused its second slot at load with a local model holding most of the card, and the session ended
zoomed out).

- **Depth needs no copies**: nothing reads it after the world's render pass.
- **Colour needs one copy and an ordering**: frame N's composite, in frame N's command buffer, is its last
  reader; a barrier at the start of frame N+1's world pass (first scope: the fragment-shader read, second:
  the attachment writes) orders it after every command submitted before it on the one queue. The cost is
  the overlap of N's composite with N+1's world pass.
- **The capture**: `tagpu_vk_world_shot` copies out of the colour after the frame's composite, so the
  copy and its layout transition back must be in the next world pass's first scope too.

BUILT (`tagpu_vk_world.c`): one target; one descriptor set a slot still, since each slot's Gamma curve
is written by the CPU; the render pass's entry dependency carries TRANSFER and the capture's closing
barrier ends at TRANSFER, so the copy chains in; a resize drains the device before the images are given
back. MEASURED 2026-11-21, the battle at 3840x2160 on the private display (3 frame slots there), the
build before and after: the game's GPU memory high water 2576 → 1808 MiB, the card 3904 → 3134 MiB
zoomed out; Supersampling flipped off and on live, two resizes through the drain, no refusal, the
picture whole after both. On the reference setup's live display, 4 slots, that is three pairs, about
1.1 GiB. The cost, the overlap of one frame's composite with the next frame's world pass, is not
measured: the private display's 4K present is a software copy that hides it.

### 3. The repose's trigonometry, once a piece

What is left of `sweep` once item 1 is in is DrawUnit's repose (96 % far), and four fifths of that is
the rotation `0x4B6CC0`, which calls `0x4B7173` three times a VERTEX, each an x87 `fsincos` of the
piece's angle (only a zero angle is skipped). The angles are the piece's, the same for all its vertices.

The posed vertices are simulation state: SweetSpot `0x43E0B0` takes the box of a piece's posed vertices
as AutoAim's target point (scouted 2026-11-21 from the disassembly; the chain up to the tick
`0x4954ED → 0x48ADDA → 0x49E1E1 → 0x48A2F7 → 0x43E3F4`). So the loops cannot be skipped; they can be
made to give the same bits faster: the three `fsincos` results once a piece, kept at 80 bits (precision
control does not touch `fsincos`, and a tbyte store and load are exact), then the engine's own per-vertex
multiply-add sequence. THE INVARIANT: every vertex the repose writes is bit-identical to the engine's.

BUILT as the always-on engine fix `repose_trig` (`tagpu_patches.c`): the vertex loop `0x45B179` jumps to
`repose_rotate`, which takes each nonzero angle's `fsincos` once a piece with the engine's own operands
and control word and runs `0x4B7173`'s per-vertex arithmetic, byte for byte the engine's; the loop,
`0x4B6CC0` and `0x4B7173` are byte-matched first. The oracle `tagpu_reposecheck.on` turns a copy of every
vertex through the engine's `0x4B6CC0` too and compares. MEASURED 2026-11-21 in the battle at 1080p, near
and zoomed out: 201 247 580 vertices of 16 777 216 pieces compared, 0 differ. The build before and after,
back to back (load about 9 both): `sweep` 13.97 → 5.45 ms a frame far and busy, 6.06 → 3.23 far and
thin, near unchanged (1.3, 0.9); game frames 9.2 → 12.2 and 14.7 → 17.4 a second far. The replay test
groups are the landing's check of the outcome.

### 4. Impure's packet publish

`publish_world` + `publish`, 6 to 8 ms a frame at 1080p, 10 to 14 live at 4K: `fill_world` walks every
unit slot each publish, `fill_pieces` copies every piece of every unit in reach (all of them, zoomed out),
and `fx_model` poses each effect model through `0x4B6CC0`, the trigonometry of item 3.

The sampler, zoomed out (MEASURED 2026-11-21, 1080p): the publish is 7.6 % of the game thread (`publish`
4.1, `publish_world` 3.5); a third of `publish` was the engine's trigonometry under `fx_model`;
`publish_world` is `fill_world` 63 % and `fill_pieces` 24 %. BUILT for `fx_model`: item 3's rotation is
one header, `tagpu_turn3.h`, shared by the engine fix and `fx_model`, which takes the triple's trig once
a model. A test build compared every `fx_model` vertex with `0x4B6CC0`'s own answer: 8 388 608, 0 differ;
the repose oracle on the shared header, 99 488 808, 0 differ. `publish` 4.77 → 3.71, 4.62 → 3.16,
3.44 → 2.75 and 1.81 → 1.61 ms a frame (near busy, far busy, near thin, far thin), the build before and
after back to back. `fill_world` and `fill_pieces` are not changed.

### 5. The particle update

The `particles` span (the tick's walker, Impure's `particle_walk`), 5 to 8 ms a frame settled and 17 to 18
busy, and half of it is the first read of each particle object. A prefetch of the next objects keeps the
same calls in the same order.

## After the list

- **The simulation's own spans.** `units` (the engine's per-unit tick) is the largest span while the
  battle is busy: 140 ms a frame at its start, 23 once it thins; `players` 6 to 25, `pathfind` 2 to 8.
  Simulation work, so every change stays replayable; to be scouted before it is planned.
- **GPU time** zoomed out: 11.6 ms a frame (p50) at 1080p, 18 at 4K live (3.8 near). The render thread
  still makes 46 frames a second, so not the bottleneck while the game thread is; item 2 takes some of
  its memory traffic.
- **The render thread's `gather`**: 6.5 ms a frame near, 12 to 15 far; not the bottleneck while the game
  thread is, and the in-tree sampler samples only the game thread.
- **Mesa's Vulkan drivers in the 32-bit process.** The loader maps every installed driver: on the
  reference setup Intel's two, Radeon's and llvmpipe's with `libLLVM`, about 220 MB of the 2 GB, for no
  device the game uses (MEASURED from `/proc/<pid>/maps`, 2026-11-21). Wine on Linux only; not binding
  (about 325 MB unmapped mid-battle).
