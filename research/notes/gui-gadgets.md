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
3. **`id` is the type and `assoc` is a group id.** The draw dispatcher switches on
   `id-1` through a 13-entry table at `0x4A962C`: 1=button, 2=listbox, 3=textfield,
   4=slider, 5=label, 6=surface, 11=picture, 13=timer. `id 0` (the panel) is not in the
   table. `assoc` groups radio buttons (`0x4A6A40`) **and** binds a scrollbar to its
   listbox (`0x4A3EF0`) — every gadget carries one, so only a shared value means
   anything. [BINARY-VERIFIED]
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
| `+0x27` | `u8` | `texturenumber` | [CORPUS] |
| `+0x28` | `u8` | `fontnumber` — really an **index into the screen's `id 7` resource gadgets**, not an absolute font id (§3) | [BINARY-VERIFIED] |
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
| `+0xBA` | `selected_i` (`i16`) | [LIVE] |
| `+0xBC` | **`top`** — index of the first visible row (scroll position) | [BINARY-VERIFIED] `0x4A1D0F` |
| `+0xBE` | **`maxtop`** — largest legal `top`, i.e. `count - visible` | [BINARY-VERIFIED] `0x4A3657`, and used as the scrollbar divisor at `0x4A3065` |
| `+0xC0` | **`count`** (`i16`) | [BINARY-VERIFIED] written by the setter at `0x4A332A`; gates drawing at `0x4A1C6A` |
| `+0xC2` | **`char* items`** — one flat blob, entries separated by `\0` or `\n` | [BINARY-VERIFIED] `0x4A1D09` + `0x4A3331` |
| `+0xC6` | `void* entries` — the *picture* flavour's records, stride `0x18` | [BINARY-VERIFIED] `0x4A3621`, `0x4A20B5` |
| `+0xDA` | `itemheight` (`i16`) — 0 means "use the font" | [LIVE] |

**There is no node structure.** `0x4B6AF0` is not a list-node accessor: it is
`char* NthEntry(char* buf, int n)`, walking a flat buffer and counting `\0` and `\n`
as separators. So the item text of a listbox is simply the blob at `+0xC2`, read
`count` entries deep. [BINARY-VERIFIED]

**Which flavour a listbox is, is a property of `attribs`, not of the pointer:**

| `attribs` bit | Set by | Items live at | Text? |
|---|---|---|---|
| `0x10` | `GUIGADGET_SetListText 0x4A32A0(gi, name, blob, count)` | `+0xC2` blob | yes |
| `0x20` / `0x80` | `0x4A35A0(gi, name, entries, count)` | `+0xC6`, stride `0x18` | **no** — the draw path (`0x4A20A6`) follows pointers to pictures |

The draw dispatcher tests `0x10` first (`0x4A1C54`) and falls through to `0xA0`
(`0x4A2052`). A picture list therefore has *no readable item text at all*, which is a
different answer from an empty list and must be reported as such. [BINARY-VERIFIED]

**Row geometry** — rows sit at a fixed pitch from the top of the gadget, not spread over
its height:

```
pitch = itemheight ? itemheight : glyph('I').height + 3      /* 0x4A1C21..0x4A1C4D */
row k occupies y = rect.y + 2 + (k - top) * pitch            /* 0x4A1D50 */
visible = (rect.height - 2) / pitch
```

The font is reached as `*(0x51FBA4) -> +0x14 -> +0x0C`, whose glyph *i* is
`*(void**)(font + i*8 + 0x28)` (`GetGlyph 0x4B7F30`) with the height in the glyph's
`i16` at `+0x02`. [BINARY-VERIFIED] Live, the engine sets `itemheight = 15` on
`LOADGAME`'s and `SELPROV`'s lists even though no `.GUI` in the stock corpus declares
one, so the font path is the fallback rather than the usual case. [LIVE]

A leading `&G` on an entry is a colour marker the engine strips before drawing
(`0x4A1E17`), so a reader should strip it too. [BINARY-VERIFIED]

### 2.5 Textfield — `id 3`

