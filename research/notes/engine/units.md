# Units: records, creation, death and transport

The unit and player records, creation, the unit cap, build lists, death, transport and attachment.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### The per-player unit cap — `0x49163F..0x49168B` — mapped by us [VERIFIED 2026-09-11, objdump of the pristine build]

At process start the engine reads the cap out of `totala.ini` and clamps it before
writing `MaxUnitNumberPerPlayer`. The read is in `0x491200`, which WinMain calls once
(`0x49EA62`), so the value cannot be raised in a running process, and `tools/tacli` writes
the file before launch for exactly that reason:

```
49163f: push 0xfa                        ; default 250
491644: push 0x509238                    ; "UnitLimit" (.data; file offset 0x107838)
491653: call 0x49f5a0                    ; GetPrivateProfileIntA("Preferences", key, default,
                                         ;   "<exe dir>\totala.ini") [VERIFIED: ret 8; pushes
                                         ;   "Preferences" 0x509894 then calls the IAT slot
                                         ;   0x4FC0D8, whose hint entry is GetPrivateProfileIntA]
491658: cmp  eax, 0x1f4                  ; 500
49165d: jle  0x491678
49165f:   mov ecx, ds:0x511de8           ; the TAdynmem base pointer
491665:   mov eax, 0x1f4                 ; ANY larger value becomes exactly 500
49166a:   mov WORD PTR [ecx+0x37eec], ax ; MaxUnitNumberPerPlayer
491671:   pop edi / pop esi / pop ebx / add esp, 0x24 / ret
491678: cmp  eax, 0x14                   ; 20
49167b: jge  0x491682
49167d:   mov eax, 0x14
491682: mov  ecx, ds:0x511de8
49168b: mov  WORD PTR [ecx+0x37eec], ax
```

* **The stock ceiling is 500 per player**, whatever the file says; four players hold 2000
  between them. Measured live 2026-09-10 with `tacli roster`: at the cap of 500 a
  fresh skirmish hands out idx 1 to player 0, **501** to player 1, **1001** to player 2.
* The store is a WORD, so the field is an `unsigned short` (where the 6553 figure
  comes from). This path writes at most 500; a saved game's `[Summary]` `maxunits` is stored
  into it unclamped (`0x432646`, below). Raising the cap means
  patching **both** immediates, the compare and the stored value — the community's
  "two offsets, set identically".
* `ActualUnitLimit` (`+0x37EEA`) is not touched on this path.
* **Under the raised limits the ceiling and the default are 1500** (`tagpu_limits.h`, the TADR
  port's landing 2): the default operand `0x491640`, the compare `0x491659` and the clamp-to
  `0x491666` all become `0x5DC`; the floor stays. MEASURED 2026-09-23: with no `UnitLimit` key,
  `+0x37EEC` reads 1500 at the menu; in a four-player skirmish `+0x37EEC` = `+0x37EE6` = 1500 and
  the slot count `main+0x14351` = **15 001**, and 6000 units were created without a refusal.
  The renderer's design point is exactly that game (`TAGPU_PK_DESIGN_SLOTS`, `tagpu_packet.h`;
  [gpu-status](../gpu-status.html) §2.86 and §2.6b).
* **A mod's exe may carry its own ceiling, and it is not the number the mod plays with.** TA Zero
  Alpha 5's `TotalA.exe` holds **5000** (`0x1388`) in both the compare `0x491659` and the clamp-to
  `0x491666`, and 250 at the default `0x491640` [DISASSEMBLED 2026-09-28, md5 `7172e8da…`]. TADR
  writes all three sites from its ini's `UnitLimit` at every launch (`LimitCrack.cpp`), and TA Zero
  ships `UnitLimit = 1500` in `TAZero.ini` and in its 2025 build's `tazero.ini.tdraw` (range
  20..1500), so a TA Zero game runs at 1500. The ceiling pair is marked `lim_file_ok`: Impure
  installs 1500 over the file's number, as it does over stock's ([the takeover](../compat/takeover.html),
  part 2).

**The FBI `commander` tag is `UnitDef+0x245` bit `0x40000`** [DISASSEMBLED 2026-09-28]. The
FBI reader pushes the tag's name (`0x5039F0`) at `0x42CB7D`, reads it with `0x4C46C0`, masks it to
one bit, `shl 0x12`, clears that bit of the dword at `+0x245` and ORs it in, and stores the dword
back at `0x42CBAB` (`89 85 45 02 00 00`). `tagpu_replay_army_values` reads it to leave commanders
out of the replay HUD's army value (*The replay HUD's stats window and the pause icon*).
`tagpu_cat.c` reads it to list a game's living commanders
for the compat suite, beside the seats in the game: a player record (`main+0x1B63`, stride `0x14B`)
with a nonzero first dword and a controller byte `+0x73` of 1 to 4 — local human, local AI,
remote human, remote AI. `0x48664B..0x486660` tests the dword and then 1 and 2 for a local player
(`cmp dword [eax],ebp`, `mov cl,[eax+0x73]`); `0x489C99` compares 3 and `0x428446` compares 4; the string's one other reference, `0x431B73`, is outside the FBI reader and
was not followed.

**Who writes the array's count, `main+0x37EE6` [DISASSEMBLED 2026-09-23].** It is the field the slot
count `10·N + 1` is computed from (`0x4854EF`, a 16-bit `imul` at `0x4854EA` that wraps past 6553,
after which `0x485502` allocates the array too small). These places write it:

- `0x4912F5`, in the process init `0x491200`, copies `+0x37EEC` into it (and `0x491308` on into
  `+0x37EEA`). It runs *before* that function's ini read at `0x491653`, so it copies whatever
  `+0x37EEC` held before the read; the game start below is what carries the configured value.
- **Game start, `0x4971C7`, dispatches on `0x435100` (`[ecx]` of `main+0x391E9`):** mode 2 (skirmish)
  copies `+0x37EEC` at `0x4973CD`; mode 3 (network) copies it at `0x4971F8`, reloads the map through
  `0x4972D6` → `0x435A20` → `0x435DA0` (which writes the `.ota` value, next item), and then, for
  the seat `0x456850` returns (the host <span class="pill pill-warn">INFERRED</span>), overwrites
  it at `0x4973B5` with the word at `+0xA5` of that player's record (`PlayerStruct + 0x27`, the
  pointer at `main + 0x1B8A + i·0x14B`). When `0x456850` returns 10 (`0x4972E2`) that overwrite is
  skipped and the map's value stays. Mode 1 (campaign, `0x49745C`) does not write it at all.
- `0x436037`, inside `0x435DA0`, the map loader: the map's `.ota` `[GlobalHeader]` key `maxunits`
  (`0x4C46C0`, default 200), stored unclamped. A campaign mission plays with this value; a skirmish
  overwrites it at game start. Every retail map sets 200 to 400 (checked over every `.ota` in the
  retail archives, 2026-09-23).
- **Where the host's word comes from.** On the host, `+0xA5` is the battleroom's MAXUNITS slider,
  whose maximum is `ActualUnitLimit − 20` (`+0x37EEA`, read at `0x44A2B2`). `ActualUnitLimit` is
  written from `+0x37EEC` by the process init (`0x491308`) and by the battleroom at `0x449C42` when
  the local record's `+0x97` bit 0 is set; a DirectPlay lobby launch writes it, and the record's
  `+0xA5`, from `0x512D6C` unclamped (`0x449D87`, `0x449D9B`). On a client the record arrives over
  the network.
- **The raised limits hold every writer to [20, 1500]:** the game-start copies take the clamped
  `+0x37EEC`, and three stores go through a clamp stub: `0x436037`; `0x432646` in `0x432610`, which
  reads `maxunits` out of a saved game's `[Summary]` (called from `0x497B29` when the game's
  `main+0x38D6B` TDF has no `BetweenMissions` key) into `+0x37EEC`; and the host's word at
  `0x4973B5`, whose read at `0x4973AE` becomes `movzx eax, word [eax+0xA5]` (the same seven bytes)
  so that the stub sees the whole value. Each 7-byte `mov word [ecx+off],ax` becomes a `call` to a
  stub that clamps `eax` and stores it, and two NOPs. `eax` and the flags are dead after all three.

**A saved game's slot partition must precede allocation** [DISASSEMBLED and MEASURED
2026-09-28]. `0x4974E3..0x49757C` restores saved player configuration, but not the unit limit.
`0x497581` then calls `LoadGameData_Main 0x4917D0`; its call at `0x4918D4` allocates the unit
pool through `0x4854A0`, including each player's range. Only afterwards does `0x497B29` call
`0x432610`. That routine selects `Summary` (pointer at `0x503320` names the string at
`0x503324`), tests `maxunits` (`0x5048F8`), reads it at `0x43263B`, and stores only the
configured cap at `0x432646`. It does not rebuild the already allocated partition.
The saved-unit constructor accepts a requested ID only inside its owner's range:
`0x486026..0x486030` compares the requested slot pointer with the player's begin/end;
failure returns NULL at `0x4861BD`. The restore loop silently skips that unit.

The raised build wraps the byte-matched call at `0x497581` (`E8 4A A2 FF FF`). On an
in-progress save it reads the saved limit, rejects missing or out-of-range values, and sets
both `main+0x37EE6` and `+0x37EEC` before calling the native allocator chain. IDs are not
remapped: COB stacks may contain them. A `BetweenMissions` key leaves the native new-mission
path alone. The loader owns the TDF and these writes; play has not started.
The native thiscall APIs are `0x4B4560(self, section)` (selects or creates a section, returns
1 when found; updates the current-section index at `*self+8` and resets its item cursor),
`0x4B48F0(self, key)` (existence), and `0x4B4800(self, key, default)` (integer value).
The later native readers select their own sections. `0x509348` is the lowercase `summary`
string used by the loader, and `0x504924` is `BetweenMissions`.

The regression driver `tools/cob_legacy_save_probe.py` made a native 32-word retail save at
250 units/player. Loading it into the raised default restored only owner 0 before the fix;
explicitly configuring the loader to 250 restored owners 0, 1 and 3 with identical active
COB records. The ordering fix restored those same three owners and records with the default
configuration unchanged (DLL SHA-256
`96a536e9b5e7e03866f70cd0f196252f939f815dac816a64c864a4c0cab30471`). This establishes retail
old-record import and the partition bug, not every mod's historical save compatibility.

**A type's own cap, `UnitDef+0x15A` [DISASSEMBLED 2026-09-23].** The unit constructor `0x485F50`
(`UNITS_CreateUnit`, `(player, type, …)`) checks it before anything else: a type whose `def+0x241`
bit 23 is clear is refused outright (`0x485FAA`, `je 0x4861BD`, return 0); when the bit is set and
`def+0x15A` is not −1, it counts the player's units of that type (`unit+0xA6`, over the
player's range at `PlayerStruct+0x67..+0x6B`, stride `0x118`) and refuses the unit at the cap
(`0x485FE4`). The load sets `def+0x15A` = −1 (`0x42B169`); the battleroom's restriction store
(`main+0x2A30`) sets both in `0x46E160` (called from `0x46CA60`; that this runs at game start is
<span class="pill pill-warn">INFERRED</span>): the bit when the type's record has `+0x18` set (by
`0x46E4D0`, cleared by `0x46E450`) and `+0x1A` set, the cap from the record's `+0x1C`. The
restriction menu keeps each type as 0..100, **101 meaning no limit** (`0x44CAFD`, operand
`0x44CAFE`: when the per-type lookup `0x46E330` answers −1 from the record's `+0x1C`, the menu
holds 101); a slider moved past 100 is stored as −1 and shown as "No Limit"
(`0x44BEC0` → `0x44BF47`), but **Cancel (`0x44C6FC`) writes the values saved when the menu opened
back as they are** (`0x44C750` → `0x46E550`), 101 included, so after a cancelled menu every
enabled, unlimited type is capped at 101. The raised
limits write the unit limit over that sentinel, as TADR does. A default network game never reaches
it: landing 1's two peers each created 450 of one type through `0x485F50` with none refused.
**Reset (`0x44C5EB`) is a second, visible path:** it writes 100 into every enabled type
(`mov ebx,0x64` at `0x44C62D`, stored through `0x46E550` at `0x44C69B`), and the menu shows 100,
so each type then stops at 100 units in a 1500-unit game. That one is stock's and is left alone:
the player sees the number they chose.

