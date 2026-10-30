# The GUI: surfaces, screens, gadgets and the palette

The retained GUI, its leaves, popups and chat, screens and their lifecycle, gadgets, menus, the palette and the loading screen.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### The saved skirmish map `0x430B82` and the map list `0x434BF0` [DISASSEMBLED + MEASURED 2026-09-27..28]

**The saved map.** At `0x430B82` the options loader reads `SkirmishMap` (string `0x50440C`) from the
`Total Annihilation` section (`0x5032E8`) through the raw read `0x4B69B0`, 0x100 bytes into
`[main+0x29A0]+0x11C`. When the value is missing (`test eax,eax; jne 0x430BFE` at `0x430B87`), it
makes the list object (`0x434AB0(2)`), calls `0x435D30(0)` and `0x435C30(0x100)` on it
(`main+0x391E9`), copies the returned name into `+0x11C` (`0x4E4760`), calls `0x434AB0(0)`, and
writes the name back with the REG_SZ write `0x4B6A20` (`0x430BF9`). [INFERRED] `0x435D30(0)` selects
entry 0 and `0x435C30` returns the selected entry's name; the MEASURED fact is the outcome: on Total
Mayhem's empty `Software\TotalM` key the value saved is the alphabetically first map, and with the
fix below the first map that has terrain ("Acid Foursome", where stock saves "A Plethora of Ponds").

**`0x434AB0(mode)` is not the builder.** It frees the list object at `main+0x391E9` unless it
already has this mode (`cmp [esi],edi` at `0x434AC8`), allocates a new one of 0xEC4 bytes
(`0x4B4F10`), stores the mode at `+0`, initialises it through `0x435110`, and stores it or NULL
(`0x434B64`, `0x434B78`). `0x434B90` frees it and the name buffer below.

**The builder is `0x434BF0`, and there is one.** It is called at `0x435D73`, `0x444F47`,
`0x444FCC`, `0x47AAF7` and `0x47AB7C`, and by itself at `0x434F29`; MEASURED 2026-09-28, the
skirmish SELMAP list and the multiplayer battle room's list both changed with the fix at
`0x434D87`. Its result is a buffer tagged `MULTI MAPS` (`0x504A24`) of NUL-separated names, cached
at `0x5122D4` with its length at `0x5122DC`, its count at `0x5122E0` and a flag at `0x5122D8`
(built once, copied to the caller from the cache after that: `0x434C39..0x434C68`). The walk:

- `0x4BCA30("Maps\*.ota", 0, &vec)` (`0x434CD0`, `0x434CE5`) fills a vector of file names,
  begin `[esp+0x24]`, end `[esp+0x28]`; the count is `(end − begin)/4` (`0x434CFE..0x434D07`).
- For each (`edi` the index, `esi = edi·4` from `0x434D26`): `0x4290F0(buf, "Maps", name, "OTA")`
  (`0x434D40`, `buf` at `[esp+0x23C]`), the .ota parsed with `0x4C2F60` on the TDF object at
  `[esp+0x30]` (`0x434D5A`), and `0x436860(3, tdf, 0)` on the list object (`0x434D7A`; what it
  tests is not mapped). Either failing takes `je 0x434EA9`.
- A map kept (`0x434D87..`): its name with the extension cut (`0x4BB0F0`), `0x4E8960`, then
  `0x4C5740`/`0x4F8A70` (a translated name if there is one), appended to the buffer through the
  realloc `0x4D84A0` (`0x434E3E`).
- `0x434EA9`, the loop's tail, reached by both paths: a refused map's `je` from `0x434D61` or
  `0x434D81`, and a kept one falling through from `0x434EA3`. When `[esp+0x18]` is set (a
  multiplayer build), the network pump `0x453D40` and `0x4618A0`; then the TDF object's destructor
  `0x4C2EB0` (`0x434ECB`) and the next name. Its `cmp [esp+0x18],ebp` needs `ebp` = 0: it is 0 at
  `0x434D61` and `0x434D81`, holds the name's length from `0x434E2A`, and is zeroed again at
  `0x434EA7`.
- **Whether a network peer has the host's map is not this list's answer.** `0x448EAE` calls
  `0x435A20` with the name, which through `0x435DA0` reaches `0x435430(1, "Maps", name, "TNT")` and
  `0x4BBC40` on the terrain file, stored at `+0xA04`; `0x448EBF` reads it back through `0x4358F0`.

**The file primitives it and the fix use** [DISASSEMBLED 2026-09-28]:

- `0x4290F0(out, dir, name, ext)`, `stdcall`, `ret 0x10`: `0x49F580` returns the `language`
  value (`0x51FB50`, read from the registry value named at `0x5097E8` by `0x42F980` at
  `0x49E9F7` and set to "english" when empty, `0x49E9FC..0x49EA27`), so it first tries
  `"%s-%s\%s"` (`0x503378`) — `Maps-english\name` on an English install — and keeps it if
  `0x4BB5B0` opens it (closed again with `0x4BB5D0`); otherwise `dir\name`. Either way, with an `ext`, `0x4BB0F0` cuts the name at its last dot and
  `"." + ext` is appended. It writes `out` with no bound.
- `0x4BB0F0(s)`, `stdcall`: writes a NUL over the LAST `.` in `s`, if any.
- `0x4BB2C0(path)` and `0x4BB5B0(path)`, `stdcall`: both call `0x4BB2E0(path, mode)` with the modes
  `0x50A474` and `0x505F10`; it calls `0x4E4990(path, mode)` first and, when that returns NULL,
  walks `[0x4B6220()+0x61C]` entries [INFERRED: the loose file, then every archive], returning a
  handle or NULL. `0x4BB5D0(handle)` closes it. No box appeared while the filtered lists were
  built, nine NULLs each (MEASURED 2026-09-28, Wine).
- `0x4C5840(shown)`, `stdcall`, `ret 4`: the reverse of the display-name translation `0x4C5740`,
  a walk of the table at `0x51FDB8` (built from Translate.tdf by `0x4C54F0`, called once, at
  `0x49EA33` in WinMain's init `0x49E830`) returning the untranslated name or NULL. The map loader
  uses it when `Maps\<name>.OTA` does not parse (`0x436114` → `0x43611E`), because a saved or
  picked name is the list's translated display name.
- `0x4352D0(name)` is the same question for a campaign, `camps\<name>.TDF` (`0x504A64`,
  `0x50341C`), with no direct caller.

**A map with no terrain** (MEASURED 2026-09-27): an `.ota` whose `.tnt` is in no archive is listed,
and on Windows the map's load stops on a box naming `Maps\<name>.TNT` (MEASURED 2026-09-28: the
SKIRMISH screen opened on "A Plethora of Ponds" with no box, and Start raised it over the loading
screen; 2026-09-27's player launch reported it at the SKIRMISH screen). Under Wine the same saved
map opens the SKIRMISH screen with no box (MEASURED 2026-09-28). A picked map's terrain path is
built by `0x435430(1, "Maps", name, "TNT")` (`0x43605D`, the string `0x504D88`). The consequence for
mods is in [compat/setups.md](../compat/setups.md), *Total Mayhem's maps with no terrain*.

**Patched** (`tagpu_patches.c`, `fix_map_terrain`, local). At `0x434D87` (7 bytes: `mov
eax,[esp+0x24]; or ecx,-1`) a stub asks whether `0x4290F0(out, "Maps", name, "TNT")` opens through
`0x4BB5B0`, the name being the one `0x434D92` reads, `[[esp+0x24] + esi]`; without terrain the map
takes `0x434EA9` with every register as the `je` at `0x434D81` leaves it, and with terrain the two
stolen instructions run and the walk goes on at `0x434D8E`. At `0x430B87` (6 bytes: `test
eax,eax; jne 0x430BFE; push 2`) a value that was read is resolved as the loader resolves it —
the name with `.ota` appended (so `0x4BB0F0` cuts only that) if `Maps\<name>.OTA` opens, else the
key `0x4C5840` returns — and asked the same question; one without terrain, or with no file and no
key, goes to `0x430B8D` as a missing value does, and that path writes the replacement to the
registry (`0x430BF9`). Both names are bounded before the builder, which has no bound of its own:
under 256 bytes each, with the `language` value's, so `"Maps-<language>\<name>.TNT"` fits the
stub's 1 024-byte buffer; a name past the bound keeps stock's verdict. A map whose file name has a dot
before `.ota` (`Map.v2.ota`) is asked about `Map.v2.TNT`, while the loader (the skirmish and
multiplayer cases 2 and 3 of `0x436844`'s jump table, at `0x4360BF`), handed the list's name
already cut at its last dot, parses `Maps\Map.OTA` (`0x436103`) and asks for the terrain through
`0x435430(1, "Maps", name, "TNT")` at `0x436190`, which cuts again (`0x4354F6`): `Maps\Map.TNT`,
stock's mis-load, which the fix neither causes nor mends. MEASURED 2026-09-28, Total Mayhem
11.3.0 under Wine, the same instance on main's build and on the fix: the skirmish list 109 → 100
and the battle room's list 109 → 100, the nine missing being exactly the nine maps with no terrain
in the install, nothing else changed; a saved "A Plethora of Ponds" became "Acid Foursome"; a pick
from the filtered list loads. With `language` = german the list read 100 German names, and a
saved "Das Ashap-Plateau" (not entry 0, which is "Absolut riesig") was kept: the SKIRMISH screen
showed it, the set-aside counter read 0 and the registry held it at exit. On the Windows 10 test
box, the same comparison on the fresh-key path: the list 109 → 100 with none of the nine, and
Start into the game where main's previous build stopped on the box.

**Not closed.** A joining peer that has the host's map's `.ota` but not its terrain loads the
host's pick by name (`0x448EAE` → `0x435A20` → `0x435DA0`), which this list does not reach, so it
meets the terrain's absence as stock does [INFERRED: the same box]. And a saved map whose terrain
is absent at launch (an archive installed only some of the time) is replaced in the registry at
the options load, where stock kept it and fell back in memory when the SKIRMISH screen could not
load its `.ota` (`0x47BCD7`).

### The download menus' records — `0x42DCF0` [DISASSEMBLED + MEASURED 2026-09-24]

**The records.** `0x42DCF0` (tag `DOWNLOADMENU`, loader thread) makes one 0xBD-byte record per
`download\*.tdf` file: a dword entry count and five 0x25-byte entries (`u16` builder type, `u8`
`MENU`, `u8` `BUTTON`, the buildee's name in 32 bytes, entry `k` at `+4 + k·0x25`). The block,
`files · 0xBD` bytes from `0x4D83B0` at `0x42DD74`, is at `[main+0x391CB]`, the record count at
`[main+0x391C7]`. The section loop `0x42DDD5..0x42DF0C` writes the count `k + 1` for section `k`
(`0x42DE12`) and fills entry `k` with no cap, so a file of six or more entries writes past its
record and the last file's past the block. The readers walk the records to `[main+0x391C7]` — the
build menu `0x41AE0F`, the downloadable check `0x42E04B`, the appender `0x42BE30` — except the page
count `0x42DF60`, inside `0x42DCF0`, which walks its local file count `[esp+0x10]`. The downloadable
check, later in `0x42DCF0`, compares each def's name with each record's first entry only and, when
the def lacks the flag, sets `+0x241 |= 0x20` (the FBI's `downloadable`; the text it formats is
"Hey!  Somebody forgot to set downloadable=1 for %s", `0x503944`), which the AI's pick skips
(`0x40BBA0`, only when `0x435100` answers 1 for `[main+0x391E9]`, a mode whose meaning is not
known): a file's first unit, since Cavedog's files name one unit for all their builders.
Stock: 70 files, at most four entries each.

**The fix**, `fix_download_records`: a file continues into as many records as it needs, at the end of
the block, section `k` being entry `k % 5` of its `(k / 5)`th record. `0x42DD74` allocates through
`dl_alloc` (zeroed, its room noted); `0x42DDF0` calls `dl_section`, which starts a record at sections
5, 10, …, growing the block when full (through the engine's allocator under the block's own name,
the string `DOWNLOADMENU` at `0x503F7C`), and writes the record's count; stock's count write at
`0x42DE12` is NOPped; `0x42DF23` becomes `imul esi,edi,0xBD` (the next file's own record);
`0x42DF35` hands the page count the record count; and `0x42E0B9` bounds the downloadable check by
the file count `dl_alloc` noted (`cmp ebp,[s_dlFiles]`), so it reads the files' own records and flags
a file's first unit only, as stock does, not the 6th, 11th, … of a long file.

**MEASURED**, one download file of 12 entries for one builder: the previous build dies during the
load in the C runtime's heap (`0x7BD35C8C`, an illegal read at `0xD9330004`, from `0x4E8952` under
`0x4E8890`), while a mod with only the 40 `canbuild` keys loads; this build reads 73 records (the
70 stock files and 5 + 5 + 2) and the builder's pages show all 12. A factory's download button queues
its unit (ARMLAB and a synthetic kbot: the button reads `+1` and the unit is built); a mobile
builder's button for a mobile unit arms nothing (`BuildUnitID` `main+0x2CC4` stays as it was),
in both builds. With the check bounded, a 12-entry file flags its first unit only; walking every
record, it flagged the 1st, 6th and 11th. The 120 stock types flagged are the same either way.
`0x42BD40..0x42BE20` is the same check as a function of its own, with the same walk to
`[main+0x391C7]` (`0x42BDF1`), and has no call and no absolute reference: dead, and left unpatched.

### The stockpile bar's divide — `0x439D41` [DISASSEMBLED 2026-09-25]

**The bar.** `0x439D20(unit)` has one caller, `0x46B446`, which draws the bottom panel. It finds
the unit's first order with `+0x42` bit 19 (`0x80000`, the weapon build) and reads:
- the order's slot index `+0x36` and progress `+0x3E`;
- the slot's weapon, `[unit + 0x10 + idx · 0x1C]` (`0x439D51`);
- that weapon's reload word `+0xE4`.

It divides `progress · 100` by the reload word at `0x439D65` (`idiv esi`).

**The defect.** Nothing bounds the index to the three inline slots, and nothing tests the divisor.
Two slots hold a zero reload:
- an unarmed slot, which holds `&Weapons[0]`, whose `+0xE4` is 0;
- a stockpile weapon with no `reloadtime`.

No stock weapon reaches it: every stockpile weapon's `reloadtime` is 120 to 180. A mod reaches
it, and so does a remote unit whose type differs on this peer.

**The fix.** A local site: a `jmp` at `0x439D41` replaces `mov ecx,[eax+0x36]; mov eax,[eax+0x3E]`.
It goes to a stub that takes `0x439D6B`, the function's own `return 0`, for any of three:
- an index above 2;
- a NULL weapon;
- a zero `+0xE4`.

Otherwise the stub repeats the two loads and rejoins at `0x439D47`, with `ecx` the index and `eax`
the progress. `esi`, pushed at `0x439D24`, is free until `0x439D47` rewrites it, and `0x439D6B`
pops it.

The only branch to the site is `0x439D34` → `0x439D41`, the site's own start. It is not run: it
rests on the disassembly.

### The in-game bitmap font, and how to rasterise TA's glyphs into a buffer of your own

[BINARY-VERIFIED 2026-09-05, this project. Read while scoping the text half of the marker
port and then USED by it — `tagpu_text.c` calls `0x4CCF60` directly. An earlier revision of
this section, written from the measure loop alone, called `font+0x00` a "baseline offset";
it is the glyph ROW COUNT, and `font+0x02` is the offset it was confused with.]

`DrawTextCustomFont 0x4C14F0` is `stdcall(OFFSCREEN* ctx, const char* str, int x, int y,
int maxWidth)`, `ret 0x14`. Its font object is `[globals+0x204]` (`0x4B6220` is just
`mov eax,ds:0x51FBD0; ret`), and the layout both its measure loop at `0x4C1527` and the
blitter read is:

```
font+0x00   u8    glyph height in ROWS       (also y+this = the measured box's bottom, 0x4C1659)
font+0x02   s8    a row offset the BLITTER SUBTRACTS from the y it is given
font+0x03   u8    first character code
font+0x04   u16[] per-character offset, indexed by (char - first); 0 = glyph absent
font+off    u8    that glyph's width, in pixels AND in bits
font+off+1  ...   the glyph: rows x width bits, MSB first, packed ACROSS row boundaries
```

`first` is the byte at **`font+3`**, loaded by `0x4CCF77` (`mov …,[esi+3]`) — it is 0 for every
stock face, which is the only reason a `font[3] != 0` test reads as "this is not the `.fnt`
format"; it is a refusal of an unusual font and not a statement about the layout
[CLARIFIED 2026-09-12, landing 4c's review, which found this fork's own comment calling it the
high byte of the y-offset word].

Neither the measure nor the blit bounds the index against the table's length — a character
past its end reads whatever follows — and both skip a code below `first` and a zero offset
**without advancing the cursor**. Those two are the ONLY characters the blit skips: `sub
ebx,[ebp-0x8]` at **`0x4CCFAA`** with `jb 0x4CCF91` at `0x4CCFAD`, and `or ebx,ebx` at
**`0x4CCFB9`** with `je 0x4CCF91` at `0x4CCFBB`. There is no upper bound anywhere, so anything
calling `0x4CCF60` has to make its own measure agree with that character for character or the
blit runs past the width the caller reserved.

**`ctx` may be NULL**, in which case `0x4C14F0` locks the screen surface itself
(`0x4C5E70(&localOFFSCREEN)`, `0x4C5FA0` to release) and draws into that.

**It does not clip the string — it REJECTS it.** `0x4C6AE0` is
`OFFSCREEN::GetClipRect(RECT* out)`, thiscall, `ret 4`: four dwords copied from `this+0x1C`.
`0x4B6750(RECT* inner, RECT* outer)` is a **containment** test — eight compares, `0` unless
every edge of `inner` lies inside `outer` — called at `0x4C1697` (ctx == NULL arm) and
`0x4C1709`, with `0x4C169E`/`0x4C1710` skipping the blit entirely when it fails. So a string whose measured box `{x, y, x+width, y+font[0]}` is not wholly
inside the context's clip rect is not drawn short: it is not drawn at all. That is why the
group digit and the `ShowRanges` labels vanished in the outer ring at zoom < 1, and why
G13p ports them rather than widening anything.

#### `0x4CCF60` — the blitter, which takes its destination directly

**Pure, instruction by instruction [VERIFIED 2026-09-12, `0x4CCF60..0x4CD00E`]:** it reads its nine
stack arguments, the font's four fields (`+0`, `+2`, `+3`, the `u16` table at `+4`, then the width
byte and the bits at the table's offset) and the string's bytes, and writes only `[edi]`, the
destination; no global, no call, no allocation, no clip. `leave; ret` at `0x4CD00D`/`0x4CD00E`.
That is the whole argument for calling it on the render thread over a buffer of ours, and the
reason `tagpu_text.c` is the one "pure engine code" entry of `tagpu/ddraw/thread-split.allow`.

**cdecl, nine arguments** (`add esp,0x24` at `0x4C16D9`):

```
0x4CCF60(u8* base, int pitch, void* font, const char* str, int x, int y,
         int fg, int bg, int transparent)
```

and `0x4C14F0` fills the first two from **its OFFSCREEN's own pixel base and pitch** —
`push [edi+0xC]` / `push [edi+0x8]` at `0x4C173E`/`0x4C1738` on the ctx path, and the same
two fields of the locally locked surface at `0x4C16A0`. Arguments 7, 8 and 9 are
`[globals+0x208]`, `[globals+0x20C]`, `[globals+0x210]`. **No OFFSCREEN reaches it, no clip
rect, not even a width or a height** — it writes exactly `sum(widths) × font[0]` pixels at
`base + (y − (s8)font[0x02]) · pitch + x` (`sub eax,ebx` at **`0x4CCF87`**, then an
**unsigned** `mul` by the pitch at `0x4CCF89`) and it is the caller's business to have measured
that. Which is the whole reason a port needs no font RE at all: hand it a buffer of yours
and TA rasterises its own glyphs into it, at any size, with none of the `0x4CC650` surface
bound that clips the line drawers.

Per pixel it is `colour = bit ? fg : bg; if (colour != transparent) *dst = colour` — a
transparent equal to `bg` makes the background bits no-ops, so `(255, 0, 0)` turns the call
into a **1-bit coverage mask** rather than a coloured sprite. The bit counter (`dl`) is reset
per GLYPH at `0x4CCFC8` and not per row, so only each glyph starts on a byte boundary.

**All three colours are used as BYTES, and the test is an 8-bit compare** [VERIFIED 2026-09-09,
G17d]: `0x4CCFD5` `mov al,BYTE PTR [ebp+0x20]` (fg), `0x4CCFD8` `mov ah,BYTE PTR [ebp+0x28]`
(transparent), `0x4CCFDF` `mov al,BYTE PTR [ebp+0x24]` (bg, on the clear bit), then `0x4CCFE2`
`cmp al,ah`. The arguments are `int`s and their high three bytes are never read, so anything
mirroring this call stores and compares the low byte and nothing wider.

**The cursor advances by the glyph's own width byte and NOTHING ELSE** [VERIFIED 2026-09-09,
G17d]: at the end of a glyph `0x4CCFF7` reloads the row start from `[ebp-0xc]` and `0x4CCFFA`/
`0x4CCFFD` add `cl` — the width read at `0x4CCFBF` — to it. There is no kerning and no pair
table, which is what makes a **per-glyph** replay of a string exact rather than approximate: the
GL UI renderer's string op stamps one quad per glyph at exactly these offsets ([GUI
renderer](../gui-renderer.html) §18). The character filter is `sub ebx,first; jb` at `0x4CCFAA`
(below `first` is skipped **and does not advance**) and `or ebx,ebx; je` at `0x4CCFB9` (a zero
table entry, likewise) — bounded below and **not above**, so the table is indexed with whatever
byte the string carries.

**The per-row column counter is a do-while, so a ZERO width byte writes 256 columns per row**
[VERIFIED 2026-09-09, G17d/G17e]: the width is read into `cl` at `0x4CCFBF`, copied to `ch` at
**`0x4CCFCA`**, and the column loop decrements it at the BOTTOM — `dec ch` at **`0x4CCFE9`**
with `je 0x4CCFEF` at `0x4CCFEB` and `jmp 0x4CCFCC` at `0x4CCFED`. `ch = 0` therefore wraps to
255 on the first decrement and runs 256 times. It is per row, not per glyph: the row tail
`0x4CD006` jumps back to `0x4CCFCA` and reloads `ch` from `cl` for every one of `font[0]` rows,
so a corrupt width byte of 0 smears `256 × rows` pixels past everything the caller measured.
**No stock font has one** — the guard matters only for a font a mod ships. `tagpu_text.c`
refuses `gw <= 0` in both `measure` and the per-glyph probe, which is a deliberate divergence:
the engine would smear, we draw nothing.

**WHAT THE BLITTER READS, EXACTLY — the fact a detour at its head can rest on** [G19f-8,
2026-09-16, read off the instruction-by-instruction pass above rather than re-derived]. For the
string it walks, `0x4CCF60` reads `font[0]` (the rows), `font[2]` (the y offset), `font[3]` (the
first code), the `u16` table entry at `font + 4 + 2*(code - first)` for every code, and, for every
code with a non-zero entry, the `ceil(rows*w/8)` bytes at `font + off + 1`. It reads the string
itself to `\0` or `'\n'` and **there is no length cap** — the walk at `0x4CCF94`…`0x4CCFA7` has
no counter in it, so a 300-byte string draws 300 glyphs. (`tagpu_gui_hook.c` and
`tagpu_gui_surf.c` both stop at 256; that bound is OURS. This paragraph said "at most 256 bytes"
of the ENGINE until the landing review read the loop — an error in the safe direction for the
subset claim below, and exactly the kind that makes one of our own limits invisible by attributing
it to the engine.) **There is no clip either**, which is the load-bearing part:
it does not skip a glyph because the destination would be off-screen — it has no destination
bound at all — so a caller cannot arrange for a glyph's bits to go unread. A detour at the
function's head that walks the same string with the same two skips (`sub ebx,first; jb` at
`0x4CCFAA`, `or ebx,ebx; je` at `0x4CCFB9`) therefore reads a **subset** of the bytes the engine
is about to read, on the next instruction — which is what lets `tagpu_gui_hook.c`'s
`text_capture` take a font's glyphs at observe time and dereference nothing at the flip. Contrast
the GAF path, where the engine blits a CLIPPED sub-rect and `tagpu_gaf_decode` reads all `w*h`:
there the same move bounds lifetime and not extent, and here it bounds both.

#### The text globals, and who sets them

| VA | What |
| --- | --- |
| `0x4C1420` | `SetFont(font)` → `[globals+0x204]`, ignoring NULL; `ret 4` |
| `0x4C13A0` | `SetTextColors(fg, bg)` → `[globals+0x208]` / `[globals+0x20C]`, each **skipped when the argument is −1**; `ret 8` |
| `0x4C13D0` | `SetTextTransparentColor(c)` → `[globals+0x210]`; `ret 4` |
| `0x4C13F0` / `0x4C1400` / `0x4C1410` | the three getters |

All three are process-global and the engine re-points them many times a frame, so anything
reading them off another thread gets whatever the game thread last drew with. Inside
`DrawGameScreen` the last pair before the marker block is **`0x4696CD`/`0x4696E7`**:
`SetFont` from a table at `main+0x3816B`, then `SetTextColors(gui[0xF], GetTextTransparentColor())`
— i.e. the in-game text is `main[0xDDA]` on a background equal to the transparent index, so
its background bits store nothing. Between that pair and the group digit at `0x469CF9`
nothing calls either function: a full-image scan puts every `0x4C1420` and `0x4C13A0` call
site outside `0x4696E7..0x469CF9`, the order driver, the walker, the five leaf drawers and
`DrawHealthBars` included. One latch at hook 8 therefore holds for the whole block.

## The UI surfaces and their writers — mapped by us (Phase E, G15a, 2026-09-07)

