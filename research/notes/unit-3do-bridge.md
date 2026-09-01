# The unit → posed-3DO bridge in memory

Goal: given a live `UnitStruct`, reach its **posed 3D model** — piece tree, current COB-driven
piece transforms, and engine-posed vertex buffers — so the GPU renderer can draw real geometry
instead of a marker. This is the Phase-A/G6 read path.

Sources & tagging. `[BINARY]` = confirmed by disassembling the pristine exe this session (VAs
cited); `[TAMEM]` = declared in `vendor/TADR/src/DDraw/tamem.h` (static_assert-checked layouts);
`[LIVE]` = previously verified against the running game. Every `[TAMEM]` offset used below was
re-checked in the binary — **no layout discrepancies found** (UnitStruct stride 0x118 is also
asserted at tamem.h:1944). A Ghidra 12.1.3 project with 613 corpus symbols + these structs in the
data-type manager lives at `tools/ghidra-projects/TA.gpr` (see `tools/GHIDRA.md`).

## The offset chain

Entry (all `[LIVE]`): `main = *(TAdynmemStruct**)0x00511DE8` → units at `*(u32*)(main+0x14357)`,
stride `0x118`, slot 0 dummy; alive = `(*(u32*)(u+0x110) & 0x10000000) && !(… & 0x4000)`.

| Struct | Field | Offset | Type | Meaning | Source |
|---|---|---|---|---|---|
| UnitStruct | body turn | +0x64/66/68 | u16×3 | **+0x64 z-roll, +0x66 y-yaw (heading), +0x68 x-pitch**; binary angle, 65536 = full circle | BINARY (pairing in 0x45AB10→0x45B0A0) |
| UnitStruct | Pos | +0x6A/6E/72 | i32×3 16.16 | world x / altitude / depth (int16 high words at +0x6C/70/74) | TAMEM + BINARY (0x421700, 0x458A7F) |
| UnitStruct | attached-unit list | +0x8A | UnitStruct* | sub/attached-unit chain walked by unit draw (tamem `Unknow_Order`) | BINARY (0x458A14) |
| UnitStruct | UnitType | +0x92 | UnitDefStruct* | `+0x18E` in it = CobHeader* | TAMEM + BINARY 0x485DA5 |
| UnitStruct | **COB context** | **+0x9A** | ptr | 0x544-byte script-VM object, vtable 0x4FD698; `ctx+0x540` = Object3do*, `ctx+0x14` = anim-state table (19 dwords/piece) (tamem `UnkPTR2`) | BINARY (0x485D73..0x485D9F) |
| UnitStruct | **Object3do** | **+0x9E** | Object3doStruct* | the posed model instance | TAMEM + BINARY (0x485DCC write, ~30 read sites) |
| Object3doStruct | NumParts | +0x00 | i32 | piece count | TAMEM + BINARY 0x45AF35 |
| Object3doStruct | TimeVisible | +0x04 | i32 | sprite-cache stamp; zeroed when a bit1-flagged piece moves | TAMEM + BINARY 0x480CA7 |
| Object3doStruct | **pose-dirty flag** | +0x08 | i32 | set 1 by every Set-Position/Angle and on body-turn change; cleared after repose (tamem `data2`) | BINARY (0x480C90, 0x45AC0A) |
| Object3doStruct | ThisUnit | +0x0C | UnitStruct* | back-pointer | TAMEM + BINARY 0x485E14 |
| Object3doStruct | cached body turn | +0x18/1A/1C | u16×3 | copy of unit+0x64/66/68, added to the **root** piece's turns at pose time (tamem `data3`) | BINARY (0x45AB20..73, 0x45B0DB) |
| Object3doStruct | BaseObject | +0x1E | PrimitiveStruct* | root piece = obj3do+0x22 | TAMEM + BINARY 0x45A9B6 |
| Object3doStruct | **inline piece array** | **+0x22** | PrimitiveStruct[NumParts], stride **0x36** | **index = COB piece number** (name-match reordered against the COB header piece table at build) | BINARY (0x480C40, 0x45A985, swap loop 0x45AA1A) |
| PrimitiveStruct | PrimitiveInfo | +0x00 | Model3DONode* | static geometry node | TAMEM + BINARY 0x45AEDD |
| PrimitiveStruct | posed positions | +0x04/08/0C | i32×3 16.16 | COB axis 0/1/2 = **x / y-up / z-depth** (`GetPosition` reads `prim+0x4+axis*4`) | BINARY 0x480C48 |
| PrimitiveStruct | posed turns | +0x10/12/14 | u16×3 | COB axis 0/1/2 = **x-pitch / y-yaw / z-roll** (`GetAngle` reads `prim+0x10+axis*2`); 65536 = 360° | BINARY 0x480CC8 |
| PrimitiveStruct | **posed origin** | +0x16/1A/1E | i32×3 16.16 | piece origin after the full transform chain, **model space** (world = this + unit Pos dwords) | BINARY 0x45B1AC, 0x4216C9 |
| PrimitiveStruct | **posed vertex buffer** | +0x22 | i32* | heap copy, VertexCount×3 dwords 16.16, fully posed, model space | BINARY (alloc 0x45AEF9, transform 0x45B18C) |
| PrimitiveStruct | verts-valid flag | +0x26 | u16 | 0 ⇒ refresh from static verts on next repose | BINARY 0x480C85 |
| PrimitiveStruct | flags | +0x28 | u8 | bit0 Visible (set iff VertexCount≥3; cleared on piece-explode/Hide), bit1 invalidates TimeVisible, bit2 always set | TAMEM + BINARY 0x45AF21/0x421639 |
| PrimitiveStruct | Sibling/Child/Parent | +0x2A/+0x2E/+0x32 | PrimitiveStruct* | tree links (point into the same inline array) | TAMEM + BINARY 0x45ABE5/0x45ABD9 |

