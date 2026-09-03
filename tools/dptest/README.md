# dptest — a DirectPlay TCP/IP host/join probe

A ~200-line 32-bit PE that asks `dplayx` + `dpwsockx` for exactly what Total
Annihilation asks of them: pick the *Internet TCP/IP Connection For DirectPlay*
service provider, create a session, enumerate it, join it, create players and
push one game message across.

It exists so that "multiplayer doesn't work" can be answered without launching
the game. TA's provider screen reports a DirectPlay failure as a two-second
"Updating..." box that bounces back to `SELPROV` — indistinguishable from a dozen
other faults. `dptest` returns the `HRESULT` instead.

## Use

```
make
./run.sh wine9  /usr/bin/wine "$CLAUDE_JOB_DIR/tmp/pfx9"
./run.sh wine11 "$HOME/.steam/steam/steamapps/common/Proton - Experimental/files/bin/wine" \
                "$CLAUDE_JOB_DIR/tmp/pfx11"
```

`run.sh` uses a throwaway `WINEPREFIX`, so it never touches `tagpu/instances/`.
Logs land in `out-<label>/`; the `.err` files carry the
`WINEDEBUG=+dplay,+dplayx,+dpwsockx` trace, which is where the actual FIXME
naming the missing feature appears.

Modes: `dptest.exe host|enum|join [address]` (address defaults to `127.0.0.1`).

## What it measured on 2026-09-02

| | wine 9.0 (Ubuntu 24.04) | wine 11.0 (Proton Experimental) |
|---|---|---|
| `EnumSessions` | `DPERR_UNSUPPORTED` | `DP_OK` |
| `Open(DPOPEN_CREATE)` — host | `DPERR_UNSUPPORTED` | `DPERR_UNSUPPORTED` |

Wine implements the **client** half of DirectPlay TCP/IP and not the host half:
`dpwsockx_main.c:DPWSCB_Open` still opens with
`if (data->bCreate) { FIXME("session creation is not yet supported"); return DPERR_UNSUPPORTED; }`
as of master.

With **native** Microsoft DirectPlay installed (`tools/dpinstall.sh`), the same
probe on wine 9.0 gets `DP_OK` for host `Open(DPOPEN_CREATE)`, join, both
`CreatePlayer`s and all three game messages — in win32 and win64 prefixes alike.
Run it that way to confirm a prefix is actually ready for multiplayer before
blaming the game.

See `research/notes/networking-lobbies.md` §"DirectPlay under Wine — measured".