*Everything the engine draws into an 8bpp surface on the UI path — the shell screens, the
in-game side panel and its option screens, the bars, the minimap, chat and the popups — and
the surfaces those pixels land in. Established for [the GL UI renderer](../gui-renderer.html):
static reading of the pristine binary (objdump, md5 `8e74a1dffa1f5988624c52048f5b20cd`),
then MEASURED by a census that observes every leaf below and diffs every surface at every
flip (`tagpu_gui_hook.c`, `tagpu_gui.on=census`). The census explains 100 % of the pixels
that change on the presented surface across the whole screen inventory (§ "What the census
measured"); the leaves below are therefore the complete set for the UI, and the exceptions
are named.*

### The GUI is retained: `GUI_StageUpdateDraw 0x4A81E0` builds, `0x4AB0B0` blits [VERIFIED]

**`0x4A81E0(GUIInfo* gi, int flags)`** — `stdcall`, `ret 8`; prologue
`8B 44 24 04 81 EC C0 03 00 00` (10). The dispatcher at `0x4A9176` (table `0x4A962C`) is
inside it. Flags: `0x1` **build** (surfaces are created only under it), `0x2` **teardown**,
`0x40` redraw, `0x8` buttons only (`test …,0x48` at `0x4A91AD`), `0x4/0x40/0x80` background
variants, `0x20` skip the snapshot, `0x100/0x1000` recentre. Observed callers: `push 2` in
`GUI_Pop 0x4A968E`, `push 1` in `TA_DialogBox_fn 0x4ABD90` (`0x4AC01B`, `0x4AC198`),
`esi|0x40` at `0x494210` (the in-game panel loader). 76 sites, **none in DrawGameScreen**.

**THE GADGET DRAW LOOP, AND WHY A SECOND CALL LEAKS** [DISASSEMBLED 2026-09-18, this project, for
the vulkan-only plan's landing 9]. The dispatcher `0x4A9176` sits at the top of a loop over the
panel's gadgets:

| at | what |
|---|---|
| `0x4A9135` | `movsx eax, WORD PTR [panel+0xB6]` — the gadget count, a **signed** word; `jle 0x4A943B` at `0x4A9148`, so a count ≤ 0 draws nothing |
| `0x4A914E` | the cursor starts at `panel+0x21F`, which is **record + 0xC4** |
| `0x4A9154` | `ebx = 0x15B` — **the gadget record stride**, 347 bytes; the array starts at `panel+0x15B` |
| `0x4A915D` | `[cursor-0x9B]` = **record+0x29**, the "draw me" byte; zero skips the gadget |
| `0x4A916D` | `[cursor-0xC4]` = **record+0x00**, the type byte (1..13) |
| `0x4A9176` | `cmp eax,0xC / ja default / jmp [eax*4+0x4A962C]` |
| `0x4A940C` | `edi++`, cursor and `ebx` both `+= 0x15B`, loop while `edi < count+1` |

`0x40` is the **redraw** bit and is tested per gadget type at seven sites inside the loop
(`0x4A92C5`, `0x4A92E7`, `0x4A932B`, `0x4A934D`, `0x4A9398`, `0x4A93D8`, `0x4A93F3`). `0x1`
(build) is tested *before* the draw half, `0x2` (teardown) *after* it.

**THE ALLOCATIONS ARE GATED ON THE BUILD FLAG, WHICH IS THE ANSWER TO "IS IT SAFE TO CALL TWICE"
— AND THIS SECTION SAID THE OPPOSITE UNTIL 2026-09-18.** It claimed the draw half allocates
unconditionally. `0x4A82F0` computes `eax = ebx & 1` — the `0x1` **build** bit — saves it at `[esp+0x18]`, and `0x4A82F7` `je 0x4A90D1`
when it is clear; `0x4A90D1` is **past both allocations**, and `test bl,0x40` at `0x4A90DE` carries
a redraw on into the gadget loop from there. **So `(gi, 0x40)` allocates nothing and frees nothing
while still drawing every gadget**, and only a second *build* would leak. The first version looked
for early RETURNS before the allocation and not for JUMPS PAST it, of which there are three.

`0x4A907C` and `0x4A90B5` call `0x4C69F0(name, w, h)` and store into `panel+0xBC` (the surface the
whole UI is pre-rendered into) and `panel+0xB8`, **overwriting the previous pointers with no free**
— under flag `0x1`.

* **`0x4C69F0(name, w, h)`** — stdcall `ret 0xC` — is an **allocator, not a find-or-create**:
  `0x4D83B0(name, w*h + 0x30)`, then it writes a surface header (`+0x00`/`+0x08` = w, `+0x04` = h,
  `+0x0C` = base+0x30 the pixels, `+0x1C..+0x2C` the clip rect `(0,0,w-1,h-1)`, `+0x10` = `0x2710`,
  `+0x14` = −1, `+0x2C |= 1` with bit 1 cleared).
* **`0x4D83B0` DROPS THE NAME** ([`MEM_Alloc`](memory.md#0x4d83b0-mem_alloc-the-named-allocator-mem_allocnamed)):
  **nothing here looks a surface up by it.**
* **`0x4C6AC0(surf)`** — stdcall `ret 4` — is the free: null-checked, gated on `surf[+0x2C] & 1`
  (the "we own the pixels" bit `0x4C69F0` sets), then `0x4D85A0`.
* **The frees are SIX sites, not two, and this bullet said two** [CORRECTED 2026-09-18 by landing
  9's review, and it is the session's sixth instance of the same error — a mechanical search
  bounded by the symbols it expected]. The search was for `0x4C6AC0`. Besides `0x4A9537` and
  `0x4A9549` the function calls the **raw** free `0x4D85A0` directly at **`0x4A9575`** and
  **`0x4A95A7`**, walking the gadget array (`lea esi,[ebp+0x2b]`, stride `0x15B`) and freeing
  each record's own pointers — `[esi]` behind `test [esi+0x89],1`, then `[esi+0xAB]` or
  `[esi+0x95]` by gadget type. **All four are inside one block** gated by `0x4A950A
  `test bl,0x2` / `je 0x4A95C2``, and `ebx` is reloaded from the flags argument `[esp+0x3D8]` at
  `0x4A9434`, `0x4A94C0` and `0x4A94C9`, so `bl` really is the flags byte on every path into it.
  **The conclusion is unchanged** — a `0x40` call frees nothing — but it now rests on the gate
  rather than on a count that was wrong.
* **What is verified about the whole body, and what is not.** Over `0x4A81E0..0x4A95F2`: **32
  distinct direct call targets**, **no indirect calls at all**, and the only indirect jumps are
  the two in-body switch tables (`0x4A95F4`, `0x4A962C`). So the function itself, under
  `flags == 0x40`, executes no allocation and no free. **Not established, and deliberately not
  claimed: that nothing it CALLS allocates.** `0x4BBC40` and `0x4BBE50` are among its callees and
  both reach `0x4D85A0`; whether either is on the `0x40` path has not been shown either way. A
  callee's own transient scratch is the engine's business — the panel surfaces are the lifetime
  this section is about.
* **THE EXIT SET IS THREE `ret 0x8`, and "one early return" was loose.** `0x4A81FF` returns 0 on
  `gi->TheActive_GUIMEM == NULL` (loaded at `0x4A81EA`, `jne` at `0x4A81F3`); `0x4A95D1` is the
  normal epilogue and returns 1; `0x4A95EF` returns 0 and is reached **only** from the two `jg`
  at `0x4A8349`/`0x4A835A` below. Both of those are inside the `flags & 1` gate, so **with
  `0x40` the only reachable exits are `0x4A81FF` and `0x4A95D1`**. `0x4A95D4` is *not* an exit:
  the `bl & 0x20` path NULLs `panel+0xB8` and jumps back to `0x4A90D1`. The old wording is
  preserved next so the correction is legible:
* ~~The function's one early *return* is~~ `gi->TheActive_GUIMEM == NULL`:
  `0x4A81EA` loads `[gi+0x18]`, `0x4A81F3` `jne` past the epilogue, and `0x4A81FF` is `ret 8`.
  ~~so every real call reaches the allocation~~ — **it does not**, per the paragraph above: the
  build gate at `0x4A82F7` jumps past both. That clause was the same error stated twice.
* `0x4A8202` is where the panel record is taken: `ebp = gi->TheActive_GUIMEM->ControlsAry`
  (`[[gi+0x18]+0x04]`), and `ebp` is the base every `panel+0x…` offset in this section is
  measured from. Arg 1 sits at `[esp+0x3D4]` and arg 2 at `[esp+0x3D8]` after the prologue's
  `sub esp,0x3C0` and four pushes.

**WHAT A `0x40` REDRAW ACTUALLY DRAWS, IN ORDER — AND IT IS NOT "EVERY PIXEL AS A DRAW"**
[DISASSEMBLED + MEASURED 2026-09-18, this project, building the vulkan-only plan's landing 9].
The redraw does **not** begin at the gadget loop. With `flags = 0x40` the path is forced: `0x4A90D1`
`test bl,0x4` falls through, the saved build flag at `[esp+0x18]` is 0, `0x4A90DE` `test bl,0x40`
is set, `0x4A90EF` `test bl,0x40` is set — so control always reaches **`0x4A90F4`**, and the
`je 0x4A912E` that would skip the background is taken only when the `0x40` bit is *clear*.

| at | what | arrives at our observer as |
|---|---|---|
| `0x4A90F4` | `esi = gi`; `eax = gi->TheActive_GUIMEM->[0x24]` | — |
| `0x4A9105` | if that is non-NULL: `0x4C6B70(panel+0xBC, that, 0, 0)` — the **whole panel surface repainted from a bitmap** | a **copy** op, twin-to-twin only if that source is a surface we hold |
| `0x4A911D` | if it is NULL (and `bl & 0x80` clear, which `0x40` is): `0x4B0230(gi, 0, panel+0xC4)`, the picture handler | whatever that handler blits |
| `0x4A9135` | **then** the gadget loop | one op per gadget draw |

So the gadget **chrome** comes back as describable draws and the **wallpaper** comes back as a
bitmap copy. `GUIMEM+0x24` is a surface pointer and was not in `gui-gadgets.md`'s layout table
before this landing.

The build path is the matching pair and explains where that wallpaper came from:
`0x4A9098` is `0x4C6B70(panel+0xBC, NULL, -panel.xpos, -panel.ypos)` — a **NULL source, which is
the primary surface**, i.e. the new panel surface is seeded with a grab of the screen behind it —
and `0x4A90CC` is `0x4C6B70(panel+0xB8, panel+0xBC, 0, 0)`, the save-under copy. A NULL
*destination* would likewise resolve to the primary surface, so calling a redraw on a screen whose
`panel+0xBC` is NULL would paint the wallpaper straight onto the frame; our caller refuses unless
`TheActive_GUIMEM`, its `ControlsAry` and `panel+0xBC` are all present.

**MEASURED, one op window each, `renderer=vulkan`, the shipped arm set** [2026-09-18]:

| top screen | ops from one `(gi, 0x40)` | by kind |
|---|---|---|
| `MAINMENU.GUI` (the shell, at arm) | **115** | `gaf 105  line 4  focus 6` |
| the shell one reset later | **40** | `gaf 29  line 4  copy 1  focus 6` |
| `ARMMAIN2.GUI` (in game) | **1** | `gaf 1` |

**AND A `0x40` REDRAW WRITES MORE THAN PIXELS** [FOUND by landing 9's review, then MEASURED].
`0x4A943B` reads the focus index at `GUIMEM+0x20`; if it is not −1 and `gi+0xA2` is set,
`0x4A947B` calls **`0x4A16F0(gi, idx, 8)`** (and `0x4A9505` calls it again behind `record+0x29`).
`0x4A16F0` writes, unconditionally at its top:

| at | write | what |
|---|---|---|
| `0x4A1708` | `[gi+0xCCA] = 1` | **the GUI dirty flag** |
| `0x4A1722` | `[record+0x1F] = 0` | on every type-3 gadget |
| `0x4A1758` | `[record+0x1F] = 0x1E` | on the focused one (`colorf`, gui-gadgets §2.1) |

`gi+0xCCA` is consumed by the GUI pump **`0x4A9FD0`**: `0x4AA0AF` tests it against 1, `0x4AA0BB`
clears it, and `0x4AA0CD` issues **another** `0x4A81E0(gi, GUIMEM->flags | 0x40)`. So a redraw
can provoke a redraw. **Whether that amplifies is NOT settled**, and the first version of this
paragraph said it was. `builds=` — every entry to `0x4A81E0` — over three boots per arm came to
means of **1.0, 72.0, 69.1** per census window with a forced repaint and **28.1, 21.8, 62.1**
without: overlapping, dominated by how long a boot lingers on which screen, no direction. What
holds in every window of every boot in both arms is `buildFlags` **0xC0** — the engine issues
`0x40` redraws continuously by itself, from `0x41A8DA`, `0x4931B4` and `0x4A96BF` — and no
runaway was observed (`dropped=0 overflows=0 stalls=0`, a complete frame at 0 magenta, and the
forced repaint firing 2–4 times a boot on a reseed edge rather than per frame).

The in-game figure is not a failure and it is the more useful fact: **`ARMMAIN2.GUI` is a
three-label screen** (`KILLS`, `LOSSES`, `TOTALUNITS` — `tacli ui --all`), not the HUD. The
in-game panel art is drawn by the in-game draw path, not by this gadget tree, so there is nothing
here for a redraw to re-issue. A forced repaint is a **shell** mechanism.

**The game's protocol is build (`0x1`) / draw (`0x40`) / teardown (`0x2`)**, with `GUI_Pop
0x4A968E` pushing `2`. A forced repaint is a `0x40` and joins the middle of that protocol without
disturbing it. Mechanically confirmed over the whole function: **exactly two** calls to `0x4C69F0`
and **exactly two** to `0x4C6AC0`, at the four addresses above and nowhere else. Two further
branches skip the allocation and are not the redraw path — `0x4A8349` and `0x4A835A` **jump to
`0x4A95E3`**, the function's tail, when the panel is wider or taller than the screen
(`0x4B6700()`/`0x4B6710()` — the screen width and height — against `panel+0x17`/`+0x19`). A panel
bigger than the screen abandons the whole call.

**The teardown's restore, and why it is a copy out of a surface about to die** [DISASSEMBLED
2026-09-27 from the pristine exe; MEASURED the same day, [GUI renderer](../gui-renderer.html) §28].
Inside the `test bl,0x2` block (`0x4A950A`):

| at | what |
|---|---|
| `0x4A9513` | `eax = panel+0xB8`, the save-under; `0x4A951D` `je 0x4A9542` skips the restore when it is NULL |
| `0x4A952B` | `0x4C6B70(NULL, panel+0xB8, panel+0x13, panel+0x15)` — the save-under copied back onto the frame (a NULL destination is the primary) at the panel's position |
| `0x4A9537` | `SurfaceFree 0x4C6AC0(panel+0xB8)` — **the very next call**; `0x4A953C` NULLs the field |
| `0x4A9549` | `SurfaceFree 0x4C6AC0(panel+0xBC)`; `0x4A955A` NULLs it |

`panel+0x13`/`+0x15` are the panel's x and y: the build path at `0x4A9081`..`0x4A908B` negates
the same two words for `0x4A9098`'s screen grab. The save-under is what `0x4A90CC` copied out of
`panel+0xBC` at build time, before a gadget was drawn into it, so the restore puts back the frame
as it was under the panel. **A surface is read and freed in one breath here**, which is why the
UI producer keeps a freed surface's twin until its copy is published (`OP_FREE`, the GUI renderer
§28): dropped at the free, the restore was lost and Escalation's orders panel (`ARMCOM1`, 128 ×
640 at y 128) stayed on screen after a deselect.

**HOW THAT PAIR WAS MISSED THE FIRST TIME, which is worth more than the pair.** The first
enumeration looked for early **returns** before the allocation, found one, and did not look for
**jumps past** it — of which there are three. An early return and a forward jump are the same
fact about reachability and only one of them was being counted. Any "this call cannot reach X"
claim on this binary has to enumerate both.

**The two rect sentinels, and why a hand-placed `xpos` is written BEFORE the build call**
[VERIFIED 2026-09-09, Phase F G18, disassembly of the pristine build]. `0x4A820C` opens with
`or eax,0xffffffff`, so `eax` is **−1** for the whole function, and `0x4A8224` sets `esi` to
**−2**. Two flag pairs *stamp* a sentinel into the panel record before anything is drawn, and
then the record is *read* twice:

| at | condition | effect |
|---|---|---|
| `0x4A820F`–`0x4A821D` | `(flags & 0x100) && (flags & 0x1)` | writes −1 into `ypos` (`+0x15`) and `xpos` (`+0x13`) |
| `0x4A8221`–`0x4A8234` | `(flags & 0x1000) && (flags & 0x1)` | writes −2 into the same two |
| `0x4A8238` | `xpos == −1` | centre on the whole screen: `xpos = (W − w) / 2`, `ypos = (H − h) / 2` |
| `0x4A8266` | `xpos == −2` | centre in the play area: `xpos = ((W − 128 − w) / 2) + 128` (`0x4A8275` loads `0xFFFFFF80`), `ypos` as above |

`w` is `+0x17` and `h` is `+0x19`. `W` and `H` come from `0x4B6700` and `0x4B6710`, each a
two-instruction getter returning `[[0x51FBD0]+0xD4]` and `[[0x51FBD0]+0xD8]`. The 128 is the
in-game side panel's width, which is what makes −2 the *in-game* centring rule and −1 the
front-end one. **Both branches write the result back into the record**, so this is not a
read-only convenience: a screen positioned by hand must carry neither sentinel. `tagpu_menu.c`
sets `xpos` to `W − 16 − 304` and passes `flags = 0x21`, which trips neither pair
(`0x21 & 0x100 == 0`, `0x21 & 0x1000 == 0`) and equals neither −1 nor −2 — and it writes that
`xpos` **before** this call, because the build consumes the rect on the way through.

**The build sequence (`0x4A905E..0x4A9135`, read 2026-09-07):**

```
panel+0xBC = 0x4C69F0(panel->name /* or 0x509920 */, w, h)   ; the screen's own surface
0x4C6B70(panel+0xBC, NULL, -xpos, -ypos)                     ; the screen UNDER it, copied in
unless flags&0x20:
  panel+0xB8 = 0x4C69F0("SAVE UNDER" @0x509914, w, h)        ; and kept aside…
  0x4C6B70(panel+0xB8, panel+0xBC, 0, 0)                     ; …for the teardown restore
background: 0x4C6B70(panel+0xBC, *(GUIMEM+0x24), 0, 0)        ; the PCX background surface
        or  0x4B0230(gi, 0, panel+0xC4)                      ; or the tiled texture
then the gadget loop 0x4A9135..0x4A942E over EVERY gadget 1..totalgadgets,
skipping only active == 0 — there is no per-gadget dirty flag
```

Every handler draws into `[panel+0xBC]` with that object as its context (`ebx=[panel+0xBC]` at
`0x4A6056` in the button handler, and so on). Teardown (`flags&2`, `0x4A950A..`) copies
`panel+0xB8` back to the screen, frees both, and `GUI_Pop 0x4A9660` frees the GUIMEMSTRUCT.
**`*(GUIMEM+0x24)` is the screen's background PCX decoded into a surface** — filled by the
loader, not by any draw leaf (MEASURED: 307 072 unexplained bytes on `MAINMENU.GUI`'s
`03D51178 640×480`, zero ops), and read only as a copy source.

#### `0x4CAF30 PCXLoadToSurface(const char* path)` — where that tag comes from [VERIFIED]

The tag is the file path because **the PCX loader passes its own argument straight through**, and
that single fact is what the fork's `PK_ASSET` rests on. Disassembled 2026-09-21:

```
4caf37: mov  ebx,[esp+0x9c]      ; arg0 — the path, held in ebx up to the call at 0x4CAFB3
                                 ;   (0x4CAFB8 then reuses ebx for the returned object)
4caf41: push ebx
4caf42: call 0x4bb5b0            ; file open (stdcall, ret 4)  -> ebp = handle
4caf4b: je   0x4cb06f            ; no file: out
4caf55: push 0x80 / push buf / push ebp
4caf5c: call 0x4bb7c0            ; read 0x80 header bytes (stdcall, ret 0xC)
4caf61: cmp  eax,0x80 ; jne out  ; a short read is not a PCX
4caf6c: cmp  BYTE [esp+0x24],0xa ; PCX magic
4caf77: cmp  BYTE [esp+0x25],0x5 ; PCX version 5 (the 256-colour flavour)
4caf82: esi = [esp+0x2c] - [esp+0x28] + 1     ; xmax - xmin + 1  = width
        edi = [esp+0x2e] - [esp+0x2a] + 1     ; ymax - ymin + 1  = height
4cafb0: push edi / push esi / push ebx        ; (tag, w, h)
4cafb3: call 0x4c69f0            ; SurfaceCreateNamed — THE TAG IS THE PATH
```

The header offsets land on PCX's `xmin/ymin/xmax/ymax` at `+4/+6/+8/+10` exactly because both
helpers are **stdcall** (`0x4BB5B0` is `ret 4`, `0x4BB7C0` is `ret 0xC` — checked), so `esp` is
unchanged across the read and the buffer stays at `esp+0x24`. Had either been cdecl the offsets
would be 12 bytes off and the reading would be wrong; it is worth saying which, because the
arithmetic is the whole proof that `esi`/`edi` are a width and a height.

So the tag of `*(GUIMEM+0x24)` is the path only in the sense that **the caller's path string is
still in `ebx`** — there is no formatting, no copy, no per-surface name. `ptr_ok` plus a bounded
eight-character compare is therefore the complete test, and the table above is why it cannot
collide.

Its creation tag is that file path, and the string is **MEASURED** as well as disassembled:
`0x4C69F0` is handed `"bitmaps\FrontendX.PCX"` for the main menu's 640×480 backdrop, and a
five-screen shell walk (2026-09-21) names exactly `"bitmaps\FrontendX.PCX"`,
`"bitmaps\singlebg.PCX"` and `"bitmaps\Skirmsetup4x.PCX"` beside `"OFFSCREEN"`,
`"MAINMENU.GUI"`/`"SINGLE.GUI"`/`"SKIRMISH.GUI"`, `"SAVE UNDER"` and `"SAVEMOUSE 1..3"` — the
whole shell inventory, nothing else.

That tag is what the fork keys `PK_ASSET` on — **the two facts here are the whole basis for it**:
the surface is named after a file, and no observed draw targets it, so its bytes are decoded
source art rather than anything the 1997 rasteriser composed. `op_add` re-checks the second one
for the surface's whole life rather than trusting it, **and since 2026-09-21 the census also
measures whether the check is telling the truth** (`s_assetDrift`: pixels that moved in a surface
that still claims the tag and whose bytes have already crossed). It reads **0** over a 920 000-flip
session across five screens. What that does *not* cover is an engine path that re-fills such a
surface without passing one of the 17 leaves — the loader itself is one, which is the premise —
so the residual is named rather than closed. Until 2026-09-21 the fork carried none of this and
every shell screen presented its gadgets over black — 3.71 % of the engine's own picture.
See [the GUI renderer](../gui-renderer.html), "the shell was 3.71 %".

| `id` | handler | `ret` | prologue (steal) | draws through |
|---|---|---|---|---|
| 1 button | `0x4A5F40` | 8 | `81 EC D8 00 00 00 53 55` (8) | `0x4B7F90(ctx, frame, x+HotX, y+HotY)` at `0x4A61BE`, `DrawText 0x4A50E0`, `0x4BE950`, `0x4B8310`; sets `TheActive_GUIMEM+0x14 = 1` at `0x4A5F5E` |
| 2 listbox | `0x4A1B40` | 8 | `81 EC BC 00 00 00 53 55` (8) | `0x4C6D20` at `0x4A1C08`, `0x4B0230(gi, idx, 0)`, rows via `0x4A23B0..0x4A2BE0` (`0x4B7F90` ×13), clip save/restore `0x4A2068` |
| 3 textfield | `0x4A4D70` | 8 | `83 EC 18 8B 54 24 1C` (7) | `0x4BF6F0`, `0x4C6D20`, `0x4BE950`, DrawText |
| 4 slider | `0x4A3EF0` | 8 | `8B 44 24 04 83 EC 08` (7) | nothing itself; `0x4A2580` paints |
| 5 label | `0x4A56B0` | 8 | `81 EC AC 00 00 00` (6) | `0x4BF6F0`, `0x4C14F0` / DrawText, `0x4BE950` |
| 6 surface | `0x4A4980` | 8 | `83 EC 50 53 55` (5) | **`0x4C7580`** (the textured-triangle stamp, below), `0x4B7F90`, `0x4BF6F0` |
| 10 | `0x4A4C90` | **0xC** `(gi, idx, flags)` | `8B 54 24 04 83 EC 10` (7) | `0x4BE950` ×2 |
| 11 picture / panel bg | `0x4B0230` | **0xC** `(gi, idx, GAFFrame*)` | `83 EC 24 8B 54 24 2C` (7) | NULL frame: a tiling branch through `0x4C6B70(panel+0xBC, tex, x, y)` that never runs, because its texture lookup `0x4A18C0` returns 0 on every path (the match it finds is discarded: `xor eax,eax` at `0x4A191A`) [DISASSEMBLED 2026-10-01], + bevel `0x4B0160`; else `0x4B7F90` |
| **12** | **`0x4A5E50`** — not `0x4A5F40` as [GUI gadgets](../gui-gadgets.html) §3 said; corrected there | | | |
| 13 timer | `0x4A4660` | 8 | `83 EC 24 53 55` (5) | `0x4C5E70(ebx)` over `[panel+0xBC]` or `[gi+0xCD2]`, then `0x4BF6F0`, `0x4B7F90`, DrawText, `0x4C5FA0` — i.e. into the back buffer |

`DrawText 0x4A50E0(ctx, str, x, y, maxW, shade)` — `stdcall`, `ret 0x18`: with no GUI font
set, `0x4C14F0(ctx, s, x, y, -1)`; else per character `0x4B7F30([[0x51FBA4]+0x14]+0xC, ch)` →
`0x4B7F90(ctx, glyph, x, y)` (`0x4A5185`) when `shade == 0`, else `0x4B8310(ctx, glyph, x, y, shade)` (`0x4A5191`). **So GUI text is GAF
glyph blits, and `0x4CCF60` is only reached through `DrawTextCustomFont 0x4C14F0`** (its two
callers `0x4C16D4`, `0x4C1744`); MEASURED, in-game option screens still take that path for some
labels (`text 1572` ops on `PREFS.GUI`).

**The GUI font is registered by `0x4AEDD0(GUIInfo* gi, const char* name, int slot)`**
[VERIFIED 2026-09-09]. An `E8` scan of `.text` finds exactly **two** call sites, both in the
in-game GUI init and both passing `main+0x519`: `0x491564` with `"hattfont12"` (`0x509250`)
and slot 0, `0x49157D` with `"hattfont11"` (`0x509244`) and slot 1. It copies the name into a
256-byte **prefix** it READS from `gi+0xAB6` through `0x4E4760` (the name is composed, not
stored there), loads the result with the GAF loader `0x4B8C60` after `0x4BBC40` opens it, and
stores the resulting bank at **`gi + 0x08 + slot*4`** (`0x4AEE5F`: `mov [ebp+ecx*4+0x8],eax`) —
which is why `slot` is the third argument. Glyphs then come out of it through `GetGlyph
0x4B7F30`. Those two GAFs are the faces every gadget label and caption is drawn with — one entry, 256 frames, one per character code, the per-frame
`ypos` hotspot being the baseline (`tools/guifont.py`).

**How a screen reaches the frame: `0x4AB0B0(GUIMEMSTRUCT*, OFFSCREEN* dst, RECT* dirty)`** —
`stdcall`, `ret 0xC`, prologue `83 EC 10 57 8B 7C 24 18` (8): recurses bottom-to-top over
`per_active`, and blits **`0x4C6B70(dst, panel+0xBC, xpos, ypos)`** (`0x4AB158`) only if
`mem+0x14 == 1` (cleared after) or `0x4B67D0(&panelRect, dirty)` [INFERRED name] says the rect
overlaps. Two callers: `0x49FA28` in the **message-box loop `0x49F9C0(gi, untilScreen)`**
(one caller, `0x4437B0`, for `MSGBOX.GUI`; *Who pushes the two rings*) —
`{pump messages; 0x4A9FD0(gi); 0x4AB0B0(top, NULL, NULL); 0x4C2870; 0x4C63A0}` — and
`0x4AB182` in the thunk **`0x4AB170(gi, ctx, dirty)`** (`ret 0xC`, prologue
`8B 44 24 0C 8B 54 24 04 8B 4C 24 08`), called once per frame from DrawGameScreen at
`0x46A303` with `(main+0x519, &[esp+0x34], main+0x37E27)` after `0x4C69C0(&ctx)` resets the
clip.

### The leaves — every function that writes UI pixels [VERIFIED; MEASURED complete]

| VA | name | convention, `ret` | arguments | destination and box | prologue stolen |
|---|---|---|---|---|---|
| `0x4B7F90` | `CopyGafToContext` | stdcall `0x10` | `(ctx, GAFFrame*, x, y)` | `{x−HotX, y−HotY, +w−1, +h−1}`, hotspot **signed** (`movsx` at `0x4B802C/0x4B803F`), clipped by `0x4C6AE0`+`0x4B7E60`; leaf `0x4CBE70` (raw), `0x4CC51D` (RLE). **A frame whose `+0x0A` (the sub-frame count) is non-zero is a STACK and draws nothing itself**: the loop `0x4B7FE9..0x4B801A` hands each sub-frame of `+0x10` to `0x4B8500` when that sub-frame's `+0x0B` is non-zero and back to `0x4B7F90` otherwise, then jumps to the exit at `0x4B8128` without a write of its own [DISASSEMBLED 2026-09-26]. SKIRMISH's side buttons are two-frame stacks whose sub-frames take the `0x4B8500` arm [MEASURED 2026-09-26] | `81 EC 94 00 00 00` (6) |
| `0x4B8500` | `AlphaCompsteBuf2OFFScreen` | stdcall `0x10` | same | same, blended through `[globals+0xC0]`; 24 callers, none in the GUI's own code — its GUI draws arrive through `0x4B7F90`'s stack loop (`0x4B7FFE`). A stack hands each sub-frame to itself (`0x4B8561..0x4B8586`) and writes nothing of its own [DISASSEMBLED 2026-09-26] | `81 EC 94 00 00 00` (6) |
| `0x4B8310` | the blit `DrawText` takes when its sixth argument, the shade, is non-zero (`0x4A5179..0x4A517F`, and the shade becomes `a5`); also called directly by the gadget painters at `0x4A5EED` and `0x4A619C`. A **blended** GAF blit, not a copy. *[CORRECTED 2026-09-23 by the landing review: this said "the blit DrawText takes under `globals+0xF0` bit 7"; bit 7 is tested only inside this function.]* | stdcall **`0x14`** (`ret 0x14` at `0x4B84F1`) | `(ctx, GAFFrame*, x, y, a5)` — **five**, not the four the call-site inference said | **draws nothing unless `globals+0xF0` bit 7 is set** (tested at `0x4B8329`, `jne 0x4B84E7` at `0x4B832D`, before any write). Sub-frames: each to `0x4B8500` (the loop `0x4B8379..0x4B8397`, the call at `0x4B838A`). Raw frame (`+0x9 == 0`): `0x4CBF2C(ctx, desc, srcR, dstR, key = a5, table = [globals+0xC8])`, whose loop (`0x4CBF96..0x4CBFBC`) skips `src == a5` and writes **`dst = table[src·256 + dst]`** — it reads the destination. RLE frame: `0x4CC3D0(base, pitch, &dstXY, rle, &srcRect, row)` cdecl (`add esp,0x18` at `0x4B84D2`, shared with the raw arm), `row = [globals+0xC8] + a5·256` built at `0x4B84AB`..`0x4B84B7` with **`a5` shifted unmasked** (the table is 32 rows). Its decode is TA-RLE — bit 0 a skip of `b>>1`, bit 1 a run of `(b>>2)+1` copies of one byte, else `(b>>2)+1` literals — and **every texel it draws is `row[src]`** (`mov dl,[esi]` at `0x4CC4AD`, `add edx,[ebp+0x1C]`, then after an `xor eax,eax` `mov al,[edx]` and `mov [edi],al` at `0x4CC4B7`; the other run arm and both literal arms, `0x4CC4C4`/`0x4CC4E5`/`0x4CC500`, are the same load, add, load and store): a remap of the frame through one row, which **never reads the destination**; skips are left alone. [DISASSEMBLED 2026-09-23.] MEASURED 2026-09-23: the post-game screen's player names (`Player`, `Core`) are drawn through it, 18 ops per repaint, and **every one takes the RLE arm** — 6×8..6×10 font glyphs, `+0x9 == 1`, key 9, `a5 == 15`, bit 7 set (caps `0x07E6`); so the names are a remap, not a blend. The raw arm's destination read is real but was not reached on that screen | `81 EC 94 00 00 00` (6) |
| `0x4B8150` | opaque GAF blit | stdcall `0x10` | `(ctx, GAFFrame*, x, y)` — a **frame**, not a descriptor | leaf `0x4CBDD1`, no key; 4 callers, **all terrain** (`0x484110`, `0x48415C`, `0x484228`, `0x484274`) — not a UI leaf. `0x4CBDD1(dst, src, srcRect*, dstPoint*)` (`0x4CBDD1..0x4CBE6F`, cdecl, a plain `ret`) — three call sites, `0x4B82BD` here and `0x4C6D4D`/`0x4C6DA7` in `0x4C6D20` — copies the inclusive source rect row by row with one element size for the whole rect, chosen from the width at `0x4CBE33..0x4CBE3C`: an odd width copies bytes (`rep movsb` at `0x4CBE61`), a width of 2 mod 4 words (`rep movsw` at `0x4CBE52`), any other dwords (`rep movsd` at `0x4CBE44`, the loop through `0x4CBE4B`); the source at `src+0xC` + y·`src+0x8` + x, with no clip of its own [DISASSEMBLED 2026-09-24]: the terrain fault in "Engine defects we patch" is its source read | |
| `0x4C6D20` | descriptor blit — a RECT COPY between two contexts | stdcall `0x10` | `(ctx, src {…, pitch +0x8, pixels +0xC}, RECT* srect, RECT* drect)` | all four go to `0x4CBDD1(ctx, src, srect, drect)` (`0x4C6DA7`; a NULL `ctx` is first acquired through `0x4C5E70` and passed as the local, `0x4C6D4D`), which takes **the size from `srect` alone** (`r−l+1` by `b−t+1`, `0x4CBDE2..0x4CBDF2`) and **only the top-left of `drect`** — its right and bottom are never read — and clips nothing [DISASSEMBLED 2026-09-26]. So the box written is `drect.l,t` by `srect`'s extent. Four callers: `0x4A1C08` (listbox) and `0x4A4EC0` (textfield), each restoring its own rect (`srect == drect`, one local passed twice) from a background surface — the listbox's is `[GUIMEM+0x24]`, else `[gi+0xCD2]` (`0x4A1BCD..0x4A1BDC`), and with neither it repaints through `0x4B0230` instead — then `0x4B9433` and `0x4B9586`. `0x4C6DC0` is the keyed twin with no callers | `8B 44 24 04 83 EC 30` (7) |
| **`0x4C7580`** | **textured-quad stamp** (`GAF_DrawTransformed` [CORPUS]) | stdcall `0x10` | `(ctx, src, int xy[8], int uv[8])` — **FOUR** screen vertices and their texture coordinates (`cmp ecx,0x4` at `0x4C7676`, stride 8; `uv == NULL` synthesises `(0,0)(w-1,0)(w-1,h-1)(0,h-1)`), **MEASURED** `(214,94)(233,94)(233,113)…` with uv `(1,1)(31,1)(31,31)…` — *the first three only; the count was corrected from three to four 2026-09-21* | the vertices' bounding box; **the in-game option screens' wide dark backdrop right of the 128-px panel is drawn as these** (13–37 per build), which is why that region has slanted edges | `B8 8C 7D 00 00` (5, the stack probe) |
| `0x4CCF60` | glyph blitter | cdecl, 9 args | `(base, pitch, font, str, x, y, fg, bg, transparent)` | row `y − (s8)font[2]`, width the sum of `font[off]` per glyph, stops at `\0` **or `\n`**; 2 callers, both in `0x4C14F0` | `55 8B EC 83 C4 F0` (6) |
| `0x4BE950` | `DrawLine` | stdcall `0x18` | `(ctx, x0, y0, x1, y1, colour)` | bbox after `0x4BEA20`; 83 callers | `83 EC 30 56 8B 74 24 38` (8) |
| `0x4BF6F0` | `DrawBar` | stdcall `0xC` | `(ctx, RECT*, colour)` | the rect, inclusive, via `0x4BF620` + `0x4CCDEA`; 47 callers | `83 EC 40 8B 44 24 48` (7) |
| `0x4BF8C0` | `DrawTranspRectangle` | stdcall `0xC` | `(ctx, RECT*, colour)` | hollow rect; 12 callers incl. the minimap view box `0x466B5E`, the HUD `0x467F6C` | `83 EC 68 53 56 57` (6) |
| `0x4BF7B0` | the focus rectangle — **a TINT, not a colour** | stdcall `0xC` | `(ctx, RECT*, level)` | **four** edges of one box — top `(l,t)-(r,t)`, right `(r,t)-(r,b)`, bottom `(l,b)-(r,b)`, left `(l,t)-(l,b)`, in that order — each through `0x4BEC70`, which READS THE DESTINATION; the two groups of four calls are **mutually exclusive arms** on `ctx == NULL` (below); its ONE caller `0x4A16F0` runs it **six times** over expanding rectangles at levels 31, 28, 24, 19, 13, 6, which is the glow (below) | `83 EC 30 53 55 56 57` (7) |
| `0x4BF4D0` | **the box SHADER** — not a fill at all | stdcall `0xC` | `(ctx, RECT*, level)` — the third argument is a **signed shade level**, not a colour | one clip through `0x4BF620` (×**1**), **no `0x4CCDEA`**, then every pixel in the box remapped through a 256-byte LUT row; **what `DrawPopupF4Dialog 0x4948E0` draws its border with** (×3 — three *calls*, not three fills) | `83 EC 40 53 55 56 57` (7) |
| `0x4BFE10` | **the GREY REMAP** — a tint like the box shader, through a table of its own | stdcall `8` | `(ctx, RECT*)` — the rect is read unconditionally (`0x4BFE18`), never NULL | one clip through `0x4BF620`, then every pixel `dst = T[dst]` through the 256 bytes at `globals+0xCC` (`0x4CCED5`); draws nothing while caps bit 8 is clear (`0x4BFE3A`); a NULL ctx is the back surface (`0x4BFE55`). Five callers: four gadget painters greying a gadget (below) and the fog draw `0x4848E0` at `0x484ACD`, per cell | `83 EC 40 8B 44 24 48` (7) |
| `0x4BF620` | the rect CLIPPER the four above share | stdcall `8` | `(clip RECT*, RECT*)` | clamps the second rect into the first and returns 0 when they do not overlap — four `cmp`/early-out pairs, then four clamps. **Carries no colour.** | `8B 4C 24 04 83 EC 10` (7) |
| `0x4CCDEA` | the solid-fill WRITER the four above share | cdecl, 3 args | `(surface, RECT*, colour)` | **uses only the LOW BYTE of `colour`** — see below | `55 8B EC 56 57 53 51 52` (8) |
| `0x4C6890` | `SurfaceFill` | stdcall `8` | `(surface, colour)` | the whole surface | `83 EC 64 53 55 56 57` (7) |
| `0x4C6B70` | surface → surface | stdcall `0x10` | `(dst, src, x, y)` | `0x4CBBE0(dst, src, x − (s16)src[+0x18], y − (s16)src[+0x1A])`; `dst == NULL` ⇒ the back buffer, `src == NULL` ⇒ the screen; 23 callers — the GUI blit `0x4AB158`, the build's snapshot `0x4A9098`, `0x4A90CC`, `0x4A9111`, the teardown `0x4A952B`, the picture tiler `0x4B02DF`, the minimap `0x466B44`, cursor save/restore | `83 EC 30 56 8B 74 24 38` (8) |
| `0x4CBBE0` | `CopyScreenContext` | cdecl | `(dst, src, x, y)` | the whole `src` at `dst+0xC + y·pitch + x`, clipped by `dst+0/+4` only — **reads neither clip rect**; 17 callers: three in the flip, two in `0x4C6B70`, **ten in cursor code** (the `SAVEMOUSE` buffers are written through it directly) | `55 8B EC 83 C4 E4` (6) |

**`0x4BF620` and `0x4CCDEA`, the pair `DrawBar` goes through — and it is the ONLY one of the four**
[DISASSEMBLED 2026-09-18, this project, `objdump -d -M intel` of the pristine build].

**This paragraph said the opposite until 2026-09-18 and the correction is the point of it.** It
claimed `DrawBar 0x4BF6F0`, `DrawTranspRectangle 0x4BF8C0`, the framed box `0x4BF4D0` and the focus
rectangle `0x4BF7B0` "all clip with the first and write with the second, so what the second does
with its colour argument is what all four mean by colour". Counting the calls in each body says
otherwise, and three of the four do not reach `0x4CCDEA` at all:

| function | clips with | writes with |
|---|---|---|
| `0x4BF6F0` `DrawBar` | `0x4BF620` ×2 | **`0x4CCDEA` ×2** — the only one |
| `0x4BF4D0` the box shader | `0x4BF620` ×1 | none — an inline per-pixel LUT remap (below) |
| `0x4BF8C0` `DrawTranspRectangle` | `0x4BEA20` ×10 | `0x4CC7AB` ×10, plus `0x4BE950` ×2 |
| `0x4BF7B0` the focus rectangle | — | `0x4BEC70` ×8 |

So `0x4CCDEA`'s byte-wide colour is a fact about **`DrawBar` alone**, and every statement below
about "the colour" is scoped to it. The three others' colour widths are **not established here**.

**THE COLOUR ARGUMENT IS AN 8-BIT PALETTE INDEX, AND THE SIGN OF THE INT PASSED IS IRRELEVANT.**
`0x4CCDEA` takes `(surface, RECT*, colour)` cdecl — `[ebp+0x8]` the surface (pitch at `+0x8`,
pixels at `+0xC`), `[ebp+0xc]` the rect, `[ebp+0x10]` the colour — and has two paths, chosen by
whether the rect's left edge and `right+1` are both 4-aligned (`and eax,3` / `and ebx,3`, summed,
at `0x4CCDFE`). **Both read the colour as a BYTE:**

* aligned (`0x4CCE2D`): `mov al,[ebp+0x10]` / `mov ah,al` / `bswap eax` / `mov al,[ebp+0x10]` /
  `mov ah,al` builds a DWORD of four copies of the low byte, then `rep stos DWORD`;
* unaligned (`0x4CCE73`): `mov eax,[ebp+0x10]` then `rep stos BYTE al` — again the low byte alone.

**A `DrawBar` colour therefore fits in one `unsigned char`** — which is what the engine itself
keeps, and the same is already recorded for the glyph blitter's three colour arguments at
`0x4CCFE2`.

#### `0x4BF4D0` is a SHADE, and its third argument IS a mode [DISASSEMBLED 2026-09-18]

**The first version of this section drew the opposite conclusion from a function it never
disassembled**, and said so emphatically: *"So a NEGATIVE colour is not a mode. `0x4AA912` calls
`0x4BF4D0(panel+0xBC, rect, -0x18)`, which looked like a special encoding and is not: `-0x18` is
`0xFFFFFFE8`, the writer takes `0xE8`, and the fill is palette index 232."* A survey had flagged
that argument as possibly a shade mode and **the survey was right**; it was overruled on a reading
of `0x4CCDEA`, a function `0x4BF4D0` does not call. Found by landing 8a's review.

What it actually does, `(ctx, RECT*, int level)` stdcall `ret 0xC`, returning 1 on a draw and 0 on
every refusal:

* `0x4BF4D7` `0x4B6220()` → the graphics globals, kept in `ebp` for the whole body;
* `0x4BF4DC` a NULL `ctx` is acquired through `0x4C5E70` (and released by `0x4C5FA0` at
  `0x4BF608`); a non-NULL one is **copied**, `rep movs` of `0xC` dwords, so the context is 0x30
  bytes and the caller's is not written;
* `0x4BF50B` a NULL `RECT*` becomes the whole surface from `globals+0xD4` / `+0xD8`;
* `0x4BF53B` **one** call to the clipper `0x4BF620`; a non-overlapping box returns 0;
* `0x4BF569` the level: `cmp eax,0` decides the table. **Negative** — clamped to `>= -0x20`
  (`0x4BF572`), table `[globals+0xC4]`, row `level + 0x20`. **Positive** — clamped to `<= 0x1F`
  (`0x4BF595`), table `[globals+0xC8]`, row `level`. **A NULL table pointer returns 0 and draws
  nothing**, which is a refusal by data rather than by argument;
* `0x4BF5B5` `row = table + (n << 8)` — so each table is **32 rows of 256 bytes**;
* `0x4BF5E8` the pixel loop, and it is a **read-modify-write of the destination**:
  `movsx ebx,BYTE PTR [ecx]` / `mov bl,BYTE PTR [ebp+ebx*1]` / `mov BYTE PTR [ecx-1],bl`.

**So `0x4AA912`'s `-0x18` is darken level 24 (row 8 of `globals+0xC4`), not palette index 232**, and
the function tints what is already in the box rather than painting over it. `0x4BF620` is shared
with `DrawBar` and carries no colour, so it is unaffected by this correction.

**A negative result worth having: the LUT index is SIGNED.** `movsx` sign-extends the destination
byte, so a palette index of `0x80`..`0xFF` addresses `row_base − 128`..`row_base − 1` — inside the
*previous* row. Whether the tables are laid out to make that deliberate is **not established**; it
is recorded because a consumer that reimplements the remap with an unsigned index will disagree
with the engine on exactly half the palette.

##### Who calls the box shader, and at what level [DISASSEMBLED 2026-09-26]

An `E8` scan of `.text` finds **22 call sites**, and the level at each is a push immediate except
the fade's. Every fixed level is one of +20, +30, +31, −19..−22, −24 and −28, and the fade walks
−19..−28, so **no stock caller reaches row 0 of either table** (level 0 in the lighten table,
−32 in PALETTE.SHD) — and row 0 is the one row whose signed index reaches the 128 bytes *in
front of* its table rather than the previous row.

| call site | in | level | what it shades |
|---|---|---|---|
| `0x41DF70`, `0x41FAE2` | `0x41DF20`, and a function whose early return is `0x41FA8E` | `[main+0x39067] − 29` | a **ten-step fade of the whole screen** (NULL ctx, the rect `0,0`..`main+0x37E1F`,`+0x37E23`): the counter is set to 10 at `0x41DEE5`/`0x41FA3D`, decremented after each shade (`0x41DF92`, `0x41FB04`), so the level runs −19 down to −28, and at 0 `main+0x39063` is set |
| `0x467F45`, `0x4686A3` | `UnitStateProbe 0x467E50`, `UnitBuilderProbe 0x4685A0` (`ta_symbols.txt`) | −24 | none: neither function has a call, a jump or an absolute pointer anywhere in the image — dead code |
| `0x494A93`, `0x494C55`, `0x494C62` | `DrawPopupF4Dialog 0x4948E0` | −24, +31, +20 | the popup's frame |
| `0x4A1F36`, `0x4A1F49`, `0x4A1F5C`, `0x4A1F6F` | the listbox handler `0x4A1B40` | −19, −20, −21, −22 | a disabled or separator row's bands |
| **`0x4A1FC4`, `0x4A1FD7`** | the listbox handler `0x4A1B40` | **+30** | **the SELECTED ROW**: taken when `+0xBA` (selected) equals the row being drawn and `+0xC0` (count) is non-zero (`0x4A1F89..0x4A1F9E`); the two sites are the two arms of a focus test (`[TheActive_GUIMEM+0x20]` against the gadget index, `0x4A1FB3`), identical in arguments, and differ in what follows |
| `0x4A220D`, `0x4A226B` | the listbox handler `0x4A1B40` | −20, +20 | the second at another `+0xBA` test (`0x4A2222`) |
| `0x4A1B32` | `0x4A1AB0` (no `E8` caller) | −20 | |
| `0x4A2BD0` | the slider painter `0x4A2580` | −20 | |
| `0x4A5B14` | the label handler `0x4A56B0` | −20 | |
| `0x4A5F26` | the `id 12` handler `0x4A5E50` | −28 | |
| `0x4A69BA` | the button handler `0x4A5F40` | −20 | |
| **`0x4AA969`, `0x4AA984`** | `GUI_Load 0x4AA8F0`, `flags & 0x800` | **−24** | **the modal dim** — the covered screen's panel, or with no screen the whole back buffer (*The screen lifecycle*, below) |

**Both tables are written once, at init** — which is what lets a reader treat a table read at
the flip as the table every draw of the session used. Each setter has one caller: `0x4BAB00`
(SHD) from `0x42E21B` and `0x4BAB30` (LHT) from `0x42E2AB`, inside `0x42E1D0` and `0x42E260`,
whose only callers are `UIPipelinesInit 0x491200` at `0x49139F` and `0x4913B1`; `0x491200` is
itself called once, from WinMain at `0x49EA62`. The allocations are `0x4BA610` (SHD, `push
0x2000; push 0x50A43C; call 0x4D83B0`, stored at `[globals+0xC4]`), one caller `0x4B5ADF`, and
`0x4BA660` (LHT, into `[globals+0xC8]`), one caller `0x4B5B0F` — 32 rows of 256 each.

**As maps of colour, the two tables are not the same kind of thing** [MEASURED 2026-09-26, the
presented palette of the reference setup]. Fitted as `out = b + k·in` over the 256 palette
entries (one scale per row, an offset per channel, least squares), the modal dim's row (−24,
PALETTE.SHD) is k = 0.513 with b ≈ 0 at rms 8.8/255 — close to a plain darkening. The selected
row's (+30, the lighten table) misses by rms 36.3/255 with the scale pinned at 1, so it is not
an affine map of colour at all. That is why the Vulkan lane's colour rule for a tint is per
entry, not per row ([GUI renderer](../gui-renderer.html) §27.3).

#### A greyed gadget is the grey remap, then the box shader at -20 [DISASSEMBLED 2026-10-02]

The button painter `0x4A5F40` keeps a greyed flag in `[esp+0x40]`. Three arms of its greyed
branch set it (`0x4A60AB`, `0x4A60CC`, and `0x4A6106` unless attribs bit 7 is set, tested at
`0x4A6100` on the low byte), and the attribs-bit-8 arm, which draws the sequence's last frame, does not. The
flag does three things:

* it is passed to `DrawText 0x4A50E0` as the label's shade (`0x4A643F`, `0x4A6531`);
* when it is set, the painter calls `0x4BFE10(ctx, &rect)` (`0x4A69AD`) over the gadget's rect;
* it then calls `0x4BF4D0(ctx, &rect, -20)` (`0x4A69BA`) over the same rect.

The other three greying painters make the same pair of calls: `0x4A1B1F`/`0x4A1B32`,
`0x4A2BBD`/`0x4A2BD0` and `0x4A5B01`/`0x4A5B14`.

The frame itself is not swapped for a grey one. Greyed, the painter takes the first of four arms
that applies: attribs `0x100` draws the sequence's last frame, `count − 1` (`0x4A6081` tests
`ch & 1`); a nonzero `+0x136` draws the stage frame `[+0x137]` (`0x4A6092`); attribs with a bit
of `0x1800` set draw frame `[+0x13B]` (`0x4A60B8` tests `ch & 0x18`); and any other draws
`min([+0x138] + 2, count − 1) + [+0x13B]`, where `+0x138` is the pressed word (`0x4A60D9`). Not
greyed, the frame comes from the stage bytes `+0x136`/`+0x137`, or from `[+0x13B]` plus the
pressed word (`0x4A611C`..`0x4A616B`). The record's greyed byte is `+0x13C` (`0x4A6071`).

**The grey table.** `globals+0xCC` is 256 bytes:

* allocated by `0x4BA6B0` at `0x4B5B29`, only when caps bit 8 (`[globals+0xF1] & 1`, `0x4B5B24`)
  is set;
* freed by `0x4BA6E0`;
* built from the palette by `0x4BAD30` (a nearest-colour search per entry through `0x4BA9D0`),
  called from `0x42E2F0`, whose one caller is `UIPipelinesInit 0x491200` at `0x4913C3`, right
  after the SHD and LHT builds;
* its in-place setter `0x4BAB60` has no direct caller.

So, like the other two tables, it is written once at init. Its writer `0x4CCED5` (cdecl
`(dst, pitch, w, h, T)`) indexes the table unsigned in both its 4-aligned and its byte path, so
the row the remap applies is the table itself, with no signed half to fold.

**MEASURED 2026-10-02**, MUSICRT in a paused game, 640×480, the original game:

* the engine surface under `tagpu_gdi.on` and under Vulkan was identical;
* the left slider arrow's frame 16, through the `-20` row alone, matched 53 of its 88 opaque
  pixels; the other 35 were its green pixels, which the grey remap had made grey first.

[Established because the vulkan-only plan's landing 8 turns these ops from published SURFACE BYTES
into drawn geometry, and needed to know what each one's third argument means before choosing a
packet field. Landing 8a shipped `OP_BAR` only, which is the one this section gets right.]

Not drawers, checked because the popups call them: `0x47F1A0` (helpers `0x47F0C0`, `0x44FDB0`,
`0x451DF0`, …, no pixel write), `0x4B6560` (`jmp [0x4FC0DC]`, an import thunk), `0x4A5030`
(text measure, calls `DrawText`). `0x4C69A0`, `0x4C61F0` and `0x4C5FA0` begin with `E8` and
cannot be prologue-detoured without relocating the call.

### The popups, chat and the HUD painter [VERIFIED call graphs]

#### In-match text chat: sender, audience and received display

DISASSEMBLED 2026-10-05 from `pristine/TotalA.exe.pristine`, with a whole
`.text` relative-call sweep. These are stock text-chat facts; the work branch
also carries a unique identity in the original native text delivery below. A send
attempt, a transport success and a recipient's displayed line are separate
observations. Primary per-pair message ordering remains unproved.

| Address | Established behaviour and evidence |
|---|---|
| `0x494050` | Opens `TALK.GUI` or `TALK2.GUI` through `0x4AA8F0`, stores callback `0x493BF0` at the returned GUI object's `+8` (`0x494120`), seeds `TALK` from `0x51E788` through `0x4A0BF0`, sets the edit index through `0x49FC50` and the GUI object's focus at `+0x20`. It refuses PlayerInfo watcher bit 6 (`+0x9B`, `0x49407C..0x494081`). Direct E8 callers are `0x493D37` and `0x496508`; the callback itself is indirect. |
| `0x493BF0` | Chat GUI callback, `ret 4`. Reads UIChange at GUIInfo `+0x60`. `LIVEPLYR` changes the indexed `main+0x2BF1` byte and mode to 3 (`0x493C6E..0x493CC3`); `SENDTO` changes the console flag and rebuilds the dialog (`0x493CDA..0x493D3D`); `SENDTYPE` selects mode, bounds it below 4, then calls `0x493AE0`. The `TALK` submission copies the inline field at `+0xB6`, skips leading spaces, and sends a leading `+` command to `0x417B50` before the remaining text goes to `0x463E50` (`0x493FAF`). It saves/restores mode and ten recipient bytes around shorthand overrides. |
| `0x493E04..0x493FAF` | Command run level starts at 1, becomes 7 with `main+0x37F2F` bit 1, and gains bit 1 when `0x5091CC` is nonzero. Chat shorthand accepts a digit or `a`/`e` followed by a separator from `0x5093F4` (`,:;`). A digit selects one native-ID-bearing row in mode 3; `a`/`e` select modes 1/2 with captions `Allies`/`Enemies` at `0x508384`/`0x5093EC`. A space is not one of these separators. |
| `0x493AE0`, table `0x493BD8` | Loops ten rows at stride `0x14B`, skipping inactive/type-4/self rows. Mode 0 selects all; mode 1 reads the controlled player's alliance byte at `player+0x108+row`; mode 2 inverts it; mode 3 reads `main+0x2BF1+row`. Calls `0x4A1080` to show the corresponding `LIVEPLYR` stages. This updates the UI; actual send filters are below. |
| `0x463E50(player, text, flags, caption)` | `ret 16`. Formats `<%s%s%s> %s` (`0x5072C8`) from `player+0x49`, optional `->` (`0x5072D4`), caption and text. Calls `0x453360` at `0x463E9F`, then `0x463CA0(line, flags, 0, 10)` at `0x463ED4`. Its six direct E8 callers are `0x447280`, `0x448042`, `0x448460`, `0x448F77`, `0x45398B` and `0x493FAF`: it is not an in-match-only boundary. |
| `0x453360(line)` | `ret 4`, one direct E8 caller `0x463E9F`. Constructs byte `0x05` followed by `strncpy(...,64)` in `main+0x2A38`; sends exactly 65 bytes. Source ID is the first controller-1 row's `player+4`, or −1. Mode 0, or an input line beginning with `+`, uses `0x451DF0` (`0x4534B6`). Mode 3 loops the ten recipient mask bytes and calls `0x451BC0` for nonzero target IDs (`0x4533F9`). Modes 1/2 target active controller-3 rows according to the controlled player's alliance vector (`0x45348B`). It adds no event identifier and performs no text deduplication. |
| `0x451BC0(sourceID, destinationID, data, bytes)` | `ret 16`. Maps IDs with `0x44FFD0`/`0x44FE40`. Requires the network bit, an active local controller-1/2 source and active controller-3 destination, both with `+0x22 == 0` (`0x451C87..0x451CDF`). A later ID scan can take a controller-1/2 local shortcut returning success without a transport call (`0x451D66..0x451DCA`), so its return alone does not prove delivery. With `0x506DBC` set, calls `0x4619B0` at `0x451D8B`; otherwise calls `0x4C97B0` at `0x451DAC` and treats zero as success. B8 already owns the generic `0x451DF0` entry. |
| `0x461990`, `0x4619B0` | Generic and targeted queue adapters, `ret 16`, into `0x462710`. The targeted adapter resolves the destination through `0x461630(destination,1)` and returns zero on a miss (`0x4619B7..0x4619D9`). This trace does not prove recipient ordering or exactly-once display. |
| `0x453D40`, `0x453DAE..0x453ECA`, `0x455F78` | Dispatcher reads native source/destination IDs from `main+0x4C9/+0x4CD`, scans ten controller-bearing rows, puts source row at stack `+0x14`, destination row at `+0x38`, sender record in EDI and destination record in EBX. A missing identity becomes row 10. The `0x455F78` branch commits a found source row before destination scanning. These identities precede the text-case switch; formatted names are not the identity. |
| `0x45522E`, table entry `0x455F90` | Text `0x05` receive case. Requires active destination EBX and `EBX+0x73 == 1`, then calls `0x463CA0(message+1,8,0,sourceRow)` at `0x455250`. Thus an AI destination is not a displayed human chat recipient. B4's `hit_rx_chat` owns the table entry and only handles its binary envelopes when text byte 1 is zero; ordinary nonzero text continues to this stock case. |
| `0x463CA0(text, flags, unitID, sourceRow)` | `ret 16`, shared display sink. Returns for empty text or zero `ScreenChatLines` at `main+0x37F27`. Thirty entries at `main+0x12EF`, stride `0x48`, with head/tail `+0x2A3E/+0x2A40`. Copies 64 bytes then sets entry `+0x3F` to NUL, giving at most 63 displayed bytes (`0x463D25..0x463D41`); stores GameTime at `+0x40`, unit ID at `+0x44`, source row at `+0x46` and low four flag bits at `+0x47`. Plays `MessageArrived` (`0x5072B8`) only when sourceRow != 10. Other callers include console replies, unit replies and death/results notices: recording every call as human chat would misclassify them. |

`tagpu_wire_received` (`tagpu_patches.c`) first reports the current owned
primary frame's delivered span when its argument equals that frame's buffer.
Otherwise it reports B3's retained SDK span only for the exact retail TLS
receive-buffer pointer. Both identified chat and the guarded unit-movement
decoder use this API. The primary
retail pump receives **one split message** per call to `0x4534E0`; the splitter's queue
records a pointer and length for each message (`0x463A5B/0x463A6F`, or
`0x463B6A/0x463B7E`). The retail pump's statistics call records the length delivered
into `main+0x2A38`, rather than the earlier aggregate; the owned receive path
publishes the same delivered span for its private allocation. The new text receiver
requires that exact pointer and exactly 97 delivered bytes before copying.
An arbitrary interior pointer does not inherit that bound.

##### Identified native text delivery

The work branch's `mp_init` also registers fixed code `0x2B`, whose stock
length is zero and dispatch target is the receive loop. A 97-byte
`RecMpChatWire` contains version/length, fresh match GUID, nonzero 64-bit sender
serial, source GameTime and the original 65-byte text packet. Identity is
`(match, native transport sender, serial)`; a repeated line or a paused clock
cannot alias another event. Source and recipient clocks remain separate.
This installs native delivery metadata, not communication-file persistence or
multiplayer playback. It leaves primary reliability and message ordering
unchanged and requires the product's same-build peers.

| Address / field | Established behaviour and branch use |
|---|---|
| `0x451FD0..0x452322`, `0x512B84`, `0x512ACC`, `0x512C6C` | Packet initializer zeroes the length table and never assigns code `0x2B` a positive length or callback; the whole-body disassembly contains no later store to these three entries after `0x4520BE`. The recorder initializer stub writes length 97, callback `0x44FD40` and receive mask 7, as well as extended start length at `0x512AF8`. All three splitter walks use the length table. Mask bit 2 admits net state 6; state 5 and other states require bits 1 and 0 respectively (`0x454745..0x4547A7`). The identified adapter further requires state 5 or 6 and a copied current match. |
| `0x44FD40..0x44FD43`, `0x44FDA0..0x44FDAC`, table `0x512A20` | The registered default callback returns −1 with `ret 4`, the same body as stock text callback `0x44FD60`. `0x44FDA0` indexes this callback table with the message code and makes an indirect call at `0x44FDA5`; registering a length alone would leave that callback missing. No new meaning is assigned to −1. |
| `0x493FAF`, `0x463E9F` | The first call scopes one ID over the in-match GUI's native `0x463E50`; the second observes that scope after formatting in the engine's 200-byte stack buffer (`sub esp,0xC8` at `0x463E54`). The callback stores its controlled-player record at stack `+0x24` (`0x493E6F..0x493E98`) and passes that pointer at `0x493FAE`. Front-end callers remain outside the scope. The copied current-match inbox authorizes loading/play scope before or after B2. The scope is only mutated by the owning game thread, saves/restores its outer value on the stack, and exhausts rather than wrapping its serial. |
| `0x4533F9`, `0x45348B`, `0x4534B6` | Call-site replacements wrap only scoped GUI text, then call the original targeted or generic sender. The native audience loops, masks, alliance tests and destination filters remain in those original functions. One source event has the same ID at every target call; a transport return is not a recipient receipt. |
| `0x4628D0`, `0x46290D..0x46292C` | Queue append compares current bytes plus supplied length with `0x42A` at `0x462911`; on the accepted path it copies all supplied bytes into its owned packet at `+0x14` before returning. The call's stack packet therefore outlives no send. Its recorded entry length is supplied length at `0x46293F`. This proves the copy lifetime, not per-pair ordering or reliable delivery. |
| `0x456028`, stock value `0x455F50` | Dispatch-table slot for code `0x2B`, selected after the code's receive-mask check. The new stub preserves flags/registers and gives its original frame to `mp_chat_receive`. Bounded native sender/destination rows and IDs, local controller-1 destination, exact delivered span, version/match/serial and embedded `0x05` nonempty text are validated. On success, a copied 65-byte stock image replaces the front of the owned writable receive buffer, then the stub enters `0x45522E`; on refusal it enters `0x455F50`. It changes no pump pointer and retains no engine address after the call. |
| `0x4C97B0`, `0x4C9800`, `0x50A780` | Ordinary raw send derives SDK flags 0/1 from whether `0x50A780` is nonzero (`0x4C97D5..0x4C97EA`), then calls vtable `+0x68` at `0x4C97F6`. The adjacent wrapper always supplies flag 1 (`0x4C9830/0x4C9839`). Installed `dplay.h` defines these as non-guaranteed/guaranteed. This says nothing by itself about the engine's queued transport. |
| `0x4C9790`, `0x4C97A6`, `0x4CA56C`, `0x4CA5E3` | Setter `0x4C9790(value)`, `ret 4`, writes the flag at `0x4C97A6` and returns its old value. Whole-`.text` direct references find two other writes: 1 at `0x4CA56C`, 0 at `0x4CA5E3` in initialization beginning `0x4CA5D0`. Setter call sites are `0x427D46`, `0x4563A0`, `0x4563D2`, `0x4565CF`, `0x45660E`, `0x498580`; their surrounding branches temporarily disable/conditionally restore it or change loading/menu state. No invariant that chat always uses guaranteed SDK sends is established. |

MEASURED 2026-10-05 on the reference setup, before the new wire path: a native
three-human *Town & Country* game receives two identical public lines on all
three peers and a digit-selected private line on sender/selected destination.
The other peer's sampled queue after a subsequent public marker has no private
line. Source/received ticks are `280/245`, `407/373`, `547/510`, `651/616`;
these observations do not establish an offset or primary ordering. Each peer
reads `0x50A780 == 0` and `0x506DBC == 1`. A diagnostic dword at `main+0x1427F`
reads `0x000F004B`; its combined field meaning is unestablished and it is not
used as an authority. These are queue observations, not a complete replay file.

MEASURED 2026-10-05 on the reference setup, corrected COVER wire path, three
human peers with local recording off: source `674591616` emits serials 1..5 at
ticks `279,400,526,697,824`. Recipient `674591618` accepts all five at
`244,365,490,662,789`; recipient `674591620` accepts 1,2,4,5 at
`241,362,660,787`. Every received serial/source tick and all 65 stock bytes'
CRC match the source. The two identical public lines share CRC `6585E629`
and have distinct serials 1 and 2. The digit-selected private line is serial 3,
CRC `0056345B`. A 60-character `full-` plus narrow `i` line is serial 4, CRC
`40E30A33`, with exactly equal 63-character visible queues on all three peers.
Serial 5 is the subsequent public marker, CRC `67CE86A6`. The native IDs,
fresh match GUID, tested DLL/map/hash, retained logs and queue snapshots are
the probe evidence. All owned instances/prefixes/display are removed. This
establishes in-play queued transport. That wire-only probe does not establish
raw transport, loading acceptance, persisted communication or playback; the
following ledger measurements address storage.

DISASSEMBLED for the copied communication ledger: the formatter calls the
native sender at `0x463E9F`, then its optional screen-message path, then calls
the local sink unconditionally at `0x463ED4`; it returns with `ret 0x10` at
`0x463EDF`. The source scope copies its 65-byte packet at the sender boundary,
audience mode from `main+0x2BF0` and actual intended target calls at
`0x4533F9/0x45348B`, then publishes copied source/local-echo metadata only after
that formatter returns. Generic `0x4534B6` retains its distinct public audience.
The validated receive boundary contributes an observation under the actual
native destination's own stream/tick, with stock display flag 8. No success
return from the targeted sender is treated as recipient acceptance. These
copied contribution records add no engine write or primary patch site beyond
the identified wire above.

MEASURED in the kind-12 recording probe: source/local sink flags are 4; actual
remote text flags are 8. Source `676590527` event 3 has audience mode 3 and the
single intended target `676590525`; only that peer displays the private text.
Its original tick is 511, the peer's acceptance tick 476 and the saving host's
observation-publication tick 519. Both other peers save no local file but publish
their actual observations. The completed file contains five originals and nine
receipts, with exact wire CRCs/65-byte data retained. Public event 1 and 2 share
their packet CRC `20E5EB9A` but remain separate identities. Ordinary teardown
closes at GameTime 1027 with zero corrections. C/Python reader application bytes
remain identical through world seek; engine multiplayer playback remains refused.

MEASURED in the separate saving-off private-source probe, three humans on
*Town & Country*, COVER, native queued DirectPlay: authors `677804643` and
`677804645` each send serial 1 only to the other. Their stock packet CRCs are
`FCC570F5` and `FDABD2F3`; source ticks are 265 and 355, actual recipient ticks
262 and 366, saving host application ticks 311/313 and 406/411 for originals/
receipts. Both original modes are 3 and each has exactly the other's native
ID as its target. The saving host `677804641` has neither private line in its
sampled native queue after its public marker and logs no private receipt.
Its own serial 1 public line has CRC `F70E8277`, source tick 515, recipient
ticks 478/471 and host application tick 522 for both receipts. All three
author/serial pairs stay distinct. The complete file retains three originals
and four actual receipts; C/Python bytes and reader seek agree. Neither
saving-off peer logs a recording writer. This establishes contribution of
chat the recorder was never addressed; it does not establish replay display.

##### Loading acceptance remains a separate lifetime boundary

DISASSEMBLED 2026-10-05: stock code `0x05` has mask 7, stored at `0x45208C`
to `0x512BD4`, after length 65 at `0x452078` and callback `0x44FD60` at
`0x452082`. The state gate at `0x45473F..0x4547AD` accepts mask bit 1 in
state 5, bit 2 in state 6 and bit 0 otherwise; a failed state-5/6 bit does
not fall through to bit 0. The identified code uses mask 7 and its adapter
bounds accepted states to 5/6. Its former mask 4 omitted stock loading admission.

A whole-`.text` relative-call sweep finds seven direct calls to `0x453D40`:
`0x41F80B`, `0x434EAF`, `0x44A69C`, `0x4954C8`, `0x4968CB`, `0x49727D`
and `0x49852E`; the sole direct call to `0x4534E0` is `0x453D94` in that
pump. This sweep does not exclude indirect calls. The loader's only direct
pump is its early wait at `0x49727D`, before `LoadGameData_Main` at
`0x497581`. Its subsequent bit-3 wait at `0x4975D6..0x4975F1` sleeps without
another direct pump; its last store sets bit 1 at `0x497C62`. The game thread
still pumps during the loading frame at `0x49852E`. Once it reads bit 1 at
`0x498342`, it calls `0x496790` at `0x49842F`; that frame pumps in catch-up
ticks and at `0x4968CB` before `0x498445` stores state 6. Thus state 5 also
includes a phase after loader completion and before the first B2 producer.

B3's fallback `wire_rx_note` stores the current `main+0x2A38` pointer and
delivered length in TLS at the receive statistics call. That binds the span
to a thread; it supplies no cross-thread lifetime. The work branch's owned
pump below removes that input-buffer dependency when the recorder's atomic
primary installer arms. The identified receiver now copies loading/play
observations into a process-owned inbox before entering the stock text case.
It does not require a live B2 producer or the GUI thread. Native execution of
loading delivery and its saved clock/order still needs a separate gate; the
controlled adapter test below is not that native loading run.

`tagpu_recmp_chat_inbox.c` owns one fixed 4,096-observation allocation. Its
critical section protects only copied match/host metadata, queue values and
the acceptance-time read of the supplied process-lifetime main GameTime field.
Held bodies contain no engine/SDK call, logging, allocation or acquisition of
another lock; the wire parser and text-queue transfer helpers use bounded
owned data only. Thus its wait cannot form a cycle through an engine or SDK
lock acquired by the holder. Initialization/free occur outside ownership;
installed callbacks retain the allocation for the process lifetime.

Start publication resets the copied epoch. A pending start and its observations
survive level-load teardown; first B2 claims it exactly once. That game producer
copies the frozen roster before transferring entries, requiring the local
recipient and bounded origin/target IDs. It drains before source advance and
again at teardown before final seals. Final transfer and admission closure
share one ownership interval: an admitted observation is transferred, and a
waiting receiver cannot append after the cut. Level-owned queues are freed
only after closure. Overflow, decreasing observation clocks or an
invalid bootstrap refuse contributions instead of dropping an event from a
successful file. Valid wire packets still restore their stock image on inbox
exhaustion. Source and receipt clocks are not changed to the first B2 tick.

The direct GameTime write inventory has five sites: shell increment
`0x44A696`, sim increment `0x4954C0`, and zero stores `0x491979`, `0x4971BB`,
`0x498180`. The loader zero precedes its early pump; level initialization
zeroes again from `0x4917D0`; the game loading entry also clears its clock.
These direct stores do not alone prove all possible loading observations
monotonic or exclude aliased writes. Native loading clocks remain a gate.

Production inbox/queue controls retain zero receipt ticks and distinct source
tick 13, preserve a pending epoch over loading reset, refuse a second claim,
and transfer 4,000 two-thread receipts. A caller blocked on inbox ownership
reads its acceptance clock only after acquiring that ownership; producer drain
cannot pass an observation whose publication owns the same lock. Controls also
cover overflow with stock restoration, changed-match and frozen-roster refusal.
A receiver held at publication ownership refuses after the final transfer
closes admission, even when the game queue has already been freed.
The exact production `mp_chat_receive` body, with only main/mode/span/log
boundaries substituted, accepts state-5 calls from both the game and a foreign
thread without a producer, restores all 65 stock bytes, and refuses wrong
mode/state/row/span/controller/network inputs. No native loading display or
saved-file application is claimed from these controls.

The integrated inbox's native three-human queued run separately passes seven
exact original/actual-recipient entries through C/Python reader seek, with the
host saving and two peers opted out. The final admission-closure build's file
closes at tick 764 with zero corrections; all 311 application envelopes /
71,127 bytes, CRC `0FE1FD27`, agree.
The host displays neither private line. Continued contributions after host
departure still refuse on the surviving peers. This is in-play integration,
not execution of the state-5 path in a native loading frame.

##### Owned primary receive frames

DISASSEMBLED 2026-10-05, retail `.text`, and checked against the branch's
`mp_receive_enter`, `mp_receive_packet`, `mp_receive_leave` and `mp_init`:

| Address / field | Established behaviour and branch use |
|---|---|
| `0x453D40..0x453D5C`, `0x455F50..0x455F77` | No-argument pump, plain `ret`; first five bytes load main. Network bit `main+0x2A44` gates it. The return paths restore its `0x51C` locals and four saved registers. A wrapper reserves a 32-byte private frame on its stack, preserves registers/flags around enter/leave, calls a copied-prologue trampoline and returns the original result/flags. `TryEnterCriticalSection` admits the owner through native receive/dispatch and the final calls `0x450980`/`0x453C20`; a busy caller returns 0 without receiving or changing TLS/IDs. The parent frame and its source/destination words are restored after a nested call. |
| `0x453D8A`, `0x453D90`, `0x453D94` | Stock takes the shared message pointer once and calls receive at `0x453D94`. That call now enters the owned receiver. Its call-entry word 5 is the pump's `esp+0x10` pointer; only a successful bounded receive replaces it. Each recursion depth reuses its own process-lifetime allocation across calls, threads and levels. Pump return does not free the packet; the receiver reuses or resizes its storage only on a subsequent receive at that depth. Native dispatch retains its packet mutations. |
| `0x4534E0..0x453639` | Stock no-argument receive returns 1 on a delivered message, 0 on no network/no message/error. It selects `0x462F30` or `0x4C9840` using `0x506DBC`, initializes size from `main+0x2A34`, and reallocates `main+0x2A38` through `0x4D84A0` at `0x453565`/`0x4535EE`. Its sole direct relative caller is redirected; the old B3 statistics stubs remain available if the atomic primary installer refuses. |
| `0x462F30..0x4635A3`, object `0x51E300` | `thiscall(context, buffer, DWORD *bytes)`, `ret 12`. It scans ten 0x34-byte source-ring records at `this+0x20..`, consuming due entries, or receives an aggregate through `0x44F9C0` at `0x4630DE`/`0x46314D` and splits it. The queued-message branches converge at `0x46354F`: they copy EDX bytes without comparing caller capacity, then store length at `0x463567`. The branch at `0x463589` writes source/destination to context `+0x4B5/+0x4B9`. System source 0 instead bounds size against caller capacity at `0x4633E3..0x463429` and preserves a too-large message for retry. |
| `0x463919..0x463939`, `0x4639E5`, `0x463B33`, table `0x512AD8` | Split messages have u16 lengths: the variable `0x2C` length uses a 16-bit reader call, fixed messages take the table's word, and the other walks mask to 16 bits. The owned queued receiver allocates 65,536 bytes before entering the transport, covering that entire range even though the final copy does not check capacity. The installer compares the variable reader call and fixed word-load bytes as prerequisites. |
| `0x4C9840..0x4C9882`, `0x4C987E` | `stdcall(context, buffer, DWORD *bytes)`, `ret 12`; tests primary interface at context `+0x4C5`, then calls vtable `+0x64` with receive flag 1, output IDs at context `+0x4B5/+0x4B9` and the caller's buffer/size. For main's context at `main+0x14`, these are `main+0x4D9`, `main+0x4C9/+0x4CD`. No primary interface is retained by the new code or sent to a worker. |
| `0x887700BE`, `0x8877001E` | Stock distinguishes no messages and buffer too small using these HRESULTs. The owned receive retries too-small only if its reported size strictly increases capacity; a failed allocation leaves the SDK message pending. Delivered length must be nonzero and at most allocation capacity before dispatch/span publication. Other errors return 0, matching the stock pump contract. |
| `0x415EF0..0x415F3C`, `0x415F40` | Message statistics are `stdcall(code, bytes, 0)`, `ret 12`; the raw path additionally calls packet statistics `(bytes, 0, 0)`. The replacement preserves both calls. The message routine updates received bytes at `0x511BC0` and code-indexed counts/bytes at `0x511A58/0x511C58`; these counters are not packet capacity or a receive lifetime. |
| `0x455002..0x45504A`, `0x455050..0x455053` | This case first reads input from the pump's private pointer, then obtains `main+0x2A38` to construct an outgoing `0x1C` reply. Its direct shared-buffer read is an output builder, so moving incoming packets does not require redirecting it. The sweep finds no other direct `+0x2A38` load inside this pump except its initial pointer read. |
| `0x45486D`, table `0x455F84`; `0x45497A`, `0x4549CC`, `0x454A26` | The case switch spans codes 2 through `0x2C`. Several cases reread `main+0x4C9` after the initial sender scan; a lock around only receive would leave their sender metadata exposed to another pump. The wrapper retains ownership through dispatch. |
| `0x454425`, `0x454548`, `0x45456C..0x45456E` | The system `0x102` case obtains its data pointer from packet `+0x0C`, then copies 46 dwords and one byte into existing PlayerInfo. This particular sink copies 185 bytes rather than retaining that data address. |
| `0x45469E`, `0x454712..0x454732`, `0x4E4760..0x4E47E8` | The system name-update path passes packet `+0x18/+0x14` pointers to the bounded string-copy routine with count 30 and destinations at player `+0x2B/+0x49`. The routine copies source bytes into destination and pads after NUL; it retains no source pointer. These facts cover this sink, not every system-message consumer. |
| `0x454685..0x454697` | A system session-data branch copies twenty dwords from message `+4` into `main+0x471`. |
| `0x4553EE..0x4553F9`, `0x48B920..0x48BACF` | The `0x2C` case passes its packet and sender player to `0x48B920`. That decoder constructs a stack bit-reader at `0x48B933..0x48B93F`, reads code/length/GameTime through `0x415DC0`, stores GameTime at player `+0x18`, and passes the stack reader to mover vtable `+0x24` (`0x48BA10`). Dirty updates may create through `0x4861D0` at `0x48BA00`; completed updates call `0x43DD20` and `0x48A870`. The round-robin path calls `0x48B3F0` at `0x48BAC3`; `ret 8` at `0x48BACF` returns to the pump. The guarded copy owns the decoder's padded packet span; this trace alone does not audit every mover's retention. |

Additional DISASSEMBLED consumer and pump-tail facts:

| Address / caller | Packet use or owned fields |
|---|---|
| `0x450980..0x450A0C`; pump calls `0x45493C`, `0x455F5F` | No callees or packet argument. Zeroes eleven stack counters and scans ten active controller-1/2/3 player rows, excluding player `+0x146 == 10`. Player `+0x0C` is bounded to 1..10 before indexing those counters; the first duplicate sets main `+0x299C` to 1, otherwise 0. Group meaning is [INFERRED]. |
| `0x453C20..0x453D30`; pump call `0x455F64` | No packet argument. Main `+0x37F2F` bit 0 skips the scan; paused `+0x38A51` bit 0 saves the clock from `0x4B6340` to `0x512C7C`. Ten controller-3 rows compare clock minus max(saved clock, player `+0x1C`) against 30 times main `+0x37F31`. Calls `0x453A50` with −1 at `0x453D11`, or the chosen row's native ID at `0x453D20`. This body does not establish the callee's full closure. |
| `0x4565A0..0x456759`; pump call `0x454B2F` | Timestamp echo, `ret 4`. A request with packet `+5 == 0` writes the native clock there, passes the same 13-byte packet through `0x451BC0` at `0x4565EA`, and flushes the queued sender through `0x4618A0` before/after when enabled. `0x4C9790` temporarily selects raw sending. The response branch copies clock minus packet `+1` into matched player `+0x14` at `0x456743`; it stores no packet pointer. Queued send's copy contract is established separately at `0x4628D0`. |
| `0x48AB70..0x48AD2D`; pump call `0x455403` | Attachment input, `ret 4`. Reads u16 unit IDs at packet `+1/+3` and state bytes `+5/+6`; the packet address is neither stored nor passed to another function. Stored links at unit `+0x86/+0x8A/+0x8E` are unit-array addresses, state writes are unit `+0xF9` and mover `+0x2E` low bits. Calls `0x47CB00`, `0x47CB40`, `0x4384A0` and `0x48C9B0` with unit/object arguments. These are retention facts, not a new proof of every ID bound. |
| `0x49AF90..0x49AFF4`; pump call `0x455443` | Reads packet coordinates `+1/+5/+9` and weapon byte `+0x0D` while scanning main `+0x141F3` records from pool `+0x141F7`, stride `0x6B`. The matching branch calls `0x499EB0(projectile, 0)` at `0x49AFEB`; it never forwards the packet or uses the sender argument. |
| `0x47F300..0x47F604`; pump call `0x45563F` | Positional sound, `ret 12`. The native case passes packet `+6` as the point and flag 0, so the outgoing branch does not rebroadcast it. That branch copies three point dwords into an 18-byte stack message before `0x451DF0` at `0x47F3AF`. The positional playback branch builds local coordinates and passes their stack copy to `0x4CF570` at `0x47F579`; fallback calls pass NULL. `0x4CF6C9..0x4CF6E0` converts the copied point into float arguments passed by value to COM vtable `+0x4C`, not a retained point address. This identifies the inspected positional path, not every sound callback. |
| `0x46D500..0x46D51E`; pump call `0x4559B7` | Unsequenced unit-sync frontend, `ret 8`. Zeroes packet `+2`; if packet `+1 < 100`, forwards the packet and sender row to `0x46D6C0`. Unlike the copying sinks above, this frontend alone does not establish its callee's retention behaviour. |

A speculative direct-call/branch traversal reaches 1,462 entries with 425
unresolved calls/jumps, including virtual calls and inferred jump-table extents.
It is not a complete wait-cycle or callback proof. Nonblocking pump admission
removes the introduced lock-wait edge by construction without depending on a
negative result from that traversal.

The whole-`.text` positive-offset reference inventory also finds shared-buffer
loads at `0x450CD9`, `0x451023`, `0x451AD7`, `0x452783`, `0x452870`,
`0x452AFE`, `0x452B7E`, `0x452BF2`, `0x453099`, `0x45332B`, `0x453368`,
`0x45337F`, `0x4533EE`, `0x453479`, `0x4534AC`, `0x4563E2`, `0x456520`,
and the stock receive/growth/statistics sites `0x45350B`, `0x453558`,
`0x45358C`, `0x4535A1`, `0x4535E1`, `0x453616`. The pointer stores are
`0x453573`/`0x4535FC`; capacity reads/stores are `0x4534F3`, `0x453548`,
`0x4535D2`. This instruction inventory is not an indirect-reference proof.
In particular, `0x453360` constructs chat in that buffer and `0x453320`
constructs loading-ready messages there. Locking only receivers would not
protect an incoming packet kept in the shared buffer from those sends. The
owned input allocation removes that dependency; it does not serialize all
native output builders or establish their separate lifetimes.

Controlled execution of the exact production receive core and its actual
`tagpu_wire_received`/`wire_s2c_copy` integration, with only SDK and
statistics targets replaced, passes a 65,535-byte split packet, raw growth to
70,001 bytes, native-output-buffer overwrite, nested storage and
source/destination/span restoration, no-message/invalid-size/network refusal,
4,000 two-thread receives and 200 nested receives. Both depth slots are reused
throughout those calls. Aliases to the last parent and nested packets remain
readable after both frames return. A foreign thread finishes its busy admission
while the owner holds the lock, with no SDK call, TLS frame or consumed packet;
the owner's exact span and bytes remain unchanged. The copy controls also
cover owned and retail spans, header clamping, zero padding, and refusal of
interior pointers and an inactive nested parent. The native three-human
COVER probe separately exercises the installed wrapper through lobby, load,
private/public delivery and orderly recording closure at tick 709, zero
corrections. Seven exact text observations agree with native source/recipient
logs and C/Python reader seek. A separate raw three-human run closes at tick
799 with zero corrections: seven text entries, eight latest-batch queries and
all 340 application envelopes / 162,631 bytes, CRC `0E49B035`, agree through
C/Python reader seek. Its observed internal speed is 10 despite a requested
registry value of 20. These text probes precede the movement integration fix.

The native movement regression demonstrates why testing chat alone is
insufficient. On *Two Continents*, two humans with a 20-unit limit each move
their commander by 96 world units. The preceding branch baseline propagates
both units to both observers. Before the integration fix, each owner's unit
moves locally but both remote images remain at their initial positions through
ticks 2057/2043; the host's movement heartbeat records `2c=0` and 1975 stale
refusals. `wire_s2c_copy` had read only the retail statistics-hook TLS, which
the owned receiver bypasses. It now obtains the exact span from the shared
receive API. The fixed queued and raw native fixtures both observe both units
move on both peers. Queued observation finishes at ticks 363/342, with 333
movement packets on each endpoint; raw finishes at 331/299, with 294/245
movement packets. Every sampled stale count is zero. Each run retains its
tested COVER DLL/map/hash, native tick/roster samples and endpoint logs, then
removes only its own instances and display.

After retaining depth storage and changing admission to `TryEnterCriticalSection`,
the installed 32-byte wrapper passes both native movement fixtures again.
Queued observations finish at ticks 315/295 with movement counts 270/319;
raw finishes at 336/311 with counts 200/222. Both use the same retained COVER
build, and every sampled stale count is zero. Neither saving-off endpoint
reports contribution refusal. The tests remove their own instances/display.

The storage lifetime no longer depends on every native consumer dropping its
packet pointer before pump return. Only a later receive at the same depth can
invalidate it; retail shared input can already be overwritten or resized by
any later receive. Nested receives use a different slot. Nonblocking admission
adds no lock-wait edge to the engine's loader/game-thread locks and consumes no
message on refusal. This does not establish the costs or callback behaviour of
the SDK itself, or the lifetimes of separate native output builders. The copied
chat/system sinks and inspected movement path do not prove every native parser
reentrant. Continued host migration, native loading-chat verification, receive cost and
physical bandwidth also remain open.

#### Textfield input beneath the chat GUI

DISASSEMBLED 2026-10-05. The editor is `0x4A7290`; `0x4A4D70` is its painter.
These facts identify observations for the failed native chat driver; they do
not establish the cause of its empty field.

| Address | Established behaviour and evidence |
|---|---|
| `0x4B5CC0`, `0x4B5DE5` | Window procedure's `WM_CHAR` arm queues wParam through `0x4C1B20` at `0x4B5DEA`. Keydown uses `0x4C1D50`; mouse moves update the current record via `0x4C2360`, while button records go through `0x4C2E30` at `0x4B5F51`. |
| `0x4C1D50`, tables `0x4C21D0`/`0x4C2230` | Keydown maps `VK_END` through `0x4C1EF7` to ring code `0xF1` (store `0x4C1F1C`), `VK_HOME` through `0x4C1EC2` to `0xF0`, and `VK_DELETE` through `0x4C1F5B` to `0xF3`. `VK_BACK` is below the switch's lower bound `0x13`; the fallback `0x4C20EA..0x4C21C7` queues letters/digits/punctuation but no backspace. Backspace therefore needs the separate `WM_CHAR(8)` path. The editor tables `0x4ABA00`/`0x4ABA28` map 8 to `0x4AB7C4`, which removes the character left of the caret, and `0xF1` to `0x4AB7B0`, which moves the caret to text length. |
| `0x4B6220`, `0x51FBD0` | Returns the engine globals pointer stored at `0x51FBD0`. The character ring's capacity is globals `+0xF2`, storage `+0xF6`, write index `+0x16E` and read index `+0x172`. |
| `0x4C1A40`, `0x4C1A60`, `0x4C1AB0`, `0x4C1B00`, `0x4C1B20` | Ring reset zeroes both indices; capacity setter caps at 30 and resets; pop advances read and returns zero when empty; peek does not advance; push drops when the next write index equals read. `0x4A82FF` drains through pop during the GUI stage updater's bit-0 arm. A queued character is not evidence of a field edit. |
| `0x4A9FD0`, `0x4A9B90` | GUI pump peeks or pops one character according to GUI object `+0x18` (`0x4AA019..0x4AA039`), calls the key dispatcher, then takes the type-3 arm at `0x4AA2A6 → 0x4A7290`. Key dispatch uses GUI object `+0x20` as focus and GUIInfo `+0x64` to recognize an active edit; `0x4A9C80..0x4A9CA0` skips the shortcut branch for an active type-3 gadget. `0x4A9BF5` also calls the mapped SHIFT sampler `0x4C1B80(0xF9)`. `0x499890` also peeks for special in-game keys before the GUI pump. |
| `0x49FC50(gi,index)` | `ret 8`. Ends a previous type-3 edit by setting `gi+0x64` to −1 without clearing its text, then sets GUIInfo `+0x64` to the new index, and, for type 3, sets caret `gi+0x74` to inline text length (`0x49FCC5..0x49FCD5`). GUI object `+0x20` and this edit index are distinct fields. |
| `0x4A0BF0`, `0x4A09C0` | Named setter calls indexed setter at `0x4A0C58`. The type-3 arm copies a NUL-terminated string into gadget `+0xB6`, updates the caret if this gadget is edited, and sets `+0x138` only when the fourth argument is nonzero (`0x4A0A61..0x4A0AD4`). The field text is inline, not a pointer to `0x51E788`. |
| `0x4A7290`, `0x4AB6C0`, `0x4AB720` | Textfield click sets focus and edit index, initializes the inline buffer/caret through `0x4AB6C0` (ret 20; preserves text within its character limit when flag is zero, otherwise clears it, and calls the painter), then resets the character ring (`0x4A7434..0x4A7439`, also the second mouse arm). An edited field runs `0x4AB720`; Return completes it and Esc clears it. The latter routine edits `+0xB6`, drains queued characters, and invokes the painter only after handling input. Printable range is `0x20..0x7F`; attrib bit 1 restricts it to alphanumeric, `_`, space or apostrophe (`0x4AB942..0x4AB972`). TALK's measured attrib 1 has that restriction clear. |
| `0x4A4440`, `0x4A716A` | Button/shortcut handlers can consume characters through the same pop. They do not establish a textfield edit; focus, active edit index and accepted inline text remain the required observations. |

`tagpu_shield.c` delivers `WM_TAGPU_CHAR` as `WM_CHAR` directly to the game;
tagged keydown separately synthesizes a character through `ToAscii` because it
bypasses the engine's `TranslateMessage`. The input-file parser splits on every
byte <= `0x20`, so a literal `char:` followed by control byte 8 is truncated and
never posts that character. `tacli ui fill` clicks first and waits for exact
inline text prefixes in batches of at most four characters. The snapshot also
publishes inline `gi+0x64/+0x74` as `edit_index`/`caret`, copied by the owning
game thread at the flip; they are data, never gadget-array addressing inputs.
The CLI keeps an already active editor, or waits for the chosen edit index
before posting text. The panel's `focus` string is only the default-focus name
at `ctrls+0xEC`. Callers reject any result that differs from requested text.

- `DrawPopupF4Dialog 0x4948E0`: `DrawText 0x4A50E0` ×5, `0x4BF4D0` ×3 (the frame), `0x4C7580`
  ×1, `0x4B7F30`. `DrawPopupButtomDialog 0x4689C0`: `DrawText` ×3, `0x4B7F90` ×1, `0x4C6AE0`.
  `DrawChatText 0x464060`: `DrawText` ×1, `0x467C00` ×1 (a message-box painter that also calls
  `0x4C6890`, `0x4C69A0` and the flip itself), `SetFont`/`SetTextColors`. **Where it draws**
  (DISASSEMBLED): each line through `DrawText 0x4A50E0`, the first line's top `0x34`
  (`[esp+0x14]`, `0x4640D6`), each next one down by the font's height (`0x4C1450`, `0x46423E`);
  nothing when `main+0x37F27` (the lines shown) is 0 (`0x464077`). The text's x is `0x8A`
  (`ebx`, `0x464186`) only for a line whose kind byte is `0x0A` (`0x464194` jumps straight to the
  `DrawText` call). Any other kind first draws an icon box through `0x467C00` at x `0x8A`, the
  line's top, `s = ftol(0.8 · h)` square, with `h` the font's height again (`0x464199`) and 0.8 the
  double at `0x4FD520`; its text then goes at `ftol(138 + 1.5 · s)`: `s · −1.5` (`0x4FD528`)
  subtracted from 138.0 (`0x4FD530`, `fsubr`), `ftol` `0x4E43A0`, and `ebx` rewritten at
  `0x4641E6`. The
  lines are a ring of 30 records of `0x48` bytes (`edi` wraps at `0x1E`, `0x46424C`), the text at
  `main + 0x12EF + 0x48·i`, a kind byte at `+0x1335` and flags at `+0x1336`, from
  `main+0x2A3E`; the gate at `0x4640DE` reads `main+0x37EFE` (1, 2 or 3) against the flags' low
  nibble [INFERRED: which kinds of message the player's setting shows]. In engine pixels, so under
  HUD scale they move with the world region. Three callers: `0x469FCB` (DrawGameScreen, return
  address `0x469FD0`), `0x41F7A5` and `0x41F9D8` (both outside DrawGameScreen; return addresses
  `0x41F7AA` and `0x41F9DD`). `0x41F9D8` is in `0x41F7F0`, which loads `MSGBOX.GUI` (`push 0x502BD4` at `0x41F97C`); `0x41F7A5` is in `0x41F760`, gated on `main+0x39057 == 1`, whose address appears nowhere in the exe, so how it is reached is not known [INFERRED: an address computed at run time, or not at all]. Neither is measured under the strip; the gate stores `0x34` for both (DISASSEMBLED: `0x41F765 cmp dword [eax+0x39057],1`,
  `jne 0x41F7E0`; no call, jump or stored dword names `0x41F760`; `0x41F7F0` is reached by
  `jmp` at `0x499880`). The frame at `0x4640D6`: `sub esp,0x20` and four pushes, `0x30`
  bytes above the return address; the push of `0x4640B1` is the argument of `0x4C1420`, whose
  `ret 4` pops it, so the return address is `[esp+0x30]` there (DISASSEMBLED). **Patched at
  `0x4640D6..0x4640DD`**,
  the 8 bytes `C7 44 24 14 34 00 00 00` (`mov dword [esp+0x14],0x34`; `eax` = main from
  `0x4640C0`, read again at `0x4640DE`). The loop's back edge is `0x46425E jne 0x4640DE`, past the
  8 bytes, and nothing lands inside them (DISASSEMBLED: no direct branch or call in the exe
  targets `0x4640D6..0x4640DD`, and the function's switch table at `0x464270` holds only
  `0x46410F` and `0x464142`).
  `chat_top_init` (`tagpu_replay.c`, from `tagpu_replay_init`) checks the 8 bytes with
  `tagpu_detour_bytes_ok` and that `0x469FCB` is still `E8` to `0x464060` (`call_is`); on a
  mismatch it logs `replay: DrawChatText's first line 0x4640D6 or its call 0x469FCB holds other
  bytes: the messages stay under the live HUD's strip` and writes nothing, and if no stub memory
  is had or the landing fails it logs that and frees the stub. Otherwise it lands a 5-byte `jmp`
  and 3 `nop`s (`tagpu_detour_land`) to the stub: `50` push eax; `B8 34 00 00 00` mov eax,0x34;
  `81 7C 24 34 D0 9F 46 00` cmp dword [esp+0x34],0x469FD0 (the return address past the push);
  `75 05` jne store; `A1 <addr>` mov eax,[`s_chatTop`]; store: `89 44 24 18` mov [esp+0x18],eax
  (the frame's `+0x14` past the push); `58` pop eax; `E9` jmp `0x4640DE`. So only DrawGameScreen's
  call reads `s_chatTop`; the other two store the engine's `0x34`. The `cmp` changes the flags,
  and `0x4640E4 dec ecx` sets them again before any read (`je` at `0x4640E5` reads ZF only, and
  no carry reader comes before a `cmp`). `s_chatTop` (`tagpu_hudinput.c`) is written on the same
  game thread at the hit map's take (`tagpu_hudinput_take`, in DrawGameScreen's hook before the
  draw), from a map whose HUD pass ran only, and at the level's end: `0x34`, or, when that map
  has the live HUD's strip (the last frame drawn had it), `32 + live_clear · scale · gh / 720`, so
  the lines start under the strip (hud-in-game.md, *The top layer*).
- **`0x467D70` the HUD panel painter**, `void (void)`, one caller `0x49842A`, prologue
  `A1 E8 1D 51 00` (5): `esi = *(main+0x37E1B)`; `0x4C69A0(esi)`; `0x4C6890(esi, 0)`; three
  side-specific GAF frames (`main + side·4 + 0x1481F / 0x14833 / 0x14847`, side from
  `[[main+0x1B8A + 0x14B·[main+0x2A43]]+0x95]`, the **viewed** record's PlayerInfo
  (`0x467D98..0x467DB3`: the byte at `+0x2A43`, scaled by 330 and added once more)) via `0x4B7F30(seq, 0)` → **`0x4B7F90(esi, f, HotX+0x81,
  HotY)`** (top bar), `(HotX+0x81, HotY+screenH−0x20)` (bottom bar), `(HotX, HotY)` (side
  panel); then `0x4C63A0()`.
- The `LIGHTBAR` slide `0x45FFB0`: `0x4B8D40`, `0x4B7F30`, `0x4B7F90`, `0x47F1A0`.
- **Three blits, no loop** [BINARY-VERIFIED 2026-09-11]. `0x467DC8`, `0x467DFD` and `0x467E2B`
  each fetch one frame and each blit it once; there is no tiling loop in the function. How the
  bars nevertheless cover a 3840-wide screen is measured in
  [resolution](../resolution.html) §3.4a — left-anchored, repeating, nothing anchored right.
- **The in-game HUD's geometry is a family of absolute immediates, not one constant.** Anything
  that wants to move the panel's edge has to reckon with all of them: `0x4981C9`'s `0x80`
  (panel) and `0x20` (bars); this function's `+0x81` for both bars and
  `screenH − 0x20` for the bottom one; and, in `DrawGameScreen`'s tail, x `0x81` (the `+bps`
  lines), `0x82` (the `+clock` line), `0x83`, `0xBC`, `0x1EE` (the debug line). The list is not
  known to be complete, which is why [GUI renderer](../gui-renderer.html) §22 magnifies a region
  rather than re-laying the HUD out.

### The resource bar tiles in a loop, and the readouts are not screen-relative [BINARY-VERIFIED 2026-09-11]

[resolution](../resolution.html) §3.4a measured the top and bottom bars to be **left-anchored,
repeating, with nothing anchored right**, and recorded that `0x467D70` has no tiling loop —
three straight-line blits and no more. The loop is in `DrawGameScreen`'s resource block
instead: at **`0x469093`** the running x (`edi`, advanced by each frame's own width read at
`0x469090`, `mov dx,[ebx]`) is compared against `[ecx+0x37E1F]` — the **screen width** — and
`0x46909D` jumps back to `0x469055` while it is short. That is the measurement's mechanism,
found in the binary rather than inferred from pixels.

The readouts themselves are **not** placed from the screen width: `DrawTextCustomFont
0x4C14F0` is called at `0x4691F3` and `0x469212` with x and y out of `[esi+0x62]`/`[esi+0x66]`
and `[esi+0xD2]`/`[esi+0xD6]`, and again at `0x46926B` and `0x4692C0`. So the metal and
energy blocks sit at layout-struct offsets near the left, which is what lets §20 magnify the
bar about its top-left without pushing them off the screen — confirmed live at `s = 2.25` and
`s = 4.5`.

### The frame's HUD extras — what gates each, what it draws, where [VERIFIED 2026-09-07, Phase E G15c]

Read for G15c so the strict walk could *reach* every writer of the in-game frame that the
screen inventory does not open by itself. `DrawGameScreen`'s tail, `0x469F40..0x46A3E0`, in
call order, with each gate (`ebx` is the function's `drawUnits` argument):

| site | gate | draws |
|---|---|---|
| `0x469F65` `DrawPopupF4Dialog 0x4948E0` | `0x435100([main+0x391E9]) == 2` [INFERRED: a mode query; 2 while the F4 popup is up] | the Kills/Losses box top-right of the viewport; the draw context is re-seeded from `main+0x37E27` after it (`0x4C6B10` at `0x469F95`) |
| `0x469F9F` `DrawPopupButtomDialog 0x4689C0` | called every frame; inside, throttled to one pass per 15 ms by **`GetTickCount`** (`0x4B6560` is `jmp [0x4FC0DC]`, the KERNEL32 import; next time in `0x51E544`), then **`0x4C1B80(0x20)`** — TA's key-id poll: id `0x20` → `GetAsyncKeyState(VK_SPACE)` (index table `0x4C1C6C`, jump table `0x4C1C48`, the case at `0x4C1C2D`; the same switch resolves the SHIFT hotkey `0xF9`) | **the same Kills/Losses box as F4, held while SPACE is down** — MEASURED: 21 382 non-key pixels in the viewport with F4, 21 382 with SPACE held over the commander, 0 with it released. "Buttom" in the corpus symbol is the space *button*, not the bottom of the screen |
| `0x469FB8` `0x468380` | `[main+0x391C3] != 0` — the **`+bps`** cheat's flag (handler `0x419540`: `xor [main+0x391C3],1`) | `"Receive - %1.1f K/s"` / `"Send - %1.1f K/s"` (`0x5076A4`, `0x5076B8`), two `DrawTextCustomFont` lines at x `0x81`, y `[main+0x37E23] − 0x5F` and `+8` (the prologue). **Not `+netstats`**: its handler `0x417570` only zeroes the counters `0x511A60..0x511C64` |
| `0x469FCB` `DrawChatText 0x464060` | `ebx` | the chat lines |
| `0x469FD5..0x46A102` | `[main+0x3923B] & 2` and `ebx` | the debug line: `"FRATE: %d"`, `"%.1f"`, `"MODE %s INFO %s"` with `NORMAL`/`DEBUG` and `[Release]` (`0x507828..0x50781C`, `0x5077FC`, `0x50780C..0x507814`, `0x502558/0x50255C`) at x `0x83`, `0xBC`, `0x1EE`, plus a fourth string from `0x415FA0` under `[main+0x2A44] & 1`. **Negative result**: bit 1 of `main+0x3923B` is flipped only at `0x49631B`, behind `main+0x37F2F` bit 1 (`0x4962F8..0x496303`), the engine's debug mode (set at `0x417009`, `0x430E68`); it is cleared at game entry (`0x49128E`) and no cheat in the NORMAL table sets it — not reachable in play, not exercised |
| `0x46A107..0x46A1CB` | `0x46A102` loads `eax` = main; `test byte [eax+0x38A51],1` at `0x46A107`, `je 0x46A140` at `0x46A10E`. Set: `0x46A110..0x46A13E` draws the GAF `main+0x1481B` (`igpaused`) frame 0 at (`[esp+0xAC]`, `[esp+0xB0]`) through `GAF_SequenceIndex2Frame 0x4B7F30` and `CopyGafToContext 0x4B7F90`, reloads `eax` and jumps to `0x46A14E`; clear: `0x46A140` loads the same `esi`/`edi` and falls into `0x46A14E`. **From `0x46A14E`, paused or not**, unless the local player's PlayerInfo (`[main + 0x14B·[main+0x2A42] + 0x1B8A]`) has `+0x9B & 0x40` (a watcher), `main+0x3923B` bit 5 → `main+0x14813`, bit 6 → `main+0x14817` | the pause icon `PAUSED`, in the middle of the world (MEASURED 2026-10-09 on the Vulkan lane: a paused replay drew it there, over the replay HUD's stats window); then the won and lost status icons [INFERRED from bits 5, 6 being the game end's won and lost]. DISASSEMBLED. The replay HUD's stub sits on the test at `0x46A107` (*The replay HUD's stats window and the pause icon*) |
| **`0x46A1D0..0x46A2A3`** | **`main+0x37F2F` bit 6 — the `+clock` cheat** (handler `0x417300`: the bit flipped, then `0x430F00`) | the sim tick `main+0x38A47` split into h:m:s (÷108 000, ÷1 800, ÷30 — 30 ticks a second), `"%s : %02d:%02d:%02d"` (`0x507794`) with `"Game Time"` (`0x504910`, through `0x4C5740` [INFERRED: the string table]); `DrawTextCustomFont 0x4C14F0` at x `0x82`, y `[gfx+0xD8]` (`0x4B6710` [INFERRED: the primary's height]) `− 0x22 −` the font height (`0x4C1450`): the viewport's bottom-left, just above the bottom bar. **The seconds tick between two shots** — the walk measures it with the in-game menu open, which pauses the sim and the clock with it **in single player** (the pause is a separate flagged action, not a property of the push -- see *The screen lifecycle* below; a network game cannot be paused unilaterally) |
| `0x46A2A8` | `[main+0x38A51] & 2` | the GAF `main+0x148CF` frame 0 at (`screenW − 0x10`, `screenH − 0x50`); not exercised |
| `0x46A2E7`, `0x46A303` | — | clip reset `0x4C69C0`, then the retained GUI blit `0x4AB170` (`46a303: call 0x4ab170`, `46a308: mov edx,[0x511de8]`, DISASSEMBLED). It paints the whole top bar, so the Impure Config sprocket is drawn after it, by an observer at `0x46A308` through `CopyGafToContext 0x4B7F90` (`tagpu_menu.c`, `draw_trigger`), and the live HUD's rewind button beside the sprocket is the Vulkan pass's top layer, drawn after the GUI composite (*Live rewind* in [the HUD in game](../hud-in-game.html)) |
| `0x46A308..0x46A3B8` | `[main+0x38DD5]` and `ebx` | the nine profiler bars `0x46B900` |
| `0x46A3C2` **`DrawOptionsTab 0x45FFB0`** (the symbol's name; the `LIGHTBAR` wipe) | `0x512FE4 != 0` | below |
| `0x46A3C7` `0x4C2870` | — | the cursor, back buffer only |
| `0x46A3CC` `test ebx,ebx` / `0x46A3CE je 0x46A3E0` | — | **guard 1 on the flip**: argument 1 (`drawUnits`, the `ebx` the marker block tests) |
| `0x46A3D0` `mov eax,[esp+0x22C]` / `0x46A3D7 test eax,eax` / `0x46A3D9 je 0x46A3E0` | — | **guard 2 on the flip**: argument 2 (`blitScreen`) |
| `0x46A3DB` `0x4C63A0` | `ebx` and `[esp+0x22C]` (`blitScreen`) | the flip, **conditional on both guards above** |
| `0x46A400` | — | a profiler accumulator; the only call between the flip and the `ret`, and it writes nothing to the primary |

**The two guards are why "the flip is inside `DrawGameScreen`" is not on its own an argument that
a frame has been presented** *[BINARY-VERIFIED 2026-09-20, re-derived for the golden-source
landing]*. A `DrawGameScreen` that draws without presenting falls through both `je`s straight to
the tail. What makes the flip certain on the played path is the **return-address filter**: only
`0x4969CD` (`push ebx; push ebx` at `0x4969CB`, `ebx` set to 1 at `0x4967CF` and not rewritten in
between — `DrawGameScreen(1, 1)`) returns to `VA_DRAW_RET_INPLAY 0x4969D2`, and both guards are
satisfied there. A future in-play call site passing `blitScreen = 0` would reach an `after`
observer with no flip having run. `tagpu_packet_pub.c`'s `after_draw` states this where it
captures the golden source.


**The `LIGHTBAR` wipe, `0x45FFB0`** — the in-game menu's opening animation, a bright bar sweeping
the panel's width while the panel is revealed behind it: the opener **`0x460160`** (`ret 4`, the
panel's `GUIMEMSTRUCT*` at `[ebp+4]`) snapshots the panel's surface (`[panel+0xBC]`, `w×h` from
`+0x17/+0x19`) into `0x512FE8`, scales it with `0x4B8AE0` into `0x512EF8`, zeroes the slide
`0x512FEC`, stores the panel's `width − 1` in `0x512F14` and its `y` in `0x512F10`, and sets
**`0x512FE4 = 1`** (`0x460204`). Each call of `0x45FFB0` then advances `0x512FEC` by `0x15` up
to `0x115` (fourteen steps), draws the `LIGHTBAR` frame — `0x4B8D40([main+0x51D], "LIGHTBAR")`
then `0x4B7F30(seq, 2)` — with `0x4B7F90`, and reveals the panel to the left of the bar with
`0x4C7580` stamps (the textured-triangle stamp, at `0x46012A`); the closer **`0x4609B0`** clears
`0x512FE4` (`0x4609D4`). The opener's callers: `0x427459`, `0x427906`, `0x44444D`, `0x460ABD`
(inside the closer) and `0x4776D8`, each right after `0x47F1A0("Options"|"Menu", 0)` [INFERRED:
a screen lookup by name — it walks the table at `main+0x33E13`]. Every pixel the wipe writes goes
through two observed leaves (`0x4B7F90`, `0x4C7580`), which is why the census explains it and the
twin follows it; the engine free-runs its frame loop, so at 60 presented frames a second the
fourteen steps are over within a present or two.

**`0x4C7580` runs in the steady game frame too** — MEASURED (the CORE census, `CORMAIN2`, nothing
selected, no popup): `scale` 208–240 ops per 50 flips, about four a frame; its ten call sites
are `0x42136C`, `0x458664`, `0x46012A` (the wipe), `0x46675E`, `0x467CA1`, `0x46BC1E`
(`drawKillsAddr+0x918`, the F4 box), `0x494CBB` (`DrawPopupF4Dialog+0x3DB`), `0x4A21C8`
(`GUI_ListboxBuild+0x688`) and `0x4A4A81`/`0x4A4B1B` (`GUI_HotspotDraw+0x101/+0x19B`, corpus
names; the per-frame ones by the neighbourhood [INFERRED]). So "no `scale` op in a window" is
only evidence of the wipe's absence while the game is paused, which the menu does.

**What the census logged as unexplained on *other* surfaces, on the CORE run**: the startup
`OFFSCREEN` 640×480 (`03D30560`, the shell's own surface, tagged by whatever GUI is on top)
written whole at every in-game screen build — `changed=192015 unexplained=192015
box=(0,4)-(639,479) ops_on_it=0` — the item already open above; and a 128×352 surface
(`04E2B850`, the menu's rect) written whole when `PREFS` opens, its `SAVE UNDER` snapshot
[INFERRED]. Neither is presented; the presented surface's total was 0 unexplained of 1 717 044.

**`ARMOPT.GUI` is not over the viewport.** Its `[COMMON]` record is `xpos=0 ypos=128 width=128
height=352` — the side panel's own rect; the in-game menu *replaces* the build panel. Opening it
pauses the game **in single player** and draws `PAUSED` -- the GAF entry `igpaused`, not text -- in the middle of the world (the 2 621–2 938 non-key viewport
pixels the walk counts on that stop). The screens that do lie over the world are `PREFS.GUI` and
`VISUALRT.GUI` (~55 000 px each at 1024×768), the F4 / SPACE box (~21 000), the chat (~800), the
clock and the `+bps` lines.

**The NORMAL cheat table at `0x501D38`** (43 `{name, handler, runLevel}` entries, the one
`InitInternalCommand 0x4B7760` registers at `0x4195A7`), name → handler, read for the two the
walk types: NoShake `0x416E60`, Contour `0x416DB0`, ScrollSpeed `0x416CF0`, IFace `0x416D20`,
Give `0x416BD0`, CDPlay `0x4167F0`, CDStop `0x416810`, Sound3D `0x416820`, Shading `0x416420`,
AntiAlias `0x416510`, Shadow `0x416550`, Dither `0x416590`, SwitchAlt `0x4165C0`, TShadow
`0x416630`, FShadow `0x416660`, LOSType `0x416690`, Light `0x4166C0`, RCache `0x416710`,
Selectable `0x416460`, MusicMode `0x4175E0`, Logo `0x4168D0`, ScreenChat `0x417130`, Gamma
`0x417290`, **Clock `0x417300`**, NetStats `0x417570`, Sing `0x4172E0`, NoMetal `0x417150`,
NoEnergy `0x4171F0`, BigBrother `0x4174E0`, Now `0x416E90`, Drop `0x4177A0`, ShootAll
`0x418CA0`, ShareMetal `0x418CD0`, ShareEnergy `0x418D90`, ShareMapping `0x418E50`, ShareRadar
`0x418FD0`, ShareAll `0x419090`, ShowRanges `0x4194C0`, SetShareMetal `0x419340`,
SetShareEnergy `0x419400`, Compression `0x4194D0`, **BPS `0x419540`**, SFX `0x419550`. The
handlers are `stdcall(argv)`, `ret 4`; `+showranges`'s writes `main+0x391BF`, as the ta-drive
skill says.

### The minimap, located [VERIFIED]

`main+0x1426B` (`TED_GENERATED_PIC`) is consumed once, at `0x46684F` in
**`BuildMinimapSurface 0x466780`** (no args, prologue `83 EC 40 53 8B 1D E8 1D 51 00`, `ret` at
`0x4669A3`). **Its one caller is `0x4669B0`, the minimap set-up** *[INFERRED name]*: that function
is `call 0x466780` as its first instruction, then creates `+0x142DB` (tag `0x507518`) and
`+0x142DF` (tag `0x507508`), both `0x4C69F0(tag, [+0x142EB], [+0x142ED])`. **`0x4669B0`'s one
caller is `0x4919C3`.** [VERIFIED 2026-09-09 by an `E8`/`E9` scan of the whole `.text` for each
target; the loader's own free at `0x483DF3`/`0x483E0B` is in a function that calls neither, so the
only moment the picture is reachable from a hook of ours is inside `0x466780`.]

**`0x466780` runs on a thread of its own, NOT the game thread** [MEASURED 2026-09-09]: an observer
at its entry with the usual game-thread guard fired on every map load and refused, naming the
thread. Every other site the GL UI renderer observes is called from the game loop; this one is not.

**The fit, exactly** [VERIFIED 2026-09-09 by disassembly of `0x466780`..`0x466868`]: with
`w = [main+0x1422B]` and `h = [main+0x1422F]`, the long axis gets **126** and the short one
`short × 126 / long`, truncating, and the box is centred in a 126×126 area — `+0x142E7` is
`(126 − w') / 2` with `+0x142E9 = 0` on a wide map (`0x4667D1`..`0x466801`) and the mirror image on
a tall one (`0x4667A4`..`0x4667CF`). Measured on Two Continents: **106×126 at offset (10, 0)**.
Then `0x4C69F0(0x5074F8, w', h')` creates `+0x142E3` at the box size, `0x466845` builds a context
for it, and — **if `main+0x1426B` is non-NULL** — `0x46685F` hands the picture straight to the
stretch `0x4B95A0` and returns. So the whole **252×252** picture is squashed into the
aspect-correct box, and a renderer drawing it with full 0..1 UVs into that box reproduces the
engine's mapping rather than approximating it. If the picture IS NULL, `0x46686C` takes a wholly
different path: a `2w' × 2h'` surface (tag `0x5074E8`) built from the terrain instead.

**`+0x142F1` bit 1 is a DIRTY flag, not a visibility flag.** `DrawMinimap 0x466B00` tests it at
`0x466B11` and **clears it at `0x466B16`** in the same breath, so it reads 0 on almost every frame;
the engine can afford that because its copy lands in the game offscreen and stays there. Anything
drawing a minimap into a surface that is cleared per frame must redraw every frame and gate on
something else — `+0x142DB` being non-NULL is the honest test. [MEASURED 2026-09-09: a renderer
gated on the flag drew nothing at all, ever.] It
fits a 126-px box (`main+0x142EB/+0x142ED` size, `+0x142E7/+0x142E9` offsets), creates
`main+0x142E3 = 0x4C69F0(0x5074F8, w, h)` and scales the picture into it (`0x4B8AE0` + `0x4B95A0`
[INFERRED stretch]). Three surfaces: `+0x142E3` the scaled map; `+0x142DF` the fog composite,
rebuilt by `0x466C20` with **direct byte writes** (callers `0x465572`, `0x48191A`); `+0x142DB`
the composite plus radar dots, rebuilt by `0x466DC0` (`0x4C6B70([0x142DB],[0x142DF],0,0)` then
unit dots through `0x4B7F90` of `main+0x147DF/+0x147E3`, `0x4C0070` arcs, `0x4BEE60 DrawPoint`;
callers `0x465072`, `0x48191F`). **Of those three writers only `0x4B7F90` is an observed leaf**
[VERIFIED 2026-09-08]: `0x4C0070` and `0x4BEE60` are absent from the twin layer's table. What
they draw still reaches the twin, but only because the base copy at the head of `0x466DC0` reads
`+0x142DF`, which no observed leaf writes and which is therefore never seeded — so that copy
publishes as a pixel op carrying `+0x142DB`'s final bytes, arcs and points included. The
correctness depends on `+0x142DF` staying unseeded ([GL UI renderer](../gui-renderer.html) §7). **Per frame, `DrawMinimap 0x466B00(ctx)`** (`stdcall`, `ret 4`,
prologue `8B 0D E8 1D 51 00`, gated on `main+0x142F1 & 2`) does
`0x4C6B70(ctx, [main+0x142DB], main+0x142E7, main+0x142E9)` at `0x466B44` and the view box
`0x4BF8C0(ctx, main+0x142CB, colour)` at `0x466B5E`; **that colour is the BYTE at `main+0xDD9`,
zero-extended, not the address** [VERIFIED 2026-09-09, G17e — an earlier revision of this line
read as though the pointer were passed]: `xor ecx,ecx` at `0x466B4E`, `mov cl,BYTE PTR
[eax+0xdd9]` at **`0x466B50`**, `push ecx` at `0x466B5B`. So the box's colour is a single palette
index held at `main+0xDD9` — the same byte §"The order-marker chain" records as the range
labels' colour (gui `0xE`) — and anything replaying the box has to resolve that index through
the palette the screen is shown with rather than assume a fixed colour; **one caller, `0x46961F` in
DrawGameScreen, with the game offscreen's context.** The minimap never goes through the GUI
surfaces. The map loader builds `main+0x1426B` at `0x483900..0x483936` as a GAF frame
(`0x4B8DA0(0x508B6C, w, h)`, filled via `0x4B8A80` + `0x4B7F90`) and frees it at
`0x483DF3`/`0x483E0B`.

### What the census measured [MEASURED 2026-09-07, `tools/uiwalk.py`, 1024×768]

With every leaf above observed and every surface diffed at every flip (`CENSUS_MS 5`, so at
most 200 diffs a second against the shell's ~5 000 flips), across the inventory — `MAINMENU`,
`SINGLE`, `SKIRMISH`, `SELMAP`, `STARTOPT`, `VISUALS`, and in game `ARMMAIN2`, `ARMCOM1/2`,
`ARMOPT`, `PREFS`, `VISUALRT`, `TALK` (chat), the F4 popup — **3 710 035 pixels changed on the
presented surface and 0 were unexplained**; a screen transition in the shell is 614 400
changed pixels (the whole 640×480) explained by ~50 000–150 000 GAF blits and a few thousand
lines, rects and copies. The unexplained changes on *other* surfaces, all outside the frame:

- the PCX backgrounds decoded into their `"bitmaps\…PCX"` surfaces by the loader (~300 000
  bytes each, zero ops) — assets, read only as copy sources;
- the `"SAVEMOUSE"` buffers, written by `0x4CBBE0` directly from cursor code;
- **the startup `"OFFSCREEN" 640×480` keeps being written in game** by an unobserved path at
  in-game screen builds (185 942 bytes when `ARMCOM1` appears, 43 869–83 573 in rows 226–479
  on `ARMOPT`/`TALK`/`ARMCOM1` pages), and **`"FLIPSURFACE" 128×352`** is filled whole
  (43 519 bytes) when `PREFS` opens — neither is ever presented, and anything reaching the
  frame from them goes through an observed copy [OPEN: the writer of each].

The census also fixed three claims elsewhere in this wiki: the nine `0x46B900` calls in
DrawGameScreen's tail are the **debug profiler bars**, not "side panel / minimap"
([UI markers](../ui-markers.html) §4); the minimap is not `0x48CC30`/`0x46A430`
([frame composition](../frame-composition.html) §1); and `id 12` dispatches to `0x4A5E50`.

### What the twin layer excludes, tests and reads [VERIFIED 2026-09-07, Phase E G15b]

The layer (`tagpu_gui_surf.c`) replays the observed ops into GL twins; three facts it leans on
were read for it, none of them patched.

**Observed but not published — the leaf calls that are not UI, matched on the return address**
(`excluded_caller()` in `tagpu_gui_leaves.h`). The ranges are the callers' extents, read from
the disassembly (`ret`/`ret n` boundaries):

| range | what | its leaf calls (return address = call + 5) | ends |
|---|---|---|---|
| `0x459200..0x459800` | the unit composite blit (`0x459200`, [GPU status](../gpu-status.html) §2.4) — the engine still calls it while `owndraw` skips the rasterisers, and the composites it blits are all key | `0x4B8500` at `0x459319`, `0x459353`, `0x4593BA`, `0x4595E9`, `0x4597D3`; `0x4B7F90` at `0x4593A4`, `0x4597AB` | `ret` at `0x4597DF`; `0x45982A` is the next function |
| `0x4C2380..0x4C2A00` | the cursor code — `0x4C2380` (dead), `0x4C24B0`, `0x4C25E0`, `0x4C2870` and the `SAVEMOUSE` copies (§ "The mouse object") | `0x4B7F90` ×4 (`0x4C23C9`, `0x4C258C`, `0x4C2732`, `0x4C297E`), `0x4C6B70` ×2 (`0x4C24A8`, `0x4C2937`), `0x4CBBE0` ×9 (`0x4C241B`..`0x4C2835`) | `0x4C2870` ends at `0x4C2989`; the last function in the range at `0x4C2A74` |
| `0x4C6300..0x4C6890` | the flip `0x4C63A0` — **three exits**, each `pop edi/esi/ebp/ebx; add esp,0xF4; ret` at `0x4C6669`, `0x4C668F` and `0x4C67BA` (a backward `jne 0x4C66EB` at `0x4C67AA` keeps the last arm inside), so it spans `0x4C63A0..0x4C67BA` — and the in-flip cursor draw `0x4C67C0` (ends `0x4C6884`) | the flip: `0x4C6B70` at `0x4C6414`, `0x4C6585`, `0x4CBBE0` at `0x4C6553`, `0x4C65F3`, `0x4C6769`; `0x4C67C0`: `0x4C6B70` at `0x4C6862`, `0x4B7F90` at `0x4C687D` | — |

**`0x4C67C0` has exactly two callers, `0x4C641B` and `0x4C6544`, both inside the flip** — so
every blit it makes is also under the observer's `s_inFlip` (set between the flip's entry and
its return), which is the guard that actually excludes them; the range only has to be honest
about where the function ends. Until 2026-09-07 the code's bound was `0x4C6800`, short of
those two calls; harmless for that reason, corrected anyway.

**Which flip is a game frame**: `DrawGameScreen`'s `call 0x4C63A0` is at `0x46A3DB`, so the
flip observer reads its own return address and compares it with **`0x46A3E0`**; a match means
the frame is the game's (the viewport is the terrain skip's key fill, subtracted by the census,
cleared by the layer), anything else is the shell's, whose flip runs from the flip's other
callers, the message-box loop's `0x49FA32` among them.

**Read on the render thread, never written** (both already in this map; listed because the
layer is a new reader on the other thread):

- `main+0x143A7`, the live RGB palette (`256 × {R,G,B,pad}`, 1024 bytes) — compared with the
  last copy at every present and re-uploaded as a `256×1 RGBA8` texture when it moved, so the
  index twin resolves through the palette the engine is presenting with.
- the mouse object `*(0x51FBD0)`: `+0x1B2` → the current sprite record (`u16 w, u16 h` at
  `+0`/`+2`), `+0x1B6`/`+0x1BA` the position it was last drawn at. That rect is the one place
  the layer does not draw and `strict` does not count: the cursor is the engine's in phase 1
  ([GL UI renderer](../gui-renderer.html) §3.7). A torn read here costs one frame of a
  misplaced exemption, nothing else.

**Read on the game thread at the blit, guarded** (`IsBadReadPtr`, like the first-sight decode):
the whole of a GAF frame's pixel plane — every byte of a raw one, every `[len][data]` row of an
RLE one — goes into the sprite's identity beside the header and plane addresses, because the
shell frees a popped screen's art and the heap hands the same addresses to the next screen's
(the 2026-09-07 review). `0x4CCF60`'s `'\n'` stop (`cmp al,0xA; je 0x4CD008` at
`0x4CCFA0`) is honoured by the glyph observer's width since the same review.

### The screen lifecycle: loading, pushing, keeping on top [VERIFIED 2026-09-09, Phase F G18]

*Read with `i686-w64-mingw32-objdump -d -M intel` on `pristine/TotalA.exe.pristine`. This
section exists because [renderers](../renderers.html) §2.10 needed to know whether a **new**
`.GUI` name can be pushed at all. It can.*

**`GUI_Load 0x4AA8F0(GUIInfo* gi, const char* name, int flags)` — `__stdcall`, `ret 0xC`,
returns the new `GUIMEMSTRUCT*` in `eax`.** Arguments proven from the frame: after
`sub esp,0x21c` and four pushes, `[esp+0x230]`, `[esp+0x234]` and `[esp+0x238]` are args 1-3;
arg 1 is dereferenced at `+0x18` (`TheActive_GUIMEM`) at `0x4AA917`, and arg 2 is `strlen`ed
(`repnz scas`, `0x4AA9B4`) and concatenated.

- **The name becomes a FILE PATH, so the exe's screen-name string table is never consulted.**
  `0x4AA99A` copies a 0x100-byte prefix from **`gi+0x9B6`**; the name is appended; `0x4AA9FB`
  pushes **`0x502828` = `"GUI"`** into the extension-setter `0x4BAFF0`; `0x4AAA10` calls
  `0x4BBC40` to open it. Setting the extension is idempotent, which is why the engine's own
  callers pass a name that already ends `.GUI`. **A name we invent is loaded if the file
  exists** — our DLL is the call site, and no stock screen has to be sacrificed. *This closes
  the question both this page's §6 and roadmap G18e recorded as open.*
- **`flags` bit `0x800` is the MODAL DIM** (`0x4AA912..0x4AA984`, re-read 2026-09-26): before
  anything else, the screen about to be covered is darkened by the box shader at level −24
  (row 8 of PALETTE.SHD). With a screen on top (`gi+0x18`) the box is its panel record's rect —
  `+0x17`/`+0x19` for the size, and `+0x13`/`+0x15` for the origin unless the record's first
  byte is 0, when the origin is `(0,0)` — shaded on the panel's own surface `[ControlsAry+0xBC]`
  (`0x4AA969`), and the top screen's `+0x14` is set to 1 so `0x4AB0B0` blits it again. With no
  screen on top it is `0x4BF4D0(NULL, NULL, −24)` — the whole back buffer (`0x4AA984`). So what
  a player sees behind SELMAP is SKIRMISH, dimmed by the dialog that covers it.
- **`flags` bit `0x200` does not suppress the push so much as replace it: the file is
  MERGED into the screen already on top** [VERIFIED 2026-09-11, and LIVE — see below].
  `0x4AAA2F` branches on it and takes `edi = gi->TheActive_GUIMEM` instead of allocating:
  `ebp` is set to `ControlsAry + (count+1)*0x15B`, i.e. **the tail of the existing gadget
  array**, and the parse writes there. `0x4AABE6` then does `add WORD PTR [ctrls+0xB6],ax` —
  the loaded file's gadget count is *added* to the existing one — a `rep movs` at `0x4AAC12`
  shifts the loaded records down one stride to overwrite the loaded file's own panel record,
  and `0x4AAC54` skips the push. `0x4AAC98` still stamps the **loaded** file's name over
  `ControlsAry[0].name`, so the merged screen answers `IsOnTop` under the *second* file's
  name.
  **Consequence, and it is not cosmetic:** a screen built this way is ONE `GUIMEMSTRUCT`
  whose array holds both files' gadgets, so nothing downstream — `GUI_FindGadgetByName`, the
  pump's hit loop, an `OnCommand` — can tell which file a gadget came from. The
  visual-options screen is exactly this: `0x45E5E0` loads `STARTOPT.GUI` with `0x80` (pushing)
  and then `VISUALS.GUI` (or `VISUALRT.GUI` in game) with `0x200`, so what `tacli ui` reports
  as `VISUALS.GUI` is STARTOPT's array with the video controls appended — its tabs, its
  OK/Cancel/Restore/Undo and the video toggles listed together in one snapshot (LIVE
  2026-09-11). The `STARTOPT.GUI` `tacli` shows *under* it is the earlier, un-merged one.
- **THE PUSH, `0x4AAC56`** — the writer of `per_active` that was previously unmapped:
  ```asm
  4aac43:  mov [edi+0x04],ebp        ; new->ControlsAry = the gadget array
  4aac4b:  mov [edi+0x1c],0          ; and +0x24, +0x3b cleared
  4aac54:  jne 0x4aac5e              ; skip the push if flags & 0x200
  4aac56:  mov edx,[esi+0x18]        ; edx = gi->TheActive_GUIMEM   (the old top)
  4aac59:  mov [edi],edx             ; new->per_active = old top    (the LIFO link)
  4aac5b:  mov [esi+0x18],edi        ; gi->TheActive_GUIMEM = new   (the push)
  ```

**`main+0x37EA0` is the name of the screen the engine KEEPS on top in game**, and it is a
plain string buffer. `0x497BA4` fills it: `sprintf(main+0x37EA0, "%sMAIN2.GUI", side)`, the
side being the 562-byte record at `main+0x37F5B` indexed by the player's side byte — so in a
game it reads `ARMMAIN2.GUI` or `CORMAIN2.GUI`.

**`GUICONTROL_IsOnTop 0x4AB060(gi, name)` is strict — it does NOT walk the stack.** It reads
`gi->TheActive_GUIMEM` (`+0x18`), then `->ControlsAry` (`+0x04`), adds 2 for the panel's
`name`, and `strncmp`s 16 bytes through `0x4FAB50`. Only the **topmost** screen can match.

**`UpdateIngameGUI 0x491D70` pops until that is true** — `0x491DCA` tests, `0x491DE0` calls
`GUI_Pop 0x4A9660`, `0x491DF2` loops. **21 call sites.** So anything pushed over the world is
popped again unless `main+0x37EA0` names it.

**`GUI_Pop 0x4A9660` itself has 39 call sites, not 21** — the 21 above is `UpdateIngameGUI
0x491D70`'s, and a downstream note borrowed it for the wrong function until 2026-09-16. `call
0x4a9660` appears 39 times in the pristine exe; `call 0x491d70` 21. **The ORDER at `0x460630` is
what matters to anything that gates on a per-level generation**, and it is the opposite of the
intuitive one: `0x460630 call 0x491B60` (the level teardown, which is where a generation moves),
then `0x460635 push 1; 0x460637 call 0x491D70` (which DRAWS — `0x491D70` reaches the GAF blit leaf
`0x4B7F90` through `0x4A9660` → `0x4C2470`), then `0x460641 add eax,0x519; 0x460647 call 0x4A9660`
(the pop, which FREES that screen's art). So art is drawn and then freed **after** the teardown has
already ended the level: anything stamped with the level generation at `0x460637` carries the NEW
one and a generation test cannot refuse it. [ESTABLISHED 2026-09-16, G19f landing 5's third review
pass; see [gpu-status](../gpu-status.html) §2.34. Note also `0x4AA7BC..0x4AA7FA`, below, which is
`GUI_Pop`'s body inlined instruction for instruction — so an observer on `0x4A9660` does not see
every pop either.]

**Two of the 21 are the engine's own visible closes, and both pass 1**: `0x460635`, a `push 1`
immediately after `call 0x491B60` (the level teardown), and `0x4929E3`, whose `call 0x491D70`
is at `0x4929EB`. `tagpu_menu.c`'s close copies them — restore `main+0x37EA0`, then **call**
`0x491D70(1)` rather than wait for one — because all 21 sites are event handlers: left to
itself a popped screen lingers until the player next does something that changes the GUI stack.

**`0x495207` is the engine's own template for pushing an in-game screen**, and it is the one
to copy:

```asm
495207:  lea ecx,[eax+0x37ea0]           ; the expected-screen buffer...
49520d:  add eax,0x519                   ; gi
495212:  push ecx                        ; ...passed AS the name
495213:  push eax
495214:  call 0x4aa8f0                   ; -> eax = the new GUIMEMSTRUCT
495219:  mov [eax+0x8],0x494890          ; new->OnCommand   (the caller sets it)
495226:  mov [eax+0xc],edx               ; new->+0x0C = main   (its context)
```

i.e. **write the buffer, load that same string, then set `+0x08` and `+0x0C`.** Closing is the
mirror: restore the buffer and let `UpdateIngameGUI` pop, without calling `GUI_Pop` at all.
The flags there are **`0x20`**.

**`GUI_Load` STAMPS the name into `ControlsAry[0].name`** [VERIFIED 2026-09-09, G18 gate 1]:
`0x4AAC98` is `strncpy(ctrls + 2, name, 16)` through `0x4E4760`, on the copy of the *name
argument* taken at `0x4AA9E2`. That is the field `GUICONTROL_IsOnTop` compares, so **the
`name=` authored in a `.GUI` file is irrelevant** — which is why `armmain2.gui` says
`name=HEADER;` on disk and `tacli ui` reads `ARMMAIN2.GUI` live, and why a screen we invent
only has to be loaded with the same string we leave in `main+0x37EA0`. (`PREFS.GUI`,
`ARMOPT.GUI` and `MAINMENU.GUI` author their own file name and so agree by accident;
`ARMMAIN2`, `CORMAIN2`, `ARMCOM1` and `VISUALRT` all say `HEADER`.)

**`flags & 0x400` suppresses GUI_Load's own STAGE 1, not merely a repaint** [MEASURED
2026-09-09]. `0x4AACB5` tests `bh,0x4` and skips `0x4AACBA..0x4AACCD`, which is
`0x4C2470(); GUI_StageUpdateDraw(gi, flags | 1); 0x4C2870()` — a counted lock pair around a
**stage 1** draw, and stage 1 is what builds the panel's own surface. Suppressing it to patch
a panel's rect first and then asking for a bare `0x40` repaint leaves the panel with **no
surface**, and the engine composites the frame's own pixels at its rect — the game drawn a
second time from the panel's x across. The fix is to reproduce the call, not replace it.
Stage 1 also *reads* the rect: `0x4A820F` and `0x4A8221` show `xpos == -1` and `-2` are
**centre-me sentinels**, written when `flags & 0x100` / `0x1000` accompany stage 1, so a rect
we want honoured must be written before stage 1 and not after.

**The path prefixes are runtime state, and there is no such literal in the exe.**
`0x49FBA0(gi, dir)` sets `gi+0x9B6` and `0x49FBF0(gi, dir)` sets `gi+0xAB6`, each
`strncpy(dst, dir, 0x100)` then `strcat(dst, "\\")` (`0x503374`). `UIPipelinesInit` calls them
once at `0x4914CE` and `0x4914E5` with `0x502820 = "guis"` and `0x502E30 = "anims"`, so the
two prefixes are `guis\` and `anims\`. [VERIFIED 2026-09-09]

**A mod renames the gui folder, so "guis" is retail's value, not the engine's.** The folder is
the `.data` string at `0x502820` (file offset `0x100E20`), pushed by `0x4914C2`
(`68 20 28 50 00`) for the setter call at `0x4914CE`. TA:ESC's exe carries `guiE` there, byte
for byte in place (the anims string at `0x502E30` is unchanged); Total Mayhem's and ProTA's exes
carry `guis`, and their archives ship `guiM\` and `guiP\` — Mayhem's Patch Loader (`dplayx.dll`)
lists the change as a patch (`;Change guis reference to guiM`), so there the string is rewritten
at run time, after the exe is mapped. Each mod ships its own `VISUALS.GUI` and `VISUALRT.GUI` in
that folder. `0x502820` has six references: `0x41AD2C`, `0x42A6FE`, `0x42A752`, `0x42D82A`,
`0x42D888`, `0x4914C2`. [DISASSEMBLED 2026-09-27; the four exes byte-compared at those offsets]

**`gi+0x9B6` has two readers besides its clear (`0x4AA861`) and its setter (`0x49FBAB`):** `GUI_Load`'s
`0x4AA989` (`add edi,0x9b6`, then the `strncpy` at `0x4AA99A`) and `0x4A8381`, a per-gadget
loop in the stage drawer `0x4A81E0` — which GUI_Load itself calls at `0x4AACC4` unless `flags &
0x400` — that copies it into its own buffer and appends a gadget's own file name to it
(`0x4A8FA0`, the gadget record's `+0xB6`). **A missing file is
a fault, never a NULL return:** when the open at `0x4AAA10` returns 0, `0x4AAA17` jumps to
`0x4AAC2D`, which takes `edi` from a local only the merge path wrote (`[esp+0x10]`,
`0x4AAA3B`), and `0x4AAC43` writes `[edi+4]` — the in-game menu's crash on TA:ESC before our
screens were redirected (the crash report's stack held `guiE\RENDER.GUI`). [DISASSEMBLED
2026-09-27]

**Our redirect, `0x4AA99F`** (`mov edx,[esp+0x240]`, 7 bytes, an observer with no return
hijack): just after the prefix `strncpy`, `[esp]` is still its destination — the local path
buffer, `esp+0x38` there (lea'd as `esp+0x30` after one push at `0x4AA994`; `0x4AA9B0`
addresses it as `esp+0x2C` after the `add esp,0xC`) — and `[esp+0x240]` is the caller's name.
`tagpu_menu.c`'s `gui_redirect` rewrites that copy to `impure\` for `RENDER.GUI`, and for
`VISUALS.GUI` / `VISUALRT.GUI` while their handlers are in, so our screens load from a folder
only our archive has, whatever the gui folder is called. `gi+0x9B6` itself is never written.
MEASURED 2026-09-27 on TA:ESC under Wine: the front end's Visuals screen, the in-game one and
the sprocket menu all ours and drawn complete; without the redirect the front end showed
Escalation's own screen and the sprocket menu faulted.

**`UpdateIngameGUI` is NOT called per frame** [MEASURED 2026-09-09]. All 21 call sites are
transition and teardown handlers — `0x460630` calls the level teardown `0x491B60` first, and
the `0x499xxx` cluster is the in-game command path — so an observer installed there is entered
on GUI *events* only. A poll hung off it never runs while the game merely runs.
**`DrawGameScreen 0x468CF0`** (`tools/ta_symbols.txt`; 4 call sites, all in `0x495C76..0x4969CD`)
is the per-frame game-thread function, and its entry is before any of the frame's drawing.

**Pausing is NOT a property of the push** [CORRECTS this page's two unqualified claims that
the in-game menu pauses]. `ARMOPT.GUI` is `xpos=0 ypos=128 width=128 height=352` — the same
side-panel rect as a build page, pushed the same way — yet the build page leaves the sim
running (LIVE 2026-09-09: selecting a commander gives `ARMCOM1.GUI`, `under: ARMMAIN2.GUI`,
with the game running). The pause is a separate action on a flag; the `PAUSED` indicator is
not text but the GAF entry **`igpaused`** (`0x5035BC`), looked up once at `0x429B66` through
`0x4B8D40` into `main+0x1481B` beside two siblings at `main+0x14813`/`+0x14817`, and drawn by
DrawGameScreen at `0x46A107..0x46A139` while `[main+0x38A51] & 1` (*The frame's HUD extras*); a
replay with the replay HUD skips it (*The replay HUD's stats window and the pause icon*).
**The pause is single-player only** — a network game cannot be paused
unilaterally — so the earlier statements need that qualifier.

### The pump's dispatch contract: `gi->UIChange_f` is the ANSWER, and an unanswered click POPS THE SCREEN [VERIFIED 2026-09-11]

*Found the expensive way. The render-options work read `UIChange_f` as an inbound argument
only, and its front-end screen was destroyed on every click with `GUI_Pop 0x4A9660` — armed
with an observer — never firing once.*

**`0x4AA78D`, in the GUI pump `0x4A9FD0`, is the only site that calls a screen's `OnCommand`
for a mouse click**, and what it does afterwards is the whole protocol:

```asm
4aa675:  mov  [ebp+0x60],eax       ; gi->UIChange_f = the actuated gadget index
...
4aa78d:  mov  edx,[ebp+0x18]       ; gi->TheActive_GUIMEM
4aa790:  mov  eax,[edx+0x08]       ; ->OnCommand
4aa795:  je   0x4aa79a
4aa797:  push ebp
4aa798:  call eax                  ; the screen's handler
4aa79a:  cmp  [ebp+0x60],edi       ; edi == -1. Still set?
4aa79d:  je   0x4aa7fa             ;   -1 -> handled; return
4aa7ac:  mov  [ebp+0x60],edi       ;   else: UIChange_f = -1 ...
4aa7b9:  call eax                  ;   ... call OnCommand AGAIN, with -1 ...
4aa7bc:  call 0x4c2470             ;   ... and then POP AND FREE the screen:
4aa7c1:  push 2 / push ebp / call 0x4a81e0
4aa7c9:  call 0x4c2870
4aa7ce:  mov  ecx,[ebp+0x18]
4aa7d1:  mov  eax,[ecx]            ;   top->per_active
4aa7d5:  mov  [ebp+0x18],eax       ;   gi->TheActive_GUIMEM = it
4aa7da:  mov  [eax+0x14],1
4aa7e1:  push ecx / call 0x4d85a0  ;   free(top)
4aa7ea:  test esi,0x800 -> 0x4a81e0(gi, 0x40)
```

**`0x4AA7BC..0x4AA7FA` is `GUI_Pop 0x4A9660`'s body inlined, instruction for instruction** —
the same `0x4C2470`/`0x4C2870` lock pair, the same stage-2 draw, the same relink, the same
`0x4D85A0` free, the same `flags & 0x800` repaint (compare `0x4A9689..0x4A96C7`). So **a
screen can leave the stack without `GUI_Pop` being entered**, and an observer on `0x4A9660`
is blind to it. `gui-gadgets.md` §1.3 now carries this too.

**Clearing `UIChange_f` is how a handler says "I consumed this", and `0x4AB0A0(gi)` is the
engine's own one-line setter for it** (`mov [eax+0x60],-1; ret 4`). The visual-options
handler `0x45E100` is the worked example, and it uses both answers deliberately:

| branch | ends with | because |
|---|---|---|
| SHADING / ANTI / BSHADOWS (`0x45E2EA`, `0x45E276`) | `0x49FA90(gi)` then **`0x4AB0A0(gi)`** (`0x45E2F0`, `0x45E27C`) | the toggle was handled; the screen stays |
| a gadget it does not recognise that is **not** a button (`0x45E48C`) | **`0x4AB0A0(gi)`** | consumed and ignored |
| UNDO / RESTORE (`0x45E318`, `0x45E425`) | its own `GUI_Pop` + rebuild | `GUI_Pop` sets `-1` at `0x4A9673`, so the pump is already answered |
| a **button** it does not recognise — the tab column (`0x45E46A` → `0x45E499`) | `GUI_Pop`, restore the index, `call [below->OnCommand]` | "the player hit STARTOPT's own gadget": pop and forward |
| OK while `selvmode.gui` is on top (`0x45E457`) | nothing | *deliberately* leaves it set so the pump pops `selvmode` |

**And `-1` on the way in means this screen is being destroyed.** Both callers that pass it —
`GUI_Pop` (`0x4A9673`) and the pump's inlined pop (`0x4AA7AC`) — free the `GUIMEMSTRUCT`
within a dozen instructions of the call returning. `0x45E100`'s `-1` branch is accordingly the
screen's destructor: it frees the display-mode list hanging off `GUIMEMSTRUCT+0x0C`
(`0x45E11B`: `[list+0x14]`, `[list+0x04]`, then the block), nulls the field, and clears
`main+0x37EBE` bit 0 — the bit `0x45E5E0` reads to choose `VISUALS.GUI` over `VISUALRT.GUI`.
A chained handler that declines to forward that call leaks all three.

**`0x45E5E0(selvmode)` is the visual-options dialog build**, and its shape is worth recording
because two of its calls are easy to misread:

| | |
|---|---|
| `selvmode != 0` | `GUI_Load(gi, "SELVMODE.GUI", 0x800)` and nothing else |
| `selvmode == 0` | `0x45CFC0` → `GUI_Load(gi, in game ? "PREFS.GUI" : "STARTOPT.GUI", 0x80)`, **which pushes**, and sets `main+0x37EBE` bit 0 on the in-game side (`0x45D002`); then `GUI_StageUpdateDraw(gi, 2)`; then `0x45CE80`, which *appends a synthesized gadget* to the pushed screen's array in game (`ctrls+0xB6` incremented at `0x45CECC`); then `GUI_Load(gi, "VISUALRT.GUI" or "VISUALS.GUI", **0x200**)` — the merge |
| both | `0x49FA50(gi)`, then `[ebx+0x08] = 0x45E100` at `0x45E68B` — the OnCommand goes on the **pushed** screen, which is the one the merge landed in |

The display-mode list is built after that: `0x45E6B0` allocates the 0x20-byte header into
`GUIMEMSTRUCT+0x0C`, `0x45E6BD` a 0x4B0-byte table, `0x45E6EC` a string block of
`count << 8`, and `0x45E726` hangs it off the `VIDSLDR` gadget (`+0x13C` = count-1,
`+0x144` = the per-gadget callback `0x45BBF0`, `+0x14A` = the list). In game the whole block
is skipped (`0x45E69E` branches on `main+0x37EBE` bit 0), which is why `GUIMEMSTRUCT+0x0C` is
NULL there and the destructor's frees are guarded by `test edi,edi` at `0x45E11B`.

**The header's own layout** [MEASURED 2026-09-11, by reading a live screen]. Worth writing
down because it is how the list a player is actually being offered can be read without
walking the slider a stop at a time:

| off | what |
|---|---|
| `+0x00` | **count** — the number of surviving modes, after `0x45E4C0`'s sort-and-drop |
| `+0x04` | the **table**: `count` records of 12 bytes, `{ u32 width, u32 height, u32 refresh }` |
| `+0x08`, `+0x0C` | two small blocks inside the header's own allocation |
| `+0x14` | the **string block**, `count << 8` bytes — the `"1024 X 768"` captions, 256 apart |

`+0x04` and `+0x14` are the two the destructor frees at `0x45E11B` before the block itself,
which is the independent check that they are the two allocations. Read it from a live game
as `main+0x531` → `+0x0C` → `+0x00`/`+0x04`.

**The rebuild idiom** is four instructions at the tail of the UNDO branch, and it is what a
caller outside the engine uses to make this screen re-enumerate:

```
45e318:  call 0x45cae0          ; UNDO only: put the engine's own option bits back
45e31d:  push esi               ; gi
45e31e:  call 0x4a9660          ; GUI_Pop(gi)   -- stdcall, ret 4; sets gi->UIChange_f = -1
45e323:  push 0x0
45e325:  call 0x45e5e0          ; the dialog build again, with a fresh mode list
```

`0x45CAE0` belongs to UNDO alone (it XORs `main+0x37F06`'s bits back from the saved copy at
`0x512F38`); the pop-and-rebuild pair is the general part.

### The in-game quit, YESORNO, a button's frame [DISASSEMBLED + MEASURED 2026-10-01, 2026-10-09]

*Read for S5's watching dock (2026-10-01) and for the replay HUD's Leave and its modal test
([The replay HUD in game](../hud-in-game.html), *Leave*, *The menu over the world*; 2026-10-09),
with `objdump -d -M intel` on `pristine/TotalA.exe.pristine`. The runs were on a private Xvfb of
the reference setup, at 1280×720 and 1920×1080 for the dock and 1920×1080 on a 3840×2160 display
for the HUD, Vulkan presenting. The dock itself is deleted; the facts below are the engine's.*

**The engine's dialogs push themselves, and none writes the expected-screen buffer.** EXITMENU
(`0x4608B0`, flags `0x1800`), ARMOPT (`0x460CC0`, `0x800`) and YESORNO (`0x460680`, `0x1000`) call
`GUI_Load` directly; none of them writes `main+0x37EA0`, so with the buffer naming a screen of ours
they sit above it and are popped by their own handlers, as they are over ARMMAIN2 in stock.
MEASURED: a click outside ARMOPT closes it (the stock behaviour), and its close clears the pause it
set. MEASURED 2026-10-09: with ARMOPT open, a device click on the replay HUD's bookmark button —
outside ARMOPT, over the world — reached the engine (the HUD's modal test refused it; no bookmark
was written) and the engine closed ARMOPT.

**The in-game quit is a tail, not a function.** `0x460630..0x46065C` sits inside YESORNO's
`OnCommand` `0x4605C0`: the teardown `0x491B60`, `UpdateIngameGUI(1)` `0x460637`, `GUI_Pop` of
`main+0x519` `0x460647`, `0x4257A0`, `SetInputMode(1)` `0x490B30` (which stores the mode and the
state function and touches no screen), and `ret 4` at `0x46065A`, the handler's own. The teardown's
call list (`0x491B76..0x491C4A`) never reads or writes `main+0x37EA0`. MEASURED (RENDER open, then
Tab, Exit, Main Menu, Yes): the shell's stack is MAINMENU alone with or without the buffer restored
before the teardown, so the stock pops after `0x491B60` do not leave ARMMAIN2 behind either way --
`0x4257A0` and the main menu's build take what is left -- but without the restore the buffer
reaches the shell naming `RENDER.GUI` (`tagpu_menu_level_end` restores it, and the shell then reads
`ARMMAIN2.GUI`).

**YESORNO's opener `0x460680`, void, `ret` at `0x4607F1`.** Two callers: `0x46083A` (EXITMENU's
Main Menu, after `mov [0x512FF8],0` at `0x46082B` and `GUI_Pop` at `0x460835`) and `0x460862`.
It loads `YESORNO.GUI` (`0x503168`) with `0x1000`, makes it take keys (`0x49FB10`, `0x4606B7`),
labels `CHOICE1` and `CHOICE2` (`0x460742`, `0x460760`, through the setter `0x4A0BF0(gi, name,
text, 0)`) and sets `TITLE` by the question, the dword `0x512FF8`: 0 is `0x506D20` "Surrender this
battle and return to main menu?" (`0x46076A..0x460784`), 2 is `0x506D10` or `0x506CE4` by
`main+0x2BEE` bit 4 (`0x460786..0x4607B8`), 1 leaves the file's. Its `OnCommand` `0x4605C0` (stored at `0x4607D2`): `-1` returns (`0x4605CF`);
`CHOICE1` with `[0x512FF8] <= 1` jumps to the quit tail; `== 2` sets bit 2 of `main+0x3923B` and
calls `0x491C60`. **`GUI_Load` does not answer a click**: it writes `gi+0x64 = -1` (`0x4AACAE`), not
`gi+0x60`, so an `OnCommand` that pushes a screen and returns without `ActDone 0x4AB0A0` has the
pump pop the NEW top (`0x4AA79A..0x4AA7FA`, below). EXITMENU's handler is answered by its own
`GUI_Pop` (`0x4A9673` writes `-1`) before it calls the opener.

**Opening YESORNO outside the pump.** The replay HUD's Leave calls the opener from the
DrawGameScreen observer (`tagpu_menu.c`, `menu_tick` → `leave_open`), the engine's Exit → Main
Menu sequence (`0x46082B..0x46083A`) without its `GUI_Pop 0x4A9660` at `0x460835`, there being no
EXITMENU to pop: `[0x512FF8] = 0`, `call 0x460680`, then the title. That call is outside the
pump's dispatch, so no click is waiting on an answer and nothing calls `ActDone`; the pump's pop of
the new top above happens only to a screen pushed inside an `OnCommand`. MEASURED 2026-10-09: the
question opens titled "Leave this replay?", No pops it and the playback goes on, Yes runs the
quit tail to the shell. Opened this way bit 0 of `main+0x37EBE` is clear (no ARMOPT under it):
MEASURED 2026-10-09, Tab opens nothing over it, and a device click on the world closes it, as
one closes ARMOPT. Before every call Impure compares `0x460680..0x4607F1` and
`0x4605C0..0x46065C` by FNV-1a against `0x8B2B971E` and `0x9EF8B480`, recomputed from
`pristine/TotalA.exe.pristine`, and refuses Leave on a mismatch.

**`0x4A07D0(gi, name, text)`, stdcall, `ret 0xC`: a label set by name on the TOP screen**
[DISASSEMBLED 2026-10-09, `0x4A07D0..0x4A0871`]. It reads the top `GUIMEMSTRUCT*` at `gi+0x18`
(`0x4A07D7`; `main+0x531` for `gi = main+0x519`) and returns at once when it is NULL; walks
records 1 to the count `+0xB6` of `[top+4]`, `0x15B` bytes apart, comparing each record's name at
`+2` with `name` over 16 bytes (`0x4E4B50`, `0x4A0805`); on a match copies `text`, at most `0x80`
bytes (`0x4E4760`, `0x4A0850`), into that record's `+0xB6`, sets `gi+0xCCA = 1` (`0x4A085E`) and
calls `0x4A05E0(gi, index)` (`0x4A0868`). It never searches below the top screen, so it is called
right after the opener has pushed YESORNO. One direct call in the image: `0x4265C9`. Impure calls
it with `"TITLE"` to retitle Leave's question.

**A screen's name, and a walk of the stack by name.** A screen's name is its record 0's,
`[gm+4]+2`, 16 bytes: the field `GUICONTROL_IsOnTop 0x4AB060` compares, and `0x4A07D0` reads at
`+2` for every record. `0x4AB060` tests the top screen only; to ask whether a screen is anywhere on
the stack, Impure walks from `main+0x531` down each screen's `+0` link, at most 32 steps (the
bound `tagpu_menu.c`'s `on_stack` uses), comparing that name (`named_on_stack`).

**A button's frame, `0x4A6062..0x4A616D`.** Not greyed: a stage button (`+0x136` nonzero) draws its
stage `+0x137`, or `count - 2` while pressed (`+0x138` nonzero, `0x4A6110..0x4A613B`); a plain one
frame `+0x138` (0, or the pressed frame) plus `+0x13B`. Greyed (`+0x13C` bit 0): attribs bit 8
(`+0x1C` bit 0) draws `count - 1` (`0x4A607E..0x4A608B`); else **a stage button draws its stage's
frame again** (`0x4A6092..0x4A60B3`); else attribs bits 11-12 draw frame `+0x13B`; else
`min(+0x138 + 2, count - 1) + +0x13B` (`0x4A60D9..0x4A610E`). So a stage button's own greyed frame
is never drawn unless attribs bit 8 is set. The stage arm also sets the local `[esp+0x40] = 1` at
`0x4A60AB`, which the caption's `DrawText 0x4A50E0` takes as its shade argument (`0x4A643F`); the
bit-8 arm jumps past it, so **a greyed button under bit 8 draws its caption unshaded**. **No hover
frame**: MEASURED, a plain button's pixels with the pointer over it differ from without only by the
cursor's. MEASURED 2026-10-01 on the dock: the plain greyed arm comes out tinted under GDI and
untinted under the Vulkan mirror (353 px over three 28×22 buttons), where the bit-8 arm is the same
on both (0 px); the source of the GDI tint was not found.

**Attribs bit 8 from a file does not survive on a stage button.** `GUI_Load` reads `[COMMON]`
through `0x4AEAC0` (called at `0x4AAA9E`), whose reader `0x4AD350` stores `attribs` as the dword at
`+0x1B` (`0x4AD407`; the TDF integer reader `0x4C46C0`, `atol 0x4E4ED0`). Stage 1, having split a
stage button's caption at `|`, rewrites its attribs to `(attribs & 0x4000) | 1`
(`0x4A8EE1..0x4A8EEC`); MEASURED, `attribs=257` in the file reads back as 1 once the screen is up.
Stage 1 also zeroes `colorf` (`+0x1F`) at `0x4A8B56`. The other readers of bit 8 are the list box
painters `0x4A1F79` and `0x4A221D` and the list box arm `0x4A13C8` of the by-index setter `0x4A12E0`
(jump table `0x4A142C`, index table `0x4A1444`). MEASURED: with bit 8 set at run time on a stage
button that is not greyed, its clicks stop answering (two buttons, both stages), while a plain
button with it answers; the reader responsible was not found -- the pump's click arm
`0x4AA35A..0x4AA39D` reads no attribs, and the press arm's attribs test (`0x4A9D80`) is bit 4.

**The panel's ground and its surface.** Stage 1 makes the panel's surface record 0's `+0x17 x +0x19`
(`0x4C69F0`, from `0x4A9071..0x4A907C`), stores it at `+0xBC`, and copies the screen under it from
`(-x, -y)` (`0x4A9081..0x4A9098`, through `0x4C6B70`); the copy `0x4CBBE0` refuses an origin outside
either surface and clamps the size to both (`0x4CBBE0..0x4CBC90`). An `id 12` picture resolves its
frame by NAME in the panel's bank, then the shared one, into `+0xB8` (`0x4A8ACA`), and is drawn by
`0x4A4660`; a panel's background is `0x4B0230` (one frame blitted at (0,0), several nine-sliced,
`0x4B039C..0x4B0439`). The GAF blitter's single-frame path `0x4B7F90` intersects the frame's rect
with the context's clip (`0x4C6AE0`, `0x4B7E60`) and returns on an empty one; an uncompressed frame
(`+9 == 0`) goes to `0x4CBE70` with the key `+8` (`0x4B8021..0x4B8108`; the clip at
`0x4B807B..0x4B80C6`). So a frame wider than its panel is drawn clipped to the panel's surface.

**The speeds.** `main+0x38A4B` (u16) is the game speed the player chose, the ceiling, 10 = 1x;
`main+0x38A4D` (u16) the live speed the scheduler `0x495230` runs at. A frame's ticks are the
elapsed time × the live speed × 0.1 (`0x4FDA28`) plus the fraction kept at `+0x38A43`; `DeltaTime`
`+0x38A3B` is 0 while bit 0 of `+0x38A51` is set (`0x4953D9..0x4953E2`) and at most 5
(`0x4953EF..0x4953FE`). The throttle counter `+0x38A4F` (u16): each clamped frame adds 1 and past 10
lowers the live speed by 1, floor 1 (`0x495409..0x495441`); each unclamped frame takes 1 and past
-100 raises the live speed by 1 up to the ceiling (`0x495448..0x49547B`). **Nothing clamps a speed
from above** there; `SetGameSpeed 0x490DF0` clamps to [1, 20], and every speed key is refused for a
watcher (`0x496550`, `0x4965AA`). Writers of `+0x38A4B`/`+0x38A4D`: the Options screen and its undo
(`0x45C143`, `0x45C6F3`, `0x45CA6D`, `0x45CD67`, `0x45EBE4`, `0x45ECA8`, `0x45FEA6`),
`0x490DC0`, `0x490E9B`, the level setups `0x4912C9` and `0x49199F` (a reset to 10 in network mode
3 only, so a skirmish keeps its speed into the next level), the start-up load `0x430509..0x43052B`
(the registry's `gamespeed` into both, unclamped) and the scheduler. The settings save `0x430F00` writes the
ceiling to `gamespeed` (`0x43136B`); the Options screen's OK reaches it at `0x45FD5F`. MEASURED on a
playback, GameTime a second over 5 s with the ceiling and the live speed written together: 5 ->
15.1, 10 -> 30.5, 20 -> 59.1, 40 -> 118.9, 80 -> **150.5**, the live speed still 80 after the 5 s:
the clamp, not the speed, bounds 8x, at five ticks per step of the 30 Hz clock
([*The scheduler, the clock and the test pacing*](sim.md#the-scheduler-the-clock-and-the-test-pacing-disassembled-measured-2026-10-08)).

**Testing at higher speed.** Retail `SetGameSpeed` forms its displayed offset as
`speed-10` at `0x490E21` and selects the positive sign at `0x490E57..0x490E67`;
internal 20 therefore displays `+10`. A patched displayed `+20` would request
internal 30, or 3x the normal tick rate, rather than 20x. The retail setter and
plus key still clamp internal speed at 20 (`0x490DF9`, `0x4965BA`). The scheduler
changes the number of due iterations, not the step size: `0x495490` reads the
bounded due-tick count at `0x495497`, increments GameTime by one at
`0x4954BD..0x4954C0`, pumps the network at `0x4954C8` when its argument is nonzero,
and calls the normal unit tick at `0x4954ED` for each iteration. Tick-based replay
and state tests can therefore run faster without coarsening the simulation step.
That does not establish equal AI choices or packet arrival ticks across speeds.
Bandwidth, deadlines and ordinary network timing require real elapsed-time
measurements and normal-speed runs; an accelerated run changes the traffic load
per wall-clock second. These facts are reconfirmed by retail disassembly, not a
new accelerated multiplayer measurement.

**The round-trip batch bound [DISASSEMBLED + MEASURED 2026-10-05].**
The ordinary skirmish frame tests front-end bit 0 at `0x496918`
(`main+0x37EBE`) and pause bit 0 at `0x496920` (`main+0x38A51`). When both
are clear it calls the scheduler `0x495230` at `0x496928`, loads main from
`0x511DE8` at `0x49692D` (`A1 E8 1D 51 00`), loads and tests the due count
`main+0x38A3B` at `0x496932..0x496938`, and branches to `0x496969` at zero
(`74 2D` at `0x49693A`). Otherwise `push 0` at `0x49693C` precedes the
step call `0x495490` at `0x49693E`. That callee caches the count in EBX
(`0x495497`, `0x4954AD`) and decrements the cached count at `0x49566C`;
changing the main word inside the unit tick cannot stop that batch. Its
once-a-call tail, including `0x428BD0/0x428BE0/0x428BF0` at
`0x495674/0x495679/0x49567E`, remains outside the loop (their meanings are
not established here). After the step, the frame times it through `0x4B6560`
at `0x49694F`, accumulating at `main+0x38D85 +0x4C`, then tests the same
front-end bit at `0x496969`, polls input `0x495E90` at `0x496971` and scroll
`0x41CE90` at `0x496976`, and calls `0x48BAE0` at `0x49697B`.

The adjacent network tail was read to distinguish these paths: after the
step's timing accumulator it calls `0x428C00/0x428C10/0x428C20` at
`0x496831/0x496836/0x49683B` and tests `main+0x38D75` bit 2 at
`0x496846..0x496851`. When set, `0x4568C0` at `0x496853` may clear that
bit at `0x496868..0x49686E`, and is timed through `0x4B6560` at `0x496881`
into the `+0x2C` accumulator. The following calls
`0x428C30/0x428C40/0x428C50` at `0x496896/0x49689B/0x4968A0` precede
the jump to `0x496964`. The paused network branch at `0x4968AA` tests
the same pause bit, optionally calls `0x4618A0` at `0x4968C6` with
`this=0x513000`, argument 0 and nonzero `[0x506DBC]`, then pumps `0x453D40`
at `0x4968CB`. A `0x4B6340` clock compared with `[0x51F300]` at
`0x4968D0..0x4968DB` gates the next `clock+60` deadline, the local-player
lookup `0x44FDB0` at `0x4968EC`, and `0x453320` at `0x4968F2` with that
lookup's result. This branch is timed at `0x496903` into `+0x2C` and
jumps to `0x496962`. No hook or new write is added in these network
branches; the six `0x428Cxx` functions' meanings are not established here.

`rt_pace_init` observes the five bytes at `0x49692D`, matching both calls
and the ten bytes of the count load/test/zero branch as well. Its before-only
observer preserves registers and flags, replays the main load and resumes at
`0x496932`. On a playing puppet with armed round trips, outside rewind, the
game thread validates the due count within 0..5 and reduces it by the smallest
positive distance to an upcoming T−1. The step therefore cannot pass that
frame boundary. This discards pacing budget for that frame, never an iteration
or simulation state. The arm table is bounded to 24 entries, with T ≥ 2;
the same game thread arms, bounds and consumes it. No lock or timing interval
is the argument. Targets already behind the reader, including after an
explicit seek, still report not run. Failure to install refuses round trips.
The pacing word was already `leave` in `replay-globals.tsv`; no exclusion
or comparison rule changed.

MEASURED on the reference setup: the first internal-speed-20
`secondary-orders` run missed the frame at GameTime 390 and refused the
round trip at 391. With the bound installed, the full live/play/round-trip/
fresh-seek fixture passes all 23 checks: frame dumps at GameTimes 390, 480
and 540 match their in-place rebuilds exactly, all 26 poisoned tables have
zero residue, and the continuations at 691, 781 and 841 match the recording.
Playback heartbeats report 58.51 and 57.46 ticks/s at live speed 20.
The same source at normal speed passes world/radar/event/count/address,
puppet-write and all three round-trip checks, with playback heartbeats around
30 ticks/s. Its images inventory fails one byte at the existing `main+0x531`
GUI-pointer gap, which `replay-globals.tsv` now leaves with the viewer (`guitop`).
This verifies accelerated tick-based replay checks; it does not measure
multiplayer traffic or establish a total harness-time reduction.

On the coverage build, `wpn-badtgt` passes all 33 checks, including accelerated
live/play/round-trip/fresh-seek checks and the W lane starting at normal speed:
the dock changes 10 to 20, the Options save retains the player's stored 10,
and Leave restores the ceiling to 10. Its three round trips poison 27 tables
with zero residue and exact A/B frame dumps. The coverage files reach
`rt_pace_init`, `rt_pace_guard`, `rt_arm` and `rt_due`. The accelerated
`takeover battle` passes all ten reported checks, including unchanged original
history, exact fork playback and AI activity over [1679,3479): seats 1/2/3
issue 1743/1987/1740 fresh orders, with 187/195/190 reaching normal dispatch,
and 91% of 1001 mobile units move. These are functional speed regressions,
not a clean result for the open GUI-pointer inventory or a throughput estimate.

**The pause** is bit 0 of the word `main+0x38A51`: the Pause key XORs it (`0x4960AA..0x4960BC`) and
then sends packet `0x19` through `0x451DF0`, with no watcher test. The level setups do not clear it
(`0x49842A`, above, is ours).

**A commander death and a defeat in a player record.** The destructor increments the owner's
`WORD +0x106` when the dying unit's def name equals its side's commander (`main+0x37F5F +
0x232·side`, compared by `0x4F8A70`; `0x486990..0x486996`, and the self-destruct path
`0x486A86..0x486A8C`), and decrements the owner's `WORD +0x144`, the units alive
(`0x486DFC..0x486E09`), calling the skirmish elimination `0x47BD70` at 0 in mode 2. Both are in
every recorded player image. MEASURED: a skirmish whose player's commander self-destructs with
"commander dies: game ends" gives one `+0x106` rise and one `+0x144` fall to 0 for seat 0, both at
GameTime 2779.

### The pump's entry, the pointer it reads, and what a list scroll is [DISASSEMBLED 2026-09-26]

*Read for `tagpu_gui_hook.c`'s list wheel, which scrolls a list from an observer at the pump's
entry, on the game thread ([GUI gadgets](../gui-gadgets.html) §2.4.1).*

- **`0x4A9FD0(GUIInfo* gi)`** — stdcall `ret 4`, prologue `83 EC 34 53 55` (5 bytes; the list
  wheel's observer steals exactly these). Seven `E8` callers: `0x41F7B7`, `0x41F9EA`,
  `0x41FFBE`, `0x42056D`, `0x499992`, `0x49FA1B` (the message-box loop `0x49F9C0`) and
  `0x4AC035`. Its first read is `gi->TheActive_GUIMEM` (`mov eax,[ebp+0x18]` at `0x4A9FDB`);
  with no screen it returns 0.
- **At the entry, the pointer `gi+0x3C` is the previous pump's.** It is refreshed at most once a
  pump, at `0x4AA00A`, by `0x4AB5D0(gi)` (`ret 4`, that one caller). When the engine's event ring
  holds a record (`0x4C2DE0`, a peek) and a screen is up, it tests the record against the top
  screen (`0x4A1680` fetches the panel's rect, right and bottom inclusive; `0x4A1920` tests the
  point against it [DISASSEMBLED 2026-10-09]) and pops it
  through `0x4C2D60` into a local only when its point is inside or its third dword — the
  message's wParam, the buttons held — is 0 (`0x4AB643..0x4AB64D`); it then copies six dwords of
  it to `gi+0x3C` (`0x4AB667..0x4AB66C`) and its third to `gi+0x54`. Otherwise it returns at
  `0x4AB680` with the record still at the head and `gi+0x3C` unchanged; IdleTick then pops it
  at `0x4999C4`, and only in the message-box loop `0x49F9C0`, where the pump is the only pop,
  does every click behind it wait (*Who pushes the two rings*). With the ring empty it fills
  `gi+0x3C` from `0x4C2340` instead. So an observer at the entry that needs the point a message
  was aimed at has to carry the point itself.
- **The engine's event ring, and how far it can move between two pump entries.** It lives in
  the graphics globals (`*(0x51FBD0)`): capacity `+0x186`, base `+0x18A`, write index `+0x18E`,
  read index `+0x192`, each index in `[0, cap)`. `0x4C2BD0(20)` sets it up once, at `0x4B5A67`.
  The one push, `0x4C2E30`, is called from TA's window procedure at `0x4B5F51` for the button
  messages only — down and up of both buttons and their double-clicks, `0x201..0x206` through
  the table at `0x4B60F4`, a double-click with the record's flag set (`0x4B5F40`) — and drops
  the record when the ring is full. A move (`0x200`) is never queued: `0x4C2360` copies it,
  six dwords, into the current record at `+0x196`, which `0x4C2340` copies back out for the
  pump when the ring is empty. The pop `0x4C2D60`
  has two callers: `0x4AB654`, inside `0x4AB5D0` (the pump's one refresh), and `0x4999C4`, which
  follows the in-game pump call at `0x499992` in straight-line code (`0x4999B5` is the only
  branch in that span, and it skips the pop when the pump took the head: *Who pushes the two
  rings*); `0x4C2DE0` is a peek and moves nothing. So between two entries of the pump the read
  index moves at most two slots. The one other writer of the
  indices is the flush `0x4C2BB0` (both to 0), called once, at `0x426636`, in the in-game setup
  `0x4263B0` (the frontend state machine's call at `0x4270A9`), which discards whatever was
  queued. The list wheel's ordering rests on these facts ([GUI gadgets](../gui-gadgets.html) §2.4.1).
- **A gadget's rect is inclusive**: the pump's panel test (`0x4AA0F9..0x4AA128`) compares
  against `x+w−1` and `y+h−1`.
- **The engine's own recipe for a scroll is `List_SelectPrev 0x4A9830`'s tail.** It decrements
  `+0xBA` (selected), and when that leaves the view decrements `+0xBC` (top) at `0x4A9947`; then
  it calls the listbox handler `0x4A1B40(gi, idx)` at `0x4A9983`, which repaints the rows, and
  `Gadget_PropagateAssoc 0x4A2BE0(gi, idx)` at `0x4A998A`, which moves the bound slider's knob to
  match (`0x4A2CA8`). Both `ret 8`; `0x4A1B40` has seven `E8` callers and `0x4A2BE0` fifteen.
- **`0x4A2BE0` from a list is a selection's consequence, not a scroll's.** For each other record
  with the source's `assoc` it takes the arm of that record's type. A list (`0x4A2D56..0x4A2D78`)
  gets the source's `top` and selection and its own `0x4A1B40(gi, j)`. A slider
  (`0x4A2C7A..0x4A2CE7`), when the list has more than one row (`0x4A2C83`), gets `knobpos =
  top · range / maxtop` (0 when `maxtop` is 0), computed on the x87 and truncated by
  `_ftol 0x4E43A0` (its control word ORs in `0xC00`, chop), written and repainted through the
  slider painter `0x4A2580(gi, j)` (`ret 8`) only when it changed (`0x4A2CD0`). A textfield
  (`0x4A2CEC..0x4A2D43`), when the list's attribs has bit 3, gets the SELECTED row's text and
  `0x4A4D70`. The engine calls it from a list only after writing the selection
  (`List_SelectPrev`, the click at `0x4A3E8A`); its scrolls that leave the selection alone
  propagate from the slider (`0x4A7037`, `0x4A42DA`) and never reach the textfield arm. So a
  scroll made from outside is `+0xBC`, `0x4A1B40`, and the list and slider arms made one by one
  — the list wheel's `lw_follow`.
- **A slider finds its list by `assoc`, first match, active or not.** `GUI_SliderUpdate
  0x4A3EF0(gi, idx)` scans from record 1 to `totalgadgets` (`ControlsAry+0xB6`) for the first
  `id == 2` record whose `assoc` equals the slider's (`0x4A3F37..0x4A3F4A`), tests no active
  flag, and returns 0 on a miss. The arrows' scan for their slider (`0x4A6FA4`) is the same
  shape for `id == 4`, and `GUI_FindGadgetByName 0x49FE60` loops to the same count.
- **The engine has no wheel of its own.** Its window procedure dispatches `0x200..0x206` only
  (the table at `0x4B5E3B`, the bound at `0x4B5E41`), so `WM_MOUSEWHEEL` falls to
  `DefWindowProcA` ([gpu-status](../gpu-status.html) §2.3). Both wheels — the zoom's and the
  list's — live in our window procedure.

### SELMAP's map preview — `0x444A20` [DISASSEMBLED + MEASURED 2026-09-26]

*Read because on the Vulkan lane the preview stopped changing after the first pick.*

**`SELMAP.GUI` is loaded from two places**, both with the `0x800` modal dim: the skirmish path
(`GUI_Load(gi, "SELMAP.GUI", 0x980)`, the push at `0x444F8C`, the call `0x444F92`) and the
multiplayer battle room (`0x880`, `0x47AB3C`/`0x47AB42`). **Both reach the same preview builder,
`0x444A20`** (no arguments, plain `ret`), which has eight `E8` callers — `0x444C11`, `0x444C9D`,
`0x4450D6`, `0x448B6D`, `0x448E9A`, `0x44ABE1`, and the battle room's `0x47AADE` and `0x47AC73`.
On each pick it:

1. fills the `MAPNAME`, `SIZE` and `Players` text (`0x4A0BF0`);
2. finds the `MAPPIC` gadget (`0x4A0280(ControlsAry, "MAPPIC")`, `0x444AE2`), **frees its frame**
   `+0xC2` (`0x4D85A0` at `0x444AF4`) and zeroes the field;
3. **allocates the next** through `0x4295B0(path, &w, &h)` (`0x444B24`, `ret 0xC`), which opens
   the map file (`0x4BB5B0`), reads the minimap's header and makes the frame with
   `0x4B8DA0("RADARPIC", w, h)` (`0x42960D`); the result goes back into `MAPPIC+0xC2`;
4. calls **`0x4665D0(frame, gadget w, gadget h, w << 4, h << 4)`** (`0x444B4E`, its one caller,
   `ret 0x14`), which copies the picture into a temporary frame (`0x4666A4`, `0x4666C0`), rewrites
   the frame's own width and height to the gadget's (`0x4666DB`, `0x4666DE`), **fills it with
   index 0** (`0x4C6890` at `0x4666EF`), stamps the temporary back into it scaled and letterboxed
   through `GAF_DrawTransformed 0x4C7580` (`0x46675E`), and frees the temporary (`0x466764`).
   Both are destinations of observed draws — `0x4B7F90` at `0x4666C0` into the temporary,
   `0x4C6890` into the frame, each through a context `0x4B8A80` builds — so both are surfaces
   to the GUI lane, and both die by `MEM_Free` inside the pick
   ([`MEM_Free 0x4D85A0`](memory.md#0x4d85a0-mem_free-the-one-way-any-engine-allocation-dies)).

**`0x4B8DA0(name, w, h)` never writes the key byte.** `ret 0xC`, six callers (`0x420D49`,
`0x42960D`, `0x4666A4`, `0x4838F5`, `0x495ACD`, `0x495AEE`): it allocates `w·h + 0x18` through the
named allocator `0x4D83B0` and writes `+0x00` w, `+0x02` h, `+0x04`/`+0x06` (the hotspot) 0,
`+0x09`/`+0x0A`/`+0x0B` 0, `+0x10` the pixel pointer and `+0x14` 0 (`0x4B8DCD..0x4B8DEC`) — **never
`+0x08`, the key colour, nor `+0x0C`**. A frame it makes carries whatever the allocation held
there: SELMAP's preview frame measured 18 on one pick and 65 on another.

**What that means for anything that captures the preview**, all MEASURED 2026-09-26:

- **The address and the size repeat.** Every pick frees the frame and allocates the next at
  once, and the allocator hands back the same block, so the frame pointer is no identity for the
  picture.
- **The plane's head is the letterbox.** For any map wider than tall the first rows are the fill
  (index 0), so a hash of the plane's first bytes makes every such map the same picture.
- **The key byte is noise**, so a consumer that drops texels equal to it drops real minimap
  pixels. The engine drops none: the preview reaches the panel through `0x4C7580`, whose span
  `0x4C7310` copies every texel. That is why the Vulkan lane now carries such a stamp as the
  indices it leaves ([GUI renderer](../gui-renderer.html), `PK_PLANE`).

### The GAF banks a screen can reach [VERIFIED 2026-09-09]

Three different name→bank paths, and only the first is the single common one:

| site | builds | bank stored at |
|---|---|---|
| **`0x4AEEE0(gi, name)`** | `<prefix at gi+0xAB6><name>` + ext `"GAF"` (`0x502E38`), opened `0x4BBC40`, loaded `0x4B8C60` | **`gi+0x04`** — `mov [ebx+0x4],eax` at `0x4AEF66` |
| `0x4A8444`, gated on `test [gadget+0xB4],0x1` | `<prefix><gadget name>` + `"_gadget"` (`0x509970`) + `.GAF` | the **gadget**, `mov [ebp+ebx+0x2b],eax` at `0x4A84C7` |
| `0x4A8565`, inside the per-`id` dispatch (`jmp [eax*4+0x4A95F4]`, `0x4A84EB`) | `<prefix><name>.GAF` | per gadget |

- **`gi+0x04` holds ONE bank and has exactly ONE call site**: `0x49154E` passes the hardcoded
  `0x50925C = "commongui"`. This upgrades the [CORPUS] `commongui_GAF` field name to verified.
  **`0x4AEEE0` leaves the old bank in place if the file is absent** (`je 0x4AEF69` skips the
  store).
- **But a screen's gadgets DO load their own GAFs by name**, which is the corollary
  [renderers](../renderers.html) §2.10 previously got wrong. 115 `anims/*_gadget.gaf` ship (all
  unit build pages) and `armopt.gaf`, `prefs.gaf`, `mainmenu.gaf` exist plain. So **new art
  reaches the engine as a file it loads itself**, not by surgery on the bank at `gi+0x04`.

### Setting a gadget's state: the engine writes the field [VERIFIED 2026-09-09]

**[CORRECTED 2026-09-09, by the G18 landing review — the earlier claim here was wrong.]** This
section used to say "no instruction anywhere in `0x49F000..0x4AB000` writes a gadget's
`status_curnt` (`+0x137`), so the engine never advances a stage button". **It does.** Three
sites in the GUI code advance it, each an `inc` of `+0x137`:

| at | shape |
|---|---|
| `0x4A6EC8` | `mov dl,[ebp+0x137]; inc dl; mov [ebp+0x137],dl`, guarded by `[ebp+0x136] != 0` (the stage count) |
| `0x4A9DB6` | the same on `ebx` |
| `0x4AA377` | `dl = [ecx+0x137]; bl = [ecx+0x136]; inc dl; cmp dl,bl; mov [ecx+0x137],dl; jb keep; else store 0` — **inc AND WRAP** against the stage count, then `call 0x4A5F40` to redraw |

The third is the dispatcher's own path and carries the guard that matters:
**`0x4AA36A` — `test BYTE PTR [ecx+0x13c],0x1`, `jne` past the whole block.** That is why a
greyed row refuses the click: the engine skips its own advance.

App code writes the field too (`0x47743B`, `0x47746A`, `0x47843D`, `0x478469`, `0x478497`,
`0x47A4F3`, …), so both do. **What this means for a screen of our own:** the engine advances
the clicked row *before* `OnCommand` sees it, so a handler that also advances would
double-advance. `tagpu_menu.c` is correct only because it keeps its own model and re-pushes
**every** row through `SetStatus` afterwards, which overwrites whatever the engine did — a
property worth knowing before anyone removes that re-push.

TA's own idiom, at `0x477416`: push a gadget **name**, resolve the record through `0x49FF10`
(a find-by-name taking `ControlsAry`, sibling of `GUI_FindGadgetByName 0x49FE60`), then
`mov BYTE PTR [esi+0x137],0` — a **direct field write**.

The setters exist and are thin:

- **`0x4A1080(gi, name, value)`** — the [CORPUS] `GUIGADGET_SetStatus`, now read. Scans
  `ControlsAry+0x15D` in `0x15B` strides comparing 16 bytes (`0x4E4B50`), then
  `mov BYTE PTR [ebx+eax*2+0x137],cl` and returns 1. **No clamp, no callback, and no redraw** —
  it is the direct write plus a name lookup, nothing more.
- **`0x4A1110(gi, name, value)`** — same scan, but writes the **word** at `+0x138`
  (`status_init`), sets `gi+0xCCA` and, for a non-zero value, calls `0x4A0340(gi, idx)` —
  the **radio-group reset**, which walks every other `id 1` gadget sharing the gadget's
  `assoc` and clears its `status_init`, redrawing each through `GUI_ButtonDraw 0x4A5F40`.

**`gi+0xCCA` is the screen's DEFERRED-REPAINT flag** [VERIFIED 2026-09-09, G18 gate 2 —
this closes the `[OPEN]` this section and roadmap G18 carried]. About twenty state-changing
GUI calls set it and there are bare accessors — `0x49FA90(gi)` sets it, `0x49FAB0(gi)` clears
it, both `ret 4`. There is exactly **one reader**, `0x4AA0AF` inside the GUI pump:

```asm
4aa0af:  cmp DWORD PTR [ebp+0xcca],1
4aa0b6:  jne 0x4aa0d2
4aa0bb:  mov DWORD PTR [ebp+0xcca],0
4aa0c5:  mov edx,[ecx+0x10]          ; the top screen's own flags
4aa0c8:  or  edx,0x40
4aa0cd:  call 0x4a81e0               ; GUI_StageUpdateDraw(gi, flags | 0x40)
```

So it is **not a precondition of anything**: it is the engine's own way of asking for the
repaint, and it repaints with the *screen's* flags rather than a bare `0x40`. Setting it and
letting the pump draw is therefore strictly better than calling `GUI_StageUpdateDraw` by
hand, and it coalesces several changes into one repaint. A bare `SetStatus` needs only this.

Redrawing is separate either way: `GUI_StageUpdateDraw 0x4A81E0(gi, flags)` with `0x40`.

### Which screen is on top in game, and what owns the top bar [LIVE 2026-09-09]

Measured with `tools/tacli ui` on a 1024×768 skirmish:

```
nothing selected    gui ARMMAIN2.GUI   under: -
commander selected  gui ARMCOM1.GUI    under: ARMMAIN2.GUI
```

**A builder's page covers MAIN2**, which is most of a game. And every in-game screen's panel
is the *side* panel — `armmain2`, `cormain2`, `armmain`, `armgen` are all `(0,128) 128×352`;
`tabmenu` alone is `(130,−33) 510×33`. **No GUI screen owns the top bar** (`y 0..31`, full
width): the engine draws the resource bar itself. Since a gadget's coordinates are
panel-relative and it is drawn into the panel's own `w×h` surface (`panel+0xBC`), **a gadget
on the top bar is impossible** — which is why the render-options trigger is drawn and
hit-tested by the DLL and is not a gadget.

### A screen's own GAF: the PANEL is the loader [VERIFIED 2026-09-09, Phase F G18]

*Read because [renderers](../renderers.html) §2.10 needed new panel art to reach the engine as a
file it loads itself. It does — but not through the `id=12` gadget that names the frame.*

`GUI_StageUpdateDraw`'s per-gadget dispatch is `cmp eax,0xD; ja …; jmp [eax*4+0x4A95F4]` at
`0x4A84E2`, indexed by **`id` directly** (unlike the *draw* dispatcher at `0x4A962C`, which is
indexed by `id-1`). The table:

| `id` | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| → | **`4A84F2`** | `4A8B4B` | `4A8A16` | `4A899F` | `4A8663` | `4A900C` | `4A9045` | `4A8EF3` | `4A8FA0` | `4A9045` | `4A9045` | **`4A84F2`** | `4A8ACA` | `4A8FEA` |

**Only `id 0` (the panel) and `id 11` (picture) reach `0x4A84F2`**, the branch that builds
`<prefix at gi+0xAB6><gadget name>` + `"GAF"` (`0x502E38`), opens it with `0x4BBC40` and loads
it with `0x4B8C60` into the **gadget's own `+0xC0`** — and only when `+0xC0` is still NULL.
`id 12` goes to `0x4A8ACA` and looks its frame up in a bank it did not load.

**So the panel is the loader, and its name is the one `GUI_Load` stamped** — the screen name —
which the extension setter turns into `anims\<screen>.GAF`. An `id=12` gadget then names a
frame in that bank, falling back to the shared `commongui` bank at `gi+0x04`.

The stock corpus says exactly this, and it is what makes the rule safe to build on:

| screen | its `id=12` | `anims/<screen>.gaf` holds | so the frame comes from |
|---|---|---|---|
| `ARMOPT.GUI` | `OPTBG` | `OPTBG` | its own file |
| `PREFS.GUI` | `IGOPT` | `PREFSBG` | `commongui` (`IGOPT` is not in prefs.gaf) |
| `VISUALRT.GUI` | `VISUALSRT` | *the file does not exist* | `commongui` |
| `MAINMENU.GUI` | *none* | `Credits` | the frameless logo button |

A screen's own GAF is therefore **optional**, and this also explains `armopt.gaf`,
`prefs.gaf` and `mainmenu.gaf` existing plain beside the 115 `*_gadget.gaf`.

**Live consequence, measured:** an archive carrying `anims/renderdd.gaf` for a screen called
`RENDER.GUI` was never opened and the panel kept the engine's dialog composite; the same
bytes at `anims/render.gaf` load.

**On a MERGED screen it is the merged file's name, not the pushed one** [VERIFIED
2026-09-11]. `0x45E5E0` pushes `STARTOPT.GUI` and then merges `VISUALS.GUI` over it with
flags `0x200`, and the merge re-stamps `ControlsAry[0].name` (`0x4AAC98`) — so by the time
stage 1 runs the panel is called `VISUALS.GUI` and the file it opens is
**`anims/visuals.gaf`**. The stock game ships no such file, which is what lets a `.ufo`
supply one without shadowing anything. Putting the frames in `anims/startopt.gaf` instead
loads nothing at all.

**`0x4B8D40(bank, name)` is the by-name entry lookup**, and it gives the bank's layout:

```asm
4b8d4e:  lea esi,[ebx+0xc]          ; bank+0x0C = GAFENTRY*[] , stride 4
4b8d51:  cmp WORD PTR [ebx+0x4],di  ; bank+0x04 = i16 entry count
4b8d5b:  mov eax,[esi]              ; the entry pointer...
4b8d5e:  add eax,0x8                ; ...+0x08 is its name  (tagpu_gaf.h TAGPU_SQ_NAME)
4b8d62:  call 0x4f8a70              ; string compare, 0 on match
```

`stdcall`, `ret 8`, returns the entry or NULL. `0x429B66` uses it for `igpaused`.

**`CopyGafToContext 0x4B7F90(ctx, frame, x, y)` takes `ctx = NULL` as the back buffer**
(`0x4B7FA5` → `0x4C5E70`), so a frame built in memory can be blitted into the buffer every
flip presents without knowing the surface layout at all. `0x46A308` — inside `DrawGameScreen`
immediately after its own GUI draw at `0x46A303` and before the flip — is where the G18
trigger uses it.

### A world click REBUILDS the in-game GUI stack [MEASURED 2026-09-09, Phase F G18]

With a screen of ours pushed over the world and `main+0x37EA0` naming it, a single left click
left the top `GUIMEMSTRUCT` a **freshly allocated `ARMMAIN2.GUI` whose `per_active` is NULL**:
our screen and the `ARMMAIN2` beneath it were both freed and the in-game screen was loaded
again from scratch. The expected-screen buffer still read `RENDER.GUI` throughout, so this is
not `UpdateIngameGUI` popping — it is the engine rebuilding the stack on its own account
(the selection path; a panel over the world takes its own clicks through it too).

**Consequences for anything living over the world:** the screen must be re-pushed rather than
assumed to persist, and whatever state it shows must be *the pusher's*, not re-read from
elsewhere on the re-push. Re-reading it put every button back the moment it was clicked.

**And `gi->UIChange_f == -1` is not proof of a pop.** `GUI_Pop` does set it before calling
`OnCommand` (`0x4A9673`), but the pump resets it too (`0x4AA096`) and calls `OnCommand` again
on the same click. The authority on whether a screen is still there is the `per_active` chain
from `main+0x531`.

**The dispatch itself is `0x4A967C`–`0x4A9687`**, and it settles the signature: `eax =
gi->TheActive_GUIMEM` (`+0x18`), then `eax = [eax+0x08]` (`0x4A967F`), `test eax,eax`, `je`
past it, and only then `push esi` — the **GUIInfo** — and `call eax`. So `OnCommand` is
NULL-guarded, takes the GUIInfo as its one `stdcall` argument, and is **never handed the
index**. The three fields set to −1 immediately before it explain what it then reads:
`0x4A9670`'s `or eax,0xffffffff` is stored to `+0x64`, `+0x60` and `+0x68` (`0x4A9673`,
`0x4A9676`, `0x4A9679`), and `+0x60` is `UIChange_f` — `main+0x519 + 0x60 = main+0x579`.

### The palette the screen is presented with, the way out of a game, and the loading screen [VERIFIED 2026-09-07, Phase E G15d]

Read for [the GL UI renderer](../gui-renderer.html) §12. The twin resolves its indices through a
palette, and the question was whether `main+0x143A7` — the table every world pass reads — is the
one the engine's own frame is displayed with. In general it is not.

**`0x4BA200(PALETTEENTRY* entries, int first, int count)` — every palette the screen gets goes
through here.** `stdcall`, `ret 0xC`, prologue `81 EC 14 08 00 00 53 55 56 57`. Under the palette
lock (`0x52A4E8`, `[0x4fc174]`/`[0x4fc170]`, owner tag `0x4D41494E`) it copies the entries into
**`globals+0x214 + 4·first`** (`0x4BA265..0x4BA297` — the graphics globals keep the last palette
set), then builds a scaled copy on the stack, each byte **`min(255, byte × *(float*)(globals+0x614))`**
(`fild`/`fmul`/`fcom 255.0`, `0x4E43A0` the CRT `ftol`, `0x4BA2C2..0x4BA35E`), and hands *that*
on: under `[globals+0x44]` (the GDI present) as a `LOGPALETTE` (`0x300`, 256) through the imports
`[0x4fc05c]`/`[0x4fc054]` (`CreatePalette`/`SetPaletteEntries` [INFERRED from the shape]);
otherwise (`0x4BA450`), when `[globals+0xF0]` bit 2, **`[globals+0x94]->SetEntries(0, first,
count, copy)`** — `call [ecx+0x18]` at `0x4BA476`, `IDirectDrawPalette`'s slot 6, which is
cnc-ddraw's `ddp_SetEntries`. Returns 1, or 0 when `SetEntries` failed.

- **`SetGamma 0x4BA590(float g)`** [CORPUS name, `tools/ta_symbols.txt`]: `globals+0x614 = g;
  0x4BA200(globals+0x214, 0, 256)` — the last palette re-applied at the new gamma. Seventeen
  callers. `0x4914A7` in `UIPipelinesInit 0x491200`, and `0x45BCDB` inside `0x45BCC0` (called
  from `0x49147C`), both compute `0.5 − Gamma × (−1/24)` = **`0.5 + Gamma/24`** from
  **`main+0x37F08`, the registry `Gamma`** (default 12 at `0x4301C0`, so a factor of 1.0);
  `0x45BD20..0x45E3F0` are fourteen option-screen handlers that write `+0x37F08` from a slider
  (`pos/(n−1) × range` at `0x45BD4E..0x45BD6F`) and re-apply [role INFERRED, the formula read at
  `0x45BD86`]; **`0x4172B2` is the `+gamma N` chat command** (`Gamma 0x417290` in the NORMAL
  cheat table): `SetGamma(N × 0.1)`, then `main+0x37F08 = N` (`0x4172C8`) — so `+gamma 15`
  presents at 1.5 and `+gamma 10` puts 1.0 back, from any skirmish, no cheat bit needed
  (MEASURED); `0x45FE54` sits with the mission-start flash.
- The eleven callers of `0x4BA200`, and what they hand it: `0x497FDB` (game entry `0x497F40`:
  `main+0x143A7, 0, 256`, before the front-end GUI stack is torn down); `0x44460B`
  (`main+0x143A7` from `ebx`, 256, after `GUI_Pop 0x4A9660`); `0x4B58CD` (`globals+0x214, ebp,
  256` — the globals' own copy, inside `DDrawDeviceCreateAndCaps 0x4B5510`, so a re-created
  screen starts with the palette the old one had); `0x4BA5AC` (`SetGamma`); `0x41E000` and
  `0x41E403` (the fade, below); `0x4ACCC4` (**one entry**: `0x4ACC70(gi, RGBQUAD* pal)`,
  `ret 8`, builds it from three sliders' positions — `gi+0xCB6`, `+0xCBA`, `+0xCBE`, each
  gadget's `+0x140` — into `pal[gi+0x9B2]` and sets index `gi+0x9B2`: an RGB colour editor
  [INFERRED]. **`0x4ACC70` is dead code** [VERIFIED 2026-09-24 against
  `pristine/TotalA.exe.pristine`]: no `call` or `jmp` in the whole disassembly targets it, and
  its address appears nowhere in the file as an absolute value, so no callback table or `push`
  can hand it out either); `0x45FBDF` (a zeroed buffer — all black — then
  `0x4C69A0(main+0x37E1B)`, a `SurfaceFill` with `main+0xDCB[…]` and a flip: the blackout
  `0x45FBC0`, `0x45FC33` its end); `0x428AA9`, `0x44B049`, `0x476798` (palettes loaded from
  files).
- **Who writes the engine's own table, and what moves the presented one in play** [VERIFIED by
  disassembly 2026-09-23, for G20c's claim that only the Gamma moves the palette in play]. The
  one direct writer of `main+0x143A7` is **`0x42A400`**, which loads `"PALETTE"` (the string at
  `0x5033A4`) into it with `rep movsd`; its one caller is `UIPipelinesInit` at `0x491378`, so it
  runs once per process. Every other reference passes the address as an argument —
  `0x444604`/`0x44461C`, `0x478F29`, `0x497FD4`, `0x49813B` and the table builders at
  `0x491388..0x4913CD`. **None of those arguments reaches a writer** [VERIFIED by disassembly
  2026-09-24]. `0x4AC7D0(gi, pal, src)`, `ret 0xC`, copies 1 KB from `src` into `gi+0xB2`,
  then for each of those 256 entries finds the nearest entry of `pal` (the sum of the three
  channels' absolute differences, first minimum) and writes its index to `gi+0x8B2[i]`: the
  GUI's 256-byte colour LUT, built at `0x4AC7D0..0x4AC89C`, which calls nothing. `+0x143A7` is
  its **second** argument, `pal`, at `0x444629`, `0x478F37` and `0x498148` — read and never
  written — with `gi = main+0x519`. At the first two `src` is `main+0x5CB`, the copy's own
  destination; at `0x498148` it is the `guipal` file `0x4BBE50` loaded (*The loading screen*,
  below). A fourth caller, `0x426503`, passes a file loaded the same way as `src` and `edi` as
  `pal`. And the one single-entry palette writer the survey found, `0x4ACC70` (above), has no
  caller at all, so it cannot write `+0x143A7` whatever it would be handed. Of the eleven
  `0x4BA200` callers, three are the shell's (`0x428AA9` inside `0x4288D0`, a `"Palette"` loaded
  from a file by many shell screens; `0x44B049` inside `0x44A680`, called from `0x42837F` in the
  shell state machine `0x426E80`; the glamour fade), one is the ARMOPT family's blackout
  (`0x45FBDF`, `0x45FBC0`, beside `0x4609B0`), and **`0x476740` — whose call is `0x476798`, a
  `bitmaps\<name>.PCX` loader — has no reference anywhere: dead code.** MEASURED the same day: a
  `big-battle` soak logged the engine table's arrival and nothing after it (`tagpu_pal.c`'s
  `pal: the engine's table changed (engine serial=1)`, one line per process), so in play only
  `SetGamma` moves what the screen is presented with.
- **So the presented palette is `gamma(globals+0x214)`, and `main+0x143A7` is never scaled.**
  On every normal path the two hold the same entries — `+0x143A7` is what `0x497FDB` and
  `0x44460B` hand over — and at Gamma 12 they are byte-equal. At any other Gamma, or after
  `+gamma`, a pass that reads `+0x143A7` shows the world at the wrong brightness, and every one
  of them did until G15d fixed the UI twin (which resolves through cnc-ddraw's palette object,
  `g_ddraw.primary->palette->data_rgb`, what `ddp_SetEntries` stored — the same table
  `tacli shot` writes into its PNG, so the walk's oracle and the twin agree by construction).
  **The world answers it the other way round since G20c** ([GPU status](../gpu-status.html) §2.3f):
  every world colour is built from the unscaled `+0x143A7`, and `globals+0x614` is applied ONCE,
  to the finished world image, through the engine's own per-level curve
  `min(255, trunc(e × factor))` — so the world matches the engine's presented pixels without a
  scaled table anywhere in it, and the restorer's tileability threshold, a raw colour distance,
  classifies the ART and not the display. **The two
  formulas are different and both are live**: an option screen writes `main+0x37F08` and applies
  `0.5 + Gamma/24` (registry Gamma 15 → **1.125**, measured), while `+gamma N` applies **`N/10`**
  outright (`+gamma 15` → **1.500**, measured 2026-09-09) and then stores N in the same field —
  so the field alone does not tell you the factor, and `globals+0x614` is the only thing that
  does. MEASURED, the heartbeat's `paldiff` counts raw differences, so at the stock 1.125 it
  reads **235** in the shell and in game alike; *beyond the gamma scale* 0 entries differ in
  game, and in the shell **one, index 9**, on every visit (its writer is not traced — whichever
  it is, it reaches the screen through `0x4BA200` like everything else, so the twin shows it
  right and a `+0x143A7` reader would not); `+gamma 15` in game makes every
  non-black entry differ and the twin still matches the engine's frame ([GL UI
  renderer](../gui-renderer.html) §12 has the run).
- **The Gamma this project runs at is a mutable registry value — read it, never assume it**
  [MEASURED 2026-09-09, [GL UI renderer](../gui-renderer.html) §15, which corrects §14]. Under
  tacli it is each instance's own, in its registry store (`tacli registry <i> Gamma`, seeded
  from the template's `user.reg`); the measurements below were taken when every instance read
  the shared file.
  `0x4301C0`'s default of 12 applies only when the registry value is absent, and it is not
  absent. But it is also **not a property of the template prefix**: `wineprefix/user.reg` and all
  58 `tagpu/instances/*/prefix/user.reg` are **one inode with 59 hard links** (`tacli`'s
  `clone_prefix` is `cp -al`), and wine rewrites that file **in place at every launch**, so there
  is a single `Gamma` for the template and every instance and it is whatever TA last stored.
  Both values have been read hours apart on the same day, on the same DLLs:
  **at 15** the presented factor is `0.5 + 15/24 = 1.125`, applied by **truncation** — the
  presented palette reproduces exactly as `min(255, (int)(entry × 1.125))` in all 256 entries,
  with rounding in only 86 — and it differs from `main+0x143A7` in **235 of 256 entries in the
  shell and in game alike**; **at 12** the factor is 1.0 and `paldiff` reads **0**. An earlier
  reading of this paragraph recorded `paldiff` as 0 in game and a later one called 235 "the
  ordinary reading"; **neither is ordinary** — the heartbeat's `paldiff=` is the measurement, and
  `+gamma N` in chat moves it for the rest of that process.
  Consequence, measured the same day: every pass that reads `+0x143A7` — terrain, features,
  effects, markers, the 3DO/unit atlas — draws the world ~11 % darker than the engine presents
  its own pixels **whenever the factor is not 1.0**. In a Classic frame with those passes drawing at 1.125, 14 896
  of a 17 049-pixel viewport sample are exact `palette.pal` colours against 244 presented ones
  (the two palettes share 8 of 256). Not a bug with an obvious side: the browser lab is built on
  `palette.pal`, so a world matching it is what `tascene ab` parity measures. Since G20c the
  passes still read the unscaled table and the world composite applies the factor after them, so
  the world is presented at the engine's brightness ([GPU status](../gpu-status.html) §2.3f).

**The glamour-screen fade — `Palette` / `currentPalette` / `desiredPalette` / `FadeTable` at
`main+0x3907F..0x3908B`** [mechanism VERIFIED; reach INFERRED]. The corpus glosses these four
"menu fades"; the menus never fade. `0x41DA60` allocates the four 0x400-byte buffers with
`MEM_Alloc 0x4D83B0(tag, 0x400)` — the tags are the names, `"FadeTable"` → `+0x3908B`,
`"desiredPalette"` → `+0x39087`, `"currentPalette"` → `+0x39083`, and `"Palette"` → `+0x3907F`
loaded from a file (`0x4290F0` at `0x41DB60`, `0x429290` at `0x41DBB2`; the neighbouring strings
are `glamour\Arm01.PCX` and `bitmaps\glamour.PCX`, `0x502950`/`0x502964`) — and saves the gamma
to `main+0x3906F` while setting `globals+0x614` to 1.0 (`0x41DAE3`). `0x41DFC0(from, to)`:
`desiredPalette ← from`, `currentPalette ← to`, `0x4BA200(to, 0, 256)` — the start; its one
caller `0x41FD55` passes `(Palette, a zeroed stack buffer)`, so the screen starts black.
`0x41E270` is the stepper: `0x4B6340()` (the millisecond clock [INFERRED]) against
`main+0x3905F`, then `FadeTable` indexed per byte into `currentPalette`, then
`0x4BA200(currentPalette, 0, 256)` at `0x41E403`; one caller, `0x41FE02`. `0x41EC50..0x41ED9E`
frees the four. All of it is one family, `0x41D8A0..0x41FEyy`, entered at `0x41FC12` behind
`0x435100(main+0x391E9) == 1` (`0x41FB77`) — the campaign flow [INFERRED from the glamour art],
and **nothing a skirmish does reaches it** (MEASURED: the walk's `palchg` moves only at the
switches and at `+gamma`). Whatever runs it, the layer follows: each step is a `SetEntries`, and
the twin's palette texture is re-uploaded from the presented table at the next present.

**Getting there: the in-game exit menu** [VERIFIED 2026-09-12]. `0x4608B0` is the only thing
that raises it — `GUI_Push(main+0x519, "EXITMENU.GUI" @0x506D64, 0x1800)`, then
`0x49FDF0(gadget, "RESTART" @0x506CA8, 1)` and `[screen+8] = 0x460800` (its OnCommand). It has
**one caller**, `0x460C81`, inside the in-game panel's OnCommand — the arm taken when the
gadget that fired is named **`"EXIT"`** (`0x503034`), one of `PREFS` / `HELP` / `MISSION` /
`MOREBAR` / `SAVEGAME` / `LOADGAME` / `EXIT` that handler dispatches. That gadget is **not on
the side panel**: it lives on `ARMOPT.GUI`, which **Tab** opens over the world. `EXITMENU.GUI`
itself carries `MAINMENU`, `EXITGAME`, `RESTART` and `CANCEL`, and `MAINMENU` raises a
`YESORNO.GUI` ("Surrender this battle and return to main menu?") whose `CHOICE1` is the one
that reaches the handler below. Esc does nothing in game: code `0x1B` lands on case 39 of the
in-game dispatcher's table (`0x496694` → `0x4965F4[39] = 0x4965CE`), the default.

**Input ownership for that stack** [VERIFIED 2026-09-14]. F2 and Tab set
`main+0x37EBE` bit 0 while `ARMOPT.GUI` is up; it remains set through
`EXITMENU.GUI`, `YESORNO.GUI` and the in-game preferences screens, and clears on
Resume. The frame callback still reaches `DrawGameScreen` with this bit set, so a
zoomed world remains published beneath the 1:1 GUI. Mouse input must therefore use
this bit as an ownership gate before applying the world s -> u transform. Live at
0.25x before the gate, `MAINMENU` at `(577,336)` returned to `ARMMAIN2`; at 8x it
produced no GUI change. With the gate, the same click raises `YESORNO` at both
limits, and `EXITGAME`, `CHOICE2` and `OK` land on their named gadgets as well.

This is **not a general in-game-modal flag** [DISASSEMBLY 2026-09-15].
`SHARE.GUI` is pushed at `0x49373C` and sets bit 6 of the same word at
`0x49374F` (`or byte [main+0x37EBE], 0x40`), so the bit-0 input gate does not
cover that screen. The options path pushes `ARMOPT.GUI` at `0x4961B7` and sets
bit 0 afterwards at `0x4961C1`; the earlier wording said the bit was set before
the push. The measured options/exit/preferences stack is closed; share-dialog
input under zoom remains unverified and unfixed.

**Leaving a game — `0x491ADC..0x491B38`** (inside the leave-game handler; the block begins with
the width test at `0x491AA0`) [VERIFIED]. `cmp eax, 0x1E0; je 0x491B5D` (already 480 high:
nothing to switch); **`MEM_Free 0x4D85A0(main+0x37E1B)` at `0x491AB8` — the game's OFFSCREEN goes
straight to the heap, not through `SurfaceFree 0x4C6AC0`** (`0x49838C` does the same at the
game's mode switch); `main+0x37E1B = 0`; `0x4C61F0(0)`; `0x4C62C0()` (restores the DirectDraw
surfaces `globals+0x88`/`+0x8C` and re-sets their palette — slots `+0x60`, `+0x6C` — no release);
`SetWindowPos(…, 640, 480, 4)` (`[0x4fc2f0]`, the call at **`0x491AFB`** — **a no-op under our
fork**, which swallows it in `fake_SetWindowPos`: [resolution](../resolution.html) §3.1c);
`NewTAScreen(640, 480)` at `0x491B0B`;
`main+0x37E1B = 0x4C69F0("OFFSCREEN", main+0x37E1F, main+0x37E23)` at `0x491B28`. **The tag
`"OFFSCREEN"` is the string at `0x5091D4`**, and the five sites that create the main offscreen
with it are `0x490AD3`, `0x491250`, `0x491B23` (this one), `0x4980CF` (the loading-screen
640×480, [resolution](../resolution.html) §2.2) and `0x498402` (the game's mode-switch re-create) —
the observer keys the main offscreen on that tag so a re-created one retires the last (G15d). MEASURED: the new 640×480
lands on the base the 1024×768 had (`05190050` both, first fit), so the observer sees a
**same-base size change** and never a free — `surf_get` now drops the ring's recorded boxes on
such a change, where before the next publish read a 1024-wide box off a 640-wide surface and
called it an overflow. `NewTAScreen` calls `DDrawDeviceCreateAndCaps 0x4B5510`, which releases
the old DirectDraw objects through `0x4B4FF0` **before** making new ones. In the proxy,
the final `dd_Release` stops and joins its render thread and zeros `g_ddraw`.
The subsequent `dd_SetDisplayMode` starts a new render thread. A ready Vulkan lane now
survives this joined replacement on the same actual window lifetime; the shell publisher's
stall guard still spans the interval with no renderer. [fullscreen-handoff](../fullscreen-handoff.html)
records the original measured 619 ms restart and escaping resize, and the fix: a persistent
window subclass retains resize ownership and observes actual destruction after Release.

### The loading frame — what the game thread draws while the loader thread runs [DISASSEMBLED + MEASURED 2026-09-29]

*Read with `i686-w64-mingw32-objdump -d -M intel` on `pristine/TotalA.exe.pristine`; the flips and
the ops recorded on the Vulkan lane's UI layer are a Two Continents skirmish on Wine, one load.*

The game-entry handler `0x497F40` runs the first-call path once and then, on every later call,
the poll path `0x498340`; while the loader thread has not set bit 1 of `main+0x38D75`
(`0x497C62`), that is the loading frame `0x4984DD..0x498CB2` (*The level load runs on a LOADER
THREAD*, above, has the flag word). **The game thread flips five times in one load**, told apart
by the return address the flip sees:

| return | call site | what it presents |
|---|---|---|
| `0x498025` | `0x498020`, `call 0x4257A0`, after the GUI stack is popped empty (`GUI_Pop 0x4A9660` in the loops at `0x497F6A` and `0x498008`) | one black frame, at the start of the load |
| `0x498CB2`, once per poll call (3 in this load) | `0x498CAD` | **the loading frame** |
| `0x49835D` | `0x498358`, `call 0x4257A0` | one black frame, once the loader is done and the in-play entry is next |

**`0x4257A0` is blank-and-present**: `SetBackBuffer(main+0x37E1B)` (`0x4C69A0`, `0x4257AC`),
`SurfaceFill(0, 0)` (`0x4C6890`, `0x4257B5`), then **`jmp 0x4C63A0`** (`0x4257BA`) — a tail call,
which is why the flip's return address is its caller's. Its other callers include `0x491C60`
and the post-game path of `0x499200` (the state-handler table, above).

**The frame.** `0x4C69A0(main+0x37E1B)` (`0x4985A5`), then `GetContext 0x4C5E70(&ctx)`
(`0x4985AF`); a zero answer jumps to `0x498CB2`, so a failed context skips the drawing **and the
flip**, and only the sleep runs. `SetFont 0x4C1420(main+0x391F9)` (`0x49860A`). The backdrop is
`0x4C6B70(&ctx, [main+0x11EB], 0, 0)` (`0x498623`, args read off the pushes at
`0x498615..0x498622`): the PCX surface `0x4288D0` stored, copied whole. Then the labels, the six
bars, and the caption. Every draw ends at `0x4C5FA0(&ctx)` (`0x498CA8`), the flip (`0x498CAD`),
and **`push 0xC8; call 0x4B6B50` (`0x498CB2`, `0x498CB7`) — a 200 ms sleep per frame**, whatever
the symbol table's name `SleepBatch100ms` says (the measured spacing of the three flips was 202
and 201 ms).

**What the UI layer records for one frame** [MEASURED, `gui.on=log`, 76 ops on the offscreen,
the same count on each of the three]: one `copy` of the whole 640×480, glyph blits for the
caption (at y 460..473, the map name) and for each label (x 90, one row per bar), and per bar row
a one-pixel-wide `bar` at x 205 and a `gaf` box `(205, y)..(555, y+20)` — the segmented bar
picture — at **y = 135, 177, 218, 262, 304, 347**, which the picture shows as Textures, Terrain,
Units, Animation, 3D Data and Explosions (the labels are translation keys, `0x4C5740`). The first frame's label glyphs arrived as `gaf`
blits and the second's as `gafb` (the shaded blit `0x4B8310`, which `DrawText 0x4A50E0` takes with
a non-zero shade), and one bar row drew its picture twice in the frames dumped [which row, and why,
not identified]. Read and not examined: `0x498C8F..0x498C9E` (with `GetGameingType == 3` the frame
calls `0x497CE0(&ctx)` and `0x456DE0()` before the context is released), the timing block
`0x4985CC..0x4985FB` (`0x4B6340`, a byte table at `0x51E6C8` decremented by 2 per tick), the
caption arm `0x498647..0x49872A`, and the font-colour latch `0x498757..0x4987B4`
(`SetFontColor 0x4C13A0` with an index from the GUI LUT at `main+0xDCB`).

### The Visuals options the store owns [DISASSEMBLED + MEASURED 2026-09-23]

The engine's side of the settings store ([renderers](../renderers.html) §2.10b): four fields of
`main`, where the engine loads and saves them, how its two Visuals screens find their gadgets,
and what each stock button writes. `tagpu_menu.c`'s section *"the engine's Visuals options,
owned by the store"* is the consumer.

**The fields.**

| field | width | what | set by |
|---|---|---|---|
| `main+0x37F06` | **WORD** | the option word, one registry `REG_DWORD` per bit (below) | the loader, the three stock toggles, the console toggles, RESTORE/UNDO/CANCEL |
| `main+0x37F08` | **signed dword** | Gamma, 0..20 on the slider; the option screens apply `0.5 + Gamma/24` (constants `−1/24` at `0x4FD4F0`, `0.5` at `0x4FD4F8`) | the loader, the slider callback `0x45BD20`, RESTORE, UNDO, CANCEL, `+gamma N` (`0x4172C8`, which applies `N/10` instead) |
| `main+0x37F1B` / `+0x37F1F` | dwords at odd offsets | the in-game screen size ([resolution](../resolution.html) §4) | the loader, `VIDSLDR`'s callback `0x45BBF0`, RESTORE (640×480, front end only), UNDO, the battleroom picker |

| bit of `+0x37F06` | registry value | read by the loader at | default |
|---|---|---|---|
| 0 | `damagebars` | `0x42FD4C` | 0 |
| 1 | `Anti-Alias` | `0x42FF38` | 1 |
| 2 | `Shadows` | `0x42FF9B` | 1 |
| 3 | `VehicleShadows` | `0x43006A` | 1 |
| 4 | `FeatureShadows` | `0x430000` | 1 |
| 5 | `Shading` | `0x4300CF` | 1 |
| 6 | `DitheredFog` | `0x430134` | 0 |
| 8 | `SwitchAlt` | `0x43020A` | — |

The Gamma load (`0x43019B`) remaps a stored **10 to 12** without writing it back (`0x4301B7`),
stores any other value **unclamped** (`0x4301D2`), and writes 12 when the value is absent.

**The loader `0x42F9A0` has exactly three call sites** (a full relative-call scan):
`0x4913F6` in `UIPipelinesInit 0x491200` (startup; `0x491200`'s only caller is `0x49EA62`),
followed by `0x45BCC0` at `0x49147C` and a second `SetGamma` at `0x4914A7`, with nothing
between that writes `+0x37F08` — so a value written when the loader returns there is the one
applied; `0x4273E8`, in the front end's state machine (case `+0x2BBE == 7`, sub-result
`+0x2BBF == 0xB` [screen identity INFERRED]), which reloads **without saving first**; and
`0x47BB52`, in the skirmish setup, which saves (`0x47BB3D`), writes `NumSkirmishPlayers` and
reloads. MEASURED on a `scenario load`: one reload after startup, which read Gamma 12, word
`0x003E` and 1024×768 from the registry while memory held 16, `0x0022` and 1024×768.

**MEASURED 2026-10-05, scenario seat extent:** the stock-limits coverage DLL's
`replay-battle-load.json` launch with saved `NumSkirmishPlayers = 2` creates only
two commanders, even though its four controller keys are supplied. The subsequent
battle apply creates no units and reports 715 resolution errors, with slots 2/3
inactive. The same native load with saved count 4 creates four commanders and
accepts all 1428 battle units without errors. Playback's CLI launch writes its
recorded seat extent; ordinary scenario loading must also supply its own extent
before the native skirmish screen reads the store. The corrected CLI does so
through its existing locked prelaunch registry write and reproduces the four-seat
result from a count-2 starting store. This is a fixture-setup defect, not a COB
scheduling or replay-state comparison failure. The native four-seat limit remains.

**The saver `REGISTRY_SaveSettings 0x430F00`** writes the same names back from memory
(`0x431108..0x431216`, Gamma at `0x4311ED`) and has **27 callers**, among them Options Done
(`0x45FD5F`), **game entry (`0x497FF6`, every launch unless `0x435100(...) == 2`)**, every
console toggle (`0x416420`, `0x416510`, …: toggle the bit, re-bake, save), the damage-bars
hotkey (`0x496077`) and the battleroom (`0x446246`, `0x44626C`, `0x4462FC`). No dedicated save
on exit was found [INFERRED]. So the registry follows memory on its own.

**The dialog build `0x45E5E0(selvmode)` finds its gadgets by NAME, two ways.** Seeding runs
only for `selvmode == 0` (`0x45E93F`), after `0x4A1110(gi, "VISUALS", 1)` at `0x45E952`.
- **`0x49FDF0(ctrls, name, _)`** — `strnicmp` over 16 characters from `ctrls+0x15D` at stride
  `0x15B`; answers the index or **−1** (`0x49FE36`/`0x49FE4C`), and the caller skips on −1:
  `ANTI` (guard `0x45E96A`, then `0x4A1030(gi, idx, (word>>1)&1)`), `BSHADOWS` (`0x45E99E`,
  value **`(word>>4)&1`, bit 4**, not bit 2), `SHADING` (`0x45E9D3`, bit 5), `GAMMA`
  (`0x45EA08`), `VIDSLDR` (`0x45E70D`, front end only). **A missing toggle is harmless.**
- **`0x4A0200(ctrls, name)`** — the same search, but a miss is **FATAL**:
  `TerminateProcess_WithWarning 0x4B6290("Error in GUI layout")` (the string is pushed at
  `0x4A026A`), a message box, then `crt_exit(1)`. The build reaches it for `GAMMA` and `VIDSLDR` only after the
  guard; **the slider callbacks `0x45BD20` and `0x45BBF0` call it unguarded.** Any screen that
  keeps either callback must carry the gadget under exactly that name.
- `0x4A1030` is SetStatus by INDEX: it writes `+0x137` only when the gadget's type byte is 1,
  and answers 0 otherwise (`0x4A1056`).

**Sliders have no OnCommand branch.** A move fires the gadget's own callback at **`+0x144`**
(`mov eax,[ebx+0x144]` at `0x4A42DF`, `call eax` at `0x4A42F5`, `stdcall(gi, [ebx+0x14A])`,
`ret 8`). The build stores GAMMA's (`0x45EA1C..0x45EA92`): `+0x13C = 20`, `+0x144 = 0x45BD20`,
knob `min(Gamma, 20) · (w−1) · 0.05`, no lower clamp. **`0x45BD20`**: `g = knob/(size−1) ·
[+0x13C]`, stored as a dword in `+0x37F08` (`0x45BD7B`), `SetGamma(0.5 + g/24)` (`0x45BD9B`),
then the volumes are re-applied; no registry write. A slider event that reached OnCommand would
fall to `0x45E48C` and be ignored.

**`SetGamma 0x4BA590`** is `stdcall(float factor)`, `ret 4`: `globals+0x614 = factor`, then
`0x4BA200(globals+0x214, 0, 256)`.

**`OnCommand_VISUALRT_GUI 0x45E100`, per button** (the front end's `VISUALS` and the in-game
`VISUALRT` share it):
- `ANTI` (`0x45E16D`) — bit 1 = `GetStatus 0x4A0F60 & 1`. `BSHADOWS` (`0x45E1CB`) — bit 4 =
  status, then bit 3 = bit 4 (`0x45E21D`), bit 2 = bit 3 (`0x45E240`): **one click sets all
  three.** `SHADING` (`0x45E289`) — bit 5. In game (`main+0x37EBE & 1`) each then calls the
  re-bake **`0x437C80`**, `thiscall` with `this = [main+0x1437B]` (`0x45E26B`/`0x45E271`, and
  `0x45E2E5`) [role INFERRED: it flushes the unit-bake heap so units re-bake]. None calls
  `SetGamma` or writes the registry.
- **UNDO** (`0x45E2FD`) → **`0x45CAE0`**: bits 1..6 back from the stash byte `0x512F38`, Gamma
  from `0x512F3A`, the screen size from `0x512F4D/51` only when `!(main+0x2A44 & 4)`, then
  `SetGamma`; then `GUI_Pop` + `0x45E5E0(0)`.
- **RESTORE** (`0x45E331`): bits 1..5 set, Gamma 12, and off the game only 640×480 and
  **DitheredFog cleared** (`0x45E3CC`); `SetGamma` (`0x45E3F0`), the volumes, pop and rebuild.
  No registry write.
- Any other button (`0x45E499`): `GUI_Pop`, then the screen below's OnCommand, **`0x45FC60`**
  (installed at `0x460249`) [target INFERRED]. There **PREV ("OK")** saves (`0x45FD5F`) and
  **CANCEL** restores everything in memory from the stash, `0x45CAE0` and `SetGamma` included
  (`0x45FD91..0x45FF17`), without a registry write.
- The stash is taken once, when Options opens: `0x460160` copies `main+0x37EE6`, 0x53 bytes,
  to **`0x512F18`** (`0x460250`), so `0x512F38` is the word's copy. `0x45C740`, `0x45CC50` and
  `0x45CDE0` duplicate RESTORE, CANCEL and the stash fill and have **no reference in the image**
  [dead, INFERRED].
- In game, `0x45CFC0` sets `main+0x37EBE |= 1` iff `main+0x2A44 & 4` (`0x45D002`); `0x45E5E0`
  tests it to choose `VISUALRT.GUI` (`0x45E641`), skips the mode list (`0x45E69E`,
  `GUIMEMSTRUCT+0xC = 0`) and runs `0x4A0570(gi, name, 0)` on every gadget whose name starts
  `MAP` or `VID` (`0x45E83A..0x45E92C`) [role INFERRED: disable]. OnCommand's −1 branch clears
  the bit (`0x45E15D`).

**Who reads bits 1 and 5** (a full scan): bit 1 at `0x459842` and `0x459BEA` in the plain
rasteriser `0x459830`, and at `0x459C82` and `0x45A404` in the lit rasteriser `0x459C70` — the
supersampled bake paths; bit 5 only at `0x45874A` in the builder `0x4586A0`, choosing `0x459C70`
(Gouraud) over `0x459830` for units with the structure bit `0x20000000`. **All three are the
engine's own unit bake**, which no Vulkan pass presents; the DLL's packet copies the word and
uses bits 0, 2, 3 and 4. MEASURED 2026-09-23, `crowd-static` paused, 1024×768: toggling
`SHADING` or `ANTI` in the in-game screen changed **0 world pixels** under Classic++ and under
Classic (the bits verified to flip: `0x3E` → `0x1E` → `0x1C`); the panel's own pixels are the
whole difference. Bits 2 and 3 are read at `0x45928E`/`0x4594A2` and `0x459324` (the unit-shadow
gates in `0x459200`), bit 4 at `0x46A6ED`, `0x46A784`, `0x46A7E0` (feature shadows); ours in
`tagpu_native.c` (2, 3) and `tagpu_feat.c` (4). Bit 6 at `0x484A5E`/`0x484A9F` (fog), bit 7 at
`0x486C74`/`0x494935`.

**What the DLL does with it** (observers, no byte patch): `0x42F9A0` is watched — after the
first load the store is pushed (Gamma, the shadow bits, the pins, the resolved screen size);
after each later load what memory held before it is put back, so a reload never changes an
owned value, and the battleroom's size and `+gamma` — which stock saves, at `0x4462FC` and
`0x4172CE` — last only until the next launch pushes the store. `0x45CAE0` is watched and
the store pushed after it. The two slider callbacks are replaced per visit in the gadget's
`+0x144`, only while they are still the engine's, by wrappers that call them and then record the
field. The in-game `VISUALRT.GUI` is replaced by a generated one carrying `GAMMA`, `RESTORE` and
`UNDO` under their stock names and rects. The re-bake `0x437C80` is called when the Shadows row
changes the word in game, as the stock toggle does. Under `tagpu_defaults.off` none of this is installed: the
Shadows row writes bits 2–4 directly, and `RESTORE`/`UNDO` reach the stock branches.
