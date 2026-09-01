# Networking, Lobbies & Multiplayer Extensions for Original TA

## Summary

Original Total Annihilation (Cavedog, 1997) is a DirectPlay peer-to-peer game. Everything that keeps it
online in 2026 works by *wrapping* DirectPlay rather than replacing the engine's netcode:

- **TA Forever (TAF)** — the live service. A FAForever fork (lobby server, IRC, TrueSkill ladder,
  Galactic War, replay vault) plus a native C++ launcher, **gpgnet4ta**, that lobby-launches
  `TotalA.exe` through real DirectPlay and then **proxies every DirectPlay socket onto one UDP port**
  so the FAF ICE adapter can do STUN/TURN/hole-punching.
- **TA Demo Recorder (TADR / "TA Demo")** — the 2000s-era recorder. A **proxy `dplayx.dll`** dropped in
  the TA directory that wraps the DirectPlay COM objects and logs the packet stream to `.tad`; plus a
  proxy `ddraw.dll` ("TA Hook") for in-game UI, and later in-memory `TotalA.exe` code splicing.
- The `.tad` format is documented, both in the 2003 source release and in TAF's C++ re-implementation.

Everything below that is marked *verified* comes from source I read directly (cloned repos, the 2003
source zip). Forum/wiki claims are marked as such.

---

## TA Forever (TAF)

**Type**: full matchmaking client + lobby server + game launcher for original TA and TA mods.
**Status**: actively developed; client `v2026.8.23` released 2026-08-23.
**Author of the launcher/netcode**: `Axle1975 <loweam@gmail.com>` (sole author of all commits in the
`gpgnet4ta` history I cloned).
**Source**: <https://github.com/ta-forever> — 13 repos.
**Licences** (GitHub API): `gpgnet4ta` **MIT** (created 2020-11-08), `downlords-taf-client` **MIT**
(2020-11-05), `server` **GPL-3.0** (2020-11-06), `db` GPL-3.0, `api` MIT, `uid` GPL-3.0,
`taftoolbox` GPL-3.0, `java-ice-adapter` (no licence file).

### What it adds to stock TA
Game browser + one-click host/join, IRC community chat, seasonal TrueSkill ladders and weekly
tournaments, a persistent Galactic War metagame, **live spectating of games in progress**, a permanent
replay archive (site claims **53,685+ battles on record since 2023**), on-demand mod download
(Original TA, ProTA, Escalation, Zero, Twilight, Total Mayhem), server-side result/score detection,
game-file CRC32 whitelisting for ranked play, anti-smurf hardware IDs, medals and a ladder-point
wagering market. Windows and Linux (the Linux path runs the native Windows binaries under Wine).

### How it works — verified from source

**1. Launch: DirectPlay "RippleLaunch", not injection.** There is no DLL injected into `TotalA.exe`.
`talauncher.exe` registers TA as a DirectPlay lobbyable application under
`HKLM\SOFTWARE\Microsoft\DirectPlay\Applications` (`libs/dplayreg/DPlayReg.cpp`), re-launching itself
elevated via `RunAs` because HKLM needs admin (`apps/talauncher/talauncher.cpp`). The per-mod
application GUID is derived as `QUuid::createUuidV5({1336f32e-d116-4633-b853-4fee1ec91ea5}, gamemod)`
(`apps/gpgnet4ta/gpgnet4ta.cpp:681`) so ProTA/Escalation/etc. don't collide. `talauncher` then stays
resident as a "launch-to-lobby server" on `127.0.0.1:48684`, so the unprivileged `gpgnet4ta` can ask
it to start the game (`libs/talaunch/LaunchServer.cpp`).

**2. `libs/jdplay`** is Edwin Stang's 2007 **JDPlay** (GPL-3), driving `IDirectPlay3` /
`IDirectPlayLobby3` with the TCP/IP service provider (`DPSPGUID_TCPIP`). `dplayx.dll` is loaded
dynamically at runtime (`libs/jdplay/DPlayWrapper.cpp`), which is what lets TAF cope with DirectPlay
being an optional Windows feature and with Wine's builtin `dplayx` (recent commits: *"Gate DPAID_INetW
address element to wine only"*, *"Fix jdplay latent bugs exposed by Wine builtin dplayx
(CrossOver/Mac)"*).

