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

| Limit | Stock | Patch |
|---|---|---|
| active projectiles | 300 | 3000 |
| explosion effects | 300 | 3000 |
| flying model-piece slots | 100 | 1000 |
| aux debris / effect records | 300 | 3000 |
| units per player | 250 | 1500 (ESC ships 1000) |
| pathfinding cycles | 1333 | 66650 |
| special-effects vector | 400 | 20480 |
| unit-type IDs | 512 | 16000 |
| weapon IDs | 256 | 4096 (ESC on) |
| model composite buffer | 600² | 1280² |
| simultaneous sounds | 8 | 128+ |

### B. Simulation bug fixes

~15 fixes: resurrection wireframes, "units exploding in factories" (deceased-ID recycle held 5 s),
ghost commander, area-damage overflow, grid-claim tie-break, ctrl-Z/A/B/C truncation of IDs ≥ 512,
black-screen-on-join, zero-shade faces, and the rotated-unit / staircase-yardmap / long-path /
print-screen / spawned-command crash fixes — plus the four escalation-only rules Wotan requested
(repair-rate fix ×3/×3, share-abuse guard, aircraft-wreck fall, 32-tile off-map AA).

### C. New data keys

Weapon TDF flags (`nottoair`, `nottounderwater`, `notoverwater`/`notoverland`, `surfacefire`,
`nomapweaponalert`, `reloadbar`); unit FBI keys (`VeterancyThresholds`, `VeterancyAccuracyBuffRate`,
`TransportedExplodeAs`/`TransportedSelfDestructAs`, `Rotations=`, `PreviewPieces=`/`PreviewObject3D=`/
`PreviewFaceOpponent=`); weapon `ID=` 0–4095. The mechanism is the portable one: hook the engine's
own TDF reader (`0x4C46C0`/`0x4c4760`/`0x4C48c0`) — never write a parser.

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
tagpu GL renderer as its `ddraw.dll` backend (the slot cnc-ddraw fills today). tdraw does game logic
plus its own UI through DirectDraw surfaces → our GL; its `DllMain` and ours both patch, at disjoint
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

## Sources

`github.com/tanvanman/TADR` — `src/DDraw/tdraw.txt`, `config.h` + `config_*.h`, the module set under
`src/DDraw/`, `src/Recorder/Plugins.pas`, `src/Recorder/plugins/*.pas`,
`src/Recorder/{Dplayx_exports,PluginEngine,InitCode_CoreExePatching}.pas`, `src/Server/`.
Cross-references: [deep-tadr.md](deep-tadr.md), [deep-ta-esc.md](deep-ta-esc.md),
[binary-patches.md](binary-patches.md), [runtime-injection.md](runtime-injection.md),
[project-map.md](project-map.md).
