# Factories: building, carrying, and letting go

*Scope: how TA attaches a unit to another unit, how a carried unit is drawn, and what
happens the moment it is released. Written while fixing the nanoframe passes (G13l) and the
report "once a unit is built, if it exits at the top, you can see it go UNDER the build place
of the plant". Everything here is either disassembly of the pristine Steam build
(`objdump -d -M intel`, image base `0x400000`) or a live measurement, marked as such. It is
meant to grow: transports, the build sequence itself, and the COB side are all thin here.*

---

## Summary — the one thing to know

**A factory does not draw the unit it is building.** It *carries* it. The unit is attached to
the factory through a general carry relationship — the same one transports use — and the
blit **merges the child's sprite into the parent's**, per pixel, by depth. Nothing sorts the
child against the parent, because as far as the draw is concerned there is only one sprite.

When the factory's script lets go, the unit becomes an **ordinary sprite sorted by its tile
row**, and that transition is abrupt. It is also *stock behaviour*, verified against the
unpatched renderer (§5).

---

## 1. The carry relationship — the fields

All offsets are into `UnitStruct`. [BINARY-VERIFIED at `0x48AB70`, this project, 2026-09-03]

| Field | What it is |
| --- | --- |
| `+0x86` | **Parent**: `UnitStruct*` of the unit carrying this one; `0` when free |
| `+0x8A` | **Head of the chain of units this one carries** (`0` = carrying nothing) |
| `+0x8E` | **Next sibling** in the parent's chain |
| `+0xF9` | the attach point byte exactly as the command gave it (`0xFF` = "inside") |
| `+0x110 & 0x20000` | set **iff** the attach point is `0xFF` — see below, it is a *draw* flag |
| `+0xA8` | this unit's own id (word); the id the attach command carries |
| `+0x92` | `UnitDefStruct*` (the FBI record) — same field the renderer uses |

**Unit id → address.** `*(main+0x14357) + id * 0x118`. The engine computes the stride the long
way at `0x48AB8A..0x48AB9F`: `eax = id*8 - id` (= `id*7`), `lea eax,[eax+eax*4]` (= `id*35`),
`lea esi,[ecx+eax*8]` (= `id*280` = `id*0x118`). Worth knowing because it is the conversion
every command packet needs, and because `id` is what the network layer passes around, not a
pointer.

---

## 2. Attach and detach are one function — `0x48AB70`

`0x48AB70 .. 0x48AD2D`, `ret 4`. One argument: a pointer to a **packed command**, not a unit.

| Offset in the packet | What |
| --- | --- |
| `+0x1` word | child unit id (`0` = none) |
| `+0x3` word | parent unit id (**`0` = detach**) |
| `+0x5` byte | attach point; **`0xFF` means "inside"** |
| `+0x6` byte | two low bits are xor'd into `Object3do+0x2E` at `0x48ACE1..0x48ACF0` |

**The guards, all of which bail to `0x48AD2A` and do nothing** (`0x48ABC7..0x48AC1D`):

- the child id must resolve to a unit;
- child `+0x110 & 0x10000000` set (the alive bit);
- child `+0x110 & 0x20000000` **clear** — **a structure can never be carried**;
- child `+0x8A == 0` — a unit that is itself carrying something cannot be attached;
- when a parent is given: parent alive, parent ≠ child, and **parent `+0x86 == 0`** — so
  **carrying does not nest**. A transport cannot be carried while it carries.

**Unlink first** (`0x48AC2D..0x48AC53`): whatever the child's current parent is, the engine
walks that parent's chain from `+0x8A` along `+0x8E` until it finds the child, and splices it
out. If the child had no parent it takes `0x48AC57` instead: `ecx = [child+0x82]`, `push child`,
`call 0x47CB00` (not disassembled).

**Then one of two tails:**

| Path | What it does |
| --- | --- |
| **attach**, parent ≠ 0 (`0x48AC63..0x48ACAD`) | `[child+0x86] = parent`; `[child+0x8E] = [parent+0x8A]`; `[parent+0x8A] = child`; `[child+0xF9] = point`; and `+0x110` bit `0x20000` is **cleared then set iff `point == 0xFF`** — `and edx,0xfffdffff` / `cmp cl,0xff` / `sete al` / `shl eax,0x11` / `or edx,eax` at `0x48AC88..0x48ACA7` |
| **detach**, parent == 0 (`0x48ACAF..0x48ACDC`) | clears bit `0x20000`, `[child+0x8E] = 0`, `[child+0x86] = 0`, then `call 0x47CB40` (not disassembled) |

