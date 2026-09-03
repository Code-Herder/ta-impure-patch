# TA file formats — a loader's reference for real models & scripts

Goal: parse **real** Total Annihilation unit assets (`.3do` meshes + `.cob` scripts, with
`.gaf` textures, `.fbi`/`.tdf` stats, all packed in `.hpi`/`.ccx`/`.ufo`) and draw them in a
GPU renderer with correct geometry and animation.

Sources & tagging. `[VERIFIED]` = read this session from a primary parser/spec — chiefly the
**Spring/Recoil** engine loaders (fetched from `raw.githubusercontent.com/spring/spring`,
`rts/Rendering/Models/3DOParser.{h,cpp}` and `rts/Sim/Units/Scripts/CobFile.cpp` +
`CobThread.cpp`), and our vendored **TADR** source
(`vendor/TADR/src/DDraw/tamem.h`, `Gaf.h`, `gaf.cpp`;
`vendor/TADR/src/Recorder/plugins/COB_extensions.pas`,
`.../TAMem/TA_MemoryStructures.pas`). `[CLAIMED]` = well-established community spec/lore
(hpiutil2/HPIView spec, the classic "3DO File Format" doc) that I did not re-read against a
running parser this session — treat exact byte offsets there as "confirm on implement".

Two Spring nuances baked into every "primary" cite below: Spring **negates Z** and applies
**`SCALE_FACTOR_3DO = 1/65536`** on load, so its field semantics are TA's but its handedness
and units are Spring's. Where that matters I say so.

## Summary

- **`.3DO`** is a tree of *objects* (pieces). Each object is a **52-byte header** (all
  `int32`) carrying counts + **absolute file offsets** to: object name, a **vertex array**
  (12-byte `int32` X/Y/Z triples in **16.16 fixed-point**, divide by 65536), a **primitive
  array**, a sibling object, and a child object. Piece position relative to parent lives in
  the header (`XFromParent/YFromParent/ZFromParent`, also 16.16). [VERIFIED]
- **The load-bearing fact for rendering: 3DO primitives are NOT triangles.** A primitive
  (32-byte record) has `NumberOfVertexIndexes` that is typically **4 (a quad)**, can be
  **>4 (arbitrary N-gon)**, and can be **3, 2, or 1** (triangle / line / point). Its
  `OffsetToVertexIndexArray` points at a list of **`uint16`** indices into the object's
  vertex array; its `OffsetToTextureName` points at a GAF texture name (or is `0` → flat
  color, using the `PaletteEntry`/color field as a TA-palette index). GPU wants triangles,
  so every primitive must be **triangulated** (fan for planar-convex; ear-clip for concave).
  [VERIFIED]
- **`.COB`** is compiled bytecode (from `.BOS` text source). A header holds counts + offsets
  to a **script-entry table**, a **script-name table**, and a **piece-name table**; the
  piece names **match the 3DO object names by string**, which is exactly how a script
  animates a specific mesh piece. The bytecode is a **32-bit-word-addressed stack VM**
  (`MOVE/TURN/SPIN/SHOW/HIDE/EMIT_SFX/…`, arithmetic, `GET`/`SET` engine queries). Its
  `MOVE`/`TURN` outputs are the **per-piece translation/rotation** a renderer applies to pose
  the 3DO tree. [VERIFIED] At runtime a unit may only run **eight scripts at once**:
  `0x4B08C0` scans a fixed eight `0xA4`-byte thread records at `cob+0x1C` and returns `-1`
  when they are all busy, and the running count at `cob+0x53C` sits immediately after the
  array, so the eight is a struct layout constant. Anything that blocks — `sleep`,
  `wait-for-turn`, `wait-for-move` — holds a record for its whole duration, and
  `COBEngine_QueryScript` fails **silently** on a full pool (it returns without writing its
  out-parameter, so the caller keeps whatever it had). Budget scripts accordingly; see
  [Extra weapons](extra-weapons.md) snag 10 for what that looks like from the outside.
  [BINARY-VERIFIED]
- **`.GAF`** is the 8-bpp paletted sprite/texture container; 3DO primitives reference frames
  in it by name. **`.HPI`/`.CCX`/`.UFO`/`.GP3`** are the (compressed, optionally XOR-obfuscated)
  archives everything ships inside. **`.FBI`/`.TDF`/`.GUI`/`.OTA`** are TDF text
  (`[Section]{ key=value; }`). **`.TNT`/`.PCX`** are map/image formats (brief).
- Our project **already has the in-memory forms** the running engine uses —
  `Model3DONode`/`Model3DOFace` (the loaded 3DO), `CobHeader`, `PrimitiveStruct` (the posed
  piece) in `tamem.h`. On-disk vs in-memory differ mostly by *offsets → pointers*; the field
  mapping is tabulated per format below.

---

## 0. Shared foundation: axes, fixed-point, and the piece tree

These conventions cut across 3DO and COB; get them right once.

- **Fixed-point, linear (positions, vertex coords, `MOVE`).** Signed `int32` in **16.16**:
  model-space value = `raw / 65536.0`. The same scale unifies 3DO vertex components, the
  3DO header's `*FromParent` piece offsets, and COB `MOVE`/position operands — a vertex
  component of `65536` and a BOS `move … [1.0]` describe the **same distance**. Spring's
  `SCALE_FACTOR_3DO = 1/65536` is exactly this. [VERIFIED] `tamem.h:365-378` also annotates
  the in-memory vertex array as "16.16 fixed-point".
- **Fixed-point, angular (`TURN`/`SPIN`, piece `*Turn`).** TA "angular units" (TAang) where
  **65536 = 360°** (≈182.044 per degree). BOS `turn … <90>` compiles to `90*65536/360 =
  16384`. In-memory piece angles are `uint16` (`PrimitiveStruct.XTurn/ZTurn/YTurn`,
  `tamem.h:1120-1123`). [CLAIMED — canonical BOS→COB scale; consistent with the `Turn`
  word-triples throughout `tamem.h`.]
- **Axis convention.** The 3DO file stores each vertex as **X, Y, Z with Y = up** (vertical);
  X and Z are the ground plane. TA's *engine* transposes names into a "screen" convention
  (X right, Y = map-depth, Z = up) — see the long comment at `tamem.h:1010-1024` warning not
  to "fix" either naming. For import, treat the **file** as Y-up and expect a handedness flip
  to reach a right-handed GL/Vulkan space: Spring does `v.z = -v.z`. [VERIFIED]
