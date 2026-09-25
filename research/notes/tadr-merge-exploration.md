# TADR merge exploration

Source read: `github.com/tanvanman/TADR`, local clone (2026-09-07); line counts measured from the
tree, not the release binaries. Scope: what the DLL TA: Escalation ships actually patches into the
game, how large the two halves of the TADR repo really are, and the options for folding that work
into the tagpu stack without adopting its renderer or recorder wholesale.
`[VERIFIED]` = read in the source; `[CLAIMED]` = asserted in in-repo prose, not re-verified here.

## The patch stack and the boundary

TA: Escalation authors no engine patch. It ships the community stack renamed. The game-patch DLL —
`TAESC.dll`, byte-identical to upstream `tdraw-escalation.zip` v2026.8.6 — is the subject; its
neighbours are out of scope per the brief. [VERIFIED]

| Binary ESC ships | What it really is | In scope? |
|---|---|---|
| `TAESC.dll` | `tdraw.dll` (TADR, escalation build) — the game patch | yes — the subject |
| `eplayx.dll` | `tplayx.dll` (TA Demo Recorder, Delphi) | excluded — but see "a second engine patch" below |
| `ddraw.dll` | `cnc-ddraw` (FunkyFr3sh) — the DirectDraw re-implementation | excluded |
| `TotalA.exe` | hex-edited: import renames + data-path renames + 3.9.02 patches | excluded |
| `emusi.dll` | "TA Music Player" — CD→MP3 shim | out of scope |

The byte-level proof of the rename is in [deep-ta-esc.md](deep-ta-esc.md); the lineage is in
[project-map.md](project-map.md).

## tdraw.dll — the game patch

The C++ half (`src/DDraw/`) is ~40 feature modules in six groups. Every limit, address and flag
below is quoted from `tdraw.txt` / `config_escalation.h` / the module source. [VERIFIED]

### A. Raised ceilings

**Ported: every limit below is raised in our stack** (2026-09-24). *Ours* is the value we ship
and *Landing* where it landed: L1–L7 in [the section-A plan](tadr-port/raised-limits.md), A′2 and
A′3 in [A′. Content IDs](tadr-port/content-ids.md). Two values differ from TADR's on purpose, in
bold.

| Limit | Stock | Patch | Ours | Landing |
|---|---|---|---|---|
| active projectiles | 300 | 3000 | 3000 | L1 |
| explosion effects | 300 | 3000 | 3000 | L1 |
| flying model-piece slots | 100 | 1000 | 1000 | L1 |
| aux debris / effect records | 300 | 3000 | 3000 | L1 |
| units per player | 250 | 1500 (ESC ships 1000) | 1500, the default and the ceiling | L2 |
| pathfinding cycles | 1333 | 66650 | 66 650 | L2 |
| special-effects vector | 400 | 20480 | 20 480 a layer | L3 |
| unit-type IDs | 512 | 16000 | **16 384**: the type masks are 2 KB, one bit a type | A′2 |
| weapon IDs | 256 | 4096 (ESC on) | 4096 | A′3 |
| model composite buffer | 600² | 1280² | 1280², bounded by area and grown up to 2048² | L4, L7 |
| simultaneous sounds | 8 | 128+ | **32**: the engine tracks 32 playing sounds, and a 33rd plays untracked | L4 |

We also raise one limit TADR leaves at stock, the wreck pool (2048 → 8192, L6), with two fixes to
how a reclaim pays: [Fixed beyond TADR](tadr-port/raised-limits.md#fixed-beyond-tadr). What stays
open is in the plan's [open questions](tadr-port/raised-limits.md#open-questions).

### B. Simulation bug fixes

TADR's list reads as ~15 fixes. Checked against the retail exe (2026-09-25,
[the evidence pass](tadr-port/sim-fixes-evidence.md)), each item is one of four kinds, and **only
the first is ported as B** ([the plan](tadr-port/sim-fixes.md)):

- **Stock defect**: a defect in the 3.1 engine's own code, in any subsystem.
- **TADR's own feature**: a fix to code only TADR has (rotation, its logger, its map spawns).
- **Gameplay change**: a rule change presented as a fix.
- **Diagnostics**: crash reporting, observe-only guards, and code that is dead or a no-op on 3.1.