| Off | Field | Evidence |
|---|---|---|
| `+0xB6` | `text[128]` | [LIVE] |
| `+0x138` | `maxchars` (`i16`) | [BINARY-VERIFIED] serializer `0x4AE8D7` |

### 2.6 Slider — `id 4`

The corpus has no `GUI3IDControl` or `GUI4IDControl` — `tamem_ghidra.h` defines
`GUI0/1/2/5/6/9` only — but the TDF reader and writer pin every field exactly.

| Off | Field | TDF key | Evidence |
|---|---|---|---|
| `+0x136` | `range` (`i16`) | `range` | [BINARY-VERIFIED] `0x4AE191` |
| `+0x13C` | `thick` (`i32`) | `thick` | [BINARY-VERIFIED] `0x4AE1A3` |
| `+0x140` | **`knobpos` (`i16`) — the value** | `knobpos` | [BINARY-VERIFIED] `0x4AE1B8` |
| `+0x142` | `knobsize` (`i16`) | `knobsize` | [BINARY-VERIFIED] `0x4AE1DE` |

Read the slider parser at `0x4AE170` and the writer at `0x4AE050`; they agree in both
directions. **Neither of the two offsets previously guessed was the value**: `+0x136` is
`range` and `+0x142` is `knobsize`. `GUI_SliderUpdate 0x4A3EF0` touching them is the
scrollbar doing arithmetic, not storing a position.

TA's sliders are **scrollbars bound to a listbox by `assoc`**: `0x4A3EF0` scans the panel
for the `id 2` gadget whose `assoc` byte matches the slider's, and `0x4A3053` then writes
`knobpos = range * list.top / list.maxtop`. [BINARY-VERIFIED] Live on `SELPROV`, the
`DPLAY` list, its `SLIDER` and its two unnamed arrow buttons all carry `assoc 50`. [LIVE]

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
| 7 | `0x4A940C` | — | **resource declaration** — never drawn; see below |
| 8–9 | `0x4A940C` | — | not drawn, and absent from the stock corpus |
| 10 | `0x4A93EB` | `0x4A4C90` | [INFERRED] |
| 11 | `0x4A9186` | `GUI_BlitToFramebuffer 0x4B0230` | picture |
| 12 | `0x4A9199` | `0x4A5F40` | button-like |
| 13 | `0x4A93AB` | `GUI_TimerState 0x4A4660` | timer / animation |

**`id 7` is how a screen declares its fonts and button art.** It is never drawn;
instead twelve separate routines walk the gadget array counting `id == 7` records and
stop when the running ordinal equals the *consuming* gadget's `+0x28`
(`0x4A1839`, `0x4A1C9B`, `0x4A25D7`, `0x4A2ED8`, `0x4A3110`, `0x4A3864`, `0x4A3F98`,
`0x4A4DA4`, `0x4A5402`, `0x4A56F9`, `0x4A5FEF`, `0x4A71F2` — all the same loop).
So `+0x28` is not a font *number* in any absolute sense: it is an index into the
screen's own list of `id 7` gadgets. [BINARY-VERIFIED] In the stock corpus they are
named `FONT` / `ARMFONT` / `CORFONT` and `ARMBUTT` / `CORBUTT` — 68 of them, the
third most common type. [CORPUS]

**What the stock corpus actually contains** — 1907 records across 144 `.GUI` files,
counted binary-safe (the files carry NUL padding, so a plain `grep` over a
concatenation silently drops most of them): [CORPUS]

| `id` | count | |
|---|---|---|
| 0 | 144 | one panel per file |
| 1 | 1311 | buttons |
| 2 | 40 | listboxes |
| 3 | 17 | textfields |
| 4 | 63 | sliders |
| 5 | 200 | labels |
| 6 | 54 | surfaces |
| 7 | 68 | resources |
| 12 | 10 | button-like |