**The pathfinder's budget [DISASSEMBLED 2026-09-23].** `0x40EAD3 mov dword [esi+0x48],0x535` (1333,
operand `0x40EAD6`), the only `0x535` in `.text`, in the pathfinder's per-game init `0x40E9E0`
(called from `0x44F6B3`; it sizes its bitmaps from the map, fills `[pf+0x79+4i]` from each
player's record, `[main+0x1A7F+i·0x14B]`, and stores `+0x54` = `0x18000`). **Its one reader is the
per-tick `0x40EB70`** (called from `0x464F92`): at `0x40EB91` it divides the budget by the u16 at
`main+0x2A3C`, adds that to each active player's credit at `[pf+0xA1+4i]`, and the searches spend
those credits. So it is a budget a tick shared among the players, not one per search. Every 150
calls (`0x511A38`) the same function rewrites a ten-dword table at `0x5119E8` (stride 4, to
`0x511A10`): a multiple of `+0x54` chosen by each player's counter at `0x511A10` over the
per-player count `main+0x1434F`, and the counter is reset. The budget is an `int` that
nothing sizes or indexes, so raising it costs time, not memory. The raised limits make it 66 650,
TADR's value. MEASURED 2026-09-23 at 6000 units: the sim held 57–60 ticks a second at game speed 20
outside the apply frame.

### The unit and player records, byte by byte: the savers, the replay layout's rows and what the values carry [DISASSEMBLED + MEASURED 2026-09-29]

What the demo recorder's replay layout rests on. The recording keeps every byte of a unit record
(stride `0x118`) and a player record (stride `0x14B`) as a value, except the bytes a row of
`tagpu/ddraw/replay-layout.tsv` gives a class; the file, its classes and its check are in the
completeness note, [*Engine records*](../tadr-port/demo-recorder-completeness.md#engine-records-recorded-whole-by-default).
The rows start from the stock savers, the engine's own inventory of a record: a byte the saver
writes raw is a value unless shown otherwise, and a byte it skips or converts needed a reason.
Read from a recursive-descent listing of the pristine exe (the linear listing loses alignment
after the embedded jump tables and hid 63 branch targets, 15 of them call targets, the seat
activation `0x464290` among them) and measured against the 97 PRS1 snapshots of experiments 2
and 3: 65,996 live unit records and 960 player records from four-AI skirmishes on Wine. No
network game was measured.

**The unit saver `0x4876C0`** walks the array and saves every unit with `+0x110` bit 28. Before
the record it writes the COB state (`0x4B1EC0` on `+0x9A`, which carries the model's piece pose,
`0x4B1F77..0x4B1FFB`), every order of `+0x5C` then `+0x60` (`0x43A970`), the mover when `+0x00`
is set (`0x43DD70`) and the resource object at `+0xBC` (`0x4010B0`: two 0x18-byte blocks,
`+0xBC..+0xEB`). Then a 0xB8-byte record:

| record | from the unit | how |
|---|---|---|
| `+0x00` | `[+0x92]+0x20` | the type's name |
| `+0x20`, `+0x21` | `+0xFF` byte, `+0xA8` word | raw |
| `+0x23`, `+0x27` | the order count; `+0x00` ≠ 0 | dwords |
| `+0x2B..+0x40` | `+0x6A`, `+0x6E`, `+0x72`, `+0x64` dwords; `+0x68`, `+0x108`, `+0xB8` words | raw |
| `+0x41..+0x88` | the three weapon slots | per slot `+0x0`, `+0x8`, `+0x10`, `+0x14`, `+0x16`, `+0x18`, `+0x1A` raw; `+0x4..+0x7` skipped; `+0xC` as the weapon's ID `[ptr]+0x10A`; `+0x1B` its low five bits |
| `+0x89`, `+0x8B` | `+0x86`, `+0xF0` | the target's `+0xA8` when it is alive, else 0 |
| `+0x8D`, `+0x8E` | `+0xF9` when the carrier is alive, else `0xFF`; `+0xF4` | bytes |
| `+0x8F..+0xAA` | `+0x58`, `+0x76`, `+0x7A`, `+0x7E`, `+0xAC`, `+0xB0`, `+0x104` | dwords raw |
| `+0xAB..+0xB1` | `+0xF5`, `+0xF6`, `+0xF7` bytes, `+0xBA` word, `+0xF8`, `+0xFA` bytes | raw |
| `+0xB2` | `+0x10E` | a word |
| `+0xB4` | `+0x110` bits 0–11, 13 and 14–25, `+0x10F` bits 0–3 | packed (`0x4879AC..0x487A06`) |

It never writes the slots' `+0x4..+0x7` (`+0x08`, `+0x24`, `+0x40`), `+0x82..+0x85`,
`+0x8A..+0x91`, `+0x96..+0x99`, `+0x9E..+0xA7`, `+0xAA..+0xAB`, `+0xB4..+0xB7`, `+0xEC..+0xEF`,
`+0xFB..+0xFE`, `+0x100..+0x103`, `+0x10A..+0x10D`, `+0x114..+0x117`, `+0x110` bits 12 and 26–31, or `+0x10F`
bits 4–7. The packed word's bits 17–19 come from the stack: `[esp+0xCC] & 0xE0000` is the dword
the previous unit's packing stored, so every unit carries the first one's uninitialised stack
bits, and the restore never reads them (`0x48735E..0x48752A`). The restore `0x487080` is *A saved
game's unit records*, below.

**The weapon slots** are three of 0x1C bytes from `+0x04`. AutoAim `0x49E1A0` and the weapons'
init `0x49E070` walk them with `esi` = unit + 0x1F + 0x1C·i, the slot's last byte.

- **`+0x0`, the target**, two words. A unit is (its slot `+0xA8`, `0x8000`) (`0x48A060`); a ground
  point is (x, z) in whole world units (`0x48A0A0`), a z of `0x8000` stored as `0x8001` so that it
  cannot read as a unit (`0x48A0C6..0x48A0D0`); none is (0, `0x8000`) (`0x48A160`; `0x48A0F0` also
  starts COB `TargetCleared`). The resolver `0x48A190` takes word 1 = `0x8000` with word 0 ≠ 0 as
  `begin + movsx(word0)·0x118`, with no upper bound and no alive test (`0x48A19F..0x48A1D7`, DIS),
  and AutoAim's per-tick target read `0x48A1E0` tests only the target's `+0xA6` (`0x48A295`, DIS),
  writing none, (0, `0x8000`), over a target whose `+0xA6` is 0 (`0x48A2AF`, `0x48A2B4`, DIS):
  *The demo recorder's lossy stream*, below, for why a takeover depends on it. Both setters clear `+0xBA` bits 10–14 (`and 0x83FF`). MEASURED: 3,448
  unit targets, each naming a live slot; every ground target is the point the probe scenario's
  attack order named.
- **`+0x4`** is the vtable `0x4FD6F0` of a two-dword callback {vtable, result} over `+0x4..+0xB`,
  written by the creates (`0x485EC6`, `0x486069`, `0x48628D`) and nothing else. AutoAim hands
  `&slot+0x4` to the `Aim` script as its completion callback (`0x49E30F`, `0x49E379` →
  `0x4B0A70`), and `vtable[0]` `0x481490` sets `+0x8` to 1, so a running COB thread holds a
  pointer into the unit array. MEASURED `0x004FD6F0` in every record.
- **`+0x8`**, aim complete: 0 before `Aim` starts (`0x49E2FF`, `0x49E369`), at fire (the fire
  routine `0x49D580`, `0x49D78E`) and in the shot builder `0x49CC20` (`0x49CCA2`, reached from the
  shot receiver at `0x49D479`, so on a puppet too), 1 through the callback.
- **`+0xC`**, the weapon def `*(def + 0x1EE + 4i)`, written only by `0x49E0B0` in `0x49E070`, from
  the creates. **The restore writes the saved weapon ID into the shared weapon def**
  (`0x487628`, `[wdef]+0x10A`) instead of re-pointing the slot: a no-op when the IDs agree.
- **`+0x10`**, a create-time distance (the ftol of 1.25 × a coordinate difference between the
  `QueryX` and `AimFromX` points, `0x49E0EA..0x49E11B`), written only there (`0x49E11B`, in
  `0x49E070`, which only the creates call: `0x485F04`, `0x4860AC`, `0x4862C4`), so constant after
  the create. Its reader is the ballistic shot builder `0x49CDE0` at `0x49CE62`: distance / weapon
  `+0x68` × gravity `main+0x14263`, subtracted from the projectile's vy `+0x20`
  (`0x49CE85..0x49CE8A`), which runs on a puppet too, through the shot receiver `0x49D270`
  (`0x49D44E`). DIS; **`+0x14`**, the reload
  countdown (set at fire `0x49E4EE`, decremented by AutoAim `0x49E1C8..0x49E1D2`, which runs for
  type-1/2 owners only); **`+0x16`/`+0x18`**, the aim heading and pitch; **`+0x1A`**, the stockpile
  count; **`+0x1B`**: bit 0 aimed, bit 1 a weapon present, bits 2–3 the slot's index, bit 4 set
  at init.

**The pointer fields**, each a row of the layout. An ID is one more than the pointer's row in its
table, never a field of the target, so NULL's is 0 and a pointer to a dead unit keeps its ID. It is
the recording's name for the row, not the engine's index: `+0x92`'s ID is one more than the type
index `+0xA6` holds.

- **`+0x00`, the mover** (0x2F bytes, constructor `0x43DC00`, destructor `0x43DD10`), made by the
  creates `0x485F50` and `0x4861D0` only when def `+0x22F` is 1 (`0x4860BB`, `0x4862CF`), freed
  and nulled by the destructor (`0x486DBF`). The replay HUD's army value counts only the types
  with that byte 1 (`tagpu_replay_army_values`). MEASURED: no type has samples both with and without.
- **`+0x5C` and `+0x60`, the order lists** (nodes of 0x56 bytes). An order goes on `+0x60` when its
  descriptor's flags carry `0x40000` — BuildWeapon `0x0D` and SelfDestruct `0x26`, no other — and
  on `+0x5C` otherwise (`0x43AC6A`, `0x43ACBA`, `0x43AD19`). Every write goes through the head's
  address; no create or destroy stores it directly. MEASURED: `+0x60` was NULL throughout the
  original layout sample. The `secondary-orders` fixture exercises it with native self-destruct.
- **`+0x82`, the sort bucket** (10-byte buckets from `*(main+0x1429F)`, the list head at `+6`,
  128-px cells, the off-map bucket `*(main+0x142B7)`). Only the restamp `0x47CC30` chooses it, from
  the position at that moment ((`+0x72` >> 23)·columns + (`+0x6A` >> 23), `0x47CCA9..0x47CCD8`),
  and the death unlink `0x47CBD0` nulls it (`0x47CC19`). The movement's relink `0x43D6D0` restamps
  only when the footprint's cell or the layer changes (`0x43D898..0x43D8BF`; otherwise it writes
  the position alone, `0x43D8C5..0x43D8D3`), so a unit stays in the bucket of the position where
  its cell last changed. MEASURED: 4,618 of 65,934 units
  on the map (7.0 %) sat in a neighbouring bucket, all within 8 px of a boundary: the bucket is
  state, not a function of the position.
- **`+0x86`, the carrier; `+0x8A`, the first carried unit; and `+0x8E`, which holds two lists**:
  the carrier's next carried unit while `+0x86` is set, otherwise the next unit in the bucket. The
  attach `0x48AB70` takes the unit out of its bucket (`0x48AC57`) and pushes it at the front of the
  carrier's chain (`0x48AC7C`, `0x48AC82`); the detach clears `+0x8E` and `+0x86` (`0x48ACBC`,
  `0x48ACD2`) and pushes it at the front of bucket `+0x82` (`0x48ACDC` → `0x47CB40`), which the
  unit keeps while it is carried. Every bucket-side `+0x8E` write (`0x47CB26` in `0x47CB00`,
  `0x47CB47` in `0x47CB40`, `0x47CBA3`/`0x47CBB0`, `0x47CC13`, `0x47CD0F`/`0x47CD1C`,
  `0x47D0B9`/`0x47D0C6`) sits behind a `+0x86` = 0 test or a caller that made it. MEASURED: every
  free unit's next shares its bucket (47,004) or is NULL (18,941); each of the 51 carried units is
  in its carrier's chain.
- **Both orders reach the simulation.** The bucket walk `0x47E890` visits each bucket of a
  rectangle from its head into a vector, from which RepairPatrol `0x405980` and VTOL_RepairPatrol
  `0x4152F0` pick `candidates[rand(n)]` by the simulation RNG `0x4B6C30` (`0x405A89..0x405A91`,
  `0x415520..0x415528`) and VTOL_SeekGuard `0x410850` takes the first (`0x410ACE..0x410AD2`); the
  unloads take the carrier's head (Ground_Unload `0x406960`, VTOL_Unload `0x4115C7`). MEASURED: of
  13,029 buckets holding two units or more, 1,150 are in ascending slot order, 6,072 descending
  and 5,807 neither. The order is history, so a restore of these lists must restore their order.
  Each machine of a network game orders its own, its moves of a remote unit following that unit's
  packets [INFERRED]. MEASURED 2026-10-13 on a four-seat recording: where two owners' units shared a bucket,
  the owners' own images of them were two chains' tails, or one named a next the other's machine
  had in another bucket, at 97 of 1,476 iterations; the recorder's own chains held at all of them.
- **`+0x92`, the type**: `*(main+0x1439B) + 0x249·[+0xA6]` (`0x485A4B..0x485A75`); the level fill
  `0x4854A0` and the destructor (`0x486DF6`) store the table's base. `+0xA6` is the type's index
  (the creates `0x486086`, `0x4862A2`; 0 at death, `0x486DC7`).
- **`+0x96`, the owner**, written only by the array init `0x4854A0`: NULL for slot 0, with `+0xFF`
  = `0xFF` (`0x485880`, `0x485887`), otherwise the record whose block holds the slot, with `+0xFF`
  = its `+0x146` (`0x485902`, `0x48590E`). A capture never rewrites it: `0x488570` kills the unit
  and sends `0x14` when the new owner is remote (`0x488693`, `0x4886A4`), and otherwise creates a
  new unit in the new owner's block (`0x488700`). The `+0x96` stores at `0x4AA004` and `0x4AA882`
  are the GUI object at `0x51FBA4`.
- **`+0x9A`, the COB object** (0x544 bytes, vtable `0x4FD698`; `0x485D9F`, NULL for a type with no
  script, `0x485DFF`; freed at `0x486D8D`), and **`+0x9E`, the model instance**, whose `+0xC`
  points back to the unit (`0x485E14`), freed through `FreeObjectState 0x45AAA0` (`0x486D9E`). Its
  piece pose is simulation state (the fire point `0x43E240` reads it).
- **`+0xA2`, the chain of references to this unit**: 0x10-byte objects {vtable `0x4FD754`, target,
  next, owner}, linked by `0x4895C0`, `0x489540` and `0x489690` (push-front) and unlinked by
  `0x489600` and `0x489650`; each refuses a NULL target or `+0xA6` = 0. The holders are order
  nodes (the reference at node `+0x12`, the node its owner: `0x43A137`, `0x43A44F`) and the air
  orders' payload objects (0x36 bytes, the reference at `+0x16`, owner 0: `0x44DE80`–`0x44E330`).
  The walkers call `owner->vtable[0](../event)`, which is `node+0x4E |= event` (`0x438870`), so the
  chain's order changes nothing. The death walk `0x489740` (one caller, the destructor's `0x486D75`)
  passes event 8, so on a puppet a replayed death ORs 8 into `+0x4E` of every node that targets the
  dying unit (DIS). MEASURED: all 770 nonempty chains equal the set of captured nodes
  that target the unit; no payload reference was caught.
- **`+0xEC`** is `+0x30` of the resource object at `+0xBC`, which `0x401070` sets from the create
  (`0x485D0A`): the owner's record again, MEASURED equal to `+0x96` in every record. The player
  record's `+0xEC` is a pointer to a heap object of the same class.
- **`+0xF0`, the last attacker**: 0 at create (`0x485C7D`), the attacker at each hit (`0x489DBA`),
  the killer at death (`0x486778`), and in a restore the saved slot, restoring that unit first
  (`0x48720D`, `0x487212`). It is read with no alive test — Follow_Ground `0x406381`,
  VTOL_Follow `0x40FD29`, the death message `0x48662E` and the destructor's kill credit
  `inc word [[+0xF0]+0xB8]` (`0x48699D..0x4869CA`) — so its slot is state after that unit dies;
  the saver writes 0 for a dead one.

**What the values carry** — fields the saver writes raw or skips that the layout keeps as values:

- **`+0xAA`**, the phase of the hover wave, drawn from the simulation RNG at create
  (`0x485D22`, `0x4B6C30(0x10000)`); its one reader is `0x48A684`.
- **`+0xB0`**, a GameTime deadline: + 150 by the repairs (SelfRepair `0x4024E1`, RepairUnit
  `0x4055E9`, RepairUnitNoMove `0x40580E`), + 300 by MobileBuild `0x403EE2`, HelpBuild `0x4041D8`,
  Reclaim `0x404CAC`, Resurrect `0x4050B7` and VTOL_Reclaim `0x414991`, + 900 by Capture
  `0x404692` and ReclaimUnit `0x4049A3`, + 600 by the shot path on the shooter (`0x49C84B`), and
  + 90 by the radar pass when an enemy is near (`0x4676E0`); 0 at create. Its one reader found is
  the economy pass: a unit with `+0x110` bit 11 and not bit 12 pays the cloak cost (def `+0x1DE`
  moving, `+0x1DA` still) only once GameTime has reached it (`0x4017E1..0x401804`).
