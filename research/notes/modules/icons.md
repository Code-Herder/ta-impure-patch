# Strategic icons

Zoomed out, a unit is drawn as a flat team-coloured icon of constant screen size, by BAR's rule;
where its model is smaller than the icon, the model is not drawn at all, and the icon answers the
pick. A mod that ships an icon set for TADR's megamap has its types drawn with that set's
pictures, read from the player's install.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`. The design and the landings: [mega map plan](../mega-map-plan.html), landings 3 and 4.

**Not yet checked in game.** The host tests pass and the DLL builds; none of the in-game checks
has been run — 1× against the stock frame, every class at the floor in both team colours, the
fade band's alphas, click, hover, band box and orders on icons, radar dots, the 4K numbers, the
cost against the models replaced, the keys changing the live view (the plan's landing 3), and for
the mod sets V4-1..V4-5: Mayhem's set at the floor in team colours and selected, ProTA's
categories and its UNKNOWN, Escalation's set from a full install and the compat setups' Escalation
with no set, TA Zero's `ZIcon` folder and retail with none, and the pick on set pictures (the
plan's landing 4). Offline, every picture the compat setups' sets ship decodes at its size through
the pure decoder (6 to 20 px); only the 100 × 10 hover circle `smallcircle` is refused.

| file | what it holds | thread |
|---|---|---|
| `inc/tagpu_icon_rule.h` | the rule (size, distance, fade, hide) and the generated shapes, pure C with their GLSL twin `TAGPU_GLSL_ICON_FN` | any |
| `inc/tagpu_icontype.h` | the classes `TAGPU_ICLS_*`, the type table's and the art's interfaces | — |
| `inc/tagpu_iconset.h`, `src/tagpu_iconset.c` | a mod set's pure half: the PCX decoder, the profile-file reader, the iconcfg's two modes and the load list, the texel classes and the atlas packer `tagpu_iconart_pack` — no file I/O, no engine name, no Win32 | any |
| `src/tagpu_packet_pub.c` (`utype_level`, `utype_level_end`, `iconart_build`, `iconset_load`, `iset_level`) | the per-type class table and each type's set picture, built once a level inside the in-play fill and carried in the packet; the art — the radar logo and a mod's set — read once a process | game |
| `src/tagpu_icon.c` | per frame: which packet unit is an icon, which model is hidden, the queue and its sort | render |
| `src/tagpu_native.c` (the unit gather), `src/tagpu_mark.c`, `src/tagpu_vk_mark.c` | the icons placed, emitted and drawn by the marker pass | render |
| `src/tagpu_zoom.c` (`icon_hit`) | the pick, inside the unit hit test's guard at `0x48CE15` | game |
| `tests/icontest.c`, `tests/icontest.py` | the rule's host test (`make icontest` is its Windows build) | host |
| `tests/iconsettest.c`, `tests/iconsettest.py` | the set's host test under ASan and UBSan, every input built by the test — no mod file is committed (`make iconsettest` is its Windows build) | host |

## The rule

**Calibration.** BAR's camera is a perspective camera with a 45° vertical field of view, and its
icon test is on the camera distance, 3000 elmos by default. At that distance the ground span is
`2 · 3000 · tan 22.5°` = 2485 elmos = 4970 TA px [INFERRED: 1 elmo ≈ 2 TA px — Spring's square is
8 elmos against TA's 16-px cell, and the Peewee is 2 × 2 in both]. TA's level `z` is screen px per
world px, so a frame `gh` engine px tall spans 4970 world px at

    z_S = gh / 4970          1080: 0.217    1440 (4K at HUD scale 150): 0.290    2160: 0.435

BAR's camera tilt is ignored [INFERRED: a top-down approximation the distance setting absorbs].

**The predicate**, per unit type, at the frame's level `z`, with the three settings in percent:

    base  = max(1, max(gw, gh) · 0.005 · 1.05 · scale/100)      BAR's UnitIconScaleUI
    S     = max(1, round(base · (0.75 · size_c + 0.25)))         BAR's size for the class
    E     = max(7, S)                                            the edge a shape is drawn at
    z_S   = gh / (4970 · dist/100)
    d'    = z_S · base / (z · S)
    b     = fade/100
    alpha = 0                          d' < 1 − b
          = 64 + 191 (d' − (1−b)) / b  b > 0 and d' < 1          the engine's fade ramp
          = 255                        otherwise
    hide  = d' > 1 and R · z < 0.45 · E

`size_c` is BAR's `icontypes.lua` size of the nearest BAR unit: COMMANDER 1.8375 (armcom), FACTORY
1.5225 (armlab), BUILDING 1.05 (armsolar), AIRCONS 1.26 (armca), AIRCOMBAT and AIR 0.861 (armfig),
CONS 0.9975 (armck), COMBAT 0.735 (armpw), UNKNOWN 1.0 (BAR's `default`). `R` is the type's model
radius in world px — the hit guard's radius, the largest corner norm of the root piece's box plus
2 ([camera and zoom](camera-zoom.html) §2.3b). `0.45 · E` is BAR's `0.9 × limit`, `limit = E/2`. A
hidden model's icon is opaque by construction (`d' > 1` gives 255). **Never an icon**: a
nanoframe (build fraction `> 0`) or a unit with no health left.

**The switch is BAR's, the cover test is the picture's.** `d'` takes BAR's size `S`, before the
drawn edge's floor, so a frame or a scale small enough for the floor to bind does not move the
level the icons take over at; only the cover test, which asks whether the icon as drawn hides the
model, takes the drawn edge `E`. The two are equal unless the frame's longer side is at most 1545
px at scale 100 (2060 at 75, 1236 at 125, 1030 at 150, 772 at 200); at scale 75 on 1920 × 1080
COMBAT is 6 px by BAR's rule and drawn at 7.

**The 7-px floor** is the smallest edge at which all nine classes' shapes are pairwise different:
under it the 1-px ink alone decides the smaller shapes (at 6 px COMMANDER is COMBAT; at 4 px the
three triangles are one outline), and a class that reads as another is a wrong answer.

The defaults (scale 100, distance 100, fade 33) — the edge, and the levels below which an icon
starts to fade in (`d'` = 0.67) and is opaque (`d'` = 1):

| class | 1920 × 1080: edge, fade-in / opaque below z | 3840 × 2160: edge, fade-in / opaque below z |
|---|---|---|
| COMBAT, radar dot | 8 px, 0.409 / 0.274 | 16 px, 0.817 / 0.547 |
| AIRCOMBAT, AIR | 9 px, 0.363 / 0.243 | 18 px, 0.726 / 0.487 |
| CONS, UNKNOWN | 10 px, 0.327 / 0.219 | 20 px, 0.654 / 0.438 |
| BUILDING | 10 px, 0.327 / 0.219 | 21 px, 0.623 / 0.417 |
| AIRCONS | 12 px, 0.273 / 0.183 | 24 px, 0.545 / 0.365 |
| FACTORY | 14 px, 0.233 / 0.156 | 28 px, 0.467 / 0.313 |
| COMMANDER | 16 px, 0.204 / 0.137 | 33 px, 0.396 / 0.266 |

**1× is the stock frame.** With the defaults, `z = 1` gives alpha 0 for every class and the radar
dot while `gh ≤ 0.67 · 4970 · min(mult)` ≈ 2600 px; the host test asserts it for every frame
480..2160 px tall at 4:3, 5:4, 16:10, 16:9, 21:9 and 32:9. With `iconfade=67` the smallest classes
fade in at 1× on a 4K-native frame; above about 2600 px tall (5K native) they fade in faintly at
1× with the defaults too.

## The class rule

Built once a level on the game thread, at the level's first in-play fill (`utype_level`, a no-op
once built for the level), one `TAGPU_PK_UTYPE` (8 B: class, layer, the type's mod set picture — `TAGPU_ICON_NOSET` unless
`iset_level` gives it one, *Mod icon sets* — and `r_px`) per
UnitDef row: `udefs = [main+0x1439B]`, stride `0x249`, rows `[0, UNITINFOCount)` with
`UNITINFOCount = [main+0x1438F]`, the count the level teardown's `0x42DB90` loops to. Row 0, the
engine's "no model", stays UNKNOWN with no radius. A unit's row is its `+0xA6`, which is both the
model id and the type row. First match wins:

1. **COMMANDER** — the def's name `+0x20` equals a side's commander name (`_strnicmp`, 0x20 bytes,
   `main+0x37F5F + 0x232·s` for `s < min([main+0x37F39], 5)`), **or** the def has both
   `showplayername` (`+0x245 & 0x20000`) and `hidedamage` (`+0x241 & 0x4000`) — TADR's rule, so a
   decoy commander is a commander to everyone.
2. immobile (`bmcode` byte `+0x22F` = 0): a builder (`+0x241 & 0x40`) that is no air base
   (`+0x241 & 0x200`) is a **FACTORY**, any other a **BUILDING**.
3. `canfly` (`+0x241 & 0x800`): a builder is an **AIRCONS**, an armed type an **AIRCOMBAT**, else
   **AIR**.
4. a builder that is no air base is a **CONS**, an armed type a **COMBAT**, else **UNKNOWN**.

*Armed* is TADR's: one of the three weapon pointers `+0x1EE`/`+0x1F2`/`+0x1F6` is a record of the
weapon array (`tagpu_limits_weapon_index ≥ 1`). The FBI bits are the engine's own
([units](../engine/units.html), *The other FBI tags a type is classed by*). The **layer** is BAR's
drawOrder: structures (FACTORY, BUILDING) 0, ground (CONS, COMBAT, UNKNOWN) 1, air (AIRCONS,
AIRCOMBAT, AIR) 2, commanders 3.

**The table rides the packet** right after the unit table (`utype_off`, `utype_n`;
`TAGPU_PK_TRUNC_UTYPE` when it did not fit). The packet's validation bounds the table and
`utype_n ≤ TAGPU_LIM_TYPES`; the consumer bounds `type_row < utype_n` and treats a class at or past
`TAGPU_ICLS_N` as no icon. `utype_n` = 0 — no table this level, or a truncated packet — means no
icons that frame: every unit is drawn as its model. A level with more types than
`TAGPU_LIM_TYPES` (16 384 under the raised limits) has no table and says so once. The packet's
reserve carries the table at `TAGPU_LIM_TYPES` rows. **The level key** is the publisher's own level
counter, and `tagpu_packet_pub_level_end` clears the table on the game thread, so neither the fill
nor the pick can use one level's table in the next.

## The shapes

A grammar, not art: the domain by silhouette, a builder by a mark.

| class | shape |
|---|---|
| COMMANDER | diamond, `|x| + |y| ≤ 1` |
| FACTORY | square, `max(|x|, |y|) ≤ 0.8`, with a plus |
| BUILDING | the square |
| AIRCONS | triangle, apex up — apex (0, −0.95), base y = 0.80, half-width 0.95 there — with a plus |
| AIRCOMBAT | the triangle |
| AIR | the triangle, hollow |
| CONS | circle, `|p| ≤ 0.92`, with a plus |
| COMBAT | the circle |
| UNKNOWN | the circle, hollow |

**Ink**, the outline, is a texel inside whose neighbour `w` device px away on either axis is
outside, `w = max(1, round(edge/12))`; the **plus** is ink, two arms to 0.45, at least one device
px and 1/7 wide; a **hollow** shape keeps a ring, the texels whose exact distance to the
silhouette's edge is at most `max(0.45 × its inradius, 1 device px)` — the circle's radius 0.92,
the triangle's incircle 0.5652 — and its centre is outside. Tied to the inradius, the hole is
55 % of the incircle wherever the 1-px floor does not bind; the circle's opens from 4 px, the
triangle's from 5. The fill is the owner's team colour, **white when selected** — `+0x110` bit 4
under the engine's SelBoxes gate (the packet's `ui_gates` bit 2), the selection rect's own test —
and the ink the GUI's black.

**Decided per fragment** from the quad's own `(±1, ±1)` and its `fwidth`, so a shape needs no atlas
and no re-raster when the size or the supersampling changes. `tagpu_icon_shape()` and the marker
pass's `taIconShape` (`TAGPU_GLSL_ICON_FN`) are the same function line for line; the host test
checks the C one (symmetry, ink on every border texel, the plus only on builders, a hollow centre
open and a solid one closed, the nine classes pairwise different from 7 to 128 px, and the 16-px
rasters' FNV `0x4EC8F4CD`).

## Mod icon sets

Several mods ship an icon set for TADR's megamap: an `iconcfg.ini` and a folder of small 8-bit
PCX pictures in the TA palette. Impure reads the same files from the player's install at run time
and draws them itself: TADR's code never runs ([keeping TADR out](tadr-exclusion.html)), and no
picture ships with Impure. Which installs carry one: [the setups](../compat/setups.html), *The mega
map's icon sets*.

**Finding it** (`iconset_load`, once a process, on the game thread at the first in-play fill,
before the art's release store):

1. **The TADR ini's name** is the string at `0x5098A3` in the running exe ([process](../engine/process.html),
   *The options file's name*): `totala.ini` in retail, rewritten by the Community Patch Loader at
   start-up to the mod's, so by the first in-play fill it is the mod's. It is taken only as 1..32
   characters of `[A-Za-z0-9_.-]` and a NUL within 33 bytes; anything else is no set.
2. `<exe dir>\<name>`, at most 64 KB → `[Preferences] MegaMapConfig`. Absent, empty, too long, or
   not relative (a drive `:`, or a leading `\` or `/`) → no set, so only the install's own files
   are read. TADR resolves the value against the current directory; the exe's folder is used here
   [INFERRED equal under every launcher in use: tacli and the loaders start TA in its folder].
3. **The iconcfg**, at most 64 KB, at `<exe dir>\<value>`; the pictures' folder is that path cut at
   its last `\` or `/`.

**Reading the files** the way the Win32 profile API does: section names and keys compared ASCII
case-insensitively; whitespace trimmed around the name, the key and the value; a line whose first
non-blank character is `;` skipped; a section line's name ends at its first `]`, so
`[Option] ; General Settings`, which every shipped set has, is `Option`; only the first section of
a name is read, and a key's first line in it; a value cut at its first `;` and trimmed again; a key
over 63 bytes or a value over 259 refused, never cut. `[Option]`'s `FillColor`,
`TransparentColor` and `SelectedColor` are their leading decimal digits when those make 0..255,
else TADR's defaults 0, 9 and 89; `UseDefaultIcon` is TRUE when its lower-cased value contains
`true`, and absent is TRUE (TADR's default). `HoverColor` and `UseCircleHover` are not read.

**The load list**, in TADR's order, which is the order a type's picture is chosen in:

- each side's `<side name>.PCX`, for `s < min([main+0x37F39], 5)`: the name at
  `main+0x37F3D + 0x232·s`, read within its 0x1E bytes and taken only as a file name — printable,
  none of `\ / : * ? " < > |`, ending within the record;
- **default mode** (`UseDefaultIcon` TRUE): `COMMANDER`, `AIRCRAFTCOMBAT`, `AIRCONS`, `CONS`,
  `MOBILECOMBAT`, `FACTORY`, `BUILDING`, `NONE`, `UNKNOWN`, `NUKEICON` (`.PCX`);
- **custom mode**: every `[Icon]` line in file order, the value the file — the key `Nothing` is
  NONE, `Unknow` UNKNOWN, `NukeIcon` NUKEICON (case-insensitive), any other key the category it
  names. A line whose value is empty or not relative is refused and counted.

At most 64 pictures, each file at most 4 KB; one that does not read or decode is skipped and named
in the log. When NONE or UNKNOWN loads twice, the last one is the set's, as in TADR.

**The PCX decoder** (`tagpu_pcx_decode`) takes ZSoft's header (`0x0A`, RLE, 8 bpp, one plane), a
picture of 1..22 px a side (TADR's `ICONMAXWIDTH/HEIGHT`) whose row stride `bytes_per_line` is at
least its width and at most 1024, and decodes `h × bytes_per_line` bytes as one stream: a run may
cross a row, only the first `w` bytes of each row are kept, a run that passes the image's last byte
ends there, and a stream that ends first is refused. Texels are TA palette indices as they are;
the trailing palette is ignored. TADR decodes `w × h` bytes and swaps the two, which gives the same
pixels for the square pictures every set ships; this decoder honours the stride. The shipped
pictures are 6 to 20 px; a set's 100 × 10 hover circle `smallcircle` is past the bound, and no
shipped list names it.

**Into the atlas.** Each picture's texels are classed in TADR's precedence — TransparentColor is
nothing, else FillColor is the team colour, else SelectedColor is shown only when selected, else
the texel is drawn as its own index (`tagpu_iconset_classify`) — and packed with the radar logo
frames into one atlas (*Radar dots*). The art keeps the set's SelectedColor as `selcol`.

**Each type's picture** (`iset_level`, once a level inside the in-play fill, right after the class
rule): the first entry in load order whose types hold the type's row, else the set's UNKNOWN, else
none — the type keeps its generated shape (TADR would index its list at −1). What an entry holds
is TADR's rule (`ExternQuickKey.cpp`):

| entry | its types |
|---|---|
| a side's picture | that side's commanders: `showplayername` ∧ `hidedamage` (decoys included) whose `Side` (def `+0xA0`, read within its 0x1E bytes: the FBI reader stores `side` there with a cap of 0x1E, `push 0x1e` at `0x42ACB0`) equals the side's name, ASCII case-insensitively (TADR copies both into 8 bytes with `strcpy_s` first, so it compares names of up to 7 characters) |
| COMMANDER | every such commander |
| AIRCRAFTCOMBAT | `canfly` ∧ a weapon that is no stockpile (WeaponTypeMask `+0x111` bit 28 clear, read only from a record of the weapon array) ∧ ¬builder ∧ no yard map ∧ ¬commander |
| AIRCONS | `canfly` ∧ builder ∧ no yard map ∧ ¬commander |
| CONS | the types of category `CTRL_B` when it holds any, else builder ∧ ¬isairbase ∧ mobile (`bmcode` ≠ 0) ∧ ¬commander |
| MOBILECOMBAT | the types of `CTRL_W` ∧ ¬canfly |
| FACTORY | `CTRL_F`'s types when it holds any, else builder ∧ ¬isairbase ∧ immobile ∧ ¬commander |
| BUILDING | a yard map (the def's pointer `+0x14E` non-NULL; never dereferenced) ∧ ¬commander |
| a custom line | the types of the category its key names |
| NONE, UNKNOWN, NUKEICON | no type |

A category with no type is the same as an absent one.

**The categories are the engine's own map, walked read-only** ([units](../engine/units.html), *The
unit-category map*): `[*(u8**)0x51E6B4, *(u8**)0x51E6B8)`, 8-byte `{name, mask}` entries, a type's
bit its row. The bounds must be equal (no categories) or a whole number of entries between two
pointers `ptr_ok` accepts, at most 65 536; an entry whose name or mask `ptr_ok` refuses is skipped.
The walk is linear and compares names with `_stricmp`'s ASCII fold, so it holds whatever order the
map is in; an engine name is read only to its NUL or the first byte that differs from ours, within
64. A mask bit is read only for a row below `UNITINFOCount`, which the class table has already
bounded by `TAGPU_LIM_TYPES`, the mask's bits under Impure's raise. **`0x488C50` is never called**:
on a miss it inserts, so a category the map lacks is simply absent.

*Why the walk is safe*: the map and its masks are freed only by the teardown (`0x488BF0`, whose one
caller is `0x491C3A`, game thread), and during play every inserter — the Ctrl keys, the SHARE give,
the console's `Reload` and the AI plan's `weight`/`limit` reached through the console — runs on the
game thread; the walk runs on the game thread inside the in-play publish, so neither happens during
it — a lifetime by thread ownership. The level's load filled the map on the loader thread, the AI
plan's inserts included, before `main+0x38D75` bit 1 (`0x497C5F`), which the game thread read before
its first in-play frame. `ptr_ok` is a filter on values, not the argument. The walk keeps no entry
pointer past its own call: an insert can move every entry ([units](../engine/units.html), *The
unit-category map*). What it takes is a snapshot for the level.

**Drawing a set picture.** A type whose `icon` names one of the published art's pictures is drawn
with it (`tagpu_icon_type`, `inc/tagpu_icon_rule.h`): at the art scale `k = max(1, round(base /
10))` — 1 at 1920 × 1080 and 2 at 3840 × 2160 with the defaults — its own aspect kept, shape 0.
`icon` is DATA: an index at or past the art's picture count is the class's shape. The rule takes
`S = E = k × the picture's longer side` (art has no 7-px floor), so where a set picture takes over
follows its drawn size, not the class's BAR size. On the atlas the TransparentColor texels are
discarded, the FillColor texels take the owner's team colour — **white when selected** — and the
SelectedColor texels are discarded unless the unit is selected, when they show in the set's own
SelectedColor (`uIconSelCol`, *The draw*). TADR keeps the team fill on a selected unit and shows the
frame; this whitens the fill, as BAR does, and shows the frame. The render thread (`tagpu_icon.c`,
the picture and its fade) and the game thread's pick (`tagpu_zoom.c`, whether the model hides and
the circle's width) both ask `tagpu_icon_type`, so they agree on every size.

With no set, every type's `icon` is `TAGPU_ICON_NOSET` and every path is the generated shapes'.

## Radar dots

**Stock draws a radar contact on the minimap alone** ([ui](../engine/ui.html), *The minimap,
located*), so ours follow the same distance rule and 1× stays stock. A unit the gather refuses by
the cloak test or the fog test (an unexplored tile, or a grey one for a foreign unit) that is a
radar or sonar contact (`+0x110 & 0x300`) with a model is drawn as **the minimap's own dot**: its
owner's logo frame of `radlogo` (`main+0x147DF`), at an integer scale `k_r = max(1, round(0.8 ·
base / 4))`, under the rule with `S = E = k_r × max(w, h)` (8 px at 1080p, 4 px at the smallest
scale; the 7-px floor is the generated shapes', not the art's), on layer 1 (BAR's unidentified
contact). **With a mod set that has a NONE picture** — the mod's art for an unidentified contact —
the contact is drawn as that picture instead, at the set's art scale `k` (*Mod icon sets*), its
FillColor in the owner's team colour, under the rule with `S = E = k × max(w, h)`. A contact hides
nothing, is never white and is not pickable in the world view — the engine's own rule (`0x48CD80`
picks a radar contact on the minimap only). No fog source, no foreign unit and no contact.

**The art** — the radar logo and a mod's set — is built once a process at the first in-play fill
(`iconart_build`, which calls `iconset_load` before it packs): `radlogo` is
session art, loaded by `0x429870` from start-up's `0x49134D`, and `0x42991E` is the only write of
its pointer ([weapons](../engine/weapons.html), *The effect GAF sequences are a SESSION asset*). Up
to 16 frames of at most 16 × 16, then a mod set's pictures (up to 64, at most 22 × 22), go into a
256 × 256 atlas of palette indices through `tagpu_iconart_pack` (`src/tagpu_iconset.c`): left to
right a texel apart, in rows from texel `(1, 1)`, a new row under the tallest of the last. The packer
refuses any layout that would leave the atlas, and a compile-time assert in `tagpu_packet_pub.c`
shows the worst case cannot: a picture advances a row by at most a 23-texel cell, so a row holds at
least 11 and is at most 23 tall, and 80 pictures take 8 rows, 185 texels of the 256. **Each logo's
team colour is its frame's texel at the hotspot** (the commonest drawn texel when the hotspot draws
nothing), never a table, because mods recolour it. The atlas's key, fill and selected markers are
the three smallest indices no drawn texel of any picture holds — a set's TransparentColor,
FillColor and SelectedColor texels are rewritten to them; a set that leaves fewer than three free
is dropped and the logos are packed alone, and logo art that draws every index leaves no art. A unit whose logo has no frame is filled in the
GUI's black; with no `radlogo` there are no radar dots and the shapes still draw.

## What an icon hides

A unit whose model is **hidden** gets no NU at all — no model, no shadow, no selection rect (BAR
draws no footprint for an icon) — and the marker pass skips its health bar and group digit. The
**cargo of a hidden unit** — a unit in a transport, a nanoframe on a factory's pad — is part of its
carrier's picture: neither a model nor an icon, and nor is the cargo of that cargo. The render
thread marks the cargo lists (`cargo_first` / `cargo_next`, every hop a packet index the publisher
bounded and re-checked against the frame's count) of every hidden or already-suppressed unit, in up
to 64 rounds (`TAGPU_ICON_CARGO_HOPS`), each list followed at most 64 members, the engine's own
guard. Only a unit the native pass draws (`TAGPU_PK_U_NATIVE`) is ever an icon, hidden or
suppressed, and only such a unit passes the suppression on: a unit the engine draws keeps its
sprite, its bar and its digit, and hides no cargo. Cargo of a carrier whose model shows is decided
by its own rule. A unit with an icon whose model is not hidden draws both, the icon over it, while
the alpha ramps.

**When icons are decided at all.** Only while the unit pass draws the units and the marker pass
draws the icons (`tagpu_mark_draws`: armed and not passive) — a model is never hidden under an
icon nothing would draw — on an in-game packet that carries a type table, once the art is
published, with at most `TAGPU_PK_DESIGN_SLOTS` units in the packet (more: no icons that frame,
said once). A unit `tagpu_native_owns_unit` refuses keeps the engine's sprite and gets no icon. A
unit with no pose in the packet can still be an icon: the gather places its anchor and runs its
slab, fog and cloak gates before the no-pieces exit.

## The draw

**Order** — BAR's, with its depth sort on: by layer, then far to near (a smaller anchor y is
further up the screen), then packet index; the sort key is `layer << 48 | sortable(ay) << 16 |
index`. **Place** — the marker pass's second draw, right after the selection rects: over the world,
under the orders, the bars, the digits and every UI layer, not fogged (the gather placed each over a
unit it judged visible, or a radar contact), not depth-tested, blended at the icon's alpha.

`tagpu_mark_emit_icon` puts one quad of `edge` screen px on the unit's anchor, its corner on the
device grid (`snap_device`), into a bucket of one quad a packet unit. **The bucket is grown before
any icon is decided** (`tagpu_mark_icon_room`, asked by the native pass in the same condition as
`tagpu_mark_draws`): a frame on which it would not grow decides no icon and hides no model, and a
frame queues at most one icon a unit, so an icon of a frame that hid a model always has its quad. The fill colour, the
selection, the alpha and the shape **ride the vertex's world slot as two integers under 2¹²** —
`team + 256 · selected` and `alpha + 256 · shape` — so the interpolated value rounds back to the
integer at every fragment. The marker FS's `uText == 2` is the icon branch (text is `uText == 1`):
shape 0 samples the icon atlas (the key discarded, the selected marker discarded unless selected
and then drawn as `uIconSelCol`, the fill marker taking the team colour or white), any other shape
is `taIconShape`. Its five uniforms `uIconFill`, `uIconSel`, `uIconWhite`, `uIconInk` and
`uIconSelCol` sit at 20, 24, 28, 32 and 36 of FS's block, which stays 48 bytes (`FS_SZ`):
`uIconSelCol` is the art's `selcol`, a mod set's SelectedColor, handed over with the other markers
(`TAGPU_MKHAND.iconSelCol`). The line program's `uGrid`, an `ivec4`, starts at 32, so
`tagpu_vk_mark.c` writes `uGrid` into line draws' blocks alone and an icon draw's block holds
`uIconInk` and `uIconSelCol` in its first two words. The atlas is the image `IMG_ICON`, in a fourth descriptor set per slot, uploaded
once per slot (it never changes after its publication); an icon draw without it draws nothing and
is logged once. `TAGPU_MK_MAXDRAW` is 24: a full frame records 16 draws, and a draw past the cap
drops the whole marker layer for the frame.

## The pick

`tagpu_zoom_hit_guard`, the call `0x48CE15` inside `GetUnitAtMouse 0x48CD80`, first asks
`icon_hit` ([camera and zoom](camera-zoom.html) §2.3b). At the game thread's applied level
(`game_level()`), with the game thread's own class table and the screen size `main+0x37E1F` /
`+0x37E23`:

- **Carried** (`+0x86` ≠ 0), asked first: **the picture's cargo rule** (*What an icon hides*),
  walked up instead of down. Each carrier counts only as a record of the unit array — at or after
  `[main+0x14357]`, a multiple of `0x118` from it and below the slot count `u16 main+0x14351`
  (else `-1`); the walk stops at a carrier `tagpu_native_owns_unit` refuses and after 64 hops
  (`TAGPU_ICON_CARGO_HOPS`, the render's bound); a carrier whose model is hidden makes the answer
  "not hit", whatever the unit is — a nanoframe on a hidden factory's pad included, which nothing
  draws (BAR gives cargo no icon; the band box `0x48C390` already skips carried units). The two
  walks read the same links from either end [INFERRED: `+0x86` names the unit whose cargo list
  `+0x8A`/`+0x8E` holds it]. Otherwise the unit's own rule, as below.
- `-1`, the guarded engine test as before: the latest command record drew no icons, no table, a
  unit `tagpu_native_owns_unit` refuses, a row past the table, a nanoframe (`+0x104 > 0`), no
  health (`+0x108 ≤ 0`), or a model the rule does not hide.
- **Hidden**: hit when `(px − ax)² + (py − ay)² ≤ (0.45 · E / z)²`, with `ax = ((x − (eyeX << 16))
  >> 16) + 0x80` and `ay = ((z − (eyeY << 16)) >> 16) − ((alt >> 16) >> 1) + 0x20` — `0x48C6A0`'s
  own anchor without its corner offsets, in 64 bits. `0x48C6A0` is never called for such a unit, so
  its int16 narrowing cannot wrap. `E` is the edge `tagpu_icon_type` gives the picture: the
  class's drawn edge, or `k ×` the longer side of the type's set picture in the art this thread
  published, the type's `icon` bounded by the art's picture count.

*Why it is safe by construction*: every value comes from the unit's own record, its carriers' and
the main block, read on the game thread that owns them inside the engine's own call; the row is
bounded by the table's count and every carrier by the unit array. HotUnits is the cull and is
untouched (one id per slot, `0x485596`). The pick follows the game thread's applied level and the
picture the frame's level; the two are equal at rest and differ only while a zoom tween moves —
a property, not a timing argument: nothing is read across threads. **The icons it answers by are
those the picture drew**: the render thread draws them only while the unit pass and the marker
pass both draw, and the command record that ends each render frame carries that frame's answer
(`TAGPU_CMD.icons`, a level like `zoom`, checked to 0 or 1 by `cmd_valid`; 0 from a frame that
left before the native pass). The apply copies the latest record at the top of each in-play draw,
and `GetUnitAtMouse` runs on the game thread from the in-game handlers (`0x499278`, `0x491D18`),
so a click is answered with the answer of the frame whose record the last apply took — the frame
the player saw, or the one before it at a switch, the same one-record lag the level has — and
never by an icon no frame drew. Only a record of the current level's epoch counts (`c->epoch ==
s_epoch`, the epoch the level end bumps), and `tagpu_zoom_level_end` clears the answer, so a record
posted before the render thread saw a level end never answers for the next level; with no such
record yet, the engine's test. Hover sets `main+0x2CBA`
through the same answer; the band box `0x48C390` tests each unit's anchor, the point an icon is
centred on, and needs no change.

## Settings

Three closed-list `impure.cfg` keys ([renderers](../renderers.html) §2.10b), read live by both
threads — every frame on the render thread, every pick on the game thread — so none needs a
restart. An unknown value in the file leaves the default and is logged; under
`tagpu_defaults.off` (every tacli control launch without `--defaults`) the defaults hold.

| key | values (default first) | meaning |
|---|---|---|
| `iconscale` | `100` `75` `125` `150` `200` | the icon size, percent of BAR's (`UnitIconScaleUI` 1.05) |
| `icondist` | `100` `50` `75` `150` `200` | where icons take over, percent of BAR's 3000 elmos; 200 = icons only from twice as far |
| `iconfade` | `33` `0` `67` | the fade band `b`, percent; 0 = BAR's hard switch, 67 = Recoil's own 1000/3000 |

**No menu rows**: the front-end screen has no free slot, and three rows would run the in-game
panel 46 px past a 640×480 screen ([defaults and settings](settings.html)).

## Threads

- **The type table** is built and read on the game thread (the publisher and the pick) and reaches
  the render thread only as values inside the packet; each type's set picture is one of them, an
  index both threads bound by the art's picture count.
- **The mod set** is read on the game thread inside the first in-play fill, before the art's
  release store: the ini name at `0x5098A3` and the sides' names in the main block, then the files,
  each bounded before it is read and DATA to the pure parsers. The load list it keeps is the game
  thread's own, read again only by the per-level walk on the same thread.
- **The category walk** runs on the game thread inside the in-play fill, once a level: a lifetime
  by thread ownership (*Mod icon sets*).
- **The art** is written once on the game thread before a release store
  (`__atomic_store_n(&s_iconArtReady, 1, __ATOMIC_RELEASE)`) and never written or freed after it;
  `tagpu_icontype_art()` takes it with an acquire load, so a render-thread pointer taken after it
  stays valid and unchanging for the process — the order art's argument.
- **`tagpu_icon_frame`** runs once a frame on the render thread before the unit gather and the
  marker gather, which read its arrays in the same frame on the same thread: program order is the
  ordering. `tagpu_icon.c` reads only the packet and the published art.
- `thread-split.allow` names the new game-thread reads inside the existing `publisher` lines of
  `tagpu_packet_pub.c` (the type table, the radar logo, the TADR ini's name, the sides' names, the
  install's files and the category walk with the def fields it reads) and `tagpu_zoom.c` (the
  pick's reads); `tagpu_iconset.c` is pure and has no line;
  `tagpu_icon.c` reads no engine memory and has no line. Presentation only: nothing writes
  simulation state, and no `sim` file is touched.

## Logs

| line | when |
|---|---|
| `icons: on= units= icons= set= hidden= cargo= dots= drop= base= zS= z= alpha(combat)= alpha(dot)= edges=<9> dotk= by-class=<9> us=<frame>+<emit>` | every 300 render frames, from the marker gather; `set=` the icons drawn as a mod set's picture (radar contacts not counted), `dots=` the radar contacts, as a dot or the set's NONE; the nine are in `TAGPU_ICLS_*` order — UNKNOWN, COMMANDER, FACTORY, BUILDING, AIRCONS, AIRCOMBAT, AIR, CONS, COMBAT, and count set pictures by their type's class. At 1× with the defaults `icons=0 hidden=0` |
| `icons: mod set <folder> from <ini> (<MegaMapConfig>), <mode> mode, N picture(s) loaded of M, R [Icon] line(s) refused, skipped: <files>` | once a process, when a set's iconcfg reads; `<mode>` is `default` or `custom`, `M` the load list's length, `<files>` the pictures that did not read or decode, or `none` |
| `icons: no mod set - the exe names no TADR ini`, `… - <ini> names no MegaMapConfig of the install's own`, `… - <ini>'s MegaMapConfig <value> does not read` | once a process, instead of the line above |
| `icons: the mod set leaves no three free palette indices beside the radar logo - not drawn` | once a process: the set is dropped and the logos packed alone |
| `icons: art N radar logo frame(s) of M, S mod set picture(s), key K fill F sel S` | once a process, at the art's publication |
| `icons: mod set by type - E map entr(ies), P of T type(s) by a picture, U UNKNOWN; CTRL_W=a CTRL_B=b CTRL_F=c; <category>=<types> …` | once a level with a set loaded: the category map's entries, the types (rows past 0) given a picture and those given UNKNOWN, the three Ctrl categories' type counts, and each custom line's category with its type count, or `absent` for one the map does not hold |
| `icons: the category map's bounds are not a map - no categories this level` | a level whose map bounds the walk refuses |
| `icons: N unit types, past the table's T - no icons this level` | a level with too many types |
| `icons: N units in the packet, past the T the icons hold - no icons while that is true` | once |
| `zoom: icon pick - N hit(s), M miss(es) so far` | at most once a second, on the game thread |
| `mark: … icons= iconover= …` | the marker pass's `log` line: icon quads, and those the bucket refused |

## What it does not do

- **No menu rows** for the three keys until a panel layout fits 640×480.
- **No hover indication** on an icon (BAR has none; a set's `HoverColor` and `UseCircleHover` are
  not read).
- **A mod set needs `MegaMapConfig`.** In default mode with no `MegaMapConfig`, TADR looks for the
  fixed pictures in the current directory; here there is no set. The iconcfg is resolved against
  the exe's folder, not the current directory [INFERRED the same under every launcher in use], and
  an absolute `MegaMapConfig` is refused.
- **A value is cut at its first `;`**; TADR cuts `MegaMapConfig` at its last, so a value holding
  two `;` reads differently.
- **The category walk runs once a level**, so its picture per type is a snapshot: a type the
  console's `Reload` adds to a category in play takes its picture from the next level
  ([units](../engine/units.html), *The unit-category map*). The empty categories the keys, the SHARE
  give or a console `weight`/`limit` can add hold no type, so they change no picture.
- **Side pictures are read at the first in-play fill of the process**: the side names then are
  the set's for the process.
- **Overlapping icons**: the engine's own tie-break picks among hits (the smallest key over def
  `+0x176`/`+0x17A`/`+0x17E`, `0x48CE1E..0x48CE75`), which need not be the icon drawn on top.
- **An icon whose unit the HotUnits cull does not list** is not pickable, so the strip of an icon
  hanging over the view's edge from a unit outside the addressable rect answers nothing.
- **A unit the native pass does not own** keeps the engine's sprite and gets no icon.
- **A Vulkan marker frame refused after the icons were decided** loses its icons while the models
  they hide stay hidden: the marker pass's own refusals — in `tagpu_vk_mark_prepare` a draw out of
  its vertex block, an icon draw with no atlas or an image that would not upload, in `mk_draw` a
  draw past `TAGPU_MK_MAXDRAW` — drop the whole marker layer of that frame, which is a failed frame — the
  orders, bars and cursor are missing too — logged once a site, not a state the icons can prevent
  from the gather.
- Nuke projectiles get no icon, so a set's NUKEICON is loaded and never drawn; a building out of radar leaves no ghost icon (BAR keeps one);
  BAR's "real icon once identified" for a radar contact has no TA counterpart.
