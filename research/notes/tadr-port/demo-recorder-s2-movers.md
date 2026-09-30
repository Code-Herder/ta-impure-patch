# Demo recorder S2 — movers and carried units (evidence)

## Status

**Desk research, 2026-09-30**, for the [skirmish slice](demo-recorder-skirmish-slice.md)'s S2 (*S2 — watching*), with its owner's decisions in that note's *Decided 2026-09-30*. S2a, the recording half, implements what this page found for the recorder and verifies it (the [slice note](demo-recorder-skirmish-slice.md)'s *S2a*); S2b builds the playback on the rest. Engine facts come from `pristine/TotalA.exe.pristine` (image base 0x400000), disassembled with `i686-w64-mingw32-objdump -d -M intel`. Tags: **DISASSEMBLED** (with the addresses), **MEASURED** (offline re-reading of the 2026-09-29 probe's own PRS1 snapshots, `_local/demo-recorder-exploration/rebuild-takeover-2026-09-29/probe_e3r*.bin` against `probe_e3p*.bin` / `probe_e3q*.bin`, main checkout, gitignored), and **INFERRED**. Names in brackets are this research's.


---

## 0. Answers in brief

1. **The move object** is one 0x2F-byte struct for every mobile unit. One constructor (`0x43DC00`)
   and one allocation size (`push 0x2F` in both creates) cover every kind; the kind lives in the
   *class object* that `+0x00` points to. That object is chosen once, at the create, from the
   owner's controller as the viewing machine sees it: local or remote proxy, ground or air.
   The mover is made by the two creates only and freed only by the unit destructor, which nulls
   the pointer, so it cannot change during a unit's life. Every byte is classified in §1.3. Only
   three things are not plain values:
   - **`+0x00`**, the class object: a different kind on every machine, so `drop`.
   - **`+0x04`**, the movement class, set once from the type: `cache`.
   - **`+0x2E` bits 3–7**, allocator garbage that no code reads: `drop`.

   Everything else can be recorded whole. On a puppet, some fields are written raw, some go
   through engine functions, and some are never written (§1.6). **The placement part `0x43D6D0`
   and the altitude fix-up `0x48A870` must not run on a puppet** once the whole mover is
   recorded (§2).
2. **Carried units.**
   - **Attach and detach** go through one function, `0x48AB70`, which takes a 7-byte `0x0A`
     record. `0x48AAC0` is the wrapper that validates, broadcasts and applies it. It has twelve
     callers.
   - **What playback writes.** The playback reproduces the carry links by calling `0x48AB70`
     directly, driven by the difference between the recorded and the puppet's carrier state.
     Then it places the cargo with the engine's own set-position `0x48A9F0` at the recorded
     position, and never runs the carried branch of `0x43D6D0` (§3.8).
   - **The probe's "1–4 wu off the pad, one 65" is explained (MEASURED).** It was the
     **altitude**; x/y matched exactly. The frames were never attached on the puppet, so the
     probe's placement set `+0x110` bit 16 and its `MovePost` (`0x48A870`) refitted them to the
     terrain or the sea floor. On the recording machine, the carried branch clears bit 16
     (`0x43D7EB`), so the fix-up never touches a carried unit (§3.5).
3. **Size.** For a moving uncarried unit, `+0x2A` becomes GameTime on every tick it moves. The
   speed changes about 200 times a tick in a 1,432-unit battle (MEASURED), and the velocity with
   it. Aircraft also change their three smoothing dwords every tick. A stationary unit changes
   nothing. Estimate: +10 to +13 KB an iteration on the battle's ~20 KB (INFERRED, §4).

---

## 1. The move object

### 1.1 Lifetime: every create and free, and why the pointer never changes

| where | what | evidence |
|---|---|---|
| local create `0x485F50` | `def+0x22F == 1` (`0x4860BB`) → `MEM_Alloc(0x2F)` (`0x4860C4`/`0x4860C6`) → ctor `0x43DC00` (`0x4860D5`) → store at unit `+0x00` (`0x4860E4`); NULL on allocation failure (`0x4860DC`); heading `+0x66` = BuildAngle `def+0x210` (`0x4860ED`); unit layer from argument 7 (`0x486103`), but the mover's layer stays 1; then the fix-up `0x48A870` (`0x486109`) and the claim `0x47CC30` (`0x48610F`, which stamps mover `+0x26`) | DISASSEMBLED |
| CreateFromNetwork `0x4861D0` | the same at `0x4862D8..0x486301` (ctor `0x4862E9`, store `0x4862F8`), then `0x48A870` (`0x486306`) and `0x47CC30` (`0x48630C`) | DISASSEMBLED |
| `0x485E50`, `0x485E90` | build a mover too, but nothing references them (whole-image scan for rel32 calls and jumps and for absolute immediates; the evidence note's P26 found the same for `0x485E50`) | DISASSEMBLED |
| destructor `0x4866D0` | `0x43DD10` (the class object's `[vt+0](1)`, `0x486DB1`) → `MEM_Free 0x4B4F20` (`0x486DB7`) → `[unit] = 0` (`0x486DBF`) → `+0xA6 = 0` (`0x486DC7`) → `+0x110` bit 28 cleared (`0x486DCE`) | DISASSEMBLED |
| level fill `0x4854A0` | zeroes every unit record, `+0x00` included | DISASSEMBLED (S1) |
| stock save and restore | `0x43DD70` writes 0x23 bytes under `u%04xmob` (`0x505258`): `+0x08..+0x29` and one byte with `+0x2E` bits 0–2 (bits 3–7 of that byte are stack garbage, `0x43DDB7..0x43DDDA`). `0x43DE30` writes them back **into the existing mover** and keeps `+0x2E` bits 3–7 (`0x43DEC8..0x43DEDA`). Not saved: `+0x00`, `+0x04`, `+0x2A..+0x2D` | DISASSEMBLED |

No other code writes unit `+0x00`. **The pointer is set once at create and cleared once at
death**, so it cannot change during a unit's life. A takeover re-creates the units, which gives
each a fresh mover of the new kind (evidence note, P26).

**The allocation is 0x2F bytes**, not 0x30 (`0x4860C4`). An image must read exactly 0x2F bytes and
zero-fill the twelfth dword's last byte, never read byte `+0x2F`.

### 1.2 One layout, four kinds

The ctor stores the class object after testing `[owner] != 0 && owner+0x73 == 3`
(`0x43DC48..0x43DC57`) and the type's `def+0x241` bit 11 (air, `0x43DC5F..0x43DC6B`):

| owner `+0x73` in the local game | air | class object | bytes | vtable | ctor |
|---|---|---|---|---|---|
| 3 (remote) | yes | remote air proxy | 0x27 | `0x4FD9E0` | `0x490940` (`0x43DC82`) |
| 3 | no | remote ground proxy | 0x1C | `0x4FD488` | `0x44F570` (`0x43DCA1`) |
| otherwise | yes | local air | 0x28 | `0x4FD9B0` | `0x4907E0` (`0x43DCD3`) |
| otherwise | no | local ground | 0x65 | `0x4FD458` | `0x44F010` (`0x43DCF2`) |

- **The base class.** It is made by `0x44EF20`, with vtable `0x4FD428`, `+0x04 = 0`, and
  `+0x08` = the unit.
- **The air classes** re-base to `0x4FD980`. They copy the unit's position into `+0x0C..+0x14`,
  zero `+0x18..+0x20`, and copy the heading into `+0x24` (`0x4907EF..0x49082B`,
  `0x49094F..0x490982`).
- **The ground proxy's step** `[vt+8]` is a bare `ret` (`0x44EFB0`).
- **The ground proxy's payload reader** `[vt+0x24]` is `0x44F5C0`. It reads one bit into the
  mover's `+0x2E` bit 2 (`0x44F60A`), then a count of waypoint pairs into the proxy.

All DISASSEMBLED.

**The kind is machine-specific by construction.** In a skirmish, every seat is local on the
recording machine, so its units get local classes. A puppet host holds 3 for every seat, so its
units get proxies. The class object is the only part of the mover whose kind differs, and no
recorded field is reached through it.

### 1.3 Byte by byte

Each row gives the default value from the ctor `0x43DC00`, the writers and readers found, and
whether the stock save carries the field. **P** marks what the probe wrote. **Parts** marks which
of the three playback parts read it: `D6` = `0x43D6D0`, `DA` = `0x43DA70`, `DB` = `0x43DB50`.

| off | size | name | ctor | writers (DISASSEMBLED) | readers (DISASSEMBLED) | save | P | parts | class |
|---|---|---|---|---|---|---|---|---|---|
| `+0x00` | 4 | [course]: the class object (§1.2) | stored (`0x43DC87`, `0x43DCA6`, `0x43DCD8`, `0x43DCF7`; NULL `0x43DD03`) | ctor only | the movement pass `[vt+8]` (`0x43DD28`); the `0x2C` sweep's payload call `[vt+0x24]` (`0x48BA10`); the dtor `0x43DD10` | no | | | **drop** |
| `+0x04` | 4 | [moveclass] | `def+0x1B6` (`0x43DC45`) | ctor only | pathfinder `0x40E63F` (hands it to `0x440AF0` at `0x40E655`), `0x40ED4A` | no | | | **cache** |
| `+0x08` | 4 | velocity x (16.16) | 0 (`0x43DC0D`) | ground step `0x43CC20` (−sin h · speed); air step `0x43D290`; layer change `0x43D210` (0 on layer 1, `0x43D232`); placement `0x43D6D0` (a cargo's copy of its carrier's, `0x43D7C7`, or 0, `0x43D7E2`; the blocked cap's recompute, `0x43D9DD`); restore `0x43DE8A` | `0x43D6D0` (`0x43D7FF`: pos += vel); a cargo's placement reads its carrier's (`0x43D7C5`); VTOL order lead `0x413264` (a *target's* pos + 45·v, no NULL test) | yes | | D6 | value |
| `+0x0C` | 4 | velocity alt | 0 (`0x43DC19`) | air step `0x43D290` (clamped against the bucket or off-map height, `0x43D4B8`); the same copy and zeroing sites | `0x43D6D0` (`0x43D805..0x43D813`) | yes | | D6 | value |
| `+0x10` | 4 | velocity y | 0 (`0x43DC1C`) | as `+0x08` (−cos h · speed) | as `+0x08`; `0x413277` | yes | | D6 | value |
| `+0x14..+0x1F` | 12 | [accel smoothing] x, alt, y (16.16) | 0 (`0x43DC22`, `0x43DC2A`, `0x43DC32`) | `0x43D0D0` only (×0xF333/0x10000 ≈ 0.95, plus the step's acceleration; from `0x43D25B` in the layer change and `0x43D6B3` in the air step); restore | `0x43D0D0`, which then writes the unit's pitch `+0x64` and roll `+0x68` from `def+0x1A2`/`+0x1A6` | yes | | | value |
| `+0x20` | 4 | speed | 0 (`0x43DC0F`) | ground step `0x43CC20` (accelerate, floor 0, cap = slope factor · `def+0x192`, halved below the sea unless `def+0x241 & 0x81000`); `0x43CD20` (brakes by `def+0x19A` with no course); air `0x43D688` (\|v\|); `0x43D23A` (0 on layer 1); placement copy `0x43D7B4` / 0 `0x43D7D8` / blocked cap `0x43D9B6`; round robin `0x48B6F9`; restore | `0x43DA87`; `0x43D9AD` (the cap test); a cargo reads its carrier's (`0x43D7B1`); hover wave `0x48A69A`; draw helper `0x458866` (`0x458810` from DrawUnit `0x45AE73`: presentation); round-robin sender `0x48B3CF`; **the shooter's, in the fire function `0x49DDF5`/`0x49DE0D` and the shot receiver `0x49D50F`/`0x49D528`** (§2.4); `0x49D081` in `0x49D000`, which nothing references | yes | **P** | D6, DA | value |
| `+0x24` | 2 | turn rate | 0 (`0x43DC12`) | `0x43CBB0` (clamped to ±`def+0x1BA`, heading += turn, bit 16); `0x43CD20` (0 with no course); air step; restore `0x43DEBD` | `0x43DA8E`; `0x43CBB0` | yes | | DA | value |
| `+0x26` | 4 | [claim stamp] | 0 (`0x43DC16`) | claim `0x47CC30` (`0x47CC54`, GameTime); release's `0x440A70` (`0x440A91`, GameTime, from `0x47D2CA`); pathfinder `0x440AF0` (`0x440B30` writes, `0x440BC4` restores); restore `0x43DEC5` | `0x440A83` (the old value against the class record's `+0x1C`); `0x440B2D`, `0x440B83`, `0x440BEA`; **the cell tests `0x47DF22`, `0x47E137` read another unit's**, the cell occupant's, against the class map's refresh stamp `+0x1C` | yes | | | value |
| `+0x2A` | 4 | [moved at] | **not initialized**: heap garbage until the first write | `0x43D6D0` only (`0x43D865`: GameTime, whenever an uncarried unit's velocity is non-zero or its mover layer differs from its unit layer, `0x43D827..0x43D854`) | hover wave `0x48A6DE` (the amplitude ramps in over 60 ticks from it, `0x48A6E8..0x48A6FA`) | **no** | | D6 (writes) | value |
| `+0x2E` bits 0–1 | — | layer | 1 (`0x43DC25..0x43DC2F`) | `0x43D210` (`0x43D284`); attach `0x48AB70` (`0x48ACF0`, from record byte 6); restore (`0x43DEDA`) | `0x43D6D0` (`0x43D75C` carried, `0x43D81C` uncarried); `0x43D210`; air class `0x490880`, `0x490913` | yes | **P** | D6 | value |
| `+0x2E` bit 2 | — | [blocked] | 0 (`0x43DC28`) | `0x43D6D0` for a local owner only (`0x43D8E3..0x43D92C`: set when `0x47DB70(def, +0xA8, new cell, layer)` answers 0, which means the next cell cannot be occupied); ground payload reader `0x44F60A`; restore | `0x43DA79` (class 0); `0x43D92F` (clamp to the cell ±0x7FFFF, cap the speed at `def+0x192`/2, recompute the velocity) | yes | **P** | D6, DA | value |
| `+0x2E` bits 3–7 | — | [spare] | kept (`and 0xF9`, `or 1`) | none: every writer masks them out (`0x43D27D`, `0x43D91C`, `0x43DED3`, `0x48ACE9`, `0x44F605`) | none | no (stack bytes) | | | **drop** |

- **Names.** "course", "moveclass", "accel smoothing", "claim stamp", "moved at", "blocked" and
  "spare" are [INFERRED]. The behaviour each row states is DISASSEMBLED.
- **`+0x04`.** `def+0x1B6` is the type's `movementclass` (the key string at `0x503990`). The type
  loader resolves it through `0x440420` (`0x42CD0C`, `0x42CD29`) to one of 32 entries of 0x20
  bytes in the static table `0x512358..0x512758`, or 0.

**What the three parts read.**
- **`0x43D6D0` reads** the mover's velocity, speed and `+0x2E`, and on the unit `+0x86`, `+0xF9`,
  the position, the cell `+0x76`/`+0x78`, the footprint offset `+0x7E`/`+0x80`, the layer and the
  owner. For a cargo, it also reads the carrier's mover (`+0x08..+0x10`, `+0x20`) and the
  carrier's model: `0x43E060`, the attach point (`0x43D6F9`), and `0x43E180`, its orientation
  (`0x43D78E`).
- **`0x43DA70` reads** mover `+0x20`, `+0x24` and `+0x2E` bit 2, unit `+0x86` and `+0x110` bits
  2–3, and `def+0x1AE`/`+0x1B2`. It writes bits 2–3 (`0x43DB3D`). On a change it starts
  `StopMoving` (`0x50523C`), `StartMoving` (`0x505230`, only from class 0) and `MoveRate1..3`
  (`0x50520C`/`0x505218`/`0x505224`) through `0x4B0940`.
- **`0x43DB50` reads no mover field.** It reads the unit's layer, the altitude word `+0x70`, the
  old `+0x10A`, `def+0x22C`/`+0x170` and the sea level `main+0x1427F`. On a change it starts
  `setSFXoccupy(new)` (`0x505248`, `0x43DBE8`) and stores the new `+0x10A` (`0x43DBED`).

All DISASSEMBLED.

### 1.4 The rows

The mover is the generator's third record (`RECORDS["mover"] = 0x2F` in `tools/replay-layout-gen.py`),
with three rows in `tagpu/ddraw/replay-layout.tsv`, the source of truth, which the owner approved
2026-09-30: `+0x000` `course` drop, `+0x004` `moveclass` cache, `+0x02E` mask `0xF8` `spare` drop. The
`spare` row names the two creates as writers: the generator requires at least one writer, and the
only "writer" is the allocation. The unit row `+0x000` (`object mover`) stays; its evidence says the
mover record records it. Every other mover byte is a value, recorded whole: velocity, smoothing,
speed, turn, `+0x26`, `+0x2A`, and `+0x2E` bits 0–2.

**The image** is option (a): a 12-dword tail on the unit's image (82 dwords, an 11-byte delta
bitmap), `REC_FORMAT` 2, whose unit image `+0x00` holds 1 when the unit has a mover (a NULL is
possible after an allocation failure) and whose tail is zero when it has none (`tagpu_recfmt.h`).

### 1.5 The pointer fields

- **`+0x00`: `drop`, not `object`.** `object` means "recorded by a record of its own", and no
  record carries the class object. Its kind legitimately differs between the recording machine
  and a puppet host (§1.2). On the recording machine it holds the unit's path and goal, whose
  results (velocity, speed, turn, bit 2) are all in the mover. A takeover re-creates the unit,
  which gives a fresh local class that plans a new path from the orders. So the path is **not**
  reproduced across a takeover: stated for S4, not closed.
- **`+0x04`: `cache`.** The table is static data of the executable, and the entry is a function
  of the type, written only by the ctor. **Alternative:** a new ID table `moveclass`
  (`(pointer − 0x512358) / 0x20`) and an `id:moveclass` row. That needs an `ID_TABLES` entry, and
  records a value the create already guarantees.
- **The class object's own pointers** are not recorded: `+0x08` points back to the unit, and the
  air class's `+0x04` is a sub-object whose `[vt+8]` answers 2 or 3 to `0x4908C0` [INFERRED:
  course state]. The class object is out of scope for S2.

### 1.6 Recording whole and writing back: sound?

**Recording whole is sound**, with the three rows above.
- **Reading is safe by ordering.** The capture runs on the game thread between iterations
  (`0x4954ED`). Every write of unit `+0x00` also runs on the game thread: the creates, and the
  destructor, whose free, null and bit-28 clear happen inside one call (`0x486DB1..0x486DCE`).
  So a live unit (bit 28) with a non-NULL `+0x00` points at a live mover whenever the capture
  looks.
- **A range test on the pointer's value** is a cheap filter, never the argument.
- **Read 0x2F bytes**, never 0x30 (§1.1).

**Writing back on a puppet is sound only field by field:**

| fields | on a puppet | why |
|---|---|---|
| `+0x00`, `+0x04`, `+0x2E` bits 3–7 | **never** | machine-specific or unread (§1.4) |
| `+0x08..+0x1F` velocity, smoothing | raw, **after** the tick's shots, and never before a call of `0x43D6D0` (which is not called; §2) | `0x43D6D0` adds the velocity to the position (`0x43D80B..0x43D81F`): the double step the probe measured with the whole pass |
| `+0x20` speed, `+0x24` turn, `+0x2E` bits 0–2 | raw, after the tick's shots and **before** `0x43DA70` | the walk class is computed from them. The shot receiver reads the shooter's speed (§2.4) |
| `+0x26`, `+0x2A` | raw, **after** `0x48A9F0` | the claim and release overwrite `+0x26` with the puppet's GameTime (`0x47CC54`, `0x440A91`) |
| layer through `0x43D210` | **never** | it calls `0x48B090(unit, 1, …)` (`0x43D265`, `0x43D275`), and the recorder is `+0x10E`'s one writer |

---

## 2. The placement on a puppet: `0x48A9F0` instead of `0x43D6D0`

The skirmish slice's S2 rules inherit "step only the movement pass's three presentation parts,
writing the mover's layer and its `+0x2E` bit 2". With the whole mover recorded, I recommend
changing that to **"place with `0x48A9F0`, then step `0x43DA70` and `0x43DB50`"**. It changes an
inherited rule, so the decision is the owner's.

### 2.1 Why not `0x43D6D0`

- **The uncarried branch computes `pos + velocity`** (`0x43D7FF..0x43D81F`). Once the recorded
  velocity is written, it moves every moving unit a second step. With it not written (the probe),
  the velocity is stale. The probe measured the same failure with the whole pass: a third of the
  units about 0.8 of their own last step off, up to 454 wu.
- **Its carried branch places the cargo** at `0x43E060(carrier, +0xF9)` with the orientation from
  `0x43E180`. Both are computed from the **puppet's** model pose of the carrier, and S2 records no
  piece pose.
- **Everything else it produces is recorded:** position, unit layer, cell, `+0x2A`, bit 2 (only a
  local owner recomputes it, `0x43D8E3..0x43D8F7`, so never on a puppet), the capped speed and
  velocity, and bit 16.

### 2.2 Why not `0x48A870` (the probe's `MovePost`)

- **What it does.** It runs when bit 16 is set or for `canhover` (`0x48A877..0x48A893`), and acts
  on a unit with a mover on unit layer 1 (`0x48A8AE..0x48A8B3`). It rewrites the altitude and the
  tilts `+0x64`/`+0x68`, and for `canhover` it uses the wall clock.
- **Everything it writes is a recorded value.**
- **Running it on a puppet** replaces the recorded altitude with the puppet's terrain fit. That is
  exactly the probe's carried-frame fault (§3.5).

### 2.3 The recipe, per unit with a mover, in ascending slot order (the unit tick's order, `0x48ADB0` loop)

First the carry changes (§3.8). Then, for each unit:

1. **Write the mover inputs raw:** velocity, smoothing, speed, turn, and `+0x2E` bits 0–2. Write
   the unit's turn words `+0x64..+0x69` raw too; no part reads them.
2. **Call `0x48A9F0(unit, x, a_pre, y, layer)`** (stdcall, `ret 0x14`) with the recorded x, y and
   unit layer.
   - When the cell (from x/y and `+0x7E`/`+0x80`) or the layer changed, it releases
     (`0x47D0E0`), writes the position, cell and layer, claims (`0x47CC30`) and re-stamps sight
     (`0x4827B0`). Otherwise it writes the position only. It then sets bit 16
     (`0x48AA00..0x48AAB1`).
   - This is the same release, claim and sight sequence as `0x43D6D0`'s relink
     (`0x43DA0E..0x43DA59`), under the same condition. The bit-2 path keeps the cell, so it
     relinks nothing in either function.
   - Calling it in slot order therefore repeats the recording machine's claims in its order
     [INFERRED: the grid is not compared in S2].
   - **`a_pre` is the altitude `0x43D6D0` produced**, before the fix-up ran after the pass
     (`0x48AFB0`). For an uncarried unit it is the previous altitude plus the recorded vertical
     velocity (`0x43D80D..0x43D813`). For a carried unit it is the recorded altitude, since the
     carried branch clears bit 16 and the fix-up does not run.
   - Feeding `a_pre` reproduces the two inputs the recording machine used at this point: the
     sight re-stamp's height inside the relink, and `0x43DB50`'s altitude (step 4).
3. **Call `0x43DA70(mover, unit)`** (thiscall on the mover, `ret 4`). Its inputs are now the
   tick's final values, as on the recording machine. On a change it writes `+0x110` bits 2–3
   and starts the walk scripts.
4. **Call `0x43DB50(mover, unit)`.** It writes `+0x10A` and starts `setSFXoccupy` on a change.
5. **Write the rest raw:** the recorded altitude `+0x6E..+0x71` (the fix-up's result), the tilts,
   `+0x110` bit 16 (the fix-up cleared it on the recording machine), mover `+0x26` and `+0x2A`.
6. **Compare, never write:**
   - `+0x110` bits 2–3 and `+0x10A`, which the parts own;
   - the cell `+0x76`/`+0x78`, the layer bits, the bucket `+0x82`/`+0x8E` and the sight stamp
     `+0x7A`/`+0x7C`/`+0xF8`, which the engine functions own.

   A difference the harness names is a bug in the playback, as for any other field.

Of the values the parts transition, **`+0x110` bits 2–3 and `+0x10A` are written only by the
parts on a puppet, never raw**. Written first, the parts would see no change and start no script
(`0x43DACD`, `0x43DBCC`). The same holds for a rebuild (S3): a fresh unit has 0 in both, and the
first stepped tick starts what is running. These two fields join the completeness note's list of
"values that go through engine functions, never a raw write". No `cache` row is needed, since the
values stay recorded and compared.

### 2.4 Order against the tick's events

- **Shots first, against the previous tick's state.**
  - AutoAim `0x49E1A0` runs early in each unit's tick (`0x48ADDA`), before the movement pass
    (`0x48AFAA`).
  - The dropped-weapon fire function (`0x49DD60`) and the shot receiver (`0x49D270`) both set the
    projectile's velocity from the **shooter's mover `+0x20` and heading `+0x66`**
    (`0x49DDE5..0x49DE19`, `0x49D501..0x49D53A`).
  - So in a skirmish, where every seat is local on the recording machine, a shot of tick N was
    built from the shooter's speed and heading at the end of tick N−1. The playback replays a
    tick's shots before writing any of that tick's unit or mover images.
  - Neither function tests the shooter's mover for NULL. Before a replayed shot, the playback
    checks that the shooter has a mover.
- **Deaths and creates before the carry diff** (§3.8). The death path detaches a carried unit and
  its cargo itself (`0x4867CB`, `0x48681D`).
- **A walk-script start is not an event.** `0x43DA70` and `0x43DB50` start their scripts through
  the COB call (`0x4B0940`, `0x4B0A70`), not through the five broadcasters the recorder hooks. So
  the puppet produces them itself by calling the parts, and recording them would double them.

---

## 3. Carried units

### 3.1 The fields

| field | what | written by |
|---|---|---|
| unit `+0x86` | the carrier | only `0x48AB70` (`0x48AC70`, `0x48ACD2`) |
| unit `+0x8A` | the first cargo | `0x48AB70` (`0x48AC82`, and the unlink through its address) |
| unit `+0x8E` | the next cargo while `+0x86` is set, else the next unit in the sort bucket | `0x48AB70`, the bucket functions `0x47CB00`/`0x47CB40`/`0x47CBD0`/`0x47CC30` |
| unit `+0xF9` | the attach piece; `0xFF` means none | `0x48AC68`, `0x485D14` (create), `0x48721F` (restore) |
| unit `+0x110` bit 17 | carried with piece `0xFF`: drawn hidden (the blit's chain skip `0x459657`) | attach `0x48ACA7`, detach `0x48ACC6` |
| mover `+0x2E` bits 0–1 | the cargo's layer, from the record's byte 6 | `0x48ACF0` |

All DISASSEMBLED. The layout file already has `+0x086`, `+0x08A` and `+0x08E` as `id:unit` rows,
and `+0xF9` and bit 17 as values.

### 3.2 The apply: `0x48AB70(rec)` (stdcall, `ret 4`)

**The record** is 7 bytes: `0x0A`, the child's u16 index, the parent's u16 index (0 means
detach), the piece byte, and the layer byte. Each index becomes `base + 0x118 · index` with no
bound, and index 0 is NULL (`0x48AB7D..0x48ABC4`).

**It refuses** (`0x48ABC7..0x48AC1D`):
- a NULL child;
- a child not alive (bit 28) or with **bit 29, the type has no mover**;
- a child that carries something (`+0x8A`);
- a parent not alive, a parent equal to the child, or a parent that is itself carried.

**Then it:**
1. Unlinks the child from its carrier's chain by walking it (`0x48AC2D..0x48AC53`), or from its
   bucket through `0x47CB00`.
2. Writes `+0xF9`.
3. **Attach:** `+0x86` = the parent, `+0x8E` = the parent's head, then pushes the child to the
   front of the chain and sets bit 17 when the piece is `0xFF`.
4. **Detach:** clears bit 17, `+0x8E` and `+0x86`, and pushes the child into the bucket at its
   `+0x82` (`0x47CB40`, `0x48ACDC`).
5. Writes the mover's layer (`0x48ACE1..0x48ACF0`).
6. **For a type-1/2 owner's child**, when the parent's type lacks `def+0x241` bit 9, it calls
   `0x4384A0` (BeCarried and the order purge).
7. **Always** calls `0x48C9B0` (deselect).

All DISASSEMBLED.

**What it does not check:**
- **The indices' range.** Impure's `wire_attach_ok` covers this on the wire.
- **The child's mover pointer.** An allocation failure leaves bit 29 clear with `[child] = NULL`,
  and `0x48ACE6` then dereferences NULL.
- **That the child is in its carrier's chain.** The unlink loop runs to NULL and faults
  otherwise. So the chain's consistency is an invariant that only this function maintains.
- It moves no position and claims no cells.

### 3.3 The wrapper `0x48AAC0(child, parent, piece, layer)` and its callers

`0x48AAC0` validates as `0x48AB70` does (`0x48AAC7..0x48AB0D`). It builds the record from
`+0xA8`, sends it through `0x44FDB0` → `0x451DF0`, and applies it through `0x48AB70`
(`0x48AB62`). The twelve callers:

| site | what | parent | piece | layer | on a puppet host |
|---|---|---|---|---|---|
| `0x402948` | BuildingBuild: the new frame onto its factory | builder | from the build query (`[esp+0x4C]`) | 1 | no: local order code |
| `0x41B9A1` | completion: the finished unit off its pad | 0 | `0xFF` | 1 | no: local |
| `0x40F221` | VTOL take-off detach | 0 | `0xFF` | 2 | no: local |
| `0x4117A6` | VTOL_Unload, the chain's head | 0 | `0xFF` | 1 | no: local |
| `0x4813A6` | COB ATTACH (`0x481340`) | the script's unit | COB argument | COB flags | **inert on a puppet** (S2 decision) |
| `0x481422` | COB DROP (`0x4813B0`), after `0x47DB70` accepts the cell | 0 | `0xFF` | 1 | **inert on a puppet** |
| `0x4867CB` | destructor: the dying unit off its carrier | 0 | `0xFF` | 1 | **yes**, inside a replayed death |
| `0x48681D` | destructor: each cargo, after 30,000 damage through `0x489BB0` (`0x48680B`) | 0 | `0xFF` | 1 | **yes**, inside a replayed death |
| `0x4871FF` | save restore | | | | no |
| `0x488263` | mission load | | | | no |

`0x48AB70`'s other callers are the `0x0A` receiver (`0x455403`, bounded by Impure's `wire_s0a`)
and the round robin (`0x48B58B`, `0x48B5C5`). Neither runs on a puppet host.

The unload handlers take the chain's head (`0x406960`, `0x4115C7`). `0x406BC0` places units with
`0x48A9F0` on layer 1, in a loop over the unit array. All DISASSEMBLED.

### 3.4 How a stock peer moves a cargo, and why a puppet cannot copy it

- **The carried branch**, on whichever machine steps the unit (`0x43D6D0` at `0x43D6DD..0x43D7EB`):
  1. The attach point comes from `0x43E060(carrier, +0xF9)`. A `floater` gets the sea-level clamp
     (`0x43D718..0x43D75A`).
  2. `0x48A9F0(unit, point, mover layer)` places it (`0x43D775`), so a cargo does relink its cells
     and sight as it moves.
  3. The orientation from `0x43E180` goes into `+0x64..+0x69`.
  4. The speed and velocity are copied from the carrier's mover, or zeroed when the carrier has
     none, as a factory does (`0x43D7A5..0x43D7E8`).
  5. **Bit 16 is cleared** (`0x43D7EB`), so the fix-up leaves the altitude alone unless the type
     is `canhover`.
- **On a remote peer the same branch runs** from the `0x2C` receiver's sweep. That sweep runs
  `0x43DD20` and `0x48A870` over every alive unit with a mover in the sender's block
  (`0x48BA28..0x48BA5C`). The round robin never sends a carried unit's position: it re-applies
  the carrier and returns (`0x48B540..`).
- So **a stock peer recomputes the cargo's pose from its own copy of the carrier's model.** On a
  puppet that would place the cargo by the puppet's COB-driven piece pose, which S2 does not
  record. The recorded position is exact, and is what a puppet must use.

### 3.5 The probe's offset, explained (MEASURED)

I re-read the AI game's eight recorded checkpoints against the replayed ones: `probe_e3r1..8`
against `probe_e3p1..8`, and `e3r6..8` against `e3q6..8`.
- **The carried units.** Every unit carried in the recording (1, 2, 4, 5, 9 and 10 at checkpoints
  3–8) was uncarried in the replay. All were frames on a pad (piece 1, 2 or 6), with 5–95 % of
  the build left.
- **x/y matched exactly for every one** (displacement 0.00).
- **The altitude did not.** At checkpoint 7:

| frame (type) | carrier (type), altitude | frame altitude, recorded | replayed |
|---|---|---|---|
| 518 (252) | 512 (202), 136.00 | 136.05 | 140.00 |
| 519 (165) | 510 (202), 136.00 | 136.05 | 139.00 |
| 522 (172) | 516 (275), 136.00 | 136.42 | 138.42 |
| 1018 (51) | 1015 (14), 136.00 | 136.23 | 138.23 |
| 1515 (272) | 1513 (147), 138.00 | 140.34 | 139.34 |
| 1516 (172) | 1514 (275), 137.00 | 137.42 | 138.42 |
| 1518 (180) | 1517 (275), 136.00 | 136.42 | 138.42 |
| 2014 (37) | 2013 (120), 74.00 | 75.00 | 75.00 |
| 2016 (118) | 2015 (120), 74.00 | **65.25** | **0.00** |

**What these show.**
- **In the recording**, each frame sits at its pad piece: the carrier's altitude plus 0.05–2.34
  wu.
- **In the replay**, each sits where `0x48A870` put it: the probe's `pl_place` sets bit 16, then
  its `MovePost` runs the fix-up on layer 1. That is the terrain fit through `0x48A490`, or 0.00
  for frame 2016. [INFERRED: 2016's type has neither `floater` nor `canhover`, and 0 is the floor
  under a yard at 74 wu; its type was not identified.]
- **Nothing moved x/y.** A frame on the puppet had velocity 0, so `MovePlace` returned at
  `0x43D854`.
- **The replay's `+0x110` also lacks bit 13** on every such frame. At checkpoint 8, two frames
  also carry bits 26–27, which the recording's lack (§7).

So the fault was the missing link plus the fix-up. **Applying the link alone would not have fixed
it:** the carried branch would then have placed the frame by the puppet's own pad pose. §2's
recipe, with the recorded altitude and no fix-up, reproduces the recorded pose by construction.

### 3.6 The factory pad

- **Build start.** BuildingBuild creates the frame, then attaches it to the factory at the pad
  piece with layer 1 (`0x402948`).
- **While building**, the owner's machine places it each tick at the pad's attach point, turned
  to the piece. Its mover gets the factory's speed, which is zero because a factory has no mover.
  Its bit 16 is cleared, so the fix-up does not run.
- **At completion** it is detached with piece `0xFF` and layer 1 (`0x41B9A1`). It then walks as an
  ordinary uncarried unit, from the position the carried branch last gave it.
- **On a puppet** the frame's position, turn and altitude are recorded values, placed as §2 says.
  Its carrier link comes from §3.8.
- The evidence note's residual still stands: after a rebuild, a lab's frame comes back
  BeCarried-then-GetBuilt where the recording had GetBuilt-then-BeCarried. That is about order
  nodes, not the link.

### 3.7 Transports loading and unloading

- **Ground and air transports** attach and drop through their COB (ATTACH `0x481340`, DROP
  `0x4813B0`). The layer comes from the script's flags, or 1 for DROP.
- **VTOL transports** also detach through the order code: take-off with layer 2 (`0x40F221`), and
  unload of the chain's head with layer 1 (`0x4117A6`).
- **On a puppet** COB ATTACH and DROP are inert (decision of 2026-09-29) and the order code does
  not run, so **the recorded state is the only writer of the links**. The transport's scripts
  still animate on the puppet (the arm, the pickup) from the parts and the replayed script starts.
- **A dying transport** damages each cargo by 30,000 and detaches it inside its own death
  (`0x486804..0x48681D`). A replayed death does the same on the puppet. The events track has to
  keep the cargo's own recorded death from being applied a second time. That is outside this
  track, and noted here because it touches the links.

### 3.8 The playback recipe for the links

Per tick, after the tick's deaths and creates and before §2.3's placement:

1. **Detach pass.** For every live puppet unit whose puppet carrier (`+0x86`) differs from the
   recorded one, or whose piece `+0xF9` differs while carried, call
   `0x48AB70({0x0A, slot, 0, 0xFF, recorded mover layer})`.
2. **Attach pass.** For each recorded carrier whose recorded chain differs from the puppet's:
   1. Walk the recorded chain from `+0x8A` through `+0x8E`. Keep only members whose recorded
      `+0x86` is that carrier, and stop at the array size or at a repeat.
   2. If the puppet's chain is not a suffix of it, detach every member still on the puppet's
      chain first.
   3. Attach the missing members **from the chain's tail to its head**, each with its recorded
      piece and mover layer. Because the attach pushes at the front (`0x48AC82`), attaching in
      reverse rebuilds the recorded order exactly, and a detach unlinks in place. So the result
      is the recorded chain whatever happened within the tick.
3. **Bound every call before it is made:**
   - the child is in `[1, max]` and the parent is 0 or in `[1, max]` (`wire_attach_ok`);
   - both are alive (bit 28);
   - the child has bit 29 clear **and a non-NULL `+0x00`**;
   - the child carries nothing;
   - the parent is not carried.

   This mirrors `tx_detach`'s test and adds the mover check. A refused link is a named harness
   failure, never a silent skip.
4. **Never write `+0x86`, `+0x8A`, `+0x8E`, `+0xF9` or `+0x110` bit 17 raw.** Only `0x48AB70`
   keeps the chain consistent, and its own unlink walk faults on an inconsistent chain. The raw
   image write skips these fields and compares them.

**Why a state difference rather than recorded attach events:**
- the tick's end state already contains every attach and detach, whatever its root: COB, orders,
  a death;
- a detach nested in a replayed death is already done when the pass runs, so it is not repeated;
- no new event kind is needed.

**What a puppet-host `0x48AB70` call skips:** the broadcast, because the wrapper is not used, and
`0x4384A0`, because every seat is 3. It still deselects (`0x48C9B0`), which is the viewer's own
state, as on a stock peer.

**The renderer draws a cargo as part of its carrier's sprite, walking the chain** (the comment at
`tagpu_native.c:2586`; the engine's blit loop `0x459646..0x4596DD`, which skips bit 17). So the
chain has to be right for the picture as well as for the simulation.

---

## 4. Size (qualitative, with measured anchors)

**Anchors (MEASURED).**
- The S1 stream averages 20 KB an iteration in a 1,099–1,404-unit battle.
- In the probe's 1,432-unit battle minute, the speed changed 363,231 times (≈ 200 a tick), and
  68 % of the unit-ticks changed something.
- 335–677 units were walking at the four checkpoints.

**What changes, per dword of the mover image** (DISASSEMBLED semantics, INFERRED rates):

| dword | holds | changes |
|---|---|---|
| `+0x00`, `+0x04` | drop, cache | never in the image |
| `+0x08`, `+0x10` | velocity x, y | whenever the speed or heading changes: ≈ the speed changes plus turning units |
| `+0x0C` | velocity alt | aircraft |
| `+0x14..+0x1C` | smoothing | aircraft: every tick while flying, since the 0.95 leak takes hundreds of ticks to reach 0 in 16.16 |
| `+0x20` | speed | ≈ 200 a tick (MEASURED) |
| `+0x24` | turn word + low half of `+0x26` | turning, or a new cell (every 16 wu of travel) |
| `+0x28` | high half of `+0x26` + low half of `+0x2A` | **every tick a unit moves** (`+0x2A` = GameTime) |
| `+0x2C` | high half of `+0x2A` + `+0x2E` | a layer or bit-2 change; the GameTime carry every 65,536 ticks |

**Estimate [INFERRED].**
- **Per unit:** a cruising ground unit changes 1 dword a tick (`+0x28`), a turning or
  accelerating one 3–5, and an aircraft 8–10.
- **Per tick:** with 400–600 moving units, about 1,500–3,000 dwords, 6–12 KB.
- **Format (a)** adds 4 bytes to each of ~970 unit deltas (≈ 3.9 KB). Format (b) adds an 8-byte
  header per changed mover instead. Either comes to **about +10 to +13 KB an iteration on the
  battle's ~20 KB**.
- **The largest single term is `+0x2A`**: 4 bytes per moving unit per tick, which a repack can
  predict as "= this tick's GameTime". Speed and velocity come next.
- **A stationary or carried unit** changes nothing except what its carrier copies into it.

---

## 5. The decisions

All six were taken as recommended on 2026-09-30: item 1 by the owner (the rows), items 2–6 as this research's calls in the slice note's *Decided 2026-09-30*; S2a implements items 1, 5 and 6, and S2b's placement and carry links are items 2–4.


1. **The three mover rows** (§1.4): `+0x00` drop, `+0x04` cache, and `+0x2E` mask `0xF8` drop.
   *Recommend as drafted.* The alternative for `+0x04` is an `id:moveclass` table, which records
   what the create guarantees.
2. **Replace the inherited rule "step the three presentation parts"** by "place with `0x48A9F0` at
   the recorded position (altitude `a_pre`), step `0x43DA70` and `0x43DB50`, never `0x43D6D0` or
   `0x48A870` on a puppet" (§2). *Recommend.* The inherited rule was right for the probe, which
   did not record the velocity. With the velocity recorded it double-steps, and its carried
   branch depends on unrecorded piece poses.
3. **`+0x110` bits 2–3 and `+0x10A`: written on a puppet only by the parts, compared, never
   raw** (§2.3). *Recommend*: otherwise the walk and occupy scripts never start.
4. **Carry links by state difference through `0x48AB70`, with the bounds of §3.8**, rather than
   recorded attach events. *Recommend.*
5. **The image**: (a) a tail on the unit image, or (b) a record pair of its own. The unit image's
   `+0x00` would say whether a mover exists (`REC_FORMAT` 2). *Recommend (a)*: the mover lives
   exactly as long as its unit, and one image per slot keeps one shadow and one verify path. It
   costs about the same as (b).
6. **`+0x2A` stays a recorded value** (its only reader is the hover wave's ramp). *Recommend
   recording it*: it is exact and cheap to repack. Dropping it would loosen a value the takeover
   and the hover wave read.

---

## 6. Not established, and open

- **Not measured:**
  - the recipe itself (§2.3, §3.8);
  - the size estimate (§4);
  - the grid and sight equality the slot-order argument predicts.
- **`0x43DB50` on a carried `canhover` unit on layer 1** (a hovercraft frame on a hover plant's
  pad). The fix-up runs for `canhover` even with bit 16 clear, so the altitude `0x43DB50` saw was
  the attach point's, which the image does not carry. `+0x10A` may then differ from the
  recording [INFERRED; not seen in the data].
- **Carry and walk class in the same tick.** If a cargo's slot is below its carrier's, the
  recording's `0x43DA70` ran before the carrier's attach in that tick. The playback applies the
  carry first, so a class transition caused by the attach alone would come one tick early. It is
  rare [INFERRED: a unit stops before boarding]. The harness would name it.
- **The class object's own state** (path, goal) for S4. A takeover re-paths from the orders.
- **`0x49D000`** (a reader of mover `+0x20`): no reference found by rel32 and absolute scans. A
  pointer table computed at run time was not ruled out.
- **The names** in brackets in §1.3, and `0x47DB70` as "can occupy".
- **Frame 2016's type**, and whether 0.00 is the sea floor (§3.5).

## 7. Adjacent findings, outside this track

- **`+0x110` bits 26–27 depend on the viewing machine.** Whether the claim `0x47CC30` sets bit 26
  or bit 27 on the unit and on each occupant depends on the occupant owner's `+0x73 == 3`
  (`0x47CDD5`, `0x47CF0B`). A puppet host (all seats 3) therefore computes different values from
  the recording machine, as the probe's checkpoint 8 shows (MEASURED).
  - **For playback**, they must be written from the recording after all claims, or classified by
    the owner.
  - **In a stock network game** the peers disagree too [INFERRED: whether it reaches the
    simulation depends on the readers `0x47C7A0`, `0x47D29C`, `0x49B720`].
- **`+0x110` bit 13 is not only "set at completion".** It was set on every frame still under
  construction in the probe's AI game (MEASURED). Its writers `0x41BC32` and `0x41BCA4` sit in a
  function that writes the build fraction `+0x104` and the HP `+0x108` [INFERRED: the build
  progress step]. The engine map's row should be corrected.
- **`0x413264`**, an air order handler, reads a *target's* mover velocity with no NULL test. That
  is harmless on a puppet host, but after a takeover a local aircraft reads a puppet's recorded
  velocity, which is one more reason to write it (§1.6).