`Model3DONode` (0x40 B, `[TAMEM]`, Ghidra-verified): VertexCount +0x04, FaceCount +0x08,
OffsetX/Y/Z +0x10/14/18 (16.16, parent-relative), pNameStr +0x1C, pTextureGAF +0x20,
pVertexArray +0x24 (static verts), pFaceArray +0x28, pSibling +0x2C, pChild +0x30, scriptNo +0x34.
Per unit *type* the static tree is also reachable via `MODEL_PTRS = main+0x14377`
(Model3DONode** indexed by `*(u16*)(u+0xA6)`). `Model3DOFace` (0x20 B): pColorTable +0,
VertexCount +4, pTextureName +8, pVertexIndices +0xC (u16*), GAF texture state +0x10..0x18,
flags +0x1C. Faces are quads/N-gons — triangulate (see file-formats). **Runtime truth of the
colour/texture fields (live-verified, Phase B):** `+0x08` is zeroed after texture resolution;
plain textures leave a pointer to an **inline `GAFFrame[]`** at `+0x10`; the team-colour path
(armcom torso) instead fills `+0x18` with a GAF anim entry whose `+0x28` points at an inline
frame table; faces with nothing resolved (e.g. the `ground` footprint quad) are **not drawn**;
and `+0x00` can hold non-pointer garbage — never dereference it blindly. Details:
[render3do](gpu-render3do.html).

## Pose math `[BINARY]`

