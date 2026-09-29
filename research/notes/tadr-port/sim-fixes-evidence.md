# B. Simulation bug fixes — the evidence pass

A read-only pass made 2026-09-25 over every item of
[the merge exploration's §B](../tadr-merge-exploration.md#b-simulation-bug-fixes) and the rest of
TADR's `TABugFix.cpp`, to settle which items fix the stock 3.1 engine, what each one touches, and
whether TADR's fix holds by construction. It was made in four parts, one per area. The decisions it
fed are in [the plan](sim-fixes.md); each part records the owner's answer where it raised a
question. Sources: `pristine/TotalA.exe.pristine` (main checkout), disassembled with
`i686-w64-mingw32-objdump -d -M intel`; `vendor/TADR` at `dcff5dd`; the retail archives read through
`tools/hpipack.py` for stock-content counts (counts and names only; nothing extracted into the
repo). Every address was re-read in the disassembly; where TADR's number is quoted it was checked,
not copied. Tags: **DIS** = disassembled here, **SRC** = read in TADR's source or history,
**INF** = inferred, with the measurement that would settle it.

TA's network model ([networking-lobbies](../networking-lobbies.md)) is **state and event
replication, not lockstep**, and several of TADR's arguments assume lockstep ("clients disagree",
"insertion order identical on every client"). Those claims do not transfer, and the parts say so
where it matters.

## What the pass found

| item | kind | where it goes |
|---|---|---|
| area-damage victim caps: 20 units, 64 features (the feature cap is not in TADR) | stock defect, sim | [B1](sim-fixes.md#the-landings) · Part 2 §1 |
| flak's divide-by-zero `0x49CF19`, and the zero-velocity divide `0x49CE6A` | stock defect, sim (a crash) | B1 · Part 2 §4 |
| the off-map bucket's off-by-one `0x47CC85`/`0x47CCA1` | stock defect, sim | B1 · Part 2 §11a |
| the line-of-sight shear `0x465B6A` | stock defect, sim | B1 · Part 2 §11c |
| stacked aircraft invisible to splash | stock defect (a data-structure limit), sim | B2 · Part 2 §2 |
| unbounded unit indices in `0x09`, `0x0B`, `0x0C`, and in the `0x2C` receiver | stock defect, local receiver bound | B3 · Part 1 §3, §9 |
| a `0x0D` reaching a peer whose shooter has diverged | stock defect, receiver | B3 · Part 1 §8 |
| a stale hit killing the slot's next unit ("units exploding in factories") | stock defect, sim and wire | B4 · Part 1 §1 |
| the ghost commander (remote units created at (0,0) for N ticks) | stock defect, sim | B5 · Part 1 §2 |
| per-peer wind | stock defect, sim (economy) | B6 · Part 4 §6a |
| a structure's yardmap parsed past its terminator | stock defect, sim (load) | B6 · Part 3 §2b |
| the save loader's by-index fallback `0x43A58D` | stock defect, load | B6 · Part 3 §3 |
| the stockpile HUD divide `0x439D65`; a range circle of radius 1 `0x438EDE` | stock defect, UI (crashes, mod data) | B6 · Part 2 §5, §6 |
| resurrection's failure branch `0x40514F` | stock branch, trigger not found | measured, else parked · Part 3 §1 |
| the tracked unit's death leaving an order armed `0x4995EF` | INF | measured · Part 1 §5 |
| a departing host (`0x4656E5`); `+lostype` at normal level; the in-game `0x20` overwrite | policy / anti-abuse | group E · Part 1 §6, Part 4 §6b–c |
| TA's flat repair rate: an inverted clamp at `0x41BD87` | the game's rule | not in B · Part 3 §5 |
| grid-claim tie-break, anti-nuke circle, allied jamming, the off-map margin, aircraft wrecks falling | not defects, or gameplay changes | not in B · Part 2 |
| `ZeroDamageMapWeapons`, `TerrainFireGate` | new data keys | group C · Part 2 §9–10 |
| rotated-unit fixes, spawned-unit commands, ctrl-F/B, the long path, print-screen, `int 29`, `NewChatTextGuard`, "unit limit between missions", perm-LOS sonar | TADR fixing its own code | not in B · Parts 3–4 |
| `OrderDispatchGuard`, `MultiplayerPlayerLostGuard`, `UnitIDOutRange`, `GUIErrorLength`, the crash rings | diagnostics, dead or no-op | not in B · Parts 1, 3, 4 |
| the download-menu zeroing, the composite clamp | stock defects we already fix (A′2, L7) | done · Part 4 §1–2 |
| the cargo-detach "Option A/B" | **not** a defect; porting it would add one | not in B · Part 4 §2d |
| black model faces `0x45A2EC` | stock draw defect | moot on Vulkan; the GDI lane stays stock · Part 4 §5 |

---

## Part 1. Unit identity, death and the ID recycler

A read-only evidence pass made 2026-09-25 over TADR's unit-identity fixes, done before we plan how
to port them. Sources: `pristine/TotalA.exe.pristine`, disassembled with
`objdump -d -M intel`, and `vendor/TADR` at `dcff5dd`, which covers `src/DDraw/TABugFix.cpp`,
`TAbugfix.h`, `UnitIdentity.{h,cpp}`, `config.h` and the recorder's history at `4eda8a8^`.
Tags: **DIS** = disassembled here, **SRC** = read in TADR's source, **INF** = inferred, and each
INF names the measurement that would settle it. Every address below was re-read in the
disassembly; where TADR's number is quoted it has been checked, not copied.

### Summary

| # | item | real stock bug? | class | TADR's fix by construction? | recommendation |
|---|---|---|---|---|---|
| 1 | **FixFactoryExplosions**: an ID hold plus LRU/bump allocation (`0x486036`, `0x486DC1`; the `0x4854A0` Init is declared but not installed) | **Yes (DIS).** A `0x0B` damage record names its victim by slot alone. The receiver `0x489CE0` tests only that the slot is alive and not pending death, and the owner's allocator `0x485F50` reuses the lowest free slot at once. | **sim**, owner-local. The chosen index is replicated in `0x09`, so mixed builds do not desync. TADR's LRU changes slot order in every game, single player included, which **breaks rule 7**. | **No, it is timing.** It holds a slot for 150 GameTime ticks (5 s at speed 10, 2.5 s at 20). Its allocator state is keyed to `GameTime==0`, not to the unit array's lifetime. | **Escalate, reason 2.** A fix by construction needs the victim's incarnation on the wire, or an acknowledgement. If the owner approves a mitigation instead: allocate first-free with a quarantine, tie its lifetime to `0x4854A0`, network games only. Do not port the LRU. |
| 2 | **GhostComFix + GhostComFixAssist** (`0x4553F2`, `0x4954ED`) | **Yes (the mechanism is DIS; the trigger is INF).** A `0x2C` dirty entry that creates a unit into an empty slot (`0x48BA00`) uses that slot's own stale position, (0,0,0) for a never-used slot. The round-robin full state fixes it only after **N ticks**: 50 s at our 1500 a player, 16.7 s at 500. | **sim** on the receiving peer. The Assist broadcasts a **synthetic `0x2C`**, which makes it class B plus a wire change. | **No, it is a heuristic.** It covers the first entry only, relative slot 0 only, and assumes the land move class's serialisation. It is active only while t < N, and the assist runs at t = 90. The Assist's loop sends **slot 0 for every land unit** in the first 90 slots, a real type-morph bug. | **Measure the root cause first. Do not port the Assist.** The design depends on the cause: hold early `0x09`s if that is it, otherwise place a dirty-created unit from its own from-position, bounded to the map. |
| 3 | **NullUnitDeathVictim** (`0x4866E8`), and the same gap in `0x09` and `0x0B` | **Yes, a robustness gap.** A `0x0C` naming index 0 dereferences NULL at `0x486706`. An index past the array runs the destructor on foreign memory. `0x09` index 0 faults at `0x486237`. A high `0x0B` victim index is used out of bounds; for kind 0xA it writes HP at `0x489D80`. | **local** bound on the receiver; stock-exact for every well-formed message | **Yes for index 0**, which is a bound, but only for index 0 | **Port, widened into one "wire unit indices bounded" landing** covering `0x09`, `0x0B` and `0x0C`, beside our `0x0D` bound at `0x49D280` |
| 4 | **UnitIDOutRange** (`0x48A26B`) | Not TADR's. It has not been installed since at least 2014 and `MaxUnitID` is never computed (SRC). | n/a | n/a | **Do not port.** The site is recorded below: a weapon's unit target is held by slot (`0x48A1E0`). |
| 5 | **UnitDeath_BeforeUpdateUI** (`0x4995EF`) | **Possibly (INF).** When the tracked unit `main+0x37E9C` dies, stock pops the GUI but leaves the order byte `main+0x2CC3` armed. | **UI-local** | n/a (a UI reset) | Measure it. Port it as a UI fix only if a wrong order or a stale build cursor results. Low priority. |
| 6 | **HostDoesntLeave** (`0x4656E5`) | **Not a code defect.** Stock asks "You're out! Continue Watching?" unless local AIs are still alive (`0x457BC0`); TADR stops the host from being offered the choice. | **local UI / MP policy** | n/a | Measure what a departing host does to the other peers; the result goes to group E |
| 7 | **MultiplayerPlayerLostGuard** (`0x450380`) | **No.** The `Player_Ary[10]` read needs two identical searches, with no call between them, to disagree. That is unreachable (DIS). TADR's guard repeats the first search, so it never changes anything. | n/a | n/a (a no-op) | **Do not port** |
| 8 | **WEAPONFIRE_DISPATCH_FROM_SLOT** (`0x49D42A`, 191c901) | **Yes.** The code shape is DIS; the crash was seen in the field (SRC). The `0x0D` receiver branches on the packet weapon's flags but divides by the *local slot* weapon's velocity (`0x49CE6A`), so it raises #DE when the local copy of the shooter is another type. | **sim**, receiver-local, and only when the peers have diverged | **Yes**: branch and callee then read one object. But it creates a projectile the owner never fired. | **Port as a bound inside our `wpn_rx_fired` (`0x49D280`)**: drop the message when the slot weapon differs from the packet weapon. |
| 9 | **0x2C receiver bounds** (TDRAW_2C_ENTRY_BAILOUT, 191c901) | **Yes, a robustness gap.** The slot delta is sign-extended and unbounded (`0x48B985`). The type is not checked against `UNITINFOCount` (`0x48B9A8` then `0x48626F`). The round-robin remainder is signed (`0x48BAAB`). | **local** receiver bound; stock-exact for every well-formed packet | TADR ships it **observe-only** | **Port as bounds**: a bad entry stops the packet at `0x48BA28` and is logged. |
| 10 | **UnitIdentity audits and breadcrumbs** (191c901, plus digest msgId `0x31`) | n/a: diagnostics | diagnostics; the digest is a **new wire message** | n/a | **Do not port the wire audit.** Port local observe-only counters, and a cross-peer roster diff in `tacli`. |

**The three findings that matter most:**

1. **The factory explosion is a real stock defect of identity by slot, and TADR's fix is a timing
   mitigation.** No design without a wire change can make it safe by construction: the receiver
   cannot tell which incarnation of a slot a damage record meant. That is escalation reason 2.
   The two by-construction options each need a wire change: a victim incarnation carried in
   `0x09` and `0x0B`, or an acknowledgement. A **quarantine of two ticks or more** does close the
   *local* stale-slot readers by construction, because each reader re-validates every tick:
   the weapon target test at `0x48A295`, the tracked unit at `0x4995E4`, and our own
   `tagpu_lerp` pairing residual.
2. **Our 1500-unit limit triples two stock identity windows**, because the `0x2C` round-robin
   full-state sweep visits a player's block one slot per tick (`GameTime % main+0x37EE6`, DIS
   `0x48B827`, `0x48BAA3`). A remote unit's position and existence are fully re-asserted only
   every **N ticks: 50 s at 1500, against 16.7 s at 500.** The ghost commander (TADR's "first 50 s"
   is exactly N = 1500) and a remote ghost left by a lost `0x0C` both live that long.
3. **Several TADR items do not survive the check.** MultiplayerPlayerLostGuard guards a path that
   cannot be reached. UnitIDOutRange is dead code. The GhostComFixAssist loop hard-codes slot 0
   for every unit it reports, so any pre-placed land unit morphs the remote commander, which is
   the very divergence UnitIdentity was written to chase. The LRU allocator broke TADR's own
   ctrl-F/ctrl-B, fixed at `63d779d`: slot order is load-bearing, which is exactly rule 7's
   concern.

---

### 0. The model this cluster rests on (DIS)

**The unit array.** Its base is `main+0x14357` (`begin`) and its last element `main+0x1435B`.
The stride is `0x118` and there are `10·N+1` slots, where `N = main+0x37EE6` (a word). **Slot 0 is
a sentinel**. Each player owns a block at `PlayerStruct+0x67` (first) to `+0x6B` (last); its first
global index is at `+0x6F` (a word, read at `0x48B7A6`). The level load `0x4854A0` zeroes the whole
array and then writes every slot's `+0xA8` = its own index and `+0x92` = the default def
(`0x4855ED..0x485629`). A slot's **type is `+0xA6`**, and a type of 0 marks the slot free. Its
index is `+0xA8`. In its state word `+0x110`, bit `0x10000000` is *alive* and bit `0x4000` is
*pending death*. The engine map already has the measured block layout: idx 1, 501, 1001 at 500 a
player.

**Who allocates.** `UNITS_CreateUnit 0x485F50` (`stdcall`, 8 args, `ret 0x20`) takes the player
index as arg 1 and a *requested index* as arg 8. With arg 8 set to 0 it scans the player's block
**from the first slot for the first `+0xA6 == 0`** (`0x486006..0x486051`) and refuses a full
block (`0x486053`). With arg 8 set, it uses that slot or refuses it. Of its 11 callers
(`0x4028EA 0x403D5B 0x405104 0x41409B 0x41794F 0x4653D9 0x48718E 0x488462 0x488700 0x497002
0x4977BB`), only `0x48718E` passes a real arg 8. That caller reads it from a 0xB8-byte record in
`0x487080`, next to the string "Number of Units", which makes it the saved-game unit restore
[role INF]. Every successful create broadcasts **`0x09`** through `0x456050` (called at
`0x486115`). The record is 0x17 bytes: `{0x09, type +0xA6, index +0xA8, position +0x6A (12 bytes),
orientation +0x64 (6)}`. Our scenario loader calls `0x485F50` and `0x4864B0` too
(`tagpu_scenario.c`).

**Who frees.** Only the destructor `0x4866D0` releases a slot in play: `mov word [esi+0xA6],0`
at `0x486DC7`. The level load's memset is the only other clear. The one other store to `+0xA6`
in a unit, `0x485ED2`, *writes* a type, and its function `0x485E90` has no direct caller. The
destructor's two callers are `Send_UnitDeath 0x4864B0` at `0x486679`, in mode 1, and the
dispatcher's `0x0C` case at `0x455423`, in mode 0. `0x4864B0` broadcasts the 0x0B-byte **`0x0C`**
only when the owner's `+0x73` is 1 or 2, i.e. local (`0x486655..0x48666D`). It then destroys the
unit locally. **The owner is authoritative for death.** The per-unit tick reaps pending-death
units through `0x4864B0` (`0x48AFB9..0x48AFD1`).

**Who computes damage.** `0x489BB0` (12 callers) builds the 9-byte **`0x0B`**
`{0x0B, victim +0xA8, attacker +0xA8, damage u16, byte, kind}`. It applies the damage **locally
first** (`call 0x489CE0` at `0x489C89`). It sends the record only if the victim's owner is
**remote** (`+0x73 == 3`, `0x489C99`) and the kind is not `0xB`. **The attacker's peer computes
the hit; the victim's owner applies it and decides the death.**

**The dispatcher** jumps through `[code−2]·4 + 0x455F84` (`0x454861..0x45486D`). Its cases:

| code | case | handler |
|---|---|---|
| `0x09` | `0x4553DA` | `CreateFromNetwork 0x4861D0` |
| `0x0B` | `0x45540D` | `0x489CE0` |
| `0x0C` | `0x45541C` | `0x4866D0(rec, 0)` |
| `0x0D` | `0x45542D` | `0x49D270` |
| `0x2C` | `0x4553EE` | `0x48B920(player=edi, pkt=edx)` |

**The `0x2C` stream.** `Send_UnitStatAndMove_2C 0x48B710(player)` writes `[8] 0x2C, [16] size,
[32] GameTime`. Then comes a **dirty list**: for each alive unit with a move class whose
`[vt+0x1C]` answers true, in slot order, a `[16]` delta from the block's first index, a
`[typeBits]` type, and the move class's own payload (`[vt+0x20]`). The list is **capped at 0x200
bytes** (`0x48B7F6`). Then a `[16] 0xFFFF` and **one round-robin full-state entry for relative
slot `GameTime % N`** (`0x48B81B..0x48B8A4`, serialised by `0x48B200`). The receiver `0x48B920`
mirrors this. For a dirty entry whose type differs from its own slot, it calls
**`CreateFromNetwork` with a record built from its own slot's position** (`0x48B9B6..0x48BA00`).
It then parses the payload with that slot's move class (`[vt+0x24]`, `0x48BA05`). The round-robin
entry is taken by `0x48B3F0`. Type 0 with a local unit present sets **pending death**
(`0x48B426..0x48B42F`), which is the engine's own ghost sweep. A differing type creates the unit
(`0x48B497`) and **then writes the absolute position from the entry** (`0x48B5CA..0x48B698`). The
tick driver `0x495490` increments GameTime (`main+0x38A47`, `0x4954C0`), dispatches the network
(`0x453D40`), and then runs the units (`call 0x48AD30` at `0x4954ED`).

**`CreateFromNetwork 0x4861D0`** (`stdcall(player, rec)`, `ret 8`) takes its index from `rec+3`
**unbounded**: index 0 gives `esi = 0`, and `0x486237` then dereferences it. It does not require
the index to lie in the sender's block. If the slot is occupied it **kills the occupant first**
(`0x4864B0(esi, 0)` at `0x486244`) and then creates the new unit. It refuses only when the
player's block pointer `+0x67` is 0 (`0x486220..0x486234`).

---

### 1. FixFactoryExplosions: the unit-ID recycler

#### What TADR says, and what stock does

**TADR (SRC).** `TABugFix.cpp:441-625` says: "TA natively assigns the first available ID…
if that ID was used by a recently-deceased unit, we may still receive damage packets for it."
The line in `tdraw.txt` reads "Fix the 'units exploding in factories' bug by holding back
recycling of deceased units' IDs for 5 seconds". The history:

| commit | date | what |
|---|---|---|
| `b36a6bc` | 2024-02 | a pure FIFO over freed IDs, plus an Init hook at `0x4854A0` |
| `4e2dde3` | | back to lowest-free, with a 5 s gate |
| `d6066f5` | | the Init hook removed; the state is reset when `GameTime == 0` (issue #124) |
| `191c901` | 2026-09-09 | a **bump pointer over never-used slots, then a FIFO of freed slots gated at 150 ticks**, with the stated aim of maximising the time before a slot is reused, so that a stale `0x0D` lands on an empty slot |

The prior art is the Delphi recorder's `.fixfacexps` (SRC `idplay.pas:1639`, still at HEAD and in
both shipped recorder binaries, off by default). It dropped each *incoming* `0x0B` that named a unit index this peer had sent a `0x0C` for
within 3000 ms of wall-clock time.

**Stock (DIS)** is §0 above. B's peer hits A's unit X in slot s and sends `0x0B(s)`. Meanwhile
A has killed X, broadcast `0x0C(s)`, and its factory's next `0x485F50` has taken **s, the lowest
free slot**, for the new nanoframe Y. When B's `0x0B` arrives, `0x489CE0` resolves s to Y. It
tests only the alive bit (`0x489D45`) and pending death (`0x489D50`), and applies the damage to Y,
whose HP is still low. **A is the owner, so A kills Y authoritatively and broadcasts `0x0C`**:
every peer sees the unit in the factory explode. The same stale record also reaches every third
peer C, since `0x0B` is broadcast. C applies it to its own copy of s. If that kills Y locally, C
destroys its copy without broadcasting (the owner is not local). A's round-robin puts Y back on C
within N ticks, through the `0x48B497` recreate.

- **A second, local path to the same symptom (DIS shape, INF reach).** A weapon's unit target is
  held **by slot**: `(index, 0x8000)` at slot `+4`, read in `0x48A1E0`. The only liveness test is
  `+0xA6 != 0` (`0x48A295`): a slot re-occupied before the shooter's next check keeps the shooter
  aimed at the new occupant, which is a unit of the same enemy player. That can be a nanoframe in
  that player's factory, with no network involved. **INF:** whether the reuse can precede the
  check inside one tick depends on the order of the block walk relative to the destructor.
  Settle it with a single-player skirmish logging each `0x48A1E0` that finds a target whose
  creation tick is later than the tick the target was acquired.
- **The ghost variant.** If A's `0x0C(s)` is lost, B keeps a live ghost of X and keeps sending
  `0x0B(s)` until A's round-robin advertises s as empty (`0x48B42C`) and B's reaper runs. That can
  take **up to N ticks** (see §2), far longer than TADR's 150.

#### Is it a real stock defect?

**Yes.** Identity by slot is carried on the wire with no incarnation, and the allocator reuses a
slot in the same tick it is freed. Both are DIS. It needs a network game with latency, and the
nanoframe is the visible victim because a unit starts its build at low HP.

#### Class, and rule 7

**The simulation.** Only the owner allocates for its own units (**INF**: `0x485F50` is called only
for local players. Settle it by logging arg 1 of every `0x485F50` call on each peer of a
two-peer game: every player must be local, `+0x73` of 1 or 2). The chosen index is replicated in
`0x09`, so **peers stay consistent whatever allocator each runs**. The allocator protects only the
units of the peers that run it. Under rule 1 everyone does, and rule 4 forbids a runtime opt-out.

**Rule 7: TADR's version changes stock below the bug in every game.** The bump pointer and LRU hand
out different slots even in single player, where no stale `0x0B` exists: `0x489C89` applies damage
locally at once. The unit tick walks blocks in slot order (`0x48AD30`), and so do many searches,
so outcomes differ from stock. **TADR's own tree shows the order is load-bearing**: `63d779d`
fixed ctrl-F and ctrl-B "skipping your most recently built factories once you had lost any units".
Its `ExternQuickKey` had scanned up to the live count, assuming low-slot packing, and the bump
allocator put new units above it.