Ids 8, 9, 10, 11 and 13 are handled by the dispatcher but never appear in a stock
screen.

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
| `0x4B6AF0` | `stdcall char* NthEntry(char* buf, int n)`; `ret 8` | Walks a flat blob counting `\0`/`\n` — the whole of "listbox items" (§2.4) |
| `0x4A32A0` | `stdcall void SetListText(GUIInfo*, const char* name, char* blob, int count)`; `ret 0x14` | Sets `+0xC2`/`+0xC0` and `attribs |= 0x10` |
| `0x4A35A0` | `stdcall void SetListEntries(GUIInfo*, const char* name, void* recs, int count)`; `ret 0x10` | The picture flavour: `+0xC6`, `attribs |= 0x80` |
| `0x4A3EF0` | `stdcall void GUI_SliderUpdate(GUIInfo*, int idx)` | Finds the listbox with the slider's `assoc` and writes `knobpos` |
| `0x4B7F30` | `stdcall glyph* GetGlyph(font*, int ch)`; `ret 8` | Row pitch, via glyph `'I'` height at `+0x02` |
| `0x4AD350` / `0x4AD2xx` | `GUI_ParseCommonFields` / its serializer | Where `help` is written and then zeroed (§7) |
| `0x4AE170` / `0x4AE050` | slider TDF reader / writer | Pins `range`/`thick`/`knobpos`/`knobsize` (§2.6) |
| `0x495010` | `void ToggleTabMenu(void)` | What `Tab` reaches in game (§9) |
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

## 7. `help` is parsed and then wiped

`help` really is `char[128] @ +0x33` — both the reader (`0x4AD485`) and TA's own
serializer (`0x4AD30A`) name that offset — but **the value from the `.GUI` never
survives loading**. `GUI_ParseCommonFields` reads it and then zeroes the field before
running it through the string table:

```asm
4ad485  lea ebp,[esi+0x33]          ; dest = gadget->help
4ad496  call 0x4C48C0               ; TdfGetString(dest, "help", 0x80, "")
4ad4a9  rep stos DWORD [edi],eax    ; edi = ebp, ecx = 0x20 -> 128 bytes of zero
4ad4ad  call 0x4C5740               ; Translate(dest) — on the buffer just cleared
4ad4b4  call 0x4E4760               ; strncpy(dest, result, 0x80)
```

`0x4C5740` is a binary search over the translation table at `ds:0x51FDB8` that returns
its argument unchanged when the table is absent, so the net effect is an empty string
either way. [BINARY-VERIFIED]

That matches the corpus and the live reads exactly. Of the 615 gadgets in the stock
`.GUI` files that declare a `help` key at all, only 11 give it a value — one on `SKIRMISH.GUI` (`Difficulty`,
*"Adjust skirmish difficulty."*) and ten on `SELGAME.GUI` [CORPUS] — and reading
`Difficulty`'s `+0x33` on a live SKIRMISH screen returns 128 zero bytes. [LIVE]

**Some screens write `help` at runtime, and those do read back.** `0x47A519` and its
neighbours `strncpy` a translated sentence into `gadget->help` while setting the
gadget's stage, for `SKIRMISH.GUI`'s toggles. Live on SKIRMISH: [LIVE]

| Gadget | `text` | `help` |
|---|---|---|
| `StartLocation` | `Fixed` | `Commanders are randomly placed on the battle field.` |
| `CommanderDeath` | `Game ends` | `Game ends when commander is destroyed.` |
| `Difficulty` | `Easy` | *(empty)* |

So a reader should keep reporting `help`: it is empty far more often than not, but when
it is populated it is real. Note `StartLocation`'s help describes the *other* stage —
observed, not explained.

No tooltip appeared on screen when the pointer was parked over a build button with the
in-game panel up, and with `help` empty on every build gadget there is nothing for a
tooltip to show. Whether TA's tooltip renderer is the consumer of `+0x33` is [INFERRED].

## 7.1 `grayedout` is used, and is not the same as `active`

`grayedout (i32 @ +0x13C)` on a button is set by 58 gadgets across 24 stock `.GUI` files
— every one of them a **builder's** build page (`ARMLAB1`, `ARMVP1`, `ARMSY1`,
`ARMSILO1`, `ARMAMD1`, their CORE twins…), plus `LOUNGE2`. [CORPUS] The commander's own
pages (`ARMCOM1/2`) declare none, which is why the first live pass — which only ever
selected a commander — never saw the flag set.

Building an `ARMLAB` and selecting it settles it: [LIVE]

