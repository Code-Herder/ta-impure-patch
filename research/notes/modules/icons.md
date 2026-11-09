# Strategic icons

Zoomed out, a unit is drawn as a flat team-coloured icon of constant screen size, by BAR's rule;
where its model is smaller than the icon, or at once under a hollow shape, the model is not drawn
at all, and the icon answers the pick. A mod that ships an icon set for TADR's megamap has its types drawn with that set's
pictures, read from the player's install. A radar contact is drawn as the minimap's dot, at every
level by default, and is hovered and ordered on in the world view as it is on the minimap.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`. The design and the landings: [mega map plan](../mega-map-plan.html), landings 3 and 4.

**Checked in game**: the [plan](../mega-map-plan.html)'s checks of its landings 3 and 4 (1× against
the stock frame, every class at the floor, the fade band's alphas, orders on icons, the 4K numbers,
the keys, and every mod set's pictures), and the radar dots at every level and under `zoomed`, the
hover ring and an attack order on a contact at 1920 × 1080 and 3840 × 2160
(`scenarios/radar-dots.json`, *Radar dots*). **Not run in game**: a contact drawn as a mod set's
NONE picture, hovered or ordered on, and the radar pick in a multiplayer game. Offline, every
picture the compat setups' sets ship decodes at its size through the pure decoder (6 to 20 px);
only the 100 × 10 hover circle `smallcircle` is refused.

| file | what it holds | thread |
|---|---|---|
| `inc/tagpu_icon_rule.h` | the rule (size, distance, fade, hide) and the generated shapes, pure C with their GLSL twin `TAGPU_GLSL_ICON_FN` | any |
| `inc/tagpu_icontype.h` | the classes `TAGPU_ICLS_*`, the type table's and the art's interfaces | — |
| `inc/tagpu_iconset.h`, `src/tagpu_iconset.c` | a mod set's pure half: the PCX decoder, the profile-file reader, the iconcfg's two modes and the load list, the texel classes and the atlas packer `tagpu_iconart_pack` — no file I/O, no engine name, no Win32 | any |
| `src/tagpu_packet_pub.c` (`utype_level`, `utype_level_end`, `iconart_build`, `iconset_load`, `iset_level`) | the per-type class table and each type's set picture, built once a level inside the in-play fill and carried in the packet; the art — the radar logo and a mod's set — read once a process | game |
| `src/tagpu_icon.c` | per frame: which packet unit is an icon, which model is hidden, the queue and its sort | render |
| `src/tagpu_native.c` (the unit gather), `src/tagpu_mark.c`, `src/tagpu_vk_mark.c` | the icons placed, emitted and drawn by the marker pass | render |
| `src/tagpu_zoom.c` (`icon_hit`, `tagpu_zoom_radar_pick`) | the pick, inside the unit hit test's guard at `0x48CE15`; the radar dot under the pointer | game |
| `src/tagpu_route.c` (`hover_handler`) | the hover query's call `0x499278`, which asks the radar pick when the engine found no unit | game |
| `src/tagpu_settings.c`, `src/tagpu_menu.c` | the four keys (*Settings*); the in-game panel's *Radar dots* row | any |
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
    hide  = d' > 1 and R · z < 0.45 · E      R = 0 for a hollow generated shape

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

**1× is the stock frame**, but for the radar contacts `radardots=always` draws there (*Radar
dots*). With the defaults, `z = 1` gives alpha 0 for every class, and for the radar dot under
`radardots=zoomed`, while `gh ≤ 0.67 · 4970 · min(mult)`; with `S` rounded to whole px the first
alpha at 1× comes at 2569 to 2666 px tall by the frame's shape (a radar contact at 5:4, COMBAT at
4:3 first). The
host test asserts alpha 0 for every frame 480..2560 px tall at 4:3, 5:4, 16:10, 16:9, 21:9 and
32:9, for the shapes and for every set picture size (*Drawing a set picture*). With `iconfade=67`
the smallest classes fade in at 1× on a 4K-native frame; above about 2570 px tall (5K native)
they fade in faintly at 1× with the defaults too.

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

**Ink**, the outline, is a texel inside whose neighbour `w` game px away on either axis is
outside, `w = max(1, round(edge/12))`; the **plus** is ink, two arms to 0.45, at least one game
px and 1/7 wide; a **hollow** shape keeps a ring, the texels whose exact distance to the
silhouette's edge is at most `max(0.45 × its inradius, 1 game px)` — the circle's radius 0.92,
the triangle's incircle 0.5652 — and its centre is outside. Tied to the inradius, the hole is
55 % of the incircle wherever the 1-px floor does not bind; the circle's opens from 4 px, the
triangle's from 5. The fill is the owner's team colour, **white when selected** — `+0x110` bit 4
under the engine's SelBoxes gate (the packet's `ui_gates` bit 2), the selection rect's own test —
and the ink the GUI's black.

**Decided once per game pixel**, at its centre, from the quad's own `(±1, ±1)` and its edge (the
vertex's colour slot), so a shape needs no atlas and no re-raster when the size or the supersampling
changes, and is the same picture at every supersampling: no line of ink is averaged with the fill. `tagpu_icon_shape()` and the marker
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
   not a path inside the folder (a drive `:`, a leading `\` or `/`, or a segment that climbs: `..`,
   or, as Win32's path parser trims trailing dots and blanks, any segment of only dots and blanks with
   two dots or more) → no set, so
   only the install's own files are read; an `[Icon]` line's file passes the same test. TADR resolves the value against the current directory; the exe's folder is used here
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
  names. A line whose value is empty or not a path inside the folder (as above) is refused and counted.

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
frames and the hover ring into one atlas (*Radar dots*). The art keeps the set's SelectedColor as `selcol`.

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
caller is `0x491C3A`, game thread) and by the process-exit destructor `0x488A00`, which the map's
static initialiser registers at `0x4889D5` (through `0x4E4AC0` [INFERRED: the CRT's `atexit`]) and
which runs only as the process leaves, after its last in-play frame; and during play every inserter — the Ctrl keys, the SHARE give,
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
`icon` is DATA: an index at or past the art's picture count is the class's shape. **The switch is
the class's**: `S` is BAR's size for the type's class, exactly as for its shape, so where a type
turns into an icon does not depend on which picture the mod drew, and 1× stays stock for every
picture size a set can hold (the host test runs 6..22 px a side). Only the cover test and the pick
take the picture as drawn, `E = k × its longer side` (art has no 7-px floor). On the atlas the TransparentColor texels are
discarded, the FillColor texels take the owner's team colour — **white when selected** — and the
SelectedColor texels are discarded unless the unit is selected, when they show in the set's own
SelectedColor (`uIconSelCol`, *The draw*). TADR keeps the team fill on a selected unit and shows the
frame; this whitens the fill, as BAR does, and shows the frame. The render thread (`tagpu_icon.c`,
the picture and its fade) and the game thread's pick (`tagpu_zoom.c`, whether the model hides and
the circle's width) both ask `tagpu_icon_type`, so they agree on every size.

With no set, every type's `icon` is `TAGPU_ICON_NOSET` and every path is the generated shapes'.

## Radar dots

**Stock draws a radar contact on the minimap alone** ([ui](../engine/ui.html), *The minimap,
located*), and hovers and orders on it there alone ([input](../engine/input.html), the `0x48CD80`
row). Ours draw it in the world view as well, as **the minimap's own dot**: at every level under
`radardots=always`, the default, and under `radardots=zoomed` by the icons' distance rule, which
keeps 1× stock (*Settings*). A unit the gather refuses by the cloak test, the engine's draw-list test (`TAGPU_PK_U_UNSEEN`,
[units](units.html), *Which units the pass draws*) or the fog test (an
unexplored tile, or a grey one for a foreign unit) that is a radar or sonar contact (`+0x110 &
0x300`) with a model is drawn as its owner's logo frame of `radlogo` (`main+0x147DF`), at an
integer scale `k_r = max(1, round(0.8 · base / 4))` and `E = k_r × max(w, h)` (8 px at 1080p and
16 at 4K native, `k_r` 2 and 4; 4 px at the smallest scale; the 7-px floor is the generated
shapes', not the art's). It is the same call, art and scale at every level, so the dot at 1× and
in the mega map is one picture. **Under `always`** its alpha is 255. **Under `zoomed`** it switches
at `S = round(0.8 · base)`, the size `k_r` is rounded from (`tagpu_icon_dot_size`): equal to `4
k_r` at 1080p and 4K native, and never the rounded-down scale, which would bring contacts in at 1×
on a 3186 × 2390 frame; at 1920 × 1080 it fades in below z 0.409 and is opaque below 0.274 (the
COMBAT row of *The rule*'s table). On layer 1 (BAR's unidentified contact). **With a mod set that
has a NONE picture** — the mod's art for an unidentified contact — the contact is drawn as that
picture instead, at the set's art scale `k` (*Mod icon sets*), its FillColor in the owner's team
colour, `E = k × max(w, h)`, under `zoomed` switching at the same contact `S`. A contact hides
nothing. No fog source, no foreign unit and no contact.

**The hovered contact** — the unit the hover query named, `main+0x2CBA`, carried in the packet
header as `hovered_id` — is drawn as the minimap draws it (`0x466F11..0x466F37`): frame 0 of
`radlogohigh` (`main+0x147E3`, a 6 × 6 ring, hotspot (2, 2), on the retail `fx.GAF`) over its
dot, on the dot's anchor, at the dot's alpha, after every other icon of the frame, in its own
palette indices. The ring's hole is its frame less a texel each side (4 × 4 about the logo's
4 × 4), and it takes the smallest integer scale whose hole holds the contact's picture as drawn:
the dot's own `k_r` for the logo, larger for a set's NONE picture (`tagpu_icon_push`). Whichever
way the unit was hovered — on the minimap or on the dot — both pictures show the ring. A contact
is never drawn as a selected picture is, so it is never white. One unit is
hovered at most, so a frame queues at most one ring, and the icon bucket is grown for one quad
more than the packet's units (`tagpu_mark_icon_room(n_units + 1)`).

**A dot is hovered and ordered on as the minimap's is.** The hover query's call `0x499278` asks
the radar pick when the engine found no unit (*The pick*); the id it answers is stored by the
engine at `0x499283` as its own would be, and the engine's cursor chooser `0x48D220` and click
`0x48CF30` take the contact from there — the attack cursor with an armed unit selected, and an
attack on the unit — with no order code of ours. MEASURED 2026-11-09 (`scenarios/radar-dots.json`,
1920 × 1080, Vulkan, `--defaults`, True LOS): 4 to 6 dots at z 1.0, 2.0, 0.6, 0.4 and the mega
map's floor 0.135; the pointer on a contact's dot gave `main+0x2CBA` = its id and the ring; with
a Peewee selected the cursor index `main+0x2CBE` read 1 (attack), and the press — the right
button under Interface Type 1, the left under 0 — set the Peewee attacking the contact (under
Type 1 it walked to the solar and destroyed it). Under
`zoomed`: no dot at z 1, alpha 129 at 0.35, 255 at 0.25. At 3840 × 2160: 16-px dots, the hover
and the ring.

**The art** — the radar logo and a mod's set — is built once a process at the first in-play fill
(`iconart_build`, which calls `iconset_load` before it packs): `radlogo` is
session art, loaded by `0x429870` from start-up's `0x49134D`, and `0x42991E` is the only write of
its pointer ([weapons](../engine/weapons.html), *The effect GAF sequences are a SESSION asset*);
`radlogohigh` is loaded beside it, and `0x429935` is the only write of its pointer. Up
to 16 `radlogo` frames of at most 16 × 16, then `radlogohigh` frame 0 (the hover ring, at most 16 ×
16, packed only when a logo frame was, its key texels not drawn), then a mod set's pictures (up to
64, at most 22 × 22), go into a
256 × 256 atlas of palette indices through `tagpu_iconart_pack` (`src/tagpu_iconset.c`): left to
right a texel apart, in rows from texel `(1, 1)`, a new row under the tallest of the last. The packer
refuses any layout that would leave the atlas, and a compile-time assert in `tagpu_packet_pub.c`
shows the worst case cannot: a picture advances a row by at most a 23-texel cell, so a row holds at
least 11 and is at most 23 tall, and 81 pictures take 8 rows, 185 texels of the 256. The ring is
the art's `high` (`has_high` 0 when the sequence or its frame does not decode, and then a hovered
dot has no ring). **Each logo's
team colour is its frame's texel at the hotspot** (the commonest drawn texel when the hotspot draws
nothing), never a table, because mods recolour it. The atlas's key, fill and selected markers are
the three smallest indices no drawn texel of any picture holds — a set's TransparentColor,
FillColor and SelectedColor texels are rewritten to them; a set that leaves fewer than three free
is dropped and the logos and the ring are packed alone, and logo art that draws every index leaves no art. A unit whose logo has no frame is filled in the
GUI's black; with no `radlogo` there are no radar dots and the shapes still draw.

## What an icon hides

A unit whose model is **hidden** gets no NU at all — no model, no shadow, no selection rect (BAR
draws no footprint for an icon) — and the marker pass skips its health bar and group digit.
**A hollow generated shape hides any model** from the level it is opaque, whatever the model's size
(`tagpu_icon_cover_r`: R is 0 for AIR and UNKNOWN with no set picture): its hole would show the model
and read as its solid twin. A set's picture and a solid shape keep the model's radius. **An opaque
icon drops the bar and the digit too**, also where a large model still shows around it: they would
cover the icon, the only reading at that level. A fading icon keeps them, so they disappear when the
ramp ends. The
**cargo of a hidden unit** — a unit in a transport, a nanoframe on a factory's pad — is part of its
carrier's picture: neither a model nor an icon, and nor is the cargo of that cargo. The render
thread marks the cargo lists (`cargo_first` / `cargo_next`, every hop a packet index the publisher
bounded and re-checked against the frame's count) of every hidden or already-suppressed unit, in up
to 64 rounds (`TAGPU_ICON_CARGO_HOPS`), each list followed at most 64 members, the engine's own
guard. Only a unit the native pass draws (`TAGPU_PK_U_NATIVE`) is ever an icon, hidden or
suppressed, and only such a unit passes the suppression on: a unit the engine draws keeps its
sprite, its bar and its digit, and hides no cargo. Cargo of a carrier whose model shows is decided
by its own rule. A unit with an icon whose model is not hidden draws both, the icon over it.

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
index`, and the hover ring after all of them (*Radar dots*). **Place** — the marker pass's second draw, right after the selection rects: over the world,
under the orders, the bars, the digits and every UI layer, not fogged (the gather placed each over a
unit it judged visible, or a radar contact), not depth-tested, blended at the icon's alpha.