- **`+0xF4`**, the player index of the last attacker, 10 for none (create `0x485C83`; the
  attacker's `+0xFF` at each hit, `0x489DC0`; `0x44FE40(DPID)` at death, `0x486787`); **`+0xF5`**,
  the kind of the last hit (`0x489DAC`; 7 at completion for def `+0x241` bit 24, `0x41B9FE`,
  `0x486167`), which the reaper hands to the death message (`0x48AFC9` →
  `0x4864B0(unit, +0xF5)`); **`+0xF6`/`+0xF7`**, the HP percentage now and 30 ticks ago
  (`0x48AEA9`, `0x48AEAF`).
- **`+0xF8` with `+0x7A`/`+0x7C`**: where the unit's sight is stamped in its owner's counters — the
  cell and a radius, in ray-fan mode (LosType `main+0x14281` bit 2) the height def `+0x170` +
  altitude clamped to 0..255, otherwise sight def `+0x202` / 32 − 5 clamped to the circle table. In ray-fan mode the re-stamp
  `0x4825B0` leaves a unit in the same cell whose height moved by 5 or less (`0x48261F..0x48262E`),
  so the value is history. Written into a live game, it must go through the unstamp `0x481D50` and
  the stamp `0x482270`, never raw: the counters hold what the old values stamped.
- **`+0xF9`**, the attach piece on the carrier, `0xFF` for none (`0x48AC68`); **`+0xFA`**, a
  recent-damage countdown (240 at each hit, `0x467954`; −1 a tick, `0x48ADFC`); **`+0xFB..+0xFE`**,
  a give lock, one unaligned dword (150 when a type-1/2 owner's unit is given to a type-3 player,
  `0x4885F3`; −1 a tick, `0x48AE0D`), read by the "usable" gates.
- **`+0x10A`**, the water state 0–4, written only by `0x43DB50` in the movement pass: 0 off layers
  1 and 2; 4 above the sea; at or below it, 1 when less than 5 below, then 2 when `+0x70` + def
  `+0x22C` is the sea level and 3 when `+0x70` + def `+0x170` is below it; **when none of these
  holds, the old value stays** (`0x43DB6B`, `0x43DB78`), so the state is hysteresis, not a
  function of the altitude.
- **`+0x10E`**, the activation bits: 0 at create (`0x485B7F`), the saved value in a restore
  (`0x4872D9`), and otherwise written through `0x48B090(unit, mask, on)` (45 call sites), where a
  change of bit 0 starts COB `Activate` or `Deactivate` (`0x48B0EE..`).
- **`+0x10F`** bits 0–3 (COB `0x480B7A`, `0x480B9E`, `0x480BD9`; YARD_OPEN `0x47DAF1`); **`+0x114`**
  bit 0 = def `+0x241` bit 7 at create (`0x485AE1`), read by the AI (`0x4586D6`).

**`+0x110`, bit by bit** (MEASURED share of the live records set):

| bits | set | what | writers | readers |
|---|---|---|---|---|
| 0–1 | 99.6 / 0.4 % | the layer | create `0x485B6D`, `0x486103`; relink `0x43DA3B`; `0x48AA94`; round robin `0x48B6B3` | |
| 2–3 | 37 / 0.1 % | the walk class | `0x43DB3D` | |
| 4 | 0 | selected | the selection code, keyed to `main+0x2A42` or `+0x2A43` (the layout's row lists it); cleared by the sweeps `0x48BD00` and `0x48D846`, the unit tick `0x48AE59`, the give `0x4885FD`, the destructor | |
| 5 | 100 % | selectable | every create; MakeSelectable `0x401CD0`; console `Selectable` `0x41648B`; cleared by the mission loader (`0x487E6E`) and the destructor | the "usable" gates |
| 6–7 | 0 | two marks | `0x48C36F` (in `0x48C320`, no caller), `0x48C621`, `0x48C965`, `0x48D5F6`, `0x48D61A`; cleared by the sweeps in `0x48BD00`, `0x48C030`, `0x48D220`, `0x48D630` and `0x48D790` | the next-unit cycle `0x48D4D0` only (`0x48D511`, `0x48D56D`; from `0x4964D2`) |
| 8–10 | 84 / 24 / 0 % | the viewed player's contact with the unit | the radar pass `0x467440` (`0x4674F9`, `0x467814`) and its callbacks `0x467840`, `0x467960`, `0x467980`; create `0x485C48` | **the simulation**: the acquisition scan `0x40AA40`, every 30 ticks for every local seat (`0x40B2C0` from the players phase, `0x465037`), tests bit 8 (`0x40AB3F`) and bit 9 through `0x465AC0` (`0x40AB11`, `0x465B38`), filling the lists weapon targeting falls back to — a unit of an owner the seat is not allied with (`AllyFlagAry`, `0x40AB05`) whose bit 8 is set goes on the seat's list at `+0x15` through `0x408F30` (`0x40AB3F..0x40AB5C`), so every local seat targets by the *viewed* player's contacts (DISASSEMBLED 2026-10-09); the draw path too (the unit blit `0x459724`, the order-queue draw `0x439740`). **On a puppet host** no simulation reads them: `0x40AA40` runs only through `0x40B2C0` — its two calls are `0x40B2E9`, in `0x40B2C0`, and `0x40AD3E`, in `0x40AD20`, which has no direct call or jump and no pointer anywhere in the file (a byte search for `20 AD 40 00` finds none; DISASSEMBLED 2026-10-24) — and `0x40B2C0` returns at once for a seat with no acquisition record (`0x40B2CC`), the player init makes none for an active type-3 record (`0x46489D..0x4648A5`), and loop 4 of the pass needs a type-1/2 owner. The display does: the cull's `0x465AC0` tests bit 9 (`0x465B38`), the unit draw bit 9 (`0x45972A`), and the minimap's unit dots bit 8 or 9 (`0x466E64..0x466E7D`, with the draw-every-dot flag at `0x466E14..0x466E32`; *The viewed record's work in the players phase*, below) (DISASSEMBLED 2026-10-23) |
| 11 | 0 | cloak on | Cloak_On `0x403091`, Cloak_Off `0x4030C1`; create = def `+0x241` bit 4 (`0x485CCF`) | the economy pass `0x4017E1` |
| 12 | 0 | an enemy near | the radar pass: cleared on every live unit (`0x4674B6`), set with `+0xB0` = GameTime + 90 for a type-1/2 owner's unit whose def has `+0x245` bit 13 when `0x40B0D0(owner, pos, def+0x208)` finds an enemy (`0x4676E8`) | the economy pass `0x4017ED` only |
| 13 | 1.2 % | set while building and at completion: the build step sets it after writing HP (`0x41BC32`, `0x41BCA4`) | `0x41B93D`, `0x41BC32`, `0x41BCA4`, round robin `0x48B4EC`; nothing clears it | none found |
| 14 | 0.0 % | dying | the hit `0x489EE5`, `0x41BA08`, `0x486171`, kill-all `0x486FAE`, round robin `0x48B42F`, debug key `0x495794`; cleared at create `0x485A9E` | the reaper `0x48AFB9`, about 19 "not dying" tests |
| 15 | 0 | a mission's unit | `0x488482` (record byte `+0x23` bit 7); cleared by MakeSelectable | the AI (`0x4071F0`, `0x4089A0`, `0x40AA40`, `0x40B7B0`) |
| 16 | 0.6 % | the altitude fix-up is due | the movers `0x43CC07`, `0x43CFE4`, `0x43D57F`, `0x43D8D3`, `0x43D9FA`, `0x43DA53`; `0x48AAB1`; round robin `0x48B6D8`; create; cleared by `0x43D7EB` and the fix-up `0x48A8A2` | `0x48A870` |
| 17 | 0 | carried with no attach piece | attach `0x48ACA7` (piece `0xFF`), detach `0x48ACC6` | |
| 18–19, 20–21 | 29 / 71, 0 / 100 % | the move and fire states | Standing_MoveOrder `0x4030EF`, Standing_FireOrder `0x403123`, BuildingBuild `0x40296D`/`0x40298C`, GetBuilt `0x402EA3`/`0x402EC2` (the builder's), the AI `0x40887E`/`0x40888E`, `0x487CFC`, the give `0x488721` (clears), create (def `+0x241` bits 0–3) | |
| 22–25 | 2.6 / 2.6 / 0 / 0 % | the build menu's page: 22 has one, 23–25 which (`0x41B200`) | `0x41BDE0`, `0x41BF10`, `0x41C060`, `0x41C180`; create when def `+0x22E` > 1 (`0x485CF1`) | the menu code only |
| 26–27 | 2.5 / 1.8 % | the footprint contest | the stamp `0x47CC30`, the re-claim `0x47C790`, the clear `0x47D0E0`, YARD_OPEN `0x47DAEC`, `0x47DB3A`; cleared at create | `0x47C7A0` (27), `0x47D29C` (26); no other reader in `.text` (DISASSEMBLED 2026-10-12: every `test`/`and`/`or` with `0x4000000` or `0x8000000` and every `shr` by `0x1A` or `0x1B`; the rest read a weapon's `+0x111` or a def's `+0x241`) |
| 28 | 100 % | alive | create `0x485A7B`; cleared by the destructor | |
| 29 | 4.6 % | def `+0x22F` = 0: no mover | create `0x485A9E` | picks the re-claim's walk (`0x47C7B4`) |
| 30 | 0 | def `+0x241` bit 9 | create `0x485B03` | as the carrier's bit in the usable gates (`0x43EB4A..`) |
| 31 | 94.4 % | def `+0x241` bit 16 | create `0x485AC5` | |

**The hover wave runs on the wall clock.** `0x48A870(unit)` — from the unit tick (`0x48AFB0`), the
`0x2C` receiver (`0x48BA4C`) and the creates (`0x486109`, `0x486306`) — runs when `+0x110` bit 16
is set or def `+0x241` bit 12 (`canhover`) is (`0x48A877..0x48A893`), clears bit 16, and acts
only on a unit with a mover on layer 1. With def `+0x241` bit 20 it sets the altitude `+0x6E` to
the terrain height `0x485070(pos)`, or with bit 12 also set to the larger of that and the sea
level less the waterline def `+0x22C`; with bit 19 (`floater`) to the sea level less the
waterline; otherwise it calls `0x48A490(unit)` (`0x48A969`). `0x48A490` fits the unit to the
heights at its footprint's four corners and writes the altitude word `+0x70` (`0x48A797`) and the
tilts `+0x68` (`0x48A7B7`) and `+0x64` (`0x48A7DB`). For a def with bit 12, alive and not dying
(`0x48A60C..0x48A630`), each corner's height gets a wave whose phase is
`((0x4B6340() & 0x1F) + 8·corner) << 11` plus `+0xAA` (`0x48A66A..0x48A684`), its amplitude from
def `+0x192` and the mover, ramped in over 60 ticks from the mover's `+0x2A`
(`0x48A6E8..0x48A6F8`). `0x4B6340` is GetTickCount (IAT `0x4FC0DC`) × `[*(0x51FBD0)+0xE8]` /
1000, a 30 Hz wall clock (`0x4B6340..0x4B6361`). So such a unit's altitude and tilt depend on the
wall clock: two peers compute different values, and a replay cannot recompute them. The altitude
is simulation input — the water state `0x43DB50` reads `+0x70`, and the weapons' water path tests a
`canhover` target's `y` + def `+0x170`/2 against the sea (`0x49AC3B`) — so this is a stock
network-game nondeterminism [INFERRED: whether a peer's result ever differs was not measured].
It is the only wall-clock value found in the unit simulation: every GetTickCount and
QueryPerformanceCounter flow was followed to its stores.

**The player record: eleven of them.** The constructor loop (`0x41D97E`, `ebx` = 11, each through
`0x463BE0`) and the reset `0x4644D0` (`add edx,0x14B; cmp edx,0xCEE; jle`, `0x4644FC..0x46450F`) cover
eleven records from `main+0x1B63`; the saver, the loader (`jl` at `0x46656F`, `0x4662DA`), the
players phase `0x464F80` and the DPID lookup `0x44FE40` loop ten. Index 10 is every "no player"
value — a free record's `+0x146`, `+0xF4` with no attacker, `0x44FE40`'s miss, a meteor's owner —
and the eleventh record at `main+0x2851` is what such an index reads: built with the others and
zeroed by every reset, which nothing then activates [INFERRED: no writer of that record's `+0x73`
was traced]. The reset zeroes each record but `+0x27` (`0x4644EB`: `rep stosd` ×0x52, `stosw`,
`stosb` = 0x14B bytes; `+0x27` restored at `0x464508`), then gives each its own alliance bytes
and a default name; its callers are `0x42734F`, `0x427D73` and `0x4518AC`.

**The player saver `0x4662F0(file)`** writes `Human Player` (`main+0x2A42`), `GameTime` (0x1C bytes
at `main+0x38A37`), and for each of the ten records whose `+0x73` is nonzero a `Player%i` section:
`Energy` `+0x8C`, `Metal` `+0x98`, the six lifetime doubles `+0xAC..+0xD8`,
`PlayerEnergyStorage` `+0xDC`, `PlayerMetalStorage` `+0xE0`, `AddPlayerStorage` `+0x149` bit 0,
`Kills` `+0xFC`, `Losses` `+0xFE`, `UpdateTime` `+0xF0`, `WinLoseTime` `+0xF4`, `DisplayTimer`
`+0xF8`, `Controller` `+0x73`, `Logo` (the key at `0x502390`) = PlayerInfo `+0x96`, `Side`
(`0x504988`) = PlayerInfo `+0x95` and `Alliances` (`0x5073D0`) = the 11 bytes at `+0x108`. The
loader is `0x466050`.

| offset | what | writers | readers, notes |
|---|---|---|---|
| `+0x00` | in use | ctor, reset `0x4645C3`, activation `0x464364`, removal `0x452E67` | every player loop skips 0 |
| `+0x04` | the transport ID: the DPID in a network game, the seat in a skirmish, −1 free | reset `0x4646AA`, activation `0x464381`, join `0x450C39`, `0x447C7E` (in `0x447B10`), removal `0x452E6D` | the dispatcher routes on it (`0x453DF3`, `0x453E5A`); the death path turns the killer's player index into it and back (`0x44FFD0` at `0x486625`, `0x44FE40` at `0x486782`, into unit `+0xF4`); MEASURED = `+0x146` = the slot in every active skirmish record |
| `+0x08` | join time, `0x4B6340()` | `0x451356` | `0x447CAB` calls the leave `0x453010` once more than 30 have passed |
| `+0x0C` | a number the host assigns | dispatcher `0x455DD3`, `0x455F41`; `0x4505F0`, `0x4506F6`; activation, removal | `0x4532B7` removes the records that share a leaving human's [INFERRED: the machine that hosts them] |
| `+0x10` | messages in this receive pass | zeroed for all at each dispatcher call (`0x453D78`), `0x45485C` | |
| `+0x14` | ping: GetTickCount (`0x45671B`, raw milliseconds) less the message's stamp | `0x456743` | |
| `+0x18` | the owner peer's GameTime | the `0x2C` sender `0x48B769`, receiver `0x48B963` | the pacing `0x495230` takes the lowest over active type-3 records with units alive (`0x4952CF..0x495303`) |
| `+0x1C` | last receive, `0x4B6340()` | `0x454856`, join `0x450C41` | the timeouts `0x453C20`, `0x453839`; `0x453C20`, from the dispatcher's tail (`0x455F64`), reads every active type-3 record's (`0x453C85`) unless SoftwareDebugMode `main+0x37F2F` bit 0 is set, and only restamps its clock while `main+0x38A51` bit 0 is |
| `+0x20` | load progress | `0x456E6E` | the load wait `0x497CE0` only |
| `+0x21` | session flags | reset (bit 0, `0x4646A4`), `0x45134B` and `0x42818E` (bit 1) | `0x451345`, the network level setup `0x49723B` |
| `+0x22` | leave reason | `0x453010`; cleared by the activation, the join, `0x41F932`, `0x443FA7` | |
| `+0x23..+0x26` | — | no writer found | MEASURED 0 |
| `+0x27` | PlayerInfo, 0xB9 bytes: `+0x94` the type's mirror, `+0x95` Side, `+0x96` Logo, `+0x97` share bits, `+0x9B` bit 6 a watcher, `+0xA1`/`+0xA3` network starting resources ÷ 100 | ctor `0x463C14`; kept by the reset | |
| `+0x2B`, `+0x49` | names, 30 bytes each | reset, activation, join, dispatcher | MEASURED: the bytes after the NUL are kept |
| `+0x67`/`+0x6B` | the unit block's first and last unit, inclusive | `0x4858BD`, `0x4858E0` in `0x4854A0` only | MEASURED begin + 0x118·`+0x6F`/`+0x71` in all 960 records, 500 units a player |
| `+0x6F`/`+0x71` | the block's first and last slot | `0x4858ED`, `0x4858F8` | |
| `+0x73` | the controller as the local peer sees it: 1 and 2 local, 3 remote | ctor, reset, activation `0x464332`, the join (3, `0x450B16`), `0x465FB0`, loader; in play through `0x463C60`: the console's AI toggle `0x416280` flips 1 and 2, a leave writes 0 (`0x45401B`, `0x454054`) | the economy pass and the `0x28` send take 1 and 2 only; 3 gets no AI object and is the pacing's lag source |
| `+0x74` | the AI object | init `0x4648C1` (not for an active type 3); activation 0 without a free; reset; teardown | the players phase (`0x46502A`) |
| `+0x78` | control groups, 10 × 0x20 | `0x480190`, `0x4801F0` | kept by `0x480250(unit, group)` from unit `+0xAC`; the restore rebuilds it (`0x487270`) |
| `+0x7C..+0x88` | sight counters; the grid's width, height and length | init `0x464849..0x46487B`; teardown | `0x4827B0`, `0x4816A0` |
| `+0x8C..+0xE0` | the economy: the saver's keys | the economy pass `0x401360` (types 1 and 2), the `0x28` receiver, the transfers, the level start `0x465E30`, the ATM cheat `0x4170C0` | the resource bars of DrawGameScreen (the energy fill `0x46912E` over `+0xA4`, the metal fill `0x469412` over `+0xA8`); the replay HUD, from the playback's image of the record (`tagpu_replay_players`): floats energy `+0x8C`, income `+0x90`, expense `+0x94`, metal `+0x98`/`+0x9C`/`+0xA0`, storage maxima energy `+0xA4` and metal `+0xA8`. MEASURED 2026-10-08 in the HUD's state dumps of a skirmish recording: the maxima read 2,000 metal and 5,000 energy at the start, as the engine's bars showed; the replay HUD's stats walker, from the recording's image (`tagpu_hudmarks.c`): income `+0x90` and `+0x9C`, and MetalWasted, the double `+0xD4` |
| `+0xE4`/`+0xE8` | share limits | SetShareMetal `0x4193C5`, SetShareEnergy `0x419485` | the auto-share `0x457D30`, network only |
| `+0xEC` | a 0x34-byte resource accumulator | init `0x4647D7` | |
| `+0xF0` | UpdateTime, the 30-tick pass | init `0x464715`; the players phase adds 30 (`0x465092`) | |
| `+0xF4` | WinLoseTime | init, loader | the saver only |
| `+0xF8` | DisplayTimer, a GameTime deadline | init `0x464765` (GameTime), loader, DrawGameScreen `0x468F87` | a write on the draw path, for the viewed record only: 30 is added while it is below GameTime, and then the resource bar's rates refresh (`0x468F73..0x468F9A`) |
| `+0xFC`/`+0xFE` | kills, losses | the destructor `0x4868C1`, `0x486906`, `0x486A33`; the `0x28` receiver | the replay HUD's stats walker, from the recording's image (`tagpu_hudmarks.c`), as words |
| `+0x100`/`+0x102` | −1 at init | `0x464817`, `0x46481E` | no reader found |
| `+0x104`/`+0x106` | commander kills, losses, words | the destructor `0x486981`, `0x486996` (`incw [eax+0x106]` on the dead unit's owner record, unit `+0x96`), `0x486A8C` | the lead ranking in mode 2; the replay HUD's skull, `+0x106` above 0 (`tagpu_replay_players`). DIS |
| `+0x108`/`+0x113`, 11 each | alliances and their mirror: byte j of `+0x108` is 1 when this record allies record j; the reset and the activation set the record's own byte, so a record always allies itself | `0x452960`, the lobby, the removal, dispatcher `0x454D2A`, the seat fill `0x47A760` (the store at `0x47A890`), the reset `0x4645B3`, the activation `0x464290` | the allied victory `0x490080`, the skirmish victory `0x490230`; the replay HUD's teams, which join two records when each one's row marks the other (`tagpu_hudstate.c`) |
| `+0x11E`/`+0x129`/`+0x134`, 11 each | the final-statistics handshake | `0x45591D`, `0x455930`, `0x45768A` | |
| `+0x13F` | the lobby team, 5 for none | activation, reset, the lobby | lobby code only |
| `+0x140`/`+0x144` | units created, alive (`+0x144` a word: `decw` at `0x486DFC`, `cmp bx` at `0x486E09`) | the creates, the destructor | "out" = none alive of some created; the replay HUD's units and its dimmed "out" row (`+0x144` = 0); its marker walker's defeats and its stats walker's units, from the recording's image (`tagpu_hudmarks.c`). DIS |
| `+0x146` | own index, 10 when free | ctor, reset, activation, the lobby's renumbering | copied into unit `+0xFF` |
| `+0x147` | start position | activation, `0x45517B`, `0x456C96` | the network level setup |
| `+0x148` | scoreboard rank | activation, the destructor; the scoreboard `0x4948E0` (`0x494E27`), whose one caller is DrawGameScreen (`0x469F65`), compacts the ranks while the scoreboard is shown (`[0x51F2D8]` > 0, `0x494986`). In a network or mode-2 game (`0x435100` = 3 or 2) the destructor's rank update reads a ranked record's `+0x148` (`0x486B2D`), computes its new rank from the other records' scores, then increments every record's rank that is at least the new one and below the old one — a loop over all ten records with **no activity test** (`0x486BE7..0x486C13`), so unseated records at rank 0 become 1 when the new rank is 0 — and stores the new rank (`0x486C17`). DISASSEMBLED; MEASURED 2026-10-05 in a four-actor network recording: rows 4..9 (unseated) went 0 → 1 at one death | the destructor's lead message (`0x486B2D..0x486C17`) |
| `+0x149` | bit 0: base storage counts | the level setups, the loader; cleared at init and when the commander dies (`0x486518`) | the economy pass (`0x401941`); no writer of bits 1–15 |

**Negative results.**

- **No caller found:** the create variants `0x485E50` and `0x485E90`, the order-list append
  `0x43B730`, the bucket move `0x47CB60` (which only Impure's seek calls, *The demo recorder's globals, projectiles and seek*), `0x48C320` (sets `+0x110` bit 6), `0x497080`, and the
  lobby's out-of-line record swaps `0x4453A0` and `0x445450`.
- **No writer found** but the fill or the reset: unit `+0xB4` and `+0x10F` bits 4–7; player
  `+0x23..+0x26` and `+0x149` bits 1–15. **No reader found:** unit `+0xB4`, `+0x100`, the slots'
  `+0x10`, `+0x110` bit 13; player `+0x100`/`+0x102`.
- **The heap dummy unit** `0x421F20` allocates 0x118 bytes at `main+0x1420F` for the feature
  drawing (`0x46A721`); three references in all, outside the array, never an attacker, target or
  chain member.
- **Clocks in the unit record:** the one wall-clock value is the hover wave; the one GameTime stamp
  is `+0xB0`. The countdowns (`+0xFA`, `+0xFB`, the reload) count simulation ticks.

### The demo recorder's unit frame: the engine rules it predicts from [DISASSEMBLED + MEASURED 2026-10-08]

Recording format 25 carries a stream block's unit deltas as residuals against a prediction, the
*unit frame* (`tagpu/ddraw/src/tagpu_reccodec.c`; the bytes in the skirmish slice's
[*The file*](../tadr-port/demo-recorder-skirmish-slice.md#the-file), the plan and its measurements in
[making recordings smaller](../tadr-port/demo-recorder-size.md)). Where a rule exists the prediction
is the engine's own, and the capture writes the constants the rules need into the recording's
header once, on the game thread at the recording's start (`header_codec`, `tagpu_recorder.c`):
RT_PHYSICS (gravity, sea level, the sight rule, the sort buckets' columns), one RT_AIR row per
aircraft type, and RT_TYPES -- the ground step's slope table and one row per unit type (its
maximum HP, sight height, maxvelocity, brake rate and acceleration). Every rule
and constant is a predictor's: a wrong one costs size, never data, because the writer writes a
block in the unit frame only when a second decoder gives its records back. Everything below was
re-read in `pristine/TotalA.exe.pristine` on 2026-10-08 (the hit points, the percentage, the
bucket, the ground speed and the slope table on 2026-10-12) unless it is marked as the size
research's (`research/sizelab/`), which MEASURED each rule's exactness on its recordings.

**What the capture reads.** The defs are `*(main+0x1439B)`, `0x249` bytes each, and the loop over
them runs below UNITINFOCount `main+0x1438F`, the count the engine's own loops over the defs run to.

| read | what | established by |
|---|---|---|
| `main+0x14263`, i32 | gravity | read by the bank `0x43D186` and 13 other instructions in `.text`, stored by none by displacement (`replay-globals.tsv`, a `leave` row). The air war's 8155 was also fitted from its banks by the size research (unique: 588,252 exact against about 503,000 for 8154 or 8156) |
| `main+0x1427F`, u8 | sea level | `mov al, [ecx+0x1427F]` at `0x48280D` after `xor eax, eax` at `0x48280B`, a zero-extended byte, in the sight emit (below); 70 byte accesses in `.text`, one of them a store (`0x416AA3`) |
| `main+0x14281` bit 2 | LosType's ray fan: which sight path a unit takes | the emitter's test `0x4825BA..0x4825CE`; RT_PHYSICS stores the bit as it stands at the segment's start, since the typed command LOSType can change it during a game |
| def `+0x241` bit 11 | the unit moves by the air step | the movement pass `0x43DD36..0x43DD44` (below) |
| def `+0x170`, u8 | the sight emit's height term | `mov cl, byte [def+0x170]` at `0x4827FB`. The water state reads the same offset as a signed **word** (`movsx` at `0x43DBBA`); the sight emit's byte is the one RT_AIR carries |
| def `+0x192`, i32 | `maxvelocity` | the def loader stores it at `0x42C151` (key string `0x503E3C`); the air step reads it at `0x43D301`; also read at `0x43CC76` (`imul`, in `0x43CC20`) and `0x43D9A2` |
| def `+0x19A`, i32 | `brakerate` | stored at `0x42C165` (`0x503E30`); the ground step `0x43CD46` and `0x43D02F`; the air step as a float (`fild` at `0x43D3BC`) |
| def `+0x19E`, i32 | `acceleration` | stored at `0x42C180` (`0x503E20`); the air step `0x43D2FB`; the ground step `0x43D0AC` |
| def `+0x1A2`, i32 | `bankscale` | stored at `0x42C1AE` (`0x503E14`); the bank `0x43D1AF` |
| def `+0x1A6`, i32 | `pitchscale` | stored at `0x42C1C5` (`0x503E08`); the bank's twin `0x43D1DB` |
| def `+0x1FA`, u32 | the hit points at most | the build step loads it as a dword (`0x41BB26`) and widens it with a zero high dword to a qword `fild` (`0x41BB2C`, `0x41BB46`), so unsigned; its cap compares unsigned (`0x41BC79..0x41BC81`); the unit tick divides by it unsigned (`div` at `0x48AE8D`) |
| `0x505200`, 11 signed bytes | the ground step's slope table, `25 55 70 85 100 100 75 50 25 20 15` in the pristine exe | its only reference in `.text` is `movsx eax, byte [eax+0x505205]` at `0x43CC61`, the index clamped to −5..5 first; RT_TYPES copies the eleven bytes from the running image, so a patched executable's table is the one a recording carries |
| `main+0x142A3`, u32 | the sort buckets' columns | LoadMap sets it to `((main+0x14223 << 16) + 0x7FFFFF) >> 23` and the rows `main+0x142A7` the same from `main+0x14227` (`0x482C6C..0x482CA6`), the buckets' count rounded up to a multiple of 8 (`0x482CAE..0x482CBA`): the map's size in 128-unit cells, rounded up. Moon Quartet's 610-cell width gives 77, MEASURED by reproduction |

The loader takes each of the five through `0x4C4800` (the key pushed at `0x42C129`, `0x42C14B`,
`0x42C16F`, `0x42C18C`, `0x42C1A8`; `damagemodifier` follows at `0x42C1CF`); the values are 16.16
per the size research's `air-challenge/lib_int/airdefs.py` [the conversion in `0x4C4800` not
re-read here].

**The rules, by the image's fields** (the unit image is the unit record's `0x118` bytes, then the
mover's `0x2F` at `+0x118`; mover offsets below are the mover's own):

- **The movement pass `0x43DD20`** (thiscall on the move object, the unit as its one argument,
  `ret 4`; callers the unit tick `0x48AFAA` and the `0x2C` receiver `0x48BA46`) calls the class
  step `[vt+0x08]` (`0x43DD28`), then loads the def `[unit+0x92]` (`0x43DD30`), shifts
  `[def+0x241]` right by 11 and tests bit 0 (`0x43DD36..0x43DD41`): set, the air step `0x43D290`
  (`0x43DD46`, its only caller); clear, the ground step `0x43CD20` (`0x43DD4D`); then the placement
  `0x43D6D0` (`0x43DD55`). The unit frame takes a type as an aircraft when RT_AIR has a row for its
  type index (the image's `+0xA6`, one less than the def's ID at `+0x92`).
- **Heading** `+0x66` += the mover's turn rate `+0x24`, as a word (`add word [unit+0x66], cx` at
  `0x43CFDB`, `cx` loaded at `0x43CFD1`).
- **Ground velocity**: `TurnXLookup 0x4B70EF` and `TurnZLookup 0x4B7123` off the sine table
  `0x509F00` times the speed (*`DrawRangeCircle 0x438EA0`*, below, and the size research's
  `0x43CC20`, `0x43CCDB..0x43CD13`: vx = −sin(h)·speed, vz = −cos(h)·speed, MEASURED exact in
  4,362,632 of 4,362,632 live mover unit-ticks).
- **The air step's damping** (`0x43D2F1..0x43D339`): the def from the unit at `[esp+0x5C]`, its
  acceleration (`0x43D2FB`) and maxvelocity (`0x43D301`), the acceleration also kept as a float
  times 2^−16 (`fmul [0x4FD2D0]`, `0x43D310`), then `d = 0x10000 − (acc << 16) / maxv` through
  `_allshl 0x4E44F0` (`0x43D326`) and `_alldiv 0x4E4440` (`0x43D32F`, `0x43D334..0x43D339`). The
  rest of the step — the drift limit against brakerate, the vertical clamp, the turn, the steering
  term toward the course's target point, the speed — is the size research's reading
  (`0x43D2F1..0x43D47D`, *Aircraft* in the size note) and was not re-read here; the steering term's
  target point lives in the course `0x490690`, not in the unit record, so the velocity is the one
  air field the image does not determine.
- **`0x43D0D0`, the smoothed acceleration and the bank** (thiscall on the move object (unit,
  &Δv), `ret 8`; callers the air step's end `0x43D6B3` and `0x43D210`'s layer-1 branch
  `0x43D25B`): each of the mover's `+0x14`, `+0x18`, `+0x1C` becomes `(A · 0xF333) >> 16` through
  `_allmul 0x4E4400` and `_allshr 0x4E43D0` (`0x43D0D9..0x43D131`), then gains Δv
  (`0x43D134..0x43D159`); `0x4B7173(heading, &A)` turns it by the heading [INFERRED from the size
  research's fit: x′ = x cos − z sin]; `q = (gravity << 16) / 0xCCD` (`0x43D186..0x43D1A4`); the bank
  `+0x64` = `0x4B715A((−x′ · bankscale) >> 16, q)` (`0x43D1A6..0x43D1CA`) and `+0x68` the same with
  pitchscale (`0x43D1CF..0x43D1F6`) [INFERRED: `0x4B715A` is an atan2 to a heading; the size
  research's integer bank matches the engine on 588,451 of 588,452 airborne iterations, MEASURED].
  On a landing (`0x43D210` with layer 1) Δv is zero [the size research's reading; no recording
  has met one].
- **Position** `+0x6A`/`+0x6E`/`+0x72` += velocity, all three for every uncarried unit, ground or
  air: the placement `0x43D6D0`, which the movement pass calls after either step (`0x43DD55`),
  branches before the sum only on the carried test (`[unit+0x86]`, `0x43D6DD..0x43D6E5`), then
  adds the mover's velocity `+0x08`/`+0x0C`/`+0x10` (`0x43D7FF..0x43D81F`; the altitude loaded at
  `0x43D80D`, added at `0x43D813`) and stores the three on each path that changes them
  (`0x43D8C5..0x43D8D0`, `0x43D9F1..0x43D9F7`, `0x43DA20..0x43DA35`). The unit frame predicts the
  altitude only for an aircraft; a ground unit's is a generic lane, the codec's choice and not the
  engine's. The **move stamp**, the mover's `+0x2A` = GameTime when the position or layer changed
  (`0x43D85A..0x43D865`).
- **The footprint cell** `+0x76`/`+0x78` = `(x − (w << 19) + 0x80000) >> 20` with the size
  `+0x7E`/`+0x80` (`0x43D86F..0x43D895`), compared with the stored cell at `0x43D898`.
- **The claim stamp `0x440A70(unit)`** (stdcall, `ret 4`): with a mover, it reads the mover's
  `+0x26` (`0x440A83`) and stores GameTime `main+0x38A47` there as a **dword** (`0x440A8B..0x440A91`),
  then re-marks the unit's rectangle in each of the 32 movement classes (`0x512358`, stride `0x20`)
  whose watermark `+0x1C` is above the old stamp (`0x440A94..0x440AB1`, through `0x440830`); with
  none, it re-marks every present class (`0x440AB9..0x440ADB`). Its one caller is the end of the
  footprint clear `0x47D0E0` (`0x47D2CA`), so the stamp moves when the footprint does. The image
  sees the step before its iteration's label: the iteration labelled `gt` holds `gt − 1`.
  `0x440A70` is not the field's only writer: the footprint stamp `0x47CC30` stores GameTime there
  too (`0x47CC54`), the pathfinder's `0x440AF0` stores GameTime (`0x440B30`) and puts the old value
  back (`0x440BC4`), the mover's constructor `0x43DC00` zeroes it (`0x43DC16`) and the restore
  `0x43DD70` writes it from its copy (`0x43DEC5`) — each DISASSEMBLED, the table in
  [the movers](../tadr-port/demo-recorder-s2-movers.md). The unit frame predicts only the move of the
  footprint or the layer; a stamp from another writer is a residual, which costs size only.
- **The sight emit `0x4827B0(unit)`** (stdcall, `ret 4`; callers `0x43DA59`, `0x464DBF`,
  `0x465053`, `0x48AAA0`, `0x48B6BF`) builds a record on its stack (`0x4827B3..0x48281B`): `+0`
  the owner `[unit+0x96]`, `+4` the address of `unit+0x7A`, `+8` def `+0x202` as a word
  [INFERRED: the sight distance], `+0xA` def `+0x170`'s byte, `+0xC` the address of `unit+0xF8`,
  `+0x10/+0x14/+0x18` x, altitude and z; it raises the altitude to `(sea level + 1) << 16` by a
  signed compare (`0x48280B..0x48281B`) and calls the emitter `0x4825B0` (`0x482824`).
- **The emitter `0x4825B0(rec)`** tests LosType bit 2 (`0x4825BA..0x4825CE`). **Set** (the ray
  fan, `0x4825D4..0x48261C`): the column `sx = movsx(x >> 16) >> 5`; the height `h = clamp(byte
  +0xA + movsx(altitude >> 16), 0, 255)`; the row `sz = (movsx(z >> 16) − (h >> 1)) >> 5`, all
  arithmetic shifts. It returns writing nothing when both cells equal the stored ones and the stored
  height byte is within 5 of `h` (`0x48261F..0x48262E`); otherwise it stores the cells
  (`0x48264C`, `0x482652`), bounds them unsigned against `main+0x14293` and `main+0x14297`
  (`0x48265B`, `0x482663`) and stores `h` at `unit+0xF8` inside (`0x48266E`), 0 outside
  (`0x4826AC`). The unit frame predicts sx as `x >> 21` for every unit, and sz this way for an
  aircraft from its RT_AIR row's height byte and for a ground unit from its RT_TYPES row's: the
  sight emit raises every unit's altitude to the sea's, not an aircraft's alone (there is no
  air test in `0x4827B3..0x48281B`); a type with neither row keeps a generic sz. **Clear** (the circle, `0x4826B7..0x48271A`):
  the radius index `trunc(+8 / 32) − 5` clamped to `[0, count − 1]` of the table
  `*(main+0x1485B)`, and the corner from `trunc(x / 2^21)` and `trunc(z / 2^21) −
  trunc(altitude_hi / 64)`, less the circle's offset from `0x4B7F30` — truncating divisions and a
  table lookup the unit frame does not predict: with RT_PHYSICS's sight byte 0, sx and sz are
  generic lanes.
- **The countdowns**: weapon slot reloads (AutoAim `0x49E1C8..0x49E1D2`) and `+0xFA`/`+0xFB`
  (`0x48ADF0..0x48AE0D`), −1 a tick down to 0.
- **The hit points `+0x108` while built: the build step `0x41BA60`** (stdcall (builder, unit,
  amount), `ret 0xC`). It returns 0 at once when the fraction left `+0x104` is 0.0 (`0x41BA6D`,
  compared with `0x4FCC88` = 0.0) or the amount is 0; otherwise fn = fo − amount / def `+0x1EA`
  (`0x41BAC7..0x41BAD7`), clamped to [0, 1] (`0x41BAD9..0x41BB24`; `0x4FCC8C` = 1.0) and kept as
  a single at `[esp+0x24]`. With M the def's `+0x1FA` it takes `trunc(fn · M)` and `trunc(fo · M)`
  through `0x4E43A0` (`0x41BB72`, `0x41BB7D`) -- `fistp qword` under a control word with RC set to
  chop, so a truncation, of each single times M at the FPU's precision -- and their difference d
  (`0x41BB8E..0x41BB90`). For an amount below 0 (an unbuild: `0x41BCD0`, called only by GetBuilt
  at `0x402F6B`, passes `−(def +0x1EA · n) / def +0x186`; a reclaim never comes here, *A reclaimed
  unit is never a nanoframe*) it stores fn (`0x41BC16`) and the HP `max(movsx(+0x108) + d,
  0)` (`0x41BBFA..0x41BC22`, `setle`/`dec`/`and`). For an amount of 0 or more it asks the builder's
  ledger (`0x4011C0`, `0x41BC60`) and only when that grants it stores the HP `movsx(+0x108) + d`
  capped at M by an unsigned compare (`cmp eax, ecx; jb` at `0x41BC7F`) and fn (`0x41BC96`); both
  paths set `+0x110` bit 13. At fn = 0.0 it calls the build done `0x41B8D0` (`0x41BCBF`). The unit
  frame predicts the HP, for a type with an RT_TYPES row and a nonzero M, from L1's and U's
  fractions whenever they differ: the cap when fn < fo, the floor otherwise; the product is exact
  in integers (`ex_fm`) for M < 2^29.
- **The HP percentage `+0xF6` and its copy `+0xF7`: the unit tick** (`0x48AE5F..0x48AEB5`). When
  GameTime `main+0x38A47` is a multiple of 30 (`div ecx` by 30, `0x48AE71`) it moves `+0xF6` to
  `+0xF7` and writes `movsx(+0x108) · 100` (`lea`, `lea`, `shl 2`: 32 bits) divided unsigned by def
  `+0x1FA` (`div` at `0x48AE8D`, `edx` still the remainder's 0), clamped as a signed value to 0..100
  (`0x48AE93..0x48AEA1`) -- so a negative HP divides to a large quotient and clamps to 100 unless M
  is 1. The only other writers of either byte are the creates (`0x485B67`, `0x485B79`) and the saved
  game's unit loader (`0x487296`, `0x4872A3`), a text scan of `.text` for byte stores at those
  displacements. The unit frame predicts both at the phase (the iteration labelled gt with gt − 1 a
  multiple of 30) and their last values otherwise, the rule always the winner; a resting unit
  must hold `+0xF7` = `+0xF6` = the percentage of its HP, the fixed point of the phase.
- **The sort bucket `+0x82`**: the restamp `0x47CC30` (*`+0x82`, the sort bucket*, above) stamps
  the claim word (`0x47CC54`), tests the cell against 0 (`jl` at `0x47CC5E`, `0x47CC6B`) and the
  footprint against the grid's last column and row (`fx + fw` against `main+0x14233`, `fz + fh`
  against `main+0x14237`, signed, `0x47CC77..0x47CCA3`; stock's `jge` at `0x47CC8B` and
  `0x47CCA3` is `jg` under Impure's always-on `fix_last_cell`, *The grid stamp's last column and
  row* below, so a footprint ending on the edge passes), then forms the bucket
  `*(main+0x1429F) + 10 · ((z >> 23) · [main+0x142A3] + (x >> 23))` (`0x47CCA9..0x47CCD8`) and
  writes it when it differs (`0x47CCDB`, `0x47CD25`). A failed test jumps to `0x47D073`/`0x47D079`,
  which loads the off-map bucket `*(main+0x142B7)` and, when `+0x82` differs, unlinks the unit and
  stores it there (`0x47D0CF`). The unit frame predicts, when the footprint's cell or the layer
  moved, the bucket's ID (index + 1) for a footprint that passes this build's tests and the
  off-map ID otherwise, and the last value when neither moved; RT_PHYSICS's columns 0 makes the
  lane generic.
- **A ground unit's speed: the ground step `0x43CC20`** (thiscall on the move object (unit,
  step), `ret 8`; its callers the ground class step `0x43CD20`, which passes minus the def's brake
  rate `+0x19A` when the course has no target (`0x43CD40..0x43CD52`), and its end `0x43D0C0`, which
  passes the acceleration `+0x19E` when the unit is far enough from the target and slower than the
  course wants, else minus the brake rate (`0x43D0A2..0x43D0BA`)). It adds the step to the mover's
  speed `+0x20` and floors it at 0 (`0x43CC2D..0x43CC3B`); then sl = `movsx(unit +0x68) >> 11`
  clamped to −5..5 (`0x43CC46..0x43CC5F`), the slope byte `[0x505205 + sl]`, and the cap
  `((slope << 16) · maxv >> 16) << 16 / (100 << 16)` -- an `imul` with def `+0x192`
  (`0x43CC76`), `_allshr 0x4E43D0`, `cdq` (the low dword), `_allshl 0x4E44F0`, `_alldiv 0x4E4440`
  (`0x43CC7C..0x43CC95`): `s32(slope · maxv) / 100`, truncated. Below the sea (`movsx(+0x70)` <
  `main+0x1427F`) a def without `+0x241 & 0x81000` halves the cap (`0x43CCA0..0x43CCCE`). The speed
  is capped by a signed compare (`0x43CCD3..0x43CCD8`), then the velocity follows it (*Ground
  velocity*, above). The unit frame offers the accelerating and the braking step as two candidates
  for a type with an RT_TYPES row that is not an aircraft, from the last iteration's `+0x68` (the
  step's call `0x43DD4D` comes before the placement's `0x43DD55`; MEASURED 1.75 KB smaller on the heavy
  hour than this iteration's); it does not model the halving in water.
