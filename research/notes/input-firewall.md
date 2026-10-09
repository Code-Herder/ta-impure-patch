# The input firewall ("the shield") — phase 1.1, built and verified 2026-09-01

*The game ignores the human's keyboard and mouse entirely and sees only what an agent
injects. Companion notes: `tacli-design.md` (why this phase exists), `windowed-mode.md`
(the pointer-theft and blanking bugs it sits next to), skill **ta-drive** (how to use it).*

Two problems, one mechanism:

1. **Crosstalk.** Instances are windowed on the human's real desktop. Once anyone clicks
   in a game window, cnc-ddraw sets its mouse lock and from then on the human's pointer
   drives that game's cursor — measured below. A test can be perturbed without anyone
   meaning to.
2. **Combos never landed.** `ctrl+d`, group assignment and friends did nothing, however
   the messages were posted. TA does not read modifiers off the message: it **polls**
   `GetKeyState`, so an interleaved ctrl-down/ctrl-up in the message queue is invisible.

## How it works

Trigger file **`tagpu_shield.on`** next to the exe, same idiom as `tagpu_nowarp.on`.
tacli writes it at create/launch (**on by default**); `tacli shield <name> on|off` flips it
live and a running game picks it up within 15 trigger ticks (about a quarter of a second).

`tagpu_shield.c` / `inc/tagpu_shield.h`:

- **Hardware input dies in the wndproc.** `tagpu_shield_wndproc()` runs first in
  `fake_WndProc` and swallows every hardware key/char/mouse/wheel message while armed.
- **Injected input arrives tagged**, as `WM_TAGPU_KEY` / `WM_TAGPU_CHAR` /
  `WM_TAGPU_MOUSE` (`WM_APP+140..142`), so it passes the same filter that kills the
  hardware kind. The wndproc translates each back into the real `WM_KEYDOWN`/`WM_CHAR`/
  `WM_MOUSEMOVE`/button message and hands it to the game's own window proc.
- **The polls are answered from injected state.** `fake_GetKeyState` /
  `fake_GetAsyncKeyState` return the virtual key state, and `fake_GetCursorPos` returns
  the injected cursor rather than the real pointer (the game polls it ~700× a launch).
  `WM_NCHITTEST` no longer forwards real pointer coordinates either.
- **Focus is the game's.** `fake_GetFocus` answers the game window while armed, because the
  injector is the game's only keyboard. The engine asks in two places and both are gates: the
  movie's frame routine `0x47C3A0` decodes nothing unless `GetFocus()` is the game window, so
  an unfocused instance sits in the movie loop without ever flipping; and the scroll poll's
  off-screen clamp `0x41CF1C` ([engine map](exe-reverse-engineering.html), *The movie
  player*). Disarmed, the real answer passes through, so a player's game that loses focus
  pauses its movie as the stock game does.
- **Modifiers are held, not interleaved.** A key is virtually down from its delivered down
  to its delivered up, and the transport below posts nothing until the event before has been
  handled, so `ctrl+d` is ctrl down, d down, d up, ctrl up, each handled in turn: the poll
  `0x4C1D56` inside the `d` keydown sees ctrl down, as it does for a player.

Tagged messages are honoured whether or not the shield is armed — arming only decides
whether hardware input is let through *alongside* them.

### Why the WM_CHAR is synthesised

Posted `WM_KEYDOWN` used to become a `WM_CHAR` through TA's own `TranslateMessage`. Tagged
messages never pass through it, so `deliver_key()` runs `ToAscii()` against the *virtual*
keyboard state and dispatches the `WM_CHAR` itself. That is what keeps menu accelerators
and chat typing working, and it makes `shift+key` type an upper-case character.

## The transport: one event in flight