`tagpu_mark_emit_icon` puts one quad of `edge` screen px on the unit's anchor, its corner on a whole
game pixel (`snap_game_px`, so art and generated shapes, both laid out in whole game pixels, cover
whole presented pixels), into a bucket of one quad a packet unit and one for the hover ring.
**The bucket is grown before any icon is decided** (`tagpu_mark_icon_room(n_units + 1)`, asked by
the native pass in the same condition as `tagpu_mark_draws`): a frame on which it would not grow
decides no icon and hides no model, and a frame queues at most one icon a unit and one ring, so an
icon of a frame that hid a model always has its quad. The fill colour, the
selection, the alpha and the shape **ride the vertex's world slot as two integers under 2¹²** —
`team + 256 · selected` and `alpha + 256 · shape` — so the interpolated value rounds back to the
integer at every fragment; a generated shape's edge rides the colour slot. The marker FS's `uText == 2` is the icon branch (text is `uText == 1`):
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
`icon_hit` ([camera and zoom](camera-zoom.html) §2.3b). **The pick needs that redirect**, which
`tagpu_vpwide.c` installs with the rest of its widening half under `tagpu_vpwide.on` (a play default,
taken off with `zoom`): without it the engine's own model test answers every click, icons
included, and an icon whose model is hidden is picked only where the engine's box for the model
lies. At the game thread's applied level
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
- **Hidden**: hit when the picture draws it, its circle holds the pointer and no other icon answers
  nearer:
  `(px − ax)² + (py − ay)² ≤ (0.45 · E / z)²`, with `ax = ((x − (eyeX << 16))
  >> 16) + 0x80` and `ay = ((z − (eyeY << 16)) >> 16) − ((alt >> 16) >> 1) + 0x20` — `0x48C6A0`'s
  own anchor without its corner offsets, in 64 bits. `0x48C6A0` is never called for such a unit, so
  its int16 narrowing cannot wrap. `E` is the edge `tagpu_icon_type` gives the picture: the
  class's drawn edge, or `k ×` the longer side of the type's set picture in the art this thread
  published, the type's `icon` bounded by the art's picture count.
