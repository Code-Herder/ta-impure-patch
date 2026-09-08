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
- **Under wine you must bring your own DirectPlay.** Wine implements the *client* half of
  DirectPlay TCP/IP and not the host half, so no wine process can create a session on any
  version, `master` included. Dropping in Microsoft's native `dplayx`/`dpwsockx`/`dplaysvr.exe`
  fixes it completely — host, join and game traffic all verified on wine 9.0, 2026-09-02, with
  `tools/dptest`. See [DirectPlay under Wine](#directplay-under-wine-measured-2026-09-02);
  `tools/dpinstall.sh` does the install.

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

---

## DirectPlay under Wine — measured, 2026-09-02

The 2026-09-02 extra-weapons session concluded that wine 9.0's DirectPlay TCP/IP
provider is "a stub" and that the fix was native `dplayx`/`dpwsockx` from
`dxnt.cab`. That was right about wine 9.0 and **wrong about the fix**. This
section replaces it with measurements.

### How it was measured

Not with TA. TA reports every DirectPlay failure the same way — a two-second
"Updating..." box that bounces back to `SELPROV` — so the game cannot tell a
missing provider from a bad address. `tools/dptest/` is a ~200-line 32-bit PE
that asks `dplayx` + `dpwsockx` for exactly what TA asks: select the *Internet
TCP/IP Connection For DirectPlay* provider via a compound address, `Open` a
session, `EnumSessions`, join, `CreatePlayer`, `SendEx`. It returns `HRESULT`s.
`tools/dptest/run.sh` runs host/enum/join against any wine binary in a throwaway
prefix.

### The result

| Call | wine 9.0 (Ubuntu 24.04 stock) | wine 11.0 (Proton Experimental) |
|---|---|---|
| `CoCreateInstance(IID_IDirectPlay4A)` | `DP_OK` | `DP_OK` |
| `CreateCompoundAddress` (SP=TCP/IP, INet=127.0.0.1) | `DP_OK` | `DP_OK` |
| `InitializeConnection` | `DP_OK` | `DP_OK` |
| `EnumSessions` | `DPERR_UNSUPPORTED` | **`DP_OK`** |
| `Open(DPOPEN_CREATE)` — *host* | `DPERR_UNSUPPORTED` | **`DPERR_UNSUPPORTED`** |

`DPERR_UNSUPPORTED` is `E_NOTIMPL` (`0x80004001`). The trace names the cause
exactly:

```
fixme:dplay:DP_SecureOpen (…): partial stub
trace:dplay:DPWSCB_Open (1,00000000,…)
fixme:dplay:DPWSCB_Open session creation is not yet supported
err:dplay:DP_SecureOpen Unable to open session: DPERR_UNSUPPORTED
```

**Wine implements the client half of DirectPlay TCP/IP and not the host half.**
`EnumSessions` succeeds and `DPWSCB_Open`'s join branch is fully written; the
create branch is a `FIXME`. With wine's **builtin** DirectPlay, two `tacli`
instances on one machine therefore cannot form a game on any wine version — and
this has nothing to do with TA, the tagpu stack or the extra weapons module.
(Native DirectPlay lifts exactly this limit; see the next subsection.)

Note what was *not* proven by that table: joining was never exercised end to end,
because with hosting broken there was never a session to join. `EnumSessions`
returning `DP_OK` with zero results is the honest reading — the call is
implemented, the network was empty.

### Resolved the same day: native DirectPlay, and multiplayer works

Swapping Microsoft's own DirectPlay in front of wine's builtins fixes hosting
outright. Measured with the same probe, **wine 9.0**, native `dplayx` +
`dpwsockx` + `dplaysvr.exe`:

| Call | builtin | native |
|---|---|---|
| `EnumSessions` | `DPERR_UNSUPPORTED` | `DP_OK`, and it *finds* the session |
| `Open(DPOPEN_CREATE)` — host | `DPERR_UNSUPPORTED` | **`DP_OK`** |
| `Open(DPOPEN_JOIN)` | never reached | **`DP_OK`** |
| `CreatePlayer` both ends | never reached | **`DP_OK`** |
| game messages host←join | never reached | **3/3 delivered** |

```
[join]   SESSION "DPTEST-SESSION" players=1/4 flags=0x44
[host]   *** GAME MSG from=0x88f4ede7 to=0x88f4ede5 len=18: "hello-from-join-0"
```

Verified in **both** a `win32` and a `win64` prefix (32-bit DLLs go to
`syswow64` on the latter). `tools/dpinstall.sh <prefix>` does the install and
prints the override string.

**Two things that will waste an afternoon if you don't know them:**

1. **Override the two EXEs by name, not just the DLLs.** With only
   `dplayx,dpwsockx=n`, wine runs *its own* stub `dplaysvr.exe`
   (`programs/dplaysvr/main.c` is `WINE_FIXME("stub:")` + `return 0`), native
   dplayx waits for a name server that never appears, and `Open` **hangs with no
   error at all**. The trace tell is `fixme:dplaysvr:wmain`. The full string is
   `dplayx,dpmodemx,dpnet,dpnhpast,dpnhupnp,dpwsockx,dplaysvr.exe,dpnsvr.exe=n`.
2. **`dplaysvr.exe` outlives the game and owns UDP 47624 across prefixes.** A
   stale one from an earlier run makes the next host fail
   `Open(DPOPEN_CREATE) = DPERR_GENERIC` — which looks like a prefix problem and
   isn't. `pkill -x dplaysvr.exe` first. This is precisely why TAF calls
   `TotalAnnihilationService.freePort47624`; that behaviour now makes sense
   rather than looking like superstition.

Where the files came from is its own small saga — see the routes below. They live
outside the repo at `~/.local/share/ta-directplay/` (Microsoft redistributables,
deliberately not committed); `PROVENANCE.txt` there records the source chain and
the signature check.

### Why wine 9.0 vs 11.0 differ, and where the ceiling is

`dpwsockx` was rewritten by **Anton Baskanov** between 2023-10 and 2024-11.
`dlls/dpwsockx/dpwsockx_main.c` goes 240 lines (everything a stub) at `wine-9.0`
→ 652 at `wine-9.18` → **1292 at `wine-9.21`**, which is the release the press
notes describe as "expanded support for network sessions in DirectPlay". It has
been **1293 lines and unchanged from `wine-10.0` through `master`** (2026); only
build-system churn since. So 9.21 is the step that matters and there is no later
one to wait for.

The surviving stubs in master are `Cancel`, `GetAddress`, `GetAddressChoices`,
`GetMessageQueue`, `Reply`, `Send` — plus the `bCreate` branch of `Open`:

```c
static HRESULT WINAPI DPWSCB_Open( LPDPSP_OPENDATA data )
{
    …
    if ( data->bCreate )
    {
        FIXME( "session creation is not yet supported\n" );
        return DPERR_UNSUPPORTED;
    }
```

*(`Send` being a stub is harmless — DirectPlay ≥3 uses `SendEx`, which is
implemented.)*

### How large the missing piece actually is

Smaller than it looks, because `dplayx` already carries the host-side protocol.
`dlls/dplayx/dplay.c` handles `DPMSGCMD_ENUMSESSIONSREQUEST` (calling
`NS_ReplyToEnumSessionsRequest`), `REQUESTNEWPLAYERID`, `CREATESESSION`,
`ADDFORWARD`, `FORWARDADDPLAYER`, `CREATEPLAYER` and `PING` — those are all
things only a host does. `dpwsockx`'s `DPWS_Start()` already binds a TCP
listener, binds a UDP socket, and runs a background receive thread.

What is missing is the rendezvous:

1. **Nothing binds UDP 47624.** `DPWS_PORT` (47624) appears exactly twice in
   `dpwsockx_main.c` — as the *destination* of the enumeration broadcast in
   `DPWSCB_EnumSessions`, and in the `#define`. `DPWS_Start` binds its UDP socket
   to a *free* port in the dynamic range, so a wine host would never see a
   client's broadcast even if it were listening.
2. **Wine's `dplaysvr.exe` is a literal stub** — `programs/dplaysvr/main.c` is
   1047 bytes whose `wmain` is `WINE_FIXME("stub:")` and `return 0`. On Windows
   this is the daemon that owns 47624 and answers enumeration for every
   DirectPlay app on the box. (It is also why TAF kills `dplaysvr.exe` before
   launching: on Windows the port is *taken*, not free.)
3. **`DPWSCB_Open`'s create branch** needs to `DPWS_Start()` and mark the local
   player as the name server, instead of dialling out to a remote one.

So the patch is: bind 47624, route what arrives there into the existing message
handler, and fill in the create branch. That is small — but it is real protocol
work, since the reply has to carry the host's TCP port in the SP header for the
joiner to connect back. Nothing upstream appears to be in progress on it.

### The IPX branch is closed, for two independent reasons

1. **The kernel no longer has IPX.** `CONFIG_IPX` was dropped in Linux **5.15**;
   the reference setup runs 7.0 and `/boot/config` has `CONFIG_ATALK` and
   `CONFIG_NETROM` but no `CONFIG_IPX`, and `modinfo ipx` finds nothing. Wine's
   IPX support needs `AF_IPX` sockets from the kernel.
2. **It would not help anyway.** `loader/wine.inf.in` registers *both* providers
   against the same DLL — `Internet TCP/IP Connection For DirectPlay` →
   `Path=dpwsockx.dll` and `IPX Connection For DirectPlay` → `Path=dpwsockx.dll`.
   Choosing IPX in TA's provider list reaches the identical `DPWSCB_Open`, so it
   hits the identical "session creation is not yet supported".

**IPXWrapper** (`solemnwarning/ipxwrapper`, tunnels IPX over UDP, no kernel IPX
needed; GOG ships it with some titles) is the answer the internet gives for IPX
games on modern systems, and it does work under wine — but it wraps *winsock*,
so it only helps a **native** `dpwsockx` that opens `AF_IPX` sockets. Against
wine's builtin provider there is nothing for it to wrap. It is a companion to
route (a) below, not an alternative to it.

### Routes considered, cheapest first — (a) is the one that worked

**(a) Native DirectPlay — done, 2026-09-02.** This is the route that worked.
`winetricks directplay` wants `dxnt.cab` from `directx_feb2010_redist.exe`, and
every Feb-2010 source is dead (table below, all re-checked in a real browser).
**The March 2008 redist carries the same `dxnt.cab` and is still on archive.org**
— item `directx_mar2008_redist`, "DirectX End-User Runtimes (March 2008)",
72,829,472 bytes, sha1 `21aa91ca…4cae` matching the item metadata. March 2008
predates the DirectPlay split, so `dxnt.cab` (13,265,040 bytes) is present.

Authenticity was verified rather than assumed: the Authenticode SHA1 computed
independently from the installer's own PE bytes,
`4c157a4dec8da4b6ff1ad07759527b22bc09718d`, is byte-identical to the digest
sealed in its PKCS#7 `SpcIndirectDataContent` (OID `1.3.6.1.4.1.311.2.1.4`), and
the signer chain is Microsoft Corporation → Microsoft Code Signing PCA →
Microsoft Root Authority. So Microsoft's signature covers that exact file and the
archive.org copy is unmodified.

Extraction needed no new packages: the outer cabinet is **LZX**, which no
installed tool handled, so `tools/dptest/`'s sibling trick was used — a small
mingw program driving `cabinet.dll`'s FDI API under wine, i.e. wine's own LZX
decoder. (`cabextract`, `7z`, `bsdtar` and `gcab` are all absent on the reference setup, and
wine's `expand` only does `infile outfile`.)

**The original framing, kept because it is still true of Feb 2010:**
`winetricks directplay` extracts `dplayx.dll`, `dpwsockx.dll`, `dplaysvr.exe`,
`dpmodemx`, `dpnet`, `dpnhpast`, `dpnhupnp`, `dpnsvr.exe` from `dxnt.cab` inside
`directx_feb2010_redist.exe`, then overrides them native. This bypasses the wine
gap completely — Microsoft's DLLs implement hosting — and it does **not** require
a newer wine, since it replaces both halves. Download status re-checked
2026-09-02, all four sources dead:

| Source | Result |
|---|---|
| `download.microsoft.com/.../directx_feb2010_redist.exe` | **404** |
| `files.holarse-linuxgaming.de/mirrors/microsoft/…` (winetricks' 2026-08 note) | **403**, with or without a `Referer` |
| `web.archive.org/…id_/…` (the URL winetricks itself now uses, sha256 `f6d191e8…`) | empty body |
| archive.org items | only `directx_Jun2010_redist` variants — **June 2010 has no `dxnt.cab`**, only the monthly D3DX cabs |

`dxnt.cab` also exists on any Windows box with DirectX 9.0c, and DirectPlay ships
in Windows XP's own `system32` — either would have worked. In the end none was
needed.

**(b) Patch wine's `dpwsockx`.** *No longer needed here, but still the only fix
that would help someone without the Microsoft files, and still upstreamable.* Scoped in "How large the missing piece is"
above. Needs a wine 11 build tree; the result is one PE DLL that can be dropped
into a prefix and overridden. Upstreamable. The honest estimate is days, not an
afternoon, and it is protocol work with a real chance of a long tail.

**(c) Host on Windows, join from wine.** Moot now, and never tested. A Windows box or VM hosting, with wine
instances joining, should work today. For the extra-weapons assertions this is
awkward — the armed/unarmed pair wants both peers under `tacli` — but it would
settle the CRC question (assertions 8/9) with one Windows peer.

**(d) Wait for upstream.** Not recommended: `dpwsockx` has been untouched since
November 2024 and no MR for host support was found.

### Practical notes for whoever picks this up

- **A wine 11 is already on the reference setup, with nothing to download**:
  `~/.steam/steam/steamapps/common/Proton - Experimental/files/bin/wine` reports
  `wine-11.0` (build `experimental-11.0-20260826`) and runs 32-bit PEs from a
  plain `WINEPREFIX` with no Steam runtime — that is how the table above was
  measured. It ships i386 `dplayx.dll`, `dpwsockx.dll`, `dplaysvr.exe`.
  **Whether TA + the tagpu `ddraw` override run under it is untested.**
- Ubuntu 24.04's `wine` is pinned at 9.0 in `noble/universe`; WineHQ's own repo
  is not configured on the reference setup. WineHQ packages install under `/opt/wine-*`, so
  a newer wine can coexist with the distro one rather than replacing it.
- **`tacli` drives this now** (2026-09-02). `tacli launch <inst> --dplay` installs
  native DirectPlay into that instance's prefix and appends
  `dplayx,dpmodemx,dpnet,dpnhpast,dpnhupnp,dpwsockx,dplaysvr.exe,dpnsvr.exe=n` to
  the hard-coded `ddraw=n,b`; it is sticky per instance, so a single-player
  instance keeps wine's builtin. `--free-dplay-port` kills a stale `dplaysvr.exe`
  first and belongs on the **hosting** launch only — the port is owned
  machine-wide, so doing it while a peer hosts takes that game down too.
- **`dpinstall.sh` must not overwrite in place.** tacli clones prefixes with
  `cp -al`, so every instance shares one inode per `system32` file with the
  template; a plain `cp` would have written Microsoft's `dplayx` through the
  hardlink into the template and all ten existing prefixes at once, including two
  games another session had running. It uses `cp --remove-destination`.
- **Let the prefix settle between `dptest` and a launch.** Starting TA into a
  prefix whose wineserver is still shutting down after a killed `dptest host`
  produced a launch that created no process at all and no `ErrorLog.txt`. The
  relaunch was fine.
- **Wine 11 is not needed for any of this.** Native DirectPlay works on the
  stock wine 9.0 the instances already use, because it replaces both halves.
- Upgrading to wine 11 **on its own does not unblock multiplayer.** It buys
  working enumeration and joining, and nothing that lets a game start locally.

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

## Extra weapons on the wire (2026-09-02)

The [more-than-three-weapons module](extra-weapons.md) is the first thing in this
project that changes the simulation, so it is the first thing that has to agree
between peers. What it relies on from the network model, and what is still open:

- Remote units are not aimed locally: `AutoAim` runs only for units owned by a local
  human or AI (`player+0x73 ∈ {1,2}` in `0x48AD30`). A remote unit's weapons are driven
  by two packets — `0x10 UNIT_START_SCRIPT` (COB method *index*, looked up by name on
  the sender) and `0x0D WEAPON_FIRED`, whose `WeapIdx` byte at `+0x23` names the slot.
  The wire format already carries a full byte, so ten weapons need no packet change;
  the receiver (`0x49D270`) is spliced to resolve `WeapIdx >= 3` into the side slot and
  clamps anything beyond the unit's count.
- An **unarmed** peer receiving `WeapIdx >= 3` would index past its three inline slots
  into `UnitOrders`. The guard is the unit-sync CRC handshake (`CRC_weapons`,
  `def+0x146`, folded into `CRC_all`), which the module now extends to `weapon4..N`.
- **Tested 2026-09-02, in three two-instance games over loopback.** Two armed peers
  stay in lockstep: the host spawned a ten-laser tower, it replicated to the joiner
  through TA's own create packet, and both peers reported the same
  `fires by slot: 0=12 3=1 4=12 5=1 6=12 7=1 8=2` — the joiner's launches coming
  purely from `0x0D WEAPON_FIRED`, since it owns nothing there and never ran
  `AutoAim`. An armed host against an **unarmed** joiner loses exactly the extended
  unit types: the engine's type table drops from 281 to 279 on *both* sides and the
  game starts, silently. So a mismatched peer can never see a `WeapIdx >= 3` packet,
  because no unit of that type exists in the game. Both controls (both-armed and
  both-unarmed) keep all 281. Full detail and the CRC formula are in
  [extra-weapons](extra-weapons.md#multiplayer-who-computes-what-and-the-guard).
- **How to run it**: `tools/mp_lobby.sh <host-instance> <join-instance> [map]`, with
  both instances launched `--dplay` (the host also `--free-dplay-port`). wine's
  builtin DirectPlay cannot host at all (`DPWSCB_Open`: "session creation is not yet
  supported"), which is why the first attempt got no further than an empty
  `SELGAME`; native `dplayx` + `dpwsockx` + `dplaysvr.exe` fix that on the stock
  wine 9.0 the instances already use.

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
9. Whether **TA plus the tagpu `ddraw` override runs under wine 11 at all** — only the DirectPlay
   probe was run against Proton's wine, never the game.
10. Whether TAF actually hosts under wine, given that `DPWSCB_Open`'s create path is unimplemented.
   TAF carries commits about coping with "wine's builtin dplayx" (CrossOver/Mac), which is hard to
   square with the measurement unless TAF hosts only on Windows or its proxy stands in for the host.
   **Not resolved** — worth reading `libs/jdplay` against the wine finding before trusting either.
11. ~~A working `dxnt.cab` source.~~ **Answered 2026-09-02**: the March 2008 redist on
   archive.org carries it, Microsoft-signed and signature-verified. Feb 2010 remains
   undownloadable from every documented URL.
12. ~~Whether TA itself (not just the probe) forms a game over native DirectPlay, and what the
   lobby does with a unit-CRC mismatch.~~ **Answered 2026-09-02**: it does — two `tacli`
   instances reach one live game over loopback (`tools/mp_lobby.sh`) — and on a unit-CRC
   mismatch TA **disables the affected unit types on both peers and starts anyway**, with no
   lobby message of any kind. See `extra-weapons.md` §Multiplayer.

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

**Code read directly (wine, 2026-09-02)**
- <https://github.com/wine-mirror/wine> — `dlls/dpwsockx/dpwsockx_main.c` at tags `wine-9.0`,
  `wine-9.18`, `wine-9.21`, `wine-10.0` and `master`; `dlls/dplayx/{dplay.c,name_server.c}` at
  `wine-11.0`; `programs/dplaysvr/main.c`; `loader/wine.inf.in` (service-provider registration).
  Commit history of `dlls/dpwsockx` and `dlls/dplayx` via the GitHub API.
- <https://github.com/Winetricks/winetricks> — `src/winetricks`, the `directplay` verb and
  `helper_directx_dl` (the `dxnt.cab` file list and the archive.org URL + sha256 it now uses).

**Measured here**
- `tools/dptest/` — the probe and its `run.sh`; raw logs regenerate in `out-<label>/`.

**Web**
- <https://www.winehq.org/news/2024110801> — Wine 9.21 release notes ("more support for network
  sessions in DirectPlay"); also covered by Phoronix, GamingOnLinux and Linuxiac.
- <https://github.com/solemnwarning/ipxwrapper> — IPX over UDP, no kernel IPX required.
- <https://cateee.net/lkddb/web-lkddb/IPX.html> — `CONFIG_IPX`, removed in Linux 5.15.
- <https://github.com/playage/dprun> — a DirectPlay lobby launcher (noted, not tried).
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