- **The piece tree.** Objects form a tree via `OffsetToChildObject` (first child) +
  `OffsetToSiblingObject` (next sibling at same level) — a classic first-child/next-sibling
  encoding. The **root object is the unit body**; children inherit the parent's transform.
  Depth-first traversal order of this tree is the canonical piece order, and it aligns with
  the COB piece-name list. [VERIFIED]

---

## 1. `.3DO` — 3D models  (PRIORITY)

A `.3do` is one **recursive tree of objects** written back-to-back in a single file. There is
no top-level file header — the file simply *is* the root object at offset 0, and every offset
field inside is an **absolute offset from the start of the file**. [VERIFIED
`3DOParser.cpp`]

### 1.1 On-disk object header — 52 bytes, all `int32` LE

Struct name in Spring: `_3DObject`. [VERIFIED `3DOParser.h`]

| Off  | Field                     | Meaning |
|------|---------------------------|---------|
| 0x00 | `VersionSignature`        | Always `1` for TA. |
| 0x04 | `NumberOfVertices`        | Count of vertices in this object's vertex array. |
| 0x08 | `NumberOfPrimitives`      | Count of primitives (polygons/lines/points). |
| 0x0C | `SelectionPrimitive`      | Index of the primitive used as the invisible mouse/selection quad (the "ground plate"). `-1`/absent on many pieces. |
| 0x10 | `XFromParent`             | Piece origin offset from parent, **16.16**. |
| 0x14 | `YFromParent`             | (Y = up) |
| 0x18 | `ZFromParent`             | 16.16 |
| 0x1C | `OffsetToObjectName`      | → NUL-terminated ASCII piece name. |
| 0x20 | `Always_0`                | Reserved. |
| 0x24 | `OffsetToVertexArray`     | → vertex array (see 1.2). |
| 0x28 | `OffsetToPrimitiveArray`  | → primitive array (see 1.3). |
| 0x2C | `OffsetToSiblingObject`   | → next sibling object, or `0`. |
| 0x30 | `OffsetToChildObject`     | → first child object, or `0`. |

Total header size **0x34 = 52 bytes**. To parse: read header at offset `p`; recurse into
`OffsetToChildObject` then `OffsetToSiblingObject`; both `0` terminates that branch. [VERIFIED]

### 1.2 Vertex array — 12 bytes/vertex

`NumberOfVertices` records of three `int32` at `OffsetToVertexArray`:

| Off | Field | Notes |
|-----|-------|-------|
| 0x00 | `x` (`int32`) | 16.16 fixed-point |
| 0x04 | `y` (`int32`) | 16.16, **up** |
| 0x08 | `z` (`int32`) | 16.16 |

Spring: `v *= 1/65536; v.z = -v.z;`. [VERIFIED `3DOParser.cpp`]

### 1.3 Primitive (polygon) record — 32 bytes, all `int32` LE

Struct name in Spring: `_Primitive`. `NumberOfPrimitives` of these at
`OffsetToPrimitiveArray`. [VERIFIED `3DOParser.h`]

| Off  | Field                       | Meaning |
|------|-----------------------------|---------|
| 0x00 | `PaletteEntry`              | **Color** for a flat (untextured) primitive: an index into TA's 256-color `PALETTE.PAL`. Ignored when textured. |
| 0x04 | `NumberOfVertexIndexes`     | **Vertices in THIS primitive** — 1, 2, 3, **4 (typical)**, or **>4 (N-gon)**. |
| 0x08 | `Always_0`                  | Reserved. |
| 0x0C | `OffsetToVertexIndexArray`  | → array of **`uint16`** indices into this object's vertex array (0-based), `NumberOfVertexIndexes` long. |
| 0x10 | `OffsetToTextureName`       | → NUL-terminated GAF texture (frame) name, **or `0`** = untextured/flat-color. |
| 0x14 | `Unknown_1`                 | Per-primitive flags/state (texture/transparency; becomes `Model3DOFace.TexState_*`/`flags` in memory). |
| 0x18 | `Unknown_2`                 | " |
| 0x1C | `Unknown_3`                 | " |

Total **0x20 = 32 bytes**. The `uint16` index list is confirmed both by Spring (reads shorts)
and by `tamem.h:373` (`unsigned short* pVertexIndices`). [VERIFIED]

**Vertices-per-primitive (the key concern).**
- **4 = quad** is the overwhelmingly common case in TA models.
- **>4 = arbitrary N-gon** occurs (roofs, hull panels, discs). Convex and planar in practice.
- **3 = triangle** occurs but is a minority.
- **2 = line**, **1 = point**: legal, used sparingly (thin antennae / point markers / the
  odd effect anchor). Spring treats primitives with `<3` verts as non-mesh and either drops
  them or renders a degenerate; a faithful renderer can emit `GL_LINES`/`GL_POINTS` for them
  or ignore them. [VERIFIED for 1–4 & N-gon existence via Spring's triangulation switch;
  CLAIMED for the exact "line/point" semantics — classic 3DO spec.]

**Color vs texture.** `OffsetToTextureName != 0` → sample the named GAF frame (8-bpp; §3).
`== 0` → fill flat with `PaletteEntry` as a palette index. Spring encodes the flat case as a
synthetic material name `"ta_color" + PaletteEntry`; textured names may get a `"00"` suffix
for team-color atlas lookups. [VERIFIED `3DOParser.cpp`]

**Selection primitive.** When rendering the **root** object, skip primitive index
`SelectionPrimitive` (it is the invisible footprint quad); child objects skip none. [VERIFIED
`3DOParser.cpp` — `excludePrim = (isRoot ? me.SelectionPrimitive : -1)`.] Spring additionally
heuristically drops "BasePlate" quads (large, upward-facing, all verts at `y<=0`).

### 1.4 The triangulation problem (3DO quads/N-gons → GPU triangles)

GPUs draw triangles; 3DO gives you quads and N-gons. For each primitive with `n =
NumberOfVertexIndexes` and index list `idx[0..n-1]`:

- **n == 3** — emit as-is.
- **n == 4 (quad)** — two triangles `(0,1,2)` + `(0,2,3)`. Spring emits exactly these 6
  indices. [VERIFIED]
