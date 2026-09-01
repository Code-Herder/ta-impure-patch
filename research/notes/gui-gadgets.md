# GUI gadgets — the live menu/panel tree, and how to drive it from text

*Static map of TA's in-memory GUI system: where the active screen lives, how a gadget
record is laid out, what each gadget type is, and which engine functions can serve as
oracles. Written to back the **`tacli ui`** layer (Playwright-CLI-style snapshot +
act-by-name), specified in `tacli-design.md` §Phase 1.3. All addresses are VAs for our
pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`);
disassembly via `objdump -d -M intel`.*

Evidence tags as in `ui-markers.md`: **[BINARY-VERIFIED]** = instructions read for this
build this session. **[CORPUS]** = `tools/tamem_ghidra.h` / TADR name, self-consistent
with the verified layout but not observed in code this session. **[LIVE]** = read off a
running instance and cross-checked against a `tacli shot` on 2026-09-01.
**[INFERRED]** = reading not yet confirmed.

Companion notes: `ui-markers.md` (per-unit *world* markers — a different subsystem),
`resolution.md` (the 640×480 front-end lock and the in-game view rect),
`tacli-design.md` (the CLI this serves).

---

## Summary — the six things to know

1. **One chain reaches every screen.** `main → +0x519 GUIInfo → +0x18 TheActive_GUIMEM
   → +0x04 ControlsAry`. Shell menus, modal dialogs and the **in-game side panel with its
   build buttons** are all the same structure — there is no second UI system to handle.
   [BINARY-VERIFIED]
2. **Gadget records are `pack(1)`, stride `0x15B` (347 bytes)**, packed immediately after
   the panel record. `ControlsAry[0]` is the panel (its `totalgadgets` at `+0xB6` is the
   count); gadget *i* is at `ControlsAry + i*0x15B`. [BINARY-VERIFIED]
3. **`id` is the type and `assoc` is a radio-group id.** The draw dispatcher switches on
   `id-1` through a 13-entry table at `0x4A962C`: 1=button, 2=listbox, 3=textfield,
   4=slider, 5=label, 6=surface, 11=picture, 13=timer. `id 0` (the panel) is not in the
   table. [BINARY-VERIFIED]
4. **Gadget coordinates are relative to the panel record's origin**, in game space
   (the space `tacli click` takes) — no scaling anywhere. Shell menus are full-screen
   panels at `(0,0)`, so their gadgets read as absolute; the **in-game build panel sits
   at `(0,128)`**, below the minimap, and its gadgets are measured from that corner.
   `click point = (panel.xpos + xpos + width/2, panel.ypos + ypos + height/2)`. [LIVE]
5. **The engine hands us three oracles**: a name→index lookup (`0x49FE60`), a
   "is this screen on top" test (`0x4AB060`), and `GUIInfo->UIChange_f` (`main+0x579`) —
   the index of the gadget the engine itself considers last actuated, i.e. free
   confirmation that a synthesized click landed where we aimed.
6. **The type structs are a union over the same bytes.** `+0xB6` is `totalgadgets` in a
   panel and `text[128]` in a button. Any walker must switch on `id` before reading past
   `+0x33`.

---

## 1. The live chain

```c
TAdynmemStruct* main = *(void**)0x511DE8;
GUIInfo*        gi   = (GUIInfo*)((char*)main + 0x519);
GUIMEMSTRUCT*   top  = *(void**)((char*)main + 0x531);   /* gi->TheActive_GUIMEM */
GUI0IDControl*  ctrl = top->ControlsAry;                 /* top + 0x04 */
```

`main+0x531` is used verbatim by the engine at `0x49588B` (`ApplySelectUnitMenu`) and
`0x491DB3` (`UpdateIngameGUI`). [BINARY-VERIFIED]

### 1.1 `GUIMEMSTRUCT` — one loaded screen

| Off | Field | Evidence |
|---|---|---|
| `+0x00` | `per_active` → the GUI **below** this one (LIFO stack link) | [BINARY-VERIFIED] `0x4A969B` |
| `+0x04` | `ControlsAry` → panel record, gadgets follow | [BINARY-VERIFIED] `0x4AB06B`, `0x49FE7D` |
| `+0x08` | `OnCommand` — `__stdcall void(GUIInfo*)`; reads the actuated index from `gi->UIChange_f`, **not** an argument | [BINARY-VERIFIED] `0x4A967F` |
| `+0x10` | flags (bit `0x800` re-runs a stage on pop) | [BINARY-VERIFIED] `0x4A96B7` |
| `+0x14` | `Active_b` — set to 1 on the GUI newly exposed by a pop | [BINARY-VERIFIED] `0x4A96A7` |
| `+0x4F` | `GUIName[16]` | [CORPUS] |

### 1.2 `GUIInfo` (at `main+0x519`)

| Off | Abs | Field | Evidence |
|---|---|---|---|
| `+0x18` | `main+0x531` | `TheActive_GUIMEM` (topmost = the only interactive screen) | [BINARY-VERIFIED] |
| `+0x3C` | `main+0x555` | cursor / rect block, 6 dwords; `[0]`,`[1]` are the pointer x,y | [BINARY-VERIFIED] `0x49FD20` |
| `+0x60` | `main+0x579` | `UIChange_f` — index of the last actuated gadget, or `-1` | [BINARY-VERIFIED] `0x4A9673`, `0x4AB0A4` |
| `+0x64`, `+0x68` | | reset to `-1` alongside `UIChange_f` on pop (focus/hot, [INFERRED]) | [BINARY-VERIFIED] reset only |

### 1.3 The stack is real

`GUI_Pop 0x4A9660(GUIInfo*)` fires `top->OnCommand(gi)`, runs stage 2 of
`GUI_StageUpdateDraw 0x4A81E0`, then relinks `gi->TheActive_GUIMEM = top->per_active`,
marks the newly exposed GUI active (`+0x14 = 1`) and frees the old one via `0x4D85A0`.
[BINARY-VERIFIED]

`UpdateIngameGUI 0x491D70` is the clearest consumer: it loops
`while (main+0x531) { if (IsOnTop(gi, main+0x37EA0)) break; GUI_Pop(gi); }` — popping
until the expected in-game screen (name held at `main+0x37EA0`) is on top.
[BINARY-VERIFIED]

**Consequence for tooling:** only the top GUI is interactive. A snapshot should report the
top screen's gadgets in full and the names beneath it as a breadcrumb — listing covered
gadgets would advertise affordances that cannot be actuated.

---

## 2. The gadget record

Stride **`0x15B`**, `pack(1)`. Proven by `GUI_FindGadgetByName 0x49FE60`, which starts at
`ControlsAry + 0x15D` (= record 1 + `name` at `+0x02`) and does `add edi,0x15B` per
iteration, bounded by `[ControlsAry+0xB6] + 1`. [BINARY-VERIFIED]

### 2.1 Common header (every type)

| Off | Type | Field | Evidence |
|---|---|---|---|
| `+0x00` | `u8` | **`id`** — gadget type | [BINARY-VERIFIED] `0x4A916D`, `0x4A4DA4` |
| `+0x01` | `u8` | **`assoc`** — radio/group id | [BINARY-VERIFIED] `0x4A6A71` |
| `+0x02` | `char[16]` | **`name`** — the selector | [BINARY-VERIFIED] `0x49FE7D` + `strncmp(...,0x10)` |
| `+0x13` | `i16` | **`xpos`** | [BINARY-VERIFIED] `0x49FD3D`, `0x4AD3BB` |
| `+0x15` | `i16` | **`ypos`** | [BINARY-VERIFIED] `0x49FD49` |
| `+0x17` | `i16` | **`width`** | [BINARY-VERIFIED] `0x4A1BB5` |
| `+0x19` | `i16` | **`height`** | [BINARY-VERIFIED] `0x4A1B8F` |
| `+0x1B` | `i32` | `attribs` | [BINARY-VERIFIED] `0x4A3F..` |
| `+0x1F` / `+0x23` | `i32` | `colorf` / `colorb` | [CORPUS] |
| `+0x27` / `+0x28` | `u8` | `texturenumber` / `fontnumber` (`+0x28` is also used as a group ordinal by the listbox and textfield handlers — [INFERRED]) | [CORPUS] |
| `+0x29` | `u8` | **`active`** — zero ⇒ the draw loop skips the gadget entirely | [BINARY-VERIFIED] `0x4A915D` |
| `+0x2A` | `u8` | `commonattribs` | [CORPUS] |
| `+0x33` | `char[128]` | `help` — tooltip | [CORPUS] |

The TDF keys these come from are parsed by `GUI_ParseCommonFields 0x4AD350`
(`COMMON` `0x509A8C`, `id` `0x509A88`, `assoc` `0x509A80`, `name` `0x503884`,
`xpos` `0x509A78`, …). [BINARY-VERIFIED]

### 2.2 Panel — `id 0`, `ControlsAry[0]`

| Off | Field | Evidence |
|---|---|---|
| `+0xB4` | `gaffile` | [CORPUS] |
| `+0xB6` | **`totalgadgets`** (`i16`) | [BINARY-VERIFIED] `0x49FE67`, `0x4A0C07` |
| `+0xBC` | `OFFSCREEN*` | [CORPUS] |
| `+0xCC` | **`crdefault[16]`** — gadget Enter activates (`Start` on SKIRMISH) | [LIVE] |
| `+0xDC` | **`escdefault[16]`** — gadget Escape activates (`PrevMenu`) | [LIVE] |
| `+0xEC` | **`defaultfocus[16]`** (`SINGLE` on MAINMENU) | [LIVE] |
| `+0xFC` | `panel[16]` — background art name | [CORPUS] |

`ControlsAry[0].name` is the **screen name** — `GUICONTROL_IsOnTop 0x4AB060` compares
exactly this against a 16-byte string. [BINARY-VERIFIED]

### 2.3 Button — `id 1`

| Off | Field | Evidence |
|---|---|---|
| `+0xB6` | `text[128]` | [CORPUS] |
| `+0x136` | `stages` — number of states (0/2 = plain button or toggle, >2 = cycle) | [LIVE] |
| `+0x137` | `status_curnt` — current stage | [LIVE] |
| `+0x138` | `status_init` (`i16`) | [BINARY-VERIFIED] `0x4A6A95` |
| `+0x13A` | `quickkey` — **`u8`**, the ASCII accelerator (`'S'` for SINGLE, `'E'` for EXIT, matching the underlined letter on screen). The corpus calls it an `i16`; the high byte is a separate field. | [LIVE] |
| `+0x13C` | `grayedout` (`i32`) | [CORPUS] |

### 2.4 Listbox — `id 2`

| Off | Field | Evidence |
|---|---|---|
| `+0xBA` | `selected_i` (`i16`) | [CORPUS], read at `0x4A1F89` |
| `+0xBC` | item count / scroll index | [BINARY-VERIFIED] used as the index arg at `0x4A1D0F` |
| `+0xC2` | **list handle** — passed to the node accessor | [BINARY-VERIFIED] `0x4A1D09` |
| `+0xDA` | `itemheight` (`i16`) | [CORPUS], read at `0x4A1CE.` |

Items are fetched by `0x4B6AF0(handle@+0xC2, index@+0xBC) -> node*` — a generic
list-node accessor. The **node layout is unidentified**; see §7. [BINARY-VERIFIED call,
[INFERRED] semantics]

### 2.5 Textfield (`id 3`) and slider (`id 4`)

**The corpus has no `GUI3IDControl` or `GUI4IDControl`.** `tamem_ghidra.h` defines
`GUI0/1/2/5/6/9` only, so these two types have no documented per-type layout.

- Textfield: `GUIGADGET_SetText 0x4A0BF0` is generic (name lookup, then write), so
  `text[128] @ +0xB6` is the likely content field. [INFERRED]
- Slider: `GUI_SliderUpdate 0x4A3EF0` touches `+0x136` and `+0x142` beyond the common
  header; the position value is one of them. [INFERRED]

---

## 3. The type table

`0x4A9176`: `cmp eax, 0xC; ja default; jmp [eax*4 + 0x4A962C]` where `eax = id - 1`
(`0x4A9173..0x4A9175`), i.e. **valid ids are 1..13**; `id 0` falls through to the default
and is never drawn. The gadget is skipped outright if `active (+0x29)` is zero
(`0x4A915D`). [BINARY-VERIFIED]

| `id` | Branch | Handler | Type |
|---|---|---|---|
| 0 | — (default) | — | **panel/window** (`ControlsAry[0]`) |
| 1 | `0x4A91A5` | `GUI_ButtonDraw 0x4A5F40` | **button** |
| 2 | `0x4A91C7` | `GUI_ListboxBuild 0x4A1B40` | **listbox** |
| 3 | `0x4A92DF` | `0x4A4D70` | **textfield** |
| 4 | `0x4A9301` | `GUI_SliderUpdate 0x4A3EF0` | **slider / scrollbar** |
| 5 | `0x4A9345` | `GUI_LabelDraw 0x4A56B0` | **label** |
| 6 | `0x4A9367` | `0x4A4980` | surface |
| 7–9 | `0x4A940C` | — | not drawn (id 7 is scanned by the textfield handler) |
| 10 | `0x4A93EB` | `0x4A4C90` | [INFERRED] |
| 11 | `0x4A9186` | `GUI_BlitToFramebuffer 0x4B0230` | picture |
| 12 | `0x4A9199` | `0x4A5F40` | button-like |
| 13 | `0x4A93AB` | `GUI_TimerState 0x4A4660` | timer / animation |

---

## 4. Coordinates

**Gadget coordinates are panel-relative, in game space.** No scaling is involved
anywhere; the only transform is the panel origin.

```
screen = (panel.xpos + gadget.xpos, panel.ypos + gadget.ypos)
click  = screen + (width/2, height/2)
```

Where `panel` is `ControlsAry[0]`'s own rect. Measured live:

| Screen | Panel rect | Consequence |
|---|---|---|
| `MAINMENU.GUI` (and every shell menu) | `[0, 0, 640, 480]` | gadget coords *are* screen coords |
| `ARMCOM1.GUI` (in-game build panel) | `[0, 128, 128, 352]` | everything shifts down by 128 |

[LIVE] `ARMSOLAR`'s raw rect is `[0,27,64,64]`; plus the panel origin its centre is
`(32,187)`, which lands on the solar icon in a `tacli shot`. Reading the raw value as
absolute puts the click at `(32,59)` — inside the minimap, where it does nothing. This
was the one thing the static reading got wrong, and only a pixel cross-check caught it:
`0x49FD20` subtracts the *gadget's* xpos/ypos from the cursor block, which is true but
says nothing about the panel term.

The claim still holds that no *scaling* is needed. Both confirmations of the shared
space stand:

1. `0x49FD20(GUIInfo*, gadget*, out rect6*)` copies the cursor block from `gi+0x3C` and
   subtracts the gadget's `xpos`/`ypos` — gadget and pointer share one space.
   [BINARY-VERIFIED]
2. `resolution.md` puts the in-game view rect at `{128,32,639,447}` for 640x480, i.e.
   the left **128 px is the side panel** — exactly the `ARMCOM1` panel width measured
   above, and exactly the strip Total Mayhem's 2x6 grid of 64x64 build gadgets tiles
   (`deep-total-mayhem.md`).

The injected-click path takes the same space: `tagpu_shield.c:242` clamps to
`g_ddraw.width/height` with the comment *"the game expects game-space coordinates in
lParam"*.

**The front end is atom-locked to 640x480 regardless of `--res`** (`resolution.md` §3)
while the in-game panel uses the requested mode — confirmed live: `MAINMENU.GUI` reports
a 640x480 surface in an instance launched at 1024x768, and `ARMCOM1.GUI` reports
1024x768. A snapshot must print the surface size so the two are never confused.

## 5. Engine helpers — oracles and models

| VA | Signature (verified) | Use to us |
|---|---|---|
| `0x49FE60` | `stdcall int GUI_FindGadgetByName(GUI0IDControl* ctrls, const char* name)` → index or `-1`; `ret 8` | The name→index rule we mirror. Case-insensitive, 16-byte bounded. |
| `0x4AB060` | `stdcall BOOL IsOnTop(GUIInfo*, const char* name)`; `ret 8` | "Am I on screen X" — the breadcrumb/`wait --gui` semantic |
| `0x4A9660` | `stdcall void GUI_Pop(GUIInfo*)` | Why the stack is a stack |
| `0x49FD20` | `stdcall void ToGadgetLocal(GUIInfo*, gadget*, rect6* out)`; `ret 0xC` | Proves the coordinate space |
| `0x4A0BF0` | `GUIGADGET_SetText(GUIInfo*, const char* name, …)` | The direct-write `fill` we deliberately did **not** take |
| `0x4A6A40` | `stdcall void(GUIInfo*, int idx)`; clears `status_init` on every `id==1` gadget sharing `assoc`, redrawing each | TA's radio-group reset — why clicking one button silently clears another |
| `0x4B6AF0` | `(list, index) -> node*` | Gate for listbox items (§7) |
| `0x495860` | `ApplySelectUnitMenu` — presses gadget `"STOP"` (`0x502714`) on the top GUI | Confirms in-game order buttons are ordinary named gadgets |

`main+0x37EA0` holds the name of the expected in-game screen. [BINARY-VERIFIED]

---

## 6. The screen inventory

Every `.GUI` the binary can load, from its string table — this is the complete navigable
surface. [BINARY-VERIFIED, strings]

**Front end:** `MAINMENU` `SINGLE` `NEWGAME` `SKIRMISH` `SELMAP` `VIEWMAP` `LOADGAME`
`SAVELIST` `LOADLIST` `MSNBRIEF` `BRIEFING` `ENDMSN` `LOGOSEL` `STARTOPT` `RESTRICT2`
**Options:** `PREFS` `VISUALS` `SELVMODE` `VISUALRT` `SOUNDSRT` `MUSICRT` `SPEEDS`
`SPEEDSRT` `CONTROL` `ARMOPT` `GAMEOPTIONS`
**Multiplayer:** `NEWMULTI` `TCP` `SERIAL` `MODEM` `SELGAME` `SELPROV` `LOUNGE2`
`ENDMULTI` `ALLIES` `SHARE` `TALK` `TALK2` `REPORT` `TIMEOUT`
**Dialogs:** `MSGBOX` `YESORNO` `CONFIRM` `CHOICE3` `INPUT` `FILEREQ` `NOTEXIST`
`CDCHECK` `EXITMENU` `RESTART` `HELP`
**In game:** `%sMAIN2.GUI` (`0x509570`, used at `0x497BA4`) · `%sGEN.GUI` (`0x50282C`) ·
`BUILDER.GUI` (`0x502844`) · `TABMENU` · `UNITINFOx`

**Build pages are `<UNITNAME><page>.GUI`** — `sprintf` at `0x41B286` with the format
`%s%d.GUI` (`0x502838`), the `%s` being a unit-name buffer built immediately above. So a
builder's menu is per-unit and paged, and page *n+1*'s gadgets live in a **file that is
not loaded** while page *n* is up. [BINARY-VERIFIED]

---

## 7. What is not known yet

Two one-field gaps, each gating exactly one verb. Neither blocks anything currently
driveable, because both surfaces are already bypassed by better paths (`--map`/`--player`
write the registry; sound is off by default; speed is `keys +`/`-`; resolution is
registry).

1. **Listbox item strings.** Read `0x4B6AF0` and identify the string offset in a node →
   unlocks item enumeration and `ui select`.
2. **Slider position field.** Disambiguate `+0x136` vs `+0x142` in `GUI_SliderUpdate
   0x4A3EF0` → unlocks slider `ui set`.

Until then a snapshot must render these as `items=?` / `value=?` — **"not implemented",
never "empty"**. An agent that reads a missing capability as an empty list will conclude
the screen is broken and navigate away.

Still unconfirmed after the first live pass, because no screen exercised them yet:
**`help` (`+0x33`)** read empty on every gadget of MAINMENU / SINGLE / SKIRMISH /
ARMCOM1 — either those screens carry no tooltips or the offset is wrong, and a screen
that definitely has help text will settle it. **`grayedout` (`+0x13C`)** has never been
seen non-zero; note that `SINGLE.GUI`'s unavailable `AnyMsn` button is `active=0`, not
grayed, so unavailability in TA is expressed through `active` at least some of the time.

## 8. What the first live run settled

[LIVE, 2026-09-01, instance launched at 1024x768]

- **The in-game screen is a stack, and the build menu is pushed onto it.** With nothing
  selected the top GUI is `ARMMAIN2.GUI` — three labels (`KILLS`, `LOSSES`,
  `TOTALUNITS`), no buttons. Selecting a builder pushes `ARMCOM1.GUI` on top of it, and
  the breadcrumb reads `under: ARMMAIN2.GUI`. So "top GUI's gadgets + names beneath" is
  the right model for in-game as well as for menus.
- **Build gadgets are named after the buildable unit**: `ARMSOLAR`, `ARMWIN`,
  `ARMESTOR`, `ARMMSTOR`, `ARMMEX`, `ARMMAKR` on page 1; `ARMLAB`, `ARMVP`, `ARMAP`,
  `ARMSY` on page 2.
- **Order buttons live in the same GUI** as the build grid: `ARMMOVE`, `ARMSTOP`,
  `ARMPATROL`, `ARMATTACK`, `ARMDEFEND`, `ARMBLAST`, with quickkeys `m s p a g d`, plus
  the two tabs `ARMORDERS` / `ARMBUILD`.
- **The pagers are `ARMPREV` / `ARMNEXT`**, and clicking one swaps the whole screen
  (`ARMCOM1.GUI -> ARMCOM2.GUI`), which is why the page number can be read off the
  screen name but the page *total* cannot: page 3 is a file that is not loaded.
- **`%sMAIN2.GUI` ends in a digit** and is not a build page — page detection has to
  exclude it by name.
- **A build button changes no GUI state.** Clicking `ARMSOLAR` leaves the screen, the
  stage and `UIChange_f` alone; what moves is the cursor-mode byte `main+0x2CC3`
  (`1 -> 14`), and the engine draws a placement footprint. Absence of a GUI-visible
  consequence is normal for this class of gadget, not a failed click.
- Placement itself is **left**-click; a right-click cancels the mode
  (`14 -> 1`). The footprint box draws red on an invalid site, which is what a
  refused placement looks like.

---

## Appendix — addresses

### Functions
```
0x41B286  sprintf "%s%d.GUI"      build page name
0x491D70  UpdateIngameGUI         pop until main+0x37EA0 on top
0x495860  ApplySelectUnitMenu     presses "STOP"
0x497BA4  uses "%sMAIN2.GUI"
0x49FD20  ToGadgetLocal           screen point -> gadget-local
0x49FE60  GUI_FindGadgetByName    (ctrls, name) -> idx | -1
0x4A0BF0  GUIGADGET_SetText
0x4A1B40  GUI_ListboxBuild        id 2 handler
0x4A3EF0  GUI_SliderUpdate        id 4 handler
0x4A4660  GUI_TimerState          id 13 handler
0x4A4980  surface handler         id 6
0x4A4D70  textfield handler       id 3
0x4A56B0  GUI_LabelDraw           id 5
0x4A5F40  GUI_ButtonDraw          id 1, id 12
0x4A6A40  assoc radio-group reset
0x4A81E0  GUI_StageUpdateDraw
0x4A9176  type switch (id-1)
0x4A9660  GUI_Pop
0x4AA8F0  GUI_LoadAndParse
0x4AB060  GUICONTROL_IsOnTop
0x4AD350  GUI_ParseCommonFields
0x4B0230  GUI_BlitToFramebuffer   id 11
0x4B6AF0  list node accessor
```

### Data
```
0x511DE8  main (TAdynmemStruct*)
main+0x519   GUIInfo desktopGUI
main+0x531   GUIInfo.TheActive_GUIMEM
main+0x555   GUIInfo cursor/rect block
main+0x579   GUIInfo.UIChange_f
main+0x37EA0 expected in-game screen name
0x4A962C  gadget type jump table, 13 entries
0x502714  "STOP"      0x502838  "%s%d.GUI"    0x50282C  "%sGEN.GUI"
0x502844  "BUILDER.GUI"          0x509570  "%sMAIN2.GUI"
```

### Record offsets at a glance
```
stride 0x15B, pack(1); gadget i at ControlsAry + i*0x15B
common: id +00  assoc +01  name +02[16]  xpos +13  ypos +15  width +17  height +19
        attribs +1B  active +29  help +33[128]
panel:  totalgadgets +B6  crdefault +CC[16]  escdefault +DC[16]  defaultfocus +EC[16]
button: text +B6[128]  stages +136  status_curnt +137  status_init +138  quickkey +13A
        grayedout +13C
list:   selected +BA  index +BC  handle +C2  itemheight +DA
```
