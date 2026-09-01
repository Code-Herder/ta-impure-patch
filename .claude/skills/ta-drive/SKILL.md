---
name: ta-drive
description: Launch and drive Total Annihilation from the CLI with tacli — isolated parallel instances, windowed on the real desktop, silent, no intro, scripted keyboard/mouse/camera, skirmish presets, and log/roster queries. Use when asked to run, start, play, test, or drive the game, or to run several game sessions in parallel.
---

# Driving TA from the CLI (tacli)

`tools/tacli` runs the game as **isolated instances**: each has its own gamedir, wine
prefix, wineserver, window, config and logs. Several agent sessions can each own one
instance without colliding, and the human keeps using the desktop meanwhile.

Reference: `research/notes/tacli-design.md` (design), `windowed-mode.md` (the two
bugs this fixed and how they were found), `cmdline-options.md` (every launch knob).
Capture and video work: the **ta-capture** skill.

## Rules of engagement

1. **One instance per session.** Name it after your task (`tacli launch fogtest`).
   Never drive an instance you did not launch — another agent may own it.
2. **Never use xdotool for keys or clicks.** All input goes through tacli, which
   writes the in-process token file the fork consumes. X injection is unreliable and
   has landed keystrokes in the user's unlock dialog (memory: `ta-input-injection-safety`).
   xdotool stays legitimate for *reading* window geometry and for capture.
3. **Silence is the default** and matters to the user: `NoDirectSound` plus six
   registry values plus dropping `music/`. Only pass `--sound` if asked.
4. **Clean up**: `tacli stop <name>` when done; `tacli rm <name>` when the instance
   has no further use. Leaving games running eats a GPU context each.
5. Rebuilt the fork? `tacli launch` re-copies `ddraw.dll` into the instance, so just
   relaunch. A running instance keeps its pinned copy (rebuilds cannot corrupt it).

## The loop

```bash
tools/tacli launch t1 --res 1024x768        # creates on first use, ~2s to window
tools/tacli ls                              # names, pids, windows
tools/tacli keys t1 space                   # menu accelerators
tools/tacli shot t1 -o /tmp/where.png       # engine surface: see where you are
tools/tacli click t1 320 240                # game coords; --right for orders
tools/tacli roster t1 --json                # units + camera eye
tools/tacli stop t1
```

**Menus are stateful — verify, do not assume.** Take a `shot` after each step rather
than firing a canned key sequence; a dropped or extra key lands you in the campaign
screen instead of skirmish. Known-good path from a fresh launch:
`space` (SINGLE) → `s` (Skirmish) → `Return` (Start), with a shot between each, and
`escape` to back out. The first key after launch is often dropped — send a throwaway
(`tab`) first. Then wait for the game proper:

```bash
tools/tacli wait t1 'alive=[1-9]' --timeout 150
```

Skirmish settings come from the registry, no clicking: `--map "Two Continents"`,
`--player 2:2:1:1` (`N:controller[:side[:color]]`, controller 0=off 1=human 2=AI,
side 0=ARM 1=CORE), `--los`, `--mapping`.

## Input details that cost time to learn

- Clicks need `Interface Type=1` (right-mouse orders) — tacli sets it. Select with
  `click`, order with `click --right`. Classic left-click-order resists posted clicks.
- Ctrl/Shift combos do **not** land (TA polls modifier state). Self-destruct, group
  assignment and friends are not scriptable yet — that is phase 1.1 (input firewall).
- `tacli eye X Y` pins the camera (writes both eye and scroll-target, else the engine
  fights back); `tacli eye <name> --release` frees it. Read the settled value from a
  `roster` call before doing coordinate maths.
- Game speed: `keys <name> plus` / `minus` (TA's own feature, up to +10, and negative
  below normal — invaluable for catching fast events or slowing them for capture).

## Observing

- `tacli shot` — TA's own 8bpp surface. Engine truth, and the only view that shows
  engine UI (menus, placement boxes). **Native GPU-rendered units are invisible here.**
- `tacli glshot` — the GL framebuffer: what is actually presented, including our
  passes. Use this to judge our renderer.
- `tacli log <name> -g <regex>` / `tacli wait <name> <regex>` — the fork logs
  `units:`/`native:`/`OWND` lines; `roster` parses the newest unit block.
- Video and frame-by-frame flicker analysis: **ta-capture** skill (60fps or you will
  alias one-present dropouts). Grab the window rect from `tacli ls --json`.

## Arming the GPU passes

```bash
tools/tacli arm t1 native.on=all owndraw.on writeback.on=armcom scaffold.on
tools/tacli arm t1 native.on=off           # clear one
```

Any `tagpu_<x>` trigger file works; value goes into the file (e.g. `all`, `armcom`).

## Things that will bite you

- `pkill -f TotalA.exe` kills your own shell (the pattern matches the wrapper).
  Use `pkill -x` / `pgrep -x`, or just `tacli stop`.
- `tagpu.log` contains binary bytes: always `grep -a` (tacli's `log`/`wait` handle it).
- Two X windows share the title `Total Annihilation` (frame + client), and the user's
  browser/Discord windows match the *substring* — tacli matches exact title + pid.
- The launch briefly warps the pointer to a screen origin (a wine-side quirk, not TA);
  tacli restores it and reports `pointer_restored`. Do not "fix" this with xdotool.
- Instances are cheap in disk (hardlinked prefix, symlinked gamedir) but each running
  game is a real GPU client — a handful at a time, not dozens.