**3. Battleroom options are written as a TA ini** from `taforever.ini.template`:
`playerlimit` clamped to **2..10**, `maxunits` clamped to **20..1500**, plus `watching=2`,
`lockOptions`, `location` (1 fixed / 2 random), `cheats=1`, `provider=TAForever`
(`apps/gpgnet4ta/GpgNetGameLauncher.cpp:351-368`). `gpgnet4ta` also copies its own bundled
`online.dll` (59,904 bytes, in the repo root) into the game directory before launch
(`copyOnlineDll`, line 383).

**4. The tunnel — this is the interesting part.** From the repo `readme.md`: ordinarily TA/DirectPlay
listens on **TCP 2300**, **UDP 2350**, and **TCP/UDP 47624** for session "enumeration" (the
enumeration reply is how a host advertises which ports it is listening on). gpgnet4ta instead:

- instantiates a `GameReceiver`/`GameSender` pair **per remote player** on loopback random ports, and
  (when not hosting) seizes 47624 to intercept enumeration (`libs/tafnet/GameReceiver.cpp`);
- rewrites the addresses embedded in DirectPlay `SuperEnumReply`, `CreatePlayerReq` and
  `ForwardPlayerReq` messages so the local TA sends all traffic to those loopback proxies
  (`libs/tafnet/GameAddressTranslater.cpp`, `TafnetGameNode.cpp`);
- multiplexes everything onto **one UDP socket** (`libs/tafnet/TafnetNode.cpp`) with a one-byte action
  code identifying the origin channel (`ACTION_TCP_DATA`, `ACTION_UDP_DATA`, `ACTION_ENUM`, …).

That single UDP port is what makes the **FAF Java ICE adapter** (`ta-forever/java-ice-adapter`,
JSON-RPC over TCP) usable: direct IPv4/IPv6, UDP hole punching, or relay through **coturn**
(`faf-coturn` in `taf-stack/docker-compose.yml`); the lobby server hands out TURN/STUN credentials
(`server/ice_servers`).

**5. Lag/loss mitigation built into the tunnel** (`libs/tafnet/TafnetNode.h`): sequenced
reliable-ordered delivery with ACK/resend, resend timeout derived from measured ping
(`11*ping/10 + 50 ms`, floor 500 ms, cap 700 ms), packet coalescing (`ACTION_MORE`), MTU probing
(`ACTION_PACKSIZE_TEST`/`_ACK`, CLI `--maxpacketsize`), an optional `--proactiveresend` mode
("measure packet-loss during game setup and thereafter send multiple copies of packets accordingly"),
fire-and-forget sequenced UDP with dedup (`ACTION_UDP_DATA_SEQ`), and an explicit **lag-switch
countermeasure**: if a peer's ACKs go silent for 800 ms, buffer its UDP game data (up to 500 packets /
5 s) and replay it in batches when it returns, "so damage packets are never lost".