- **The nearest icon answers.** The engine asks about one unit at a time and keeps, among its hits,
  the smallest key of the `+0x92` record (`0x48CE1E..0x48CE75`), which knows nothing of the pointer
  [DISASSEMBLED]. So a hit walks HotUnits (`main+0x1435F`, count `+0x14367`, each id below the slot
  count, model id 0 and units `tagpu_native_owns_unit` refuses skipped) with the same circle, keeps
  the smallest distance, an equal one on the lower record, and only that unit is a hit: exactly one
  hidden-model icon answers where their circles overlap. **One walk a pick**: the engine's loop
  `0x48CDF1..0x48CE8E` has no exit and `0x48C6A0` no other caller, so the first listed id with a
  model starts a pass and clears the winner, and the cost is one list walk however many units are
  under the pointer [DISASSEMBLED]. A count at or above the slot count is refused: the list holds
  `10 · MaxUnits` entries against `10 · MaxUnits + 1` slots.
- **What the picture refuses is not a hit.** HotUnits lists a foreign unit when any of four box
  points is in sight (`0x465AC0` from `0x48BC56`), while the gather asks only the anchor's tile, so
  it holds units with no icon or only a radar dot. The circle answers "not hit" for them, by the
  gather's own gates asked of the fog source the game thread packs (`tagpu_fogwide_source`): a
  foreign unit with no source or cloaked (`+0x10E` bit 2), any unit on unexplored ground, a foreign
  unit out of sight. That source can be one packet ahead of the frame the player saw, which costs at
  most one click at a fog edge. MEASURED 2026-11-05 (retail, floor 0.165 at 4K, a Peewee ringed by 50
  enemy solars at 200 to 298 px): 26 icons drawn and 26 named, none without an icon; the build before
  this test named units with no icon. Stock draws such a listed unit dimmed in the grey, as `tagpu_gdi.on`
  shows at 1×, and the native pass does not: a difference at the fog's edge at any level.