- **The turn rate's blip** (MEASURED, the size research's gap-units round, 2026-10-11; no engine
  writer traced): a turn rate one away from the iteration before's returns to it on the next, so
  the unit frame predicts the rate before last when the last two differ by one, else the last.
- **The ledger's requested and granted** `+0xC0`/`+0xC4` (energy) and `+0xD8`/`+0xDC` (metal), with
  the owner's player ID at `+0xEC` ([*The ledger*](weapons.md#the-nano-spray-two-emitters-fifteen-order-handlers-disassembled-2026-09-29)
  `unit+0xBC`, DISASSEMBLED 2026-10-10). The
  requests `0x4011C0`/`0x401180` add the same float to the requested and, while the unit's own
  debts `+0xC8`/`+0xE0` are not above 0 (either refuses both in `0x4011C0`; `0x401180` tests
  `+0xC8` alone), to the granted; the settle `0x401B3D` zeroes both, for units with `+0x110` bit
  28, on the owner's 30-tick pass. The unit
  frame predicts the requested as 0 at the owner's settle, the last value plus the slot's step
  while it grows, the last value when it held; the granted as this tick's requested when the two
  were equal, else the last value. It learns the settle per owner from the last requested that
  fell to zero and the step from the first value after a zero, and adds singles in integers,
  rounding to nearest even (`reccodec_f32_add`), the sum the FPU gives under any precision
  control. MEASURED on the heavy hour (*L5a* of the size page): the unit frame packs from 28.16 to
  13.73 MB. A weapon's shot paid at once (`0x4012A0`) and a cloak's cost (the economy pass) add to
  the requested only: residuals.