- **n > 4 (N-gon)** — **triangle fan from vertex 0**: `(0, i, i+1)` for `i = 1..n-2`.
  Spring does this fan. [VERIFIED]

Caveats a real loader must handle:
- **Convex/planar assumption.** TA polys are *generally* planar and convex, so a fan from
  vertex 0 is correct and cheap. **Concave** polygons make a naive fan produce
  overlapping/inverted triangles — detect (cross-product sign changes around the loop) and
  fall back to **ear-clipping**. Rare in stock units but present in some third-party models.
- **Degenerate/duplicate verts.** Collinear or repeated indices yield **zero-area triangles**
  — cull (`|cross| < eps`) so they don't corrupt normals or z-fighting.
- **Normals.** Don't derive the face normal from the first 3 verts (they may be collinear on
  an N-gon); use **Newell's method** over the whole loop for a robust planar normal. TA
  models are **flat-shaded per primitive**, so give every triangle of a primitive the same
  normal (or average shared vertex normals only if you deliberately want smooth shading).
- **Winding / back-face.** Determine front-face winding empirically (load a known unit,
  compare to in-game). Because Spring **negates Z**, it must also flip winding to keep
  outward normals — if you adopt `z = -z`, invert your triangle order (or your cull face)
  to match. Safe default while bringing the pipeline up: **disable back-face culling** and
  use two-sided lighting; enable culling once winding is confirmed.

### 1.5 In-memory forms (what our project already has) and how they relate

The running engine has already parsed the `.3do` into pointer-based structs. Our
`vendor/TADR/src/DDraw/tamem.h` maps them (verified vs Ghidra):

**`Model3DONode`** — the loaded object/piece, **0x40 = 64 bytes** (`tamem.h:380-398`):

| Off  | Field            | On-disk counterpart |
|------|------------------|---------------------|
| 0x00 | `field_0`        | (`VersionSignature`) |
| 0x04 | `VertexCount`    | `NumberOfVertices` |
| 0x08 | `FaceCount`      | `NumberOfPrimitives` |
| 0x0C | `MarkedFaceIdx`  | `SelectionPrimitive` |
| 0x10 | `OffsetX`        | `XFromParent` |
| 0x14 | `OffsetY`        | `YFromParent` |
| 0x18 | `OffsetZ`        | `ZFromParent` |
| 0x1C | `pNameStr`       | resolved `OffsetToObjectName` |
| 0x20 | `pTextureGAF`    | resolved texture GAF handle |
| 0x24 | `pVertexArray`   | resolved `OffsetToVertexArray` (int triples, 16.16) |
| 0x28 | `pFaceArray`     | resolved `OffsetToPrimitiveArray` (`Model3DOFace[]`) |
| 0x2C | `pSibling`       | resolved `OffsetToSiblingObject` |
| 0x30 | `pChild`         | resolved `OffsetToChildObject` |
| 0x34 | `scriptNo`       | (piece index used by COB) |
| 0x38 | `unknown1_38`    | — |
| 0x3C | `unknown2_3c`    | — |

**`Model3DOFace`** — the loaded primitive, **0x20 = 32 bytes** (`tamem.h:368-378`):

| Off  | Field            | On-disk counterpart |
|------|------------------|---------------------|
| 0x00 | `pColorTable`    | `PaletteEntry` resolved to a palette/color-table pointer |
| 0x04 | `VertexCount`    | `NumberOfVertexIndexes` |
| 0x08 | `pTextureName`   | resolved `OffsetToTextureName` (0 → flat color) |
| 0x0C | `pVertexIndices` | resolved `OffsetToVertexIndexArray` (`uint16*`) |
| 0x10 | `TexState_a`     | from `Unknown_1` (GAFSpriteState, 12 bytes across a/b/c) |
| 0x14 | `TexState_b`     | `Unknown_2` |
| 0x18 | `TexState_c`     | `Unknown_3` |
| 0x1C | `flags`          | primitive flag bits |

Relationship in one line: **on-disk `_3DObject`/`_Primitive` become `Model3DONode`/
`Model3DOFace` with every `Offset*`/`*Name`/index resolved to a live pointer**; counts,
piece offsets, and the sibling/child links carry over 1:1. For a standalone GPU renderer you
will **re-parse the on-disk form yourself** (§1.1–1.3, à la Spring/TA3D) — TADR itself never
parses `.3do`; it reuses TA's in-memory loader via HPI file hooks and only *reads* these
structs (e.g. `unitrotate.cpp`, `buildghost.cpp`). [VERIFIED]

The *posed* per-piece transform at runtime is a different struct, **`PrimitiveStruct`**
(0x36 = 54 bytes, `tamem.h:1116-1133`): `XPos/ZPos/YPos` (16.16) + `XTurn/ZTurn/YTurn`
(`uint16` TAang) + a `Visible` bit + `Sibling/Child/Parent` pointers. This is where COB
`MOVE`/`TURN`/`SHOW` write — see §2.6. The root is `Object3doStruct.BaseObject`
(`tamem.h:1099-1109`).

### 1.6 Tools & parsers (3DO)

- **Spring / Recoil engine** (C++, GPL) — `rts/Rendering/Models/3DOParser.{h,cpp}`. The
  cleanest modern primary parser; used for every `[VERIFIED]` offset here.
- **TA3D** (C++, GPL) — full open-source TA reimplementation with an independent 3DO loader
  (`src/ta3d/src/mesh/`); good cross-reference.
- **Upspring** (C++) — model editor that imports/exports `.3do` (and Spring `.s3o`); the
  practical tool for inspecting/authoring.
- **Cavedog "3DO Builder" / "3DO Builder Pro"** (Win32, closed) — the original modelers;
  useful for ground-truthing.
- **`3do2obj` / `obj23do`** and various community converters (C/Python) — quick sanity dumps.
- **Our vendored `tamem.h`** — the authoritative in-memory field map (§1.5).

---

## 2. `.COB` — compiled unit scripts  (PRIORITY)

**BOS vs COB.** `.BOS` = human-readable script **source** (C-like: `piece body, turret;`,
`MoveObject`, `TurnObject`, `EmitSfx`, event functions `Create`, `StartMoving`, `AimPrimary`,
`Killed`, …). **`.COB`** = the **compiled bytecode** shipped in the HPI (`scripts/*.cob`);
the game never reads BOS. Cavedog's **Scriptor** compiles BOS→COB; the engine runs the COB
on a little stack VM, one cooperative thread per active script call. [VERIFIED — opcode set
below; CLAIMED — Scriptor naming.]