Both then fall into a common tail (`0x48ACE1..0x48AD25`) that pokes `Object3do+0x2E`, and — when
`[child+0x96]` dereferences non-zero with its `+0x73` equal to 1 or 2, a parent exists, and the
**parent's** `UnitDef+0x241 & 0x200` is clear — calls `0x4384A0(child)`, then always
`0x48C9B0(child)`.

**`0x20000` is a "drawn by nobody" flag, not a "is cargo" flag.** It is set only for the `0xFF`
attach point. Both draw paths skip it: the blit's cargo loop at `0x459657`
(`test [esi+0x110],0x20000; jne` → next sibling) and `DrawUnit 0x45AC20` at `0x45AD43`
(`jne 0x45AE4A`). So a unit attached *inside* something — a transport hold — is drawn neither by
itself nor by its carrier, which is exactly right. A unit attached *to a piece*, like the one on
a factory pad, has the bit clear and is drawn **by its carrier**.

### The four call sites

| VA | What it is |
| --- | --- |
| `0x455403` | inside a large command dispatcher (every arm `jmp`s to `0x455F50`), taking the packet from `[esp+0x10]`. **Attach/detach is therefore a simulation command**, which is why it is packed by unit *id* |
| `0x48AB62` | a small wrapper `0x48AB40..0x48AB6A`, `ret 0x10`: builds the packet on the stack (`[esp+0xD] = al & 3`, `[esp+0xE] = cl`), calls `0x44FDB0` then `0x451DF0`, then the attacher |
| `0x48B58B` | **attach** with a real piece: the point byte comes from `call 0x415DC0` / `0x415E60` results — the unit-script accessors *[INFERRED from the `thiscall` shape and the small integer arguments; the enclosing function was not delimited]* |
| `0x48B5C5` | **detach**: guarded on `[edi+0x86] != 0`, it fills child id from `[edi+0xA8]`, **parent id `0`**, **point `0xFF`**, and calls the same function. Detaching is "attach to nobody" |

**So the moment a unit stops being part of its factory's sprite is chosen by the script**, not by
the build finishing, not by the renderer, and not by any distance test in the draw path.

---

## 3. How a carried unit is drawn — the cargo loop

Covered in full on the [engine map](exe-reverse-engineering.html); the short version, because it
is the half people need when they are looking at a factory:

```
0x459646  esi = [unit+0x8A]                 ; chain head, exit at 0x4596EB when 0
0x459657  test [esi+0x110],0x20000 -> skip  ; also the loop's re-entry point
0x459670  call 0x4586A0(cargo_obj, 1, -1)   ; rebuild the cargo composite, EVERY frame
0x459686  call 0x458DD0(cargo_comp, cargo_obj)  ; the build-state effect on the cargo
0x4596D8  call 0x4B90A0(src, dst, sx, sy, dbias)  ; z-merge into the PARENT's scratch
0x4596DD  esi = [esi+0x8E]                  ; next, loop to 0x459657
```

`0x4B90A0` is a depth-tested 8bpp paint with **exactly one caller in the whole image** — this
one. `sx = HIWORD(dx)`, `sy = HIWORD(dz) − HIWORD(dy)/2` (the isometric projection of the
position delta), `dbias = HIWORD(dy)` (the raw height delta, applied to the depth plane). The
source wins on `dstDepth <= srcDepth + dbias`.

Two consequences worth holding on to:

- **the cargo composite is rebuilt every frame**, so a unit on a pad is not cheap;
- **the merge carries a position delta**, so the engine *could* draw a carried unit anywhere
  relative to its carrier and still sort it correctly. It does not have to be on the pad.

---

## 4. The build state is a separate axis

Being carried and being under construction are independent. `Nanoframe` (`+0x104`, the fraction
of the build **remaining**) drives the recolour and wireframe, applied at blit time by `0x458DD0`
on a scratch copy — see [build state](build-state.html). A factory's unit-in-progress is both
carried *and* a nanoframe; a transported unit is carried and not; a building going up under a
commander is a nanoframe and not carried.