- **The water state `+0x10A`** is stored as one dword (`mov [esi+0x10A], edi` at `0x43DBED`); the
  lane table keeps it as two u16 lanes, as the size research measured them. A lane's width is a
  size hint only.
- **The sine table `0x509F00`**: MEASURED equal to `round(8192 · sin(2πi / 512))` on all 512
  entries, the closest to a rounding tie 0.0016, so the codec carries it as a literal of the
  formula and never reads the executable.

**Not predicted**, each costing size only: the circle's sight path (LosType bit 2 clear); a
landing (`0x43D210` with layer 1: the rule is written, never met); air transports' cargo; a
non-zero pitch scale (none of the recordings had one); the steering term's hidden target; the
ground speed's halving in water; a sort bucket that `fix_last_cell`'s stub clamps into the grid
(a linear index outside [0, rows · columns): RT_PHYSICS carries no row count); a type past the header's RT_TYPES rows (the header holds
`RECC_TYPES_MAX` 2,048 at most, and as many as its room allows).

### A create refused during the load, and the dirty create's position — the load state `0x497F40`, `0x48BA00` [DISASSEMBLED + MEASURED 2026-09-25]

**The load state `0x497F40`.** A state function, called once a frame from the main loop's
`call [main+0x391F5]` at `0x499A1C`, and installed right after state 5 is stored (`0x496C2E` →
`0x496C3D`, `0x496D65` → `0x496D75`, `0x496D87` → `0x496D9D`, `0x496DEF` → `0x496DFF`; and `0x490BB3`, a case
of the table after the generic state store `0x490B3D`). Its flags are the word `main+0x38D75`:

| bit | set | cleared | meaning |
|---|---|---|---|
| 0 (`1`) | `0x49832D`, the first call | `0x49847E` (with `+0x38D6F..+0x38D76`) | the load has started |
| 1 (`2`) | `0x497C62`, the loader's **last store** before it returns | `0x49847E` | the loader is done |
| 2 (`4`) | `0x4975CA`, the loader after the level init, in a network game only (`0x435100` = 3 at `0x4975AB..0x4975B3`; any other type goes to `0x4978DA`) | `0x49855D` (and `0x49686E` in the frame function), the game thread | this peer has loaded |
| 3 (`8`) | `0x498579`, the game thread, when `0x4568C0` returns nonzero | `0x49847E` | [INFERRED] every player has loaded; the loader waits for it at `0x4975D6..0x4975F1`, 50 ms at a time, on the same network-only path as bit 2 |

**Who passes the barrier last** [MEASURED 2026-09-26, three peers, Town & Country]. A peer frozen
(SIGSTOP) at the barrier with bit 2 set holds every other peer at bit 2 too (`0x0005` on both
others for 90 s), so bit 3 needs a message from each peer after the waiting peer's own load, not
only the status sent before it. The peer that finishes loading **first** passes the barrier
**last**: a peer slowed through its whole load holds everyone's status when it is done and makes
the others' commanders in catch-up ticks, while a peer that loaded at full speed and was then
slowed to 3 % at the barrier held both others' creates and replayed them at `0x49842F` (one start
of two; `tadr-port/sim-fixes.md`, B5).