`tacli keys` writes one **batch**, `#<id> <tokens>` (the id 1 to 16 hex digits, the file at most
1023 bytes), under a temporary name and hard-links it to `tagpu_keys.txt`; a link never replaces
a file, so the DLL never reads half a batch and a second driver waits for the first batch to be
taken. tacli splits a long list of tokens into batches that fit the file and the DLL's 4096
events, counting each token's events as the DLL parses it (a click is four, `wheel:+64` sixty-four),
and refuses a token that alone makes more before it writes anything. The DLL (`tagpu_input.c`,
game thread, a `GetFileAttributes` probe every fifth trigger tick) takes the file only while no
batch is in progress, parses it whole -- an unknown token, a missing id or more than 4096 events
refuses the batch before anything is posted -- and logs `input: batch <id> taken: <tokens>`.

Then it posts **one event at a time**. Each tagged message carries a 16-bit sequence number
(`wParam` bits 16..31 for keys and the mouse, `lParam` for a character); the wndproc hands it
back once it has delivered the message, and the event is **consumed** when that has happened
and, at a later trigger tick, both of the engine's input rings read empty from their own
indices (`tagpu_gui_input_rings`: the mouse ring at graphics globals `+0x18E`/`+0x192`, the key
ring at `+0x16E`/`+0x172`). The window procedure pushes while it handles the message, and each
engine site that pops an entry hands it to its handler before the frame draws and flips
([engine map](exe-reverse-engineering.html), *Who pushes the two rings, and who pops them*), so
an empty ring after the delivery is the engine having handled the event. A move or a wheel notch
pushes nothing: delivery and a later tick consume it. The next event is posted only onto empty
rings. After the last, `input: batch <id> done: N events; engine E, menu M, dropped D, untracked
U` (the sprocket took M, the display-only ring dropped D, SendInput sent U with the shield
disarmed; a SendInput token counts as consumed when it is sent, since nothing can tag it); a
post that fails refuses the rest. While a batch runs it logs at most one line every two
seconds: `input: batch <id> progress: c/n events consumed` when an event was consumed since its
last line, `input: batch <id> waiting: event i/n <token> delivered=d mouse=p key=q` when none
was.

**No clock decides a live game's outcome; a stall ends by abandon.** `tacli keys` waits for the
done line as long as the DLL's queue moves: every `taken`, `progress` or end line of any batch
restarts its 60-second bound, so a slow game, a long batch or a click whose handler starts a
level load is waited out. It fails at once on `REFUSED`, and when the game exits. Every pop in
the engine is conditional, though: in the message-box loop `0x49F9C0` (`MSGBOX.GUI`, its one
caller) a press outside the box's panel is never popped, and in game a key would wait while the
in-game key pass is skipped and the GUI pump does not take it. Then nothing moves for 60 s, and
tacli writes `tagpu_keys.<id>.abandon`. The
DLL, which looks for it every fifth trigger tick while a batch runs, ends the batch with `input:
batch <id> ABANDONED: event i/n <token> mouse=p key=q; released <vks>`, posting an up for every
key and button the batch's events left down, and tacli dies with that line. A batch still
standing untaken behind a stalled one is deleted instead (`ABANDONED: not taken; batch <other>
at event ...`). A game that does not answer the abandon in 30 s more has stopped ticking, and
tacli says so. The abandon ends the batch, not the engine's state: the record the engine would
not pop is still at the head of its ring, so the next batch waits on that ring until the engine
pops it.

What that buys: a burst cannot overflow a ring (the push drops silently when full, and a batch
of ten clicks used to overflow the mouse ring, which holds 19 records, in one drain); no release
overtakes the press whose handler polls it; a modifier is down for exactly the events between its down and its up;
and a return from `tacli keys` means the input's direct effects -- orders, selection, the
placement rectangle -- are written. One residual: the in-game setup's flush `0x4C2BB0` empties
the mouse ring unhandled, so an event in flight across a level's start would be acknowledged;
the harness cannot reach it, because `tacli keys` returns only on consumption and a Start click
is popped before the setup runs *[INFERRED]*.

Players are unaffected: nothing runs without a batch file.

## Token surface (`tagpu_keys.txt`, i.e. `tacli keys`)