The items below are the ones this section's summary named; the evidence page covers the rest of
`TABugFix.cpp` and the recorder's plugins.

#### Stock defects

- **"Units exploding in factories"** (`FixFactoryExplosions`). A damage message names its victim by
  slot, and the owner reuses a freed slot at once, so a late hit kills the new nanoframe. TADR holds
  freed slots 5 s, which is timing. **Ported, redesigned**: an incarnation on the wire and a
  two-tick hold (B4).
- **The ghost commander** (`GhostComFix`). A remote unit whose first update arrives before it exists
  is created at (0,0) and corrected only after N ticks. **Ported, redesigned**: the cause measured,
  then an ordering (B5). TADR's Assist would morph the commander and is not ported.
- **Area-damage overflow** (`AreaDamageOverflow`). Two defects: past 20 units (and past 64
  features, which TADR misses) a victim is hit once per footprint cell; and aircraft stacked on one
  cell take no splash. **Ported, redesigned** (B1, B2).
- **Resurrection wireframes.** The failure branch is stock, but no stock trigger was found. **Measured,
  else parked.** TADR's fix writes into grid cells it has not checked.
- **Ctrl-Z/A/B/C truncating IDs ≥ 512.** **Already done** by A′2's 2 KB masks.
- **The crash fixes that are stock**: flak's divide by zero (TADR only logs it) and the stockpile HUD
  divide. **Ported** (B1, B6).