#### TADR's safety argument, re-checked

- **The hold is timing, not construction.** It is `RECYCLE_MARGIN_TIME = 5 * 30` GameTime ticks.
  That is 5 s at speed 10 and **2.5 s at speed 20**. The argument is "a stale `0x0B` arrives
  within 150 ticks". TAF's tunnel deliberately buffers up to 5 s of a silent peer's traffic and
  replays it "so damage packets are never lost" (`networking-lobbies.md` §5). That is exactly the
  hold. A lost `0x0C` ghost can emit stale damage for up to N ticks. The bump pointer and the LRU
  make reuse *rarer*, with 1500 fresh slots a player before any reuse, but rarer is what CLAUDE.md
  says is not a fix.
- **The lifetime is not tied to the array.** The state resets when `GameTime == 0` on every hook
  call (`UnitIdFreeList_SyncSize`). The unit array lives from `0x4854A0` (allocate) to `0x485980`
  (free). A game whose first create happens at GameTime > 0 inherits the previous game's
  `bumpNext` and FIFO: a loaded saved game, **INF** whether it restores GameTime. Its entries'
  `freedAt` can then exceed the current GameTime, so `GameTime - freedAt < 150` holds and
  **nothing can be built** until the clock passes them. The original `b36a6bc` hooked `0x4854A0`,
  which was the right lifetime.
- **Recycle-site correctness holds (DIS).** Every in-play release passes `0x486DC1`, the
  destructor's tail, reached only for an alive unit (`0x486706`), so "one push per free" holds.
  The teardown `0x485980` also passes it for every unit. `CreateFromNetwork` can occupy a slot
  behind the allocator's back, but only through a `0x09` or `0x2C` naming *our* block, which an
  honest peer never sends. TADR re-checks `UnitID == 0` on every hand-out, which is correct.
- **Stock's identity checks on the receiver are all one-shot**: alive, and not pending death. No
  receiver re-derives which unit a message meant.

#### What a fix by construction needs (escalation reason 2)

The receiver cannot tell incarnations apart, because `0x0B` carries none. A **type check is not
enough**: a factory usually rebuilds the same type. The by-construction options:

- **(a) An incarnation on the wire.** Class B plus a wire change, in the same-build spirit as
  A′3's `0x0D` widening. The owner bumps a per-slot incarnation at create. The incarnation must
  travel in `0x09` so that peers learn it, and `0x0B` must carry the victim's incarnation, which
  the receiver compares. **Where the bits could go:** at 10×1500+1 = 15 001 slots, the u16 index
  in `0x09` (`rec+3`) and in `0x0B` (`rec+1`) has **2 spare high bits** (15 001 < 16 384). That
  gives only a mod-4 incarnation, which wraps and so is not strictly by construction. A full
  counter needs a companion message, as A′3's `0x0E` did, or a size change.
- **(b) An acknowledged quarantine.** Class B plus a new message. The owner keeps slot s empty
  until every remote peer has acknowledged processing `0x0C(s)`. This is by construction **only
  if** B's `0x0B`s and its acknowledgement arrive at A in the order B sent them. **INF:** TA's
  ordering and reliability for these subpackets, meaning whether `0x451DF0` sends them
  guaranteed and whether the TAF tunnel reorders. Settle it by disassembling `0x451DF0` and
  `0x44FFD0` for the DirectPlay send flags, and measuring the sequence of subpackets per peer
  pair under loss.
- **(c) A timing quarantine** (TADR-like), **only with the owner's approval.** It must be
  documented as a mitigation, with the residual hole named: any `0x0B` delayed by more than H
  ticks, including the tunnel's 5 s buffer, and ghosts from a lost `0x0C` (up to N ticks).

**Part of it *is* by construction, locally.** Hold a freed slot for **at least 2 ticks** under any
of the options. Then every per-tick reader of slot identity sees the slot empty at least once
before reuse: the weapon target clear at `0x48A295`, the tracked-unit check at `0x4995E4`, and
the renderer's lerp pairing, which `tagpu_lerp.c` names as its one residual ("a unit that died
and one that took its id within a single tick could be paired"). **INF:** that each runs every
tick for every holder. Verify it per reader before citing it.

#### Overlap

- `tagpu_reclaim` hooks `FreeObjectState 0x45AAA0` inside the same destructor (`0x486D9E`). A
  recycle hook at `0x486DC1` sits after it and does not conflict.
- **Our raised limits:** N = 1500 means 15 001 slots and 1500 fresh slots a player. **TADR's bump
  pointer therefore postpones any reuse until a player has created 1500 units.** At stock's 500 it
  was 500. Stock's lowest-free exposure does not change with N. The ghost-sweep window grows
  with N (§2).
- **The saved-game restore** (the requested-index path from `0x487080`) must stay exact. Our
  saved-game fixes (`fix_saved_features_border`, `fix_restore_record_owner`) are on the feature
  side and do not touch it.
- `tagpu_lerp`'s residual and `TAGPU_PK` slot keying both benefit from any quarantine.

#### Proposed our-design