**6. Replays and live spectating.** `TafnetGameNode` tees every TA packet into a
`TADemo::TAPacketParser`. `libs/tareplay/TaDemoCompilerClient` streams it to a server-side
`TaDemoCompiler`, which merges all players' streams into one `.tad` (`.part` → `.tad` on
finalisation), while `TaReplayServer` streams the same bytes live to watchers (subscribe by
`gameId` + `position`; now gated by signed watch tickets — `TaReplayServerMessages.h` status
`AUTH_REQUIRED`). `apps/replayer` is the viewer: it **hosts a synthetic DirectPlay session** and
feeds a real `TotalA.exe`, walking the demo through the lobby unit-sync handshake and then the tick
stream (`apps/replayer/TaReplayer.h`: `START_SYNC → SEND_UNITS → WAIT_RECEIVE_UNITS → CHECK_ERRORS →
SEND_ACKS → WAIT_GO → LOADING → PLAYING`). URL forms: `gpgnet://host:port/gameid` for live,
`file://foo.tad` for local. **Note**: `TaDemoCompiler.cpp` / `TaReplayServer.cpp` /
`apps/replayserver` were **deleted from the public repo on 2026-06-14** (commit `3c959e5`, "Add
support for authenticated replay watch tickets"); they remain in git history.

**7. Result detection & anti-cheat.** `apps/gpgnet4ta/GameMonitor2.cpp` reconstructs armies, teams,
alliances, unit counts, commander deaths and end-game conditions purely from the packet stream.
`--verify <file>:<crc32>,…` CRC32-checks game files against a server whitelist and refuses ranked play
on mismatch (`GpgNetGameLauncher::verifyGameFileVersions`). Locally recorded demos are **ChaCha20
(RFC 8439) encrypted with the key RSA-sealed (256-byte seal)** so a player cannot read their own
in-progress demo as a maphack; the server unseals afterwards
(`downlords-taf-client .../replay/LocalDemoSealService.java` — its comment cites
`tadr.git src/dplayx/log2.pas`, i.e. TAF maintains a private TADR continuation).
Ratings: `ta-forever/trueskill-taf` (per-mod TrueSkill with sigma relaxation / mu decay for
inactivity). Anti-smurf: `ta-forever/uid` (RSA-encrypted machine info, inherited from FAF).
Completed demos are auto-uploaded to **tademos.xyz** (`server/tada_service.py`, `TADA_API_URL`).

---

## TA Demo & the .tad replay format

**TA Demo Recorder 0.99β2**, by **Fnordia, SJ and Yeha**; full source released **5 Nov 2003**
(<https://www.clan-sy.com/download/tademo/>, `tademo99b2-src.zip`, Delphi 6 + MSVC 6/7; no licence
statement — "provided as is").

**How it hooks TA (verified from the source zip):**
- `Recorder/Dplayx.dpr` builds a **replacement `dplayx.dll`** placed in the TA directory. It exports
  `DirectPlayCreate`, `DirectPlayEnumerate{,A,W}`, `DirectPlayLobbyCreate{A,W}`,
  `gdwDPlaySPRefCount`, `DllCanUnloadNow`, `DllGetClassObject` by ordinal, `LoadLibrary`s the real
  `%SystemRoot%\system32\dplayx.dll`, and returns wrapper COM objects (`TDplay`, `TLobby` in
  `idplay.pas`) so every `Send`/`Receive` can be logged. It self-validates with an XOR-fold CRC stored
  in the last dword of the DLL and refuses to load if altered (`calccrc`).
- `DDraw/` builds a **proxy `ddraw.dll`** ("TA Hook") that provides the interface upgrade — build
  rings, DT lines, whiteboard, minimap handler, share macro — over a reverse-engineered map of TA's
  in-memory structs (`DDraw/tamem.h`: `PlayerStruct`, `UnitStruct`, `WeaponStruct`, …).
  `VisPatcher/` patches `totala.exe` to load `spank.dll` instead of `ddraw.dll` on Windows 95.
- `Server/` replays a `.tad` by hosting a DirectPlay session for a real TA instance — the same trick
  TAF's `replayer` uses today.

**The format** — original spec, `Docs/saveformat.txt` (Swedish comments): every record is
length-prefixed with a `word`.

```
header: word len; char magic[8]="TA Demo\0"; word version; byte numPlayers; string mapName
player  x numPlayers:  byte color; byte side (0=ARM,1=CORE,2=WATCH); byte number; string name
playerstatusmessage x numPlayers: byte number; string statusmessage  (raw TA net-format 0x20 packet)
unitdata x1: all $1a (subtype 2,3) unit-sync packets exchanged between players 1 and 2, uncompressed
packet  xN: word time (ms since previous packet); byte sender; string data
```

Later versions extend it. TAF's C++ re-implementation (`libs/tapacket/TADemoRecords.h`,
`TADemoParser.cpp`) accepts **versions 3–5**, adds `maxUnits` to the header, and adds an
`ExtraHeader{numSectors}` + `ExtraSector{type,data}` table with types
`COMMENTS=1, CHAT=2, RECORDER_VERSION=3, DATE=4, RECORDER_CONTEXT=5, PLAYER_ADDR=6, MOD_ID=7`.
Version 3 demos carry per-subpacket timestamps. Recorder-side "SmartPak" compression rewrites the
0x2c stream using tick codes `0xFE/0xFF/0xFD` (`Docs/unSmartPak.txt` in TADR;
`TPacket::unsmartpak`).

**What you can extract from a .tad**: players, sides, colours, slots, map name and hash, unit limit,
mod id, recorder version/date, chat, the full unit-sync table (unit id, CRC, limit, in-use), and the
complete in-game packet stream — enough to recompute unit counts, income, scoreboards and alliances,
and to replay the match inside real TA. `tademos.xyz` does exactly that (unit-count aggregations,
income charts, scoreboards). TAF's client shells out to `replayer.exe --demourl <path> --info` and
parses JSON (`downlords-taf-client .../fa/DemoFile.java`).

