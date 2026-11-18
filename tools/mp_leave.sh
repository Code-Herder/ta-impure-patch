#!/bin/bash
# mp_leave.sh -- one peer of a network game back at its main menu through the game's own menus,
# from whatever screen it has on top: a game in play, paused or not, a notice over it, a level
# that has ended.
#
#   ./mp_leave.sh <instance>
#
# EACH STEP READS THE SCREEN ON TOP AND TAKES THAT SCREEN'S ONE ACTION. Nothing is assumed about
# the screen an action leads to, because the engine changes the screen on its own while a peer
# leaves: a network level ends by itself once its last human in play holds no unit or the local
# human has won (research/notes/engine/network.md, *The other ends of a network level*), which
# puts ENDMSN on top of whatever menu was open; a human whose commander dies beside its own live
# computer seat gets the watch notice (MSGBOX) and one with none "Continue watching?" (YESORNO
# [INFERRED: the dialog was not met; the surrender's is YESORNO, MEASURED]).
# So an action that fails is not judged: the next read decides again.
#
#   MAINMENU    done.
#   ENDMSN      the level has ended: its MainMenu.
#   MSGBOX      a notice: its OK, which returns to the screen under it.
#   YESORNO     CHOICE1, its yes: the surrender's ("Surrender this battle and return to main
#               menu?") ends the level; "Continue watching?"'s keeps the level, where the next
#               step opens the menu (its CHOICE2 is the one that stops the level, 0x464E55).
#   EXITMENU    MAINMENU, which asks the surrender.
#   *OPT        the in-game options: EXIT.
#   TABMENU     OPTIONS.
#   any other   the game in play. A paused game takes its Pause key first (the pause bit, main
#               +0x38A51 bit 0: the surrender is walked on a running game; any peer's key lifts a
#               pause, MEASURED on one another peer had set), then Tab opens TABMENU.
#   no UI       a teardown or a load answers no snapshot: read again.
#
# THE BOUND. The menus are a chain, each action one screen nearer the main menu, and the engine
# moves the screen on its own at most three times a level (the notice, the dialog, the end), each
# of them one action from the chain or from the main menu. STEPS actions is several times that
# path; past it, or after NO_UI reads in a row with no screen, the leave fails and names every
# screen it read.
#
# Other peers can remain in play after one leaves: each peer needs its own leave. Departure
# roots: research/notes/tadr-port/sim-fixes.md (B5).
set -u

[ $# -eq 1 ] || { echo "usage: mp_leave.sh <instance>" >&2; exit 2; }
INST="$1"
TACLI="$(cd "$(dirname "$0")" && pwd)/tacli"
STEPS=16
NO_UI=12                        # reads in a row, each tacli's own five seconds for a snapshot
PAUSE_WORD='*0x511DE8+0x38A51:2'

# The action's own exit is not read: the level can end under any click, and the screen the next
# step reads is the fact.
ui() { "$TACLI" ui "$INST" "$@" || true; }

# The screen on top, upper case: NONE while no snapshot answers, GONE when the instance is not
# running.
top() {
    local snap gui
    if ! snap=$("$TACLI" ui "$INST" --json 2>&1); then
        case "$snap" in *"is not running"*) echo GONE ;; *) echo NONE ;; esac
        return
    fi
    gui=$(printf '%s\n' "$snap" | sed -n 's/^ *"gui": *"\([^"]*\)".*/\1/p' | head -n 1 | tr 'a-z' 'A-Z')
    echo "${gui:-NONE}"
}

paused() {
    local v
    v=$("$TACLI" peek "$INST" "$PAUSE_WORD" --json 2>/dev/null |
        sed -n 's/.*"value": *"\([0-9][0-9]*\).*/\1/p' | head -n 1)
    [ -n "$v" ] && [ $((v & 1)) -eq 1 ]
}

seen=""
blank=0
k=0
while [ "$k" -lt "$STEPS" ]; do
    gui=$(top)
    seen="$seen $gui"
    case "$gui" in
    MAINMENU.GUI)
        echo "mp_leave: $INST is at its main menu:$seen"
        exit 0 ;;
    GONE)
        echo "mp_leave: $INST is not running:$seen" >&2
        exit 1 ;;
    NONE)
        blank=$((blank + 1))
        if [ "$blank" -ge "$NO_UI" ]; then
            echo "mp_leave: $INST answered no UI snapshot $NO_UI reads in a row:$seen" >&2
            exit 1
        fi
        continue ;;
    esac
    blank=0
    k=$((k + 1))
    case "$gui" in
    ENDMSN.GUI)   ui click MainMenu --timeout 30 ;;
    MSGBOX.GUI)   ui click OK --timeout 20 ;;
    YESORNO.GUI)  ui click CHOICE1 --timeout 30 ;;
    EXITMENU.GUI) ui click MAINMENU --timeout 20 ;;
    *OPT.GUI)     ui click EXIT --timeout 20 ;;
    TABMENU.GUI)  ui click OPTIONS --timeout 20 ;;
    *)
        if paused; then
            seen="$seen(paused)"
            "$TACLI" keys "$INST" pause || true
            # The key toggles, so the bit is read until it changes before anything presses again:
            # a second press on a bit not yet cleared would pause the game anew.
            for _ in 1 2 3 4 5 6 7 8 9 10; do
                paused || break
                sleep 0.5
            done
        else
            "$TACLI" keys "$INST" tab || true
            # Tab toggles too: TABMENU is waited for, so the next read does not press it shut.
            ui wait --gui TABMENU --timeout 20
        fi ;;
    esac
done
echo "mp_leave: $INST is not at its main menu after $STEPS actions:$seen" >&2
exit 1
