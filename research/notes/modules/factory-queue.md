# Factory queue: a hundred at a click

**Ctrl+Shift+click** on a unit button in a factory's build menu queues 100 of the type, and
**Ctrl+Shift+right-click** takes 100 off. A plain click is still ±1 and Shift+click is still ±5.
This is TADR's binding. Ctrl alone changes nothing, as in TADR.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). The engine side is in the engine map: the click in [UI](../engine/ui.html), *The factory branch, byte for byte*; the queue's count nodes and `0x43B0B0` in [recorder](../engine/recorder.html).

## What it does (`tagpu_factoryq.c`)

The build menus' handler `0x41AA00` ends, for a factory, in one call: `0x419B00(name, unit,
amount)` at `0x41AC3C`, where the amount is ±1, or ±5 when the engine's key sampler `0x4C1B80`
answered Shift. The module re-points that call's rel32 to a stdcall wrapper. When the amount is ±5
and the same sampler answers Ctrl (`0xFA`), the wrapper passes ±100. It then calls `0x419B00`
itself. Everything after the click is the engine's own:

- `0x43B0B0` adds the amount to the last count node of the type, or appends one.
- The factory handler `0x402640` builds from the head and takes one off per unit finished.
- The button's `+N` label is refreshed by `0x4199B0`.

The engine has no limit to raise. The count is a dword, the label buffer is 128 bytes, and a
take-off larger than a node empties it and carries the rest to the last remaining node of the
type, toward the head: `0x43B0B0` rescans from the head on each pass and takes the last match
(`0x43B149..0x43B1DC`), as stock's −5 does.

**What it reaches.** It reaches only the human click. The AI's two calls of `0x419B00` keep the
engine's bytes. A stockpile button (MAKENUKE, MAKEANTI) takes the same branch, so Ctrl+Shift
queues 100 missiles there, as in TADR.

**Install.** It is installed at DLL attach and always on; there is no arm file and no setting. It
installs only when `0x41ABFA..0x41AC3B` match the engine's bytes, with the calls of `0x4C1B80`
and `0x4AB6B0` and the queue call decoded. The rule "±5 means Shift" holds only for that code. On
any other exe the log says `factoryq: … holds other bytes; Ctrl+Shift stays 5` and the click is
stock's. The armed line is `factoryq: armed (0x41AC3C): Ctrl+Shift+click queues 100`.

**Replay and multiplayer.** The module keeps no state. The queue it makes is the engine's own
count nodes on the owner's factory, which the recorder already carries
([demo recorder S2](../tadr-port/demo-recorder-s2-orders.html), *Factory queues and rally
points*). On a puppet host the call acts on no unit, as stock's does, because the controlled
block is empty.

## How it was verified

`scenarios/exit-sort.json` (an ARMLAB), driven through tacli's injected keys, read through
`tacli ui show ARMPW`, MEASURED 2026-11-15. From an empty queue the button read `+5` after
Shift+click, `+105` after Ctrl+Shift+click, `+106` after a click and `+107` after Ctrl+click.
Then Ctrl+Shift+right-click gave `+7`, Shift+right-click `+2`, and Ctrl+Shift+right-click an
empty label.

With commanders kept and full storage, and game speed raised, a queue of 100 went `+99`, `+97`,
`+95`, `+93` at 20-second reads while the Peewees came off the line. The sidebar draws `+200` on
the button legibly at 1024x768.

## Gaps

- No other modifier steps (for example a ±20 on Ctrl alone). The owner chose TADR parity.
- Not run beside a mod exe that patches `0x41AC04..0x41AC3B`; such an exe gets the stock click and
  the log line above.
