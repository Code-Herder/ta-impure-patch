# Orders and the AI

The order module, order fallbacks, resurrect, the AI's order activity and resource placement.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### The resurrect order `Order_Resurrect 0x404DB0`, and its exits after the unit is made

`0x404DB0` is `stdcall(builder, order, flags)`, `ret 0xC`, the handler of order descriptor 18
(record `0x4FC8AA`: "Resurrecting", the handler at `+4`, the name "Resurrect" at `+0x15`)
[DISASSEMBLED; MEASURED live: the counters below fire inside it on RESURRECT orders]. Its state is
the order's byte `+0x5`, dispatched through the table `0x4052D8` (states 0–6). The full walk, and
why TADR's post-create fix was not ported, are in
[the section B evidence, Part 3 §1](../tadr-port/sim-fixes-evidence.md). What this map keeps:

- **The entry check, every state ≤ 5** (`0x404DC6..0x404E16`): `0x421DA0(order+0x22)` answers the
  feature's def through one `0xFFFE` hop; `0xFFFF` → "Resurrection failed" (`0x501698`) and
  return 8, and a FeatureDef without `+0xFE` bit 7 → return 8.
- **The countdown** (state 3, `0x404F92..0x404FEE`): `order+0x3A = BuildTime × 0.3 /
  ⌊WorkerTime / 30⌋`, the resurrected type's UnitDef `+0x1EA` against the builder's `+0x1FE`, the
  0.3 being the double at `0x4FC948`, made an integer by `0x4E43A0` [INFERRED: `_ftol`]. State 4
  (`0x405005`) counts it down, one per call.
- **The second lookup, after `CreateUnit`** (state 5): `0x4815F0(order+0x22)` → the anchor cell,
  and `GetGridPosFeature 0x421E60` → its def. **`0x4815F0` follows the `0xFFFE` hop itself**
  (`0x481640..0x481679`, bounded), so an order on any cell of a multi-cell wreck reads the
  anchor's record at `0x40516C` — the record index at the anchor's `+0xA`, not the hop offsets.
  `0x40514F cmp ax,0xFFFB; jb 0x405164`: the **failure exit `0x405155`** returns 8 with the new
  unit left a nanoframe; the **success path `0x405164`** copies the record's `+0x20` rotation to
  `unit+0x64` (`0x40516C..0x405191`), removes the wreck with `FEATURES_Destroy 0x4246B0` (which
  hops too, `0x4246BE..0x4246E6`), and finishes the unit.
- **How a player gives it: `reclaim`** [DISASSEMBLED]. The order resolver `0x43F0E0` has no
  resurrect type among its 1–14 (jump table `0x4401EC`); its reclaim case `0x43F4F7` needs the
  unit's def `+0x245` bit `0x400` (`0x43F4FD`), finds the feature under the position (`0x4815A0`
  and the `0xFFFE` hop, `0x43F50B..0x43F58A`), and when the def also has `+0x245` bit `0x800`
  (`0x43F5A5`) [INFERRED: `canresurrect`; CORNECRO is the one retail unit with that tag], the
  cell is in the viewing player's sight (`0x43F5D1..0x43F5FF`: the word at `main+0x14273`, bit
  `main+0x2A43`) and FeatureDef `+0xFE` has bit 7 (`0x43F609`), it jumps to `0x44004C` (`0x43F612`), which
  resolves the name "RESURRECT" (`0x5052F0`) through `0x438760`. So a scenario's or `tacli order`'s
  `reclaim` of a wreck by a Necro is a resurrect order.