**Modern lineage — TADR.** `https://github.com/Skirmisher/tadr` mirrors `svn.riouxsvn.com/tadr`
(last commit 2015-07-30): the same Delphi codebase grown into a plugin engine
(`src/Recorder/plugins/`) with LOS sharing, GUI enhancements, `SaveGame`, `Voting`,
`WarZoneRankings` (HTTP user-agent string: *"Total Annihilation Unofficial Patch"*), and in-memory
patching of `TotalA.exe` — `InitCode_CoreExePatching.pas` splices a jump at a hardcoded address
(`0x004E6FA0` for TA 3.1) out of the proxy `dplayx.dll` into a thunk. Its
`netmsgHandling/TA_NetworkingMessages.pas` documents exact packet records for speed/pause (0x19),
rejection (0x1B), ally (0x23) and team (0x24).

---

## Historical services & shims (Boneyards, GameRanger, DirectPlay)

- **Boneyards** (Cavedog's official service): per the TA Fandom wiki summary, beta from
  **1998-11-12**, public **1999-04-08**, shut down a few months before Cavedog's demise in **August
  2000**; featured the "Galactic War" ARM-vs-CORE metagame that TAF's Galactic War revives.
  *(Wiki/GameSpot secondary sources — I could not open the pages directly.)*
- Contemporary third-party services listed for TA: **Heat, IGZ, Kali, Mplayer, TEN, Wireplay** — all
  defunct. *(Wiki summary, secondary.)*
- **GameRanger**: Mac-first (July 1999), **Windows support added 2008** (v4.9, 2008-11-13); still
  lists Total Annihilation among 700+ titles (Wikipedia). **Its technical mechanism is not documented
  in any source I could reach** — I could not verify whether it tunnels DirectPlay, emulates a LAN
  segment, or relays. Treat "GameRanger does X" claims as unverified.
- **DirectPlay on modern Windows**: `dplayx.dll` is a *legacy component*, enabled via "Turn Windows
  features on or off → Legacy Components → DirectPlay"; "A required file DPLAYX.DLL was not found" is
  the classic symptom (Steam discussions / TA readme). TAF works around this by loading `dplayx`
  dynamically and, on Linux, running the Windows binaries under Wine — it even kills `dplaysvr.exe`
  to free port 47624 before launching (`TotalAnnihilationService.freePort47624`).
- **"TA Direct Connect" / TADR as a lobby**: I could **not** verify a distinct project called "TA
  Direct Connect". "TADR" in the TA community means the **TA Demo Recorder**, not a lobby.
  tauniverse's guidance is plain direct-IP/LAN peer-to-peer play. tauniverse.com is behind Cloudflare
  and I could not read `/play-ta/` or the forum threads.

---

## What we learned about TA's network model

*(All verified from source unless noted.)*

- **Transport**: DirectPlay P2P. TCP 2300 carries lobby/game-init traffic (game + player status, unit
  sync, chat); UDP 2350 carries in-game data after launch; TCP/UDP 47624 carries session enumeration,
  and the enumeration *reply* is the primary way a host advertises its ports (gpgnet4ta `readme.md`).
- **Wire encoding** (`libs/tapacket/TPacket.cpp`): byte 0 is `0x03` = uncompressed / `0x04` =
  compressed. Compression is LZ77-ish: one control byte carries 8 flag bits, a set bit introduces a
  16-bit back-reference (12-bit offset, 4-bit length + 2). The "encryption" is trivial — an
  incrementing XOR key starting at 3, with a 16-bit additive checksum stored in bytes 1–2.
- **Subpacket taxonomy** (`TPacket.h`, corroborated by TADR's `Docs/lobbyprot.txt` and
  `TA_NetworkingMessages.pas`): 0x02 ping, 0x05 chat, 0x08 loading started, 0x09 build started,
  0x0B damage, 0x0C unit killed, 0x0D weapon fired, 0x12 build finished, 0x14 give unit, 0x16 share
  resources, **0x18 host migration**, 0x19 speed/pause, **0x1A unit data (sync)**, 0x1B reject,
  0x20 player info/status (193 bytes), 0x23 ally, 0x24 team, 0x28 player resource info,
  0x2A loading progress, **0x2C unit stat + move**, 0xFC map position.
- **Not classic lockstep.** TA replicates *state and events*, not just orders: 0x2C carries per-unit
  status/position and is what everything treats as the game clock (`GameMonitor2` uses "serial of last
  2C packet" as the tick), 0x28 carries resource totals. The only "sync" in the classic sense is the
  **pre-game unit-data handshake**: every unit definition's id, CRC and limit is exchanged as 0x1A
  subpackets and mismatches counted (`TaReplayer`: `unitSyncSendCount`, `unitSyncErrorCount`,
  `unitSyncAckCount`; `Docs/lobbyprot.txt`: subtype 3 = "tell client this unit exists / is synced",
  subtype 2 = checksum). That is a *content-compatibility* check (all peers must have identical unit
  data), not a runtime desync detector. TAF layers file-CRC whitelisting on top for ranked play.
- **The unit limit.** TADR's `plugins/UnitLimit.pas` embeds the disassembly of TA 3.1 at `0x0049163A`:
  TA reads `"UnitLimit"` from the ini with default `0x5DC` and **clamps it to `[0x14, 0x5DC]` =
  `[20, 1500]`** — exactly the range TAF re-implements. Crucially, **unit IDs are allocated in
  per-player blocks of `maxUnits`**: `GameMonitor2::onUnitDied` identifies a commander as
  `unitId % m_maxUnits == 1`. So `maxUnits` is part of a shared ID space every peer must agree on, and
  it is carried in the 0x20 status packet (offset `0xA6`, per `libs/tapacket/notes/statuspackets.txt`)
  and in the `.tad` header. Verdict: the cap is an **engine ID-space constant that is
  network-significant**, not a pure bandwidth throttle — though 0x2C state traffic does scale with
  unit count. TADR's `history.txt` records fixing crashes "when the max unit count is greater than
  500" and its plugin comment claims it "ups the unit limit from 500 to 1500 or 5000". **I found no
  primary source stating the limit exists *because of* bandwidth.**
- **The player cap is 10.** `TA_MemoryConstants.pas`: `MAXPLAYERCOUNT = 10`; the player array is
  `0..9` plus one "extra" slot, and `PlayersSlotsExpand.pas` lists ~15 separate `cmp …, 0Ah` sites in
  `TotalA.exe`. That plugin patches the count byte at `0x00464995` to **32** and relocates the player
  struct array (`InitPlayersArray` / `SetPlayerStructMem` splices at `0x00464992` / `0x00401083`).
  Whether it ever worked in practice is **unverified**; shipped TADR and TAF still cap at 10, and
  tademos.xyz shows 10-player FFAs as the maximum.
- **Spectating is native.** `side = 2` means WATCH in both the `.tad` player record and the status
  packet; the ini option is `watching=2`. TAF's replayer joins as a watcher (with cheats enabled for
  full vision) rather than adding a new observer mode to the engine.
- **Host migration exists** as packet 0x18 (`TPacket::createHostMigrationSubpacket`).

---

## State of online play today

- **TA Forever is the primary service.** Client `v2026.8.23` (2026-08-23), Windows + Linux installers,
  requires an owned copy of TA (Steam/GOG). Release notes from July–August 2026 show continuous work
  (reconnect robustness, browser-based replays, start-position pre-selection, wagering, medals).
- **Activity is real and daily**: `tademos.xyz` listed 16 uploaded matches for 2026-08-30/31 alone,
  including 10-player FFAs, across OTA (v3.x), ProTA (4.8) and Escalation (10.2). The TAF site claims
  53,685+ battles on record since 2023.
- **Concurrent player counts: not verified.** taforever.com renders "Commanders online now" /
  "Battles in progress" client-side; the static HTML I fetched shows `0` placeholders, so I have no
  real concurrency figure.
- **GameRanger** still lists TA; **direct IP / LAN** still works if the DirectPlay Windows feature is
  enabled. Boneyards, Kali, Mplayer, TEN, Heat and Wireplay are all long gone.

---

## Open questions / uncertainty

1. Real concurrent-player numbers for TAF — the live counters are JS-rendered and I could not read them.
2. GameRanger's actual transport mechanism for TA (DirectPlay tunnelling vs LAN emulation vs relay).
3. tauniverse.com (forums, `/play-ta/`, the Cavedog Boneyards mirror) is Cloudflare-gated; I could not
   read the primary community threads, including "About .tad file" (t=40243), "TA Demo Recorder v1.0
   RC1/RC2" (t=35116, t=35957) and "Networking, TA's Achilles Heel" (t=27068).
4. The **modern TADR source is not public** — TAF references `tadr.git src/dplayx/log2.pas`, but the
   only public copy I found is Skirmisher's 2015 SVN mirror.
5. TAF's **replay server and demo compiler are now closed source** (removed 2026-06-14); only the
   pre-ticket versions are recoverable from git history.
6. Whether TADR's 32-player-slot patch ever functioned; and whether `>1500` unit limits work at all
   over the network.
7. `online.dll` shipped in the gpgnet4ta repo root: I did not disassemble it, so its exact role
   (Cavedog's original online plugin vs. a TAF replacement) is unverified.
8. "TA Direct Connect" as a named project — I could not find it; it may be a colloquialism for
   direct-IP play.

---

## Sources

**Code read directly**
- <https://github.com/ta-forever/gpgnet4ta> (MIT) — `readme.md`, `libs/tafnet/*`, `libs/tapacket/*`,
  `libs/jdplay/*`, `libs/dplayreg/*`, `libs/tareplay/*`, `libs/talaunch/*`, `apps/gpgnet4ta/*`,
  `apps/replayer/*`, `apps/talauncher/talauncher.cpp`, `taforever.ini.template`,
  `libs/tapacket/notes/{statuspackets.txt,dplay dwuser status bits.txt,subpacket types reading from TA socket.txt}`,
  commit `3c959e5` (2026-06-14, removal of `TaDemoCompiler`/`TaReplayServer`).
- <https://github.com/ta-forever/downlords-taf-client> (MIT) —
  `fa/TotalAnnihilationService.java`, `fa/DemoFile.java`, `replay/LocalDemoSealService.java`,
  `fa/relay/ice/*`.
- <https://github.com/ta-forever/server> (GPL-3.0) — `server/config.py`, `server/tada_service.py`,
  `server/ice_servers`.
- <https://github.com/ta-forever/taf-stack> — `docker-compose.yml` (`faf-coturn`,
  `taf-replay-server`, `taf-demo-compiler`).
- <https://github.com/ta-forever/trueskill-taf>, <https://github.com/ta-forever/uid>,
  <https://github.com/ta-forever/java-ice-adapter>, <https://github.com/ta-forever/unrealircd>.
- <https://www.clan-sy.com/download/tademo/> → `tademo99b2-src.zip` (TA Demo 0.99β2 source,
  2003-11-05) — `TADemoSrc.txt`, `Recorder/Dplayx.dpr`, `idplay.pas`, `DDraw/*`,
  `Docs/{saveformat.txt,lobbyprot.txt,PACKETS.TXT,TANET.TXT,newfeatures.txt,donestuff.txt}`.