*Why it is safe by construction*: every value comes from the unit's own record, its carriers', the
other listed units', the main block and the fog source this thread packs, read on the game thread that owns them inside the engine's own call; the row is
bounded by the table's count and every carrier by the unit array. HotUnits is the cull and is
untouched (`0x485596`). The pick follows the game thread's applied level and the
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

**The radar dot under the pointer** (`tagpu_zoom_radar_pick`). The engine never names a radar
contact in the world view: `0x48CD80`'s viewport branch walks HotUnits, which lists a foreign unit
only when `0x465AC0` says it is in sight (`0x48BC56`). So the router redirects the hover query's
call `0x499278` ([hook map](hook-map.html), *The in-play mouse handler*) to `hover_handler`, which
calls `0x48CD80` and asks the radar pick only when it answered 0: **a unit the engine names always
wins over a dot**. The pick answers 0 unless the latest command record drew icons (`s_gIcons`, the
gate `icon_hit` has), the pointer is on the world — bit 1 of the region byte `main+0x2CC6`, which
`0x498DA0` writes for the record before the hover query runs and vpwide's replacement of it
computes from the TRUE viewport, clearing it over the minimap — the art is published, the
dispatched point `main+0x2C76`/`+0x2C7A` lies in the viewport rect `main+0x37E27` (inclusive on all
four edges, the test `0x4B6720` makes at `0x48CDB0`) and the game thread packs a fog source. The
rect alone is not the test: below zoom 1 vpwide widens it over the minimap's click rect
(10, 0)–(115, 125), so a minimap point would name a dot elsewhere on the screen — the same
collision `0x48CD80`'s own viewport branch has for units in sight [INFERRED from vpwide's rect;
not measured]. Then, over the unit array's slots from 1 below
the slot count `u16 main+0x14351` (at most `TAGPU_PK_MAX_UNITS`), it asks the render gather's own
gates of the engine's records, in the gather's order:

- alive (`+0x110` bit 28), not excluded (bit 14), a radar or sonar contact (`& 0x300`), and a model
  id `+0xA6` from 1 below `UNITINFOCount` `main+0x1438F`;
- drawn by the unit pass (`tagpu_native_owns_unit`);
- refused by the picture (`icon_refused` on the packed fog source: a foreign unit cloaked, one
  the engine's draw list leaves out — `tagpu_packet_pub_unit_seen`, the same call that sets the
  unit pass's `TAGPU_PK_U_UNSEEN` — or one on unexplored ground or out of sight at its anchor's
  tile) — the test that makes the gather draw a
  dot instead of the unit;
- drawn as the art the gather picks: a set's NONE picture at the art scale, else the owner's logo
  frame (`PlayerInfo+0x96`, `tagpu_packet_pub_player_logo`, bounded by the art's logo count) at
  `k_r`; under `radardots=zoomed` only where the rule gives it an alpha at the game thread's level.

A dot is hit when the pointer is inside its drawn rect: half its edge over `z` each side of the
anchor `icon_circle` takes (`0x48C6A0`'s without its corner offsets, in 64 bits). Among dots that
overlap, the one drawn last answers — the larger anchor y, then the later slot, the order
`tagpu_icon_emit` draws them in, all on one layer. The answer is the unit's `+0xA8`, kept only
below the slot count (the engine indexes the unit array with it, `0x48CF8C`, `0x48D229`), and
logged as `zoom: radar pick` (*Logs*).

*Why it is safe by construction*: it runs on the game thread inside the engine's own call and
reads the engine's records there; every slot is a stride-aligned record below the engine's slot
count, the model id is bounded by `UNITINFOCount`, the id answered by the slot count and the logo
by the art's count, and the art is
never written after its release store. The level and the fog source carry `icon_hit`'s
one-record lag, so at a zoom switch or a fog edge a hover can, for one record, name a dot the
frame did not draw or miss one it did.

**What the answer does downstream is the engine's.** The cursor chooser `0x48D220` maps the
hovered unit with no sight test, and the click `0x48CF30` makes it the target when `0x43E470(mode)`
is non-zero, which it is for every mode but 5, 10 and 14 (mode 0 asks the action's descriptor
instead, bit 9 of `+0x11`); per selected unit the resolver `0x43F0E0`
needs the target alive (`0x43F0F4`) and takes an enemy (`0x43F118`) under order 1, when the unit's
def has `+0x245` bit `0x10`, as order 3 (`push 3` at `0x43FE57`), the attack on a unit. None of the
three, nor the chooser's mapper `0x43E490`, calls `UnitInPlayerLOS 0x465AC0` directly: its seven
direct callers (`0x40AB11`, `0x439761`, `0x46AF3D`, `0x46B5B4`, `0x480EE5`, `0x48BC56`, `0x494396`)
lie outside all four [DISASSEMBLED 2026-11-09]. So an order on a dot is the minimap's order on the same contact. The formation move
leaves a press with a hovered unit to stock (`tagpu_formation.c`, `admit`), so a press on a dot is
stock's click. The placement's ring target calls `0x48CD80` itself (`tagpu_place.c`,
`ring_target`) and gets no dot; the dead copy `0x491D18` in `0x491CC0` keeps the engine's call.

**The field's other readers see the contact as the minimap hands it to them** ([input](../engine/input.html),
the `main+0x2CBA` row). Two matter to a player. The order-marker driver `0x48CC30` asks whether the
camera, tracked or hovered unit's def can build (`+0x156`, `0x48CCB0..0x48CCEE`) without asking its
owner, and so does its mirror `tagpu_order.c`, so hovering a constructor's dot shows the player's own
builders' queued build squares — the tell the stock minimap gives on the same contact. The read at
`0x46ABA9` reaches `UnitInPlayerLOS 0x465AC0` at `0x46AF3D` before it reads the unit's def, and
`0x494354` reaches it at `0x494396` [DISASSEMBLED 2026-11-09; that `0x46ABA9`'s function is the
bottom bar's unit name is INFERRED, and MEASURED 2026-11-09 the bar stayed empty with a contact's
dot hovered]. The debug key at `0x495751`, behind
`main+0x3923B` bit 1, writes `+0xF4` and `+0x110` of the hovered unit, a contact included.

## Settings

Four closed-list `impure.cfg` keys ([renderers](../renderers.html) §2.10b), read live by both
threads — every frame on the render thread, every pick and hover query on the game thread — so
none needs a restart. An unknown value in the file leaves the default and is logged; under
`tagpu_defaults.off` (every tacli control launch without `--defaults`) the store is not read, the
three icon keys take their defaults and `radardots` reads `zoomed` (`tagpu_settings_radardots`),
whose frame at 1× is the stock one.

| key | values (default first) | meaning |
|---|---|---|
| `iconscale` | `100` `75` `125` `150` `200` | the icon size, percent of BAR's (`UnitIconScaleUI` 1.05) |
| `icondist` | `100` `50` `75` `150` `200` | where icons take over, percent of BAR's 3000 elmos; 200 = icons only from twice as far |
| `iconfade` | `33` `0` `67` | the fade band `b`, percent; 0 = BAR's hard switch, 67 = Recoil's own 1000/3000 |
| `radardots` | `always` `zoomed` (`1` `0` also read) | a radar contact's dot in the world view at every level, or only where the icon rule brings it in (`TS_RADARDOTS`) |

**`radardots` is the in-game panel's *Radar dots* row** (Zoomed out \| Always; Restore defaults
sets Always), not the front-end screen's, whose two columns are full ([defaults and
settings](settings.html)); under `tagpu_defaults.off` the row is held and reads Zoomed out. **The
three icon keys have no row**: they are set in `impure.cfg` and read live.