**MEASURED 2026-09-26: the failure exit never fired.** Scratch counters (a build never committed)
on `0x405155` and `0x405164` logged the order's position, the cell and its record index at each
pass; `ddraw.dll` at `8d033d1`, gamespeed 10, `scenarios/b1-resurrect.json` and seven variants of
it (`b1-resurrect.gen.py`: 110 wrecks of 1×1 to 5×6 on the Two Continents plateau, each with a
CORNECRO ordered to reclaim it, ten wrecks with a second Necro, a third of the multi-cell orders at
the anchor and the rest at the footprint's centre or far corner). **847 passes through `0x405164`,
0 through `0x405155`**: 440 on 1×1 wrecks, 126 on a multi-cell wreck's anchor, 136 on another cell
of one, and 145 in a first layout that was not split by footprint. Every pass read the anchor's
own def at the cell `0x4815F0` returned. About a third of the orders never reached state 5 (a
shared wreck's second Necro finds it gone at the entry check; the rest were not followed up).

### The saved-game loader's order fallback — `0x43A58D` [DISASSEMBLED 2026-09-25]

**The walk.** The order loader `0x43A420` has one caller, `0x487594`. It takes an order's type
from its `<key>_name` (`0x43C6B0`, then a case-insensitive `_stricmp 0x4F8A70` against the table). A name not found goes to
`0x43A552`, "Ready", with `dl` = 0. A record without the name falls back to its stored index. It
walks the order table `0x512344` counting records whose `+0x14` bit 0 is clear
(`0x43A556..0x43A58F`).

**The defect.** The walk runs while the record is at most the table's end `0x512348`
(`0x43A58B`, `cmp eax,esi; jbe 0x43A56D`). That is one record past the end. An index the walk
does not meet goes from `0x43A58F` to `0x43A598`, which leaves `dl` at the count
(`mov dl,cl`): 68 or 69 against a table of 68 records. That becomes the order's type byte, and a
wild dispatch on its first tick.

This exe's writer always stores the name (`0x43AA90..0x43AAE3`), so only a foreign or damaged
save reaches the walk. The table holds 68 records for the whole process:
`UIPipelinesInit 0x491200` fills it once. So the walk's first record, examined before the bound,
is always a real one.

**The fix, two bytes of one local site:**
- `jbe` → `jb` at `0x43A58D`, so the table's end is exclusive;
- the not-found `jmp` at `0x43A58F` → `0x43A552` (`EB 07` → `EB C1`), the by-name branch's own
  "Ready".

`ebx` and the other registers reach `0x43A59A` as before. Only `dl` differs, and only for an index
the table does not hold. It is not run: it rests on the disassembly.

## The order module — where an order's position lives, and in what units — mapped by us

[MEASURED 2026-09-04, this project — `objdump` of the pristine Steam build, plus a live A/B on
Two Continents. Established while fixing the scenario applier, whose orders all walked to the
map origin.]

**`ORDERS_NewMainOrder2Unit 0x43AFC0`** — stdcall, `ret 0x1C`, seven args
`(actionIndex, shift, unit, target, pos, p1, p2)`; `pos` is a pointer to **three 16.16
dwords**.

**Its first branch is on `shift`, and with `shift = 0` almost none of the function runs**
[BINARY-VERIFIED, corrected by the landing review — the first draft of this section described
the scan as unconditional]:

```
43afc0  mov  eax,[esp+0x8]      ; arg2 = shift
43afd4  test eax,eax
43afda  je   0x43b08a           ; shift == 0 -> straight to the allocator
43afe0  mov  esi,[edi+0x5c]     ; else walk the order list, head ALWAYS +0x5C
```

`0x43B08A` re-pushes the arguments and calls the allocator **`0x43ADC0`**, which takes `0x56`
bytes (`push 0x56; call 0x4B4F10` at `0x43ADC4`) and constructs the order in place with
**`0x43A0C0`** (`thiscall`, `ecx` = the new order). The scenario applier passes `shift = 0`, so
**that is the whole of its path** and everything below about the scan is context, not
description of what we do.

**What the `shift != 0` scan does when it matches is CANCEL, not merge**
[BINARY-VERIFIED `0x43B034..0x43B087`]: `0x43B03D` picks `unit+0x60` over `+0x5C` as the list
head to unlink from when the matched order's flag word `+0x42` has `0x40000` (this is the
*unlink* head — the walk itself started unconditionally at `+0x5C`), `0x43B05B..0x43B05E`
unlinks it, `0x43B075 call 0x43A1F0` destructs it, `0x43B07B call 0x4B4F20` frees it, and
`0x43B087 ret 0x1C` returns **without creating anything**. That is TA's shift-click-a-waypoint-
to-remove-it behaviour, not a merge. A node matches on its action byte `+4` (`0x43AFF2`), on its
target `+0x16` only when a target is passed (`0x43AFF7..0x43AFFE`), and on the ±16 wu position test
below only when a position is passed (`0x43B000`); with no match the seven arguments go unchanged
to `0x43ADC0` (`0x43B08A..0x43B09D`), which links the order without the wipe below
[DISASSEMBLED 2026-09-28].

**And `0x43ADC0` wipes the queue on the way in when `shift = 0`.** Only with its `shift`
argument 0 (`0x43AE02..0x43AE08`) and a new order whose flags `+0x42` lack `0x40` (`0x43AE0E`),
`0x43AE14..0x43AE59` walks `unit+0x5C` and destroys every existing order that lacks flag bit `0x4`
before linking the new one — so with `shift = 0` an order REPLACES the unit's orders rather than
queueing, and with `shift ≠ 0` the wipe is skipped. The link [DISASSEMBLED 2026-09-28,
`0x43AE5B..0x43AFB1`]: a new order without `0x40000` first drops the leading `+0x5C` orders that
carry `0x4000`; it gets `0x1`, and `0x2000` when `shift = 0`; an order with `0x20` or `0x40000` goes
to the head of its list (`+0x60` for `0x40000`, else `+0x5C`) and takes the old head's `0x4000`;
any other goes right after the `+0x5C` order carrying `0x1000`, taking that mark from it, or to the
tail when none carries it. The applier's `orders`
array therefore only ever takes effect in its last element; recorded in
[scenario format](../scenario-format.html).

**The position is copied verbatim, which is why a read-back probe cannot check it**
[BINARY-VERIFIED `0x43A164..0x43A177`]:

```
43a164  mov ecx,[eax]        ; eax = the caller's pos[]
43a169  mov [esi+0x22],ecx   ; Pos.X
43a16c  mov ecx,[eax+0x4]
43a16f  mov [edx+0x4],ecx    ; edx = esi+0x22  -> +0x26
43a172  mov eax,[eax+0x8]
43a175  mov [edx+0x8],eax    ;                 -> +0x2A
```

Three dwords, no scaling and no reordering. So writing the position and reading
`UnitOrders->Pos` back proves only that nothing mangled it — `passed == stored` is a tautology,
and the applier's phase-C "measurement" that concluded whole world units was reading exactly
that tautology.

**What settles the scale, directly.** `0x4815A0` takes a `pos` pointer and turns it into a
map cell [BINARY-VERIFIED, and this is the strongest evidence — it is a consumer, not an
inference]:

```
4815a0  mov eax,[esp+0x4]    ; the pos pointer
4815a5  mov ecx,[eax]        ; pos[0]
4815a7  mov eax,[eax+0x8]    ; pos[2]
4815aa  sar ecx,0x14         ; >> 20 = 16.16 -> world, then / 16 -> a CELL
4815ad  sar eax,0x14
4815b4..c8  bounds-check against main+0x14233 / +0x14237, the map's size IN CELLS
```

`sar 0x14` is only a cell index if the input is 16.16, and the two components it takes are
**0 and 2**, bounds-checked against the cell dimensions — so those are the ground plane and
the middle one is the altitude. `ScriptAction_Type2Index` agrees from the other side: at
`0x43FF5B..0x43FF6F` it reads the three HIGH words (`[esi+0x2]`, `[esi+0x6]`, `[esi+0xA]`) and
does `sar ebx,1` on the middle one before subtracting it from the third — altitude/2 against
depth, the engine's own screen-y term.

**The duplicate-order test at `0x43B006..0x43B029` corroborates it** (on the `shift != 0` path,
so not on ours) [BINARY-VERIFIED]:

```
43b004  mov ebp,[eax]          ; caller pos[0]
43b006  sub ebp,[esi+0x22]     ; minus the candidate order's Pos.X
43b009  add ebp,0x100000
43b00f  cmp ebp,0x200000
43b015  ja  0x43b02b           ; too far -> not the same order
43b017  mov ebp,[eax+0x8]      ; caller pos[2]
43b01a  sub ebp,[esi+0x2a]     ; minus Pos.Z
43b01d  add ebp,0x100000
43b023  cmp ebp,0x200000
43b029  jbe 0x43b034           ; near enough -> the match (cancel)
```

A tolerance of **±0x100000, which is ±16.0 in 16.16 — one map cell**. In whole world units it
would be ±1 048 576, i.e. the whole map and then some, which is not a tolerance at all; and it
too compares components **0 and 2** and never 1. The order position is therefore the same
`TPosition {x, altitude, depth}` that `UNITS_CreateUnit 0x485F50` takes. **There is no
create/order asymmetry**; `scenario-format.md` §"The coordinate asymmetry" claimed one from
TADR's `ConstructionKickout` and was wrong, and is corrected there.

**Live confirmation** [MEASURED 2026-09-04, `scenarios/shadow-air.json`, Two Continents]: with
whole world units, `1900 >> 16 == 0`, and every ordered unit set off for the map origin — a
Peewee told to `move` to `(1900, 1450)` walked north-west and five aircraft told to fly east
ended stacked in the north-west corner, one of them reading world `(1, 0)`. With the shift the
Peewee stops at `1902` and every aircraft reaches `4200`. `patrol` was broken the same way and
now loops its lane; it had looked like a separate defect and was not.

**Negative results.** `0x43A1F0` (the order destructor, called at `0x43B075`) and `0x4B4F20`
(the free, `0x43B07B`) were not chased further. Three bits of the order flag word
`unit_order+0x42` appear and none is named: `0x40000` picks the `+0x60` unlink head
(`0x43B034`), `0x10000` is OR-ed in at `0x43B068` when the cancelled order is not the head of
`unit+0x5C` (the head is kept in the fifth argument's slot, stored at `0x43AFE8`), as `0x43ADC0`'s
wipe does for every order it destroys but the old head (`0x43AE34..0x43AE3A`), and `0x4` exempts
an order from `0x43ADC0`'s wipe. `0x41074A` reads
`order+0x22`/`+0x2A`/`+0x26` and tests each against zero — an "is the position set" check, in
that order, which is itself consistent with 0 and 2 being the ground plane; what the caller
does with the answer was not chased. `0x43B0B0` is a **different** function
that also calls `0x43ADC0` — do not read the `call 0x43adc0` at `0x43B09D` and the one at
`0x43B120` as the same site. It keeps a factory's queue as count nodes (*Replay state design*,
in the demo recorder section).

## Direct AI order activity observer

DISASSEMBLED against the pristine 3.1 executable: `0x408C40` has one direct
caller, the players phase at `0x465031`. It receives its AI object in ECX,
tests the object's owner record for active and controller 2 at
`0x408C45..0x408C50`, runs the group assignment `0x408830` every 30 calls,
and dispatches due planner objects from ten slots at AI+0x11 through their
vtable at `0x408C8E`. Group assignment classifies live units and calls
`0x480250` at `0x4088FA`; it does not directly construct orders in the
disassembled body. `0x408BF0` contains a similar assignment/planner loop
without the controller gate; the activity observer does not use it.

`0x4089A0` is the final call on both branches of `0x408C40`: at `0x408C9A`
with argument 1 for AI, and at `0x408CA7` with 0 otherwise. These are its only
direct callers. It scans weapon targets and invokes target setters/resetters
`0x48A0A0`, `0x48A060`, `0x48A0F0`, with queries `0x49D120`, `0x48A190` and
`0x40B7B0`. Because human seats also run it, activity attribution ends on
entry to this phase, through the existing `tagpu_weapons.c:my_Acquire` wrapper.
A weapon reaction alone cannot prove planner liveness. Acquisition already owns
a guarded detour; adding an observer over that detour makes the limits integrity
check refuse startup. The recorder therefore calls no second patch installer there.

`0x43A0C0` is the common order constructor: ECX is the new node, returned in
EAX at `0x43A1C6`; `ret 0x18` at `0x43A1CF`. It sets action+4, state+5 and
event fields, stamps GameTime at node+0x46 (`0x43A12E`), and leaves the
owner+0x0E NULL (`0x43A193`). It runs no order handler. The high-level helper
`0x43ADC0` calls it at `0x43ADEF`; callers can insert the result inline,
so observing only list-insertion functions misses new AI orders.

The four examined insertions are `0x43AC60` (insert before the supplied node,
ret12), `0x43ACB0` (head, ret8), `0x43AD10` (tail, ret8), and `0x43AD50`
(priority position, ret8). Each stores the owner at node+0x0E and links through
node+0x4A. Their direct callers are listed below; no direct caller of
`0x43AD50` was found in the disassembly.

- `0x43AC60`: `0x43BA1E`, `0x43BA30`.
- `0x43ACB0`: `0x401C99`, `0x401CAC`, `0x401F59`, `0x401F6E`, `0x4032F7`, `0x40330A`, `0x40529A`, `0x4052B2`, `0x405C06`, `0x405D18`, `0x405D33`, `0x406108`, `0x40611B`, `0x406532`, `0x40667A`, `0x406695`, `0x40F92B`, `0x40F943`, `0x40FEC3`, `0x410109`, `0x410124`, `0x4105D0`, `0x410A2A`, `0x410B0D`, `0x41101A`, `0x411EE1`, `0x411EF9`, `0x412692`, `0x412C14`, `0x413365`, `0x413380`, `0x413B5A`, `0x415453`, `0x4155EA`, `0x41578B`, `0x4157A6`, `0x489E97`, `0x489EA6`, `0x48A044`, `0x48A051`.
- `0x43AD10`: `0x410384`, `0x411FBE`, `0x411FD6`, `0x41203E`, `0x412056`, `0x412786`, `0x4127E2`, `0x4127FA`, `0x412DAE`, `0x412DC6`, `0x412E28`, `0x412E40`, `0x4134D9`, `0x4134F1`, `0x413551`, `0x413569`.
- `0x43AD50`: no direct caller.
- `0x43A1F0`: `0x4384E4`, `0x439EF3`, `0x439F5F`, `0x439FC9`, `0x43AE47`, `0x43AEB8`, `0x43B075`, `0x43B1C9`, `0x43B93B`, `0x43BA5B`, `0x43BBE6`, `0x43BC49`.

At the existing normal dispatcher observation sites `0x43B878` and
`0x43BB1D`, ESI names the live order node; both read node+0x0E and call
its handler. Destruction has a separate site, `0x43A216`, which is excluded
from execution evidence. The destructor `0x43A1F0` receives the node in ECX
and reaches any event-2 handler only after `0x43A1F5`. Forgetting a sample
at entry establishes its lifetime before an allocation can reuse its address.
The observer dereferences only the current dispatcher node, never a saved
sample pointer, and bounds its owner against the level's unit allocation.

DISASSEMBLED: the secondary controller `0x43BAD0`, called at `0x48AF9E`, reads
the unit's `+0x60` list at `0x43BADA`. A node with nonzero `+6` waits until
GameTime reaches its `+0x0A` deadline (`0x43BAE8..0x43BAFD`). Dispatch clears
`+6` at `0x43BB09`, pushes event 0 and the current node at `0x43BB15..0x43BB16`,
then reads its owner and calls the handler at `0x43BB1D..0x43BB21`. Removal
calls the destructor at `0x43BBE6` or `0x43BC49` before `0x4B4F20` frees the
node. The controller is inside the unit tick's owner-type-1/2 block, beside
the primary controller at `0x48AF98`.

MEASURED on the reference setup, 2026-10-04, normal coverage build: native
selection of the isolated ARMSOLAR at screen `(512,384)`, then Ctrl+D, removed
that unit while the two remote units stayed alive. The coverage bitmap reached
`ev_nano_h2`. This covers the secondary dispatch callback; a human self-destruct
does not count as a planner-originated AI order.

Observer stolen prefixes: `0x408C40` five bytes `53 56 57 8B F9`;
`0x43A0C0` seven bytes
`83 EC 0C 8A 44 24 10`; `0x43A1F0` five bytes `53 56 8B F1 57`.
All are complete instructions without relative operands. The normal-dispatch
observers already exist for the recorder's nano context.
`ai_hooks_init` checks all three current prefixes with `tagpu_detour_bytes_ok`
before installing any observer. `tagpu_detour_observe` alone compares an
already registered Impure stub; its first-install path does not match the
engine's bytes. A mismatch therefore leaves all three new observers uninstalled,
and the harness refuses AI activity observation.

### Resource placement and contextual order contracts [DISASSEMBLED + MEASURED 2026-10-04]

`tagpu_resource.c` implements manual mex/geo snapping and contextual resource building. All
queries and dispatches run on the game/input thread; the renderer gets only packet values.
The following contracts were disassembled from `pristine/TotalA.exe.pristine`. Caller counts
are an instruction-level `call rel32` scan of `.text`, not a scan of address-shaped bytes.

| Entry | Contract and callers |
|---|---|
| `0x4197D0` | Manual build cursor, `ret 0`, prologue `83 EC 14 8B 15 E8 1D 51 00`; called at `0x491CDB`, `0x499241`. Reads type `main+0x2CC4` and point `+0x2CAA`; writes six build-corner ints `+0x2C92..+0x2CA6` and the `+0x2CC6` valid/region bits. Snapping observes its entry; original code owns the rectangle and validation |
| `0x498DA0` | Mouse-to-world update, called only at `0x499221`. The existing viewport wrapper repairs its result before resource input captures the raw point. In `0x499200`, cursor update `0x499241`, hovered unit query `0x499278` and context resolution `0x499297` precede button dispatch |
| `0x498F70` | Left button handler, `ret 4`, prologue `51 A1 E8 1D 51 00`, one caller `0x4995B3`. The resource gate's saved entry stack holds the six-dword message pointer at `[esp+4]` |
| `0x499100` | Right button handler, `ret 4`, prologue `51 8B 0D E8 1D 51 00`, one caller `0x4993AC`. Contextual orders use right when Interface Type `main+0x37EFA` is nonzero; zero uses left. Explicit commands retain native button/cancel behavior |
| `0x47D2E0` | Site test, `stdcall(def, packed_cell, ignore_unit_id, player)`, `ret 16`; callers `0x40A4CD`, `0x4198C6`, `0x47DBE5`. Requires origin at least 1 and footprint end strictly below W/H. Its return's bit 0 reports acceptance |
| `0x47C780` | Build-site altitude result, `ret 0`, only caller `0x4198F5`; low byte is the site's altitude. Reads the scratch produced by the preceding site test. Resource preview/click queries save and restore `0x51E684`/`0x51E688`; the native manual cursor retains its normal scratch ownership |
| `0x47D820` | Blocked-site altitude [DISASSEMBLED 2026-10-09], `stdcall(def, x \| z<<16)`, `ret 8`; callers `0x4198FE` (the build cursor, on a refused site) and `0x47DE4A`. Answers 0 for an origin below 1 or a footprint end at or past W/H (`0x47D834..0x47D888`); otherwise walks the yardmap `def+0x14E` over the footprint `def+0x14A`/`+0x14C` (grid `main+0x14287`, 13-byte cells, width `+0x14233`) and, over the cells whose yard byte has `0x08`, takes the lowest cell byte `+6` and the highest `+5`: it returns the lowest `+6` as a byte, or sea level `main+0x1427F` less `def+0x22C` when that highest `+5` is below it, which includes a footprint with no such cell (`0x47D92A..0x47D94A`). It writes nothing outside its own frame. Quick Build gives it to the ghost of a queued plan whose ground the site test refuses |
| `0x43AFC0` | Ordinary mouse order dispatch, `stdcall(action, shift, unit, target, pos, param1, param2)`, `ret 28`. Called at `0x4197A9` (manual build) and `0x48D1F4` (contextual command). A build's param1 is the definition ID; an upgrade uses the resolved reclaim action then an appended build |
| `0x438760` | Action-name lookup, thiscall `(output_byte*, name)`; 109 direct callers, including manual dispatch `0x4197A4`. Resource dispatch asks for `MOBILEBUILD` (`0x502654`) or `VTOL_MOBILEBUILD` (`0x502640`) according to the builder's fly bit |
| `0x43F0E0` | Action resolver, `stdcall(index*, mode, unit, target, pos)`; returns the caller's output pointer; refusal writes action byte zero (`0x4401E3`), which native dispatch rejects at `0x48D0AF`. Guard is mode 7, reclaim mode 12. There are 28 direct callers; context dispatch uses `0x48D0A0`, script orders `0x4804A3`, and constructor/assist paths `0x487D5B`..`0x487F8C`. Resource input asks the resolver instead of assuming every selected builder can reclaim or Guard |
| `0x48CD80`, `0x48D220` | Hovered-unit query and context resolution; callers respectively `0x491D18`/`0x499278` and `0x491D36`/`0x499297`. Hovered unit ID is stored at `main+0x2CBA`; feature ID is `+0x2CBC` |
| `0x481930` | Explored-map setter, six callers: `0x4818E6`, `0x48269C`, `0x48279B`, `0x482901`, `0x482A94`, `0x482C08`. Uses the player's map bit, not its seat number; allocation `0x483CF6`..`0x483D25` is W×H/4 u16 entries for even W/H |
| `0x422040` | Applies indestructible metal-spot feature values to grid byte `+7`; only caller `0x483D7D`. The cell's feature ID must be below `0xFFFB`, its definition's metal float `+0xF0` nonzero and flags `+0xFF & 2` set. Uses definition footprint `+0x94/+0x96`, bounds helper `0x481550`, and float conversion `0x4E43A0`. This is how the private synthetic fixture supplies metal values |

The full resolver caller list is `0x401F2F`, `0x402E3D`, `0x4031A7`, `0x405265`, `0x405ABC`,
`0x4064F5`, `0x40804C`, `0x408299`, `0x408403`, `0x408435`, `0x408589`, `0x40FE86`,
`0x410AD9`, `0x43B242`, `0x43B285`, `0x43B416`, `0x43B451`, `0x43B583`, `0x43FE5A`,
`0x43FE83`, `0x43FECC`, `0x4804A3`, `0x487D5B`, `0x487DD3`, `0x487E2E`, `0x487EFD`,
`0x487F8C`, `0x48D0A0`.

**Candidate-cell distance and actual placement centers.** SOURCE: TADR v2026.8.6
`tahook.cpp:SnapToNear` ranks equal-coverage candidates by squared distance from
`(cx + 0.5, cz + 0.5)` to the raw mouse in cell units; its geo caller uses the same selector.
Impure preserves this metric. It is not a guarantee of the nearest actual building center.
DISASSEMBLED: manual dispatch `0x419670` (sole direct caller `0x498F98`) reads type
`main+0x2CC4`, point `+0x2CAA` and definition footprint
`+0x14A/+0x14C`. Its `0x4196D8..0x419714` conversion reconstructs each order coordinate
as `(2 * footprint_origin + footprint_size) << 19`, passed through `0x43AFC0` at `0x4197A9`.
`tagpu_resource.c:position` uses that same center: even footprint axes place at `cx * 16`
world units, odd axes at `(cx + 0.5) * 16`. Thus an even axis's actual center differs by
eight world units from the candidate-cell center used for the tie. No metric change is implied.

#### Native MobileBuild placement retries [DISASSEMBLED 2026-10-05]

An accepted contextual upgrade queues native reclaim followed by `MOBILEBUILD`; queue
creation does not establish that the replacement will start. `MobileBuild` at `0x403A20`
dispatches state 1 to `0x403B45`. It obtains the replacement definition from `node+0x36`,
recomputes its packed footprint origin from the stored position, and calls
`0x47DB70(def, 0, packed_cell, 1)` at `0x403CC7`. The zero is the ignored unit number:
this test does not exempt the constructor. For a stationary definition (`def+0x22F == 0`),
`0x47DB70` delegates to the yard test `0x47D2E0` at `0x47DBE5`, also with ignored unit
number and player zero. The other branch tests occupied cells against the supplied
ignored number at `0x47DD00..0x47DD0A`.

On placement refusal, `node+0x3E` counts retries. The first refusal emits its message at
`0x403CDF`; `0x403CE4..0x403CF4` increments the counter, arms a 30-tick wait through
`0x439E80`, and returns 2. Subsequent refusals reach `0x403D03`: a counter greater than
10 emits the final message at `0x403D10` and returns 8 at `0x403D15`, abandoning the
command. Acceptance calls `0x47DDC0` at `0x403D2F`, creates a nanoframe through
`0x485F50` at `0x403D5B`, and links it into `node+0x16` through `0x489690` at
`0x403D64`. Failure to obtain a unit retries after 300 ticks (`0x403D7D..0x403D89`),
which is distinct from placement refusal. These contracts are disassembly, not proof
that a particular reported upgrade failed for constructor overlap.

Installed stock definitions read for the Comet Catcher stress test: `ARMMEX`/`CORMEX`
have 3x3 footprints, `ARMMOHO`/`CORMOHO` 5x5, and `ARMACK`/`CORACK` 2x2 with
build distance 60. `tools/resource_upgrade_stress.py` measures actual mouse dispatch,
reclamation, frame creation and completion; its native-order snapshots require the
pause bit at `main+0x38A51`, so the unit tick and its order destructors cannot run
between the game-thread peeks.

#### CORE reclaim-to-Moho handoff [DISASSEMBLED + MEASURED 2026-10-05]

On installed Comet Catcher, stock `CORACK` at `(2892,1772)` upgrading `CORMEX`
at `(2824,1704)` through a contextual click repeatedly reclaimed the old mex,
created `CORMOHO`, then abandoned it without reducing `unit+0x104` from 1.
The native MobileBuild node reached state 2, flags `0x14`. At `0x403DE1`,
`0x438590` starts StartBuilding; state 2 calls `0x438700(unit,node,0xA)` at
`0x403DFD`. That helper waits for `unit+0x10F` bit 0 (`INBUILDSTANCE`), marking
the node's flags with bit 2 while it is clear. State 3 builds through `0x41BA60`.

The COB trace identified the missing transition. At tick 2901 all eight native
slots were occupied: MotionControl and walklegs, SmokeUnit, RequestState,
Stop and close, StartBuilding and StopMoving. StartBuilding's START of another
RequestState was refused at `0x4B08C0` (`cmp eax,8` at `0x4B08E3`), so the
script never requested its new build state. The installed CORACK script has one
serialized RequestState coordinator, which CALLs Go/open or Stop/close; a new
RequestState updates its desired state and returns while that coordinator is busy.
The handoff therefore requires a ninth record to deliver that update. Those
chains have no recursion. This establishes the demand of the reproduced handoff,
not a bound on every possible mod script or arbitrary additional callbacks.

The nanoframe's GetBuilt handler (`0x402DA0`) remains incomplete when stance
never arrives. Its wait arms 300 ticks through `0x439E80` from state 0 at
`0x402F8C`; state 2's elapsed-event branch at `0x402F5B` calls the native
unbuild/decay path `0x41BCD0(unit,11)` at `0x402F6B`. A ghost disappearing here
does not establish a placement failure. In one paused reproduction, the bot's
2×2 footprint at cells `(178,109)` and the accepted Moho's 5×5 footprint at
`(174,104)` shared no cell. Explicit native build from idle at the same site and
bot position entered state 3, stance 1 and reached remaining fraction `0.961694`.

On the sixteen-slot diagnostic prototype, the trace accepts
the formerly refused RequestState in slot 8. The focused native assertion reached
stance 1 and remaining `0.947848` after 679 ticks. An all-deposit CORE sweep
completed all 61 accepted upgrades by 7541 elapsed ticks; 20 initial site refusals
are separate from accepted upgrades. The constructors, mexes and Mohos retained
stock definitions; only supply was synthetic. The original mixed sweep completed
all 37 accepted ARM upgrades and abandoned all 24 accepted CORE upgrades. These
measurements identify a real CORE COB failure; they do not identify the faction
or exact cause of an unrecorded earlier play report.

The chosen ten-record runtime repeated the same CORE phase-0 sweep: **61 of 61
accepted upgrades completed**, by 7611 elapsed ticks, 20 initial refusals and no
unfinished build. Its trace accepts RequestState in slot 8 (including units 169
and 265 at tick 2651). Native ten-record save/load preserved all ten active records
on human and local-AI probes; native eight-record saves imported without loss.
Full-pool START and CALL probes passed 16 and 3 assertions respectively.
The automated native save/load preflight separately preserves all ten active
records on both owners; its coverage bitmap reaches the replacement `cob_save`
and `cob_load` functions at the native `0x4B1EC0`/`0x4B2040` methods.
The ARM phase-1 sweep on ten records completed all 67 accepted upgrades by 8303
elapsed ticks, with 14 initial refusals. Keeping generated starts 128 world units
inside the border lets the previously motionless lower-border bot reclaim and
complete; no native pathfinder change is made.

The long replay fixture checks completed stock Moho at tick 7800 and requires its
last comparison tick at or after that checkpoint. The diagnostic run's unit/player,
effect-count and three restore comparisons passed, while paused viewer input first
computed CORMOHO's ghost-mask row (`s_ghost` type 219): piece-count/valid/any and its
hide bits changed at frozen tick 9219. `tagpu_datakeys_ghost_mask` now keeps an
uncached paused preview in local storage; `main+0x38A51` bit 0 gates cache writes,
so a first hover cannot write SIM_STATE while the simulation tick is frozen.
Puppet cache misses use local storage regardless of native pause, leaving the
recording the only writer even while a replay is paused. No declaration or recorded-state comparison is removed by this correction.

#### Input bounds and layout

The root is `0x511DE8`. The controlled player byte `main+0x2A42` is bounded below 10, and resource
input refuses puppet seats. Global unit begin/end `+0x14357/+0x1435B` are inclusive, stride
`0x118`; their aligned distance plus one must equal the engine count `u16 +0x14351`. The
controlled player's record at `+0x1B63 + seat*0x14B` has first/last pointers `+0x67/+0x6B`, also
inclusive and aligned within the global pool. Own-unit and order scans use that smaller partition.

Unit flags `+0x110`: alive `0x10000000`, selected `0x10`, excluded `0x4000`, structural footprint
`0x20000000`, contested footprint bits `0x0C000000`. Owner is byte `+0xFF`; definition pointer
`+0x92` must be aligned in the `0x249`-stride definition array at `main+0x1439B`, bounded by
`+0x1438F`. Unit number is `u16 +0xA8`, world position is three 16.16 ints `+0x6A/+0x6E/+0x72`;
`+0x104 == 0` identifies completed construction in the resource path. Grid origin is
`u16 +0x76/+0x78`, footprint size `+0x7E/+0x80`.

Definitions: footprint shorts `+0x14A/+0x14C`, yard pointer `+0x14E` (allocated as FootX×FootZ by
`0x42CF30`), CANBUILD count `+0x152` and u16-array pointer `+0x156`. The existing whole-build-list
patch maintains `bl_room(count)` at every allocator/copy/append; resource hooks require its
complete installed state. `s_buildListsLive` is `SIM_INSTALL`, set from the deferred patch-table
registration; its public predicate also requires `s_limState == 1`. This prevents input using a
count bound without the allocation bound. Building movement code is byte `+0x22F == 0`, builder
bit `+0x241 & 0x40`, fly bit `+0x241 & 0x800`; worker time is u16 `+0x1FE`, repair capability
`+0x245 & 0x200`. Metal extraction and energy output floats `+0x1CE/+0x1C2` must be finite and
positive. Geo recognition scans the complete bounded yard for byte `0x8F`, at most 64×64 cells.

The feature grid `main+0x14287` has dimensions `+0x14233/+0x14237`, stride 13. Dimension products
are checked before indexing. Metal is byte `cell+7`. Background `SurfaceMetal` is loaded through
`[main+0x391E9]+0xD30` (`0x4366B3`..`0x4366DB`) and seeds ordinary grid metal
(`0x4839B3`..`0x4839ED`). Explored entries at `main+0x14273` use the player record's byte `+0x146`
as a bit index, bounded below 16. Every metal read first checks that cell's explored 2×2 block;
unknown neighbors prevent a Quick deposit from being proved closed. The engine's full-map
reveal fills this explored map; resource scoring has no alternate unseen-data path.

Order nodes belong to the live unit's game-thread-owned heap chain at `unit+0x5C`; no node is
retained across iterations. Walks stop at 16384 nodes and check owner pointer `node+0x0E`.
Action `byte +4` indexes the level's descriptor vector `0x512344..0x512348` at stride `0x19`;
capability `+0x0C & 1` distinguishes a placed build from factory counts. Target is pointer
`+0x16`, position `+0x22/+0x26/+0x2A`, build type u16 `+0x36`, flags `+0x42`, next pointer `+0x4A`.
Plain order clearing (`0x43ADC0`) preserves flags bit 2 (`4`); queued collision tests reproduce
that distinction for participating builders/helpers rather than discarding every old plan.

#### Contextual cancellation through the native order roots [DISASSEMBLED 2026-10-05]

`0x43AFC0` tests Shift at `0x43AFD4` and scans only the main chain `unit+0x5C` at
`0x43AFE0`. The first action match (`0x43AFF2`) whose optional target matches (`0x43AFF7`..`0x43AFFE`)
and whose X/Z positions are within inclusive ±16 world units (`0x43B004`..`0x43B029`) wins.
The position arithmetic is unsigned wrapping 32-bit subtraction plus `0x100000`, compared against
`0x200000`. **It never compares the build type at `node+0x36`.** A NULL target argument skips
the target comparison. With no match, `0x43B08A`..`0x43B09D` appends through `0x43ADC0`.

`0x43B034` chooses the unlink head: `+0x60` for node flags `+0x42 & 0x40000`, otherwise
`+0x5C`. `0x43B05B`..`0x43B05E` unlink the matched node. The original head is saved at
`[esp+0x24]`; a non-head removal sets `+0x42 & 0x10000` at `0x43B068`. The calls at
`0x43B075` and `0x43B07B` destruct (`0x43A1F0`) and free (`0x4B4F20`) it, then return
without creating an order. The root is used by manual placement `0x4197A9`, contextual orders
`0x48D1F4` and the resource input path.

The destructor calls the action handler with flags 2 at `0x43A21A` when node byte `+6 & 2`
is set. Flags `+0x42 & 0x400000` run the native `StopMoving` script lookup/start at
`0x43A227`..`0x43A258` (`0x4B07C0`, `0x4B0B00`, `0x456190`). Its `+0x52` object cleanup
at `0x43A264`..`0x43A2A4` can invoke virtual destructors. A head removal also calls
`0x489800(unit,3)` at `0x43A2B9`; the `0x10000` flag skips that call for non-heads.
`0x489650` at `0x43A2C1` releases the target reference at `node+0x12`. Thus cancellation
can re-enter engine handlers, and retaining an order-node pointer across the root is unsafe.

**Exact removal `0x439F80(unit,node)`** is stdcall, `ret 8`. Its two direct callers are
`0x43BA7C` (order controller) and `0x49646F` (Ctrl+D cancellation). `0x439F89` reads the main
head, `0x439F8F` selects the sub-list for flags `0x40000`, and `0x439FA1`..`0x439FAD`
search for the exact pointer. A missing node returns without appending or freeing. A found node
is unlinked at `0x439FB3`..`0x439FB8`, sets `0x10000` when not the main head at `0x439FBC`,
and is destructed/freed by `0x439FC9`/`0x439FCF`. Resource cancellation finds the actual
selected constructor's main-list node in the bounded owned chain and uses this root, so build
variant changes and neighboring positions cannot redirect deletion.

**The paired-reclaim lifetime:** the immediately preceding node must be the resolved reclaim
on the old target at exactly the build position. The build must be unstarted (`byte +6 & 2 == 0`),
without `+0x42 & 0x400000` or an owned object (`+0x52 == NULL`). Being preceded proves it is
non-head; exact removal sets `0x10000`. These conditions disable every callback branch of
`0x43A1F0`. Its remaining `0x489650` call has no calls or virtual dispatch: `0x48965D`..`0x489683`
only unlink this node's target reference from the intrusive unit-reference chain. Therefore the
preceding reclaim stays allocated across build removal and can be passed to the second exact
removal root. This is the only order pointer retained across a root; it has an explicit lifetime
proof. After reclaim removal no node pointer is used. Helpers' Guards remain intact.

**New orders `0x43ADC0(action,shift,unit,target,pos,p1,p2)`** use the same seven-argument
stdcall layout and `ret 28` as `0x43AFC0`. It constructs a `0x56`-byte node through
`0x4B4F10`/`0x43A0C0` at `0x43ADC4`..`0x43ADF4`, preserves existing non-temporary work with
Shift, and replaces removable work without Shift (`0x43AE02`..`0x43AE59`). The native temporary
head cleanup and linking retain their normal descriptor flags (`0x43AE5B`..`0x43AFB1`). It does
not run the mouse root's proximity toggle. New Quick reclaim/build orders use this root, ensuring
that Shift can append a distinct adjacent site. Its 27 direct callers are `0x4029A5`, `0x402E5E`,
`0x402EFA`, `0x403DC5`, `0x408064`, `0x4082B0`, `0x408421`, `0x408453`, `0x4085A1`, `0x4140F3`,
`0x43B09D`, `0x43B120`, `0x4804BB`, `0x487D70`, `0x487DE8`, `0x487E3F`, `0x487E8A`, `0x487F25`,
`0x487FA5`, `0x487FF6`, `0x4880AB`, `0x4880D2`, `0x488110`, `0x488175`, `0x4881D1`, `0x4881F8`,
`0x48821E` (instruction-level call scan).

**Geo identity:** `0x47D684` tests candidate yard bit `0x80`. `0x47D69D` reads the feature ID
at grid-cell `+8`; an ID below `0xFFFB` is checked against the feature count. `0xFFFE` resolves
through cell `+10` (dz) and `+11` (dx), subtracting `(dz*W+dx)*13` at `0x47D6C7`..`0x47D6E4`.
`0x47D708` tests feature-definition byte `+0xFE & 0x20`; no spacing rule separates geo sites.
Resource identity bounds the alias offsets before subtracting, checks explored anchor cells and
feature IDs against the allocation, and requires one unique canonical geothermal anchor among
all G-yard cells. Candidate and queued footprints must name the same anchor; touching multiple
anchors is ambiguous and does not permit cancellation. The generated adjacent-one-cell fixture
exercises the distinction that a +/-16-world-unit comparison cannot establish.

#### The ground Guard's node, and what the order dispatch does with a handler's result [DISASSEMBLED 2026-10-09]

`tools/resource_regression.py` compares a constructor's queue before a plan and after its
cancellation, with a Guard as the preceding order because a Guard does not finish on its own.
What that rests on:

- **`0x406300` Follow_Ground**, the handler of order type `0x12` ("Guarding"; record `0x4FC733`;
  `tacli order guard` on a ground unit), `stdcall(unit, order, events)`, `ret 0xC`. Its exits
  before the state switch: target `+0x16` NULL → 5 (`0x406312`); the guarder's `+0x86` nonzero →
  7 (`0x40632F`); the target definition's `+0x241` bit 11 → 8 (`0x406352`). The state byte `+0x5`
  selects state 0 (`0x406705`) or 1 (`0x406381`); any other state returns 7.
- **State 0** (`0x406705..0x40677D`) writes `+0x36` = (guarder `+0x7E` + target `+0x7E` + 2) × 16
  [INFERRED: the two units' radii] and, from `0x4B6C30(0x10000)` [INFERRED: a random angle] through
  `0x4B70EF`/`0x4B7123` [INFERRED: sine and cosine], `+0x22/+0x26/+0x2A` as an offset of that
  length around the target, then returns 1. So the node's position stops being the issued one at
  its first step. MEASURED 2026-10-09 on Comet Catcher: ARMCK's Guard of ARMMSTOR, read paused
  before that step, held (1300, 55, 1550) and `+0x36` 0; after it, (−115.3, 0, −54.7) and 128.
- **State 1** (`0x406381..0x406702`) returns 3, with an order put in front of itself, in three
  branches: `0x43B1F0` succeeding at `0x4063F5`, reached when the target's `+0xF0` is set among
  other tests [INFERRED: it is attacked]; the resolver's mode 8 (repair, `0x4064F5`) when the
  target's `+0x108` is below its definition's `+0x1FA` [INFERRED: health against maximum], the
  node made at `0x40652B` and put first by `0x43ACB0` at `0x406532`; and a target whose head
  order's flags `+0x42` carry `0x100000` (`0x40658A`) [INFERRED: an assist of its build],
  through `0x43ACB0` at `0x40667A`; that branch is skipped when the target's head order targets
  the guarder itself (`0x406597`), which keeps two units guarding each other out of it. Otherwise it moves to the target position plus the offset (`0x438930` at
  `0x4066E0`), waits 30 ticks (`0x439E80`), and returns 2. **No state-1 path writes
  `+0x22..+0x2D` or `+0x36`.**
- **The dispatch `0x43B87C`** maps a handler's result through the jump table `0x43BAA4`
  (10 entries; above 9 → `0x43BA90`): 0 → state 0 (`0x43B89F`); 1 → state + 1 (`0x43B897`);
  2 and 4 → nothing (`0x43B99F`); 3 → a random wait (`0x43B890` → `0x43B951`); 5 and 8 → unlink and
  free (`0x43B8A8`); 6 → `0x43B975`; 7 → `0x43BA3D`; 9 → `0x43B8E5`, which also returns a node with
  no successor (`+0x4A` null, `0x43B8F8` → `0x43B94B`) to state 0. Follow_Ground returns only 1, 2,
  3, 5, 7 and 8, never 0 or 9, so once its first step has run, nothing returns its node to state 0.

A Guard of an owned unit that is neither damaged, building nor attacked therefore keeps one node
whose compared fields (action, target, position, `+0x36`) are fixed from its first step on.

#### An upgrade preview without grid mutation

`0x47CC30` stamps a footprint; callers `0x43DA41`, `0x48610F`, `0x48630C`, `0x48AA9A`, `0x48B6B9`.
An old yard byte's bit 0 stamps cell flag bit 1 (`2`) at `0x47CE32`; yard occupancy bit 2 (`4`,
or bit 1 when unit byte `+0x10F & 4`) stamps its unit number at `0x47CE23`.
The mask selection at `0x47CD88..0x47CD9D` reads that byte, ANDs with `4`, and chooses yard
mask `2` when set, otherwise `4`.
`0x47D0E0` clears the footprint; callers `0x43DA0F`, `0x47CBD6`, `0x48AA6B`, `0x48B685`.
It clears a matching unit number at `0x47D172`, and clears cell flag `2` for every old yard bit 0
at `0x47D17C`. The site's only read of the candidate yard's bit 0 is `0x47D530`; its occupancy
check at `0x47D554` ignores the supplied unit ID exactly.

Therefore, for a validated uncontested old footprint, clearing candidate yard bit 0 where it
would overlap an old yard bit 0 gives the site's post-clear yard-bit test without touching the
grid. A stack copy of the definition points at this bounded stack yard. Both old and new
footprints and the old live footprint dimensions are validated first. Other yard bits and the
old-ID occupancy exemption retain the engine's tests. Contested footprints are refused because
`0x47D2B9`..`0x47D2C4` calls `0x47E5C0` to restamp neighbors: the simple projection is insufficient
there. No timing or pointer probe supplies this proof.

**MEASURED on retail Comet Catcher:** the patch spans metal cells 88–90 by 81–83, metal byte 223
against SurfaceMetal 3. Manual ARMMEX snaps to footprint origin (1408,1296), and a stationary Alt
returns raw origin x = 1440. The resulting order center is (1432,1320). Advanced ARMACK
queues reclaim of a completed own ARMMEX followed by ARMMOHO, while ARMCK/CORCK receive persistent
Guards and ARMPW's orders stay unchanged. The generated RS maps exercise background 5 with metal
5, background 5 with metal 20 everywhere, and background 20 with an isolated metal-223 patch.
These measurements establish the input cases; their final replay/compatibility gates are tracked
in the landing's notes, not assumed from these values.

**MEASURED on retail Comet Catcher, 2026-10-05:** at eye (1000,950), pointer (588,377),
manual ARMMEX's rectangle at `main+0x2C92` and Quick Build's native order at `unit+0x5C`
both center at (1432,1320), with a 3×3 footprint. At pointer (620,375), the raw x is 1492;
manual placement retains origin x=1472 and queues center x=1496. Before Quick Build shared
the manual complete-target check, it instead queued x=1448: the radius-3 search captured only
part of the deposit. A footprint-radius search from that candidate finds the full-coverage
origin (88,81), so the candidate is not stable. The centered site is accepted by `0x47D2E0`
at the nearer pointer, and both modes use definition 78; the difference is the selection check,
not a footprint conversion or engine refusal. The shared check retains the original radius
and refuses that partial Quick target, leaving the native contextual action available.

**MEASURED native interface selection:** Tab → `PREFS` → `SPEEDS`, set `LEFTCLICK` stage 0,
then `PREV` → `OK` sets `main+0x37EFA` to 0 after launch. `tacli` forces this field's registry
setting to 1 at every launch, so a prelaunch fixture edit cannot establish left-button coverage.
The classic resource regression uses that native menu path and checks the live field before its
clicks; the other mex fixtures require value 1.

#### GUI-only writes found by the resource replay fixtures

The graphics globals base is `0x51F320`. Its event-ring write/read indices `+0x18E/+0x192` are
`0x51F4AE`/`0x51F4B2`, capacity `+0x186 = 20`; `0x4C2E30` and `0x4C2D60` advance them through
mouse-button messages even when a modal GUI consumes the click. The GUI is `main+0x519`:
its latest event occupies `main+0x555..+0x56C` (`GUI+0x3C..+0x53`): six dwords x, y,
wParam, timestamp, message and double-click flag. Thus `main+0x559` is y, `main+0x561..+0x564`
its timestamp, `main+0x565` its message and `main+0x569` its double-click flag, and `main+0x56D`
is `GUI+0x54`, the wParam of the last event the pump popped, while `main+0x533` is inside
its active-screen pointer `+0x18`, and `main+0x585` is the previous hovered/help gadget index at `GUI+0x6C`: `0x4AA596`
and `0x4A00B5` copy `GUI+0x68` into it before selecting that gadget's text at record `+0x33`. The existing event-ring and GUI pump sections establish those owners. A two-image null
twin whose arrows cancel and whose keyboard menu opens without clicks cannot observe every
changed ring/eye bit. A native injected click returns the pointer to screen centre, so the
harness follows each sampled click with a separate move and requires the GUI x/y/message echo
before dumping. It samples GUI-consumed clicks, every valid coordinate bit and several eyes at
one paused GameTime, and requires its carried simulation records to remain equal. A click is two
ring entries, its press and its release, so the twin's whole clicks move the read index by even
steps of the ring of 20 and never change its low bit: the images gate cannot count that bit as the
viewer's. MEASURED 2026-10-09, resource-geo-upgrade at speed 20: a dump at T1 451 taken as a click
of the script reached the engine held a read index of 9 live against 6 played and sought, and
failed the gate on that bit.
The recorder's 60 recorded global slots / 78 dwords are unchanged. Owner-approved `leave`
rows keep the active GUI pointer (`main+0x531`, four bytes), GUI mouse-event clock
(`main+0x561`, four bytes) and bitmap-font pointer (`0x51F524`, four bytes) local.
The three additional UI fields below and the two keyboard indices above also have exact
owner-approved rows, and so, since 2026-10-09, do the mouse-button ring's indices and the GUI's
copy of the popped event (*The mouse-button ring is viewer input*, below). Coordinates and text colours retain their existing classification;
orders and carried simulation records retain their existing comparisons.

The GUI event timestamp is a wall-clock sample. `0x4B5E70`, `0x4B5ED1` and `0x4B5F1D`
call GetTickCount through IAT `0x4FC0DC`, multiply the low 32 bits by graphics `+0xE8`, and
divide by 1000, as `0x4B6340` does. Moves copy the event through `0x4C2360`; buttons preserve
it in the ring. `0x4AB66C` copies the six dwords into `GUI+0x3C`; with an empty ring,
`0x4AB67B` calls `0x4C2340`, whose copy is at `0x4C2356`. The pump copies the event into
its local dispatch data at `0x4AA151`. The original timestamp at `0x51F4C2` and its input
copy at `main+0x2C82` already remain local under the existing mouse/input rows.
Ring flush `0x4C2BB0` resets only the indices, and GUI init `0x4AA850` does not clear
`GUI+0x48`. The window procedure stores the stamp into the event it builds at `0x4B5E9C` (a
move), `0x4B5EF0` (a button) and `0x4B5F3C` (a double click). The record's three readers each
copy all six dwords to their stack (`0x4A44B7..0x4A44D1`, `0x4A6B57..0x4A6B63`,
`0x4A737D..0x4A7389`) and go on with the copy's x and y (`[copy+0]`, `[copy+4]`, read at once);
`0x49FD20(gui, gadget, out)`, `ret 0xC`, which copies the six dwords out whole and makes x and y
gadget-relative, has no direct caller and no stored address in the image. The `leave` row
`guistamp` (`main+0x561`) rests on this. Ordinary native moves and clicks produce another clock sample. A short paused
probe cannot establish a change of every clock bit by waiting for arithmetic rollover.

**The mouse-button ring is viewer input [DISASSEMBLED 2026-10-09].** The ring lives in the
graphics object `0x51F320` (`0x4B6220` returns it): capacity `+0x186`, entries at `[+0x18A]`
(24 bytes each, the six-dword mouse record), write index `+0x18E` (`0x51F4AE`) and read index
`+0x192` (`0x51F4B2`). Its writers: the init `0x4C2BD0`, whose one call `0x4B5A67` passes a
capacity of 20, allocates the entries (`MEM_AllocNamed 0x4D83B0`, `0x4C2BEF`) and zeroes both
indices (`0x4C2C0B`, `0x4C2C11`); the flush `0x4C2BB0` (`0x4C2BB7`, `0x4C2BBD`; one call,
`0x426636`); the push `0x4C2E30`, which advances and wraps the write index (`0x4C2E78`,
`0x4C2E82`) and whose one caller `0x4B5F51` is the window procedure's `0x201..0x206` arms (the
switch at `0x4B5E3B`); and the pop `0x4C2D60`, which advances and wraps the read index (`0x4C2DB5`,
`0x4C2DBF`). The peek `0x4C2DE0` reads both and writes neither. The pop is called only from the
input pass `0x499890` (`0x4999C4`) and the GUI pump's refresh `0x4AB5D0` (`0x4AB654`), the peek from
the same two (`0x499982`, `0x49999C`, `0x4AB60F`). No ring function's address, and neither index's,
is stored in the image (a search of the file for each as a little-endian dword). The 25 other
instructions with a `+0x18E` or `+0x192` displacement (`0x413060..0x48A68F`) form their base with no
fetch of the graphics object in the 25 instructions before them [INFERRED: other objects' fields].
A recursive descent over direct calls and jumps from the step `0x495490` (946 functions reached,
173 of them with an indirect call or jump it does not follow) reaches the pop and the peek only
through `0x4AB5D0` ← `0x4A9FD0` ← `0x4ABD90` ← the players phase `0x464F80`: the watch-mode notice
("You are placed in watch mode because you…", `0x507338`), a dialog box opened at `0x465816` whose
pump runs at `0x4AC035`, which computes no simulation state. It does not reach the push, the init
or the flush.

The GUI's copy of the event: `GUI+0x4C` (the message, `main+0x565`) and `GUI+0x50` (the
double-click flag, `main+0x569`) are written only by the six-dword copies `0x4AB66C` (from the
popped entry) and `0x4C2356` (`0x4C2340`, from the current record when the ring is empty, called at
`0x4AB67B`). `GUI+0x54` (`main+0x56D`) takes the popped entry's wParam at `0x4AB664` and is zeroed
at `0x4AB502` by `0x4AB4E0`, which the process init `0x491200` calls once with `main+0x519`
(`0x491537`). Their readers are the GUI's: the button tests `0x4AB510` (a press or a double click)
and `0x4AB570` (a double click) read `GUI+0x4C`, and `0x4AB5B0` reads `GUI+0x54`, all from the gadget
handlers (call sites above and below); a displacement search over `0x4A0000..0x4AFFFF` finds no
reader of `GUI+0x50`. All five are `leave` rows of `replay-globals.tsv`, the owner's (2026-10-09):
`mousehead`, `mousetail`, `guimsg`, `guidblclick` and `guipopwparam`.