The owner chose (a) with the two-tick hold (decided below). The allocator part starts with
**one detour at `0x486036`**. That instruction is `cmp word [esi+0xA6],0`, 8 bytes long, and is
entered from `0x486009` and by fall-through from `0x486030`. **With arg 8 set, the detour keeps
stock's rule. With arg 8 = 0, it scans first-free as stock does, but skips a slot freed less than
two ticks ago.** It returns to `0x48605D` (use `esi`) or `0x486053` (none). **One detour at
`0x486DC1`** (6 bytes, `mov ebp,[esi+0x110]`; the target of `0x486DAD`'s `je`) stamps the slot as
freed. The state is a DLL static `u32 freed_at[TAGPU_PK_DESIGN_SLOTS]`, **reset at `0x4854A0`**:
its lifetime is the array's, never keyed to GameTime. The hold applies in every game, so single
player differs from stock only on a same-tick reuse. The incarnation is a second static of the same
shape, bumped at create and carried on the wire (the plan's B4). **Do not port the bump pointer or the LRU.**

#### How to reproduce and test

- **The defect, deterministically.** The race window on loopback is one or two ticks, so add a
  test-only arm lever on the attacker's peer, e.g. `tagpu_dmgdelay.on=K`. It holds each outgoing
  `0x0B` in the `0x489CB9` send path for K ticks. That is latency for exactly the message at
  issue, with no `tc netem`: the desktop and the network belong to the owner. Then:

  ```
  tacli launch h1 --dplay --free-dplay-port
  tacli launch j1 --dplay
  mp_lobby.sh --map 'Two Continents' h1 j1
  ```

  Set MultiCommanderDeath to 0 (the recipe is in `references/modules.md`). Apply the two halves
  of the fixture:

  - `scenario apply h1 …`: a Kbot lab with a long queue of Peewees, and a line of Peewees in
    front of j1's guns
  - `scenario apply j1 …`: three LLTs in range of the line

  The oracle is a DLL counter on h1, "a received `0x0B` applied to a unit created less than K
  ticks ago", with `tacli roster h1 --json` for units that died with build < 100 % inside the
  lab's footprint. Expect the count to be above 0 with stock's allocator at K = 10.
- **The fix.** At K below the hold, the count is 0. At K above the hold, it is above 0 again:
  **that run is the proof of the residual hole**, and it belongs in the note. At the same seed
  without the lever, compare the per-peer rosters (paused, keyed by `engine_index`) before and
  after the change.
- **Rule 7.** Compare a single-player skirmish against the previous build (the parent commit's DLL
  built in a scratch checkout, `--keep-dll`; B's fixes are in both `make` builds): the creation
  indices must be identical except where a slot freed within the last two ticks would have been
  taken, which is the proof that single player stays first-free apart from the hold.

#### Decided 2026-09-25

The fix is (a): a per-slot incarnation carried with `0x09` and `0x0B` in a companion message, and a hold of at least two ticks on every freed slot in every game, single player included ([the plan, B4](sim-fixes.md#the-landings)).

---

### 2. GhostComFix and GhostComFixAssist

#### What TADR says, and what stock does

**TADR (SRC).** The `tdraw.txt` line reads: "remote commanders appear in top left of map during
first 50sec of game".

- **`GhostComFix`** sits at `0x4553F2`, which is the `push edx; push edi; call 0x48B920` of the
  dispatcher's `0x2C` case (DIS). It parses **the first dirty entry** of each `0x2C` from a
  non-local, non-watcher player while `GameTime < N`. If that entry is relative slot 0, the
  commander, and its 3 move bits have bit 4 set, it writes `fromX` and `fromY` into
  `player->Units[0].XPos` and `YPos` (`+0x6C` and `+0x74`, the integer halves of `+0x6A` and
  `+0x72`) **before** stock parses the packet.
- **`GhostComFixAssist`** sits at `0x4954ED`, the tick driver's `call 0x48AD30`. At
  `GameTime == 90` it broadcasts a **synthetic `0x2C`** for each local player: a move from the
  unit's position to its order target.

**Stock (DIS).** The receiver `0x48B920`, finding a dirty entry for a slot whose type differs,
builds the create record from **its own slot's `+0x6A` and `+0x64`** (`0x48B9C2..0x48B9FB`) and
calls `CreateFromNetwork`. The load zeroed a never-used slot, so the unit is created at **(0,0,0),
the top-left**. The move payload is then parsed by that unit's move class. **Only the
round-robin full-state entry carries an absolute position** (`0x48B5CA..0x48B698`). That entry
reaches relative slot r when `GameTime % N == r` (`0x48BAA3`). For the commander, r = 0, so the
correction lands at t = N. A dead slot is not cleared of its position by the destructor's tail
(`0x486D75..0x486E59`). So a dirty-path create into a *reused* slot uses the last occupant's
death position. **INF:** the earlier part of `0x4866D0` was not read for position stores.

**Why the commander's `0x09` does not create it (INF).** Every local create sends `0x09` with the
position (`0x456050`). `CreateFromNetwork` drops it silently when the receiver's block pointer is
0 (`0x48622B`). The dispatcher also gates each code on a state table at `0x512BC0`
(`0x454758..0x4547A7`, not decoded). The likely cause is that the commander's `0x09` is sent at
game start (`0x4653D9`) while a peer still drops it. **Settle it:** on the joiner, log each
`0x4861D0` call with its return address (`0x4553E9` for `0x09`, `0x48BA05` for a dirty entry,
`0x48B49C` for the round robin) and the host commander's first appearance.

#### Is it a real stock defect?

**Yes: the dirty-path create at an unknown position is DIS.** The window is **N ticks**, the
round-robin period. At our 1500 a player that is **50 s** at speed 10, the "50 s" TADR describes.
At stock's 500 it is 16.7 s. **Our raised limit tripled it.** It affects the simulation, not just
the picture: the receiver's own units target the ghost at (0,0), and the receiver computes and
sends damage for hits on it (§0: the attacker's peer computes the hit).

#### Class

`GhostComFix` writes the **receiver's** copy of a remote unit's position. That is simulation on
that peer; the owner stays authoritative. **`GhostComFixAssist` puts a synthetic `0x2C` on the
wire, which every peer, with or without the fix, applies as a real move**. That makes it class B
plus a wire change, and it changes every game's traffic at t = 90 even when no ghost exists
(rule 7).

#### TADR's safety argument, re-checked

- **It is a heuristic, and partly timing.** It reads the first entry only, and only relative slot
  0. It assumes the land move class's serialisation (3 bits, then `[16]×4`) for whatever class
  the entry belongs to; an air commander, or a mod's, would misparse into a garbage position.
  The positions are 16-bit but **not bounded to the map**. It is active only while t < N, and the
  assist runs at "t = 90 (3 secs)".
- **A real bug in the Assist (SRC, `TABugFix.cpp:382-431`).** It loops over every `iUnit < min(N,
  GameTime)` with a Land move class. For each it writes **relative index 0** ("unit index
  (commander=0)") but that unit's *own* type and position. With any land unit besides the
  commander in the first 90 slots at t = 90, the receiver's slot 0 gets a different type. The
  receiver then kills its commander copy through `0x4864B0` and **creates that unit in the
  commander's slot** (`0x48B9AD` → `CreateFromNetwork`). That is a type morph on every peer. It
  happens with pre-placed units: TADR's map spawns, or our `scenario apply` at start.

#### Overlap

None hooked. The engine map does not yet document `0x48B920`, `0x48B3F0` or `0x4861D0`; they are
listed at the end. `raised-limits.md` leaves open whether the raise stretches a remote unit's
update interval (its tier 2 measured lags of up to 40 ticks at 1500). **The round-robin half of
that question is answered here, by DIS:** a slot's full state recurs every N ticks.

#### Proposed our-design

**Measure the cause first.** If the start-of-game `0x09` is dropped because the block does not yet
exist, the by-construction fix is an **ordering**: keep `0x09` records that arrive before the
sender's block exists, and apply them when `0x4854A0` has set it. Nothing is guessed: the owner's
own position arrives. If the cause is elsewhere, the dirty-path create at `0x48BA00` into an
**empty** slot should take its position from the entry's own payload where the move class
carries one, **bounded to the map** (`main`'s map size), and only for the move classes whose
serialisation we have disassembled. **Do not port the Assist in any form.**

#### How to reproduce and test

```
tacli launch h1 --dplay --free-dplay-port; tacli launch j1 --dplay
mp_lobby.sh --map 'Two Continents' h1 j1
```

Then, every 2 s for 60 s, run `tacli roster h1 --json` and `tacli roster j1 --json`. Find the
host commander's `engine_index` (its relative slot is 0: `main+0x1B63 + p·0x14B + 0x6F`, read with
`tacli peek`). Compare j1's position for it with h1's. Expect j1 to show about (0,0) until about
t = N. Repeat with the host's MAXUNITS slider at 500 and expect the window to shrink to about
16.7 s. Add the create-path log above for the cause. For a regression check after the fix, the
same run must show j1 matching h1 from the first sample.

#### Decided 2026-09-25

Measure the cause first. If it is the dropped `0x09`, the receiver keeps records that arrive before the sender's block exists and applies them once `0x4854A0` has set it ([the plan, B5](sim-fixes.md#the-landings)). The Assist is not ported in any form.

---

### 3. NullUnitDeathVictim, and the unbounded indices of `0x09`, `0x0B` and `0x0C`

#### What TADR says, and what stock does

**TADR (SRC).** It writes 6 bytes at `0x4866E8`, `75 04 33 F6 EB 18` → `0F 84 6B 07 00 00`. This
is a raw `SingleHook` with no expected-bytes compare. **Checked (DIS):** the 6 bytes are exactly
`jne 0x4866EE; xor esi,esi; jmp 0x486706`, and the new `je` goes to `0x4866EE + 0x76B =
0x486E59`, the destructor's epilogue. For index 0 it skips to the exit.

**Stock (DIS).** In `0x4866D0`, `rec+1` is 0, so `esi` = 0, and `test [esi+0x110]` at `0x486706`
dereferences NULL. **Any index past the array** makes `esi` point beyond `main+0x1435B`: the
alive test reads foreign memory, and if the bit reads set, the whole destructor runs on it,
freeing whatever pointers it finds. The killer at `rec+7` is also unbounded (`0x486753..0x486778`;
0 becomes NULL, which is handled). **The same gap exists in:**

- **`0x09`.** `CreateFromNetwork` faults on index 0 at `0x486237`, and is unbounded above.
- **`0x0B` (`0x489CE0`).** The victim index is 0-guarded but unbounded above, and a heal-kind
  record writes HP at `0x489D80`. The attacker at `rec+3` is unbounded above.

Locally none of these can be 0: `0x4864B0` writes the live unit's own `+0xA8`, which is at least
1. **Only a malformed or foreign message reaches them.** Our `0x0D` fix already bounds the
shooter and target against the last element (`wpn_rx_fired`, `0x49D280`).

#### Real defect, class, and rule 7

These are **real robustness defects** (DIS), reachable only from the wire. The fix is a **local**
bound. **It is stock-exact for every well-formed message**, so rule 7 holds.

#### TADR's argument

**A bound, by construction, but only for index 0.** It leaves indices past the array open.

#### Proposed our-design

One landing, "**wire unit indices bounded**", in the `fix_weapon_ids` idiom: the sites are
checked against stock's bytes, fail closed, and a drop is logged with a budget. It covers:

| message | site | the rule |
|---|---|---|
| `0x0C` | `0x4866E0..0x486705` | index 0, or past `(last−first)/0x118`, goes to `0x486E59`; the killer is bounded or made NULL |
| `0x0B` | `0x489CED` | the victim and the attacker are bounded the same way; the drop goes to `0x489F93` |
| `0x09` | `0x4861F7` | the index is bounded, and **optionally** required to lie in the sender's block (`[player+0x67] ≤ slot ≤ [player+0x6B]`) |

The block rule is **INF**: that AI players' creates also arrive from the AI's own seat. Settle it
with a network game with an AI hosted on h1, logging the sender seat against the block of every
`0x09` on j1. Apply the block rule only after that. Count the drops in the `enginefix:` line.

#### How to reproduce and test

The drops cannot be reached honestly, so add a test-only lever, `tagpu_wirefuzz.on`. On the game
thread it feeds crafted `0x09`, `0x0B` and `0x0C` records straight to their handlers: index 0, the
last slot, the last slot + 1, and `0xFFFF`. It runs in a single instance, with no MP needed.
Without the fix, index 0 of `0x0C` or `0x09` faults, the target TADR measured in the wild. With
the fix, each is a logged drop and the game ticks on (`peek *0x511DE8+0x38A47:4` twice). The
regression check is a two-peer fight like the weapon-ID fixture's, and every drop counter must
read 0.

---

### 4. UnitIDOutRange (`0x48A26B`)

**TADR (SRC).** The proc jumps to `0x48A270` (return 0) when `(eax & 0xFFFF) > MaxUnitID`. **Its
install has been commented out since at least the 2014 SVN import** (`de5129b` deletes the
commented lines). `MaxUnitID` is set to 0 and never computed; the commented formula
`ActualUnitLimit*10` would also have been one short of `10N`.

**Stock (DIS).** `0x48A26B` is in `0x48A1E0`, whose one caller is `0x49E1E1`. The function reads a
weapon slot's target at `slot+4` (`[slot+6] == 0x8000` means a unit). It **sign-extends** the
index (`movsx` at `0x48A27C`). That is fine up to 32 767, and 15 001 is inside it. The index comes
from engine state, not the wire. The finding that matters here is §1's: **a weapon target is
identity by slot** (`0x48A295` tests only the type).

**Recommendation: do not port.** It is dead in TADR, and the reachable concern is §1's.

---

### 5. UnitDeath_BeforeUpdateUI (`0x4995EF`)

**TADR (SRC).** It sets `PrepareOrder_Type` (`main+0x2CC3`) to STOP (1) whenever it is not
already 1. **Stock (DIS).** An in-game per-frame function reads the **tracked unit** at
`main+0x37E9C` (`0x4995C3..0x4995F1`; the function's role is INF, and it calls
`GameFrame_InGame 0x496790` at `0x4995B8`). That is a u16 index, the one the engine map lists at
`0x48CC58`, and it is the unit whose menu is up [INF]. If the index is nonzero and its slot is
**empty**, the function calls `UpdateIngameGUI 0x491D70(0)`. That call clears `0x37E9C`
(`0x491DA5`) and pops the GUI stack back to the game screen; under some UI flags (`+0x37EBE`,
`0x2BEE`) it defers instead (`0x491D97`). **`0x491D70` never writes the order byte `0x2CC3`.** It
stays, for example, 0xE (build) with `BuildUnitID` `0x2CC4`. **INF:** that no other path resets
it.

**Real defect? INF.** The likely symptom is a build cursor, and **our build ghost** (it draws while
`0x2CC3 == 0xE`), left armed for a builder that is gone. A click then issues the pending order to
whatever is selected. There is also a stale-slot hazard: if the slot is **re-occupied in the same
tick**, `0x4995E4` sees a type and neither pops nor resets, so the "tracked unit" silently becomes
the newcomer. That is identity by slot again, and a ≥2-tick quarantine (§1) closes it.

**Class:** UI-local. **By construction:** a UI state reset needs no invariant argument.

**Test (single instance).** Select a constructor, open its build menu and choose a building:
`peek *0x511DE8+0x2CC3:1` reads 0xE. Kill the constructor with a scenario kill
(`UNITS_KillUnit`). Then read `0x2CC3` and `0x37E9C`, click the map, and check what order the
remaining selection holds and whether the ghost still draws. Port only if that shows a wrong order
or a stale ghost; the fix is then a detour on the 7 bytes at `0x4995EF` (`push 0`, then the
relative `call`, which must be re-encoded).

**Measured 2026-09-26** (`tools/b6-tracked-death.sh`): no wrong order, and a stale ghost. After the
death `0x37E9C` read 0 and the build menu was popped, while `0x2CC3` stayed `0x0E` and `0x2CC4`
246. The engine's placement square and our ghost kept drawing, and every left press went to the
placement (`0x4993B6` → `0x498F70`): both clicks landed on blocked sites, so the map got no
building and the CORAK was not selected. A click on a clear site would have handed the build to
the selection (`0x498F93..0x498FC0`). The owner ruled it a defect to fix; it is B9, which disarms
the placement, and any command mode, whenever no unit is left that the click would order, checked in the frame
(`0x49697B`) and in the in-play handler's head (`0x499226`) ([the plan's B9](sim-fixes.md)).

---

### 6. HostDoesntLeave, i.e. PutDeadHostInWatchMode (`0x4656E5`)

**TADR (SRC).** If the local player is the host (player number 1), it prints "You're out! You are
placed in watch mode because you're hosting…" and jumps to `0x465508`.

**Stock (DIS).** In a network game (`0x435100` returns 3), a player is marked out
(`[record+0x9B] |= 0x40` at `0x46569D`). When that player is the one whose view this is
(`main+0x2A42`), stock continues silently if `0x457BC0` finds local AI players still in play.
That function counts seats whose `+0x73` is 2 and that still have units (roughly: it tests
`+0x146` and `+0x144`), which keeps a machine hosting AIs from leaving. **Otherwise it
opens `YESORNO.GUI` with "You're out!  Continue Watching?"** (strings at `0x503168` and
`0x507318`, opened at `0x4656FC`); "No" leaves the game. TADR removes the choice for the host.

**Real defect? Not in this code.** It is a policy. The concern behind it is what a departing host
does to the others. **INF:** host migration exists as `0x18`, but whether the game survives the
host leaving is unmeasured. **Class:** local UI; no simulation effect of its own.

**Test.** Three peers, h1, j1 and j2. Kill h1's commander with MultiCommanderDeath at 0. On h1,
answer No: `tacli ui h1 click CHOICE2`. Then read GameTime twice on j1 and j2. If both keep
ticking and agree when paused (by roster), the stock choice is harmless and there is nothing to
port. If they stop, a departing host is a real network defect. Forcing the host to stay is then a
workaround, not a fix, and the owner decides.

---

### 7. MultiplayerPlayerLostGuard (`0x450380`)

**TADR (SRC, `TABugFix.cpp:2268-2315`).** Its claim: "reads one slot PAST the end of
Player_Ary… the first result is guarded (index 10 → NULL); the second is not", which prints a
junk chat line. The guard suppresses the call when no seat has that DPID.

**Stock (DIS).** `0x450380(dpid)` (`ret 4`) is called only from the destructor, when an owner's
live count reaches 0 in a network game (`0x486E32`). **Search 1** (`0x4503BF..0x45040B`) walks
the ten seats for `+0x73 != 0 && +0x4 == dpid`. If it misses, it sets `al=10`, then `esi=0`, and
**prints nothing** (`0x45040D..0x450415`). If it hits, **search 2** (`0x45041C..0x450466`) runs
the *identical* loop over the same memory, with **no call between the two**. Only a miss in
search 2 selects seat 10 (`0x45046E`, then `0x450488`). **So the out-of-bounds read needs the two
searches to disagree, which a single thread cannot produce.** TADR's guard is search 1 again, so
it cannot change what stock does. The only way to reach the read is a concurrent writer to the
player table. **INF:** whether any other thread writes `+0x4` or `+0x73` in play. Settle it with a
hardware watchpoint on one seat's `+0x73` during a network game, checking the writer's thread.
TADR's junk line more likely came from its own re-entrancy, which its `NewChatTextGuard` comment
describes: "our own packet handlers send, re-entering TA's network stack".

**Recommendation: do not port.** For the engine map: the phrase comes from CRT `rand() & 7`
(`0x4E4870`), not the simulation RNG.

---

### 8. WEAPONFIRE_DISPATCH_FROM_SLOT (`0x49D42A`)

**TADR (SRC, `UnitIdentity.cpp:239-316`, `config.h:160-179`).** The fix makes the `0x0D` receiver
choose the projectile branch from the shooter's own slot weapon rather than from the packet's
weapon. It is on by default and compile-time only.

**Stock (DIS).**

- `0x49D270` checks the shooter's alive bit (`0x49D354`), so a dead or empty shooter is dropped.
- It takes the slot `+4 + pkt[0x23]·0x1C` (`0x49D366..0x49D378`) and writes two packet words
  into it, at `+0x18` and `+0x16` (`0x49D37C`, `0x49D384`).
- At `0x49D42A` it branches on **`[packetWeapon+0x111]`**, the packet weapon's flags.
- The ballistic callee `0x49CDE0` divides by **`[[slot+0xC]+0x68]`** (`0x49CE62..0x49CE6A`), the
  local slot weapon's velocity.
- The local fire path reads its flags from `[esi+0xC]` (`0x49D742..0x49D74B`), the same object it
  then uses, so it cannot mismatch.

When the local copy of the shooter is **a live unit of another type**, the slot's weapon can be
the all-zero entry, and #DE results (SRC: game 190441, `INT_DIVIDE_BY_ZERO` at `0x49CE6A`).

**Real defect: yes.** A consistent packet crashes a diverged peer. **Class:** simulation on the
receiver, and only when diverged. For consistent peers the slot weapon *is* the packet weapon,
because the sender fills the packet from its slot at `0x49D742`, so stock stays exact.
**TADR's fix is by construction** (one object is read for both the choice and its use), but it
fires the *local* weapon, a projectile the owner never fired.

**Overlap: our `wpn_rx_fired` (`tagpu_patches.c`, at `0x49D280`).** It already bounds the shooter
and target indices and the slot byte, and it knows the full weapon ID after A′3.

**Our design.** In `wpn_rx_fired`, after the index bounds, resolve the shooter's slot weapon
pointer, through the extra-weapons module's accessor when the slot is past 2. **Drop the message
through `0x49D55D` when it differs from `&Weapons[id]`**, and count the drops. That is a bound on
engine plus wire data, and it never fires between consistent peers. **Test:** the §3 wirefuzz
lever feeds a `0x0D` whose shooter slot holds a different weapon: stock faults with #DE, the fix
logs a drop. The regression check is the two-peer weapon-ID fixture (`tools/weaponids_fixture.py`)
with the drop counter at 0.

**Decided 2026-09-25.** Drop the message and count it ([the plan, B3](sim-fixes.md#the-landings)).

---

### 9. The `0x2C` receiver's bounds (TDRAW_2C_ENTRY_BAILOUT)

**TADR (SRC, `UnitIdentity.cpp:319-384`).** It validates each dirty entry at `0x48B9AD` and
records a `2CBD` breadcrumb. **The bailout to `0x48BA28` is compiled off.** The reason given: its
field crash reports show 13 faults at `0x48BA07` (a NULL move class) and 5 at `0x48B9AD`, where
the wild pointers lie beyond what a 16-bit delta can reach, "so a guard… would hide the fault".

**Stock (DIS).**

- The delta is `movsx` of `[16]`, relative to `[player+0x67]` (`0x48B985..0x48B99A`), with no
  bound. A delta of −1 for the first block lands on slot 0, whose `+0xA8` is 0, and
  `CreateFromNetwork` then dereferences NULL at `0x486237`.
- The type is `[typeBits]` with no check against `UNITINFOCount`. `CreateFromNetwork` indexes the
  def table `main+0x1439B + type·0x249` (`0x48626F..0x48627F`).
- A type whose def has no move class (`def+0x22F != 1`, `0x4862CF`) leaves `[esi]` at 0, and
  `0x48BA05..0x48BA10` then dereferences it. That is TADR's 13 faults at `0x48BA07`, reachable by
  a misframed stream: the sender only lists units with a move class (`0x48B784`).
- The round-robin slot is the **signed** `idiv` of a wire GameTime by N (`0x48BAA3..0x48BAAB`), so
  a negative GameTime gives a negative remainder.

**Real defect: yes, a robustness gap** that misframed or foreign streams reach. **Class:** a local
receiver bound, and **stock-exact for every well-formed packet**: an honest delta is in [0, N),
an honest type in [1, count), and it has a move class.

**TADR's worry, answered.** A bound on the *delta* cannot explain a wild `[player+0x67]`. So check
that too, by construction: the player's first slot must equal `begin + (1 + k·N)·0x118` for its
block k, and lie inside `[first, last]`.

**Our design.** Detours at `0x48B985` and `0x48B9AD` check the delta, the type and the move class.
The round-robin remainder is taken unsigned. On any failure the parser **stops at `0x48BA28`**,
the engine's own end-of-list, and logs a line that names the field that failed.

**Test.** The wirefuzz lever feeds crafted `0x2C`s: delta −1, delta N, a type equal to the count,
a building type in a dirty entry, a negative GameTime. The regression check is a ten-peer tier 2
run (`limits-tier2-p*`) with the drop counter at 0.

**Decided 2026-09-25.** The parser stops. Past a field that fails, the bitstream cannot be re-framed, so nothing after it could be parsed anyway ([the plan, B3](sim-fixes.md#the-landings)).

---

### 10. UnitIdentity's audits and breadcrumbs (191c901)

**SRC.**

- **MORF**: `CreateFromNetwork` onto an occupied slot whose type changes, hooked at `0x4861D0`,
  with its three callers named by return address: `0x4553E9`, `0x48BA05` and `0x48B49C`, all
  verified by DIS.
- **GHST**: the engine's own ghost detector at `0x48B426`.
- **WPNX**: §8's mismatch.
- **SYNC**: every 150 ticks, walk each block against its live count (`+0x144`, the count
  `0x486187` increments and `0x486DFC` decrements), and **broadcast an FNV digest over CHAT_05
  hijack msgId `0x31`**.

**Class:** diagnostics. The digest is a new wire message. **Recommendation:** do not port the wire
audit; TADR's own text calls it diagnostic only. Instead, add three **observe-only DLL counters**,
morph, recreate and ghost, printed on the heartbeat. They are cheap, local, and they are the
oracle for §1, §2 and §8. The cross-peer comparison belongs in the harness: `tacli roster --json`
on every paused peer, diffed by `engine_index` and type, as `references/modules.md` already
describes by hand.

---

### Adjacent findings (not TADR items)

- **The `0x2C` dirty list stops at 0x200 bytes a tick, filled in slot order from the block's
  start** (`0x48B7F6`). **INF:** if `[vt+0x1C]` does not clear a unit's dirty state when it is
  skipped, high slots of a busy block starve until the round robin, up to N ticks. At 1500 a
  player, many movers can overflow 512 bytes. This bears on `raised-limits.md`'s open
  "does the raise stretch the update interval" question, and on any allocator that pushes new
  units to high slots, as TADR's bump pointer does. Settle it on a tier 2 run by logging, per
  tick, the last slot the sender reached.
- **The player blocks are ordered by DPID in a network game** (`0x485676` compares seat `+4`
  while ordering the ten records). **INF** that the block assignment follows that order; it is
  consistent with every peer agreeing on `engine_index` (tier 2).

### Engine facts this pass established (for `exe-reverse-engineering.md`)

These are all DIS except where marked. The dispatcher's jump table is `0x455F84`, indexed by code
− 2. The functions:

| address | what |
|---|---|
| `0x4861D0` | `CreateFromNetwork`: its index is unbounded, it kills an occupant, it refuses on a null block |
| `0x456050` | the `0x09` sender |
| `0x489BB0` | the damage function and `0x0B` sender |
| `0x489CE0` | the `0x0B` receiver |
| `0x48B710` | the `0x2C` sender (the 0x200 cap, the round-robin `GameTime % N`) |
| `0x48B920` | the `0x2C` receiver |
| `0x48B3F0` | the round-robin receiver and the ghost sweep |
| `0x4995C3..0x4995F1` | the tracked unit's death check |
| `0x450380` | the player-lost announcer (two identical searches) |
| `0x4656E5` | the defeat dialog, with `0x457BC0` counting local AIs |
| `0x48A1E0` | the weapon slot's unit target, held by slot |
| `0x487080` | the saved-game unit restore [role INF], the only caller that passes arg 8 to `0x485F50` |
| `0x49D42A` / `0x49CE6A` | the `0x0D` branch and the divide |
| `0x4855ED` | the per-slot `+0xA8` and `+0x92` initialisation |

### What this pass did not establish

- **Why the commander's `0x09` is missed at game start** (§2): the `0x512BC0` gate table was not
  decoded.
- **Whether TA's transport keeps the order of each peer pair's subpackets**, which option (b) of
  §1 needs.
- **That `0x485F50` is only ever called for local players** (§1's class).
- **Whether a loaded saved game starts at GameTime > 0** (the §1 lifetime hazard in TADR's
  design).
- **Whether the local weapon-target reuse (§1) can happen inside one tick.**
- **The dirty-list starvation** (adjacent findings).

None of these was measured: this pass only read.

---

## Part 2. Combat, damage and targeting

A read-only pass made 2026-09-25 over the combat, damage and targeting items in TADR's §B (and the
two escalation-only rules next to them), to settle what each one touches before any plan. The
sources are `pristine/TotalA.exe.pristine` (main checkout), disassembled with
`i686-w64-mingw32-objdump -d -M intel` (the whole-image listing, plus `--start-address` re-runs
where the linear sweep desynchronised: `0x49C740`, `0x43903A`, `0x4670xx`, `0x4675A0`), and
`vendor/TADR` at `dcff5dd`. The stock-content counts come from the retail archives, read locally
through `tools/hpipack.py` (counts and names only). Tags: **DIS** = disassembled here, **SRC** =
read in TADR's source, **INF** = inferred, with the measurement that would settle it.

"Class B" below means *changes the simulation, so every multiplayer peer must run it* (standing
rule 1). **Who computes damage matters for every item here.** `0x499EB0` applies a projectile's
damage (`0x499CD0` for a direct hit, area damage `0x49A120` otherwise) only when the projectile's
owner is local ([limits-evidence §8](limits-evidence.md)), and `AutoAim` runs only for units that a
local human or AI owns ([extra-weapons](../extra-weapons.md)). So damage and firing are decided on
the firer's peer and replicated as events. A mixed build therefore does not desync the state. It
applies different rules to different players' shots, which is still exactly what the same-build
contract exists to prevent.

### Summary

| # | item | real stock bug? | class | TADR's fix by construction? | recommendation |
|---|---|---|---|---|---|
| 1 | area-damage victim dedup cap: 20 units (`0x49A28F`), and **64 features** (`0x49A5FA`, not in TADR) | **yes** (DIS). Victim 21 onward is hit once for every cell it stands on. Reachable with stock content (commander blast AOE 950, nukes 512, fusion 516, building deaths 325–420) | B | partly. The bound is sound, but nesting is handled by `>=`, which trades a double hit for a missed hit. That is harmless only because stock nests after the walk (DIS) | **port** (both caps), exact below 20 / 64 |
| 2 | AreaDamageOverflow: aircraft stacked on one cell are invisible to splash | yes, a data-structure limit: each cell has one air slot, so non-holders are never victims | B | **no.** The index is rebuilt once per main-loop `GameTime` change and used a step later. The unit tick and the network pump run in between and can free a unit, and the provider does not re-check liveness at use (timing) | port **our design** (live candidate list, validated at use, served after the stock walk), air only |
| 3 | GridClaimTieBreak: "evict the incumbent if it belongs to a remote human" | **no defect shown.** Peers disagree about who holds a contested cell, but under state replication the firer's peer decides every hit. TADR's own header calls the harm INFERRED | B, **changes stock single-player behaviour** (first claimer → lowest index) | deterministic, but it fixes a lockstep concern this engine does not have | **do not port** |
| 4 | ballistic divide-by-zero `0x49CF19` (and its sibling `0x49CE6A`) | **yes**, a crash: burnblow+ballistic shot with pitch within ±0.35° of vertical. Stock flak (`ARMFLAK_GUN`, `CORFLAK_GUN`, `ARMYORK_GUN`, `CORSENT_GUN`) is exactly that kind of weapon | B (projectile lifetime; the receiver path crashes too) | **TADR has no fix**, only crash-handler logging. Its analysis is half wrong (see §4) | **port**: divisor bound at the point of use |
| 5 | BuildWeaponSlotGuard: HUD `idiv` `0x439D65`; order slot index `0x402B7F` | HUD: yes, a crash on a zero reload divisor (not reachable with stock data; reachable through identity divergence or a mod). Slot index ≥ 3: unchecked read/write (INF reachable) | HUD: UI/local. Sim bail-out: B | HUD divisor check: yes (a bound). Wrapped in SEH `SafeIsBadReadPtr` probes, which are not the argument. Sim bail-out frees every order | **port the HUD bound**; the sim bound only with our extra-weapons slot count |
| 6 | CircleRadius `0x438EDE` `jl`→`jle` | yes, a crash: range circle of radius 1 → `idiv 0` at `0x438EEE` (not in stock content) | UI, local | yes (a bound), but N = 0 then draws the label at screen (0, 4) | **port**, skipping the whole circle, label included |
| 7 | anti-nuke circular search `0x49D120` (+ minimap ring `0x4670C4`) | **no.** Stock's square test is consistent. The in-game ring is a circle of radius `coverage`, the minimap ring a circle of `coverage − 512` | B (balance) | n/a (shape change). Its entry `jmp` collides with our extra-weapons detour at `0x49D120` and it refuses slots ≥ 3 | **do not port**: not a defect |
| 8 | JammingOwnRadar `0x467608` | **no.** A design choice: a jammer not owned by the view player jams its radar, allies' included | **B**, not UI: the bits it changes feed the radar-targeting list `0x40AA40` | n/a | **not in B**: not a defect |
| 9 | ZeroDamageMapWeapons | no. A new TDF key (`nomapweaponalert`) | B for flagged weapons only | n/a | group C |
| 10 | TerrainFireGate | no. New TDF keys (`notoverwater` / `notoverland`) | B for flagged weapons only | n/a | group C (hooks inside `AutoAim` — overlaps extra-weapons) |
| 11a | off-map bucket off-by-one (`0x47CC8B`, `0x47CCA3`: `X+fw >= W`) | **yes** (DIS). A footprint that ends on the last column or row is parked off-map, so an aircraft there takes no splash and no direct hits (bottom and right edges only) | B | TADR works around it with the margin, not at the test | **port**: `jge`→`jg` at both sites (the clear path keys on the bucket, DIS) |
| 11b | OffMapAircraft margin (aircraft beyond the edge) | no. By design, off-map units are untouchable | B, **a gameplay change** (the width is a balance knob: 1 in mainline, 32 in Escalation) | walks a sanity-capped list (4096) and hard-codes the stock 300-projectile pool; the splash is re-implemented in C floats | **not in B** (a gameplay change); if ever wanted, serve off-map aircraft through #2's candidate list (engine math, one dedup) |
| 11c | LOS shear `0x465B6A..0x465B93` | **yes** (DIS). The row is `(z − alt/2) >> 5` under an unsigned bound, so an airborne unit near the north edge is invisible to everyone | B (acquisition) | clamp: yes (a bound); scoped to airborne units | **port**, as a clamp exact whenever stock's row is in bounds |
| 12 | AirCorpseFall | stock asymmetry: land wrecks get no velocity, so they are never integrated. **Unreachable through aircraft with stock data** (none of 30 stock flyers has `Corpse=`); maybe through cargo (INF) | wreck record Y; mostly visual | trivial and bounded | measure the cargo path; a floating wreck is raised as a visual residual |

**The three findings that matter most:**

1. **The dedup caps are a real, reachable stock damage multiplier, and TADR fixes only half of
   them.** Victim 21 onward of one explosion is damaged once for every cell of its footprint
   (up to 9× for a 3×3 structure). Features have the same bug at 64 (`0x49A5FA`). Commander blasts
   and nukes reach both. Fixing them is exact below the caps.
2. **TADR's AreaDamageOverflow is not safe by construction.** Its per-cell index is built from the
   main loop once per `GameTime` value. In `0x495490` the network pump and the unit tick run before
   the projectile tick uses it. `UNITS_Destroy 0x4866D0` can free a unit in either place. Its
   provider then hands a freed slot back to the engine's victim code without re-checking it. Our
   design must validate at the point of use, and can be exact where TADR is approximate.
3. **The `0x49CF19` crash is live in stock content, and TADR only logs it.** Flak is
   ballistic+burnblow, and its flight time divides by `v·cos(pitch)`, which is exactly 0 in two
   ±0.35° bands (pitch 90° and 270°). TADR's "case A" (`weaponvelocity=0`) crashes earlier, at
   `0x49CE6A`, and its "bit 23 = ballistic" is actually burnblow.

---

### 1. The area-damage victim dedup caps (20 units, 64 features)

**What TADR says (SRC, `AreaDamageOverflow.h`).** Vanilla keeps a 20-entry per-explosion victim
list. Past 20 it "stops recording but keeps damaging", so unit 21+ takes the blast once per cell.
TADR fixes this under `AREA_DAMAGE_OVERFLOW_FIX_DEDUP_CAP 1` as a side effect of its overflow
module. It is on in every config that enables the module (ProTA, Escalation, Mayhem, Twilight) and
absent from OTA, BTA and TA Zero, where the module is off.

**What stock does (DIS), `0x49A120`.** The callers are exactly two: `0x49A0A9` inside
`0x499EB0` (= `+0x1F9`) and `0x49A109` inside `0x49A0C0`. `0x49A0C0`'s one caller is `0x423BE0`,
in the fire spread `0x4239C0`.

- The radius is `(u16)w[+0xD6] >> 1` (`0x49A149..0x49A150`). The cell rect is centre ± (r/16 + 1),
  clamped to `[0, W)`×`[0, H)` with `W = main+0x14233`, `H = main+0x14237` (`0x49A1A7`,
  `0x49A1BD`). Each cell comes from `0x481550(x,z)` (`0x49A1EB`); a NULL cell is skipped
  (`0x49A208`).
- The unit loop runs selector `[esp+0x10]` = 0, 1 (bound `cmp eax,1` at `0x49A41A`). Selector 0
  reads slot A `[cell+0]`, selector 1 slot B `[cell+2]` (`0x49A214..0x49A22F`). The unit is
  `[main+0x14357] + idx·0x118` (`0x49A231..0x49A24B`). The attacker `proj+0x52` is skipped
  (`0x49A259`).
- **The list:** the count is `[esp+0x98]` (zeroed at `0x49A12F`), and 20 unit pointers sit at
  `[esp+0xA0]`. The linear search is `0x49A262..0x49A28B`. **It records only while
  `count < 20` (`0x49A28F cmp ecx,0x14 / jge 0x49A2AA`), and `0x49A2AA` is the damage block
  itself.** So a victim found after the 20th is never recorded, and every later cell of its
  footprint finds it again and damages it again.
- Per victim: distance from the blast point to the unit's def box (`+0x15E..+0x172` about
  `+0x6A/+0x6E/+0x72`), in x87, taking the high word (`0x49A35A..0x49A39E`). At or beyond the
  radius it skips. Otherwise edge falloff with `w+0xD8` (`0x49A3B6..0x49A3E0`), then
  `0x499CD0(proj, unit, factor)` (`0x49A3F5`). Friendly and enemy totals are kept by `proj+0x66`
  against `unit+0xFF` (`0x49A3FA..0x49A40D`) and handed to `0x406F50` at the end (`0x49A837`).
- **Features, the same bug at 64 (not in TADR).** Features are skipped only when `w+0x111`
  bit 14 (unitsonly) is set (`0x49A432`). A `0xFFFE` cell resolves to its anchor
  (`0x49A443..0x49A474`), and cells at `0xFFFB` and above are skipped (`0x49A480`). The list is
  64 anchor-cell pointers at `[esp+0xF0]` with the count at `[esp+0x9C]`: search
  `0x49A5CE..0x49A5F4`, record only while `count < 64` (`0x49A5FA cmp esi,0x40 / jge 0x49A615`),
  and **`0x4244B0` is called regardless (`0x49A626`)**. In a network game `0x4244B0` damages the
  feature on the host and sends `0x0F` from every other peer (the engine map, *Who sends a feature
  hit*), so feature 65 onward is damaged, or reported, once per cell.
- **Nesting is reachable, but only after the walk.** The interceptor tail runs when `w+0x111`
  bit 30 is set (`0x49A66F`). It calls `0x499EB0(proj,0)` for every live projectile inside the
  AOE (`0x49A764`), which re-enters `0x49A120` through `0x49A0A9` whenever that projectile's owner
  is local (single player against an AI, for instance). The cell walk ends at `0x49A65E`, before
  the tail. No path from the per-victim or per-feature calls back into `0x49A120` was found: the
  only other caller, `0x49A0C0`, is reached from the fire spread alone.

**Real stock defect: yes.** Stock AOE (READ, 574 weapon sections): COMMANDER_BLAST 950,
CRAWL_BLAST 556, ATOMIC_BLAST 516, NUCLEAR_MISSILE / CRBLMSSL / ARMEMP_WEAPON 512, building
deaths 325–420. A 950 blast covers a rect of about 60×60 cells, so more than 20 victims (and a
forest's more than 64 features) is ordinary play. Which victims are affected is arbitrary: they
are ordered by the row-major cell walk, not by distance.

**Class B.** Rule 7 holds by construction. At or below 20 victims / 64 features the victim set,
the order of first hits and every damage value are stock's; only the repeat hits disappear. It is
a balance change only in the sense that stock's accident goes: blasts on dense bases get weaker
against victim 21 onward.

**TADR's safety argument, re-checked.** The dedup is a per-unit generation table sized to the
live unit array and bounds-checked (`index < g_hitGenCount`) — a bound, good. For nesting it
saves and restores the generation and uses `>=`. That turns "an outer blast re-hits a unit the
inner one hit" into "an outer blast misses a unit only the inner one hit". Neither is exact. It is
harmless today only because stock nests after the walk (above), which TADR asserts is unreachable
without having checked the interceptor tail. **Keyed by `UnitInGameIndex` (`+0xA8`)**, which is
fine.

**Overlap.** A′3 splices the `0x0E` sender's ID reads at `0x49A78C`/`0x49A7CD` in the same function
(`tagpu_patches.c:2162`). Different sites, but the new module must share the byte-check table. No
other hook of ours touches `0x49A120`.

**Our design.**
- Wrap the two call sites `0x49A0A9` and `0x49A109`. Each call gets **its own** seen-set: a unit
  bitset over `[0, unit count)`, with the index `(esi − base)/0x118` bounded by the array's count,
  plus a feature set keyed by anchor-cell ordinal, bounded by `W·H`. Hold them in a
  depth-indexed stack whose depth is saved and restored around the call. Nested calls then cannot
  share or clobber a set, by construction and not by the "nesting only happens after the walk"
  argument. (A bitset for units is about 1.9 KB at 15 001 slots. For features, a small
  per-call vector grown from the heap, because a per-map bitset would be 128 KB per call on
  1024².)
- Replace the list blocks `0x49A262..0x49A2A9` (72 bytes) and `0x49A5CE..0x49A614` with calls
  that answer "seen, skip" or "record, continue". Stock keeps doing the damage math.
- **Invariant:** one explosion damages a unit at most once and reports a feature at most once;
  every index is bounded before it is used.

**How to test.**
- Compare the previous build's DLL (the parent commit, built in a scratch checkout) with the fixed
  one, each through `tacli --keep-dll`; there is no runtime opt-out (standing rule 4).
- The scenario: 30+ identical 2×2 or 3×3 structures on one ring about a blast point, at the
  distance where the blast's falloff leaves them alive, and a commander (`blast` order via
  `tacli order --unit N --expect ARMCOM blast`) or a nuke at the centre. Owner of the structures:
  an idle AI.
- Read each structure's HP (i16 at `unit+0x108`): `tacli peek t1 '*0x511DE8+0x14357:4'` gives
  the array base, then `tacli peek t1 '0x<base + idx·0x118 + 0x108>:2'`.
- Prediction (DIS): stock gives equal ring-mates equal loss for the first 20 visited in row order,
  and a whole-number multiple (their cells inside the rect) for the rest. The fixed arm gives
  equal loss to all.
- For features, the same with 70+ multi-cell wrecks (the `one-wreck` / `feat-forest` scenarios
  are the starting point), read through the wreck records.

**Decided 2026-09-25.** Both caps are fixed ([the plan, B1](sim-fixes.md#the-landings)).

---

### 2. AreaDamageOverflow: aircraft stacked on one cell

**What TADR says (SRC).** A cell has two unit slots. Stack aircraft on one cell and the grid names
one of them, so the rest are "structurally invisible" to splash. It was measured on Escalation's
CORMUAT: 0 of ~20 missile impacts. The fix splices the slot dispatch `0x49A214` (29 bytes), widens
the selector bound `0x49A41C` from 1 to 7, and serves selectors 2..7 from a DLL index. The index
is rebuilt per `GameTime` change from `GameTickHook` (`0x4969CB`, in the main loop) and holds six
air units per cell. It is air-only, excludes cargo and off-map units, and is class B.

**What stock does (DIS).**
- The stamp is `0x47CC30` (the tick's call is `0x43DA41`, right after the clear `0x47D0E0` at
  `0x43DA0F`). Its off-map test (`0x47CC57..0x47CCA3`) parks a unit in the bucket
  `main+0x142B7`. When `+0x110` bit 29 is set, the yardmap path writes slot A. Otherwise
  `(mask & 3) == 1` claims **slot A** (`0x47CEBC..`) and `== 2` claims **slot B**
  (`0x47CF98..`). **Any other value stamps nothing** (`0x47CF9B`) — TADR's "slot B = everything
  else" is slightly wrong.
- A contested slot is decided by the rule in §3. The loser gets `+0x110` bit 27, the winner
  bit 26.
- Area damage reads only the two slots (§1). Direct-fire collision `0x49B090` (one caller,
  `0x49BD88`) also reads only slot A (`0x49B1C7..0x49B213`) and slot B (`0x49B222..0x49B275`). So
  a non-holder takes neither splash nor direct hits in that cell. A direct shot hits the holder
  instead, which is at least a hit on something; splash that skips the rest is the defect.

**Real stock defect: yes**, a data-structure limit rather than a rule anyone chose. Stock flyers
have 2×2 or 3×3 footprints (READ, 30 flyers), and hovering VTOLs and transports sent to one point
overlap. Whether stock aircraft *fully* lose every footprint cell as readily as Escalation's
CORMUAT is **INF**. Settle it by peeking slot B of each cell of each unit's footprint
(`*(main+0x14287) + (z·W + x)·0x0D + 2`) with 10 ARMBRAWL / ARMATLAS ordered to one position.

**Class B**, and it changes who is hit. That is the point of it, but it is still a gameplay change
(stacked air starts taking flak splash). Rule 7: stock's victims, their order and their damage
must be untouched. TADR serves overflow units **interleaved** in the walk, at the first cell
where they appear. That keeps stock victims' values, but interleaves new `0x499CD0` calls between
stock's. If damage application draws the sim RNG or emits packets in order, the order changes
(INF; `0x499CD0` → `0x489BB0` builds the `0x0B` packet).

**TADR's safety argument, re-checked — it does not hold by construction.**
- **Staleness is structural.** In `0x495490` the order is: `GameTime++` (`0x4954C0`), the pump
  `0x453D40` (`0x4954C8`), units `0x48AD30` (`0x4954ED`), projectiles `0x49B720` (`0x495513`),
  then players `0x464F80` and features `0x424050`. TADR builds after a step and uses during the
  next one, so the pump and the unit tick run in between. The pump also runs from the frame
  callback (`0x4968CB`, limits-evidence §8), and a `0x0E` it receives can run area damage outside
  the step.
- **A freed unit comes back.** `UNITS_Destroy 0x4866D0` is reached through `Send_UnitDeath
  0x4864B0` and from the network dispatcher (`0x455423`, a `0x0C` in the pump). `0x4864B0`'s
  callers are `0x4859A8`, `0x486244`, `0x486EB2`, `0x486EF8`, `0x486FB4` and `0x48AFD1`; the last
  lies after the unit tick's entry `0x48AD30` (INF: that it is inside its body). It frees the
  object, clears the alive bit and resets `+0x92` to a default def (`0x486DF6`, engine map). TADR's
  provider checks aliveness only when it builds. At use it hands the freed slot to the engine,
  which measures the dead unit's box and calls `0x499CD0` on it. The same holds for a slot
  recycled by a new unit.
- The bounds, by contrast, are real: the cell ordinal against the indexed map, the map pointer
  compared, the index against the array's count.
- The six-slot residual is **measured** by TADR (26 Hawks on one cell: 5 million saturation
  events).
- Also: the `>=` nesting trade (§1), and a cross-module "disjointness invariant" with
  OffMapAircraft that is enforced by a skip in one file.

**Overlap.** §1's dedup is a prerequisite, so this module must land on it or with it.
`tagpu_packet_pub.c` draws units; it does not read the grid.

**Our design (exact where TADR is approximate).**
1. **Build the candidates at the explosion, from live state.** In the wrapper (§1), walk a list of
   airborne units: `mask & 3 == 2`, not cargo (`+0x86 == 0`), not in the off-map bucket (unless
   §11b), alive, and with `+0x9E` non-NULL (the engine map's correct death guard). The list is
   rebuilt at the entry of `0x49B720` each step, which is only a candidate pool: every member is
   **re-validated in the wrapper**. Keep those whose live footprint (`+0x76/+0x78`, `+0x7E`)
   intersects the blast rect.
2. **Exclude any candidate the stock slots will return.** A candidate that holds slot B of any of
   its footprint cells inside the rect is left to stock.
3. **Serve the rest after stock has finished.** Splice the selector bound (`0x49A415..0x49A426`)
   so the **last** cell of the walk loops `2 + n` times, not 2. Selector ≥ 2 there returns
   candidate *k*. Stock's victims, their order and their values are then byte-identical, and the
   new victims go through the engine's own per-victim code — no new float anywhere. There is no
   fixed per-cell capacity, so TADR's six-per-cell residual goes too.
4. **Invariant:** every unit handed to the engine was validated alive in this call, and every
   index was bounded first.

**How to test.**
- Stack 10 ARMATLAS (hover) with `tacli order t1 --sel move pos X Z`. Confirm the stack by the
  cell peeks above.
- An AI side with CORFLAK / CORRL in range (`shootall`), read HP per unit as in §1.
- Stock: only the slot-B holders lose HP. Fixed arm: all members inside the blasts do.
- Then the same with a 200-aircraft dogfight (`scenarios/air-war.json`) for cost; the frame packet
  timing lines give the budget.
- Two peers (`tools/mp_lobby.sh`, the `limits-mp-*` fixtures): paused, both hold the same HP.

**Decided 2026-09-25.** (a) Yes, with our design. (b) Air only. Stock aircraft losing their cells is measured before anything is built ([the plan, B2](sim-fixes.md#the-landings)).

---

### 3. GridClaimTieBreak

**What TADR says (SRC).** The rule "evict the incumbent if it belongs to a remote human" is
client-relative, so peers disagree about who holds a contested cell. TADR replaces it with "lower
`UnitInGameIndex` wins" at six sites. It admits that the harm is INFERRED ("damage is applied
through a single net-broadcast choke point").

**What stock does (DIS).** All six 17-byte sites match TADR's bytes. In `0x47C790` (re-claim,
gated on bit 27 at `0x47C7A0`) they are `0x47C847`, `0x47C950`, `0x47CA2C`. In `0x47CC30` (stamp)
they are `0x47CDCA`, `0x47CF00`, `0x47CFDA`. The rule is: incumbent's player `+0x96` active and
its type `+0x73 == 3` → evict (claimant gets the cell and bit 26, incumbent bit 27); otherwise
keep. So on each peer local units win contests against remote ones, and in single player the first
claimer holds. The callers of `0x47CC30` are the motion relink (`0x43DA41`) and `0x48610F`,
`0x48630C`, `0x48AA9A`, `0x48B6B9`; the callers of `0x47C790` are `0x47DAFD` and `0x47ED35` (DIS,
every `call` in the image).

**Real stock defect: not shown.** TA replicates state and events ([networking-lobbies](../networking-lobbies.md)).
The grid is per-peer derived state. The only sim decisions read from it — area damage, direct
collision (`0x49B090` → `0x499EB0`, which damages only for a local owner), landing — are made on
the owning or firing peer and then replicated. Peer disagreement therefore changes nothing that
peers must agree on. The rule looks deliberate for a replicated engine: prefer the unit whose
position this peer actually simulates.

**Class B, and it changes stock behaviour in single player** (first claimer → lowest index) and on
every owning peer. That breaks rule 7 with no defect underneath. TADR's "side benefit" (no
dereference of the incumbent's player) is moot: a destroyed unit's `+0x96` is left intact
(`0x4866D0` resets `+0x92`, not `+0x96`).

**Recommendation: do not port.** If the owner wants it measured anyway: two peers, one aircraft
from each player on one cell, peek slot B on both peers (they will disagree), then fire and compare
HP on both peers (they will agree).

---

### 4. The ballistic divide-by-zero `0x49CF19` (and `0x49CE6A`)

**What TADR says (SRC, `TABugFix.cpp:1973`).** "UNITS_FireProjectile_Ballistic" divides by
`EBP = TurnZLookup(elevAngle)·muzzleVelocity`. It is zero in case A (`weaponvelocity=0`) or case B
(elevation 90°). The proposed fix is to jump to `0x49CF29` — but **no hook exists**: the only code
is a crash-handler branch that writes `Errorlog.txt` and returns `EXCEPTION_CONTINUE_SEARCH`.
TADR's changelog: "Diagnostic logging for crashes 004cbed5 and 0049cf19". The older
`UnitVolumeYequZero` hook at `0x49CE65` is declared and set to NULL, never installed.

**What stock does (DIS), `0x49CDE0`.**
- Callers: `0x49D0F8` in the fire dispatch `0x49D0C0`, taken when `w+0x111` bit 1 (**ballistic**)
  is set and bits 0 (lineofsight) and 20 (selfprop) are clear; `0x49D44E` (the `0x0D` receiver
  path); and `0x49D76E`.
- It **appends the projectile first** (`0x49CDE9..0x49CE20`, the `0x12C` cap), then initialises
  it through `0x49C740` with **no target position** (arg 4 = 0).
- **`0x49CE65..0x49CE6A`: `div ecx` with `ecx = w+0x68`, `weaponvelocity × 2184.53`** (the loader
  at `0x42E4C6`, the double at `0x4FD240` = 65536/30), for **every** ballistic shot. That is
  TADR's case A: it faults here, never at `0x49CF19`.
- `ebp = TurnZLookup 0x4B7123(pitch = slot+0x18, v)` = `v·cos` from the 512-entry table `0x509F00`
  (8192 = 1.0, `+0x1000 >> 13` rounding). The heading `slot+0x16` then splits it into
  `proj+0x1C`/`+0x24`.
- **`0x49CECC`: bit 23 is `burnblow`**, not "ballistic" as TADR's log text says. With burnblow,
  flight time = `hypot(dx,dz)` (`0x4FB440`, ftol) `/ ebp` at `0x49CF19`. Without it, flight time =
  `w+0xE6` = `weapontimer × 30` (loader `0x42E5CA..0x42E5D8`; path `0x49CF29..0x49CF31`). The
  expiry is `proj+0x46 = GameTime + t` (`0x49CF42`).
- **`ebp == 0` exactly when the table entry is 0**: entry 256 or 0 (read: both are 0; entries 255
  and 1 are 101), i.e. **pitch ∈ [0x3FE0, 0x405F] or [0xBFE0, 0xC05F]**, 90° or 270° ± 0.35°. For a
  nonzero entry the product rounds to 0 only if `v < 41` raw, far below any stock velocity.

**Real stock defect: yes, reachable with stock content.** READ: twelve ballistic+burnblow sections,
all flak — `ARMFLAK_GUN`/`CORFLAK_GUN` (v 950) and `ARMYORK_GUN`/`CORSENT_GUN` (v 900), in
`rev31.gp3`, `ccdata.ccx` and `btdata.ccx` — and all anti-air (`toairweapon=1`, `weapontimer=1`).
An aircraft almost exactly overhead of a flak gun asks for a pitch near 90°. (INF: that the
trajectory solver returns a pitch inside the band for such a target. TADR's handler implies it was
seen in production.) No stock ballistic weapon has velocity 0 (READ), so `0x49CE6A` is mod-only.
Because the `0x0D` receiver calls the same function, **every peer that receives the shot crashes
too** (INF: the order of send and fault).

**Class B** (projectile lifetime). Rule 7 holds trivially: the fix changes only inputs on which
stock faults.

**TADR's argument:** none to re-check, since there is no fix. Its analysis names the wrong fault
for case A and the wrong flag.

**Overlap.** Extra-weapons splices the same function at `0x49CF65` (`fp0.name`) and `0x49CF8D`
(`fp0.heading`) (`tagpu_weapons.c:1422,1447`). The new site is clear of both, but the two modules
must share the byte check.

**Our design.**
- Replace `0x49CF18..0x49CF20` (`cdq; idiv ebp; mov edx,[0x511DE8]`: 9 bytes; no branch lands
  inside — the only nearby targets are `0x49CF29` and `0x49CF3E`) with a `jmp` to a stub:
  `if (ebp == 0) { eax = [esi+0xC]; jmp 0x49CF29 }` — the engine's own non-burnblow rule, flight
  time = `weapontimer`. Otherwise `cdq; idiv ebp; mov edx,[0x511DE8]; jmp 0x49CF21`.
- For `0x49CE6A`, a load-time bound in the weapon loader: a ballistic weapon with `w+0x68 == 0`
  gets 1, logged. That is the identity for all stock data.
- **Invariant:** no divide in this function sees a zero divisor.

**How to test.**
- An AI CORFLAK and a human ARMATLAS ordered to hover at the flak's exact world position
  (`tacli order t1 --unit N --expect ARMATLAS move pos <flak x> <flak z>`), `shootall` on.
- Peek the flak's slot pitch `unit + 0x04 + 0x18` (u16) while it fires. Stock: `tacli crash t1`
  names `0x49CF19` (INF until run). Fixed arm: the shells expire at `weapontimer` and a counter in
  the enginefix line counts fallbacks.

**Decided 2026-09-25.** `weapontimer`, the engine's own rule ([the plan, B1](sim-fixes.md#the-landings)).

---

### 5. BuildWeaponSlotGuard: the stockpile HUD divide and the order's slot index

**What TADR says (SRC).** Five production crashes were at `0x439D65` (`idiv esi`, esi = 0), all
dividing by `&Weapons[0]`: the "no weapon" sentinel, whose `reloadtime` is 0 forever. The cause,
per PR #26, is unit-identity divergence: this peer's copy of a remote unit has the wrong type, so
a legitimate order names a slot that is unarmed here. There are three pieces:
- Fix 1: clamp a stockpile weapon's `+0xE4` from 0 to 1 at load.
- Fix 2a: bound order `+0x36` in the sim `0x402B70`, bailing out through `0x402BA4`'s `return 7`
  (which frees every order).
- Fix 2b: bound it in the HUD, with a divisor check, returning 0.

Only Escalation enables it.

**What stock does (DIS).**
- `0x439D20` has one caller, `0x46B446`. It finds the first order in `[unit+0x60]` with `+0x42`
  bit 19 (`0x80000`), reads the slot index `+0x36` and progress `+0x3E`, the weapon
  `[unit + 0x10 + idx·0x1C]` (`0x439D51`) and `+0xE4` (`reloadtime × 30` as a word, loader
  `0x42E54B..0x42E561`), and computes `progress·100 / reload` with an integer `idiv`
  (`0x439D65`). **No bound on idx, and no zero test.**
- `0x402B70` (the order table's `'Nanolathing'` row, `0x4FC575`) loads the weapon for the same
  slot unconditionally (`0x402B89`). In state 2 it increments `[unit + idx·0x1C + 0x1E]` — the
  slot's `cStock` for idx ≤ 2, the position for idx 3 — and calls `0x41C150` (`0x402BB3..0x402BC3`).
  Its divides by `+0xE4` are x87 (`0x402BF3..0x402C4C`) and survive 0.

**Real stock defect.**
- HUD: a crash, yes. **Not reachable with stock data** (READ: every stock stockpile weapon has
  `reloadtime` 120–180). It is reachable through an identity divergence (TADR's evidence is
  Escalation's), or through a mod with `stockpile=1` and no `reloadtime`.
- idx ≥ 3: an unchecked read and write, INF reachable (who writes `+0x36` for this order is not
  traced here).

**Class.**
- HUD: UI, local. Returning 0 for a zero divisor changes no state.
- Sim bail-out: B, and not harmless (`return 7` frees every order).

**TADR's argument.** The divisor check at the point of use is a bound, correct by construction.
But every read goes through `SafeIsBadReadPtr` (an SEH probe), which CLAUDE.md does not accept as
the argument. The argument is that `idx ≤ 2` makes the slot one of three inline fields of a live
unit, and a slot's weapon is always a `Weapons[]` entry (`0x49E070` copies the def's pointers,
unarmed = `&Weapons[0]`).

**Overlap: extra-weapons.** Side-slot weapons live in a DLL side table, not at
`unit + 0x10 + idx·0x1C`, and extra-weapons notes that stockpile weapons past slot 3 exist
(`UNITS_GiveUnit` loses their stock). If a BuildWeapon order can ever name a side slot, both stock
functions read garbage today. The bound must be `idx < 3`, redirecting side slots to
`slot_ptr(u, idx)` if the module ever builds them (INF). A′3's weapon array is a DLL static, and
the sentinel is still entry 0 with `+0xE4 == 0`.

**Our design.** At `0x439D41` (`mov ecx,[eax+0x36]; mov eax,[eax+0x3E]`, 6 bytes; `0x439D6B` is
the function's own `return 0`): `if (idx > 2 || !w || w[+0xE4] == 0) → 0x439D6B`. No probes. The
sim side we leave to stock unless the idx ≥ 3 path is shown reachable.

**How to test.**
- A fixture `.ufo` (built with `tools/hpipack.py`) holding a silo whose stockpile weapon has no
  `reloadtime`. Queue a missile, select the silo so the bottom panel draws.
- Stock: `tacli crash` at `0x439D65` (INF). Fixed: no crash, no bar.

**Decided 2026-09-25.** Ported ([the plan, B6](sim-fixes.md#the-landings)).

---

### 6. CircleRadius `0x438EDE`

**What TADR says (SRC).** `jl` → `jle`, "radius must be bigger than 0". Installed unconditionally.

**What stock does (DIS).** `DrawRangeCircle 0x438EA0` returns at once for radius 0 (`0x438EAF`).
N = `(int)(r·2π·0.125)` (the doubles `0x4FD2B0`, `0x4FD2B8`); the guard `0x438EDE jl 0x43904D`
catches only a negative N; then `idiv ecx` at `0x438EEE`. **Radius 1 gives N = 0 and faults.** The
target `0x43904D` sets both endpoints to 0 and still draws the label through `0x4C14F0` at
(0, 0+4) when a label is passed (`0x439055..0x43908A`). The engine map already records the fault.

**Real stock defect: yes** (a crash), **not reachable with stock data** (the engine map: nothing in
stock is that small). **Class: UI, local** (game thread, the frame).

**TADR's argument.** `jle` is a bound, but N = 0 then draws the circle's label in the screen's
top-left corner.

**Overlap.** Our own label placer already skips `n <= 0` (`tagpu_order.c:1076-1083`).

**Our design.** Test N = 0 and go to `0x43908F`, skipping the label too. Stock's negative-N path
stays as it is (rule 7).

**How to test.** A fixture FBI with one ShowRanges radius of 1 (e.g. `radardistancejam=1`), shown
through the ShowRanges set (`main+0x391BF`). Stock: crash at `0x438EEE`. Fixed: no circle, no
corner label.

**Decided 2026-09-25.** Ported ([the plan, B6](sim-fixes.md#the-landings)).

---

### 7. Anti-nuke circular target search `0x49D120` (and the minimap ring `0x4670C4`)

**What TADR says (SRC, `TABugFix.cpp:43`).** Replace the search's independent X/Z bounds with a
circle, and NOP the minimap's `sub ecx,0x200`. It is installed unconditionally, by an entry `jmp`
with a 5-byte check.

**What stock does (DIS).**
- `0x49D120(unit, idx)`: idx `& 0xFF`. With no stock in the slot (`unit + 0x1E + idx·0x1C == 0`)
  it returns NULL. Otherwise, over every projectile:
  - skip if `proj+0x66 == unit+0xFF` (**same owner only, so an ally's nuke is a valid target**);
  - skip if the weapon lacks `targetable` (`w+0x111` bit 29);
  - keep only `|unit.x − proj+0x28| ≤ c` and `|unit.z − proj+0x30| ≤ c`, where
    `c = w+0xE0 << 16` (`coverage`, loader `0x42E52A..0x42E540`);
  - skip it if another projectile's `+0x56` already names it.
- Callers: `0x408945`, `0x408B48`, `0x49DC2F`.
- **`+0x28..+0x30` is the projectile's TARGET, not its position.** `0x49C740` copies the start to
  `+0x04` and `+0x10` and arg 4 to `+0x28` (`0x49C786..0x49C797`). TADR's comment says "current
  map coordinates". Its code uses the same fields as stock, so the shields cover *where the missile
  is going* in both.
- The minimap ring `0x4670B7..0x4670CE` draws radius `coverage − 512`. The ShowRanges ring draws
  `coverage` (engine map, `0x439811..0x439948`).

**Real stock defect: no.** The engine consistently uses a square of half-side `coverage`. The two
rings disagree with each other and with the square. The circle removes 1 − π/4 = 21.5 % of the
protected area. READ: stock anti-nukes all have `coverage=2000`.

**Class B, a balance change.**

**TADR's argument:** not a safety question. But its jump replaces our extra-weapons detour at
the same entry (`FindIntercept`, `tagpu_weapons.c:1407`), and it refuses `idx >= 3`.

**Recommendation.** Do not port as a fix. If the owner wants circles, change both stock's search
and our `my_FindIntercept`, and the minimap ring with them.

**How to test.**
- ARMAMD with a stockpiled missile. An AI CORSILO ordered (`tacli order … attack pos`) at a target
  at (0.9c, 0.9c) from the AMD: inside the square, outside the circle.
- Stock intercepts; circle arm does not.

**Decided 2026-09-25.** Not a defect, so not in B: section B takes stock defects only. The search stays square.

---

### 8. JammingOwnRadar `0x467608`

**What TADR says (SRC).** Pass 3 of the view player's map build exempts only the view player's own
jammers. TADR exempts every ally (strict ally test when following another player's POV).

**What stock does (DIS).**
- `0x467440` runs inside the player loop `0x464F80`, in its periodic block, which runs once every
  30 ticks per player (`player+0xF0` against GameTime `main+0x38A47`, then `+0xF0 += 0x1E`,
  `0x46507D..0x465092`), and there only for the player equal to `main+0x2A43` (`0x465565`; its
  one call, `0x46556D`). Before the 30-tick gate, and so every tick, sit the per-unit sight emits
  (`0x4827B0`, `0x465053`) and, for that same player only (`0x465065..0x465070`), the radar
  rebuild `0x466DC0` (`0x465072`).
- Pass 1 sets or clears `unit+0x110` bits 8–10 for every unit (`0x46749F..0x4674F9`). Pass 2
  stamps radar and sonar through `0x47E890`.
- Pass 3 (`0x4675DC..0x46766C`): for every live unit whose owner `+0xFF` differs from the view
  player `[player+0x146]` and whose `+0x10E` bit 0 is set, it clears contacts inside
  `def+0x20A` (radar jam) and `def+0x20C` (sonar jam). **Allies' jammers jam you.**
- Those bits are not display-only. The acquisition builder `0x40AA40` pushes every unit with
  `state & 0x100` onto each player's radar list, which a targeting facility's owner falls back to
  ([line-of-sight](../line-of-sight.md)). Since the bits are relative to the local view player,
  AI players hosted on this peer use them too.

**Real stock defect: no.** It is a design choice, arguably a poor one. **Class B**: it changes
radar-targeted acquisition, and every observable radar picture.

**Recommendation.** Owner's design call, not a fix. If adopted: same site, ally test from
`[owner+0x108 + viewidx]` as pass 1 uses (`0x4674C5`).

**How to test.**
- The human, an allied AI with ARMJAMT, and an enemy unit inside the human's radar and the
  jammer's radius.
- Peek the enemy's `+0x110`: bit 8 is clear in stock and set with the change.

**Decided 2026-09-25.** A design choice, not a defect, so not in B.

---

### 9–10. ZeroDamageMapWeapons, TerrainFireGate — group C

- **ZeroDamageMapWeapons** (SRC). With `nomapweaponalert=1` on a weapon with `damage=0` and no
  attacker, it skips the whole area-damage call at `0x49A0A7` (and with it the feature hits, the
  interceptor tail and the under-attack alert) and hides the minimap dot (`0x467206`). No stock
  weapon carries the key, so stock is unchanged.
- **TerrainFireGate** (SRC). `notoverwater` / `notoverland` skip a weapon slot inside `AutoAim` at
  `0x49E1D6`. That overlaps our extra-weapons `AutoAim` entry hook.
- **Neither is a defect.** Both are class B for the flagged weapons only. Plan them with §C.

---

### 11. OffMapAircraft: three different problems

**What TADR says (SRC).** Aircraft off the map, or on it but "sheared off the LOS grid", cannot be
seen, targeted or damaged. It patches three things, all bounded by
`OFFMAP_AIRCRAFT_TARGETABLE_MARGIN_TILES` except the shear:
- B1: `UnitInPlayerLOS 0x465AFD`;
- B2: the collision call `0x49BD88`;
- B3: area damage at `0x49A12C`.

**The margin is installed in every config**: 1 tile in OTA, ProTA, BTA and TA Zero; 32 in
Escalation, Mayhem and Twilight. Only its width is escalation-specific.

**(a) The off-by-one (DIS) — a real defect TADR works around rather than fixes.**
- `0x47CC85 cmp edx(X+fw), W / jge off-map` and `0x47CCA1 cmp eax(Z+fh), H / jge off-map`, with
  `W = [main+0x14233]` the cell count (area damage clamps its exclusive bound to it).
- A footprint ending on the last column or row is therefore parked in `main+0x142B7`, although
  every cell is on the map. So an aircraft over the **bottom or right** edge takes no splash and
  no direct hits there; top and left are fine.
- Ground units and buildings are kept off the border by LoadMap's `0xFFFD` mask (the engine map),
  so this is an air bug in practice.
- **Fix:** `jge` → `jg` at both sites.
- The clear `0x47D0E0` decides by the bucket (`0x47D0F0..0x47D0FF`), not by a bounds test, so
  stamp and clear stay paired by construction. **INF** to check before landing: that the sort
  bucket index computed at `0x47CCA9` stays inside the bucket grid for a unit on the last cell,
  and that the re-claim `0x47C790` has no bounds test of its own.
- Class B, exact for every unit whose footprint does not touch the last row or column.

**(b) Aircraft beyond the edge (DIS: `0x49B0A6..0x49B0F8` marks an off-map projectile detonated
with no damage; off-map units have no stamps).**
- This is the engine's design, not a defect, and letting players shoot there is a gameplay change
  whose width is a balance knob.
- TADR's implementation:
  - walks the bucket list capped at 4096 (a sanity cap unrelated to our 15 001 slots);
  - keeps off-map rounds alive while the pool has fewer than 270 of **300** projectiles, which is
    stock's size — ours is 3000;
  - re-implements splash falloff and box distance in C `float`/`double`, not the engine's x87
    sequence, so values can differ in the last bit.
- **If wanted:** feed off-map aircraft within the margin through §2's candidate list. The engine's
  own distance test (which uses live positions) then does the math, with one dedup and no
  cross-module "disjointness invariant".

**(c) The LOS shear (DIS) — a real defect.**
- `UnitInPlayerLOS 0x465AC0` samples box points at `col = x>>5`, `row = ((z_hi) − (y_hi>>1)) >> 5`,
  under an **unsigned** bound against the player's LOS width and height (`0x465B6A..0x465B93`,
  again at `0x465C04..0x465C2D`).
- An airborne unit whose `z < alt/2` near the **north** edge gets a negative row, so it is
  invisible to every other player on every peer. The periodic acquisition `0x40AA40` uses it, so
  it is also untargetable. TADR measured an ARMATLAS at altitude 275, 5.5 tiles inside the border,
  landing on row −2.
- **Fix:** when the sheared row is out of bounds and the unit's true row is in bounds, use the
  true row (TADR's rule), else clamp. That is a bound, exact whenever stock's row is in bounds.
  Class B.
- Our order-marker code calls `0x465AC0` too (`tagpu_order.c:92`) and would follow automatically.

**How to test.**
- (a) ARMATLAS hovering at `x = W·16 − 8`. Peek `unit+0x82` against `*0x511DE8+0x142B7`: equal
  in stock. Then flak it and read HP.
- (c) An enemy ARMATLAS at `z ≈ 60` and cruise altitude. The human's AA does not acquire it in
  stock, and does with the fix.
- Both on two peers.

**Decided 2026-09-25.** (a) and (c) are ported as defects ([the plan, B1](sim-fixes.md#the-landings)). (b) is a gameplay change and is not in B.

---

### 12. AirCorpseFall

**What TADR says (SRC).** `UNITS_CreateCorpse` gives a water wreck a sink velocity but a land
wreck none, and the integrator retires all-zero records. So a wreck created in the air hangs.
TADR seeds `vy = −1` at `0x486439`. Escalation only.

**What stock does (DIS).** The feature tick's integrator `0x424214..0x4242B3` retires a record whose
velocity is all zero (`0x42422A` → `0x4242B8`). Otherwise it integrates, clamps to ground
(`0x485140`, zeroing the velocity at `0x424262..0x424270`), sinks at a fixed `−0x2CCC` below sea
level (`0x42428C..0x424299`), or subtracts gravity `main+0x14263`. No RNG and no cell writes.

**Real stock defect.** A true asymmetry. But READ: **none of the 30 stock flyers has `Corpse=`**,
so no stock aircraft leaves a wreck. The remaining stock path is cargo killed inside a transport
over land (INF).

**Class.** It changes the wreck record's Y (`+0x0C`), which is saved state and drawn. No reader in
the sim that depends on it is known (reclaim goes by cell), so it is mostly visual.

**TADR's argument.** Trivially bounded (reads the record it was handed, never overwrites a nonzero
velocity).

**How to test.** A loaded ARMATLAS (cargo ARMSTUMP) killed over land by AI AA. Look for the
cargo's wreck, compare its record Y with the ground under it (`0x485070` height), and take a
`tacli shot`.

**Decided 2026-09-25.** Measured alongside a landing. If stock leaves a cargo wreck in the air, it is raised as a visual residual ([the plan](sim-fixes.md#the-landings)).

---

### Corrections this pass makes (to TADR, and one to our notes)

- **The engine map, `0x439811..0x439948`, calls `w+0xE0` "attackrunlength".** The loader stores
  the `coverage` key there (`0x42E523..0x42E540`; `range` at `+0xDC`), and the label the same
  drawer prints for it is "weapon %d - coverage". The note needs this fix.
- TADR `TABugFix.cpp:1973`:
  - `weaponvelocity=0` faults at `0x49CE6A`, not `0x49CF19`;
  - bit 23 of `+0x111` is `burnblow`;
  - the zero case is two ±0.35° bands, not only "90°".
- TADR `FindAntiNukeTargetCircular`: `+0x28/+0x30` is the target, not the position.
- TADR `AreaDamageOverflow.h`: slot B is claimed only by `mask & 3 == 2`; other layer values stamp
  nothing.
- TADR (AreaDamageOverflow and GridClaimTieBreak) assume lockstep ("insertion order identical on
  every client", "clients disagree"). This engine is state and event replication. The claims do
  not transfer.

### Safety review in one place

- **Probes.** `SafeIsBadReadPtr` in BuildWeaponSlotGuard and in the crash handler. None is
  needed: every value in these items can be bounded (a slot index against 3, a unit index against
  the array's count, a cell ordinal against `W·H`, a divisor against 0).
- **Timing.** AreaDamageOverflow's index: built once per `GameTime` in the main loop, used after
  the pump and the unit tick, not re-validated. This is the one real hole in the cluster.
- **Sanity caps posing as bounds.** OffMapAircraft's 4096-node walk, and its hard-coded 300/270
  pool figures (wrong under A.1's 3000).
- **Blind writes and entry `jmp`s at shared sites.**
  - `0x49D120`: TADR `jmp` against our detour.
  - `0x49A0A7` / `0x49A0A9`: two TADR modules, with an install-order dependency documented in a
    comment.
  - Our port needs one byte-check table per function (`0x49A120`, `0x49CDE0`, `0x49D120`) shared
    with the modules already there.

---

## Part 3. Orders, construction, resurrection, scripts

A read-only pass made 2026-09-25 over the TADR fixes in this cluster, to settle which of them fix
the stock 3.1 engine and what a port would rest on. Sources: `pristine/TotalA.exe.pristine`,
`objdump -d -M intel` (the linear sweep in the scratchpad, cross-checked with
`--start-address` where a jump table had misaligned it); `vendor/TADR` at `dcff5dd`; the retail
install's unit FBIs read through `tools/hpipack.py` (nothing extracted into the repo). Tags:
**DIS** = disassembled here, **SRC** = read in TADR's source, **INF** = inferred, with the
measurement that would settle it.

"Reachable from X" below means the direct-call closure of X in the linear sweep (3 844 function
starts). Indirect calls are listed where they matter; they are the method's blind spot.

### Summary

| item | real stock bug? | class | TADR's fix by construction? | recommendation |
|---|---|---|---|---|
| 1. Resurrection finalisation (`fe685c7`) | **the failure branch is stock** (`0x40514F` → return 8 leaves the new unit a 0-HP nanoframe), **the trigger TADR names is not found in stock**: nothing reachable from `CreateUnit` writes a cell's def word | sim (owner's peer runs the order; result replicates) | **no**: writes `+0xA..0xB` of a cell whose contents changed, re-reads a possibly reused wreck record, runs `FEATURES_Destroy` on whatever now sits there, return-address hijack in globals | **measure first** (counter on `0x405155` over many resurrections); port nothing now. If it fires: our own finalisation that touches no grid cell |
| 2a. `KeepOnReclaimPreparedOrder` (`0x4A7132`) | no — a UI behaviour for Mex/WreckSnap | UI, local | n/a (applies to *every* ON toggle gadget, not only Reclaim) | do not port (belongs with WreckSnap, group D) |
| 2b. `JunkYardmapFix` (`0x42CF5E`) | **yes**: a structure with no `YardMap` (or a short one ending in an invalid char) is parsed past the string's NUL, from stale stack bytes | sim (occupancy, placement, pathing; load time, every peer) | **no**: a NULL yardmap is dereferenced unguarded by six readers (`CreateUnit`'s stamp, the placement test) — it turns nondeterminism into a fault for any footprint > 0 | **port, our design**: a bounded parse, stock-exact for all 126 retail structures (scanned) |
| 2c. `UnitVolumeYequZero` (`0x49CE65`) | not in this cluster: the site is the ballistic divisor `div [weapon+0x68]` | — | dead code in TADR (commented out 2014, never installed) | nothing to port; hand the divisor to the weapons cluster |
| 2d. `BadModelHunter` (`0x458C5A`) | yes, but draw-only, and **already fixed by us** (L7 composite bound) | draw, local | a size compare that skips the draw and pops a message | do not port (superseded) |
| 3. `OrderDispatchGuard` (`0x43B865` `0x43BB0F` `0x43A202`) | **not as TADR frames it**: the table holds 68 records for the process and every order creator traced writes an index < 68 by construction. TADR's two motivating crashes were its own return-address envelope. One latent stock hole: the save loader's by-index fallback (`0x43A556..0x43A598`) | diagnostic (observe-only); sim if it bailed | **no**: `SafeIsBadReadPtr` probes; a bound cannot make a freed order safe | do not port the guard. Optional: 2-byte fix of the loader fallback |
| 4. Spawned-unit initial commands (`3112e68`) | **no** — TADR passed the wrong context to `0x487BF0` in its own InitialOrders feature (`9e1cdee`) | TADR-only (group E, map unit spawns) | n/a | do not port; the stock call shape is recorded for group E |
| 5. `RepairRateFix` (Escalation) | **no, balance.** Stock heals **exactly 1 HP and charges exactly 1 energy per repairer per call** (`min(ceil(…),1)`, not TADR's `max(1,ceil(…))`) | sim, balance | bound ✓ (index-keyed table); but its "energy unchanged" claim is false against the bytes | do not port: TA's flat rate is the game's rule |
| 6. ctrl-F / ctrl-B skipping units | **no** — TADR's own hotkeys (`ExternQuickKey`), which walked `UnitsNumber` instead of the slot block | UI, local | n/a | out of scope; one engine fact kept |
| 7. Rotated-unit fixes (`ed71666` `b306cbf` `fa27e3e` `577a22d`) | **no** — all four repair TADR's rotation (its UNITINFO swap and return-address envelopes) | TADR-only | n/a | out of scope; one reentrancy lesson kept |
| 8. PatrolDisable\*, FixedPositionGuardingCons, ConstructionKickout | **no** — behaviour options | sim, owner-local settings | n/a | group D; no stock bug hides in them |

### 1. Resurrection finalisation (`fe685c7`, `unitrotate.cpp`)

**What TADR says.** "TA can create a replacement unit from a wreck and then fail its second
lookup of that wreck after unit creation has already cleared it. The resurrection order then
exits before initialization is complete, leaving a mobile unit partly built or a structure as a
permanent unusable wireframe" (`tdraw.txt:259-270`, credited to a tester report; no
reproduction in the repo). The commit adds a snapshot of the wreck at the entry of state 5
(`CaptureResurrectFeature`, `unitrotate.cpp:712`) and a hook on the post-create check
(`ResurrectPostCreateFeatureCheck_Proc`, `:752`, at `0x40514F`, 6 bytes). The rest of the diff
is the rotation feature: arming `g_pendingCreateRotation` from the wreck's heading and clearing it
through a return thunk (`:792`, `:1284`). **That half is rotation-only and out of scope.**

**What stock does (DIS).** `0x404DB0` is `Order_Resurrect`, `stdcall(builder, order, flags)`,
`ret 0xC`. It is the `+4` handler of the descriptor record at `0x4FC8AA` (`0x4FC6E8 + 18·0x19`),
whose `+0` is "Resurrecting" and `+0x15` the name "Resurrect" — so the engine map's
`[INFERRED]` on `0x40514A` can become DIS (the dispatch loads `+4`; see item 3).

- **The entry check, every state ≤ 5** (`0x404DC6..0x404E16`): `0x421DA0(order+0x22, …)` returns
  the wreck's def through one `0xFFFE` hop; `0xFFFF` → "Resurrection failed" (`0x501698`) and
  return 8; FeatureDef `+0xFE` bit 7 clear (not resurrectable) → return 8.
- **The jump table `0x4052D8`**: 0 `0x404E3C`, 1 `0x404E88`, 2 `0x404F1F`, 3 `0x404F36` (the
  type from the wreck's name cut at `_`, `0x488B10`, into `order+0x36`; none → "Ressurection
  failed" `0x501684`), 4 `0x405005` (the countdown with the nano stream `0x4720D0`), 5
  `0x4050D1`, 6 `0x405243` ("Resurrection complete", a repair order from `0x43F0E0`, return 5).
- **State 5** (`0x4050D1`): `CreateUnit 0x485F50(builder+0xFF, order+0x36, order+0x22, 0, 1, 0)`
  — sixth argument 0, so the unit is born a nanoframe (`Nanoframe` `+0x104` = 1.0, `0x485B27`)
  — then `0x489690` sets `order+0x12` (the unit at `+0x16`). NULL → "Unable to create any more
  units" (`0x501310`), wait `0x12C`, return 2: the retryable failure. Otherwise
  `0x4815F0(order+0x22)` → the anchor cell, `GetGridPosFeature 0x421E60` → its def, and:
  - **`≥ 0xFFFB` → return 8** (`0x40514F..0x405161`). The unit stays as `CreateUnit` left it,
    `Nanoframe` 1.0 and no repair order follows. **This is the stock branch TADR describes.**
  - `< 0xFFFB` → the wreck record `main+0x1420B + (cell+0xA)·0x30`, its `+0x20` rotation triple
    copied to `unit+0x64`; `FEATURES_Destroy 0x4246B0(0x4815A0(pos), 0)`; in a network game the
    `0xFF` sentinel for the anchor's cell through `0x451DF0` (`0x4051B9..0x405210`);
    `unit+0x104 = 0.0`, `unit+0x108 = 1`; `0x41C110(builder)`; return 1 → state 6.

**Is the trigger a stock defect? Not demonstrated.** The failing lookup runs ~30 instructions
after the entry check found the wreck, in the same call. Between them run only `CreateUnit` and
`0x489690`. DIS:

- Nothing in the direct-call closure of `0x485F50` (374 functions) or of `0x489690` reaches
  `FEATURES_Destroy 0x4246B0`, `SpawnFeatureOnMap 0x423C50`, the swap `0x423710`, `FeatureDie
  0x423550`, `KillUnit 0x4864B0`, the corpse spawn `0x486360`, the network dispatcher `0x453D40`,
  or any order dispatcher.
- Every function in that closure that loads the grid base `main+0x14287` only reads the def word
  (`0x47CC30` writes a cell's `+0`/`+0xC`, `0x483210` its `+5`/`+6`); the only writers of a cell's
  def are `FEATURES_Destroy` (`0x424774..0x424780`) and LoadMap.
- The closure's indirect calls: the listener broadcast `0x490580` (`ecx = [main+0x391ED]`), whose
  listeners are the campaign's victory goals (`KillEnemyCommander`, `DestroyAllUnits`,
  `KillAllMobileUnits`, vtables `0x4FD960`/`0x4FD948`, built at `0x48E010`); `0x48B090`'s
  `call [eax]` (activate-on-create); the COB VM under `UNITS_CreateModelScripts 0x485D40`. None is
  known to touch features [INF].

So in stock the second lookup should agree with the first. TADR ran with its own `CreateUnit`
envelope (rotation) and other hooks live; the trigger may be TADR's. **INF — settle it with**: a
measurement build that counts passes through `0x405155` (failure) and `0x405164` (success), the
stock-rules DLL (no rotation), and a few hundred resurrections of 1×1 and multi-cell wrecks.

**TADR's safety argument, re-checked.**

- The snapshot is bounded (tile inside `FeatureMapSizeX/Y`, def `< NumFeatureDefs`, the
  resurrectable bit). ✓
- The fix itself is **not by construction.** It fires exactly when the anchor cell has changed,
  then writes the saved record index into `root+0xA..0xB` without knowing what the cell now is.
  If it is another feature's `0xFFFE` cell, that overwrites the anchor offsets that `0x4815F0`,
  `0x421E60` and `0x421DA0` follow — and `0x421DA0` reads `[plot+8]` after its hop with **no
  NULL test** (`0x421DFF` → `0x421E04`). If it is a new anchor, it relinks that feature to a
  stale record. Stock then copies a rotation out of a record that may belong to another feature,
  runs `FEATURES_Destroy` on the cell at the order position (destroying whatever now stands
  there), and broadcasts a removal to peers: a new sim divergence in exactly the case it targets.
- The globals and the return-address hijack (`ResurrectReturnThunk`) are safe only because
  `Order_Resurrect` cannot re-enter itself during `CreateUnit` (DIS: no direct path from
  `0x485F50` to `0x43B7C0`/`0x43BAD0`/`0x43A1F0`/`0x404DB0`). Code shape, not design; it is the
  pattern that crashed TADR in item 3.

**Class.** Sim. Orders run on the owner's peer (the model in networking-lobbies.md; here, only the running peer broadcasts the `0xFF` removal); the unit already went out as `0x09` inside
`CreateUnit` (`0x456050`), and its build state replicates as unit state. Standing rule 7 holds
for any fix confined to the failure branch.

**Overlap.** Our NULL-plot guard on `0x421E60` (`tagpu_patches.c`) covers `0x40514A`: a NULL plot
now answers `0xFFFF` and lands in this same return-8 branch instead of faulting. The engine map
notes `0x40514A` as "not audited"; this pass audits it. An off-grid position fails the entry check
cleanly (`0x421DA0` tests the first plot, `0x421DCA`), and a `0xFFFE` cell whose anchor offset
leads off the grid faults inside the entry check first (`0x421DA0` does not test the plot after
its hop, `0x421E04`). So `0x40514A` sees a NULL plot only if the cell changes between the two
lookups — the same unfound trigger.

**Proposed design (only if the measurement fires).** A detour on the failure branch that touches
no grid cell: the unit in `order+0x16` is this call's `CreateUnit` result by construction
(`0x489690` sets or clears it at `0x40510D`; `0x405115` tested it non-NULL), so finalise it as the
success path does — `+0x104 = 0.0`, `+0x108 = 1`, `0x41C110(builder)`, return 1 → state 6 — and
skip the rotation copy (no record) and the feature removal (nothing to remove). No return-address
hijack.

**Test.** Only `CORNECRO` (`CorNecro.ufo`, WorkerTime 160) has `canresurrect` in the retail data
(scanned). A scenario with a Necro and wrecks of several footprints (`features: [{type:
"armlab_dead", …}]`). `tacli order` has no resurrect verb (its table stops at the button codes
1–14); add one resolved by descriptor name "Resurrect" (`0x438760`), or click the RESURRECT
order button with `tacli ui`. Read each new unit's `+0x104`/`+0x108` with `tacli peek` (unit
array `*0x511DE8+0x14357`, stride `0x118`).

**Decided 2026-09-25.** (a) A time-boxed measurement; parked if the counter never fires. (b) If it fires, the unit is finalised as the success path does, touching no grid cell ([the plan](sim-fixes.md#the-landings)).

**Measured 2026-09-26: it never fired** — 0 of 847 resurrections took `0x405155`, 262 of them
ordered on a multi-cell wreck. There is no resurrect verb to add: `reclaim` by a unit that can
resurrect resolves to RESURRECT (`0x43F5A5` → `0x44004C`). The numbers are in
[the plan's B1 results](sim-fixes.md), the paths in the engine map's `Order_Resurrect` section.

### 2. `TABugFix.cpp`: KeepOnReclaimPreparedOrder, JunkYardmapFix, UnitVolumeYequZero, BadModelHunter

#### 2a. KeepOnReclaimPreparedOrder (`0x4A7132`, `e27f118`, all configs)

**Stock (DIS).** `0x4A7132` is inside the gadget input handler `0x4A6AE0` (one caller,
`0x4A9FD0`): a gadget's hotkey (`+0x13A`) on a toggle gadget (`+0x1B` bit `0x40`) flips its
`status_init` word `+0x138` (gui-gadgets.md §3) and redraws it through `GUI_ButtonDraw 0x4A5F40`.
**TADR** (`TABugFix.cpp:314`) skips the flip to `0x4A715C` when the prepared order is BUILD and
the gadget is on, so shift+q/e alternates mex-building and reclaiming (WreckSnap). The test does
not check which gadget it is: any ON toggle keeps its state under its hotkey while a build is on
the cursor. **Not a defect; UI; local.** Do not port (group D, with WreckSnap).

#### 2b. JunkYardmapFix (`0x42CF5E`, `16d2429`) — a real stock defect

**Stock (DIS).** In the unit-def loader, for `BMcode` 0 (`[def+0x22F] == 0`, `0x42CF38`):
`TdfFile::GetString 0x4C48C0(buf, "YardMap", 0x400, "")` into a stack buffer at `[esp+0x128]`
(`0x42CF42..0x42CF59`). GetString returns 1 and NUL-terminates within the size (`0x4C4951`), or
returns 0 and copies the default `""` (`0x4C4963`). The return is ignored. `0x42CF5E..0x42CF7A`
allocates `footX·footZ` bytes into `def+0x14E`, and the loop `0x42CF9D..0x42D06B` fills them:

- a char in `.CGOYcfowy` writes one cell (`.`→`0x00`, `C`→`0x35`, `G`→`0x8F`, `O`→`0x2B`,
  `Y`→`0x31`, `c`→`0x2D`, `f`→`0x6F`, `o`→`0x2F`, `w`→`0x37`, `y`→`0x29`; table `0x42D198`, jumps
  `0x42D16C`), and advances unless the **next** char is NUL — so a short string repeats its last
  valid char;
- **any other char, NUL included, is skipped and the pointer advances** (`0x42CFB5` → `0x42D049`).

So a missing key (buffer `"\0"`), or a short value whose last char is not valid, walks past the
terminator into the rest of the 0x400-byte buffer — stale stack bytes, likely the previous
unit's value when the loader runs at the same depth [INF] — and on up the stack until it has
found `footX·footZ` valid bytes. The yardmap is then whatever
the stack held: it decides placement (`0x47D2E0`), occupancy (`0x47CC30`'s stamp) and pathing, so
two peers can disagree [INF: settle by loading one such unit twice and comparing the bytes].

**Reachable in stock data? No.** Scanned the 278 retail unit FBIs: all 126 `BMcode=0` units have a
`YardMap`; every short one ends on a valid char (`armestor` "o", `armdrag` "f", `armguard`
"oooo"); the one value with a trailing blank, `corgant`, has its 81 valid chars before it. **Mods
reach it** — hence TADR's fix, and its anticheat alarms on yardmap hashes.

**TADR's fix is not safe.** Undefined (or `footX·footZ == 0`) → `def+0x14E = NULL`. A `BMcode` 0
unit sets `unit+0x110` bit 29 (`0x485A8B..0x485A9E`), and every yardmap reader on that path
dereferences with no NULL test: `0x47CD82` (the stamp inside `CreateUnit`), `0x47D4ED` (the
placement test), `0x47C7FE`, `0x47D166`, `0x47D8C2`, `0x47DA47`. Only the GUI's `0x4A2650` tests
it. So for any footprint > 0 the NULL faults at the first placement preview or creation [INF:
settle with a test unit under TADR]. Safe only for a 0-cell footprint, where stock was already
harmless.

**Class.** Sim, load time; every peer runs it (rule 1). Rule 7: identical to stock for every
string stock parses inside its terminator — all retail units.

**Overlap.** The same TADR commit also carries `CanBuildArrayBufferOverrunFix` (`0x42DAC7`) and
the download-menu allocator (`0x42DD74`); **we already fix both** ("whole build lists" and
"download menus past five entries", binary-patches.md).

**Proposed design.** Detour at `0x42CF5E` into our own parse. **The invariant:** it never reads
past the NUL GetString wrote, and GetString always writes one inside the 0x400 buffer. It applies
stock's table, skip rule and repeat-last rule byte for byte, and where stock would step past the
NUL with cells still to fill, it fills them deterministically: the last valid char, or a fixed
value when the string has none. Stock's allocation stays (never NULL).

**Test.** A test `.ufo` (`tools/hpipack.py`, one unit = one `.ufo`): a copy of `CORSOLAR` (5×5)
without `YardMap`, and one whose value is `"oo?"`. On the stock-rules build, `tacli peek` the 25
bytes at `*(*0x511DE8+0x1439B)+type·0x249+0x14E` across two launches; with the fix, the chosen
fill both times. MEASURED 2026-09-25 (B6): the stock-rules bytes did **not** differ between two
launches of one build on the reference setup (`"oo?"` read `2f2f31313100002b2b…` both times, the missing
key all `0x2F`), so the stack's leftovers are repeatable there; the defect is that they are the
stack's, whatever a given build leaves in it. A retail-units regression: all 126 structures' yardmap bytes
identical with and without the fix.

**Decided 2026-09-25.** Past the terminator, the last valid char repeats; a string with no valid char fills `o`. The same rule covers a trailing invalid char ([the plan, B6](sim-fixes.md#the-landings)).

#### 2c. UnitVolumeYequZero (`0x49CE65`)

**TADR.** Declared, set to NULL, its install commented out since the SVN import (`3ca8be3`,
2014) and deleted in `de5129b` (2023). **Dead.** It "fixes" a unit's `Turn.Y`. The site is
`mov ecx,[edx+0x68]` / `div ecx` in `0x49CDE0` (TADR: `UNITS_FireProjectile_Ballistic`; callers
`0x49D0C0`, the `0x0D` receiver `0x49D270`, `0x49D580`), where `edx` is the **weapon** and `+0x68`
its velocity: weapon 0, the "no weapon" entry, divides by zero (TADR `tamem.h:179`). That is
weapons-cluster territory (our `0x49D280` fix takes the weapon from the full ID). Nothing to port
here.

#### 2d. BadModelHunter (`0x458C5A`)

**Stock (DIS).** `0x458C5A` is the `rep stos` that clears the composite scratch frame inside the
build-state copy `0x4589C0`. **TADR** (`TABugFix.cpp:2796`) checks the frame against 600 × 600,
logs the unit to `ErrorLog.txt`, pops a chat warning, and unwinds to `0x4596EB` (no draw).
**Draw-only, and superseded**: our L7 composite bound (binary-patches.md, `0x458B87` and the other
writers) sizes every writer by area and grows the frame to 2048². Do not port.

### 3. OrderDispatchGuard (`0x43B865`, `0x43BB0F`, `0x43A202`)

**What TADR says** (`TABugFix.cpp:1121-1331`). The two controllers dispatch
`call [*(0x512344) + order[+4]·0x19 + 4]` with no bound against the end `0x512348`, so a recycled
order drives a wild call. The guard probes the order with `SafeIsBadReadPtr(order, 5)`, bounds
the index, checks the handler lies in `.text`, and records a breadcrumb. It is **observe-only**
(`TDRAW_ORDER_DISPATCH_BAILOUT 0`): "we want the crash".

**The three sites (DIS).** `0x43B865` `mov ecx,[0x512344]` in `0x43B7C0`, call at `0x43B87C`;
`0x43BB0F` `mov edx,[0x512344]` in `0x43BAD0`, call at `0x43BB21`; `0x43A202` `mov
ecx,[0x512344]` in `0x43A1F0`, call at `0x43A21A`. `0x43A1F0` is **the order destructor**
(vtable `0x4FD2C8`), which dispatches the handler once more with flags 2 when `order+6` bit 1 is
set. Its callers include `NewMainOrder2Unit 0x43AFC0` and both controllers, so a handler that
orders **another** unit re-enters that unit's handler, nested — stock is reentrant here.

**The table's count (DIS): 68, fixed for the process.** `UIPipelinesInit 0x491200` (one caller,
`0x49E830`, under the program's entry chain) calls `0x43C050` once. That appends the `"Ready"`
record at `0x4FD288` (handler `0x439EA0`, empty name), then the three static tables through
`0x43BC90`: 22 at `0x4FC6E8` (Standby … Resurrect `0x404DB0` … RepairUnitNoMove), 22 at `0x4FCA18`
(the VTOL set) and 23 at `0x4FC490` (Stop … QPatrol). Each append re-sorts by name (comparator
`0x43C020`, strcmp on `+0x15`), so index 0 is "Ready". No other function writes
`0x512344`/`0x512348`, directly or through the vector helpers `0x43C360`/`0x43C390`/`0x43C3A0`
(called only from those two); the destructor `0x438480` frees it at exit.

**Is the index bounded by construction? Yes for every creator traced.** The byte stores to
`order+4` found in the order module and the handlers are four:

- the constructor `0x43A0C0` (`0x43A0E0`), 48 callers. Those read pass the result of the name
  lookup `0x438760` (binary search; not found → 0, `0x438819`; found → its index), of the
  order-for-target chooser `0x43F0E0` (itself 15 lookups), or a copy of another order's byte
  (`0x43A074`); the rest pass their own caller's argument (`NewMainOrder2Unit 0x43AFC0`'s family)
  [INF: that every chain ends in a lookup — settle with a static trace of the 48 sites];
- `0x438B90` (set type), from `0x4031B1` (`0x43F0E0`'s result) and `0x40624C` (a lookup of
  `VTOL_MOVE`);
- the save loader `0x43A420` at `0x43A60A`. Its by-name branch (`<key>_name`, `0x43C6B0` then
  strcmp) gives the index or 0. **Its fallback, taken when the record has no name, walks the table
  `jbe` — inclusive of `end` (`0x43A58B..0x43A58D`)** — counting records whose `+0x14` bit 0 is
  clear. A stored index it does not meet leaves `cl` at 69, or 68 if the phantom record past the
  end matched, and that byte becomes the order's type: **a wild dispatch on the first tick. A
  latent stock defect**, reachable only from a save without the name: this exe's writer
  (`0x43AA90..0x43AAE3`) always writes `<key>_name`.

So in stock a live order's index is `< 68` for every creator read. A wild index then means the
order is not a live order — a lifetime bug a bound cannot make safe (the handler would still read
freed memory).

**TADR's evidence is its own.** The 2026-09-14 "venom" crash and the 2026-08-31 game 189724 share
one signature: a return into `dynmem+0x14353`. The first is diagnosed in `unitrotate.cpp:541-560`:
its `Order_MobileBuild` envelope saved the return address in one global, ConstructionKickout's hook
at `0x403CD0` cancelled another unit's build order from inside the handler, the destructor
re-dispatched it (`0x43A21A`), and the outer return went to the inner caller. `577a22d` fixed that
with a return stack. **No stock defect is shown.**

**TADR's safety argument.** `SafeIsBadReadPtr` is a probe — "what does not count" in CLAUDE.md.
The `.text` range test on the handler is a sanity filter. Observe-only, it changes nothing.

**Proposed design.** Do not port the guard. Optional, by construction: close the loader's fallback
with two bytes — `0x43A58D` `76 DE` → `72 DE` (`jb`, exclusive end) and `0x43A58F` `EB 07` → `EB C1`
(not found → `0x43A552`, `xor dl,dl`, i.e. "Ready" like the by-name branch). Identical to stock for
every save stock writes. **For the whole port**: order handlers are reentrant through `0x43A21A`,
so no detour may keep a return address or per-call state in a global.

**Test.** Loader: take a `.sav`, remove one order's `_name` string and set its stored index past
the saveable count; load on stock (expect a fault in `0x43B7C0`'s dispatch, `tacli crash`) and
on the fix (the unit idles in "Ready"). The format is the save writer's `0x43AA90`, not mapped
yet [INF].

**Decided 2026-09-25.** Yes: it is a stock defect proven by the disassembly, and the two bytes ride with B6 ([the plan](sim-fixes.md#the-landings)).

### 4. Spawned-unit initial commands (`3112e68`, `MultiplayerSchemaUnits.cpp`)

**What changed.** `9e1cdee` (2024-10) added InitialOrders to TADR's map unit spawns and called
`Campaign_ParseUnitInitialMissionCommands 0x487BF0(unit, text, int* uniqueId)`. `3112e68`
(2025-01) passes `{int iMissionUnit; UnitStruct** spawned}` instead, parses the commands only
after every unit exists (`:459`), and refuses `unitInfoId < 0` (`:44`).

**Stock (DIS).** The only stock caller, `LoadCampaign_UniqueUnits 0x488310` at `0x488514`, passes
the address of a two-dword local whose second dword is the array of units it created
(`[esp+0x20]`, `0x488502`). The Ident resolver `0x487AF0` reads that array through `[ctx+4]`
(`0x487B02`), bounded by the mission's unit count `[[main+0x391E9]+0xDB0]` (`0x487B19`). The stock
call is self-consistent. **TADR's crash was its own `int*`**: `[ctx+4]` read the next word of its
stack as the array.

**Class.** TADR-only (group E, map unit spawns). Do not port. If group E ever ports spawns: call
`0x487BF0` with the stock context — array sized to the mission count, filled before any command
is parsed.

### 5. RepairRateFix (Escalation only)

**What TADR says** (`RepairRateFix.h`, `.cpp`, `f183cb6`, `f973336`). `HealUnit_HealTimeWay
0x41BD10` computes `max(1, ceil(maxHP·t/buildTime))` HP and `max(1, ceil(E·t/buildTime))` energy
per repairer per call, so several cheap repairers out-heal one strong one. TADR replaces it with
an exact remainder carried per (repairer, target), energy "vanilla-identical", ×3 for Escalation.

**Stock (DIS).** Five callers pass `t = floor(WorkerTime/30)` (`0x88888889`-magic divide of
`def+0x1FE`): `0x402518` SelfRepair, `0x40561A` RepairUnit, `0x40583F` RepairUnitNoMove,
`0x41513D` VTOL_RepairUnit. The fifth, `0x48AF92`, is passive regen: every 8th tick
(`test [main+0x38A47],7`) with `t = floor(HealTime·8/30)`. The function returns 0 at full health,
computes `hp = ftol((maxHP·t − 1)/bt + 1)` and `e = ftol((E·t − 1)/bt + 1)` (ceilings; constants
`1.0` at `0x4FCC8C`, `−1.0` at `0x4FCCA0`), and then:

```
0x41BD87  83 FE 01        cmp esi,1         ; hp
0x41BD90  7C 05           jl  +5
0x41BD92  BE 01 00 00 00  mov esi,1         ; hp >= 1  ->  hp = 1
0x41BD97  83 F8 01        cmp eax,1         ; energy
0x41BD9A  7C 08           jl  +8
0x41BD9C  C7 44 24 20 01… mov [esp+0x20],1  ; e >= 1   ->  e = 1
```

**That is `min(x, 1)`, not `max(1, x)`.** Stock heals exactly 1 HP and charges exactly 1 energy
per repairer per call whenever `maxHP·t ≥ 1` (any WorkerTime ≥ 30), whatever the WorkerTime:
30 HP/s per repairer at gamespeed 10 if the handler calls it every tick [INF]. Passive regen gives 1 HP every 8 ticks for any HealTime ≥ 4. That is exactly why
"many cheap repairers heal faster": the rate is per repairer, flat. It is a rule, not a defect.

**Consequences for TADR's version.** It makes repair proportional to WorkerTime, so it buffs every
repairer whose exact rate exceeds 1 HP/tick. Its energy cost `max(1, ceil(E·t/bt))` is not
"vanilla-identical": for `ARMCOM` (t = 10) repairing `ARMLLT` (E 2546, bt 4662) it charges 6 per
tick where stock charges 1 [INF until measured]. The accumulator itself is sound: keyed by
`UnitInGameIndex`, bounded by a table resized from the unit array on every call — a bound ✓. The
multiplier choice by return address `0x48AF9B` is a code-shape constant ✓.

**Class.** Sim, balance; owner's peer; below-limit behaviour changes by design (rule 7 does not
apply to a rule change).

**Recommendation.** Do not port as a fix. If the owner wants Escalation's balance, it is a group-D
rules decision, and it should be written from the stock formula above, not TADR's.

**Test.** A scenario with `ARMLLT` at 10% HP and one `ARMCOM` (t = 10) ordered `repair unit`; a
second run with `ARMCK` (WorkerTime 80, t = 2). Read `+0x108` with `tacli peek` at t0 and t0+10 s
(check `gamespeed` is `0xa` first). Stock per DIS: +300 HP in both runs. TADR's model predicts
+600 for the Commander.

**Decided 2026-09-25.** TA's flat rate is the game's rule, not a B defect. The inverted clamp is recorded in the engine map, and a live check of the per-call rate rides along with a landing.

### 6. ctrl-F / ctrl-B skipping units (`63d779d`, `ExternQuickKey.cpp`)

**TADR's own hotkeys**: `ExternQuickKey::Message` intercepts ctrl+B (`FindIdleConst`) and ctrl+F
(`FindIdelFactory`) in the window procedure (`:218-240`). The bug was in its walk: it looped to
the player's live `UnitsNumber`, but a death frees its slot in place, so survivors above that
count were never visited. The fix walks the slot block `Units..UnitsAry_End` inclusive and skips
`UnitID == 0`. **Not stock; UI; local.** Out of scope.

**The engine fact to keep** (DIS, `CreateUnit 0x485FEA..0x486051`): a player's unit slots run
from `[main + player·0x14B + 0x1BCA]` to `[+0x1BCE]` **inclusive** (`jbe`); a free slot has
`UnitID` (`+0xA6`) 0. Any walk of our own must use that block, never the live count.

### 7. The rotated-unit fixes — all out of scope

We have no unit rotation, and none of these fixes reaches a stock path.

- **`ed71666`** staircase yardmap: TADR's UNITINFO footprint/yardmap swap leaked from its
  `_TestBuildSpot` preview into the next `CreateUnit` (the comment at its `MobileBuild_PreCreate`).
- **`b306cbf`** builder sitting in a rotated footprint: its drag-orders (`tahook.cpp` `DragUnitOrders`)
  and kickout read unswapped `FootX/FootY`.
- **`fa27e3e`** give / resurrect / capture / save-load re-creation paths that skipped its
  `CreateUnit` rotation envelope.
- **`577a22d`** the return stack for its re-entered `Order_MobileBuild` envelope (item 3).

The lesson for the port is item 3's: handlers re-enter.

### 8. Behaviour options — group D, no stock bug

- **PatrolDisableBuildRepair / PatrolDisableReclaim** (`0x4059E4`, `0x405B18` in RepairPatrol
  `0x405980`; VTOL `0x41547D`, `0x415621` in `0x4152F0`): under hold-position, a patrolling
  builder skips the build/repair search, or the reclaim search, according to the ctrl-F2 dialog.
  A sim behaviour driven by a local setting (orders run on the owner's peer).
- **FixedPositionGuardingCons** (`0x4066AC`, in `Follow_Ground 0x406300` by address order): rewrites a guarding
  builder's home offset (stay / scatter). A behaviour.
- **ConstructionKickout**: build under your own units (`0x4198C3` `push 0` → 1, the placement
  test `0x47D554`); raise the blocked-site retries from 10 to 20 (`0x403D05` `cmp eax,0xA`; VTOL
  `0x414046`); kick the blocker from inside `Order_MobileBuild` (`0x403CD0`, `0x41400D`). It
  rewrites the build-square colour byte at `0x469E7F` with `WriteProcessMemory` every frame, and
  it is the re-entry path of item 3's crash. Stock waits 30 ticks ten times and gives up with a
  message (`0x403CE4..0x403D08`). A behaviour, and a hazard to design around if group D takes it.

### Engine facts for the map (from this pass)

- `0x404DB0` is `Order_Resurrect` (DIS: `+4` of descriptor `0x4FC8AA`; the engine map's
  `0x4FC8A6` is 4 bytes early). Its states, messages and failure branch are in item 1.
- The order-descriptor array: 68 records, built once by `0x43C050`, sorted by name, index 0
  "Ready". Name lookup `0x438760` returns 0 when not found. The destructor `0x43A1F0` re-dispatches
  (reentrancy). The save loader's inclusive fallback walk is `0x43A556..0x43A598`.
- `0x421DA0` reads `[plot+8]` after its `0xFFFE` hop with no NULL test (`0x421E04`); callers
  `0x404AE9` (Reclaim), `0x404DE4` (Resurrect), `0x414788` (VTOL_Reclaim), `0x465358`. Reachable
  only through a corrupted `0xFFFE` cell.
- The yardmap parse `0x42CF3E..0x42D071`: its char table, the skip and repeat rules, and the walk
  past the NUL. Six readers of `def+0x14E` do not test NULL; `0x4A2650` does.
- `HealUnit 0x41BD10`: `min(ceil(…), 1)` for HP and energy; its five callers and their `t`.
- `0x487AF0`, the Ident resolver: context `{?, UnitStruct** units}`, bounded by
  `[[main+0x391E9]+0xDB0]`.
- The campaign victory-goal listeners: `0x490580` via `[main+0x391ED]`, built at `0x48E010`.

---

## Part 4. Engine crashes and the rest

A read-only pass made 2026-09-25 over the parts of TADR's `TABugFix.cpp` (and friends) that are
not about units, orders or combat. Sources: `pristine/TotalA.exe.pristine` (main checkout),
`objdump -d -M intel` (the scratch copy); `vendor/TADR` at `dcff5dd`. Tags: **DIS** = disassembled
here, **SRC** = read in TADR's source or history, **INF** = inferred, with the measurement that
would settle it. Every address below was re-read in the disassembly; none is copied on trust.

### Summary

| # | Item (TADR) | Real stock bug? | Class | TADR's fix by construction? | Recommendation |
|---|---|---|---|---|---|
| 1 | `ZeroingDownloadMenuAlloc`, call at `0x42DD74` | **Yes**: a download file with no sections, or one that does not open, leaves its record's count as heap garbage | load / UI (local crash at load) | Yes: a zeroed block is a by-design initial value | **Already done.** A′2's `dl_alloc` redirects the same call and zeroes the block. Nothing to port; add an empty-file test |
| 2 | `CompositeAABBClamp` `0x458B87` | **Yes**: the build-state box is unbounded against the scratch frame | local draw (the engine still rasterises every unit under our DLL) | **No**: it clamps the header, but the cargo merge `0x4B90A0` has no right or bottom clip | **Superseded by our L7** at the same site. Do not port |
| 3 | `CrashFix004cbed5` hook at `0x4CBE81` | The crash is real: `0x4CBE70` reads past its source | local draw | **No fix**: diagnostics only, it "allows the crash" | Do not port. L7's invariant closes the composite route to this read |
| 4 | `GAFGetCurrentFramePtr` `0x4B7EE0` | Stock never bounds the frame index (DIS). No crash cause is established | local draw | **No fix**: a diagnostic ring | Do not port. Its one useful fact is about Wine's `IsBadReadPtr` (below) |
| 5 | The "Option A/B" note: `0x41B8D0` skips cargo detach for controller type 3 | **No**: the non-owner skip is by design. The owner replicates the detach as message `0x0A` | sim (attach state) | Both options would make a non-owner **broadcast** a detach | **Do not port.** Two engine-map corrections come out of it |
| 6 | `NullLpszPasswordInUpdateGameInfo` `0x4C98FD` | **Not established**: no dangling pointer exists. The host passes `""` | network session (lobby) | The write is harmless; the diagnosis is a guess ("Potential bugfix") | Do not port for now. Our loopback multiplayer already hosts and joins with the stock `""` |
| 7 | Long-path crash (v2025.12.6, the fix is `25a649d`) and the >1024-character log `int 29` | **No**: both are TADR's own `_vsnprintf_s` in its logger | TADR-internal | n/a | Moot. Our tree calls no `_s` CRT function |
| 8 | Print-screen crash (`5d25b80`) | **No**: TADR's own surface wrapper never reset `lpBackLockOn` | TADR-internal | n/a | Moot |
| 9 | `GUIErrorLength` (four byte writes) | **No-op on 3.1**: every target byte is already `0x80` | UI | n/a | Do not port. An unbounded copy nearby is noted |
| 10 | `ShadingFix` `0x45A2EC` | **Yes**, cosmetic: shade row 0 is black, and a negative dot product wraps to rows 27–31 | draw only | A value remap, so yes, but it changes the look | **Moot on Vulkan**: our unit pass picks rows by its own rule. It matters on the GDI lane only |
| 11 | `WindSpeedSync` `0x490C5A` | **Yes, in multiplayer**: each peer gets a different wind, in both timing and value | **sim** (wind-generator energy); multiplayer fairness | **Partly**: TADR's generator is static per process, so the second game desyncs if one peer restarted. TADR's own OTA config ships it **off** | **Port our own version**: a per-game generator seeded from a value every peer shares |
| 12 | `LosTypeShouldBeACheatCode` data byte `0x501DF4` | **Yes**: `+lostype` toggles terrain-blocking line of sight at normal command level | E (anti-cheat), sim-adjacent | Yes: a one-byte data patch | Port with group E, as that byte (`1 → 2`), failing closed |
| 13 | `Save/RestorePlayerColor` `0x454927` / `0x45493C` | **Yes**: message `0x20` overwrites a remote player's whole 0xB9-byte info record, in game too | E (anti-abuse) | **Partial**: it restores the colour only | Defer to E. Our design would refuse the in-game copy |
| 14 | `RemoveSharedResourcesFromTotal` `0x401BAE` | Arguable: received shares are counted as "produced" | statistics / UI | yes | D or E, optional |
| 15–25 | the one-liners (§6d) | no, or TADR-internal, or dead | local / UI / E | — | see §6d |
| 26 | Recorder plugins (Delphi) | four candidate stock fixes, **none active for stock content** | — | — | inventory §7 |

**The three findings that matter most.**

1. **The cargo-detach "Option A/B" is not a stock bug, and porting it would add one.** The owner
   detaches through `0x48AAC0`, which sends message `0x0A`, and every peer applies it in the
   dispatcher at `0x455403`. On a non-owner, `0x41B8D0`'s controller test at `0x41B957` skips the
   detach on purpose. Option B would make every non-owner send a detach of its own as its first
   local player, because `0x44FDB0` picks the sender. The composite crash TADR chased
   (`0x458B87`, `0x4CBED5`) is already closed by L7 at the same site, whatever its cause.
2. **Wind is the one real simulation defect in this cluster.** Stock schedules each wind change
   with CRT `rand()`, seeded from the clock at `0x4971AE`. It draws the new value from the sim RNG,
   which is itself seeded per peer from `QueryPerformanceCounter` at `0x49719D`. That settles the
   engine map's open [INFERRED] on the sim RNG's seed: **the sim RNG is per peer by construction.**
   Wind feeds wind-generator energy (`0x40156F`: ratio × UnitDef `+0x1D2`). TADR's fix works only
   while its process-lifetime generator is fresh.
3. **Half the list is TADR fixing TADR:**
   - the long path and the `int 29` crash (its own logger);
   - print-screen (its own surface wrapper);
   - `NewChatTextGuard` (its own handlers re-entering the network stack);
   - "unit limit reached between missions" (its own ID-recycle queue);
   - the perm-LOS sonar fix (its own replay feature).

   `GUIErrorLength` is a no-op on 3.1. The download-menu zeroing is already ours. The two real
   stock defects that remain belong to group E: `+lostype` at normal command level, and the
   in-game `0x20` overwrite.

---

### 1. `ZeroingDownloadMenuAlloc`: the download-menu block (`0x42DD74`)

**What TADR says (SRC).** `0x42DCF0` allocates one 0xBD-byte record per `download\*.tdf` through
`cmalloc_comt` `0x4D83B0`, which does not zero. A record's count is written only if its file
parses a section, so an empty file leaves garbage for the loop at `0x42DF86`. TADR calls this
"benign on native Windows, fatal under Wine", and says TAF-Twilight ships about 307 empty files.
Its fix redirects the call at `0x42DD74` to an allocator that zeroes.

**What stock does (DIS).**
- `0x42DD58..0x42DD86`: `files × 0xBD` bytes from `0x4D83B0`, count at `[main+0x391C7]`, block at
  `[main+0x391CB]`.
- The count is written only at `0x42DE12` (`mov [ecx+esi],ebp`, which is k+1), once per section.
- A file that fails to open (`0x42DDCB je 0x42DF19`) or has no sections (`0x42DDEA je 0x42DF11`)
  never reaches that store.

The readers:
- The page-count loop `0x42DF72..0x42DFE4` does a signed `cmp [ebx+eax],0; jle` and then walks
  `count × 0x25` bytes. A garbage positive count reads past the block, and an entry whose word
  happens to match writes a byte into a UnitDef at `+0x22E` (`0x42DFB4`).
- The downloadable check `0x42E04B` string-compares every record's `+8` name whatever the count,
  so a garbage name can run off the heap.
- The build menu `0x41AE0F` and the appender `0x42BE30` walk the count.

**A real stock defect?** Yes. How often it fires depends on the heap; TADR's "benign on
Windows" is INF.

**Overlap.** Our A′2 `fix_download_records` (`tagpu_patches.c:1588..1717`) already redirects
**the same call** at `0x42DD74` to `dl_alloc`, which zeroes the block, and zeroes every block it
grows (`dl_section`). Its comment names this case: "so does the count of a file with no sections or
one that did not open". TADR's fix is a strict subset of ours, and porting it would collide on the
site.

**Proposed.** Nothing to port. **Test** (add to the A′2 regression set): a mod folder with a
zero-section `download\EMPTY.TDF`, and one containing only a comment line. `tacli launch` it, reach
skirmish, and open a builder's build menu. Expect no fault, and the
`enginefix: … download menus past five entries (0x42DCF0) armed` line.

### 2. The composite, and the crashes around it

#### 2a. `CompositeAABBClamp` `0x458B87`, against our L7

**What TADR says (SRC).** It hooks `0x458B87` (`mov ecx,[ebx+0x10]; neg eax`), just before the
header gets the box's width and height (`si`, `dx`). It clamps them to the live frame's `CurtX` /
`CurtY` (INI, default 1280) and logs the unit. It calls itself "DEFENSIVE"; the "REAL FIX" would
be Option A or B (§2d).

**What stock does (DIS).** Stock sets the header with no comparison against the frame. Our engine
map ("The composite scratch frame") and L7 hold the full account.

**TADR's safety argument, re-checked.** It is a bound on the header, but not on what follows it.
The cargo merge `0x4B90A0` at `0x4596D8` paints a carried unit into the scratch with no right or
bottom clip, so a clamped box moves the overrun one call later. L7 rejected clamping for exactly
this reason (raised-limits.md, *The composite scratch: grow, then fall back*). The clamp also
bounds `w ≤ CurtX` and `h ≤ CurtY` separately rather than rows against the rasteriser's 800-row
span table (`0x4C8760`), which L7 found faults at `0x4C8035`.

**Overlap.** L7 `fix_composite_scratch` detours **the same bytes** (`SCRSITE` row 2,
`0x458B87`, `8B 4B 10 F7 D8`) into `scratch_build_state`. That check grows the frame or refuses.
**Do not port.** Porting would collide on the site.

#### 2b. `CrashFix004cbed5` (hook at `0x4CBE81`)

**What TADR says (SRC).** It runs at the top of `CopyScreenContext`. It checks the source and
destination contexts and rectangles, `lpSurface` (with `SafeIsBadReadPtr`) and the pitch, logs to
`Errorlog.txt`, and then **returns 0: "Continue execution normally (and allow the crash)"**. The
crash address is `0x4CBED5`.

**What stock does (DIS).**
- `0x4CBE70` is a colour-keyed copy with no clipping at all: `cdecl (dst ctx, src ctx, src RECT*,
  dst RECT*, key)`. `0x4CBED5` is `mov al,[esi]`, the read of the source pixel.
- Callers: `0x4B8103` (the raw arm of `CopyGafToContext 0x4B7F90`), `0x4C6DF6` and `0x4C6E55`.
- `CopyGafToContext` clips to the **destination** (`0x4C6AE0`, `0x4B7E60`). It reads the whole
  source by the frame header's width and height, so a header larger than its planes reads past them.
- TADR's own trace picks out the caller `0x45AE78`, which is `DrawUnit`'s call to `0x458810` at
  `0x45AE73`: the composite path.

**Class and overlap.** A local draw path that still runs under our DLL: "units are never skipped
now", gpu-status. That is the reader L7's invariant covers: "no reader sized by a header leaves its
allocation". TADR's other hypothesis, a freed frame, is a lifetime question. L7 frees the old frame
only after `ctx+0x10` points at the new one, and the frame has no other holder. **Do not port**:
the hook fixes nothing, and it runs `IsBadReadPtr`-style probes on every blit.

#### 2c. `GAFGetCurrentFramePtr` (`0x4B7EE0`)

**What stock does (DIS).** `0x4B7EE0`, `stdcall(state)`, returns `*(seq + 0x28 + frame·8)` with
`seq = state+8` and `frame = u16 state+0`. It tests `seq` for NULL only (`0x4B7EE9`) and **never
bounds the frame against `seq+0` (Frames)**. It has 15 call sites. effects.md already records it as
unbounded.

**What TADR does (SRC).** A diagnostic ring only; it returns 0 in every branch. Its comment
carries one fact worth keeping: under Wine 9, `IsBadReadPtr` catches only `0xC0000005`, and a
probe that lands on a thread-stack guard page (`0x80000001`) escapes as an unhandled exception.
TADR's own crash of 2026-05-04 was its **diagnostic probe** doing exactly that. **INF** for our
Wine: settle it with a test DLL that calls `IsBadReadPtr` on another thread's guard page.

**Relevance to us.** CLAUDE.md already says the check "can swallow a guard page". TADR adds that
under Wine it can **kill the process** instead. We have about 55 `IsBadReadPtr` sites
(`tagpu_gaf.c` 11, `tagpu_scaffold.c` 8, `tagpu_posebake.c` 6, …). That is another reason each one
needs a bound behind it, and it belongs in the standing-debt note.

**Recommendation.** Do not port. If a bound is wanted later, `frame < *(u16*)seq`, else frame 0,
at `0x4B7EE0` is a by-construction fix. Measure the need first: a counting trace at `0x4B7EE0` over
`scenario load 200v200`, counting `frame >= Frames`.

#### 2d. The "Option A/B" note: `UNITS_BroadcastUnitBuildFinished` `0x41B8D0`

**What TADR says (SRC, `TABugFix.cpp:927-936`, `:2172-2180`, commit `75898fd`).** The
`ARMGEO_UPGRADE` composite overflow is attributed to `0x41B8D0` skipping the cargo detach for a
built unit owned by a RemoteHuman (controller 3). The note proposes two fixes:
- **Option A**: after the call, force `AttachUnitToUnitByPieceIdx(built, 0, 0xFF, 1)` when `+0x86`
  is non-zero.
- **Option B**: a 2-byte patch at `0x41B957` that routes controller 3 to the detach at `0x41B990`.

Neither was ever implemented. The note says "implement when the clamp ring confirms", and nothing
in the history does.

**What stock does (DIS).**
- `0x41B8D0(builder, built)`, `ret 8`. It bails unless both units are alive (`+0x110 & 0x10000000`)
  and the builder's def has a build list (`+0x156`). Then it clears `built->+0x9E->+0x10` and
  `+0x104`, and sets `+0x110 |= 0x2000`.
- `0x41B945..0x41B959`: if the owner player (`built+0x96`) is active **and its controller byte
  `+0x73` is 1 or 2** (local human or local AI, `tamem.h:1765`):
  - with the structure bit (`0x20000000`) set, it refreshes `BUILDER.GUI` (`0x4AB060`, `0x49FA90`);
  - otherwise, if `+0x86` (the parent) is set, it calls **`0x48AAC0(built, 0, -1, 1)`**: the detach.
- Controllers 3 (remote human) and 4 (remote AI) skip that block.
- `0x41BA0E..0x41BA26`: under the same controller test it calls **`0x4560C0`**, which sends
  **message `0x12`**: `{0x12, u16 built, u16 builder}` through `0x451DF0`.
- Callers of `0x41B8D0`:
  - `0x4026EE`, `0x402B25` and `0x41BCBF`: the local build paths;
  - `0x4555F1` and `0x455610`: **the `0x12` receiver** (dispatch slot `0x12` → `0x4555BA`, table
    `0x455F84`, with an index of type − 2).

**Attach and detach are replicated by the owner.**
- **`0x48AAC0` is the attach wrapper**, called from 30 sites, `ret 0x10`. After its own guards it
  builds the 7-byte message `{0x0A, u16 child, u16 parent, u8 point, u8 flag}` and sends it with
  `0x451DF0(0x44FDB0(buf, 7), buf, 7)` (`0x48AB47..0x48AB58`). Then it applies the message locally
  through `0x48AB70` (`0x48AB62`).
- **`0x44FDB0`** returns the DirectPlay ID (`+0x1B67`) of the **first local player**: the first
  whose `+0x73` is 1 or 2.
- **`0x451DF0(from, buf, len)`** sends only if `from` names a player whose controller is 1 or 2 and
  whose `+0x22` is 0 (`0x451E8F..0x451EA5`). `0x44FFD0(i)` returns player i's ID, or −1.
- **Every peer applies `0x0A`** in the dispatcher: slot `0x0A` → `0x4553FE` →
  `call 0x48AB70` at `0x455403`.

So on the owner, `0x41B8D0` detaches, which sends `0x0A`, and then sends `0x12`. A non-owner
receives `0x0A` (detach applied) and `0x12` (build finished), and skips both the detach and the
resend by design.

**A real stock defect?** **No** in the form TADR states it.

**The hazard in the fix.** Option B, and Option A as written, call `0x48AAC0` on a **non-owner**.
That sends a `0x0A` under the non-owner's own first local player (`0x44FDB0`), so every non-owner
broadcasts a duplicate detach for a unit it does not own. A duplicate that arrives after the owner
re-attached the unit (a transport pick-up right out of the factory) detaches it on every peer. That
is a divergence created by the fix. Option B also leaves controller 4 (remote AI) out, which is
inconsistent even on its own terms.

**What could still leave a cargo attached on a non-owner (INF).**
- A `0x0A` that is lost, or that arrives when the child is not yet alive on that peer: the guard
  at `0x48ABCF` drops it silently.
- Mod content: TADR's case, `ARMGEO_UPGRADE`, is a mod unit.

Settle it with two peers (`tools/mp_lobby.sh h1 j1`):
1. The joiner's factory builds a unit, and the build finishes.
2. Pause (`tacli keys h1 shift pause`).
3. On both peers, `tacli peek <i> '*511DE8+14357:4'` for the base, then read the unit's `+0x86`
   (`engine_index × 0x118`).

Repeat with a transport loading the fresh unit straight out of the factory.

**Overlap and recommendation.** **Do not port** either option. The symptom TADR reported, a
composite overrun from a runaway cargo box, is already safe under L7 by construction:
`scratch_merge` refuses a cargo past the carrier's frame, and the build-state box grows or falls
back. If a non-owner ever does keep a stale cargo, the fix belongs on the replication side (the
`0x0A` path), not in a non-owner broadcast.

**Engine-map corrections this section produces** (for the documentation pass):
- *The attach packet* section says `0x48AB62` is called "from the wrapper `0x48AB40..0x48AB6A`".
  The wrapper is **`0x48AAC0..0x48AB6A`**: `0x48AABB` is the previous function's `ret 0x14`, then
  `nop nop`. It has **30 callers**. It **sends `0x0A` itself** before applying it, which the map does
  not say.
- New rows: `0x41B8D0`, `0x4560C0` (the `0x12` sender), `0x44FDB0`, `0x44FFD0`, `0x451DF0`'s
  local-sender gate, and the dispatcher table at `0x455F84` (index = message type − 2; slot `0x20` →
  `0x454874`, `0x12` → `0x4555BA`, `0x0A` → `0x4553FE`).

### 3. `NullLpszPasswordInUpdateGameInfo` (`0x4C98FD`, "black screen on join", `553e49a`)

**What TADR says (SRC).** "Potential bugfix for intermittent black-screen on join". Before the
`SetSessionDesc` call in `HAPINET_updategameinfo` it zeroes `desc+0x34` (`lpszPassword`). "The
only dplay-related hook left after full revert of the larger experiments."

**What stock does (DIS).**
- **`0x4C9890`** (update game info) copies the name into `hapi+0`. It writes the session name
  (`hapi+0x48D` = desc `+0x30`) and `dwUser1..4` (`+0x49D..+0x4A9`), sets `dwSize = 0x50`, and calls
  vtable `+0x7C`, which is `IDirectPlay::SetSessionDesc`, on `&hapi+0x45D`. **It never writes
  `lpszPassword` (`+0x491`).** It ignores its third argument (`0x5119B8`). The hook's 5 bytes
  (`6A 00 8B 0E 50`) end on an instruction boundary, and `eax` is the desc there.
- **`0x4C9920`** (host create, from `0x4515BE`) zeroes the desc, sets `lpszPassword = arg3 =
  0x5119B8` (`0x4C99CC`) and calls `Open(DPOPEN_CREATE)`. `0x5119B8` is a `.data` string that is
  all zeros in the file and is referenced about 200 times as a default text: **the empty string
  `""`**.
- **`0x4C9FD0`** (join) zeroes the desc (`lpszPassword = NULL`) and calls `Open(JOIN)`.
- **`0x4C9A70`** (lobby launch) copies the lobby's `DPLCONNECTION->lpSessionDesc` into the desc
  (`0x4C9B38..0x4C9B4F`), so `lpszPassword` points into the lobby buffer `[hapi+0x4D1]`. That
  buffer is allocated at `0x4CA52E` and freed only by `0x4C9B70` (`0x4C9C00`), which first closes
  and releases DirectPlay. The pointer cannot outlive the session.
- Callers of `0x4C9890`: `0x451208` and `0x454135` (in the dispatcher; which message is not
  identified).

**A real stock defect?** **Not established.** No path leaves a dangling password pointer. What is
left is that the battle-room host hands DirectPlay `""` rather than NULL. Whether native `dplayx`
treats `""` as "password required" is **INF**. It cannot be the general cause: our own loopback
multiplayer (native DirectPlay, `modules.md`) hosts through `0x4C9920` with `""` and joins
reliably. TADR's case is a TAF-lobby launch (`0x4C9A70`), which our harness never uses.

**Class.** Network session (lobby), not simulation.

**Safety.** Writing NULL is harmless in itself, but it would also **remove a real lobby password**
on the first update, so a password-protected lobby game would become open.

**Recommendation.** Do not port until it is reproduced.

**Test** (if it is ever ported): `mp_lobby.sh h1 j1` with the host's `SetSessionDesc` argument
logged, and a third instance joining after the host changes the map (which calls `0x4C9890`).

### 4. The long-path crash, the `int 29` log crash, print-screen, `GUIErrorLength`

- **The long path (tdraw.txt v2025.12.6) and the >1024-character log (`25a649d`, same day).** One
  fix. TADR's own `IDDrawSurface::OutptTxt` formatted into `char[1024]` with `_vsnprintf_s(buf,
  sizeof buf, …)` and no `_TRUNCATE`. A long line, such as one carrying a long install path, calls
  the invalid-parameter handler, which ends in `int 29` (fail-fast). The fix is a 16 KB buffer and
  `vsnprintf`. **TADR-internal, not stock.** Moot for us: `tagpu` calls no `*_s` CRT function
  (grep), and our logger truncates with `_snprintf`.
- **Print-screen (`5d25b80`).** One line in TADR's wrapper, `IDDrawSurface::Unlock`:
  `lpBackLockOn = false`. **TADR-internal.** Moot. (Whether TA's own screenshot key works on our
  DLL is a separate one-minute check: `tacli keys t1 …` and look for `SHOT*.PCX`.)
- **`GUIErrorLength` (`0x4AEBBE`, `0x4AEBCA`, `0x4AEC2C`, `0x4AEC87` ← `0x80`).** **A no-op on
  3.1.** Each is the immediate of `cmp ax,0x80` (`0x4AEBBC`), `mov word [ebp+0x62],0x80`
  (`0x4AEBC6`) or `push 0x80` (`0x4AEC2B`, `0x4AEC86`), and every one is already `0x80` in the
  pristine exe (DIS). The patch dates from 2014 (`3ca8be3`) and presumably targeted another build.
  **Do not port.**
  - A real but narrow defect sits next to it (DIS): at `0x4AEC36..0x4AEC5E` the gadget loader reads
    a 0x80-byte string into `[ebp-0x20]`, passes it through the translation lookup `0x4C5740`, and
    copies the result back **with no length bound** (`repnz scasb` + `rep movs`). It overruns into
    the gadget's `+0x60` fields if a translated string is 128 characters or longer. The table is
    `[0x51FDB8]`, loaded once from WinMain (`0x49EA33` → `0x4C54F0`). **INF**: whether any stock
    install loads a table. Class UI. It is not worth a patch unless a mod ships one.

### 5. `ShadingFix` (`0x45A2EC`, "black model faces", `a3a1b7e`)

**What stock does (DIS).** In the 2× bake `0x459C70`'s vertex loop, `0x45A2B6..0x45A2F1` computes
`row = ftol(dot(N, L) · 5.0) & 0x1F` when the piece's shade bit is set, else 15 (build-state.md
records this). So:
- a face whose `|N·L| < 0.2` gets **row 0**, which is black;
- a negative dot product **wraps to rows 27..31**;
- the row indexes the engine's 32 × 256 shade table in the lit rasteriser `0x4C8020`.

**TADR's fix (SRC).** At `0x45A2EC` (`and eax,0x1f`), a row of 0 becomes 5 (the ini key
`ShadingZeroFallbackLevel`).

**Class.** Draw only. It touches no simulation state.

**Under our renderer.** Moot on Vulkan. Our unit pass picks its own row, `neutral + dir ·
round(12 · N·SH_L)` clamped to 0..31 (`tagpu_render3do.c:66`, `tagpu_posedraw.c`), and shades by
the fitted multipliers `k[row]`. So neither the black row nor the wrap reaches the screen. It still
shows on the GDI lane (the engine's own frame).

**Recommendation.** Do not port. On the GDI lane the wrap to 27..31 is the larger artefact; the
owner keeps that lane stock (decided below).

### 6. The rest of the constructor

#### 6a. `WindSpeedSync` (`0x490C5A`): a real multiplayer defect

**What stock does (DIS).** The wind updater is `0x490C40`. It is called at `0x491903` (level load,
right after `+0x37EC8 = 5000` and `+0x37EC4 = esi`) and at `0x49558F` (the per-tick chain in
`0x495490`).

1. If `next [main+0x37EC4] >= GameTime [main+0x38A47]`, it clears the changed flag `+0x37EE2` and
   returns (`0x490C51`).
2. `next += 30 · (5 + rand()·10/0x8000)`, using **CRT `rand` `0x4E4870`** (`0x490C60..0x490C8E`).
3. `speed [+0x37EDA] = min [+0x1425B] + simrand(max [+0x1425F] − min)`, using **the sim RNG
   `0x4B6C30`** (`0x490CA4`).
4. If the speed is not 0, `dir [+0x37ED8] = simrand(0x10000)` (`0x490CD1`).
5. The components `+0x37ECC` and `+0x37ED4` come from `dir` and `speed`
   (`0x4B70EF`/`0x4B7123`).
6. `ratio [+0x37EDE] = speed / [+0x37EC8]`, capped at 1.0, and `+0x37EE2 = 1`.

**The two seeds (DIS). This settles the engine map's open [INFERRED].** At level load `0x497180`
calls **`QueryPerformanceCounter`** (IAT `0x4FC0BC`), then **`0x4B6CA0(low + high)`**, which sets
the sim RNG state `0x51FC88 = (seed ^ 0x66E29572) | 1`. It then calls `srand(time(0))`
(`0x4971A5`/`0x4971AE`), which seeds only the loader thread's CRT stream: `rand` keeps its state
per thread (`+0x14` of `0x4EB0F0`'s data), and the wind's `rand()` at `0x490C60` runs on the game
thread, whose stream WinMain seeds with `srand(time(0))` at `0x49E8BB`. **Both RNGs are seeded per
peer.** So stock never intended the sim RNG
to agree across peers, which fits the state-and-event replication model. It also means stock's wind
differs per peer in both **when** it changes and **what** it changes to.

**Who reads the wind (DIS).**
- `0x40156F`: the per-unit resource tick adds `ratio × UnitDef+0x1D2` (WindGenerator) into the
  unit's energy make `+0xBC`.
- `0x488F68`: a unit's energy-make getter, which returns `−ratio × +0x1D2`.
- `0x437910` (`0x437933`/`0x437941`/`0x437973`), called from `0x48ADC4`: on a tick where the
  changed flag is set, a wind generator's scripts `SetDirection(heading)` and `SetSpeed(speed << 4)`.
- The projectile pass `0x49B720` (the tick's call at `0x495513`) adds the component vector from
  `+0x37ECC` to a projectile's position every tick (`0x49BC58..0x49BC62`, `0x49BD04..0x49BD2D`).
- The fire spread `0x4239C0` (from `0x424463`) reads both components (`0x423AA1`, `0x423AC1`).
- The effect handlers at `0x474B09`, `0x474FC9`, `0x475366` and `0x475626` read both components
  [INFERRED: smoke and particle drift].
- `0x409B90` reads `+0x37EC8` (5000) against the map's maximum wind, not the drawn wind.

The projectile pass and the fire spread make the wind simulation state, beyond the economy.

**A real stock defect?** **Yes, in multiplayer.** Economy is owner-authoritative: resources travel
as `0x28` (networking-lobbies.md). So each player's wind generators produce by the **wind on their
own machine**, and players get different wind. That is unfair rather than a divergence of shared
state. **INF**: whether any peer recomputes a remote player's wind income locally. Settle it with
two peers and wind generators only: pause, then compare each player's energy production
(`PlayerRes +0x04`) on both peers.

**TADR's safety argument, re-checked (SRC).**
- It hooks `0x490C5A`, replaces both the schedule and the values with a
  `std::default_random_engine` seeded from the host's DirectPlay ID (clock fallback), and jumps to
  `0x490CE8`.
- It is deterministic across peers only if:
  - (a) every peer runs `0x490C40` at the same `GameTime` values, which holds since it is tick-driven;
  - (b) the generator starts fresh each game. **It does not**: the generator is a function-static
    `unique_ptr` created once per **process**. A second game in the same process continues the
    sequence, and a peer that restarted TA between games has a fresh one. That is a wind desync
    by design;
  - (c) every peer builds the same generator. The same build and the same MSVC STL give that.
- It no longer draws the sim RNG for wind. Since the sim RNG is per peer anyway, nothing that was
  agreed is lost.
- **TADR's own OTA config ships it off** (`config_ota.h:91 WIND_SPEED_SYNC 0`; on in every mod
  config).

**Class.** Simulation (economy), multiplayer.

**Proposed design.**
- Our own generator: a specified Park–Miller or xorshift with **its own state**, never the sim RNG
  or the CRT.
- **Reset at every level load** at `0x491903`, before the first call, from a value every peer
  shares: the host's DirectPlay ID, mixed with a hash of the map name (built instead from
  DirectPlay's session instance GUID: the host's ID can change during the load, see the plan's B6
  deviations). Single player takes the same
  path, seeded from the counter stock seeds its RNG with, so it stays random per game (decided).
- It replaces both the schedule draw and the two value draws. They run on the game thread only:
  the tick's call is there, and the level load's call, on the loader thread, finds `next` = 0 and
  GameTime 0 and draws nothing.
- Bound as stock does: `max ≤ min` gives `min`. Stock's `n < 2` path returns 0.
- Written once and fail-closed per rule 3, with no runtime opt-out per rule 4; the comparison is
  the previous build's DLL.

**Test.**
1. `mp_lobby.sh --map <a map whose wind range is not zero> h1 j1`.
2. Pause both peers at the same `GameTime`.
3. Peek `*511DE8+37EDA:4`, `+37ED8:2` and `+37EC4:4` on both.

Stock differs; ours must be equal. Then run two games in a row in the same processes, and a third
after restarting only `j1`: all must be equal.

#### 6b. `LosTypeShouldBeACheatCode` (data `0x501DF4`, `1 → 2`)

**What stock does (DIS).** The chat-command table is `{name*, fn*, level}` at 12 bytes an entry
(decoded around `0x501D80`: `CDStop`, `Sound3D`, `Shading`, … `LOSType` @ `0x501DEC`, `Light`,
`Logo`, …). `LOSType` → `0x416690`, at **level 1 (normal)**. That handler toggles **bit 2** of
`LosType [main+0x14281]`, terrain-blocking raycast line of sight (terrain-depth.md), and calls
`Game_SetLOSState 0x4816A0`.

**A real stock defect?** Yes, as an anti-cheat gap. In a true line-of-sight game, any player can
drop terrain blocking for themselves without cheats. Line of sight is per player, and the owner is
authoritative for its units' decisions, so this is sim-adjacent.

**Class.** E.

**TADR's fix.** A one-byte data patch, safe by construction.

**Recommendation.** Port it with group E as that byte, failing closed (the byte must read 1). Our
harness never uses `+lostype` (grep of `tools/`, `.claude/`).

**Test.** Two peers. On `j1`, chat `+lostype` and peek `*511DE8+14281:2` before and after. Stock
toggles `0x4`; ours leaves it unless cheats are on.

**Decided 2026-09-25.** Deferred to group E, with the `0x20` overwrite.

#### 6c. `SavePlayerColor` / `RestorePlayerColor` (`0x454927` / `0x45493C`)

**What stock does (DIS).** The handler for message `0x20` ("player info/status", slot → `0x454874`)
finds the sender. If the sender is **controller 3** (`0x45491A`), it copies **0xB9 bytes** of the
message (`rep movsd ×0x2E` + `movsb`, `0x454934..0x45493B`) over that player's `PlayerInfo`
(`+0x1B8A`), then calls `0x450980`. There is **no in-game gate**.

**TADR (SRC, `36a5d39`).** It saves the colour byte before the copy and restores it after, in game
only.

**A real stock defect?** Yes (anti-abuse): a peer can rewrite its own player record mid-game.
TADR's fix is partial. It covers the colour only, while the rest of the record is still replaced.
Which fields matter to the simulation is **INF** (tapacket's `statuspackets.txt` has the layout).

**Class.** E.

**Recommendation.** Defer to E. The by-construction form is to refuse the whole copy in game (or
keep only the fields a status message legitimately updates), not to restore one byte.

#### 6d. The one-liners

| Item | Address (DIS) | What it is | Class | Verdict |
|---|---|---|---|---|
| `RemoveSharedResourcesFromTotal` | `0x401BAE` (end of the per-player resource tick, `edi` = player, `+0xEC` = `resourcesShared`) | subtracts received metal and energy from `fTotal*Produced` | statistics / score | optional, D/E; not a defect of the simulation |
| `MultiplayerVictorySound` | `0x46A182` (inside the victory draw) | plays "Victory Condition" in multiplayer, throttled to 10 s, from a draw loop | audio | D, a feature |
| `SoundInstanceLimit` | `0x4CF582` (bytes `89 74 24 18 89 6C 24 10` match) | drops a repeat of the same sound within 50 ms | local audio | D, a feature; separate from A's 32 channels |
| `CrackCd` / `CrackCd2` / `CrackCd3` | `0x41D4CD` (`je` → NOP), `0x50289C` (1 → 0), `0x41D6B0` (returns `'.'`) | no-CD: skips the CD archive search | local, DRM | moot on the Steam install |
| `SinglePlayerStartButton` | `0x456780` (`cmp [eax+0x73],3; jne` → `cmp …,2; jge`) | counts AI and remote seats, so the host can start with one human + AI | E, lobby | E |
| `CDMusic_*` | `0x460E0D` (NOP a 10-byte `push 1 … call 0x4CE910`), `0x490B30`, `0x4996DF` | CD-audio pause and resume for TADR's `audiere` music | local | moot |
| `DisplayModeMin*` | `0x45E589`, `0x42FA97`, `0x42FA83`, `0x42FA42`, `0x42FA2E` | an ini option forcing a 1024×768 minimum | local | moot: `impure.cfg` owns the mode |
| `ResourceStripHeightFix` | `0x469078` (after `0x4B7F30`) | clamps the resource-strip frame's height to the game screen's top − 1 by **writing the shared GAF header** | UI draw | minor, D. Whether the Vulkan UI shows the overdraw is INF: capture the top bar |
| `NewChatTextGuard` | `0x463CA0` | refuses chat printed from a staging buffer refilled by **TADR's own** handlers sending mid-dispatch | TADR-internal | moot. For A′2's `0x05` companion: stock `0x451DF0` → `0x4C97B0` → `IDirectPlay::Send` shows no path back into the receive loop `0x44F9C0` at the depths read (INF beyond that). If one is ever found, the by-construction guard is to jump to `0x455F50` after a consumed companion instead of falling into `0x45522E` |
| `Enter/LeaveDrawPlayer_MAPPEDMEM`, `Enter/LeaveUnitLoop` | `0x467440`, `0x465572`, `0x464F80`, `0x46563B`, `0x4655F4` | addresses only. `EnterProc`/`LeaveProc` are **never installed** at `dcff5dd` | dead in TADR | nothing |
| "unit limit reached between SP missions" | `d6066f5`, `e6e4fe6` | TADR's own ID-recycle queue (`FixFactoryExplosions*`) not reset per game | TADR-internal | a lesson for the units cluster: any per-game table **resets at level load** |
| "non-functional perm-LOS sonar" | `e1bff7a`, `TenPlayerReplay.cpp` `0x46754A` | TADR's own replay-watcher feature | TADR-internal | moot |

### 7. Recorder plugins (Delphi, `src/Recorder/plugins`): stock-defect claims only

Registration is in `src/Recorder/Plugins.pas:52-104`. Features and limits are left out.

| File | Claim | Address | Active? | One line |
|---|---|---|---|---|
| `KillDamage.pas` | "fix veteran clone bug" | hook `0x489C2F` (inside the veterancy damage scale at `0x489C05..0x489C34`, DIS) | **no**: commented out at `Plugins.pas:45,81` | A veteran's damage reduction also scales the engine's kill damage, so a unit near the constant's HP survives a "kill" (give-unit): a plausible stock sim defect for the combat cluster |
| `TAExceptionsLog.pas` | "prevent crash when pointer to COB is nil"; cancel order with no unit | `0x4B0BC0`, `0x4B0A70`, `0x4B094D` (commented out); `0x43A264`, `0x43A227` | **no**: the unit is not registered in `Plugins.pas` | the order-cancel NULL-unit sites are for the orders cluster |
| `Transporters.pas` | "Ground transporter overload fix" | `0x406789` (`test eax,eax; je 0x4068CB` in a load routine, DIS) | only when `ModId > 1` | refuses a load when load + 1 > `TransportCap`: a stock capacity defect for the units cluster to verify |
| `SaveGame.pas` | "Load game fix for units with no scripts"; "struct size fix" | `0x4875F9` (commented out); `0x4B207A`, `0x4B20AC` (only with `ScriptSlotsLimit`) | no, or feature-coupled | the size fixes follow TADR's own script-slot expansion |
| `UnitActions.pas` | "fix unit y pos for surfacing subs" | `0x48A8B9` | only when `ModId > 1` | applies TADR's own `ForcedYPos` custom field: a feature, not a stock fix |

### Engine-map facts to write down (documentation pass)

- `0x41B8D0`: build finished, `(builder, built)`; the owner-only detach and the `0x12` send; its
  five callers.
- **`0x48AAC0..0x48AB6A` is the attach wrapper** (the map says `0x48AB40`). It has 30 callers and
  sends `0x0A` via `0x44FDB0`/`0x451DF0` before applying it.
- `0x44FDB0` (the first local player's ID), `0x44FFD0(i)` (player i's ID or −1), and `0x451DF0`'s
  gate (it sends only from a local-controller player).
- The dispatcher table `0x455F84` (index = type − 2) and the slots named above.
- **The sim RNG seed: `0x497180` → `QueryPerformanceCounter` → `0x4B6CA0(lo + hi)` →
  `0x51FC88 = (s ^ 0x66E29572) | 1`, then `srand(time(0))`.** This replaces "still unread".
- The wind updater `0x490C40` and its fields `+0x37EC4..+0x37EE2`, and its readers `0x40156F`,
  `0x488F68` and `0x437933`.
- The chat-command table around `0x501D80` (`{name, fn, level}`); `LOSType` → `0x416690` toggles
  `LosType` bit 2.
- `0x4C9890`, `0x4C9920`, `0x4C9A70`, `0x4C9FD0`, `0x4C9B70`: the session descriptor at
  `hapi+0x45D`, and who writes `lpszPassword`.
- `0x4B7EE0`: no frame bound. `0x4CBE70`: no clipping, reads the whole source.
- The unbounded translated-string copy at `0x4AEC36..0x4AEC5E`.

### Decided 2026-09-25

1. **Wind (§6a).** Ported, reset at every level load, one path in single player and in network games ([the plan, B6](sim-fixes.md#the-landings)).
2. **`+lostype` (§6b)** and 3. **the `0x20` overwrite (§6c)** go to group E.
4. **The password hook (§3)** stays parked: neither reproduced nor proven.
5. **`ShadingFix` (§5).** The GDI lane stays stock.
6. **`IsBadReadPtr` under Wine (§2c).** Recorded in the notes (here and the engine map), not in CLAUDE.md.