---

## 5. Letting go: why a unit "walks under the plant"

**Reported from play (2026-09-03):** *"once a unit is built, if it exits at the top, you can see
it go UNDER the build place of the plant."*

**This is stock behaviour.** [MEASURED 2026-09-03, two independent ways]

**From the binary.** §2: a unit on a pad is merged into the factory's sprite, and `0x48B5C5`
detaches it in one step — parent `0`, point `0xFF`, bit cleared, links cleared. There is no
intermediate state in which the unit is still in the chain but positioned away from the pad, so
there is nothing for a renderer to get wrong at the boundary. Before the detach it is part of the
factory's sprite; after it, it is its own sprite.

**From an A/B on the stock renderer.** `scenarios/exit-sort.json` puts an ARMLAB at world
`[2000,1200]` and Peewees at `[2000,1160]` (40 units north, i.e. 2.5 tile rows) and
`[2000,1120]`. Captured on the same instance twice, once with every pass unarmed and once with
all of them armed:

- the Peewee **40 units north is completely hidden by the lab in both**;
- the one **80 units north is visible in both**.

Identical sorting. The artifact is the engine's, and our pass reproduces it.

**The cause is sprite-level sorting.** TA orders whole sprites by **tile row**, 16 world units
per row. A unit one row north of a building sits a whole depth key behind the building's *entire*
sprite — including the parts that are visually in front of it, the pad, the near wall, the
doorway. So instead of standing in the opening the unit drops behind everything at once, which
reads as sliding under the plant. Our native pass encodes the same thing
(`encBase = 1 + rel*4`, `rel = (wy >> 4) − r0`; features at `3 + rel*4` — see
[terrain, features & depth](terrain-depth.html)), deliberately, for parity.

**What it would take to do better than the engine.** Not a bug fix — a deliberate deviation. The
building's own geometry would need a depth spanning its footprint *in the same units as the row
term*, so a unit in the doorway is occluded only by the geometry actually in front of it. Today
the intra-model term `md = (2y − z)/256` is model-local and clamped to ±1.8 inside a ±2 band
around a per-unit row key, so two units in different rows can never interleave whatever their
real shapes. Making depth continuous per vertex touches the key scheme **every** pass shares —
units, features, effects, markers — so it is a real piece of work with parity risk across all of
them. Not attempted.

---

## 6. What our renderer does with a factory's cargo, and where it approximates

`tagpu_native.c` walks `unit+0x8A` / `+0x8E` after the gather, mirrors the engine's
`state & 0x20000` skip, and gives every chain member **the parent's depth row and air band**.
Without it a cargo unit landed on its own tile row — measured at one 16-unit row from its lab,
four whole depth keys behind it, and the lab covered it at every pixel ("the unit is being built
UNDER the lab", reported from play against the first cut).

**It approximates the merge; it does not port it.** `0x4B90A0` compares a *height* biased by
`HIWORD(dy)` and samples at the projected offset; `md` is model-local and carries neither term.
The two agree while parent and cargo are level — which every factory pad is — and diverge for a
cargo whose origin sits above or below its parent. See [build state](build-state.html) §7.

---

## 7. What is not known yet

- **Which COB opcode, and where in a factory's script, the drop happens.** `0x48B58B` and
  `0x48B5C5` are the attach and detach call sites, but their enclosing function was not
  delimited and the script opcode that reaches them was not traced.
- `0x47CB00` and `0x47CB40` — called on the no-previous-parent and detach paths — were not
  disassembled. Both are in the cluster that also writes `+0x8E` (`0x47CB26`, `0x47CB47`,
  `0x47CBA3`, `0x47CBB0`, `0x47CC13`, `0x47CD0F`, `0x47D0B9`), so there is a **second chain
  manipulator** in `0x47Cxxx` that this page has not read at all.
- `unit+0x82`, `unit+0x96` (and its `+0x73`), and `Object3do+0x2E`'s two-bit field are named only
  by their use in `0x48AB70`.
- The build sequence itself — how the factory advances `Nanoframe`, where the unit is created,
  and what positions it on the pad — is not covered here.
- Whether keeping a unit attached while it walks clear would look right: the merge carries a
  position delta (§3), so it is mechanically possible, but nothing was measured.