`main+0x531..+0x534` is the full active GUI pointer (`GUI+0x18`), not a unit type or a
simulation pointer. Init zeros it at `0x4AA85E`; `GUI_Load` links its allocated screen to
the old top at screen `+0`, then installs it at `0x4AAC5B`. Both `GUI_Pop` (`0x4A96A2`)
and the pump's own pop (`0x4AA7D5`) replace it with that previous link before freeing
the removed screen. MEASURED resource-refusals: live T301 held `0x0F72F320`, T1891 and
the paused base held `0x0DC035B0`, menu views held `0x0B0B8018`, and the native RSNRACK
selection returned to `0x0DC035B0`. Playback held `0x0B0A0BD0`; fresh seek held
`0x0B0A7AF8`. Different screen allocations do not guarantee that every differing address bit
changes during the null twin.

`0x51F524..0x51F527` is the full current bitmap-font pointer, graphics `+0x204` (the text
globals above). `SetFont 0x4C1420` ignores NULL at `0x4C1425..0x4C1427`, then stores the
pointer at `0x4C142E`. `GetFont 0x4C1440` reads it at `0x4C1445`; `0x4C1450` reads its
first byte as font height. Text drawing `0x4C14F0` passes this font to the glyph rasterizer
`0x4CCF60` at `0x4C16D4`/`0x4C1744`. The adjacent `+0x208/+0x20C/+0x210` fields are text
colour latches, not part of this pointer. MEASURED resource-classic: live T301 held
`0x0DC48C20`, T1441 and all thirteen paused views held `0x0DA31580`; playback, second playing
and seek held `0x0B0EBF90`, `0x0B0EB178` and `0x0B0EB128`. The two reported bytes at
`0x51F525` were the middle of that four-byte pointer. A NULL SetFont call cannot clear it.
Font loader `0x42A320` reads the two native faces through `0x4BBE50` at
`0x42A347`/`0x42A387` and stores the returned allocations at `main+0x391F9/+0x391FD`
(`0x42A375`/`0x42A3A2`). The file loader allocates bytes through `MEM_AllocNamed 0x4D83B0`
at `0x4BBF3C` and reads them at `0x4BBF49`; teardown frees them through `MEM_Free 0x4D85A0`
at `0x42A3BC`/`0x42A3E0`. The font latch holds a process allocation address, not a font ID.
All three focused fixtures' carried simulation, events, round trips and seeks compare equal,
including 39 additional paused views and three final endpoints. The former strict image
failures identify the exact fields above; their owner-approved rows now classify that metadata
explicitly. The full landing tier remains the runtime gate for the resulting list.