- <https://github.com/Skirmisher/tadr> (mirror of `svn.riouxsvn.com/tadr`, last commit 2015-07-30) —
  `src/Recorder/{TADemoConsts.pas,InitCode_CoreExePatching.pas,history.txt,unSmartPak.txt}`,
  `src/Recorder/plugins/{UnitLimit.pas,PlayersSlotsExpand.pas}`,
  `src/Recorder/netmsgHandling/TA_NetworkingMessages.pas`, `src/Recorder/TAMem/TA_MemoryConstants.pas`,
  `src/Docs/lobbyprot_2.txt`, `ta entry point.txt`, `ta info.txt`.
- <https://github.com/jchristi/tademo99b2-src> (another copy of the 2003 source).

**Web**
- <https://www.taforever.com/> — client version, feature claims, 53,685+ battles since 2023.
- <https://www.tademos.xyz/> — the TAF demo archive (browsed 2026-08-31).
- <https://github.com/ta-forever/downlords-taf-client/releases> — changelogs v2026.7.19 … v2026.8.23.
- <https://en.wikipedia.org/wiki/GameRanger> — Mac 1999, Windows support 2008 (v4.9, 2008-11-13).
- <https://totalannihilation.fandom.com/wiki/Boneyards> and
  <https://www.gamespot.com/articles/cavedog-digs-up-boneyards/1100-2465464/> — Boneyards dates
  (accessed only via search-result summaries; pages themselves returned 402/403).
- <https://www.tauniverse.com/> , <https://www.tauniverse.com/play-ta/> ,
  <https://www.tauniverse.com/forum/showthread.php?t=27068> — **Cloudflare-blocked, not read**.
- <https://www.pcgamingwiki.com/wiki/Total_Annihilation> — **Cloudflare-blocked, not read**.
- Steam community threads on enabling DirectPlay under Windows 10/11 (via search summaries).