**The first call** (bit 0 clear, tested at `0x497F54`) runs `0x497F5E..0x498334` once per level: the
timing base `main+0x38A37` (`0x498164`), GameTime 0 (`0x498180`), and the loader thread
(`push 0x497C70; call 0x4B6B20` at `0x4982C5..0x4982CA`; `0x497C70` is an exception frame around
the body `0x497180`). **Every call** then tests bit 1
(`0x498342..0x49834D`): clear, it is the loading frame `0x4984DD` — per-player `0x453320`, the pump
`0x453D40` at `0x49852E`, the barrier above; set, it is **the in-play entry** — `0x467D70`, the
frame function `0x496790` at `0x49842F`, `0x4C2870`, state 6 at `0x498445`, the next state function
`0x499200` at `0x498455`, and the flag word zeroed at `0x49847E`.

**Up to five ticks run before state 6.** The frame function `0x496790` first moves its profile
counters (`main+0x38D85..`) and reads the clock (`0x4B6560`); in a network game (`main+0x2A44`
bit 0) it asks `0x495230` how many ticks are due since the timing base (`main+0x38A3B`, capped at
5 at `0x4953F5`; `0x495230` itself sends and receives nothing), and `0x495490(1)` (`0x49680B`) runs
that many, each `GameTime++` (`0x4954C0`) and, its argument being nonzero, a pump (`0x4954C8`),
then the units `0x48AD30` and the rest. After the ticks the frame function pumps once more itself
(`0x453D40` at `0x4968CB`), still in state 5 at the in-play entry. At `0x49842F` the base is the
load's start, so the catch-up is the cap: five ticks in state 5, before `0x498445`. MEASURED: GameTime read 5 just after the
state-6 store in every two-peer start of B5's first build that held a create (four starts).
GameTime's five writers stand; it is not 0 when play starts, and nothing ticks before
`0x49842F`'s call.

**The loader `0x497180`** (loader thread) pumps at `0x49727D` in its early wait, runs the level
init `0x4917D0` at `0x497581` (the unit array, `0x4854A0` at `0x4918D4`), sets bit 2, waits for bit
3, creates the local player's commander (`0x485F50` at `0x4977BB`, which sends its `0x09` while this
peer is still in state 5), and sets bit 1 last. So during a load two threads pump: the game thread
(`0x49852E`, and at the in-play entry the catch-up ticks' `0x4954C8` and the frame function's
`0x4968CB`) and the loader (`0x49727D`). The loader's last pump
precedes its last store, and the game thread reaches `0x498445` only after reading that store.

**Why a peer loses the others' commanders.** Every peer creates its commander after the barrier,
still in state 5, and the dispatcher passes `0x09` only in state 6 (`0x45473F`, `0x512BC0`): a peer
whose remaining load is longer refuses the others' creates at `0x455F50`. In every two-peer start
measured, the joiner. Since B4 the create rides in a `0x4A` companion, which state 5 passes (`0x05`
has mask 7) and B4's receiver refuses by the same table (`gate=`). **Every unit message is
refused alike:** `0x451FD0` gives mask 4 (state 6 only) to `0x09`, `0x0A`..`0x10` and `0x2C`
among others (`0x451FE0..0x452035`), so in state 5 a peer drops the others' kills (`0x0C`), hits
(`0x0B`) and full state (`0x2C`) too. A message failing its mask in state 5 leaves by the
dispatcher's refusal branch `0x45477F..0x454787` (`cmp edx,5; je 0x455F50`), with the message at
`[esp+0x10]` and the sender as below.

**A create before the first tick is what stock does.** `CreateFromNetwork 0x4861D0` calls
`0x485A40`, `0x485D40`, `0x49E070`, `0x437840`, `0x4B4F10`, `0x43DC00`, `0x48A870`, `0x47CC30`,
`0x482AC0` and `0x490580`, every one of which the local create `0x485F50` calls too, and the loader
runs `0x485F50` for this peer's commander in state 5 before any tick (`0x4977BB`); neither body reads
the net state `main+0x391F1` or GameTime. Over an occupied slot `CreateFromNetwork` first destroys
the occupant (`0x486237..0x486244`, `0x4864B0(unit, 0)`), which sends a `0x0C` only for a local
player's unit (below).

**What removes a copy whose unit is gone: the round robin's type 0.** The owner's `0x2C` sender
`0x48B710` runs from the unit tick (`0x48B003`) for each local player in a network game
(`main+0x2A44` bit 0). After the dirty entries and their `0xFFFF` end it always appends one
round-robin entry (the flag bit set unconditionally, `0x48B81B..0x48B84C`) for the slot
`GameTime % N` of its block (`N` = `main+0x37EE6`, `0x48B827..0x48B8A4`), written by `0x48B200`:
the type word first (`0x48B222`), and for an empty slot nothing more (`0x48B227..0x48B22F`). The
receiver `0x48B920` reads that flag (`0x48BA6A..0x48BA97`), takes the slot from the header's
GameTime (`0x48B95A`) `% N` (`0x48BA9F..0x48BABA`) and hands it to `0x48B3F0`: a type 0 over an
occupied slot sets `+0x110` bit 14, pending death (`0x48B415..0x48B42F`: an empty slot, `+0xA6` 0,
leaves at `0x48B420`; the set itself is `0x48B426..0x48B42F`, `or ch,0x40`). The receiver's unit tick
then calls `0x4864B0(unit, +0xF5)` for every unit with that bit (`0x48AFB9..0x48AFD1`), which
destroys it (`0x4866D0` at `0x486679`) and sends a `0x0C` only when the unit's player is local
(`0x48664B..0x48666D`, the image's one send of the `0x0C`'s 11 bytes). So a copy whose owner's
slot is empty is gone within N of the owner's ticks: 1500 at the raised limit (50 s at 30 ticks a
second), 500 at stock's (16.7 s).

**A sender, as the dispatcher sees it.** The pump's frame holds the sender's **record index** in
the low byte of `[esp+0x14]` and its record `main+0x1B63 + k·0x14B` in `edi`, paired by
`0x453E84..0x453EA1` (the byte masked, then `edi = edx + k + 2·165k + 0x1B63` = `main + 0x1B63 +
k·0x14B`, the `lea edi` ending at `0x453EA1`): the loop `0x453DBD..0x453E12` finds the record whose `+4` is the message's
DirectPlay id and stores its index at `0x455F78`. The sender test
(`0x4547AD..0x4547E2`): `[rec] ≠ 0`, `+0x73` 3 (1 and 2, local, are dropped), `+0x146 ≠ 10`; the
destination record in `ebx` passes the same (`0x454821..0x454844`). A record's index is not its
block's: blocks are handed out in DirectPlay-id order (`0x485842..0x485853` compares `[rec+4]`
when `0x435100` returns 3; `0x4858BD` stores the block), records in local order.