### 2.1 On-disk COB header — `int32` LE fields

Field order from Spring's `CCobFile` reader (`CobFile.cpp`), cross-checked against the
in-memory `CobHeader` at `tamem.h:849-861`. All values are **absolute file offsets** unless
noted. [VERIFIED]

| Off  | Field                          | Meaning |
|------|--------------------------------|---------|
| 0x00 | `VersionSignature`             | `4` for retail TA. TA:Kingdoms differs and adds the two sound fields below. |
| 0x04 | `NumberOfScripts`              | Count of script functions (`Create`, `StartMoving`, …). |
| 0x08 | `NumberOfPieces`               | Count of model pieces the script addresses. |
| 0x0C | `TotalScriptLen`               | Length of the bytecode, **in 32-bit words** (see 2.3). |
| 0x10 | `NumberOfStaticVars`           | Count of unit-persistent static variables. |
| 0x14 | `Unknown_2` / `Always_0`       | Reserved. |
| 0x18 | `OffsetToScriptCodeIndexArray` | → `NumberOfScripts` × `int32`: each script's **entry point** (a **word index** into the code, not a byte offset). |
| 0x1C | `OffsetToScriptNameOffsetArray`| → `NumberOfScripts` × `int32`: each → a NUL-terminated **script name**. |
| 0x20 | `OffsetToPieceNameOffsetArray` | → `NumberOfPieces` × `int32`: each → a NUL-terminated **piece name**. |
| 0x24 | `OffsetToScriptCode`           | → start of the bytecode (array of `int32` words). |
| 0x28 | `Unknown_3`                    | Usually points at the first script name. |
| 0x2C | `OffsetToSoundNameArray`       | **TA:Kingdoms only.** |
| 0x30 | `NumberOfSounds`               | **TA:Kingdoms only.** |

Retail-TA header is 11 dwords (through `Unknown_3`), i.e. **0x2C = 44 bytes** before the
tables/code; strings live back-to-back in a name area the offset tables point into. [VERIFIED]

### 2.2 The piece-name list ↔ 3DO (why scripts can animate the model)

`OffsetToPieceNameOffsetArray` yields `NumberOfPieces` NUL-terminated strings — e.g.
`base`, `turret`, `barrel`, `flare`, `wheel1`. These **must string-match the 3DO object
names** (`OffsetToObjectName`). At unit load the engine binds each COB **piece index**
(its position in this list) to the `Model3DONode` of the same name. So when bytecode says
`MOVE (piece 3) …`, "piece 3" is the 4th name here, which resolves to a concrete mesh piece.
**This name binding is the entire bridge between script and model** — a renderer that wants
COB-driven animation must reproduce it. [VERIFIED — piece-name table in header; binding is
how `MOVE`/`TURN` take a piece operand.]

### 2.3 The bytecode VM

- **Word-addressed.** The code section is an array of `int32` little-endian **words**. Script
  entry points (2.1) and all jump targets are **word indices** into this array, never byte
  offsets. `TotalScriptLen` counts words. [VERIFIED — Spring stores `std::vector<int> code`.]