```
gui ARMLAB1.GUI    9  button  IGPATCH    grayed
                  15  button  ARMATTACK  grayed
                  16  button  ARMDEFEND  grayed
                  17  button  ARMBLAST   grayed
```

`IGPATCH` reads `active=1, grayed=1` — so TA expresses unavailability **both** ways and
an actionability check needs both tests. `SINGLE.GUI`'s `AnyMsn` is the `active=0` case.

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

## 9. What the second live run settled

[LIVE, 2026-09-01, second pass — the run that closed phase C]

- **The in-game menu is `Tab`.** The key dispatcher at `0x495E90` indexes a byte table at
  `0x496694` with `key - 9`; `VK_TAB` maps to case 0, which reaches the TABMENU toggle at
  `0x495010`. [BINARY-VERIFIED] Live it pushes **`ARMOPT.GUI`** (`GAME OPTIONS`) with
  `SAVEGAME` / `LOADGAME` / `PREFS` / `MISSION` / `HELP` / `EXIT` / `OK`, over
  `ARMMAIN2.GUI`. `Esc` does nothing in game, held or tapped.
- **`SAVEGAME` and `LOADGAME` both open `LOADGAME.GUI`** — one screen with the listbox
  `GAMES`, the scrollbar `SLIDER`, the textfield `GAMENAME` and `LOAD` / `CANCEL` /
  `DELETE`. It is the only screen reachable without multiplayer that carries all three
  deferred surfaces at once, which makes it the test bed for them.
  `SAVEGAME.GUI`, `SAVELIST.GUI`, `OPTION.GUI` and `CMENU.GUI` ship in `totala1.hpi` but
  **no string in the exe names them** — they are dead files, and reading them as the live
  save UI wastes a session. [BINARY-VERIFIED, strings]
- **Textfields validate their own content.** `GAMENAME` (`maxchars=20`) keeps letters,
  digits, space and `_`, and silently drops `- . , ! # @`. A `fill` that comes back short
  is TA refusing characters, not input being lost.
- **A held modifier bleeds across a batch.** The shield holds `shift` for 150 ms because
  TA polls it, so a `shift+c` token in a `fill` batch capitalises every character behind
  it — `Claude` arrived as `CLAUDE`. Typing through the `char:` (WM_CHAR) token instead
  carries no modifier state and round-trips exactly, mixed case included.
- **Only clicking `SELECT` on `SELPROV` crashes**, not touching its list. Moving the
  `DPLAY` selection by clicking a row (`#0 -> #2 -> #1`) is safe and the screen keeps
  working; the null call in `ErrorLog.txt` comes from the provider handshake behind the
  `SELECT` button. That is the difference between reading the providers and using one.
- **`assoc` binds a scrollbar to its list.** `SELPROV`'s `DPLAY`, `SLIDER` and its two
  unnamed arrow buttons all carry `assoc 50`; `LOADGAME`'s carry `assoc 1`.

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
0x4B6AF0  NthEntry(buf, n)        listbox item locator, \0/\n separated
0x4A32A0  SetListText             blob flavour  (+0xC2/+0xC0, attribs|0x10)
0x4A35A0  SetListEntries          picture flavour (+0xC6,   attribs|0x80)
0x4AE050  slider TDF writer
0x4AE170  slider TDF reader
0x4B7F30  GetGlyph(font, ch)
0x495010  ToggleTabMenu           what Tab reaches in game
0x495E90  in-game key dispatcher  byte table 0x496694, jump table 0x4965F4
0x47A519  runtime help writer     SKIRMISH toggles
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
list:   selected +BA  top +BC  maxtop +BE  count +C0  items* +C2  entries* +C6
        itemheight +DA   (text flavour iff attribs & 0x10; picture iff & 0xA0)
field:  text +B6[128]  maxchars +138
slider: range +136  thick +13C  knobpos +140  knobsize +142
```

### Data (runtime globals)
```
0x51FBA4  GUI draw context   ->+0x14 font set ->+0x0C font (glyphs at +0x28, stride 8)
0x51FDB8  translation table  (NULL => Translate() returns its argument)
```