`main+0x37E9E` is the command panel's displayed unit type: word stores at `0x41B0D2` and
`0x41B1BC` copy unit `+0xA6`; the no-unit branch zeros it at `0x41B1E1`. The adjacent word
`main+0x37E9C` receives unit `+0xA8` at `0x41B0BE`/`0x41B1A8`. MEASURED: native selection of
ARMACK changes the type to 4 at paused GameTime 1722, which remains 1722 after selection.
The resource-refusals null twin selects ARMACK through native input and requires its displayed
type before a thirteenth dump; that dump's carried simulation must still equal the paused base.

**Additional strict-image failures [DISASSEMBLED + MEASURED 2026-10-05].** The
disk-interrupted combined run also completed three failing image comparisons:
`resource-nozoom` at T1291 names `main+0x37E9E` (live `0xA5`, playback/seek zero),
`resource-plain` at T481 names `main+0x11E7` (live 1, playback/seek zero), and
`resource-upgrade` at T481 names `main+0x55D` (live 4, playback/seek zero).
Each reports one differing byte. The owner subsequently approved exactly the
full four-byte wParam, four-byte gadget latch and two-byte displayed type as
viewer-local fields. Their leave rows add ten bytes without changing any recorded
global slot or comparison tolerance. A new full run remains the validation gate.