From 0x45B0A0 (composer) / 0x45B150 (vertex transformer) / 0x4B6CC0 (rotator): per piece,
angles = (XTurn, YTurn, ZTurn), plus the obj3do cached body turn if the piece is the root.
Rotation order **Rz, then Rx, then Ry**, as 2×2 rotations via the sin/cos helper `0x4B7173`
taking u16 binary angles. Translation = posed position (+0x4/8/C) + node offset (+0x10/14/18).
Children are transformed first; each ancestor then re-applies its transform to the whole
subtree's vertex buffers; results land in prim+0x22 (verts) and prim+0x16 (origin).
Reposing is **lazy**: only when obj3do+0x8 is dirty, during draw-prep of *visible* units
(COB-ctx vtable slot 12 → `0x45AB10`, which is called just before TADR's `DrawUnit 0x45AC20`).
The pos/turn *inputs* are always-current sim state; `vbuf`/`org` are current-as-of-last-repose.

## Key functions `[BINARY]`

- `0x45AB10` — unit→model sync + tree walk: reads unit+0x9E, re-syncs body turn (threshold 8),
  walks Child (+0x2E)/Sibling (+0x2A), recurses via 0x45B030. Called from COB-ctx vtable slot 12
  (0x480EFC).
- `0x45B0A0` / `0x45B150` — pose composer / vertex transformer (details above).
- `0x4B0DA0` — COB VM dispatch (0x4B0E6B `and edx,0x100ff000`): MOVE 0x10001000 @0x4B0E8F,
  TURN 0x10002000 @0x4B0F2C (shortest-path test `cmp eax,0x8000` @0x4B0FC3 ⇒ u16 wraparound),
  SPIN @0x4B100A, MOVE_NOW @0x4B11B6 → vtbl slot 0, TURN_NOW @0x4B1210 → vtbl slot 1.
  Anim-state table `[ctx+0x14]`, index = piece·19+axis.
- COB-ctx virtuals (vtable 0x4FD698): SetPosition 0x480C50, SetAngle 0x480CE0, GetPosition
  0x480C30, GetAngle 0x480CB0 — all compute `[ctx+0x540] + piece*0x36` then `+0x26+axis*4`
  (≡ prim+0x4+axis·4) or `+0x32+axis*2` (≡ prim+0x10+axis·2); clear prim+0x26; set obj3do+0x8=1.
  **This is the definitive piece-index ↔ prim ↔ axis mapping.**
- Unit ctor path: 0x485D73 `push 0x544` (ctx alloc) → 0x485D9F `mov [esi+0x9a],ecx` →
  0x485DC0 `call 0x45A950` (builder) → 0x485DCC `mov [esi+0x9e],eax` → 0x485E14 back-pointer.
  (= TADR's `UNITS_CreateModelScripts 0x485D40`.)
- Builder 0x45A950 / 0x45AEC0: allocs `0x22 + count*0x36`; COB name-match reorder (stricmp
  0x4F8A70 vs node+0x1C) with 0x36-byte swaps @0x45AA1A; per-piece vbuf alloc VertexCount·12.
- Render consumers: `0x4589C0` (TADR `DrawCompsiteBuf_Unit`; takes Object3do*, walks attached
  units via unit+0x8A) and `0x4584D0` (TADR `Render_DrawSpriteGroupUnit`; per-vertex
  `sx=x, sy=−z−y/2`). Piece-explode 0x421620/0x421700 computes world piece pos =
  prim+0x16/1A/1E + unit+0x6A/6E/72.
- 3DO loading (TADR names, catalog-consistent): Load3DO_FromHPI 0x42A2C0, Open3do 0x4CB560,
  Parse3dO 0x4CB590, TextureMatch3DO 0x42A140.

## Renderer read strategies

(a) read per-piece `pos/turn` and compose ourselves — smooth and always fresh, needs the exact
sign conventions of `0x4B7173` (lock empirically); or (b) read the engine-posed `vbuf`/`org` —
zero math and exactly the data TA rasterises into the sprite cache, but only fresh after the
lazy repose of visible units. **(b) is the natural G6 source.**

## Runtime confirmation — CONFIRMED LIVE (2026-08-31)

A read-only probe (`probe_unit_model()` in `tagpu/ddraw/src/tagpu_overlay.c`, logs one alive
unit's full piece tree every 300 frames) ran against a live skirmish. The ARM Commander came
back exactly as the chain predicts — 15 pieces, real 3DO piece names, COB-hidden emit points
invisible, engine-posed origins and vertex buffers populated (full log:
`assets-probe-armcom.txt` beside this note):

```
probe: unit=06380138 obj3do=0528DEB8 base=0528E1CE parts=15 bodyTurn=(0,0,0)
  piece  0 'torso'     vis=1 verts=37 pos=(0,0,0) turn=(0,0,0) org=(0,25,0)   vbuf=026674F0
  piece  1 'ruparm'    vis=1 verts=9  ... org=(9,30,0)      piece  2 'luparm'  org=(-10,30,0)
  piece  3 'rbigflash' vis=0 verts=9  ... org=(11,15,0)     piece  4 'nanospray' vis=0
  piece  5 'pelvis'    vis=1 ... org=(0,22,0)               piece  7 'head'    org=(0,34,0)
  piece  8/9 l/rthigh, 12/13 r/lleg, 10 'nanolath', 11 'biggun', 14 'ground' org=(0,0,0)
```

`base` == obj3do+0x22 as predicted; visibility bit clear on the COB-Hide'd flash/spray pieces;
idle commander shows all turns 0 with bodyTurn cached (0,0,0). The read chain is proven
end-to-end on live memory.

Open questions: exact sin-table sign conventions in 0x4B7173 (needed only for strategy (a));
semantics check that a driving unit changes only +0x66 (heading); the divisor `[ctx+0x4]`
scaling COB speeds; where the per-tick animator consuming `[ctx+0x14]` runs. None block the
G6 read path.