| token | effect |
|---|---|
| `a` `space` `f5` `plus` … | tap: tagged down+up |
| `ctrl+d`, `shift+2`, `alt+f4`, `ctrl+shift+a` | stackable modifier prefixes: each modifier down, the key down and up, the modifiers up in reverse |
| `down:<tok>` / `up:<tok>` | explicit hold/release; `<tok>` may be `ctrl`/`shift`/`alt` or `lbutton`/`rbutton`/`mbutton` (→ drag-select: `down:lbutton`, `mouse:x,y`, `up:lbutton`) |
| `pclick:x,y` / `prclick:x,y` | injected click at exact game coordinates, then parks the cursor at view centre (an edge-parked cursor makes the engine scroll forever) |
| `mouse:` `click:` `rclick:` `mouserel:` | injected while armed; fall back to the old SendInput path only when disarmed (SendInput moves the human's real pointer) |
| `char:X`, `keydown:<vk>`, `keyup:<vk>` | tagged raw forms (vk 1..255) |
| `wheel:N` | N notches (at most 64 a token) where the pointer is |
| `mouselock` | the fork's mouse lock |

## Evidence

**Hardware isolation** — A/B, same script, same hardware click + four-point pointer sweep
across the window, reading the engine's *own* memory mouse (`mouse: screen=` in `tagpu.log`,
from `draw_mouse` in `tagpu_overlay.c`):

| shield | engine memory mouse |
|---|---|
| off | (512,384) → **(833,663)** — the human's pointer is driving the game |
| on | (512,384) → **(512,384)** — unmoved |

Note the control needs a *click* first: windowed cnc-ddraw only forwards mouse messages
while `g_mouse_locked`, which activation alone does not set. So the pre-shield hazard is
"human clicks in the window once, then owns the game's cursor from anywhere on the
desktop" (there is no fencing either — `tagpu_nowarp.on`).

**The transport** [MEASURED 2026-10-09, an instance at the main menu, then the
`resource-snapping` scenario paused]:

| | before (appended file, every token posted at once) | after (one event in flight) |
|---|---|---|
| 5,000 one-token `pmove` batches, one driver, every 15±10 ms | 4,997 logged: three batches lost whole (ids 3716, 4460, 4558) | 5,000 taken, 5,000 done |
| two concurrent drivers, 2,500 batches each | -- | 2,500 + 2,500 taken and done, no `waiting` line |
| ARMCK selected, one `keys down:shift`, 12 × `prclick` on open ground, `up:shift` | 10 Moves: the last two clicks overflowed the mouse ring | 12 Moves, in order (50 events) |

**The abandon** [MEASURED 2026-10-09, a skirmish, the `input.stall` lever]: with shift held
through `down:shift q up:shift` the chat line read `<Player> Q`. With the lever armed,
`tacli keys <i> down:shift pmove:500,400` logged 29 `waiting` lines at event 1/2 and, 60.2 s
after it was sent, `ABANDONED: event 1/2 down:shift mouse=0 key=0; released 0x10`, and tacli
exited 1 with that line. With the lever removed, the next batch `return z return` was done in
one call and the chat line read `<Player> z`: the abandon had let shift up. Two drivers at once,
the first `down:lbutton pmove:1,1`, the second `pmove:2,2` placed a second later: both ended
`ABANDONED` (`released 0x01`, then the second taken and ended), and the next batch was done.
A press outside `SELMAP.GUI`'s panel in the shell is consumed (0.2 s): IdleTick runs there, so
the lever is the only on-demand way to a batch the engine does not consume.

**End to end** [MEASURED 2026-09-01, with the hold this transport replaced]: `tacli click` the
commander (build panel appears → selected) then
`tacli keys <name> ctrl+d` → the commander self-destructs after TA's ~5 s countdown, the
roster loses its `own=0` unit, and the game shows **DEFEAT** with Losses = 1. First
modifier combo ever landed in this project.

## What this did not change

The launch-time pointer warp (wine's own, `windowed-mode.md` bug 3) is still there and
still mitigated by tacli's save/restore. The shield does not make it moot: it happens
before our window exists.