`main+0x55D` is the full four-byte native event wParam, `GUI+0x44`, within the
six-dword copies at `0x4AB66C`/`0x4C2356` described above. Bit 2 is `MK_SHIFT`;
the right mouse button is bit 1. An isolated native paused probe at GameTime 172
changes the flags from 0 to 4 using held Shift and a mouse move. The same frozen
probe selects ARMCK, ARMACK and CORCK and observes displayed types 32, 4 and 165
respectively. Thus the old no-zoom value `0xA5` is CORCK's displayed type, not
ARMCK's. The current refusal-only selection samples do not exercise that type
in every resource fixture.

`main+0x11E7` is `GUI+0xCCE`, a four-byte saved signed gadget status. The native
handler `0x4A6AE0`, called at `0x4AA24C`, resolves the active screen at `GUI+0x18`
and its gadget array at `screen+4`, with stride `0x15B`. At `0x4A6BC0` and
`0x4A6BF1` it sign-extends gadget word `+0x138`; the stores at `0x4A6BC7` and
`0x4A6BF8` save that value in the GUI latch. The branches follow left/right
press tests through `0x4AB510` at `0x4A6BA1`/`0x4A6BD2`. That helper tests
`GUI+0x4C` for native messages `0x201`/`0x203` or `0x204`/`0x206` and returns
with `ret 8`. Its other direct callers are `0x4A3A13`, `0x4A3A58`, `0x4A430F`,
`0x4A43B8`, `0x4A44EC`, `0x4A451B`, `0x4A4BCF`, `0x4A4C02`, `0x4A6DD2`,
`0x4A73D7` and `0x4A744F`.