- **Wind**, per peer in stock. **Ported, redesigned**: one wind a game (B6).
- **Two stock defects TADR carries but its summary does not name**: a yardmap read past its string
  (TADR's NULL would crash six readers; **ported, redesigned**, B6), and the download-menu blocks
  left uninitialised (**already done** by A′2).

#### TADR's own features

- **The rotated-unit crash fixes** (staircase yardmap, the builder inside a rotated footprint, the
  return stack) and **the resurrection fix's rotation half**. We have no unit rotation.
- **The long-path and print-screen crashes**: TADR's own logger and surface wrapper.
- **The spawned-command crash**: TADR's own map-spawn feature passed the wrong context.

#### Gameplay changes

- **The grid-claim tie-break.** Written for lockstep; under state replication the firer's peer
  decides every hit, and the change alters single-player behaviour. Not ported.
- **The four escalation rules**: the repair rate (stock heals a flat 1 HP a call per repairer, the
  game's rule), the share guard (group E), aircraft wrecks falling (no stock flyer leaves a wreck;
  the cargo path is measured), and off-map anti-air. Under the off-map margin sit two real edge
  defects, an off-by-one and a line-of-sight shear, which are **ported** (B1); the margin is not.

#### Diagnostics, and items that did not hold

- **Black screen on join**: a "potential" fix with no mechanism found. Parked.
- **Zero-shade faces**: a real draw defect, moot on Vulkan; the GDI lane stays stock.
- **The cargo-detach "Option A/B"**: not a defect, since the owner replicates the detach; porting it
  would add one.
- The order-dispatch guard, the crash rings, the player-lost guard: observe-only, dead, or a no-op.

### C. New data keys

Weapon TDF flags (`nottoair`, `nottounderwater`, `notoverwater`/`notoverland`, `surfacefire`,
`nomapweaponalert`, `reloadbar`); unit FBI keys (`VeterancyThresholds`, `VeterancyAccuracyBuffRate`,
`TransportedExplodeAs`/`TransportedSelfDestructAs`, `Rotations=`, `PreviewPieces=`/`PreviewObject3D=`/
`PreviewFaceOpponent=`); weapon `ID=` 0–4095. The mechanism is the portable one: hook the engine's
own TDF reader (`0x4C46C0`/`0x4c4760`/`0x4C48c0`) — never write a parser.

**Planned 2026-09-25** in [C. New data keys](tadr-port/data-keys.md), after
[an evidence pass](tadr-port/data-keys-evidence.md): four landings, TADR's key names and documented
meanings. `ID=` landed with [A′3](tadr-port/content-ids.md). `Rotations=` and `reloadbar` go to
group D with the features they switch on.

### D. UI / quality-of-life

Megamap, nanoframe ghost preview, building rotation, Mex/WreckSnap, drag-queued orders, con-unit
behaviour (hold-position = reclaim-only; stay-put after build; auto-kickout), weather report, visible
map DTs, unit status counters, team-coloured nanolathe, reload bars, accessible chat, unicode, TA
Hook + whiteboard, HUD polish.

### E. Multiplayer / anti-abuse

Start positions by team, `+autoteam`/`+randomteam`, vote-to-reject, share guard, `.take`
arbitration, lag-switch guard, map unit spawns, CRC reports, challenge-response anticheat, lobby
conveniences (start with 1 player + AI, `.noshake`/`.ready`/`.autopause`, 10-player replay).

### The escalation delta

Escalation = mainline ("prota") **minus mex snap**, **plus** share guard, repair-rate fix, air-wreck
fall, 32-tile off-map AA, and extended weapon IDs. Six knobs differ; the other ~30 are identical.
[VERIFIED]

## The recorder is a second engine patch

The thing the brief sets aside as "the recorder" is not a recorder. `src/Recorder/` (`tplayx.dll`)
is about the same size as the whole patch, and only a slice of it records demos. [VERIFIED]

| Component | Files | Lines |
|---|---|---|
| `src/Recorder/` total | 89 | 67,333 |
| − third-party (Synopse mORMot) | 2 | −32,257 |
| recorder-authored ≈ | ~87 | ~35,000 |
| └ plugins/ (engine-patch layer) | 56 | 16,212 |
| └ TAMem/ (the TA memory map) | ~8 | ~6,400 |
| └ network (DirectPlay + lobby + packets) | ~10 | ~10,000 |
| └ commands / replay / misc | ~13 | ~3,000 |
| `src/Server/` (replayer, Server.exe) | 20 | 13,092 |

For scale, `src/DDraw/` (`tdraw.dll`) is 219 files / 66,752 lines — **the two DLLs are within 1% of
each other.**

The recorder's ~35 plugins cluster as: limits (`UnitLimit`, `WeaponsExpand`, `MultiAILimit`), the
`ShieldRange` engine shield (`UnitInfoExpand` + `UnitSearchHandlers` + `SideDataExpand`), LOS
(`LOS_extensions` + 6 `LOS_*`), scripting (`COB_extensions` ~200 opcodes, `MaxScriptSlots` 64 slots,
`ScriptCallsExtend`), orders/gameplay (`OrdersOverride`, `UnitActions`, `Transporters`,
`WeaponAimNTrajectory`), con-units (`Builders`, `NanoFrameUnits`, `StartBuilding`), UI
(`GUIEnhancements`, `BattleRoomEnhancements`, `SkirmishEnhancements`, `ClockPosition`, `Colors`),
input/clock (`SpeedHack`, `PauseLock`, `InputHook`, `KeyboardHook`), data (`MapExtensions`,
`GAFSequences`, `ExtensionsMem`), and infra (`IniOptions`, `RegPathFix`, `StatsLogging`, `SaveGame`,
`ErrorLog_ExtraData`, `Thread_marshaller`, `Developers`). Plus the non-plugin network/replay slice:
the DirectPlay proxy, `.take`/`.give`, `Voting.pas`, `.tad` recording. [VERIFIED]

## Where the two DLLs overlap

Four features are implemented in both, and the network seam is split between them. [VERIFIED]

| Feature | tdraw (C++) | recorder (Delphi) |
|---|---|---|
| unit limit | `LimitCrack.cpp` (4-byte) | `UnitLimit.pas` (2-byte) |
| weapon-type limit / ID 256→4096 | `WeaponIdOverflow.cpp` | `WeaponsExpand.pas` |
| weapon-ID network transmit (≥256) | `WeaponFiredExt.cpp` | `WeaponsExpand.pas` |
| per-weapon TDF tags | `WeaponTags.cpp` + `NotToAir.cpp` | `WeaponsExpand.pas` |

The seam: the **recorder** owns the DirectPlay proxy (`dplayx→tplayx`) and the actual `.take` unit
transfer (`Misc_Commands.inc`); **tdraw** owns the CHAT_05 hijack channel (`PacketChatRouter` +
`ChatHijackIds.h`), vote-to-reject transport (msgId `0x2c`), and battleroom command dispatch. Voting
is split by kind, not by DLL: vote-to-*reject* is tdraw's (`VoteDialog`/`VoteReject`);
ready-to-*unpause* (`.ready`/`.voteready`/`.votego`) is the recorder's (`Misc_Commands.inc` +
`Voting.pas`). [VERIFIED]

