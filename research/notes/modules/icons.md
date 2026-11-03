# Strategic icons

Zoomed out, a unit is drawn as a flat team-coloured icon of constant screen size, by BAR's rule;
where its model is smaller than the icon, the model is not drawn at all, and the icon answers the
pick.

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`. The design and the landings: [mega map plan](../mega-map-plan.html), landing 3.

**Not yet checked in game.** The host test passes and the DLL builds; none of the in-game checks
has been run — 1× against the stock frame, every class at the floor in both team colours, the
fade band's alphas, click, hover, band box and orders on icons, radar dots, the 4K numbers, the
cost against the models replaced, the keys changing the live view (the plan's landing 3).

| file | what it holds | thread |
|---|---|---|
| `inc/tagpu_icon_rule.h` | the rule (size, distance, fade, hide) and the generated shapes, pure C with their GLSL twin `TAGPU_GLSL_ICON_FN` | any |
| `inc/tagpu_icontype.h` | the classes `TAGPU_ICLS_*`, the type table's and the art's interfaces | — |
| `src/tagpu_packet_pub.c` (`utype_level`, `utype_level_end`, `iconart_build`) | the per-type class table, built once a level inside the in-play fill and carried in the packet; the radar logo art, decoded once a process | game |
| `src/tagpu_icon.c` | per frame: which packet unit is an icon, which model is hidden, the queue and its sort | render |
| `src/tagpu_native.c` (the unit gather), `src/tagpu_mark.c`, `src/tagpu_vk_mark.c` | the icons placed, emitted and drawn by the marker pass | render |
| `src/tagpu_zoom.c` (`icon_hit`) | the pick, inside the unit hit test's guard at `0x48CE15` | game |
| `tests/icontest.c`, `tests/icontest.py` | the host test (`make icontest` is its Windows build) | host |

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
once built for the level), one `TAGPU_PK_UTYPE` (8 B: class, layer, a mod set's icon, `r_px`) per
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

## Radar dots

**Stock draws a radar contact on the minimap alone** ([ui](../engine/ui.html), *The minimap,
located*), so ours follow the same distance rule and 1× stays stock. A unit the gather refuses by
the cloak test or the fog test (an unexplored tile, or a grey one for a foreign unit) that is a
radar or sonar contact (`+0x110 & 0x300`) with a model is drawn as **the minimap's own dot**: its
owner's logo frame of `radlogo` (`main+0x147DF`), at an integer scale `k_r = max(1, round(0.8 ·
base / 4))`, under the rule with `S = E = k_r × max(w, h)` (8 px at 1080p, 4 px at the smallest
scale; the 7-px floor is the generated shapes', not the art's), on layer 1 (BAR's unidentified
contact). A dot hides nothing, is never white and is not pickable in the world view — the
engine's own rule (`0x48CD80` picks a radar contact on the minimap only). No fog source, no
foreign unit and no dot.

**The art** is decoded once a process at the first in-play fill (`iconart_build`): `radlogo` is
session art, loaded by `0x429870` from start-up's `0x49134D`, and `0x42991E` is the only write of
its pointer ([weapons](../engine/weapons.html), *The effect GAF sequences are a SESSION asset*). Up
to 16 frames of at most 16 × 16 go into a 256 × 256 atlas of palette indices, each in a 17-texel
cell (a texel of gap), 15 cells to a row: frame `i` at `(1 + 17 (i mod 15), 1 + 17 ⌊i / 15⌋)`, so
the 16th starts the second row and every frame read has its cell inside the atlas (a compile-time
assert holds the count against the rows); **each logo's team
colour is its frame's texel at the hotspot** (the commonest drawn texel when the hotspot draws
nothing), never a table, because mods recolour it. The atlas's key, fill and selected markers are
the three smallest indices no drawn texel holds. A unit whose logo has no frame is filled in the
GUI's black; with no `radlogo` there are no radar dots and the shapes still draw.

## What an icon hides

A unit whose model is **hidden** gets no NU at all — no model, no shadow, no selection rect (BAR
draws no footprint for an icon) — and the marker pass skips its health bar and group digit. The
**cargo of a hidden unit** — a unit in a transport, a nanoframe on a factory's pad — is part of its
carrier's picture: neither a model nor an icon. The walk follows the packet's `cargo_first` /
`cargo_next`, every hop a packet index the publisher bounded and re-checked against the frame's
count, under the engine's own 64-hop guard. A unit with an icon whose model is not hidden draws
both, the icon over it, while the alpha ramps.

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
device grid (`snap_device`), into a bucket grown to the packet's unit count. The fill colour, the
selection, the alpha and the shape **ride the vertex's world slot as two integers under 2¹²** —
`team + 256 · selected` and `alpha + 256 · shape` — so the interpolated value rounds back to the
integer at every fragment. The marker FS's `uText == 2` is the icon branch (text is `uText == 1`):
shape 0 samples the icon atlas (the key discarded, the selected marker shown only when selected,
the fill marker taking the team colour or white), any other shape is `taIconShape`. Its four
uniforms `uIconFill`, `uIconSel`, `uIconWhite`, `uIconInk` sit at 20, 24, 28 and 32 of FS's block;
the line program's `uGrid` is also at 32, so `tagpu_vk_mark.c` writes `uGrid` into line draws'
blocks alone. The atlas is the image `IMG_ICON`, in a fourth descriptor set per slot, uploaded
once per slot (it never changes after its publication); an icon draw without it draws nothing and
is logged once. `TAGPU_MK_MAXDRAW` is 24: a full frame records 16 draws, and a draw past the cap
drops the whole marker layer for the frame.

## The pick

`tagpu_zoom_hit_guard`, the call `0x48CE15` inside `GetUnitAtMouse 0x48CD80`, first asks
`icon_hit` ([camera and zoom](camera-zoom.html) §2.3b). At the game thread's applied level
(`game_level()`), with the game thread's own class table and the screen size `main+0x37E1F` /
`+0x37E23`:

- `-1`, the guarded engine test as before: the latest command record drew no icons, no table, a
  unit `tagpu_native_owns_unit` refuses, a row
  past the table, a nanoframe (`+0x104 > 0`), no health (`+0x108 ≤ 0`), or a model the rule does
  not hide.
- **Carried** (`+0x86` ≠ 0): the carrier counts only as a record of the unit array — at or after
  `[main+0x14357]`, a multiple of `0x118` from it and below the slot count `u16 main+0x14351` —
  and if the carrier's model is hidden the answer is "not hit" (BAR gives cargo no icon; the band
  box `0x48C390` already skips carried units).
- **Hidden**: hit when `(px − ax)² + (py − ay)² ≤ (0.45 · E / z)²`, with `ax = ((x − (eyeX << 16))
  >> 16) + 0x80` and `ay = ((z − (eyeY << 16)) >> 16) − ((alt >> 16) >> 1) + 0x20` — `0x48C6A0`'s
  own anchor without its corner offsets, in 64 bits. `0x48C6A0` is never called for such a unit, so
  its int16 narrowing cannot wrap.

*Why it is safe by construction*: every value comes from the unit's own record, its carrier's and
the main block, read on the game thread that owns them inside the engine's own call; the row is
bounded by the table's count and the carrier by the unit array. HotUnits is the cull and is
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
never by an icon no frame drew; with no record yet, the engine's test. Hover sets `main+0x2CBA`
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
  the render thread only as values inside the packet.
- **The art** is written once on the game thread before a release store
  (`__atomic_store_n(&s_iconArtReady, 1, __ATOMIC_RELEASE)`) and never written or freed after it;
  `tagpu_icontype_art()` takes it with an acquire load, so a render-thread pointer taken after it
  stays valid and unchanging for the process — the order art's argument.
- **`tagpu_icon_frame`** runs once a frame on the render thread before the unit gather and the
  marker gather, which read its arrays in the same frame on the same thread: program order is the
  ordering. `tagpu_icon.c` reads only the packet and the published art.
- `thread-split.allow` names the new game-thread reads inside the existing `publisher` lines of
  `tagpu_packet_pub.c` (the type table, the radar logo) and `tagpu_zoom.c` (the pick's reads);
  `tagpu_icon.c` reads no engine memory and has no line. Presentation only: nothing writes
  simulation state, and no `sim` file is touched.

## Logs

| line | when |
|---|---|
| `icons: on= units= icons= hidden= cargo= dots= drop= base= zS= z= alpha(combat)= alpha(dot)= edges=<9> dotk= by-class=<9> us=<frame>+<emit>` | every 300 render frames, from the marker gather; the nine are in `TAGPU_ICLS_*` order — UNKNOWN, COMMANDER, FACTORY, BUILDING, AIRCONS, AIRCOMBAT, AIR, CONS, COMBAT. At 1× with the defaults `icons=0 hidden=0` |
| `icons: art N radar logo frame(s) of M, key K fill F sel S` | once a process, at the art's publication |
| `icons: N unit types, past the table's T - no icons this level` | a level with too many types |
| `icons: N units in the packet, past the T the icons hold - no icons while that is true` | once |
| `zoom: icon pick - N hit(s), M miss(es) so far` | at most once a second, on the game thread |
| `mark: … icons= iconover= …` | the marker pass's `log` line: icon quads, and those the bucket refused |

## What it does not do

- **No menu rows** for the three keys until a panel layout fits 640×480.
- **No mod icon sets** — a mod's own TADR `iconcfg.ini` art is landing 4; every type draws its
  generated shape (`TAGPU_PK_UTYPE.icon` is `TAGPU_ICON_NOSET`).
- **No hover indication** on an icon (BAR has none; TADR's HoverColor is not carried over).
- **Overlapping icons**: the engine's own tie-break picks among hits (the smallest key over def
  `+0x176`/`+0x17A`/`+0x17E`, `0x48CE1E..0x48CE75`), which need not be the icon drawn on top.
- **An icon whose unit the HotUnits cull does not list** is not pickable, so the strip of an icon
  hanging over the view's edge from a unit outside the addressable rect answers nothing.
- **A unit the native pass does not own** keeps the engine's sprite and gets no icon.
- Nuke projectiles get no icon; a building out of radar leaves no ghost icon (BAR keeps one);
  BAR's "real icon once identified" for a radar contact has no TA counterpart.