The latch's word reads at `0x4A6C56`/`0x4A6CE3` restore gadget `+0x138` when
the pointer is outside its rectangle. Its dword read at `0x4A6CB2` supplies the
zero/nonzero toggle stored at `0x4A6CBF`. The latter branch tests the held-button
mask through `0x4AB5B0`: that helper reads `GUI+0x54`, ANDs its requested mask
and returns a boolean with `ret 8`; it does not decode a mouse-up message.
Direct callers are `0x4A3AB6`, `0x4A4204`, `0x4A4553`, `0x4A4C3E`, `0x4A6C17`,
`0x4A6C82`, `0x4A6D66` and `0x4A6E62`. The handler calls `0x4A0340` and
`0x4A5F40` at `0x4A6CC8`/`0x4A6CCF` after updating gadget state, and returns
with `ret 12`. A displacement search finds those two stores and three reads of
`+0xCCE`; it does not prove the absence of an aliased or block-copy access.

Related reset `0x4A69D0` clears nonzero type-1 gadget status words over the
native signed count at panel `+0xB6`, starting at gadget 1; it requests a redraw
through `GUI+0xCCA` at `0x4A6A14`. No direct CALL to this entry was found.
`0x4A6A40` applies the same reset only to the supplied gadget's byte-1 group,
and sets the redraw flag at `0x4A6AAF`. Its direct callers are `0x4958AD`,
`0x49591F`, `0x495F82`, `0x49900C`, `0x4990F1`, `0x499159` and `0x49933D`.
Neither reset writes `GUI+0xCCE`.