**Why it's in both:** `TotalA.exe` imports three Cavedog-era DLLs (`ddraw`, `dplayx`, `winmm`), and
each was hijacked as an independent entry point by a different community in a different language.
Each proxy had to be self-sufficient — the recorder shipped alone next to vanilla TA for a decade
before `tdraw` existed, so it bundled its own limit/LOS/COB patches rather than depend on a DLL that
wasn't there. The two merged into one repo (~2019–23) but never into one program; nobody reconciled
the overlap because both copies are live and each mod toggles its own independently. See
[deep-tadr.md](deep-tadr.md).

## Merge & rewrite scope

Three ways to get TADR's feature set into the tagpu stack.

**Way 1 — use both at once (chain).** Point the exe's import at `tdraw.dll`; let it chain-load our
tagpu renderer as its `ddraw.dll` backend (the slot cnc-ddraw fills today). tdraw does game logic
plus its own UI through DirectDraw surfaces → our renderer; its `DllMain` and ours both patch, at disjoint
addresses (we own draw leaves; TADR owns sim tick / loaders / combat). One swap, no fork.

**Way 2 — merge into one DLL (vendor).** Compile TADR's self-contained sim/engine modules (all
`#if`-gated `Install()`/`Shutdown()`) into our tree under MIT, reusing `tamem.h` and `hook/`. One
proxy, one `DllMain`, one patch-address registry.

**Way 3 — rewrite it ourselves (review-and-improve each feature).** Tractably small, because the
expensive part is the render-side UI we already replace, and the sim/engine set is ~14 small +
~11 medium hook/patch modules. The infrastructure is already ours — see [gpu-status.md](gpu-status.md)
and [exe-reverse-engineering.md](exe-reverse-engineering.md). Exclude the megamap throughout.

**Conflict audit (before either way):** the dual-proxy roles; the `EngineLimits` projectile/explosion
pool relocation (our `tagpu_fx`/`tagpu_native` read those pools by pointer and must follow it);
UI ownership (our G15 vs TADR's megamap); Class-B patches that must ship identically to every player;
`tamem.h` struct pins vs our own offsets; config-key collisions; the MIT chain.

**Recommendation:** prove it with Way 1 (one swap, full fidelity), then graduate the sim/engine
features into Way 2 or Way 3 for long-term ownership. Rewriting (Way 3) is where the four
duplications and the recorder's 40 features collapse into one owner each — but it means inventorying
the Delphi side too, not just `tdraw.dll`.

**Taken: Way 3.** [The TADR port](tadr-port/overview.md) rewrites the features group by group, under
standing rules the owner set on 2026-09-23 — TADR's multiplayer behaviour, fail-closed installs, no
runtime opt-out for anything that changes the simulation. Group A is done, in
[the section-A plan](tadr-port/raised-limits.md) and [A′. Content IDs](tadr-port/content-ids.md);
group B is planned in [B. Simulation bug fixes](tadr-port/sim-fixes.md) and group C in
[C. New data keys](tadr-port/data-keys.md); groups D and E are not planned yet. The state of
every group is in [the port's groups](tadr-port/overview.md#the-groups).

## Sources

`github.com/tanvanman/TADR` — `src/DDraw/tdraw.txt`, `config.h` + `config_*.h`, the module set under
`src/DDraw/`, `src/Recorder/Plugins.pas`, `src/Recorder/plugins/*.pas`,
`src/Recorder/{Dplayx_exports,PluginEngine,InitCode_CoreExePatching}.pas`, `src/Server/`.
Cross-references: [deep-tadr.md](deep-tadr.md), [deep-ta-esc.md](deep-ta-esc.md),
[binary-patches.md](binary-patches.md), [runtime-injection.md](runtime-injection.md),
[project-map.md](project-map.md).