## Threads

- **The type table** is built and read on the game thread (the publisher and the pick) and reaches
  the render thread only as values inside the packet; each type's set picture is one of them, an
  index both threads bound by the art's picture count.
- **The mod set** is read on the game thread inside the first in-play fill, before the art's
  release store: the ini name at `0x5098A3` and the sides' names in the main block, then the files,
  each bounded before it is read and DATA to the pure parsers. **The reads block that frame**: at
  most 71 small files opened and read in turn (the TADR ini, the iconcfg, five sides' pictures and
  64 more), once a process, on the first in-play frame. The load list it keeps is the game
  thread's own, read again only by the per-level walk on the same thread.
- **The category walk** runs on the game thread inside the in-play fill, once a level: a lifetime
  by thread ownership (*Mod icon sets*).
- **The art** is written once on the game thread before a release store
  (`__atomic_store_n(&s_iconArtReady, 1, __ATOMIC_RELEASE)`) and never written or freed after it;
  `tagpu_icontype_art()` takes it with an acquire load, so a render-thread pointer taken after it
  stays valid and unchanging for the process — the order art's argument.
- **`tagpu_icon_frame`** runs once a frame on the render thread before the unit gather and the
  marker gather, which read its arrays in the same frame on the same thread: program order is the
  ordering. `tagpu_icon.c` reads only the packet — the hovered unit too, `hovered_id`, which the
  publisher copies from `main+0x2CBA` at the fill — and the published art.