Three isolated diagnostic attempts did not reproduce the latch changing to 1
with the native Interface `LEFTCLICK` control. Their stage assertions failed
before the paired dump comparisons ran. The frozen-tick flags/type observations
above are established; a paired carried-state proof for this particular gadget
status is not. The native writers establish its GUI owner, not a successful
diagnostic regression. Owner approval of those exact extents is separate from
the native ownership evidence and from the unsuccessful paired-state probe.

`main+0x142CB..+0x142DA` holds four signed dwords for the minimap camera rectangle.
`0x41C3C0` clamps eye `+0x1431F/+0x14323` against native scroll extents `+0x1422B/+0x1422F`
minus viewport dimensions `+0x37E37/+0x37E3B`, then passes `main+0x142CB` to
`0x466B70` at `0x41C426`/`0x41C442`. The indirect stores are `0x466B99` (left),
`0x466BBF` (top), `0x466BE6` (right) and `0x466C0F` (bottom). Left/top use signed
minimap origin `+0x142E7/+0x142E9` plus minimap dimensions `+0x142EB/+0x142ED`
times the eye divided by those extents. Right/bottom add those minimap dimensions
times view cell dimensions `+0x1423B/+0x1423F` times 16 divided by those extents,
then subtract one. The rectangle is passed to `0x4BF8C0` at `0x466B5E` after
the minimap image copy through `0x4C6B70` at `0x466B44`. DISASSEMBLED.

