#!/bin/bash
# mp_lobby.sh -- drive tacli instances from the shell into one live TA game.
#
#   ./mp_lobby.sh [--map <map>] <host-instance> <join-instance> [<join-instance>...]
#
# Two to ten players: the host and one to nine joiners, which join in the order
# given. Every instance must already be running, launched with --dplay (the host
# with --free-dplay-port as well); wine's builtin DirectPlay cannot create a
# session at all, so without native DirectPlay this script stalls on SELGAME. See
# research/notes/networking-lobbies.md. A map seats only the player counts its
# .ota lists (Town & Country takes ten, Two Continents two).
#
# Everything goes through `tacli ui`, which auto-waits on each gadget, so the
# script is a straight line with no sleeps of its own.
#
# Three things here are not obvious and each one cost a run to find:
#
#  1. With NATIVE DirectPlay the provider list is four rows in a different order
#     from wine's builtin -- TCP/IP is row 3, not row 0. Select it by name.
#  2. A text field must be CLICKED before it is filled. Typing into an unfocused
#     field sends the first character to the screen as a quickkey (on SELGAME 'J'
#     is JOINGAME's) and drops the rest, which is what the old "the address field
#     kept only 1" note was really about.
#  3. START ungreys only once EVERY player is ready -- the host included. The
#     host's own row is READY0 on its own screen; each client lists itself first.
set -eu

USAGE='usage: mp_lobby.sh [--map <map>] <host-instance> <join-instance> [<join-instance>...]'
MAP=""
if [ "${1:-}" = "--map" ]; then MAP="${2:?$USAGE}"; shift 2; fi
[ $# -ge 2 ] && [ $# -le 10 ] || { echo "$USAGE" >&2; exit 2; }
HOST="$1"; shift
JOINS=("$@")
TACLI="$(cd "$(dirname "$0")" && pwd)/tacli"
PROVIDER='Internet TCP/IP Connection For DirectPlay'

ui() { "$TACLI" ui "$@"; }

# click-then-fill (note 2), and skip when the field already reads what we want:
# TA remembers the address and the nickname between runs, and `fill` clears with
# backspaces, which cannot always reach the whole field -- refilling a correct
# field is how you turn a working screen into "ADDRESS still reads '127.0.0.'".
field() {
    local cur
    cur=$(ui "$1" show "$2" 2>/dev/null | sed -n 's/^text  *//p')
    if [ "${cur:-}" = "$3" ]; then echo "$2 already reads '$3'"; return 0; fi
    ui "$1" click "$2" >/dev/null
    ui "$1" fill "$2" "$3"
}

to_selgame() {   # <instance> -- main menu through the provider screen to SELGAME
    ui "$1" click MULTI
    ui "$1" select DPLAY "$PROVIDER"
    ui "$1" click SELECT --timeout 25
    field "$1" ADDRESS 127.0.0.1
    ui "$1" click OK --timeout 30
    ui "$1" wait --gui SELGAME --timeout 30
}

echo "== $HOST: hosting =="
to_selgame "$HOST"
field "$HOST" NICKNAME "$(echo "$HOST" | tr 'a-z' 'A-Z')"
ui "$HOST" click STARTNEW --timeout 30
field "$HOST" GAMENAME "TACLI"
field "$HOST" NICKNAME "$(echo "$HOST" | tr 'a-z' 'A-Z')"
ui "$HOST" click OK --timeout 30
ui "$HOST" wait --gui LOUNGE2 --timeout 30

if [ -n "$MAP" ]; then
    echo "== $HOST: map '$MAP' =="
    ui "$HOST" click MAP --timeout 25
    ui "$HOST" select MAPNAMES "$MAP" --timeout 120
    ui "$HOST" click LOAD --timeout 25
fi

for JOIN in "${JOINS[@]}"; do
    echo "== $JOIN: joining =="
    to_selgame "$JOIN"
    field "$JOIN" NICKNAME "$(echo "$JOIN" | tr 'a-z' 'A-Z')"
    ui "$JOIN" click JOINGAME --timeout 30
    ui "$JOIN" wait --gui LOUNGE2 --timeout 30
done

if [ -n "${MP_NO_START:-}" ]; then
    echo "== all $((${#JOINS[@]} + 1)) in the battle room; MP_NO_START set, stopping here =="
    exit 0
fi

echo "== all ready, starting =="
for JOIN in "${JOINS[@]}"; do
    ui "$JOIN" click READY0 --timeout 20      # each client lists itself as row 0
done
ui "$HOST" click READY0 --timeout 20
ui "$HOST" click START --timeout 30
for INST in "$HOST" "${JOINS[@]}"; do
    "$TACLI" wait "$INST" 'alive=[1-9]' --timeout 120
done
echo "live: $HOST (host) and ${JOINS[*]} are in one game"