**The dirty create's record** (`0x48B9B6..0x48B9FB`, into the 0x2C frame's `esp+0x1C`): `[0]` 9,
`[1]` the entry's type, `[3]` the slot's `+0xA8`, `[5]`/`[9]`/`[13]` the **slot's own** `+0x6A`,
`+0x6E`, `+0x72` (zero in a fresh array, the last unit's in a freed slot), `[17]` `+0x64`, `[21]`
`+0x68`; the player is the slot's `+0xFF`. `CreateFromNetwork` hands that position to `0x485A40` (`0x4862B8`) and
gives the unit a move object (`0x43DC00`, `0x4862E9`) — only when the def's `+0x22F` is 1
(`0x4862CF`) — before the rest of its set-up; only then does
`0x48BA10` parse the entry's move payload with the new object's `[vt+0x24]`.

**The move object `0x43DC00`** (`unit+0`, `0x2F` bytes; the class object is its `+0`). A unit
whose player is remote (`+0x96` → `+0x73` 3, `0x43DC48..0x43DC57`) gets a proxy: the **air proxy**
`0x490940` (`0x27` bytes, vtable `0x4FD9E0`) when `def+0x241` bit 11 is set (`0x43DC5F..0x43DC68`),
else the **ground proxy** `0x44F570` (`0x1C` bytes, vtable `0x4FD488`). The owner's classes are the
local ones. These are course-object sizes, not the outer mover's allocation:
create sites `0x4860C6` and `0x4862DA` allocate the outer `0x2F` bytes. Rechecked
by disassembly for native file capture, `0x43DC00` initializes that outer record,
chooses air proxy allocation `0x27` at `0x43DC6D` / constructor `0x490940` at
`0x43DC82`, or ground proxy allocation `0x1C` at `0x43DC90` / constructor
`0x44F570` at `0x43DCA1`, stores the course pointer at outer `+0` and returns
the outer record. Local air uses allocation `0x28` at `0x43DCC2` / constructor
`0x4907E0` at `0x43DCD3`; local ground uses `0x65` at `0x43DCE1` / `0x44F010`
at `0x43DCF2`. The recorder copies the bounded outer 47 bytes and excludes
its course pointer under the existing layout; it never follows any of those
four class allocations. `0x43DD10` destroys the course, called by the unit
teardown at `0x486DB1`; that teardown frees/nulls the outer mover before returning.
This capture runs on the producer thread before unit ticks and ends before level
reclamation. Inspect `0x43DC00..0x43DD18` in the pristine executable. `[vt+0x1C]` says whether the unit is dirty, `[vt+0x20]` writes its payload and
`[vt+0x24]` reads it (the dirty list, `0x48B77A..0x48B7FD`, writes delta, type and `[vt+0x20]`):

| vtable | class | dirty `+0x1C` | writes `+0x20` | reads `+0x24` |
|---|---|---|---|---|
| `0x4FD428` | base | `0x44EFE0` (never) | `0x44EFC0` (nothing) | `0x44EFD0` (nothing) |
| `0x4FD458` | local ground mover (`0x44F040`) | `0x44F480` | `0x44F4A0` | `0x44EFD0` |
| `0x4FD488` | remote ground proxy (`0x44F570`) | never | nothing | `0x44F5C0` |
| `0x4FD980` | air base (`0x490940` first) | never | nothing | `0x44EFD0` |
| `0x4FD9B0` | local air mover | `0x4908B0` | `0x4908C0` | `0x44EFD0` |
| `0x4FD9E0` | remote air proxy | never | nothing | `0x490A10` |

The step `[vt+0x08]` is what the movement pass `0x43DD20` (thiscall on the move object, the unit
as its argument, `ret 4`) calls first, at `0x43DD28`, before `0x43D290` (def `+0x241` bit 11) or
`0x43CD20`, then `0x43D6D0`, `0x43DA70` (the `StartMoving`/`StopMoving` and `MoveRate*` starts,
`0x43DAF2`, `0x43DB27`) and `0x43DB50`. The steps are `0x44EFB0` for the base and the remote ground
proxy — a bare `ret` — `0x44F1A0` for the local ground mover, `0x490690` for the air base and the
remote air proxy, and `0x490880` for the local air mover (the vtables' `+8` dwords).
`0x43DD20`'s only callers are the unit tick's `0x48AFAA`, for a local owner, and the `0x2C`
receiver's block sweep `0x48BA46`, so a remote unit steps only when its owner's `0x2C` arrives
[DISASSEMBLED 2026-09-28]. What each part does, and why an unfed proxy brakes: *Driving and
rebuilding puppets*, above. The local ground mover is dirty (`0x44F480`) while its `+0x64` bit 3 is
set or its `+0x64` bit 2 differs from the move object's `+0x2E` bit 2; the path writers set bit 3
(`0x44F0B3`, `0x44F0F1`, `0x44F142`, `0x44F235`, `0x44F441`), and the payload writer `0x44F4A0`
clears it and copies bit 2 once it has written (`0x44F54A..0x44F55C`).

**The payloads, and where a position lands.** The bit reader is `0x415DC0` (`{dwords, word, bit}`,
least significant bit first; a read that ends on a word boundary still loads the next dword, masked
to nothing), the writer `0x415C10`.

- **Ground** (`0x44F4A0` → `0x44F5C0`): one flag bit (bit 2 of the move object's `+0x2E`), a 2-bit count n —
  the local mover's path count `+0x5C`, capped at 3, 0 when `+0x64` bit 0 is clear — then n points
  of `int16 x, int16 z`, whole world px, from the path's front `+0xC`. At most 99 bits. **No field is
  the unit's own position; point 0 is the node the unit last reached.** A straight order's path
  begins at the unit: `0x44F3F2..0x44F417` stores count 2, point 0 = the unit's `+0x6C`/`+0x74`
  (the integer halves of `+0x6A`/`+0x72`), point 1 the goal. The front is dropped only by the
  mover's step `0x44F1A0` (vtable `0x4FD458` `+8`, called through `0x43DD20` — `call [vt+8]` at
  `0x43DD28` — from the unit tick at `0x48AFAA`; the drop is `0x44F1D7..0x44F235`, after calls
  through the object at `+4`, `0x44F1A5..0x44F1D4`): with a count of 2 or more (`0x44F1D7`) it
  takes the squared distance from the unit's `+0x6C`/`+0x74` to **point 1** (`0x44F1DF..0x44F203`)
  and, only when it
  is at most `0x19` (5 px, `0x44F205`), shifts the path down one point and decrements the count
  (`0x44F20A..0x44F229`; below 2 it clears `+0x64` bit 0, `0x44F231`; `+0x64` bit 3 set,
  `0x44F235`). So a straight move keeps its origin as point 0 until the unit is within 5 px of its
  goal. `0x44F100` does the same shift by a count argument, and nothing calls it — no direct call
  or jump and no stored pointer in the image. The proxy hands the points out as `x<<16, 0, z<<16`
  (`0x44F650`, its `[vt+0x0C]`) [INFERRED: as the unit's path].
- **Air** (`0x4908C0` → `0x490A10`): a 2-bit selector, the sub-object's type (`[vt+0x08]`) 2 → 1,
  3 → 2, none → 0; then that sub-object's own payload (`[vt+0x28]`), then 2 bits of the move object's
  `+0x2E`. The proxy's step `0x490690` (its `[vt+0x08]`, above) asks the sub-object for its point (`[vt+0x20]`, at
  `0x4906B8`) into its own `+0xC` — the point the unit is steered to (`0x490650`, its `[vt+0x10]`,
  hands out `+0xC` and the velocity `+0x18`).
  - selector 2, type 3 (`0x2C` bytes, vtable `0x4FD3F8`; `0x44E930` → `0x44E9C0`): one flag bit, x,
    y, z (16.16) at `+0xA`/`+0xE`/`+0x12`, a velocity at `+0x16`/`+0x1A`/`+0x1E`, and with the flag
    a `u16` at `+0x24` — 209 bits at most. Its point `0x44EA60` is the position, then advanced by
    the velocity: the unit's dead-reckoned position.
  - selector 1, type 2 (`0x36` bytes, vtable `0x4FD3B8`; `0x44DDC0` → `0x44E080`): 8 flag bits;
    bit 0 a `u16` and a unit index (the followed unit, `0x489690`), bits 4, 3, 6 a `u16` each, bit 5
    a vector at `+0x26` — 184 bits at most. Its point `0x44E3C0` is `+0x26`, refreshed from the
    followed unit (`0x43E060`), its y clamped to `0x1FF` px (`0x44E4F1`): a **goal**, not the
    unit's own position.

**What B5 patches** (the plan's *B5 DESIGN*; the code is `fix_ghost_commander`): B4's `0x05`
receiver holds a carried `0x09` its gate refuses in state 5, with the sender's record index and
DirectPlay id; the refusal branch `0x45477F` notes a refused `0x0C`, which cancels the latest
create held for its slot from its sender, or marks dying, as `0x48B42C` does, a copy made before
state 6; `0x497F5E` empties the queue at the load's start; `0x49842F` replays it before the frame
function runs the first tick, through B4's receiver past its state test and `CreateFromNetwork`,
and from there on B4's receiver makes a create the gate refuses in a catch-up tick at once, in
that tick's pump (or the frame function's `0x4968CB`);
`0x48BA00` calls a stub that rewrites the dirty create's position from the entry's own payload
(ground point 0; air selector 2's x, y, z) when it lies on the map, then enters `CreateFromNetwork`
under `0x48BA05` — reading the payload only from B3's copy of the `0x2C`, under B3's reader, within
the message's length; B5 refuses the fail-closed table unless B3 armed. Compared, not written: `0x497F54`, `0x497F64`,
`0x497C5F`, `0x498348`, `0x48B9F5`, `0x4861D0`, `0x496790`, `0x454788`, `0x455F50`, `0x48B933`
(12 bytes, the reader's slots) and `0x453E84` (30 bytes, the sender's record). All are rows of the
fail-closed table, in both builds.
MEASURED 2026-09-25 (the plan's *B5 LANDED* section, two peers, Two Continents): on the previous
build the joiner lacked the host's commander until t = 50 s at the 1500-unit limit and t = 18 s at
500, and one ordered to move at once appeared at `(1,−2)` / `(2,−3)` and walked from the corner;
on B5's first build, which replayed at the state-6 store, it was present at the host's position at
the first sample in all four starts, from an exact copy (record 1 on the joiner is the host, whose
block holds slot 1), and with the queue turned off the dirty create alone put it at `(368, 7664)`
where stock's record had `(0, 0)`. **The host's
create reaches the joiner in its catch-up ticks, not its load**: with a drain before the first
tick and one after the state-6 store, the first found nothing (GameTime 0) and the second made the
commander at GameTime 5; on the final build the drain again finds nothing and the create is made
in the pump of the joiner's first tick (`created slot 1 from sender 1 … at GameTime 1, in a
catch-up tick`), present at the host's position at the first sample, `(368,7663)` against
`(369,7660)`. The final build has run two such starts (1500, the commander moving) and one with
the joiner's queue off, where the dirty create alone put the commander at `(368, 7664)`, point 0,
from B3's copy (`unbound=0 short=0`); the replay at `0x49842F` has never replayed a record live,
and the 500-unit and idle starts were not re-run on it.

### A builder's build list, `TEMP UTYPE LIST` — `0x42DA58`, `0x42DAC7`, `0x42BEAF..0x42BED3` [DISASSEMBLED + MEASURED 2026-09-24]

**The list.** A builder's `canbuild%d` keys in `gamedata\sidedata.tdf`'s `[CANBUILD]` section are read
at game load into one shared heap block named `TEMP UTYPE LIST` (`0x42D971`, 0x3C bytes, 30 `u16`
type IDs). The append at `0x42DA58` (`mov [ebp],ax; inc ebx; add ebp,2`) runs until a key is missing
(`0x42DA46..0x42DA99`); the count goes back to 0 for each builder (`0x42D91D`, `0x42DAA5`). The
builder then gets its own copy at def `+0x156`: a fresh 0x3C bytes (`push 0x3C; push eax; call
0x4D83B0` at `0x42DAC7`, named `CANBUILD %s`) filled by a `rep movs` of 15 dwords, with the real
count at `+0x152`. The download appender `0x42BE30` (from `0x42E0DE`) adds the builder's download
entries while the count is at most 30 (`0x42BEAF`: `cmp dword [ebp],0x1E; jg`), so it writes entry 30,
two bytes past the block (`0x42BEC3..0x42BED3`). The AI's pick `0x40BDB0`, its debug listing
`0x46887B..0x468982` and the uncalled `0x4894F0` loop to the count; the other readers test the
pointer only ([limits-evidence §8](../tadr-port/limits-evidence.md) lists them). The def clone `0x42B370`
runs before `0x42D9BB`/`0x42D9C7` zero the pair, and the teardown frees each copy (`0x42DC52`).

- **Every builder gets a copy.** The pair is zeroed for every def, and a def with the builder bit
  `+0x241 & 0x40` goes on to the copy (`0x42D9B5..0x42D9CD`). A builder with no `[CANBUILD]`
  section, or no subsection of its own name in it, jumps past the reads straight to the copy
  (`0x42D9EC`, `0x42DA01` → `0x42DAA7`) and gets a non-NULL block of count 0.
- **The appender ignores MENU.** `0x42BE30` appends every download entry of the builder, whatever
  its `MENU` (`0x42BE9E..0x42BED1`), so an entry with no `MENU`, which no build page shows, is still
  on the list.
- **No order is made from it.** The build menu's placement dispatch `0x419670` gives every selected
  builder (`+0x241` bit `0x40`) a `MOBILEBUILD` or `VTOL_MOBILEBUILD` of `main+0x2CC4`
  (`0x41975E..0x4197A9`) and reads no list; nor do the order roots or MobileBuild `0x403A20`.
  Its walkers are the AI's (`0x40BDB0`, `0x408149`, `0x408781`), the debug listing and the
  uncalled `0x4894F0`. So the list is what the AI builds from, not what a player's menu offers:
  Impure's Quick Build reads each constructor's build menu instead (*The left sidebar*, below,
  and *Resource placement and contextual order contracts*).

**The fix**, `fix_build_list`: a block holding `n` entries has room for `bl_room(n)` — 30, then
powers of two from 64 — and each writer grows its block through `0x4D83B0` before the entry that
would not fit, the old block freed through `0x4D85A0`. `0x42DA58` and `0x42BEC3` call the two appends,
`0x42DAC7` the copy (then `jmp 0x42DAE3`, past the `rep movs`), and the cap at `0x42BEAF` is NOPped.
A builder with 30 entries or fewer, every stock builder, gets stock's exact blocks.

**MEASURED**, 40 `canbuild` keys added to ARMCOM: the previous build read a count of 59 over a copy
of 30 entries followed by heap bytes, with no fault that load; this build reads 71 (19 stock, the
40, and 12 download entries), every ID in order, in a block of 128.

### A yardmap parsed past its string — `0x42CF5E` [DISASSEMBLED + MEASURED 2026-09-25]

**Where it is.** The unit-def parser `0x42BF40` (`sub esp,0x518`, `ret 8`) is called from
`0x42D269` and `0x42D722`. `0x42D722` is inside `0x42D2E0`, which the level load `0x4917D0` calls
at `0x4918CA`, on the loader thread; `0x42D269` is inside `0x42D1F0`, whose only call is at
`0x4174BE` in the typed-command handler (the game thread [INFERRED]). The yardmap is read only for a def whose BMcode `[def+0x22F]` is 0
(`0x42CF30..0x42CF38`):
- **The read.** The parser reads the FBI's `YardMap` key with
  `GetString 0x4C48C0(buf, "YardMap", 0x400, "")` into a 0x400-byte stack buffer at
  `[esp+0x128]` (`0x42CF3E..0x42CF59`). GetString has 120 callers. It NUL-terminates inside the
  size, or copies the `""` default when the key is missing, and the parser ignores its return.
- **The allocation.** The parser allocates `footX · footZ` bytes (`+0x14A`, `+0x14C`, words) at
  `def+0x14E` through `0x4D83B0` (`0x42CF5E..0x42CF7A`).
- **The fill.** It fills the cells row by row (`0x42CF9D..0x42D06B`). The byte map `0x42D198`
  sends each char into the jump table `0x42D16C`, and a char of `.CGOYcfowy` writes one cell:

  | char | `.` | `C` | `G` | `O` | `Y` | `c` | `f` | `o` | `w` | `y` |
  |---|---|---|---|---|---|---|---|---|---|---|
  | cell | `00` | `35` | `8F` | `2B` | `31` | `2D` | `6F` | `2F` | `37` | `29` |

  After writing a cell, the parser moves on to the next char unless that char is the NUL, so a
  short string repeats its last char. Any other char, the NUL included, is skipped without
  filling a cell (`0x42CFB5` → `0x42D049`).
- **The rejoin.** The parse ends at `0x42D079`, where the footprint's box is loaded. Nothing after
  it reads the parse's registers or `[esp+0x1C]` before writing them: `ebx` is written at
  `0x42D0E3` and `edi` at `0x42D0F1`.

**The defect.** Two strings walk past the NUL: a missing key, and a string that ends on an
invalid char. The walk goes on up the stack until every cell has a byte. The yardmap decides
placement (`0x47D2E0`), occupancy (`0x47CC30`) and pathing, and each peer's stack is its own.

No retail unit reaches it. MEASURED: 279 defs, 126 of them with a yardmap (81 cells at most,
2440 in all), and every one ends on a valid char. A mod reaches it. MEASURED with two
scratch-built copies of a 5 × 5 retail structure, on the previous build:
- `B6YARD0` has no key, and its cells read all `0x2F`: the stack held `o` bytes on both
  launches;
- `B6YARD1` has the key `oo?`. Its cells read `2f2f31313100002b2b…`: the `Y`, `.` and `O` bytes
  come from past the string. The run was identical from launch to launch: what one build
  leaves on its stack on the reference setup, not a rule.

**The fix: the parse stays inside its string.** A `jmp` at `0x42CF5E`, a row of the fail-closed
table, goes through `fix_call_regs` into `yard_parse`, and the stub rejoins at `0x42D079`.
`yard_parse`:
- makes stock's allocation through the same allocator;
- fills the cells by stock's table, stock's skip rule and stock's repeat rule;
- reads nothing past the string's NUL or past the buffer's 0x400 bytes;
- gives each cell stock would have filled from past the NUL the last valid char's byte, or `o`
  (`0x2F`) when the string had none [DECIDED 2026-09-25].

Two more rows check the bytes around the site: `0x42CF3E`, the 32 bytes of the key's read into
its 0x400 bytes, and `0x42D079`, 7 bytes.

TADR's fix leaves `def+0x14E` NULL, and six readers dereference it unchecked. MEASURED on the new
build:
- the 2440 retail cells are identical to the previous build's, 0 of 2440 differing;
- `B6YARD0` and `B6YARD1` read all `0x2F` on both launches, with the log lines `25 of its 25
  cells` and `23 of its 25 cells ... take 0x2F`.

## A unit's death, its transport and its explosion — mapped for TADR section C, landing C4 [DISASSEMBLED + MEASURED 2026-09-26]

Every address transported explosions (C4) touch or read
([data keys, C4 as built](../tadr-port/data-keys.html#c4-as-built); the hooks are in
[gpu-status §2.101](../gpu-status.html)). A rel8/rel32 scan of `.text` and every dword of the image
finds no branch or pointer landing inside any replaced range past its first byte except the pick's
own `je` (`0x49B017` → `0x49B021`): `0x49B017`–`26`, `0x48664B`–`50` (`0x48663D` jumps to its first
byte), `0x486679`–`7D`, `0x486810`–`15`, `0x486F10`–`19` and `0x42B019`–`1E`.

### The death explosion — `0x49B000(unit, selfd)`, stdcall, `ret 8`

- **Frame and pick.** `0x49B000..0x49B016`: `eax = selfd`, `ecx = unit`, `sub esp,0x6C`,
  `edx = 0`, `cmp eax,edx`, `eax = [ecx+0x92]` (the def), `push esi; push edi` — so at `0x49B017`
  `selfd` is `[esp+0x7C]` and the flags are still the compare's. `0x49B017..0x49B026` (16 bytes):
  `je` → `def+0x220` (ExplodeAs), else `def+0x224` (SelfDestructAs); `0x49B027` `cmp eax,edx`, a NULL
  weapon returns at once.
- **The projectile.** A stack record at the unit's position (`+0x6A`), its player the dying unit's
  (`unit+0xFF`, `0x49B034`, stored at `proj+0x66`, `0x49B055`), no shooter (`proj+0x52` = 0,
  `0x49B03E`), handed to `0x499EB0` (`0x49B074`). Its area damage runs
  only on the dying unit's owner's peer (the owner gate `0x49A03F..0x49A047`); elsewhere it is a
  picture.
- **Its two callers.** The destructor at `0x486D50`, when the record's severity `rec+9 > 0`
  (`0x486D28`) and `+0x104` is 0.0 (`0x486D2F..0x486D40`), with `selfd` = (the record's kind
  nibble `== 3`) (`0x486D42..0x486D4B`); and kill-all at `0x486F9E` with `selfd` = 1.

### `Send_UnitDeath 0x4864B0(unit, kind)`, stdcall, `ret 8`

- `0x4864BB`: a unit without the alive bit (`+0x110` bit 28) returns. `0x4864CB..0x48650F`:
  `edi` = 1 when the unit's type name (`def+0x20`) equals (`0x4F8A70`) the commander name of its
  player's side (`main+0x37F5F + 0x232·side`); such a death clears bit 0 of the player's word
  `+0x149`.
- `0x486521..0x4865CE`: the severity (`[esp+0x24]`, the unit argument's slot reused) and the corpse
  type (`[esp+0x10]`). Kind 7: severity 0, corpse 1. Kinds 4, 5 and 9, and a unit whose HP word
  (`+0x108`) is above 0: both 0. Otherwise the severity is `(100·(−HP) / max HP (def+0x1FA) +
  unit+0xF7) / 2`, held to 1..100, and `0x4865C3` runs the unit's COB script `Killed` (the name at
  `0x508BE8`, through `0x4B0BC0`) with pointers to both, and the script answers the corpse type.
  `0x4865D2..0x4865E5`: a unit whose `+0x104` is not 0.0 leaves no corpse.
- **The record**, 11 bytes at `[esp+0x14]`: `+0` = `0x0C`; `+1` = the unit's index word, copied
  from `+0xA8` (`0x4865ED`, `0x4865FC`); `+3` = the DirectPlay ID `0x44FFD0` gives for the player byte `unit+0xF4` (`0x486625`); `+7` = the
  killer's index, `+0xF0`'s `+0xA8` or 0 (`0x48662E..0x486646`); `+9` = the severity; `+0xA` =
  `kind << 4 | corpse` (`0x48661C..0x486621`). It is complete at `0x48664B`.
- `0x48664B..0x48666D`: the send, `0x451DF0([player+4], rec, 0xB)`, only for a unit whose player
  record has a nonzero first dword and type 1 or 2 (`+0x73`) — a LOCAL player;
  `0x486672..0x486679`: the destructor on the same record, `0x4866D0(rec, 1)`.
- `0x48667E..0x4866B3`: a commander's death (`edi`) with `ActiveCommanderDeath` (`main+0x37EF6`)
  set, for a local player, calls `0x491D70(1)` (deselect everything) and kill-all for `unit+0xFF`.

**The destructor** `0x4866D0(rec, mode)`, which this calls and every other peer's `0x0C` case calls — its slot and B3's bound, the
cargo loop that deals each passenger 30 000 (the only source of kind 6), the explosion's call at
`0x486D50`, and what the free clears and leaves (*Unit identity on the wire*) — is
[its entry](memory.md#0x4866d0-the-unit-destructor-inferred-name-units_destroy).

### The draw's unit list — HotUnits, its cull `0x48BAE0` and its readers [DISASSEMBLED 2026-10-09]

- **The list**: `u16` unit slots at `*(main+0x1435F)`, the count at `main+0x14367`. The cull
  `0x48BAE0` (cdecl, no argument, plain `ret`) rebuilds it from the unit array, the rect
  `+0x37E27`, the eye and the viewed record `main+0x2A43`, and lists only slots whose type
  `+0xA6` is nonzero (`0x48BB3E`). Its callers are `0x48D5A5`, `0x495C6D`, `0x495E5D` and
  `0x49697B`; the last is on every path of the frame callback `0x496790` to the draw call
  `0x4969CD`, and nothing jumps into `0x496980..0x4969CD` from outside it, so the engine's draw
  always reads a list culled after the frame's steps.
- **The bar loop** `0x469C09..0x469D1F`, inside `DrawGameScreen 0x468CF0`, walks that list and
  never tests a slot's type. It draws a bar only when option-word bit 0 is set (damagebars,
  `0x469C97`) and the slot's owner record `+0x146` equals the viewed record (`0x46967D..0x469689`,
  `0x469C9C..0x469CAC`), calling `DrawHealthBars 0x46A430` (stdcall `ctx, unit, x, y`, `ret 0x10`),
  which tests HP `+0x108 > 0` and then divides by the def's maxHP `+0x1FA` unsigned at `0x46A4BB`
  with no guard. A slot the destructor emptied after the cull keeps its HP and its owner and has
  maxHP 0, so the divide faults: MEASURED 2026-10-09, b7-word-outright's ARMMSTOR slot 2 listed
  at GameTime 5 and emptied by a seek to 301 at the frame visit, `Integer Divide by Zero` at
  `0x46A4BB` on the draw.
- **The list's other readers**: the row filing `0x4697B7`, the Under Attack test `0x48BCB0`, the
  pickers `0x48C030`, `0x48C390`, `0x48CD80` and `0x48D4D0`. None of them faults on an emptied
  slot: the unit pass tests `+0x9A` and selection bits 4 and 5, which the destructor clears
  (`0x469B6A..0x469BAC`).
- **Impure's call, the fifth caller**: the replay's frame visit (`tagpu_replay.c`, THE DRAW'S
  UNIT LIST) runs between `0x49697B` and `0x4969CD`, and is the one place Impure destroys or makes
  units outside a step (a seek, a takeover, a rewind), changes the viewed record (Sight), or moves
  the eye (Follow where no zoom apply runs). A visit that did any of those ends with `0x48BAE0`
  again, so the draw reads a list built after them.

### Kill-all for a player — `0x486F10(player)`, stdcall, `ret 4`

- The player record from the byte argument (`main+0x1B63 + p·0x14B`), nothing when its unit
  count `+0x144` is 0; its block `[+0x67, +0x6B]`, stride `0x118` (`0x486F48..0x486F5A`).
- For each unit alive and not pending death: a player of type 1 or 2 with a DirectPlay id (a
  LOCAL player, `0x486F73..0x486F87`) hits its own unit with 30 000 of kind 3, the unit as its own
  attacker (`0x486F94`), and the reaper destroys it later; any other player's unit is detonated at once, `0x49B000(unit, 1)`
  (`0x486F9E`), marked pending (`0x486FAE`) and destroyed, `Send_UnitDeath(unit, 3)` (`0x486FB4`),
  whose destructor runs the cargo loop with kind 3 — and, the passengers' player not being local,
  only detaches them, before their own turn when the transport is earlier in the block. A passenger
  earlier in the block is still carried at its turn and detaches itself (`0x4867CB`). `0x486FC4..0x486FC6`: `pop edi; pop esi; ret 4`.
- Callers: `0x4866B3` (a commander's death); `0x452E17` in the player removal `0x452CC0(dpid)`
  (nine callers, among them the network pump's leave case `0x4550D5`; *The session, not the host*),
  which kills the seat's army before it clears the seat's type (`0x452E62`), so every remaining peer
  takes the non-local branch for a departed player's units; and three console commands, each a row
  `{name, handler, 4}` of the debug-level table (DISASSEMBLED):
  - **`Kill`** (`0x501FE8`, `0x4164B0`): with a seat argument, kill-all for it (`0x4164DF`), then
    the objective's disarm `0x4904B0` (`0x4164EF`). **After `+kill N` the level never ends by a
    victory or a defeat**: both end tests answer 0 while `+0x88` is 0 (*The two end tests*);
  - **`IWin`** (`0x501FF4`, `0x4169D0`): kill-all for seat 0 when the controlled player's
    PlayerInfo `+0x95`, its side, is 1, else for seat 1 (`0x4169EA..0x416A00`), then
    `main+0x3923B` |= `0x10`, `0x20`, `0x04` (`0x416A0A..0x416A22`): won and stop, without the
    objective. MEASURED 2026-10-02 on a takeover as seat 0 (ARM) of a two-seat skirmish: seat 1's
    army died, the level ended at ENDMSN reading VICTORY, and the recording's end block read
    seat 0 won, seat 1 lost (flags `0x34`);
  - **`ILose`** (`0x502000`, `0x416A30`): kill-all for the seat whose number is the controlled
    player's side (`0x416A51..0x416A58`), then bit 4 cleared and bits 6 and 2 set (`0x416A62..0x416A7C`): lost and stop.

### What a death sends, and who removes a player

- **The detach broadcasts.** `0x48AAC0(child, parent, a3, a4)` (`ret 0x10`) returns doing nothing
  unless the child is alive, lacks `+0x110` bit 29 and carries nothing (`0x48AAC7..0x48AAED`), and a
  named parent is alive, not the child and not itself carried (`0x48AAF7..0x48AB0D`); it then builds
  the 7-byte `0x0A` (`+1` the child's `+0xA8`, `+3` the parent's or 0, `+5`/`+6` the low bytes of
  `a3`/`a4`, `0x48AB0F..0x48AB4E`), sends it through `0x451DF0` whichever peer runs it
  (`0x48AB58`), and applies it with `0x48AB70` (`0x48AB62`). So the destructor, which runs on every
  peer, broadcasts both of its detaches from every peer that runs it: its own unit's when that
  unit is carried (`0x4867CB`), and each passenger's in the cargo loop (`0x48681D`).
- **The hit's send.** `0x489BB0` applies a hit locally (`0x489C89`) and sends it only when the
  victim's player record has a nonzero first dword and type 3 (a remote player) and the kind is
  not 11 (`0x489C8E..0x489CA2`); with an attacker the sender is the attacker's player's
  DirectPlay id (`0x489CAF..0x489CB9`), else `0x44FDB0()` (`0x489CC7`). A peer that runs the
  cargo loop for another player's transport therefore sends that player a 30 000 hit on each
  passenger. `0x489CE0` clamps a remote copy's HP word at 0 (`0x489EF1`) where a local one goes
  pending.
- **The removal from TIMEOUT.GUI.** A peer that stops answering raises `TIMEOUT.GUI` on the others
  ("<name> will be rejected in N seconds"); its `REJECT` (the name at `0x506570`, compared at
  `0x453A14`) calls `0x453010(dpid, 6)` (`0x453A2B`), which holds four of the removal `0x452CC0`'s
  nine calls (`0x453169`, `0x453200`, `0x4532C8`, `0x4532FA`; which one reason 6 takes is not
  traced). MEASURED: a peer frozen with `SIGSTOP` raised it on both others within seconds, and
  `REJECT` ran the kill-all of the frozen player's army on the peer that pressed it. When the peer
  that pressed it was the host, the third peer removed the player at the same moment, without its
  own `REJECT` (MEASURED once, 2026-09-27); after the host's console `+kill` instead, the third
  peer's copies stood until its own `REJECT`.
- **The removal runs on whichever thread pumps.** The retail pump `0x453D40` holds no lock across its
  dispatch; the work branch's owned primary wrapper serializes it when armed. Retail tests `main+0x2A44` bit 0 (clear, it returns 0 at once, `0x453D4B..0x453D5C`, so
  outside a network session no message case runs), zeroes ten dwords and calls the receive `0x4534E0`
  (`0x453D40..0x453D94`), and the cases run on the caller's thread. During a network
  load the loader (`0x49727D`) and the game thread's load loop (`0x49852E`) both run it, and the
  leave case (`0x4550B8..0x4550D5`) tests only the seat record and bits 0 and 1 of
  `main+0x38D75` (`0x4550C2`, `0x4550CB`), so the removal `0x452CC0`, and its kill-all, can run on either thread.

### What the tests measured

MEASURED 2026-09-26 on Show Down with C4's fixture: an ATLAS carrying an ARM Commander clone,
AI towers, and unarmed Krogoth clones (29 918 HP) as the ring.

- **A carried unit takes no area damage.** A missile dealing 65 000 to the commander hit its ATLAS
  nine times and never touched the passenger — the victim test "uncarried (`+0x86` 0)" in the area
  pass above. So in stock a unit dies carried only through kill-all, a self-destruct, or its
  transport's cargo loop.
- **An aircraft is hit well past a weapon's `range`**: a missile tower at `range=150` downed an
  ATLAS 440 away.
- **A player's own tower takes no `attack unit` against the player's own aircraft**: the order is
  issued and the tower's order node reverts to its standing type (46).
- **The cargo loop's 30 000 leaves a passenger above 30 000 HP alive**: a 32 000-HP commander clone
  came down with 2 066 HP, detached, on the ground.
- A carried transport is picked by the pointer where `+0x2CBA` names it **and** the cursor
  (`main+0x2CBE`) reads 15; its passenger sits on top of it and reads 19.
- **A remote copy is carried too**: the host's copy of the joiner's loaded commander has `+0x86`
  naming the host's copy of the ATLAS.
- **A surrender kills the army before the peer leaves.** Through `tools/mp_leave.sh`, the joiner
  sent the deaths of all five of its remaining units (B8's `out` counter); its loaded ATLAS died
  with a kind other than 3, so its passenger died of the cargo loop's kind 6, a carried death that
  is not a self-destruct (`TX_BLAST_E`, on both peers), and the host destroyed every copy from those
  deaths. So a graceful leave leaves kill-all's non-local branch nothing to do.
- **Kill-all's local branch, and an AI that flies its own ATLAS.** In a single-player skirmish
  (both players' records have first dword 1: the human type 1, the AI type 2), the console's
  `+kill 1` hits the AI's units with 30 000 of kind 3, and the reaper destroys them within a
  second. The AI moves its loaded ATLAS about, so where the blast lands varies from run to run.
  In five runs with the pair still within 150 of its start, the passenger's carried `TX_BLAST_S`
  took 222 from each of the player's Krogoths 260 away (29 918 to 29 696). In one of them it also
  downed the player's own ATLAS about 1 000 away, whose passenger then died of the cargo loop's
  kind 6, carried: `TX_BLAST_E`. In two runs the AI had already unloaded its passenger and flown
  off; the passenger died uncarried and took stock's blast (about 8 100 from each). Three runs,
  one of them with an unkeyed control, took nothing, the pair's position not recorded: a blast
  out of the ring's reach [INFERRED]. MEASURED 2026-09-26 and 2026-09-27.
- **Kill-all's non-local branch detonates a passenger twice**: once at `0x486F9E`, and again in its
  destructor (`0x486D50`) since the cargo loop's 30 000 left its HP word at or below 0 and so the
  severity above 0. The ATLAS, not hit, detonates once.

## `0x48AB70` — attach and detach one unit to another — mapped by us

[MEASURED 2026-09-03, this project — `objdump -d -M intel` of the pristine Steam build. Read, not
patched, while answering "does a unit walk under the factory in the original too?". Full write-up,
including how a carried unit is drawn: [factories](../factory-build.html).]

`0x48AB70 .. 0x48AD2D`, `ret 4`. **One argument, and it is a packed command, not a unit** — which
is what tells you attach/detach is a *simulation* event, replicated by unit id rather than by
pointer:

| Packet | What |
| --- | --- |
| `+0x1` word | child unit id (`0` = none) |
| `+0x3` word | parent unit id (**`0` = detach**) |
| `+0x5` byte | attach point; **`0xFF` = "inside"** |
| `+0x6` byte | two low bits xor'd into `Object3do+0x2E` (`0x48ACE1..0x48ACF0`) |

**Unit id → address**, computed the long way at `0x48AB8A..0x48AB9F`: `id*8 − id` then
`lea eax,[eax+eax*4]` then `lea esi,[ecx+eax*8]` = `*(main+0x14357) + id*0x118`.

**The fields it owns**, all in `UnitStruct`: `+0x86` parent, `+0x8A` head of the carried chain,
`+0x8E` next sibling, `+0xF9` the attach-point byte, and bit `0x20000` of `+0x110`.

**The guards** (`0x48ABC7..0x48AC1D`, all bailing to `0x48AD2A` having done nothing) are the
interesting part, because each is a rule of the game engine: the child must be alive
(`+0x110 & 0x10000000`); **a structure can never be carried** (`+0x110 & 0x20000000` must be
clear); a unit that is itself carrying something cannot be attached (`+0x8A` must be `0`); and
**carrying does not nest** — the parent's own `+0x86` must be `0`.

**`+0x110 & 0x20000` is set iff the attach point is `0xFF`** (`and edx,0xfffdffff` /
`cmp cl,0xff` / `sete al` / `shl eax,0x11` / `or edx,eax`, `0x48AC88..0x48ACA7`). It is a
**"drawn by nobody" flag, not an "is cargo" flag**: the blit's cargo loop skips it at `0x459657`
*and* `DrawUnit 0x45AC20` skips it at `0x45AD43`, so a unit in a transport hold is drawn neither
by itself nor by its carrier. A unit attached to a *piece* — the one on a factory pad — has the
bit clear and is drawn by its carrier, merged through `0x4B90A0`.

**Call sites (4).** `0x455403`, the network dispatcher's case for code `0x0A` (`0x4553FE`, entry 8
of the jump table `0x455F84`, indexed by `code − 2`), which hands it the received record;
`0x48AB62` from the wrapper `0x48AAC0..0x48AB6A` (`ret 0x10`, 30 callers; it tests the unit alive
and sends the `0x0A` through `0x44FDB0`/`0x451DF0` before applying it); and the two in the `0x2C`
round robin's full-state entry `0x48B3F0` (called once, at `0x48BAC3`), which replicates a remote
unit's carrier: `0x48B58B` attaches to the carrier the stream names (a 15-bit id read at
`0x48B56B`, the point an 8-bit read at `0x48B579`, both by the stream's bit reader), and
**`0x48B5C5` the detach** — child id from `+0xA8`, parent id `0`, point `0xFF`, guarded on
`[edi+0x86] != 0` when the stream says the owner's unit has no carrier. Detaching is "attach to
nobody", and it happens in one step, so there is no state in which a unit is still in the chain but
positioned away from its carrier (DISASSEMBLED 2026-09-25).

**Neither id is bounded here.** Both are scaled into the array as they come (`0x48AB8A..0x48AB9F`,
`0x48ABAF..0x48ABC4`). The two callers that take an id off the wire are bounded before the call,
by B3's stubs ([the network
receivers](network.md#the-network-receivers-take-a-unit-index-straight-off-the-wire-0x09-0x0b-0x0c-0x2c-disassembled-2026-09-25)):
the `0x0A` case at `0x4553FE` (both ids; a bad record is dropped) and the round robin's carrier at
`0x48B574` (made 0). The wrapper's ids are live units' own `+0xA8`, and the round robin's child is
its slot's own. The attach point `+5`, a byte stored into the child's `+0xF9`, is not an index into
the unit array; its readers were not examined.

**`+0x8E` holds two lists.** `0x47CB00` takes a unit out of bucket `ecx` and clears its `+0x8E`
(`0x47CB26`); `0x47CB40` pushes it at the bucket's front (`0x47CB47`, `0x47CB4D`). The other
`0x47Cxxx` writers (`0x47CBA3`, `0x47CBB0`, `0x47CC13`, `0x47CD0F`, `0x47D0B9`) are the bucket's
too, each behind a `+0x86` = 0 test or a caller that made it, so `+0x8E` is the bucket's link
while the unit is free and the carrier's chain while it is carried (*The unit and player
records, byte by byte*).