MEASURED combined ten-slot resource-refusals: T691 rectangle `(53,22,66,32)` and
displayed type 4; T1711 and paused base T1759 rectangle `(32,20,45,30)` and type 0.
P/R/Q at T691 held `(12,0,25,10)` and type 0. The small paused camera sweep reached
right coordinates only 25..50, never setting the differing bit 6 at `main+0x142D3`;
paused RSNRACK selection displayed 280 (`0x118`), never setting ARMACK's bit 2.
The null twin now spans the map's native scroll extents and samples ARMACK and RSNRACK selection,
while preserving the exact paused-simulation check and existing comparison list.


The native Stop-button callback plays a UI sound at `0x41A0F1`, returning to `0x41A0F6`,
through `0x47F1A0("immediateorders", 0)` (name pointer `0x50271C`). The Stop branch
compares `"STOP"` (`0x502714`), sets mode 1, clears region bit 3 and dispatches through
`0x48CF30` (`0x419CBC`..`0x419CF2`), then jumps to this shared UI sound tail. The Load and
other unarmed command paths share the tail; it is not a
simulation reply produced by `0x47F780`/`0x47F850`. The resource-priority fixture observed three
such calls in its live [481,1681) count window and none on the puppet. Its unit/player/order,
round-trip and fresh-seek comparisons still agree. The owner approved its exact call-site expected-absent row on 2026-10-05; the command
orders and their effects retain the ordinary recorder checks.

The resource profile-label refresh writes only a label (`gadget+0`, ID 5) on a screen whose
OnCommand dispatcher is one of Impure's own menu handlers. `GUI_Load 0x4AA8F0` allocates
`0x10F57` bytes at `0x4AAA68` and advances `ControlsAry` by `0x3F` at `0x4AAA95`: the remaining
`0x10F18` bytes hold exactly 200 records of stride `0x15B`, including panel record zero.
The signed count at panel `+0xB6` must therefore be 1..199 before a refresh walks records
1..count. The profile label's text is `char[128]` at gadget `+0xB6`, ending before its status
word at `+0x137`. This is the allocation bound, not a readable-page probe.

Resource raw coordinates are captured after the `0x499221` conversion independently of zoom.
With zoom disabled, the installed wrapper calls pristine `0x498DA0` directly before capture;
no zoom conversion or viewport rect write runs. Resource hooks require that producer
installation to be live, in addition to the complete CANBUILD allocation patch.

The resolver's mode-7 branch at `0x43F4C7` requires definition `+0x245` bit `0x20`
(CanGuard) and a nonzero relationship at `0x43F4D4`. Mode 12 starts at `0x43F4F7` and
requires `+0x245` bit `0x400` (CanReclamate) at `0x43F4FD`. Refusal reaches `0x4401DC`,
loads the caller's output pointer into EAX, writes byte zero at `0x4401E3` and returns
`ret 0x14` at `0x4401E7`. Native dispatch at `0x48D0A5`..`0x48D0B1` tests that byte
and skips zero. A nonnull pointer alone is not eligibility. The resource wrapper returns
`-1` for byte zero before any reclaim/build/Guard plan is accepted.

Visuals Undo snapshots the two raw per-mod resource preferences, not their capped plates.
Restoration uses the settings store's global value bounds rather than the active profile
cap; menu refresh derives the effective values again. Raising a cap can therefore recover
a saved higher radius even after Undo.

Profile polling runs inside the existing `0x4C63A0` flip observer, after its game-thread guard
and before census/publisher early returns. `DrawGameScreen` returns from its flip call at
`0x46A3E0`; that game-flip identity freezes policy even without frame packets. Main-menu
selection releases the freeze. Polling is independent of mailbox capacity and of a live render
consumer; `fill_shell` only fills presentation data. No additional byte patch is installed.

DISASSEMBLED GUI stack removal: `GUI_Pop 0x4A9660` installs the previous active link
`[gi+0x18] = [removed]` at `0x4A96A2` before freeing the removed GUIMEMSTRUCT through
`MEM_Free 0x4D85A0` at `0x4A96AF`. Profile refresh uses that current link on the same
GUI/input thread and retains no GUI pointer beyond the refresh call.