- **The radar pick** runs on the game thread, inside the hover query's call in the in-play handler,
  and reads the engine's records there; it shares nothing with the render thread but the command
  record's icon answer and the published art, both taken as `icon_hit` takes them.
- `thread-split.allow` names the new game-thread reads inside the existing `publisher` lines of
  `tagpu_packet_pub.c` (the type table, the radar logo and its hover ring, the TADR ini's
  name, the sides' names, the install's files and the category walk with the def fields
  it reads), `tagpu_zoom.c` (the pick's and the radar pick's reads) and `tagpu_route.c` (the hover
  query's call); `tagpu_iconset.c` is pure and has no line;
  `tagpu_icon.c` reads no engine memory and has no line. Presentation only: nothing writes
  simulation state, and no `sim` file is touched.

## Logs

| line | when |
|---|---|
| `icons: on= units= icons= set= hidden= cargo= dots= drop= base= zS= z= alpha(combat)= alpha(dot)= edges=<9> dotk= by-class=<9> us=<frame>+<emit>` | every 300 render frames, from the marker gather; `set=` the icons drawn as a mod set's picture (radar contacts not counted), `dots=` the radar contacts, as a dot or the set's NONE; the nine are in `TAGPU_ICLS_*` order — UNKNOWN, COMMANDER, FACTORY, BUILDING, AIRCONS, AIRCOMBAT, AIR, CONS, COMBAT, and count set pictures by their type's class. At 1× with the defaults `icons=0 hidden=0` |
| `icons: mod set <folder> from <ini> (<MegaMapConfig>), <mode> mode, N picture(s) loaded of M, R [Icon] line(s) refused, skipped: <files>` | once a process, when a set's iconcfg reads; `<mode>` is `default` or `custom`, `M` the load list's length, `<files>` the pictures that did not read or decode, or `none` |
| `icons: no mod set - the exe names no TADR ini`, `… - <ini> names no MegaMapConfig of the install's own`, `… - <ini>'s MegaMapConfig <value> does not read` | once a process, instead of the line above |
| `icons: the mod set and the hover ring leave no three free palette indices - no hover ring` | once a process: the ring gives way first, and the set is packed with the logos; a hovered contact then has no ring |
| `icons: the mod set leaves no three free palette indices beside the radar logo - not drawn` | once a process, when the set does not fit without the ring either: the set is dropped and the logos packed alone, with the ring |
| `icons: the hover ring leaves no three free palette indices beside the radar logo - no hover ring` | once a process, when the logos and the ring do not fit and the logos alone do: no ring, the dots stay |
| `icons: art N radar logo frame(s) of M, hover ring H, S mod set picture(s), key K fill F sel S` | once a process, at the art's publication; `H` 1 when `radlogohigh` frame 0 is in the atlas |
| `icons: mod set by type - E map entr(ies), P of T type(s) by a picture, U UNKNOWN; CTRL_W=a CTRL_B=b CTRL_F=c; <category>=<types> …` | once a level with a set loaded: the category map's entries, the types (rows past 0) given a picture and those given UNKNOWN, the three Ctrl categories' type counts, and each custom line's category with its type count, or `absent` for one the map does not hold |
| `icons: the category map's bounds are not a map - no categories this level` | a level whose map bounds the walk refuses |
| `icons: N unit types, past the table's T - no icons this level` | a level with too many types |
| `icons: N units in the packet, past the T the icons hold - no icons while that is true` | once |
| `zoom: icon pick - N hit(s), M miss(es) so far` | at most once a second, on the game thread |
| `zoom: radar pick - N hover(s) on a dot so far, the last id I` | at most once a second, on the game thread, while hover queries land on a radar dot; `I` the unit's `+0xA8` |
| `route: armed (stub 0x4993BF, right 0x49939B, entry 0x499200, cursor 0x499297, hover 0x499278)` | once, at attach, when every router site was written; `route: the hover query's call (0x499278) holds other bytes; not installed` or `… could not be written; not installed` instead when that one was not, and then the engine's own call answers alone |
| `mark: … icons= iconover= …` | the marker pass's `log` line: icon quads, and those the bucket refused |

## What it does not do

- **No menu rows** for the three keys until a panel layout fits 640×480.
- **No hover indication** on an icon (BAR has none; a set's `HoverColor` and `UseCircleHover` are
  not read); only a radar contact has one, the minimap's ring, round a set's NONE picture too.
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
- **A unit the rule does not hide** goes to the engine's test whatever the gather decides: a foreign
  model listed through a visible box corner with its anchor out of sight draws nothing, and can still
  be hit, its key beating a drawn icon under the pointer. A nanoframe too. A radar contact listed
  that way is the same case: the engine names it, so the radar pick is not asked. MEASURED
  2026-11-09 (`scenarios/radar-dots.json`, 1920 × 1080): a Core solar whose box corner was in a
  Peewee's sight and whose anchor was on grey ground was hovered by the engine's own pick with
  neither a dot nor a model drawn, and the radar pick's count did not move.
- **The radar pick's dot is the frame's only to a record**: the level and the fog source are the
  game thread's, one command record behind the picture at a zoom switch or a fog edge
  (*The pick*).
- **Multiplayer and mod sets, not run**: the radar pick and its order in a multiplayer game, and a
  contact drawn as a set's NONE picture hovered or ordered on, are not run in game. Nothing in the
  pick writes simulation state; the order goes through the engine's own click.
- **Overlapping icons** answer by nearness, which need not be the icon drawn on top. An opaque icon
  over a model the rule does not hide has no circle, so two of those are settled by the model's box
  and the engine's key.
- **An icon whose unit the HotUnits cull does not list** is not pickable, so the strip of an icon
  hanging over the view's edge from a unit outside the addressable rect answers nothing.
- **A unit the native pass does not own** keeps the engine's sprite and gets no icon.
- **A Vulkan marker frame refused after the icons were decided** loses its icons while the models
  they hide stay hidden: the marker pass's own refusals — in `tagpu_vk_mark_prepare` a draw out of
  its vertex block, an icon draw with no atlas or an image that would not upload, in `mk_draw` a
  draw past `TAGPU_MK_MAXDRAW` — drop the whole marker layer of that frame, which is a failed frame — the
  orders, bars and cursor are missing too — logged once a site, not a state the icons can prevent
  from the gather.
- **A click where an icon overlaps a model the rule does not hide**: that model answers through the
  engine's own test and tie-break, so the icon need not win.
- Nuke projectiles get no icon, so a set's NUKEICON is loaded and never drawn; a building out of radar leaves no ghost icon (BAR keeps one);
  BAR's "real icon once identified" for a radar contact has no TA counterpart.
