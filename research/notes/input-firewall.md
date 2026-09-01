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
live and a running game picks it up within 15 frames.

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
- **Modifiers are held, not interleaved.** A combo posts the modifier down with a
  **150 ms hold** (`MOD_HOLD_MS`); the frame hook posts the up when it expires. An
  interleaved up races the poll; a hold cannot.

Tagged messages are honoured whether or not the shield is armed — arming only decides
whether hardware input is let through *alongside* them.

### Why the WM_CHAR is synthesised

Posted `WM_KEYDOWN` used to become a `WM_CHAR` through TA's own `TranslateMessage`. Tagged
messages never pass through it, so `deliver_key()` runs `ToAscii()` against the *virtual*
keyboard state and dispatches the `WM_CHAR` itself. That is what keeps menu accelerators
and chat typing working, and it makes `shift+key` type an upper-case character.

## Token surface (`tagpu_keys.txt`, i.e. `tacli keys`)

| token | effect |
|---|---|
| `a` `space` `f5` `plus` … | tap: tagged down+up |
| `ctrl+d`, `shift+2`, `alt+f4`, `ctrl+shift+a` | stackable modifier prefixes, modifier held 150 ms |
| `down:<tok>` / `up:<tok>` | explicit hold/release; `<tok>` may be `ctrl`/`shift`/`alt` or `lbutton`/`rbutton`/`mbutton` (→ drag-select: `down:lbutton`, `mouse:x,y`, `up:lbutton`) |
| `pclick:x,y` / `prclick:x,y` | injected click at exact game coordinates, then parks the cursor at view centre (an edge-parked cursor makes the engine scroll forever) |
| `mouse:` `click:` `rclick:` `mouserel:` | injected while armed; fall back to the old SendInput path only when disarmed (SendInput moves the human's real pointer) |
| `char:X`, `keydown:<vk>`, `keyup:<vk>` | tagged raw forms |

## Evidence

**Hardware isolation** — A/B, same script, same hardware click + four-point pointer sweep
across the window, reading the engine's *own* memory mouse (`mouse: game=` in `tagpu.log`,
from `draw_mouse` in `tagpu_overlay.c`):

| shield | engine memory mouse |
|---|---|
| off | (512,384) → **(833,663)** — the human's pointer is driving the game |
| on | (512,384) → **(512,384)** — unmoved |

Note the control needs a *click* first: windowed cnc-ddraw only forwards mouse messages
while `g_mouse_locked`, which activation alone does not set. So the pre-shield hazard is
"human clicks in the window once, then owns the game's cursor from anywhere on the
desktop" (there is no fencing either — `tagpu_nowarp.on`).

**Modifier polls** — the shield logs each hold when it expires:

```
shield: vk=17 released after 150ms, polls=2 down=2
```

VK 17 = `VK_CONTROL`: TA asked twice during the keystroke and was told "down" both times.
`polls=0` would mean TA does not poll that key there and a modifier has to arrive some
other way — that is the diagnostic to read first if a combo misbehaves.

**End to end**: `tacli click` the commander (build panel appears → selected) then
`tacli keys <name> ctrl+d` → the commander self-destructs after TA's ~5 s countdown, the
roster loses its `own=0` unit, and the game shows **DEFEAT** with Losses = 1. First
modifier combo ever landed in this project.

## What this did not change

The launch-time pointer warp (wine's own, `windowed-mode.md` bug 3) is still there and
still mitigated by tacli's save/restore. The shield does not make it moot: it happens
before our window exists.