- **Stack machine.** A per-thread operand stack; `PUSH_*` push, arithmetic/compare pop two &
  push one, object opcodes pop their numeric args. **Local variables** and **static
  variables** (`NumberOfStaticVars`, persist for the unit's life) are addressed by index.
- **Operand encoding.** Most opcodes are one word; some read **inline operand words** that
  follow: object opcodes read a **piece index** inline (then pop distance/speed/axis from the
  stack); `PUSH_CONSTANT` reads the constant inline; `PUSH_STATIC`/`POP_STATIC` and the
  local-var ops read a var index inline; `JUMP`/`JUMP_NOT_EQUAL` read a target word inline;
  `CALL`/`START` read a script index (+ arg count) inline.
- **Cooperative threading.** `START` spins a new thread; `WAIT_TURN`/`WAIT_MOVE`/`SLEEP`
  block the *calling* thread until an animation on a piece/axis finishes or a timer elapses;
  `SIGNAL`/`SET_SIGNAL_MASK` kill sibling threads (used to cancel a walk cycle when a unit
  stops). Linear operands are 16.16, angular are TAang (§0). [VERIFIED opcodes; mechanics per
  Spring `CobThread`.]

### 2.4 Opcode set — exact values

Every opcode is a full **32-bit word**. Values from Spring `CobThread.cpp`. [VERIFIED]

**Model / piece animation**

| Opcode | Value | Effect |
|--------|-------|--------|
| `MOVE`       | `0x10001000` | Move piece along an axis to an offset at a speed (interpolated). |
| `TURN`       | `0x10002000` | Rotate piece about an axis to an angle at a speed. |
| `SPIN`       | `0x10003000` | Continuously spin piece about an axis (accelerating to a target rate). |
| `STOP_SPIN`  | `0x10004000` | Decelerate/stop a spin. |
| `SHOW`       | `0x10005000` | Make piece visible. |
| `HIDE`       | `0x10006000` | Make piece invisible. |
| `CACHE`      | `0x10007000` | (render-cache hint; largely inert) |
| `DONT_CACHE` | `0x10008000` | " |
| `MOVE_NOW`   | `0x1000B000` | Instant move (no interpolation). |
| `TURN_NOW`   | `0x1000C000` | Instant rotate. |
| `SHADE`      | `0x1000D000` | Enable shading on piece. |
| `DONT_SHADE` | `0x1000E000` | Disable shading. |
| `EMIT_SFX`   | `0x1000F000` | Emit an effect (smoke/fire/muzzle flash/wake) from a piece — the render-facing effect hook. |

**Blocking**

| Opcode | Value |
|--------|-------|
| `WAIT_TURN` | `0x10011000` |
| `WAIT_MOVE` | `0x10012000` |
| `SLEEP`     | `0x10013000` |

**Stack / variables**

| Opcode | Value |
|--------|-------|
| `PUSH_CONSTANT`    | `0x10021001` |
| `PUSH_LOCAL_VAR`   | `0x10021002` |
| `PUSH_STATIC`      | `0x10021004` |
| `CREATE_LOCAL_VAR` | `0x10022000` |
| `POP_LOCAL_VAR`    | `0x10023002` |
| `POP_STATIC`       | `0x10023004` |
| `POP_STACK`        | `0x10024000` |

**Arithmetic / bitwise**

| Opcode | Value | | Opcode | Value |
|--------|-------|---|--------|-------|
| `ADD` | `0x10031000` | | `BITWISE_AND` | `0x10035000` |
| `SUB` | `0x10032000` | | `BITWISE_OR`  | `0x10036000` |
| `MUL` | `0x10033000` | | `BITWISE_XOR` | `0x10037000` |
| `DIV` | `0x10034000` | | `BITWISE_NOT` | `0x10038000` |
| `MOD` | `0x10034001` | | | |

**Native / queries**

| Opcode | Value | Effect |
|--------|-------|--------|
| `RAND`           | `0x10041000` | Random in range. |
| `GET_UNIT_VALUE` | `0x10042000` | Query with a unit-id param. |
| `GET`            | `0x10043000` | Engine query by value-id (§2.5) → pushes result. |
| `SET`            | `0x10082000` | Set an engine/unit property by value-id. |
| `ATTACH`         | `0x10083000` | Attach a unit (transport). |
| `DROP`           | `0x10084000` | Detach a unit. |

**Comparison → push 0/1**

| Opcode | Value | | Opcode | Value |
|--------|-------|---|--------|-------|
| `SET_LESS`             | `0x10051000` | | `SET_GREATER_OR_EQUAL` | `0x10054000` |
| `SET_LESS_OR_EQUAL`    | `0x10052000` | | `SET_EQUAL`            | `0x10055000` |
| `SET_GREATER`          | `0x10053000` | | `SET_NOT_EQUAL`        | `0x10056000` |

**Logical**

| Opcode | Value |
|--------|-------|
| `LOGICAL_AND` | `0x10057000` |
| `LOGICAL_OR`  | `0x10058000` |
| `LOGICAL_XOR` | `0x10059000` |
| `LOGICAL_NOT` | `0x1005A000` |

**Flow control**

| Opcode | Value | Effect |
|--------|-------|--------|
| `START`          | `0x10061000` | Start a script in a new thread. |
| `CALL`           | `0x10062000` | Call a script (blocking). |
| `REAL_CALL`      | `0x10062001` | (variant) |
| `LUA_CALL`       | `0x10062002` | (Spring extension — not in retail TA) |
| `JUMP`           | `0x10064000` | Unconditional jump (inline word target). |
| `RETURN`         | `0x10065000` | Return from script. |
| `JUMP_NOT_EQUAL` | `0x10066000` | Pop; jump if zero-flag not equal (the `if`/loop primitive). |
| `SIGNAL`         | `0x10067000` | Send a signal (kills threads whose mask matches). |
| `SET_SIGNAL_MASK`| `0x10068000` | Set this thread's signal mask. |

**Effects / misc**

| Opcode | Value |
|--------|-------|
| `EXPLODE`    | `0x10071000` | Detonate a piece (death animation debris). |
| `PLAY_SOUND` | `0x10072000` |

### 2.5 `GET`/`SET` value IDs (engine queries)

`GET`/`SET` take a **value-id** selecting what to read/write. **Standard retail-TA IDs are
1–20** (below); community engines & mods extend from **21 up** (our vendored
`COB_extensions.pas` defines the extension range — it explicitly sets `CUSTOM_LOW =
WEAPON_AIM_ABORTED = 21`, so 1–20 are the original set). [CLAIMED for the 1–20 list — classic
TA/Spring `CobInstance` enum; VERIFIED that 21+ are extensions, `COB_extensions.pas:22-186`.]

Standard (1–20): `ACTIVATION`(1), `STANDINGMOVEORDERS`(2), `STANDINGFIREORDERS`(3),
`HEALTH`(4), `INBUILDSTANCE`(5), `BUSY`(6), `PIECE_XZ`(7), `PIECE_Y`(8), `UNIT_XZ`(9),
`UNIT_Y`(10), `UNIT_HEIGHT`(11), `XZ_ATAN`(12), `XZ_HYPOT`(13), `ATAN`(14), `HYPOT`(15),
`GROUND_HEIGHT`(16), `BUILD_PERCENT_LEFT`(17), `YARD_OPEN`(18), `BUGGER_OFF`(19),
`ARMORED`(20).

These are exactly the hooks mods exploit without engine patches — e.g. TA:ESC/TA Zero
implement "shields" purely in COB by differencing `get HEALTH` and toggling `set ARMORED`
(see `deep-ta-esc.md`, `_index.md`). Extension IDs of note from `COB_extensions.pas`:
`UNITX/UNITZ/UNITY`(100–102), `TURNX/TURNZ/TURNY`(103–105), `HEALTH_VAL`(107),
`ATTACKER_ID`(134), `CREATE_UNIT`(151), `KILL_THIS_UNIT`(152) — all TADR/ProTA-era additions,
not stock. [VERIFIED extension IDs.]

### 2.6 How COB poses the 3DO tree (the render bridge)

Per simulation tick the engine steps every active COB thread; `MOVE`/`TURN`/`SPIN`/`MOVE_NOW`/
`TURN_NOW` update the addressed piece's runtime transform, and `SHOW`/`HIDE` its visibility.
Those land in `PrimitiveStruct` (`tamem.h:1116-1133`):

| `PrimitiveStruct` field | Off | Written by | Units |
|-------------------------|-----|------------|-------|
| `XPos`,`ZPos`,`YPos`    | 0x04/0x08/0x0C | `MOVE`/`MOVE_NOW` | 16.16 (add to the 3DO's static `OffsetX/Y/Z`) |
| `XTurn`,`ZTurn`,`YTurn` | 0x10/0x12/0x14 | `TURN`/`SPIN`/`TURN_NOW` | `uint16` TAang (65536 = 360°) |
| `Visible` (bit)         | 0x28 | `SHOW`/`HIDE` | 1 = drawn |

**Renderer recipe:** for each piece build a local matrix
`L = T(OffsetX+XPos, OffsetY+YPos, OffsetZ+ZPos) · R(XTurn,YTurn,ZTurn)` (mind TA's axis
order/handedness, §0), compose down the child/sibling tree (`world = parent.world · L`), and
draw the piece's triangulated primitives at `world`, skipping pieces whose `Visible` bit is
clear. Static display (no scripting) = all `*Pos/*Turn = 0`, `Visible = 1`. Animated display =
run the COB VM (or, in-process, read the engine's already-updated `PrimitiveStruct`s straight
out of `Object3doStruct.BaseObject`). [VERIFIED struct/field map; recipe is the standard
scene-graph application.]

### 2.7 Tools & parsers (COB/BOS)

- **Spring / Recoil** (C++) — `rts/Sim/Units/Scripts/CobFile.cpp` (header/tables),
  `CobThread.cpp` (opcode VM), `CobInstance.{h,cpp}` (`GET`/`SET`). Primary source for §2.
- **TA3D** (C++) — independent COB VM (`src/ta3d/src/scripts/`).
- **Scriptor** (Cavedog, Win32) — the reference **BOS→COB** compiler; **"Cobbler"/"scriptor"
  clones** and modern reimplementations exist.
- **COB decompilers** — community `cob`→pseudo-BOS tools (e.g. the "vangelis"/"spidermonkey"
  lineage) for reading shipped scripts.
- **Our vendored TADR** — `COB_extensions.pas`, `ScriptCallsExtend.pas` (adds `GET`/`SET` IDs
  by hooking TA's COB call dispatcher `0x4FD6xx`), `CobHeader` in `tamem.h`.

---

## 3. `.GAF` — texture / sprite / animation container  (secondary)

8-bpp paletted sprite frames; 3DO primitives name individual GAF frames as textures, and the
UI/effects use GAF sequences. Struct names from `vendor/TADR/src/DDraw/Gaf.h`. [VERIFIED
header/frame layout & decompression]

**File header (12 bytes)** — `Gaf.h:5-10`:

| Off | Field | Value |
|-----|-------|-------|
| 0x00 | `Signature` (`u32`) | `0x00010100` (version marker). |
| 0x04 | `Entries` (`u32`)   | Number of entries (sequences). |
| 0x08 | `Always0` (`u32`)   | Reserved. |

Then `Entries` × `u32` **offsets to entry structures**. Each **entry** = `u16 Frames`,
`u32 Signature(=1)`, `u16 Padding`, `char Name[32]`, then `Frames` × **frame-entry** pairs
(`{ u32 offsetToFrameData; u32 flag }`; `flag` 2/5 = animated, 10 = fixed). [VERIFIED
`_GAFSequence`/`GAFENTRY`, `Gaf.h:13-26` — note the in-memory struct has pointers where the
file has offsets.]

**Frame data header (per frame)** — `_GAFFrame`, `Gaf.h:28-40`:

| Off | Field | Notes |
|-----|-------|-------|
| 0x00 | `Width` (`u16`) | |
| 0x02 | `Height` (`u16`) | |
| 0x04 | `xPosition` (`u16`) | hotspot/anchor X |
| 0x06 | `yPosition` (`u16`) | hotspot/anchor Y |
| 0x08 | `Background`/transparency index (`u8`) | palette index treated as transparent |
| 0x09 | `Compressed` (`u8`) | 0 = raw 8-bpp; nonzero = RLE (below) |
| 0x0A | `FramePointers` (`u16`) | number of sub-frames (>0 → this "frame" is a stack of sub-frames) |
| 0x0C | `Unknown0` (`u32`) | |
| 0x10 | `PtrFrameBits` | offset→pixel data (pointer in memory) |
| 0x14 | `FPS`/unknown (`u32`) | |

**Pixels** are **8-bit palette indices** into TA's shared `PALETTE.PAL` (256 RGB entries).
**RLE decompression** (verified from `gaf.cpp` `CopyGafToBits`): each **row** starts with a
`int16 bcount` (compressed byte count for the row), then mask-led runs until `bcount`
consumed:
- `mask & 0x01` → **transparent run**: skip `(mask >> 1)` pixels (fill background).
- `mask & 0x02` → **repeat run**: write the next single byte `(mask >> 2) + 1` times.
- else → **literal run**: copy the next `(mask >> 2) + 1` bytes verbatim.

Uncompressed frames are just `Width×Height` bytes, row-major. [VERIFIED `gaf.cpp:200-256`]

**For the renderer:** resolve each 3DO primitive's `pTextureName` to a GAF frame, expand it to
RGBA via the palette (treat `Background` index as alpha 0), and upload as a texture (an atlas
of a unit's texture GAFs is convenient). Team color is handled by a reserved palette band
that TA remaps per player — replace those at texture-build time. The band is **indices
105–110**: that is what ARM's own colour patches (`colorsmd`, `colorsdk`, `colordk2` in
`textures/logos.gaf`) are painted with. [VERIFIED — pixel histogram; an earlier revision of
this note guessed ~216–223.]

Tools: **Spring/Recoil** GAF loader; **TA3D**; Cavedog **"GAF Builder"**; community
**"gaf2png"/"TAGaf"** converters; **`gafbuilder`** editors.

---

## 4. `.FBI` / `.TDF` / `.GUI` / `.OTA` — TDF text formats  (secondary)

All are **TDF** ("Text Data File"): nested sections `[' NAME ']{ key=value; … [SUB]{…} }`,
`//` line comments, **case-insensitive** keys, values untyped (parsed as int/float/string on
demand). [CLAIMED — canonical TDF grammar; VERIFIED that our target engine parses them with
`TdfFile::GetInt/GetFloat/GetString` at `0x4C46C0/0x4C4760/0x4C48C0`, per `_index.md` — the
exact hook mods use to add new keys without a parser.]

- **`.FBI`** — unit stats. One `[UNITINFO]{…}` section: `UnitName`, `Objectname` (→ the `.3do`
  file), `Side`, `BuildCostMetal/Energy`, `MaxDamage`, `MaxVelocity`, `TurnRate`, weapon
  slots (`Weapon1/2/3`), category masks, etc. Maps to `UnitDefStruct` (`tamem.h:863-968`,
  size 0x249) — note `ObjectName[0x20]` at 0x80 is the model name and `cobDataPtr` at 0x18E
  the loaded script. **`Objectname` is the join key from unit → 3DO.**
- **`.TDF`** — general config: `sidedata.tdf` (sides/colors), `weapons/*.tdf`
  (`[WeaponName]{…}`), `gamedata/*.tdf`, `downloads.tdf`, `*.tdf` mod overlays.
- **`.GUI`** — panel/gadget layouts (`[GADGET0]{ COMMON{…} …}` + typed gadget records);
  parsed into the `_GUIxIDControl` structs in `tamem.h`. The build menu is *data* here, not
  code (`_index.md`).
- **`.OTA`** — per-map metadata (`[GlobalHeader]{…}`, start positions, wind, tidal, gravity,
  minerals), pairs with the `.TNT` heightmap.

Tools: **TA3D** TDF parser; the engine's own `TdfFile` (our project); **"TA Unit Builder"/
"Unit Editor"**, **"SelfEd"**, **"Download Builder"** (Win32); trivial to parse fresh.

---

## 5. `.HPI` / `.UFO` / `.CCX` / `.GP3` — archives  (secondary)

Everything (`units/*.fbi`, `objects3d/*.3do`, `scripts/*.cob`, `textures/*.gaf`,
`unittextures/*.gaf`, `anims/*.gaf`, `maps/*.tnt|.ota`) ships inside these. **`.UFO`, `.CCX`,
`.GP3`, `.GPF` are the *same* HPI container** under different extensions/roles (`.ccx` =
Cavedog content packs, `.ufo` = third-party/mod packs, `.gp3` = official patch e.g.
`rev31.gp3`). TA loads all of them from the game dir and **later-overrides-earlier**
(roughly: `.hpi` < `.ccx` < `.gp3`/`.ufo`, then alphabetical), so a mod's `.ufo` can shadow a
stock `.3do`. On a Steam install expect `totala1.hpi`…`totala4.hpi` (core),
`tactics*.hpi`/`ccdata.ccx`/`cctextures.ccx` (The Core Contingency), plus `*.ufo`/`*.ccx`
mods. [CLAIMED — load-order lore; VERIFIED that TADR reaches these via TA's `fopen_HPI`
family, `taHPI.h`.]

**HPI structure** (hpiutil2/HPIView spec — `[CLAIMED]`, confirm offsets against `hpiutil2`
source when implementing):

- **Header (20 bytes):** `char Marker[4]="HAPI"` (0x49504148); `char Version[4]` — `0x00010000`
  for standard TA, the ASCII `"BANK"` for saved games, TA:Kingdoms uses another; `u32
  DirectorySize`; `u32 HeaderKey` (0 = not obfuscated); `u32 Start` (offset to the directory).
- **Whole-archive obfuscation:** if `HeaderKey != 0`, derive
  `key = ~((HeaderKey*4) | (HeaderKey>>6))`; each byte at file position `p` is
  `plain = (char)(((p ^ key) & 0xFF) ^ ~cipher)`. (`HeaderKey == 0` → stored plain.)
- **Directory:** at `Start`, `{ u32 NumEntries; u32 EntryArrayOffset; }`; each entry
  `{ u32 NameOffset; u32 DataOffset; u8 Flag; }` — `Flag==1` → sub-directory (recurse at
  `DataOffset`), else file, where `DataOffset` → `{ u32 FileDataOffset; u32 DecompressedSize;
  u8 CompressionType (0 none / 1 LZ77 / 2 ZLib); }`.
- **Compressed files** are split into **64 KiB decompressed chunks**; the file data begins
  with a `u32[nChunks]` size table, then each chunk carries a **`SQSH` header**
  `{ u32 Marker="SQSH"; u8 Version; u8 CompMethod(1 LZ77 / 2 ZLib); u8 Encrypt; u32
  CompressedSize; u32 DecompressedSize; u32 Checksum; }` followed by `CompressedSize` bytes.
  If `Encrypt`, each byte `x` at chunk index `i` is `(unsigned char)((x - i) ^ i)`.
- **LZ77 (method 1):** LZSS, 4096-byte sliding window; tag byte of 8 flags, LSB→MSB: **clear**
  = copy literal byte (and into window); **set** = 2-byte back-ref, `val = lo | (hi<<8)`,
  `offset = val >> 4`, `count = (val & 0x0F) + 2`; `offset == 0` ends the stream.
  [VERIFIED — this polarity, and *only* this one, decodes the shipped archives: `ARMPW.3DO`'s
  first tag byte is `0x74`, and the other reading asks for a back-reference before anything is
  in the window. An earlier revision of this note had the two swapped; see
  [model export](model-export.md).]
  **ZLib (method 2):** raw `inflate`.

**To extract a `.3do`/`.cob`:** open archive → walk directory to the path
(`objects3d/ARMCOM.3do`, `scripts/ARMCOM.cob`) → read its entry → per-chunk decompress
(LZ77/inflate) into the `DecompressedSize` buffer → feed to §1/§2. [CLAIMED structure /
VERIFIED intent.]

Tools: **HPIView** (Joe D, Win32 GUI — the classic extractor); **hpiutil / hpiutil2** (C,
CLI — the reference implementation these offsets come from); **Spring/Recoil** `HpiArchive`
(C++); **TA3D** `hpi.cpp` (C++); python HPI libs (`pyhpi`/`ta-tools`); **"HPIPack"** to
repack. `.ccx`/`.ufo` open with the same tools by extension-agnostic sniffing of `HAPI`.

---

### What the engine actually checks when it mounts an archive (2026-09-02, live)

Learned while writing `tools/hpipack.py` (a writer *and* reader for this container),
by watching which archives `InitTAHPIAry` (`0x41D4C0`) accepts:

- **The last 36 bytes of the file must be the literal
  `Copyright 1997 Cavedog Entertainment`** (`1998` in the later packs; the year is
  patched in from a 4-byte string at `0x50390C` before the compare). The open routine
  (`0x4BDD70`) seeks to the end and `strcmp`s; anything else is not an archive, silently.
  It is read raw — outside the whole-file XOR. Every third-party HPI tool appends it;
  a hand-rolled writer that forgets it produces a file every *reader* accepts and the
  *engine* ignores.
- Header key 0 is fine (`rev31.gp3` ships with it); the XOR key is
  `~((k*4)|(k>>6))` applied to every byte after the 20-byte header.
- File payloads are SQSH chunks: `"SQSH" ver=2 method enc=1 csize dsize sum`, body
  per-byte `((plain ^ i) + i)`, `sum` = the stored (encrypted) body bytes. Method 1
  (LZSS, 4 KiB window) is what every Cavedog `.hpi`/`.ufo` uses, method 2 (zlib) what
  `rev31.gp3` and `ccdata.ccx` use; both are read by the same code. A literal-only
  method-1 stream (tag `0x00` + eight bytes, terminated by a set bit with offset 0)
  is a valid, uncompressed encoding.
- **Loose `units\*.fbi` files are found and then rejected.** The menu-time loader
  (`0x42A8D0`) opens the disk copy first, but after the `Version` check it tests
  `0x4BB650(handle)` ("came from an archive") and drops the type when it did not
  (with `0x50289C`/`0x511DE4` nonzero — the stock case). So a loose FBI does not
  override the archived one; it makes the unit vanish. Ship overrides in a `.ufo`.
- Archive precedence for *duplicate* paths was not established (a `.ufo`, `.ccx`,
  `.gp3` and `.hpi` copy of `units/ARMPW.fbi` all lost to `totala1.hpi`'s in the
  same session, but those tests ran before the trailer fix and are not conclusive).
  New unit names in a `.ufo` work; that is what the extra-weapons fixtures use.

## 6. `.TNT` / `.PCX` — maps & images  (brief)

- **`.PCX`** — standard ZSoft PCX (8-bpp, RLE), used for logos/loading screens and some UI;
  ordinary PCX decoders work.
- **`.TNT`** — the map terrain file: a header (map tile dimensions, tileset count, offsets to
  the tile-graphics block, the tile-index map, height data, minimap, features), a set of
  **32×32×8-bpp** tile bitmaps, and a 2-D array of tile indices. Our project reads a live
  `TNTHeaderStruct` via `TNTtoMiniMap` (`vendor/TADR/src/DDraw/MegamapTAStuff.cpp:155-378` —
  `tnt->tiles`, `tnt->PTRtilegfx`, `tnt->PTRmapdata`). Pairs with the map's `.OTA` (§4).
  Out of scope for unit rendering; documented here for completeness. [VERIFIED that TADR
  consumes those TNT fields; CLAIMED for the full on-disk TNT header layout.]

Tools: **TA3D** map loader; **"Annihilator"/"TA Map Editor"**; Spring map converters.

---

## Implications for our GPU renderer

**Pipeline to put a real unit on screen:**

1. **Mount archives.** Open `totala*.hpi` + expansion `.ccx` + any `.ufo`/`.gp3` with the
   override order (§5). Build one virtual filesystem so `objects3d/<Objectname>.3do`,
   `scripts/<name>.cob`, and `*textures/*.gaf` resolve through it.
2. **Resolve the unit.** Parse `<unit>.fbi` (§4) → `Objectname` gives the `.3do`, the FBI
   base name gives the `.cob`. Load `PALETTE.PAL`.
3. **Parse the 3DO (§1).** Recurse the object tree from offset 0; for each object read the
   52-byte header, the 12-byte vertex triples, and the 32-byte primitives with their `uint16`
   index lists and texture names. Keep the child/sibling links and the `*FromParent` offsets.
4. **Triangulate (§1.4).** Per primitive: `n==3` direct; `n==4` two-tri; `n>4` fan from vert 0
   (ear-clip if concave). Cull zero-area tris. Compute a per-primitive normal (Newell) for
   flat shading. Decide winding once and set cull face accordingly.
5. **Resolve textures (§3).** For each primitive: `pTextureName==0` → flat material from the
   `PaletteEntry` palette color; else sample the named GAF frame, expanded palette→RGBA
   (transparency index → alpha 0, team-color band remapped). Batch into an atlas per unit.
6. **Build the scene graph.** One node per 3DO piece; local matrix from `*FromParent` (+
   runtime `PrimitiveStruct.*Pos/*Turn` if animating). Upload interleaved vertex buffers
   per piece (or a merged buffer with per-vertex piece id for skinning-free posing).
7. **Pose (§2.6).** Static: identity per-piece deltas, all visible. Animated: either run a COB
   VM (parse §2.1 header, bind piece names → nodes §2.2, interpret §2.3–2.5 bytecode) or, when
   running in-process against the live game, read the engine's already-updated
   `PrimitiveStruct`/`Object3doStruct` tree directly.

**Specific gotchas (do not skip):**

- **Quads/N-gons, not triangles.** The single biggest correctness trap. Every primitive is
  `NumberOfVertexIndexes`-gon; you MUST triangulate, and handle 1/2-vertex primitives (skip or
  draw as points/lines) so they don't crash a triangle-only path.
- **16.16 fixed-point scale.** Divide vertex components AND piece `*FromParent` AND COB `MOVE`
  operands by **65536**. Angles are a *different* fixed-point (**65536 = 360°**) — don't reuse
  the linear divisor on `TURN`/`SPIN`.
- **Y-up vs your engine.** The 3DO file is **Y-up**; the running TA engine transposes axis
  *names* (`tamem.h:1010-1024`) — keep file-space and engine-space straight. If you adopt
  Spring's `z = -z`, also flip triangle winding.
- **Winding / back-face.** Establish front-face winding against a known unit before enabling
  culling; until then cull off + two-sided lighting.
- **Per-primitive color vs per-vertex.** TA lights **per primitive (flat)**, not per vertex,
  and untextured faces are a *single palette index*, not an RGB — resolve through the palette,
  and don't smooth-shade across primitive boundaries unless you intend to.
- **Textured vs flat in one mesh.** A single piece mixes textured and flat-colored primitives;
  keep two material paths (sampled GAF vs solid palette color) and don't assume a UV set
  exists for flat primitives.
- **Piece-name binding is the animation contract.** COB addresses pieces by an index into its
  own name list, bound by string match to 3DO object names — if your loader renames or
  reorders pieces, animation silently targets the wrong mesh.
- **Selection/base-plate primitives.** Skip the root's `SelectionPrimitive` (invisible
  footprint quad) and consider dropping large upward-facing ground plates, or they draw as a
  flat slab under the unit.

**What we already have vs. must build.** In-memory field maps for the loaded model
(`Model3DONode`/`Model3DOFace`), the posed piece (`PrimitiveStruct`/`Object3doStruct`), the
COB header (`CobHeader`) and unit def (`UnitDefStruct`) are done in `tamem.h`. **Missing** =
a standalone **on-disk** parser stack (HPI reader → 3DO parser + triangulator → GAF/palette
decoder → COB VM). Fastest path: port Spring/Recoil's `3DOParser`, `CobFile`/`CobThread`, GAF
loader, and TA3D's `hpi.cpp` — all GPL C++, all cited above.
