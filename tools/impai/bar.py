#!/usr/bin/env python3
"""bar.py -- ImpAI's bar: one-against-one games against OTA AI and between ImpAI's levels, judged.

    tools/impai/bar.py ids --pair PAIR [--maps a,b] [--blocks N]    the game ids of a pair, one a line
    tools/impai/bar.py play --ids FILE|PAIR --lanes L [--setup NAME] [--out DIR] [--dll PATH] [--cap T] [--gpu]
                            [--draw] [--no-curtail]
    tools/impai/bar.py row b4|b5 --lanes L [--setup NAME] [--out DIR] [--dll PATH] [--gpu] [--draw]
                                                          A2b's row: its games played to its bound, then checked
    tools/impai/bar.py judge DIR...                       the verdict, per setup
    tools/impai/bar.py check b4|b5 DIR...                 A2b's b4 or b5 criteria over the games in DIR...

THE PAIRS (research/notes/impai/bar.md). Seat "A" is the player under test, "B" its opponent:

    eota   ImpAI Easy   against OTA AI, Difficulty Easy
    nota   ImpAI Normal against OTA AI, Difficulty Medium
    hota   ImpAI Hard   against OTA AI, Difficulty Hard
    eoh    ImpAI Easy   against OTA AI, Difficulty Hard (the level lever): Easy's target
    noh    ImpAI Normal against OTA AI, Difficulty Hard (the level lever): Normal's target
    ne     ImpAI Normal against ImpAI Easy   (the level lever, Difficulty Hard)
    hn     ImpAI Hard   against ImpAI Normal (the level lever, Difficulty Hard)
    p:<row> ImpAI Hard on the persona row PERSONA_ROWS[row] (the persona lever) against OTA AI, Difficulty Hard:
           a calibration row
    m:<row> ImpAI Hard (no limits) against ImpAI on the persona row PERSONA_ROWS[row], Difficulty Hard: THE MIRROR
    n:<row> ImpAI on Normal's row NORMAL_ROW against ImpAI on the persona row PERSONA_ROWS[row], Difficulty Hard:
           THE MIRROR, which decides Easy's row once Normal's is chosen
    h:<m>  ImpAI Hard against OTA AI, Difficulty Hard, OTA AI's production times m (the income lever): THE HANDICAP
    g:<a>.<b> ImpAI against ImpAI, Difficulty Hard, A on the settings a and B on b, each a PERSONA_ROWS row or H,
           the DLL's Hard with no persona lever: THE GAUNTLET
    dry   OTA AI against OTA AI, Difficulty Hard: the dry run, which proves the seating, the end of a side and the
           rate before ImpAI fights

eota, nota and hota are 100 games: five maps x four side pairings (aa, ac, ca, cc: A's side, B's) x five games, A
at the map's first start in games 1, 3 and 5 and its second in 2 and 4; the dry run is the five maps, sides ac, two
games each. eoh, noh, ne, hn, the calibration rows and the mirror are played in THE BLOCKS. A game's id is
`bar-<pair>-<map>-<sides>-<n>`, so a piece is a list of ids.

THE BLOCKS. A block is every map of the pair's pool played once from each start, 10 games on five maps: block b is
game n = 2b - 1 (A at the map's first start) and 2b (its second) of each map, with the side pairing BLOCK_SIDES[(the
map's index in MAPS + b - 1) % 4], so a block holds every side pairing and a map plays each once every four blocks.
`ids` lists a pair's blocks in order, so its first k blocks are its first 10 k games and a list cut there is
whole blocks. The blocks a pair is played to are its run set's: eoh and noh 12 (120 games), ne and hn 10, a
calibration row 4, a mirror pair 10, a handicap pair 2, a gauntlet pair 2 (`ids --blocks N` for more, as THE
ORDERING's third look asks of the ladder's).

THE GAUNTLET. `g:<a>.<b>` seats ImpAI on two settings, each through the persona lever (H: none, the DLL's Hard), every
ImpAI seat at the level lever's `hard`, and plays the development pool, so no settings row is chosen on the maps
that judge it. tools/impai/gauntlet.py plays a candidate's pairs against a pool of earlier champions and judges
them; `judge` reports a gauntlet pair's points and judges nothing.

THE HANDICAP. `h:<m>` gives OTA AI's seat the income lever tagpu_impai_income.on at m (tagpu/ddraw/src/
tagpu_income.c: the seat's production times m at every economy pass, m three places in 1.000..8.000), so one
multiplier is one pair, its games their own seeds; a game could not run unless the level init's `income: seat=`
lines name exactly the seats the game armed, at their multipliers (incomes). Its two blocks are one look of the
handicap ladder (ladder.py), which finds the multiplier at which ImpAI Hard falls to half the points; `judge`
reports a handicap pair's points and judges nothing. THE CHEAT FLAG reads a handicap seat's income as any other's:
the lever's first bonus reaches the second economy pass, so the first, `income_t1`'s, is the seat's own.

THE PERSONA ROWS are the calibration ladder, L0 (the softest) to L5 (Hard's: no limits), and the rows between and
below them (L01, L0a, L0b) and those that cut one axis from Hard's (Hlook, Hreact, Happm, Hatt), each nine
integers as the persona lever tagpu_impai_persona.on takes them (tools/impai/level_lever.py; L1 and L3 are the
DLL's Easy and Normal, impai_react.c IMPAI_PERSONAS). A game of a persona row writes its seat's row to that lever
beside the level lever's `hard` and could not run unless the seat's `impai: persona` line at its init names the
same row `from=lever`; each result carries every ImpAI seat's persona as logged (`persona`). A row between two is
a new entry under a new name: a row's games are resumed only on the same nine integers (game_kept) and judged only
with them.

THE MIRROR. Against OTA AI at Hard every row from L0 to L5 takes 0.53 to 0.74 of the points with no gradient
(round 3 at 57d39d71), and Hard against Normal was futile (hn 33 won, 34 lost, 33 at the cap), so the levels are
separated by ImpAI against ImpAI: `m:<row>` puts B on the row and A at Hard, and `n:<row>` puts A on NORMAL_ROW,
the row Normal plays, through the lever too. A mirror pair is judged as the ladder is (THE VERDICT): A's wins of
its first 100 games, THE CURTAILMENT and THE ORDERING.

THE SEED. Every bar game plays from its own seed, a function of its id alone (game_seed: the first four bytes of
the SHA-256 of `bar.py seed <id>`), so a game is the same game whenever it is played and no two games of the bar
are one. The scenario carries it as its `seed`, which tacli puts on the wire; under the harness's switch
tagpu_seeds.on, which every tareplay Worker writes, the scenario's clearing apply seeds the simulation RNG, the
game thread's CRT stream, the wind and the meteors from it (tagpu_patches.c, A RUN'S SEEDS) and logs `enginefix:
run seeds before the creates: the wire's seed <n>`. A game whose log does not name its seed so could not run: without the seeds the
games of one map, sides and start are one game (MEASURED 2026-11-14, the b7 round 2: games 1 and 3 of each id
identical, 42 pairs of 42, on a DLL whose apply did not seed). Each result carries its seed.

THE MAPS are two pools of five, one map of each kind. The bar's (BAR_MAPS), held out to judge, by the rule the
specification states (at least three starts, no uniform metal; the land, sea and mixed maps from the middle third
of the 90 such retail maps by area, the small one from the smallest third and the large one from the largest; each
the median of its class by area): Pincushion (land), Red Triangle (sea), Trout Farm (mixed, 40 % under sea), Lava
Mania (small), Plains and Passes (large). The development pool (DEV_MAPS), each the runner-up of its kind in the
same survey, on which the calibration rows and the tuning tournament's tier B play, so that no row or table is
chosen on the maps that judge it: Acid Foursome (land, 3 % under sea), Steel Jungle (mixed, 44 %), Luschaven (sea,
65 %), Gasplant Plain (small), Lava Alley (large). A calibration row and a mirror pair play the development pool,
every other pair the bar's.

A GAME. The template scenarios/impai/bar.json, filled: the map, the Difficulty, seat 0 an idle human with no unit,
A and B in seats 1 and 2 (A in seat 1 when it plays the first start, seat k standing at StartPos(k + 1)), each a
commander placed at that start position, read from the map's .ota through tavfs (the schema whose start count is
the three seats, else the one with most, as the skirmish picks, N-b1). ImpAI's levels go to the level lever
tagpu_impai_level.on, a calibration row's persona to tagpu_impai_persona.on and a tournament's tables to
tagpu_impai_table.on, each in the game folder before the launch (the DLL reads them at attach), each written by
level_lever's open, which never writes through a link. A lane (a tareplay Worker, held, turbo k = 1) loads it, then
holds it every 3000 ticks (STEP), or at the first frame visit at which seat 1's or seat 2's units alive, +0x144, is
0 (`pause at T or units <s> <= 0`, tagpu_replay.c THE PAUSE AT T), and peeks both: the game ends when one is 0 --
the other side won -- or at the cap, GameTime 81 000 (45 minutes): a failure for ImpAI in eota, nota and hota,
scored by THE VALUE AT THE LIMIT in a block pair. Both at 0 at one look is a draw, outcome "cap" with `draw`.
With the commander-death rule on, a commander's death kills its side (0x48667E..0x4866B3); bar.json sets the rule
on (setup.commander_death), and the harness reads the level's ActiveCommanderDeath main+0x37EF6 into each result,
so every result states the rule it was played under. A hold the engine does not reach within the harness's bounds
is "could not run", never an outcome. A veteran CORKROG (29 918 HP) can survive the kill-all's 30 000 hit, so a
side whose commander died can still hold that one unit (scout N-b4). Each result carries its last two looks
(`looks`: the GameTime, each seat's units alive, and at the last also its commanders lost, +0x106).

THE END is the engine's own end test, the same for every setup whatever its menus (a mod's in-game menu need not
hold the stock Exit: Escalation's TABMENU.GUI has none, and TA Zero's GOKMAIN2.GUI no route to one, MEASURED
2026-11-14). Once the outcome is read, on the held level, the end scenario (end_doc) clears every unit, places one
unit of the human seat -- its side's commander -- and arms the level's end test (`objective on`, which bar.json's
`off` disarmed). The level then runs: the skirmish victory 0x490230 answers 1 once no record the controlled
player does not ally has a live unit (engine/sim.md, *Seating a level*, the two end tests), and the level is torn
down 151 to 180 ticks later (tareplay, THE LEVEL'S END), within the bound Log.wait_end holds it to. The teardown
0x491B60 ends the recording and writes ImpAI's level-end lines (tagpu_impai_level_end). The harness waits for
the level-end line, the recorder's end and its writer's `closed` line, then stops the game through its own
instance (Worker.stop with hard: `tacli stop --hard`, which ends the PID the instance's prefix runs and then its
wineserver, and fails unless the game is gone), so no game is left at the post-game screen holding the
instance's registry store when the lane launches its next. A game whose level does not end so could not run.
The unit kept is the human's, not an AI seat's, so that no AI seat holds a unit in the ticks the end test takes:
neither player under test acts on the world after its outcome is read. What a result reads from the play -- the
pulses, the military's task lines -- it reads from the lines logged before the end scenario's apply
(read_result's `upto`); only the level-end lines come after it, and a seat that ordered no second factory is
stamped there with the level end's GameTime, not the outcome's. A result is the one the in-game menu's Exit gave:
MEASURED 2026-11-14 on the base game, bar-nota-pincushion-ac-1 (won at GameTime 32 853) and -ac-2 (at the cap
36 000), each played to its end by the menu's Exit and by this end, gave results equal in every field but the
level end's GameTime (`level_end_at`, 177 and 165 ticks later). From the level-end lines each ImpAI
seat's `end` (level_end): its units completed, its army by class, its factories completed by their build tech
(IMPAI_TINFO.ctech: the highest tech their constructors build), its water-only factories, the sea plants it
ordered, its attack waves, raids and scouts, its naval judgement and its tech-up income; from the military's
task lines its `first` ATTACK, RAID and scouting tick (first_ticks); from its pulses its best metal income a pass
over a pulse minute (`income_max`); from its `impai: second` line its second-factory decision (`second`,
second_factory); from its `impai: persona` line its persona (`persona`, personas).

THE VALUE AT THE LIMIT. A game at the cap -- not a draw at one look -- carries every seat's value at the cap
(`value`: {A, B: {live, t}}), read from its recording at the cap's own iteration (recording_evidence with `at`,
value_at): the value of each seat's finished units alive -- army, economy and commander alike, a unit's type's
metal + energy / 60 (IMPAI_TINFO.value) by the level's own type table (`tacli units`, Lane.type_table) -- over
the units the recording holds alive, by owner (+0xFF), type (+0xA6) and fraction left (+0x104, 0.0 finished). An
OTA AI seat's value is read the same way; no line of the log lists a seat's units by type. The hold at the cap +
15 puts the end's clear 15 ticks after the iteration the value reads. A live unit of a type the table does not
hold makes the game "could not run". MEASURED 2026-11-14 on the ten kept recordings of the base game's capped bar
games: the ImpAI seat's value so read equals its own pulse's `vlive` at the pulse's tick, 20 checks of 20 within
0.75 (ImpAI's float32 sum against this exact one), and each seat's units alive in the recording at the cap equal
the harness's look there, OTA AI's included (research/notes/impai/bar.md, *The value at the limit*).

WHAT IS KEPT FOR A DIAGNOSIS. With --keep-all every game is kept, a win included (a paired bar). A game A did not win, or in which an ImpAI seat stalled (for_diagnosis), keeps the
instance log of its launch beside its result (`<id>.tagpu.log`), as a game that could not run does; a bar game
(parse_id's `recordings`) keeps its recording too, as `<id>.imprec` beside its result with its path in the
result's `recording`, once its writer's `closed` line is in the log -- that line is what says the file is whole;
the `.imprec.lock` beside a recording stays by design (tagpu_recfile.h, THE PATH LOCK) and says nothing. Every
other game's recording is deleted once THE CHEAT FLAG's evidence is read from it, and its `recording` is null.

THE SETUP. `--setup NAME` plays the games on a compatibility setup's own folder (tools/compat/setups.json, built
as the compatibility suite builds it: engrun.SetupWorker over tacompat.prepare_wine), with the same ids, seeds,
holds, end and judging; a setup whose goal is not Impure running (tacompat.impai_stage_eligible) is refused. One
process and one directory hold one setup's games: each result names its setup (`setup`, null for the base game),
a resume replays a result of another, and `play` refuses a directory holding another setup's results; the default
directory is .data/impai/bar/<setup>-<time>. A setup's commanders are the ones its own sidedata names
(side_commanders: SIDE0 for `arm` and SIDE1 for `core`, tacli's side numbering), read through the profile of its
running image (tavfs.profile: the folder its exe or Patch Loader names, the archives it mounts); TA Zero's are
GoKCommander and ArmCommander, the commanders its compatibility ImpAI stage recorded the engine giving those
sides. The maps' start positions are read from the setup's archives the same way. The base game keeps COMMANDER
and the install's archives.

THE CHEAT FLAG. A setup's stock AI is judged a cheat by evidence from its games, never by a list. Each result
carries, from its recording (tools/replay/tarec.py): `income_t1`, each side's metal and energy production
(+0x9C and +0x90 of its player record) at the recording's first iteration, GameTime 1, at which each seat holds
its commander alone and the economy pass has run once; and `ai_only`, each side's completed units of an AI-only
type -- one on no build page any commander's menus lead to (tech 0 in `tasim_run roles` on the lane's own
folder: the file filler, which roles_check.py holds to the engine's) -- counted from the recording's deaths whose
image has the fraction left +0x104 at 0.0 (engine/units.md); the end's clear kills every unit, so each unit
completed dies once in the recording. `judge` flags a setup in which an OTA AI seat's income at t=1 exceeds an
ImpAI seat's on the same side -- more than a player of that side gets, so a stock handicap (Difficulty scales a
stock AI's income x 0.5 on Easy and x 0.7 on Medium, engine/sim.md *Difficulty and the AI handicap*) or a mod that
drops it is no cheat -- or in which an OTA AI seat completed an AI-only type. Sides are compared with themselves
alone, since a mod's sides' commanders can make different incomes. MEASURED 2026-11-14 on the pair nota's
recordings: OTA AI produced 0.7 times ImpAI's commander income on the same side on the base game and on the
loader+tadr-ota, 392+tadr-2026.8.6, twilight-2.0b98 and tazero-alpha5 setups, 1.0 times it on prota-4.8 and
mayhem-11.3.0, and 2 times it on escalation (4.0 metal and 100 energy against 2.0 and 50); TA Zero's OTA AI
completed `_AI` types on no page (ArmT1Mex_AI, GoKT1AF_AI, ...; 60 of TA Zero's 270 types are on none, and 12 of
the base game's 278, ARMBEAC and ARMGATE among them).

THE TURBO is k = 1, one tick a frame, and no option changes it: the step's once-a-call tail -- the temporary sight
records' expiry 0x482130 among it -- runs once a batch, not once a tick (engine/sim.md, *The batch is part of the
result*; tagpu_replay.c, THE TURBO), so a game at k > 1 is another game than the one a player at normal speed
plays, not the same game drawn less often (MEASURED 2026-10-08: speed 20 against k = 2 parted in 1 272 and 1 383
unit fields). The pause at T stays tick-exact at any k (its bound ends the batch at T); the end's clause does not,
since a batch of k ticks ends where it ends.

THE STALL RULE, from ImpAI's pulse lines (`impai: pulse`, one a game minute a seat): a stall is a window of 3600
ticks, after GameTime 1800 and before the game's end, in which the seat's `built` and its `eroots` both did not
rise (the economy stopped), or in which it held at least one army unit and its `mroots` did not rise (the army
stopped).

THE RENDERER. A game renders with GDI (tareplay.GDI_LEVER) and takes no GPU token; `--gpu` renders it on the
default renderer, for a debugging run, and takes a token a lane. Every result names its renderer, a game that could
not run included, and is compared only with results of the same one (research/notes/modules/impai.md, *The
renderer*); `judge` refuses a result that names none.

THE PACING. A GDI game runs NO DRAW (tareplay.NODRAW_LEVER): while the turbo runs, a frame whose batch was the
turbo's skips the game screen's draw and runs only what the harness reads -- the triggers, ImpAI's digests and the
roster (tagpu_nodraw.c, THE GATE). Held frames draw. `--draw` renders every frame, for debugging; `--gpu`
implies it, the lever being GDI's alone. Every result names its pacing (`pacing`: draw or nodraw) and, under no
draw, the level's account (`nodraw`: frames not drawn, drawn for the scoreboard, drawn otherwise).

THE STOP. SIGINT or SIGTERM during `play` sets tareplay's STOP: no game starts after it, every game still playing
is stopped through its own instance (tacli stop, which ends the PID found by that instance's prefix, then its
wineserver) and written as could_not_run, reason "stopped"; the lanes are closed, the run lock is released and the
exit is 2. A game not started is not written, so the same `play` resumes it.

RESUMING. `play` into an out dir that holds results skips every game whose result is already there, played on
the same setup to the same cap on the same renderer at the same pacing with the same tables from the same seed
(game_kept; a result that names no pacing was drawn): a game reloads its scenario from scratch at its own launch (Lane.play: `scenario load --restart`), so a game skipped changes nothing another plays. A result
that could not run is played again.

THE VERDICT. `judge` reads each setup's games apart (the base game's are one more), says whether it is stock or
cheats with THE CHEAT FLAG's evidence, and for each pair its wins, losses and caps; only a stock setup's games
count toward the verdict. A result with no seed was played before each game had its own and is refused.
- eota, nota, hota: the pair's 100 games (ids), every one a win for A against OTA AI and none with a stall; a game
  at the cap fails.
- Every block pair scores a game at the cap by THE VALUE AT THE LIMIT (by_value, the one rule wherever a cap is
  scored by value, tourney.py's `value` among them): a seat whose value is at least VALUE_LEAD (1.3) times the
  other's, and more than it, wins, and otherwise the game is a draw. A draw at one look (both sides gone in one
  tick) is not at the limit and stays a draw. A cap whose result holds no value at the cap itself is not scored,
  and its block is not complete.
- THE POINTS: 1 for A's win, 1/2 for a draw, 0 for a loss. A block pair reads the blocks complete -- every game of
  each played and scored -- in order, and its points as THE INTERVAL over its map means: each map's mean points
  one observation, since games on one map are correlated, the estimate their mean and the two-sided 90 % t
  interval beside it (T95, df = maps - 1).
- eoh: its 12 blocks' points at least EASY_BAND's floor (0.50; above 0.75 is accepted); noh: at least NORMAL_MIN (0.90); each with no game in which
  A stalled. A calibration row: its 4 blocks' points read against both, which way the row moves, and no verdict.
- ne, hn and the mirror pairs: LADDER_WINS (90) wins of their first 100 games, a game at the cap won by value
  counted and a draw not. THE CURTAILMENT: once LADDER_MISS (11) of those games are not won the pair has failed
  for certain, and `judge` says so whatever is left unplayed; `play` starts none of that pair's games after it
  (`--no-curtail` plays on, for THE ORDERING's later looks).
- THE ORDERING, for the same pairs, reported beside the ladder and judging nothing (the verdict that remains if
  the ladder is relaxed): THE LOOKS at its first 4, 8 and 12 blocks (ORDER_LOOKS), each the t of the map means
  against a share of 1/2 and the look's O'Brien-Fleming t bound for three looks on that many maps (OBF_T, two-sided
  10 %): "better" or "worse" at the bound, "futile" at an earlier look whose conditional power under the current
  trend is below FUTILITY (0.20), read to the first look that gives one; before the first look, the t of the
  blocks complete, which judges nothing.

THE ROW CHECKS. `check b4` and `check b5` score A2b's rows from the results (a2-spec.md section 9): ImpAI Hard
against OTA AI Hard (the pair hota), each game played to the row's bound, given to `play` as `--cap` (b4: 27 000,
four games on two maps or more; b5: 36 000, a game on each of the five maps). `row b4|b5` plays the row's own
games (ROWS) to its bound into a directory of its own and checks them. b4: an attack wave sent, a scout sent and
no stall. b5: army units of ROLES_MIN classes or more completed; a factory of build tech 2 or above -- one whose
constructors build tech 2 -- completed wherever ImpAI's best metal income a pass reached its tech-up income
`tech_m`; and the yard, judged at the second-factory decision from its `impai: second` line (yard_verdict): on
the sea map, or where ImpAI judged the map naval at that decision, "yard ordered" when it chose a sea plant, "naval,
no site" -- met, and listed by name -- when the decision's sea search found no candidate at all, and "yard
missing" otherwise; elsewhere the yard is not asked.

Exit 0 when the verdict passed (or the command ran), 1 when it failed, 2 when it could not run or was stopped."""
import argparse
import hashlib
import json
import math
import queue
import re
import shutil
import signal
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(TREE / "tools"))
sys.path.insert(0, str(TREE / "tools" / "replay"))
sys.path.insert(0, str(TREE / "tools" / "wiki"))
sys.path.insert(0, str(TREE / "tools" / "tasim"))
sys.path.insert(0, str(TREE / "tools" / "compat"))
sys.path.insert(0, str(HERE))
import level_lever  # noqa: E402

TEMPLATE = TREE / "scenarios" / "impai" / "bar.json"
# THE MAPS: the bar's five, the held-out pool that judges; and the development pool's five, on which the levels'
# rows are chosen (the calibration pairs) and which the tuning tournament's tier B plays, so that no row or table
# is chosen on the maps that judge it
BAR_MAPS = {"pincushion": "Pincushion", "redtriangle": "Red Triangle", "troutfarm": "Trout Farm",
            "lavamania": "Lava Mania", "plainspasses": "Plains and Passes"}
DEV_MAPS = {"acidfoursome": "Acid Foursome", "steeljungle": "Steel Jungle", "luschaven": "Luschaven",
            "gasplantplain": "Gasplant Plain", "lavaalley": "Lava Alley"}
MAPS = {**BAR_MAPS, **DEV_MAPS}
SIDE = {"a": "arm", "c": "core"}
COMMANDER = {"arm": "ARMCOM", "core": "CORCOM"}     # the base game's; a setup's are its sidedata's (THE SETUP)
SIDE_INDEX = {"arm": 0, "core": 1}                  # a scenario's side by its sidedata index (tacli SCN_SIDES)
# THE CHEAT FLAG: the relative margin above ImpAI's income an OTA AI seat's float32 income on the same side can reach
# by rounding alone
INCOME_EPS = 1e-4
PLAYER_METAL_IN, PLAYER_ENERGY_IN = 0x9C, 0x90      # player record floats: the last pass's production
UNIT_LEFT = 0x104                                   # unit float: the fraction left to build, 0.0 once complete
UNIT_TYPE, UNIT_OWNER = 0xA6, 0xFF                  # unit word: the type's index; byte: the owner's record
ENERGY_PER_METAL = 60.0                             # IMPAI_TINFO.value: metal + energy / TASIM_ROLES_R
PAIRS = {   # Difficulty, A (controller, level), B (controller, level)
    "eota": ("easy", ("impai", "easy"), ("ai", None)),
    "nota": ("medium", ("impai", "normal"), ("ai", None)),
    "hota": ("hard", ("impai", "hard"), ("ai", None)),
    "eoh": ("hard", ("impai", "easy"), ("ai", None)),
    "noh": ("hard", ("impai", "normal"), ("ai", None)),
    "ne": ("hard", ("impai", "normal"), ("impai", "easy")),
    "hn": ("hard", ("impai", "hard"), ("impai", "normal")),
    "dry": ("hard", ("ai", None), ("ai", None)),
}
LADDER = ("ne", "hn")
# THE PERSONA ROWS: the calibration ladder, each `<look_eco>,<look_mil>,<react>,<apm_eco>,<burst_eco>,<apm_mil>,
# <burst_mil>,<att_eco>,<att_mil>` as the persona lever takes it (level_lever.py --persona). L1 and L3 are the
# DLL's Easy and Normal (impai_react.c, IMPAI_PERSONAS), L5 its Hard; a rate or an attention of 0 is no limit.
# A pair `p:<row>` plays ImpAI at Hard on that row against OTA AI at Hard, `m:<row>` and `n:<row>` against ImpAI
# (THE MIRROR); a row between two of these is a new entry here, under a new name.
PERSONA_ROWS = {
    "L0": "75,150,60,5,2,7,2,3,1",
    "L1": "60,90,45,10,3,15,3,4,2",
    "L2": "45,60,30,16,4,24,4,6,2",
    "L3": "30,45,15,24,4,36,6,8,3",
    "L4": "15,30,0,40,6,80,10,12,4",
    "L5": "15,30,0,0,0,0,0,0,0",
    # between L0 and L1: L0's reaction and attention, a military look and budgets between the two
    "L01": "75,120,60,7,2,10,2,3,1",
    # below L0 on every axis: the economy's look 105, the reaction 90, the budgets 3/1 and 4/1, 2 builders busy
    # (the military's look 150 and attention 1 are already the lever's harshest)
    "L0a": "105,150,90,3,1,4,1,2,1",
    # the lever's harshest looks, reaction and attention, and budgets of 2 and 3 actions a minute
    "L0b": "150,150,135,2,1,3,1,1,1",
    # one axis cut from Hard's no limits, at L0b's harshness: the looks alone; the looks and the reaction (the
    # reaction cannot be cut alone: it is at most the shorter look less 15), so Hreact against Hlook is the
    # reaction's own cost; the budgets alone; the attention alone
    "Hlook": "150,150,0,0,0,0,0,0,0",
    "Hreact": "150,150,135,0,0,0,0,0,0",
    "Happm": "15,30,0,2,1,3,1,0,0",
    "Hatt": "15,30,0,0,0,0,0,1,1",
}
CALIBRATION = "p:"                      # a calibration pair's name: `p:<row>`
# THE MIRROR: ImpAI against ImpAI, B on a persona row through the lever. `m:<row>`: A at Hard (no limits);
# `n:<row>`: A on Normal's row, NORMAL_ROW, through the lever too. Each is judged as the ladder is.
MIRROR, NORMAL_MIRROR = "m:", "n:"
NORMAL_ROW = "L3"                       # Normal's row: the DLL's, until the calibration chooses another
# THE HANDICAP: `h:<m>`, ImpAI Hard against OTA AI at Hard with OTA AI's production times m through the income
# lever, m written with three places in 1.000..8.000 (the lever's own precision), so one multiplier is one pair and
# its games one set of seeds. A look of the handicap ladder (ladder.py) is HANDICAP_BLOCKS of its blocks.
HANDICAP = "h:"
HANDICAP_RX = re.compile(r"h:([1-8]\.[0-9]{3})")
HANDICAP_BLOCKS = 2
# THE GAUNTLET: `g:<a>.<b>`, ImpAI on the settings a as A against ImpAI on b as B, each a PERSONA_ROWS row or
# HARD_ROW, the DLL's Hard with no persona lever. A gauntlet pair is GAUNTLET_BLOCKS of its blocks by default.
GAUNTLET = "g:"
HARD_ROW = "H"
GAUNTLET_BLOCKS = 2
# THE BLOCKS: a block is every map of its pool played once from each start, 10 games on five maps; block b plays
# game n = 2b - 1 (A at the first start) and 2b (A at the second) of each map, with the side pairing
# BLOCK_SIDES[(the map's index in MAPS + b - 1) % 4], so a block holds every side pairing and a map plays each
# once every four blocks. A pair's run is its blocks in order, so its first k blocks are its first 10 k games.
# The blocks a pair is played to: T's targets 12, L's ladder 10 and the mirror's as many, C's calibration rows 4.
BLOCK_SIDES = ("aa", "ac", "ca", "cc")
BLOCKS = {"eoh": 12, "noh": 12, "ne": 10, "hn": 10}
CALIBRATION_BLOCKS = 4
MIRROR_BLOCKS = 10
CAP = 81000
# Ticks between two looks at the units alive. The look does not find a game's end: the units clause of every hold
# does, pausing at the frame visit after the step in which a seat's count reached 0 (Lane.hold), so a longer step
# cannot pass an ending: a game is read at the tick its count fell, whatever the step. The step decides how often
# the level is held -- each hold is five waits on the game's answers -- and how far apart the two `looks` a result
# keeps lie.
# Nothing else reads it: the stall rule reads the pulse lines, which ImpAI logs once a game minute whether the
# level is held or not, and the cap is held at exactly, since no hold is armed past it (`min(..., cap)`; CAP is 27
# steps).
STEP = 3000
REC, STRIDE = 0x1B63, 0x14B             # record 0 in main, a record (tagpu_engine.h)
ALIVE = 0x144                           # WORD, the units alive (engine/units.md)
LOST = 0x106                            # WORD, the commanders lost: the destructor's 0x486996 (engine/units.md)
CMDR_DEATH = 0x37EF6                    # u32 ActiveCommanderDeath
PULSE = re.compile(r"impai: pulse seat=(\d+) t=(\d+) built=(\d+) mroots=(\d+) eroots=(\d+) army=(\d+) "
                   r"squads=(\S+) metal=(\S+) energy=(\S+) vlive=(\S+) vbuilt=(\S+)")
TABLE = re.compile(r"impai: table seat=(\d+) hash=([0-9a-f]{16}) from=(\w+)")
# a seat's persona at its init, in the lever's order (tagpu_impai.c, impai_mil_init)
PERSONA_LINE = re.compile(r"impai: persona seat=(\d+) level=\S+ look=(\d+),(\d+) react=(\d+) "
                          r"apm=(\d+)/(\d+),(\d+)/(\d+) att=(\d+),(\d+) from=(\w+)")
# a seat the income lever armed, at the level init (tagpu_income.c, tagpu_income_level_init)
INCOME_LINE = re.compile(r"income: seat=(\d+) mult=(\d+\.\d{3}) from=lever")
ECO_END = re.compile(r"impai: eco seat=(\d+) (.*)")                       # the level end's (tagpu_impai.c)
MIL_END = re.compile(r"impai: mil seat=(\d+) level=\S+ (.*)")
MIL_TASK = re.compile(r"impai: mil seat=(\d+) t=(\d+) attack=(\d+) raid=(\d+) scout=(\d+)")   # tasks begun
LEVEL_END = "impai: handicap rows"      # every level end writes it, whoever plays (tagpu_impai_level_end)
REC_START = re.compile(r"recorder: recording (?!timeline )(.+?\.imprec) from GameTime")
REC_FORK = re.compile(r"recorder: recording timeline \d+ of (.+?\.imprec) from GameTime")    # a takeover's fork
REC_CLOSED = re.compile(r"recorder: writer: (.+?\.imprec) closed:")
RUN_SEEDS = re.compile(r"enginefix: run seeds before the creates: the wire's seed (\d+) at GameTime")      # tagpu_seed_run: only a DLL that seeds before the creates names this form
# the second-factory decision (tagpu_impai.c, THE SECOND FACTORY): at the decision that orders it, or at the level
# end for a seat that ordered none (chose=none)
SECOND = re.compile(r"impai: second seat=(\d+) gametime=(\d+) naval=(-?\d+) chose=(sea|land|none) "
                    r"sea_site=(found|none|refused|-)")
PASS_TICKS = 30                         # an economy pass: the income a pass is what is produced in 30 ticks
FIRST_BONUS = 31                        # the first economy pass an income-lever bonus reaches (tagpu_income.c)
STALL_WINDOW, STALL_FROM = 3600, 1800
LEVER_LEVEL, LEVER_TABLE = "tagpu_impai_level.on", "tagpu_impai_table.on"
# the pause a hold waits for: at its T, or earlier by a seat's units clause; or its T not taken
PAUSED = r"pause: (?:paused at GameTime (\d+)(?: \(units of seat \d+ <= \d+\))?\s*$|at {T} not taken.*)"
VULKAN = "vulkan"                       # the default renderer's name in a result
# THE VALUE AT THE LIMIT: the lead a seat's value at the cap must reach to win the game; less is a draw
VALUE_LEAD = 1.3
# THE TARGETS, each on a pair's points over its maps (THE POINTS): Easy's floor and Normal's floor against OTA AI
# at Hard; the ladder's wins in its first LADDER_GAMES games, and the games not won that fail it for certain.
# Easy has a floor and no ceiling: the owner accepts an Easy stronger than 0.75 (research/notes/impai/bar.md, The
# rules); EASY_BAND's upper end is still printed, as where the calibration's rows would aim, and judges nothing.
EASY_BAND, NORMAL_MIN = (0.50, 0.75), 0.90
LADDER_WINS, LADDER_GAMES = 90, 100
LADDER_MISS = LADDER_GAMES - LADDER_WINS + 1
# THE INTERVAL: t(0.95, df), the two-sided 90 % interval's quantile over M map means, df = M - 1
T95 = {1: 6.314, 2: 2.920, 3: 2.353, 4: 2.132, 5: 2.015, 6: 1.943, 7: 1.895, 8: 1.860, 9: 1.833}
# THE LOOKS: O'Brien-Fleming bounds for two-sided 10 % over K equally spaced looks, by integration of the joint
# normal (the levels' specification, stats/obf2.py): the z bounds, and the t bounds of the same nominal levels
# over M map means (t(df = M - 1) at each z bound's two-sided p); futility below a conditional power of FUTILITY
OBF_Z = {2: (2.373, 1.678), 3: (2.961, 2.094, 1.710)}
OBF_T = {(2, 5): (3.893, 2.193), (3, 5): (6.398, 3.099, 2.253),
         (2, 10): (2.898, 1.876), (3, 10): (4.010, 2.458, 1.918)}
FUTILITY = 0.20
ORDER_LOOKS = (4, 8, 12)                # THE ORDERING: the blocks of a ladder pair its three looks read


class BarError(Exception):
    pass


def next_hold(T: int, cap: int) -> int:
    """The GameTime of the hold after one at T: the next multiple of STEP, never past the cap, so a game is held
    at its cap exactly, whatever the cap -- the bar's 81 000 or a row's shorter bound."""
    return min((T // STEP + 1) * STEP, cap)


def renderer(gpu: bool) -> str:
    return VULKAN if gpu else "gdi"


def pacing(gpu: bool, draw: bool) -> str:
    """THE PACING: "draw" on the GPU or with --draw, else "nodraw"."""
    return "draw" if gpu or draw else "nodraw"


def game_kept(out: Path, game: dict, gpu: bool, draw: bool = False):
    """The result of `game` already in `out`, or None: a game that ran (not could_not_run) on the same setup (THE
    SETUP; none for the base game) to the same cap, on the same renderer at the same pacing (a result that names
    none was drawn), from the same seed (THE SEED; none for a game that names none), each seat with the table it is
    to play (a seat's table_hash, where the spec names one) and on the persona row it is to play (none for none)."""
    p = Path(out) / f"{game['id']}.json"
    try:
        r = json.loads(p.read_text())
    except (OSError, ValueError):
        return None
    if r.get("id") != game["id"] or r.get("outcome") in (None, "could_not_run"):
        return None
    if r.get("setup") != game.get("setup"):
        return None
    if r.get("cap", CAP) != game.get("cap", CAP) or r.get("renderer") != renderer(gpu):
        return None
    if r.get("pacing", "draw") != pacing(gpu, draw):
        return None
    if r.get("seed") != game.get("seed"):
        return None
    for s in ("A", "B"):
        want = game["seats"][s].get("table_hash")
        if want and (r.get("tables") or {}).get(s) != want:
            return None
        for key in ("persona", "income"):
            if game["seats"][s].get(key) != ((r.get("seats") or {}).get(s) or {}).get(key):
                return None
    return r


# ---- the game list ----

def pair_spec(pair: str) -> tuple:
    """(Difficulty, A, B) of `pair`, each seat (controller, level, persona): a pair of PAIRS; a calibration pair
    `p:<row>`, ImpAI at Hard on that PERSONA_ROWS row against OTA AI at Hard; or a mirror pair (THE MIRROR),
    `m:<row>`, ImpAI at Hard against ImpAI on that row, or `n:<row>`, ImpAI on NORMAL_ROW against ImpAI on that
    row, every ImpAI seat at the level lever's `hard` and a row through the persona lever. BarError for no such
    pair."""
    if pair in PAIRS:
        diff, a, b = PAIRS[pair]
        return diff, a + (None,), b + (None,)
    if pair.startswith(HANDICAP):
        income_of(pair)
        return "hard", ("impai", "hard", None), ("ai", None, None)
    if pair.startswith(GAUNTLET):
        a, b = gauntlet_rows(pair)
        return "hard", ("impai", "hard", PERSONA_ROWS.get(a)), ("impai", "hard", PERSONA_ROWS.get(b))
    kind = next((k for k in (CALIBRATION, MIRROR, NORMAL_MIRROR) if pair.startswith(k)), None)
    row = pair[len(kind):] if kind else None
    if row not in PERSONA_ROWS:
        raise BarError(f"no pair {pair!r}: {', '.join(PAIRS)}, or {CALIBRATION}, {MIRROR} or {NORMAL_MIRROR}<row> "
                       f"of {', '.join(PERSONA_ROWS)}, or {HANDICAP}<m>")
    if kind == CALIBRATION:
        return "hard", ("impai", "hard", PERSONA_ROWS[row]), ("ai", None, None)
    a = PERSONA_ROWS[NORMAL_ROW] if kind == NORMAL_MIRROR else None
    return "hard", ("impai", "hard", a), ("impai", "hard", PERSONA_ROWS[row])


def income_of(pair: str):
    """OTA AI's income multiplier of a handicap pair `h:<m>` (THE HANDICAP), a float; None for any other pair.
    BarError for an `h:` pair whose m is not three places in 1.000..8.000, the items the income lever takes
    (level_lever.income_error)."""
    if not pair.startswith(HANDICAP):
        return None
    m = HANDICAP_RX.fullmatch(pair)
    if not m or level_lever.income_error(f"1={m.group(1)}"):
        raise BarError(f"no pair {pair!r}: a handicap pair is {HANDICAP}<m>, m three places in 1.000..8.000")
    return float(m.group(1))


def gauntlet_rows(pair: str) -> tuple:
    """(a, b), the settings of a gauntlet pair `g:<a>.<b>` (THE GAUNTLET); BarError for any other pair or a
    setting that is neither a PERSONA_ROWS row nor HARD_ROW."""
    rows = pair[len(GAUNTLET):].split(".") if pair.startswith(GAUNTLET) else []
    if len(rows) != 2 or any(r != HARD_ROW and r not in PERSONA_ROWS for r in rows):
        raise BarError(f"no pair {pair!r}: a gauntlet pair is {GAUNTLET}<a>.<b>, each {HARD_ROW} or a row of "
                       f"{', '.join(PERSONA_ROWS)}")
    return rows[0], rows[1]


def gauntlet_pair(a: str, b: str) -> str:
    """The gauntlet pair of A on the settings `a` against B on `b`; BarError for a setting no pair takes."""
    pair = f"{GAUNTLET}{a}.{b}"
    gauntlet_rows(pair)
    return pair


def handicap_pair(mult: float) -> str:
    """The handicap pair of OTA AI's multiplier `mult`, rounded to the lever's three places."""
    return f"{HANDICAP}{round(mult, 3):.3f}"


def persona_pair(pair: str) -> bool:
    """A pair that plays a persona row: a calibration row, a mirror pair or a gauntlet pair."""
    return pair.startswith((CALIBRATION, MIRROR, NORMAL_MIRROR, GAUNTLET))


def ladder_pair(pair: str) -> bool:
    """A pair judged by the ladder's rules (THE VERDICT): ne, hn and the mirror's."""
    return pair in LADDER or pair.startswith((MIRROR, NORMAL_MIRROR))


def block_pair(pair: str) -> bool:
    """A pair played and judged in blocks (THE BLOCKS): the targets', the ladder's, the calibration's, the
    mirror's and the handicap's."""
    return pair in BLOCKS or persona_pair(pair) or pair.startswith(HANDICAP)


def run_blocks(pair: str) -> int:
    """The blocks a block pair is played to: its run set's."""
    if pair.startswith(HANDICAP):
        return HANDICAP_BLOCKS
    if pair.startswith(GAUNTLET):
        return GAUNTLET_BLOCKS
    return BLOCKS.get(pair) or (CALIBRATION_BLOCKS if pair.startswith(CALIBRATION) else MIRROR_BLOCKS)


def pool(pair: str) -> dict:
    """The maps a pair plays by default: a persona row's pairs the development pool's, so that no row is chosen on
    the maps that judge it; every other pair the bar's."""
    return DEV_MAPS if persona_pair(pair) else BAR_MAPS


def ids(pair: str, maps=None, blocks=None) -> list:
    """The game ids of `pair`: a block pair's `blocks` blocks (THE BLOCKS; by default its run set's), in block
    order; the dry run's two games a map; any other pair's 100."""
    pair_spec(pair)
    maps = list(maps or pool(pair))
    if pair == "dry":
        return [f"bar-dry-{m}-ac-{n}" for m in maps for n in (1, 2)]
    if block_pair(pair):
        nb = blocks or run_blocks(pair)
        return [f"bar-{pair}-{m}-{BLOCK_SIDES[(list(MAPS).index(m) + b - 1) % 4]}-{2 * b - 1 + second}"
                for b in range(1, nb + 1) for m in maps for second in (0, 1)]
    return [f"bar-{pair}-{m}-{s}-{n}" for m in maps for s in ("aa", "ac", "ca", "cc") for n in range(1, 6)]


def block_of(gid: str) -> int:
    """The block game `gid` of a block pair belongs to: game n of its map is block (n + 1) // 2."""
    return (int(gid.rsplit("-", 1)[1]) + 1) // 2


def game_seed(gid: str) -> int:
    """THE SEED of game `gid`: the first four bytes of SHA-256 over `bar.py seed <gid>`, big-endian, the 32 bits
    the scenario's `seed` takes (0..0xFFFFFFFF). A function of the id alone, so of its pair, map, sides and its
    index n: replaying an id replays its game."""
    return int.from_bytes(hashlib.sha256(f"bar.py seed {gid}".encode()).digest()[:4], "big")


def parse_id(gid: str) -> dict:
    """A bar game's spec, in the shape tourney.game_spec gives: map, sides, start (A's: 0 first, 1 second),
    seats {A, B: {ai, level}}, cap; and the bar's own pair, its Difficulty, its seed (game_seed) and
    `recordings`, which keeps the recording of a game kept for a diagnosis (THE END in the module's docstring). A
    seat played on a persona row carries it as `persona`, the lever's nine integers."""
    try:
        bar, pair, m, sides, n = gid.split("-")
        assert bar == "bar" and m in MAPS and len(sides) == 2 and set(sides) <= set(SIDE)
        n = int(n)
        diff, *seats = pair_spec(pair)
    except (ValueError, AssertionError, BarError) as e:
        raise BarError(f"not a bar game id: {gid!r}") from e
    spec = {}
    for s, (ai, level, persona) in zip(("A", "B"), seats):
        spec[s] = {"ai": ai, "level": level}
        if persona:
            spec[s]["persona"] = persona
    if pair.startswith(HANDICAP):
        spec["B"]["income"] = income_of(pair)
    return {"id": gid, "pair": pair, "map": m, "sides": {"A": SIDE[sides[0]], "B": SIDE[sides[1]]},
            "start": 0 if n % 2 else 1, "rep": n, "difficulty": diff, "seats": spec, "cap": CAP,
            "seed": game_seed(gid), "recordings": True}


# ---- the scenario ----

def map_name(m: str) -> str:
    return MAPS.get(m, m)


def seat_of(game: dict) -> dict:
    """{"A": seat, "B": seat}: A sits in seat 1 when it plays the first start, else in seat 2."""
    return {"A": 1, "B": 2} if game["start"] == 0 else {"A": 2, "B": 1}


def start_positions(name: str, seats: int = 3, vfs=None) -> dict:
    """{StartPos N: (x, z)} of the schema a skirmish of `seats` seats reads: the schema whose start count equals the
    seats, else the one with most, the first of equals (N-b1)."""
    import tavfs
    vfs = vfs or tavfs.Vfs(tavfs.find_install(None))
    hit = None
    for path, where in vfs.enumerate("maps", "*.ota", False):
        if Path(path).stem.lower() == name.lower():
            hit = vfs.read_from(path, where)
            break
    if hit is None:
        raise BarError(f"no map {name!r} in the install")
    schemas = []
    for top in tavfs.tdf_sections(hit):
        for sub in top["sub"]:
            if not sub["_name"].lower().startswith("schema"):
                continue
            pos = {}
            for s in sub["sub"]:
                if s["_name"].lower() != "specials":
                    continue
                for sp in s["sub"]:
                    what = (sp["keys"].get("specialwhat") or "").lower()
                    if what.startswith("startpos") and what[8:].isdigit():
                        pos.setdefault(int(what[8:]), (int(float(sp["keys"].get("xpos", 0))),
                                                       int(float(sp["keys"].get("zpos", 0)))))
            schemas.append(pos)
    if not schemas:
        raise BarError(f"{name}: no schema")
    exact = [p for p in schemas if len(p) == seats]
    return exact[0] if exact else max(schemas, key=len)


def side_commanders(blob: bytes, where: str = "sidedata") -> dict:
    """{"arm": type, "core": type} of a sidedata TDF: the `commander` of [SIDE0] and [SIDE1], the sides a scenario's
    `arm` and `core` name by index (SIDE_INDEX), the first section of a name winning as the engine's lookup does
    (tools/wiki/gfx.py, read_sides). BarError when one names none."""
    import tavfs
    secs = tavfs.tdf_sections(blob)
    out = {}
    for side, k in SIDE_INDEX.items():
        sec = next((s for s in secs if s["_name"].lower() == f"side{k}"), None)
        name = (sec["keys"].get("commander") or "").strip() if sec else ""
        if not name:
            raise BarError(f"{where}: [SIDE{k}] names no commander")
        out[side] = name
    return out


def setup_install(gamedir: Path) -> dict:
    """{"vfs", "commanders"} of a setup's game folder, as its running image reads it (tavfs.profile): the archives
    it mounts, and the commanders of its sidedata, `<gamedata>-<language>\\` first as the engine's open 0x4290F0
    tries it (gfx.Engine.open)."""
    import tavfs
    try:
        prof = tavfs.profile(gamedir)
        img, _ = tavfs.running_image(gamedir)
        vfs = tavfs.Vfs(gamedir, image=img)
    except tavfs.VfsError as e:
        raise BarError(f"{gamedir}: {e}") from None
    name = f"{prof['sidedata']}.{prof['tdf_ext']}"
    for path in (f"{prof['gamedata']}-{prof['language']}\\{name}", f"{prof['gamedata']}\\{name}"):
        hit = vfs.open(path)
        if hit and hit[0]:
            return {"vfs": vfs, "commanders": side_commanders(hit[0], path)}
    raise BarError(f"{gamedir}: no {prof['gamedata']}\\{name}")


def ai_only_types(gamedir=None) -> set:
    """The type names, lowered, on no build page any commander's menus lead to: tech 0 in `tasim_run roles` (THE
    CHEAT FLAG) on `gamedir`, or on the retail install when it is None."""
    import native
    rc, out, err = native.roles(gamedir)
    if rc:
        raise BarError(f"tasim_run roles: {(err or out).strip()[-300:]}")
    return {u["name"].lower() for u in json.loads(out)["units"] if u["tech"] == 0}


def game_doc(game: dict, starts: dict, commanders: dict = COMMANDER) -> dict:
    doc = json.loads(TEMPLATE.read_text())
    s = doc["setup"]
    s["map"] = map_name(game["map"])
    s["difficulty"] = game["difficulty"]
    seat = seat_of(game)
    doc["units"] = []
    for side in ("A", "B"):
        k = seat[side]
        if k + 1 not in starts:
            raise BarError(f"{s['map']}: no StartPos{k + 1}")
        x, z = starts[k + 1]
        ctl = "impai" if game["seats"][side]["ai"] == "impai" else "ai"
        s["players"].append({"slot": k, "controller": ctl, "side": game["sides"][side], "color": k,
                             "storage": {"metal": 1000, "energy": 1000}})
        doc["units"].append({"id": f"cmd{k}", "type": commanders[game["sides"][side]], "owner": k, "pos": [x, z],
                             "facing": 0})
    s["players"].sort(key=lambda p: p["slot"])
    doc["description"] = f"bar.py: {game['id']}"
    if game.get("seed") is not None:
        doc["seed"] = game["seed"]      # THE SEED; a game that names none takes tacli's, a hash of the scenario
    return doc


def end_doc(doc: dict, commanders: dict) -> dict:
    """THE END's scenario for the game `doc`: every unit cleared, the end test armed, and one unit of the human
    seat kept -- its side's commander, at the first commander's start -- so the skirmish victory ends the level
    and no AI seat holds a unit while it does."""
    human = next(p for p in doc["setup"]["players"] if p["controller"] == "human")
    return {"format": doc["format"], "description": f"bar.py: the end of {doc['description'][len('bar.py: '):]}",
            "on_error": "abort", "setup": {"clear_existing": True, "objective": "on"},
            "units": [{"id": "end_keep", "type": commanders[human["side"]], "owner": human["slot"],
                       "pos": doc["units"][0]["pos"], "facing": 0}]}


# ---- the stall rule ----

def pulses(lines) -> dict:
    """{seat: [(t, built, mroots, eroots, army)]} in GameTime order"""
    out = {}
    for ln in lines:
        m = PULSE.search(ln)
        if m:
            seat, t, built, mr, er, army = (int(m.group(i)) for i in range(1, 7))
            out.setdefault(seat, []).append((t, built, mr, er, army))
    for v in out.values():
        v.sort()
    return out


def stalls(rows, end: int) -> list:
    """The stall windows [t0, t0 + 3600] of one seat's pulses: from a pulse after GameTime 1800 to the pulse 3600
    ticks later, before the game's end."""
    at = {r[0] // 1800: r for r in rows}
    out = []
    for r0 in rows:
        if r0[0] < STALL_FROM:
            continue
        r1 = at.get(r0[0] // 1800 + STALL_WINDOW // 1800)
        if not r1 or r1[0] > end:
            continue
        econ = r1[1] == r0[1] and r1[3] == r0[3]
        army = r0[4] >= 1 and r1[2] == r0[2]
        if econ or army:
            out.append({"from": r0[0], "to": r1[0], "economy": econ, "army": army})
    return out


def income_max(lines, k: int):
    """Seat k's best metal income a pass over a pulse minute -- the rise of its TotalMetalProduced between two
    pulses, over the passes between them -- or None with fewer than two pulses. The economy pass adds each pass's
    income to TotalMetalProduced (eco_bench.py, scout H1), so a minute's mean is at most its best pass's income:
    an income this reaches, some pass of that minute reached."""
    rows = sorted((int(m.group(2)), float(m.group(8))) for ln in lines for m in [PULSE.search(ln)]
                  if m and int(m.group(1)) == k)
    best = None
    for (t0, m0), (t1, m1) in zip(rows, rows[1:]):
        if t1 > t0:
            v = (m1 - m0) * PASS_TICKS / (t1 - t0)
            best = v if best is None or v > best else best
    return None if best is None else round(best, 2)


# ---- the level's end ----

def fields(text: str) -> dict:
    """{key: value} of a line's `key=value` words"""
    return dict(w.split("=", 1) for w in text.split() if "=" in w)


def counts(text) -> dict:
    """{name: n} of a level-end list `name:n,name:n`, {} for `-` or none"""
    out = {}
    for item in (text or "").split(","):
        name, _, n = item.partition(":")
        if name and name != "-" and n.isdigit():
            out[name] = int(n)
    return out


def _int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def level_end(lines, seat: dict) -> dict:
    """{A, B: ...} for each side whose seat wrote level-end lines (tagpu_impai.c, impai_play_end): from its `eco`
    line its units completed (`built`), its army units completed by class (`army`, ImpAI's own classes, and their
    sum `army_built`), its factories completed by their build tech (`factories`, {"1": n, "2": n}; "3" holds 3 and
    above; IMPAI_TINFO.ctech), its water-only factories completed (`yards`), the sea plants it ordered and its
    income at the end; from its `mil` line its attack waves, raids and scouting runs begun, its naval judgement (-1 never
    judged) and the income a pass at which it techs up (`tech_m`). A key the line does not carry is None."""
    by_seat = {v: k for k, v in seat.items()}
    out = {}
    for ln in lines:
        m = ECO_END.search(ln)
        if m and int(m.group(1)) in by_seat:
            f = fields(m.group(2))
            army = counts(f.get("classes"))
            out.setdefault(by_seat[int(m.group(1))], {}).update({
                "built": _int(f.get("built")), "army": army, "army_built": sum(army.values()),
                "factories": counts(f.get("ftech")), "yards": _int(f.get("yards")),
                "sea_plants": _int(f.get("sea_plants")), "metal_in": _float(f.get("metal_in"))})
            continue
        m = MIL_END.search(ln)
        if m and int(m.group(1)) in by_seat:
            f = fields(m.group(2))
            out.setdefault(by_seat[int(m.group(1))], {}).update({
                "attacks": _int(f.get("attacks")), "raids": _int(f.get("raids")), "scouts": _int(f.get("scouts")),
                "naval": _int(f.get("naval")), "tech_m": _float(f.get("tech_m")), "dguns": _int(f.get("dguns")),
                "defends": _int(f.get("defends")), "retreats": _int(f.get("retreats"))})
    return out


def first_ticks(lines, seat: dict, impai) -> dict:
    """{A, B: {attack, raid, scout}} for each side of `impai`: the GameTime of the military's first task line
    that began an attack wave, a raid, a scouting run (`impai: mil seat= t= attack= raid= scout=`), None for a
    kind it never began."""
    by_seat = {seat[s]: s for s in impai}
    out = {s: {"attack": None, "raid": None, "scout": None} for s in impai}
    for ln in lines:
        m = MIL_TASK.search(ln)
        if not m or int(m.group(1)) not in by_seat:
            continue
        got, t = out[by_seat[int(m.group(1))]], int(m.group(2))
        for kind, g in (("attack", 3), ("raid", 4), ("scout", 5)):
            if int(m.group(g)) and (got[kind] is None or t < got[kind]):
                got[kind] = t
    return out


def second_factory(lines, seat: dict, impai) -> dict:
    """{A, B: {gametime, naval, chose, sea_site}} for each side of `impai` that logged its second-factory decision
    (`impai: second`, tagpu_impai.c THE SECOND FACTORY): the GameTime, the military's naval judgement then (-1 never
    judged, 0 land, 1 naval), the plant it ordered (sea, land, or none by the level's end) and the decision's sea
    search (found a site; none, no candidate at all; refused, given up after its refusals; `-` none this decision).
    A seat's first line is its decision: the level end writes one only for a seat that logged none."""
    by_seat = {seat[s]: s for s in impai}
    out = {}
    for ln in lines:
        m = SECOND.search(ln)
        if m and int(m.group(1)) in by_seat and by_seat[int(m.group(1))] not in out:
            out[by_seat[int(m.group(1))]] = {"gametime": int(m.group(2)), "naval": int(m.group(3)),
                                             "chose": m.group(4), "sea_site": m.group(5)}
    return out


def seeded(lines, seed) -> bool:
    """True when the launch's clearing apply seeded its streams from `seed` (`enginefix: run seeds`, THE SEED)"""
    return any(m and int(m.group(1)) == seed for ln in lines for m in [RUN_SEEDS.search(ln)])


def for_diagnosis(res: dict) -> bool:
    """A game kept for a diagnosis: one A did not win, or one in which an ImpAI seat stalled."""
    return res.get("outcome") != "A" or any((res.get("stalls") or {}).values())


def personas(lines, seat: dict) -> dict:
    """{A, B: {"row": the lever's nine integers, "from": "lever" | "level"}} of each seat of `seat` (the ImpAI
    seats) from its persona line, its first: the init's."""
    by_seat = {v: k for k, v in seat.items()}
    out = {}
    for ln in lines:
        m = PERSONA_LINE.search(ln)
        if m and int(m.group(1)) in by_seat:
            out.setdefault(by_seat[int(m.group(1))], {"row": ",".join(m.group(i) for i in range(2, 11)),
                                                     "from": m.group(11)})
    return out


def incomes(lines, seat: dict) -> dict:
    """{A, B: the multiplier} of each seat of `seat` the income lever armed, from its level init's line; a seat
    played at x1 is absent."""
    by_seat = {v: k for k, v in seat.items()}
    out = {}
    for ln in lines:
        m = INCOME_LINE.search(ln)
        if m and int(m.group(1)) in by_seat:
            out.setdefault(by_seat[int(m.group(1))], float(m.group(2)))
    return out


def read_result(lines, seat: dict, game: dict, outcome: str, end: int, upto: int = None) -> dict:
    """What a game's result takes from its launch's log, the level ended: each seat's table hash from its
    `impai: table` line; and for each ImpAI seat its persona (personas), its stalls, its level end (level_end),
    its first tasks (first_ticks), its best income (income_max) and its second-factory decision
    (second_factory). The pulses and the task lines are read from the play's lines alone, `lines[:upto]` (THE
    END: the end scenario's apply is line `upto`)."""
    impai = [s for s in ("A", "B") if game["seats"][s]["ai"] == "impai"]
    by_seat = {v: k for k, v in seat.items()}
    play = lines[:upto]
    p = pulses(play)
    ends = level_end(lines, seat)
    return {"tables": {by_seat[int(m.group(1))]: m.group(2) for ln in lines for m in [TABLE.search(ln)]
                       if m and int(m.group(1)) in by_seat},
            "outcome": outcome, "end_tick": end, "persona": personas(play, {s: seat[s] for s in impai}),
            "stalls": {s: stalls(p.get(seat[s], []), end) for s in impai},
            "end": {s: ends[s] for s in impai if s in ends},
            "first": first_ticks(play, seat, impai),
            "income_max": {s: income_max(play, seat[s]) for s in impai},
            "second": second_factory(lines, seat, impai)}


def level_end_at(lines):
    """The GameTime the level ended at, from its level-end line (`impai: handicap rows ... gametime=`), or None."""
    for ln in lines:
        if LEVEL_END in ln:
            m = re.search(r"gametime=(\d+)", ln)
            return int(m.group(1)) if m else None
    return None


# ---- THE CHEAT FLAG ----

def recording_evidence(path: Path, R, at: int = None, values=()) -> dict:
    """What THE CHEAT FLAG reads of a closed recording (tools/replay/tarec.py): {"at": the GameTime of its first
    iteration, "income": {record: (metal, energy)} there for records 0..9, "deaths": [(owner, type index, fraction
    left)] of every unit death in it}; with `at`, also "units": {"at": the GameTime read, "units": [(owner, type
    index, fraction left)] of every unit alive at the end of iteration `at`}, what THE VALUE AT THE LIMIT reads;
    and "values": {T: the same of iteration T} for each GameTime of `values` (`play --values`). CouldNotRun when the
    file does not read or does not reach `at` or one of `values`."""
    import struct
    import tarec
    try:
        h, bl, _stop = tarec.load(path)
        w = tarec.stream_to(h, bl, want=1)          # the first iteration's world, whatever its GameTime
        income = {k: (struct.unpack_from("<f", p, PLAYER_METAL_IN)[0], struct.unpack_from("<f", p, PLAYER_ENERGY_IN)[0])
                  for k, p in enumerate(w.players)}
        out = {"at": w.tick, "income": income, "deaths": []}
        for kind, _first, _last, payload in bl[1:]:
            if kind != tarec.RB_STREAM:
                continue
            for rk, b in tarec.records(payload):
                if rk == tarec.RK_EV_DEATH:
                    out["deaths"].append((b[tarec.DEATH_OWNER], struct.unpack_from("<H", b, tarec.DEATH_TYPE)[0],
                                          struct.unpack_from("<f", b, tarec.DEATH_HEAD.size + UNIT_LEFT)[0]))
        def alive(t):
            w = tarec.stream_to(h, bl, want=t)
            if w.tick != t:
                raise ValueError(f"it ends at GameTime {w.tick}, before {t}")
            return {"at": w.tick, "units": [
                (img[UNIT_OWNER], struct.unpack_from("<H", img, UNIT_TYPE)[0],
                 struct.unpack_from("<f", img, UNIT_LEFT)[0]) for _slot, img in sorted(w.units.items())]}
        if at is not None:
            out["units"] = alive(at)
        out["values"] = {t: alive(t) for t in values}
    except (tarec.Unreadable, ValueError, struct.error, OSError) as e:
        raise R.CouldNotRun(f"the recording {path.name} does not read: {e}") from None
    return out


def value_at(units: dict, seat: dict, costs: dict) -> dict:
    """THE VALUE AT THE LIMIT of each seat from recording_evidence's "units": {A, B: {"live": the value of its
    finished units alive, "t": the GameTime}}, a unit's value being its type's metal + energy / ENERGY_PER_METAL
    (IMPAI_TINFO.value) by `costs`, {type index: (metal, energy)} of the level's own type table. ValueError for a
    live unit of a type the table does not hold: the value is taken over the level's own types or not at all."""
    by_seat = {v: k for k, v in seat.items()}
    live = {s: 0.0 for s in seat}
    for owner, typ, left in units["units"]:
        s = by_seat.get(owner)
        if s is None or left != 0.0:
            continue
        if typ not in costs:
            raise ValueError(f"a unit of {s}'s at GameTime {units['at']} is of type {typ}, which the level's type "
                             f"table does not hold")
        metal, energy = costs[typ]
        live[s] += metal + energy / ENERGY_PER_METAL
    return {s: {"live": round(v, 2), "t": units["at"]} for s, v in live.items()}


def evidence(rec: dict, seat: dict, types: dict, ai_only: set) -> dict:
    """A result's evidence for THE CHEAT FLAG from recording_evidence's `rec`: `income_t1`, {"at", A, B: {metal,
    energy}} of each side's record at the first iteration; and `ai_only`, {A, B: {UnitName: n}} of the units each
    side completed (its deaths with no fraction left) whose type `types` names and `ai_only` holds, lowered."""
    by_seat = {v: k for k, v in seat.items()}
    income = {"at": rec["at"]}
    for s, k in seat.items():
        metal, energy = rec["income"][k]
        income[s] = {"metal": metal, "energy": energy}
    done = {s: {} for s in seat}
    for owner, typ, left in rec["deaths"]:
        name = types.get(typ)
        if owner in by_seat and left == 0.0 and name and name.lower() in ai_only:
            got = done[by_seat[owner]]
            got[name] = got.get(name, 0) + 1
    return {"income_t1": income, "ai_only": done}


def cheat_evidence(rs) -> list:
    """THE CHEAT FLAG over one setup's results: a line for each side on which an OTA AI seat's income at t=1
    exceeded an ImpAI seat's on that side (the first such pair of games), and one for the AI-only types OTA AI seats
    completed, with their counts and the first game of each; [] when there is none. Sides are compared with
    themselves alone: a mod's sides' commanders can make different incomes (TA Zero's). A seat the income lever armed
    is read only from a first iteration before GameTime 31, the second economy pass, which its first bonus reaches
    (FIRST_BONUS); a later one is left out, never compared."""
    impai, ota, types = {}, [], {}
    for r in rs:
        inc = r.get("income_t1") or {}
        for s in ("A", "B"):
            if s not in inc or s not in (r.get("seats") or {}):
                continue
            side = r["sides"][s]
            armed_late = r["seats"][s].get("income") and inc.get("at", 0) >= FIRST_BONUS
            if r["seats"][s]["ai"] == "impai":
                if not armed_late:
                    impai.setdefault(side, (inc[s], r["id"]))
            elif not armed_late:
                ota.append((side, r.get("difficulty", "hard"), inc[s], r["id"]))
            for name, n in ((r.get("ai_only") or {}).get(s) or {}).items():
                if r["seats"][s]["ai"] != "impai":
                    got = types.setdefault(name, [0, r["id"]])
                    got[0] += n
    out, flagged = [], set()
    for side, diff, inc, gid in ota:
        if side not in impai or side in flagged:
            continue
        mine, mgid = impai[side]
        over = [k for k in ("metal", "energy") if inc[k] > mine[k] * (1 + INCOME_EPS)]
        if over:
            flagged.add(side)
            out.append(f"OTA AI's {side} income at t=1 {inc['metal']:g} metal, {inc['energy']:g} energy ({gid}, "
                       f"{diff}) above ImpAI's {mine['metal']:g}, {mine['energy']:g} ({mgid}) in "
                       f"{' and '.join(over)}")
    if types:
        out.append("OTA AI completed AI-only types: " + ", ".join(
            f"{name} x{n} ({gid})" for name, (n, gid) in sorted(types.items())))
    return out


def recording_of(lines, fork: bool = False):
    """(the recording's path as the recorder logged it, its writer closed) of a launch's log; None when it
    recorded nothing. `fork`: the file a takeover appended its timeline to, not a new recording."""
    rec = None
    start = REC_FORK if fork else REC_START
    for ln in lines:
        m = start.search(ln)
        if m:
            rec = m.group(1)
    if rec is None:
        return None
    name = rec.replace("\\", "/").rsplit("/", 1)[-1]
    closed = any(m and m.group(1).replace("\\", "/").rsplit("/", 1)[-1] == name
                 for ln in lines for m in [REC_CLOSED.search(ln)])
    return rec, closed


# ---- a lane ----

class Lane:
    """One tareplay Worker and the games it plays, one at a time."""

    def __init__(self, j, out, dll, gpu=False, setup=None):
        import tareplay as R
        self.R = R
        self.j, self.out, self.dll, self.setup = j, out, dll, setup
        if setup:
            import engrun
            self.w = engrun.SetupWorker(out, setup, dll, j)              # THE SETUP
        else:
            self.w = R.Worker(out)
        self.w.turbo = 1                # THE TURBO in the module's docstring: k > 1 plays another game
        self.w.held = True
        self.w.gdi = not gpu
        self.w.nodraw = pacing(gpu, _PACING["draw"]) == "nodraw"     # THE PACING
        self.log = None
        self.install = None             # {"vfs", "commanders", "ai_only"}: the lane's folder, read once
        self.types = None               # {type index: name}: the level's type table, read once
        self.costs = None               # {type index: (metal, energy)}: the same table's costs

    def read_install(self) -> dict:
        """The lane's install as its games read it, once: its archives (None: the retail install's) and commanders
        (THE SETUP), and its AI-only types (THE CHEAT FLAG)."""
        if self.install is None:
            got = setup_install(self.w.gamedir) if self.setup else {"vfs": None, "commanders": COMMANDER}
            got["ai_only"] = _ai_only(self.setup, self.w.gamedir)
            self.install = got
        return self.install

    def type_table(self):
        """({type index: UnitName}, {type index: (metal, energy)}) of the held level's type table (`tacli units`),
        the index a recording's unit and death name. A lane's install is one, so its table is too."""
        R = self.R
        rc, o = R.tacli("units", self.w.name, "--json", "--limit", "0", timeout=R.ACK_S + 60)
        if rc or "{" not in o:
            raise R.CouldNotRun(f"tacli units: {R.last(o)}")
        table, _ = json.JSONDecoder().raw_decode(o[o.index("{"):])     # stderr's lines follow stdout's
        return ({u["id"]: u["name"] for u in table["units"]},
                {u["id"]: (float(u["cost"][0]), float(u["cost"][1])) for u in table["units"]})

    def hand_to_stock_ai(self, rec: int) -> None:
        """Player record `rec`, a human's after a takeover, handed to OTA AI on the held level: cheats on, then the
        console's `AI` command typed as a chat line (0x416280: type 1 becomes 2; research/notes/modules/impai.md,
        *The `AI` command*), checked by the record's controller byte `+0x73` reading 2. OTA AI starts on it as it
        starts on every other seat a takeover gives it: afresh. ImpAI's seats keep playing what they knew."""
        R = self.R
        rc, o = R.tacli("switches", self.w.name, "cheats=on", timeout=R.ACK_S)
        if rc:
            raise R.CouldNotRun(f"switches cheats=on: {R.last(o)}")
        R.step(self.w, "type", f"+ai {rec}")
        got = self.peek(0x1B63 + 0x14B * rec + 0x73, 1)
        if got != 2:
            raise R.CouldNotRun(f"the AI command left record {rec} of controller {got}, not OTA AI's 2")

    def peek(self, off: int, size: int) -> int:
        R = self.R
        spec = f"*0x511DE8+0x{off:X}:x{size}"
        self.log.read()
        n = len(self.log.lines)
        R._CLI_API.put_trigger(self.w.gamedir / "tagpu_peek.trigger", spec + "\n")
        end = time.monotonic() + R.ACK_S
        while True:
            self.log.read()
            rows = R._CLI_API._peek_rows("\n".join(self.log.lines[n:]), [spec])
            if rows:
                try:
                    return int.from_bytes(bytes.fromhex(rows[0]["value"]), "little")
                except ValueError:
                    raise R.CouldNotRun(f"peek {spec}: {rows[0]['value']}")
            if time.monotonic() >= end:
                raise R.CouldNotRun(f"peek {spec}: no answer in {R.ACK_S} s")
            R.nap(self.log.NAP_S)
            self.log.alive()

    def hold(self, T, seats=()):
        """The level run on and held at GameTime T, or at the first frame visit before it at which a seat of
        `seats` holds no unit (`pause at T or units <s> <= 0`, tagpu_replay.c THE PAUSE AT T): at k = 1 that is
        the visit after the step in which its count fell to 0, so a game's end is read at its own tick, never up
        to a STEP later, when the other seat's count may have fallen to 0 too. Returns the GameTime held."""
        R = self.R
        line = f"pause at {T}" + "".join(f" or units {k} <= 0" for k in seats)
        n = len(self.log.lines)
        R.arm(self.w, self.log, line, rf"pause: armed for GameTime {T}\b", rf"pause: at {T} refused: .*",
              f"pause at {T}")
        self.log.pending, self.log.hold_from = T, n
        R.turbo_engage(self.w, self.log)
        R.resume(self.w, self.log)
        rx = PAUSED.replace("{T}", str(T))
        m = re.search(rx, self.log.wait_running(rx, n))
        if not m.group(1):
            raise R.CouldNotRun(f"pause at {T}: {m.group(0).strip()}")
        at = int(m.group(1))
        self.log.pending, self.log.held, self.log.at = None, True, at
        return at

    def look(self, seat: dict) -> dict:
        """{A, B: {alive, lost}}: each seat's units alive and commanders lost, read on the held level."""
        rec = {s: REC + seat[s] * STRIDE for s in ("A", "B")}
        return {s: {"alive": self.peek(rec[s] + ALIVE, 2), "lost": self.peek(rec[s] + LOST, 2)} for s in ("A", "B")}

    def keep_log(self, gid: str):
        """The game's instance log from its launch on, beside its result: the next launch on this lane starts a
        run of its own, so a game is diagnosed later from this copy or not at all."""
        if self.log is None:
            return
        try:
            self.log.read()
        except self.R.Failed:
            pass                        # a COB stop's line is in `lines` all the same: read() keeps it, then raises
        (self.out / f"{gid}.tagpu.log").write_text("\n".join(self.log.lines) + "\n")

    def lever(self, name, items):
        """A lever file in the game folder, its items one a line, written fresh by level_lever's open, which never
        writes through a link; none when there are no items. CouldNotRun when the open is refused."""
        (self.w.gamedir / name).unlink(missing_ok=True)
        if items and level_lever.write_lever(self.w.gamedir, name, items):
            raise self.R.CouldNotRun(f"the lever {name} could not be written")

    def play(self, game: dict) -> dict:
        R = self.R
        R.stopping()
        self.log = None
        self.w.watch_caps()             # this game's refusals of the GPU memory cap, none before it
        t0 = time.monotonic()
        seat = seat_of(game)
        res = {"id": game["id"], "map": game["map"], "sides": game["sides"], "start": game["start"],
               "seats": {s: dict(game["seats"][s], seat=seat[s]) for s in ("A", "B")},
               "difficulty": game.get("difficulty", "hard"), "cap": game.get("cap", CAP),
               "renderer": renderer(not self.w.gdi), "pacing": "nodraw" if self.w.nodraw else "draw",
               "seed": game.get("seed"), "setup": game.get("setup")}
        if game.get("setup") != (self.setup or {}).get("name"):
            raise BarError(f"{game['id']} is a game of {setup_label(game.get('setup'))}; this lane plays "
                           f"{setup_label((self.setup or {}).get('name'))}")
        install = self.read_install()
        # A SITUATION's own scenario (tools/impai/exploit.py), or the bar's game; a takeover's is the recorded
        # game's, which only the end reads
        doc = game.get("doc") or game_doc(dict(game, difficulty=res["difficulty"]),
                                          start_positions(map_name(game["map"]), vfs=install["vfs"]),
                                          install["commanders"])
        takeover = game.get("takeover")
        scn = self.out / f"{game['id']}.scenario.json"
        scn.write_text(json.dumps(doc, indent=1) + "\n")
        levels = [f"{seat[s]}={game['seats'][s]['level']}" for s in ("A", "B")
                  if game["seats"][s]["ai"] == "impai" and game["seats"][s].get("level")]
        tables = []
        for s in ("A", "B"):
            path = game["seats"][s].get("table")
            if path:
                name = f"impai_table_{seat[s]}.bin"
                (self.w.gamedir / name).unlink(missing_ok=True)
                shutil.copyfile(path, self.w.gamedir / name)
                tables.append(f"{seat[s]}={name}")
        persona = [f"{seat[s]}={game['seats'][s]['persona']}" for s in ("A", "B")
                   if game["seats"][s]["ai"] == "impai" and game["seats"][s].get("persona")]
        for item in persona:
            why = level_lever.persona_error(item)
            if why:
                raise BarError(f"{game['id']}: the persona {item} {why} (impai_persona_ok)")
        self.w.place({"name": "impai-bar"}, self.dll)
        self.lever(LEVER_LEVEL, levels)
        self.lever(LEVER_TABLE, tables)
        self.lever(level_lever.PERSONA_LEVER, persona)
        income = {s: game["seats"][s]["income"] for s in ("A", "B") if game["seats"][s].get("income")}
        self.lever(level_lever.INCOME_LEVER, [f"{seat[s]}={m:.3f}" for s, m in income.items()])
        self.log = R.Log(self.w)
        if takeover:
            # A PUZZLE: the recording taken over at T as OTA AI's seat, the level held at the refresh at or before T
            # (tacli replay takeover; tagpu_replay.c THE TAKEOVER), then that seat handed back to OTA AI below. The
            # idle human cannot take it: the takeover refuses a player with no unit.
            rc, o = R.tacli("replay", "takeover", self.w.name, takeover["recording"], "--at", takeover["at"],
                            "--player", takeover["player"], "--defaults", "--restart", "--keep-dll",
                            "--timeout", R.ACK_S, "--json", timeout=R.LAUNCH_TIMEOUT + R.ACK_S)
            try:
                got = json.loads(o[o.index("{"):])
            except ValueError:
                got = {}
            if rc or not got.get("taken") or not got.get("held"):
                raise R.CouldNotRun(f"replay takeover at {takeover['at']}: "
                                    f"{got.get('line') or got.get('timeout') or R.last(o)}")
        else:
            rc, o = R.tacli("scenario", "load", self.w.name, scn, "--defaults", "--restart", "--keep-dll",
                            timeout=R.LAUNCH_TIMEOUT)
            if rc:
                raise R.CouldNotRun(f"scenario load {scn.name}: {R.last(o)}")
        T = R.held_start(self.w, self.log)
        if takeover:
            self.hand_to_stock_ai(takeover["player"])
        if self.types is None:
            self.types, self.costs = self.type_table()
        res["commander_death"] = self.peek(CMDR_DEATH, 4)
        cap = res["cap"]
        outcome, end = "cap", cap
        looks = []
        # A SITUATION's waves: each scenario applied on the level held at its GameTime, its units added
        waves = sorted((int(t), str(f)) for t, f in game.get("waves") or ())
        while True:
            want = next_hold(T, cap)
            if waves and waves[0][0] < want:
                want = waves[0][0]
            T = self.hold(want, (seat["A"], seat["B"]))
            alive = {s: self.peek(REC + seat[s] * STRIDE + ALIVE, 2) for s in ("A", "B")}
            looks = (looks + [{"at": T, "seats": {s: {"alive": alive[s]} for s in ("A", "B")}}])[-2:]
            if not alive["A"] or not alive["B"]:
                # both gone at one tick: no side won, a draw, not a game at the limit (THE VERDICT)
                outcome = "cap" if not alive["A"] and not alive["B"] else "B" if not alive["A"] else "A"
                res["draw"] = outcome == "cap"
                end = T
                break
            if T >= cap:
                break
            while waves and waves[0][0] <= T:
                _t, f = waves.pop(0)
                rc, o = R.tacli("scenario", "apply", self.w.name, f, timeout=300)
                if rc:
                    raise R.CouldNotRun(f"the wave {Path(f).name} at {T}: {R.last(o)}")
        looks[-1]["seats"] = self.look(seat)
        res["looks"] = looks
        R.say(f"lane {self.j}: {game['id']}: at {T}: " + ", ".join(
            f"{s} (seat {seat[s]}) {v['alive']} alive, {v['lost']} commanders lost"
            for s, v in looks[-1]["seats"].items()))
        at_limit = outcome == "cap" and not res.get("draw")
        if at_limit:
            # THE VALUE AT THE LIMIT is the recording's iteration at the cap: the end's clear, applied on this
            # hold, lies 15 ticks after it, so no iteration the clear reaches is the one the value reads; and both
            # seats' pulses of the cap's minute (cap + 7 k % 15) are written by then
            self.hold(cap + 15)
        upto = self.end_level(doc, install["commanders"], game["id"])
        # the game has ended: a refusal of the GPU memory cap anywhere in it fails it by name
        refused = self.w.cap_failure()
        if refused:
            raise R.Failed(refused)
        lines = list(self.log.lines)
        if game.get("seed") is not None and not seeded(lines, game["seed"]):
            raise R.CouldNotRun(f"the level was not seeded from the game's seed {game['seed']} (no `enginefix: run "
                                f"seeds` line naming it): a DLL without the run seeds, or no tagpu_seeds.on")
        res.update(read_result(lines, seat, game, outcome, end, upto))
        for s in ("A", "B"):
            want = game["seats"][s].get("persona")
            got = res["persona"].get(s) or {}
            if want and (got.get("row"), got.get("from")) != (want, "lever"):
                raise R.CouldNotRun(f"seat {s} did not play its persona {want} from the lever (its persona line: "
                                    f"{got.get('row')} from {got.get('from')})")
        armed = incomes(lines[:upto], seat)
        if armed != income:
            raise R.CouldNotRun(f"the income lever armed {armed or 'no seat'}, not {income or 'no seat'} (the "
                                f"`income: seat=` lines at the level init; tagpu_income.c)")
        res["level_end_at"] = level_end_at(lines)
        if self.w.nodraw:
            res["nodraw"] = self.log.nodraw
        src = self.recording(fork=bool(takeover))
        if takeover:
            # the fork is the recording's timeline 1, which the evidence's reader, timeline 0's, does not read: a
            # puzzle is judged by its looks alone
            res["wall_s"] = round(time.monotonic() - t0, 1)
            res["recording"] = self.file_recording(game["id"], src, bool(game.get("recordings")))
            self.keep_log(game["id"])
            R.say(f"lane {self.j}: {game['id']}: {outcome} at {end} ({res['wall_s']} s)")
            return res
        # the values asked at fixed GameTimes (`play --values`), each one the game reached before its end: an
        # iteration at or after the end can hold the end scenario's clear
        ts = sorted(t for t in game.get("values") or () if 0 < t < end)
        rec = recording_evidence(src, R, at=cap if at_limit else None, values=ts)
        res.update(evidence(rec, seat, self.types, install["ai_only"]))
        try:
            if at_limit:
                res["value"] = value_at(rec["units"], seat, self.costs)
            if ts:
                res["values"] = {str(t): value_at(rec["values"][t], seat, self.costs) for t in ts}
        except ValueError as e:
            raise R.CouldNotRun(f"no value of a seat's: {e}") from None
        res["wall_s"] = round(time.monotonic() - t0, 1)
        keep = _KEEP["all"] or for_diagnosis(res)
        res["recording"] = self.file_recording(game["id"], src, keep and bool(game.get("recordings")))
        if keep:
            self.keep_log(game["id"])
        R.say(f"lane {self.j}: {game['id']}: {outcome} at {end} ({res['wall_s']} s)"
              + (", kept for a diagnosis" if keep else ""))
        return res

    def end_level(self, doc: dict, commanders: dict, gid: str) -> int:
        """THE END in the module's docstring: on the held level the end scenario applied, the level run to its
        teardown -- its level-end line, the recorder's end and its writer's `closed`, each within the end test's
        bound in game ticks (Log.wait_end, which fails the game past it) -- and the game stopped through its own
        instance, which fails unless it is gone. Returns the number of the launch's log lines before the apply:
        what the play logged."""
        R = self.R
        end = self.out / f"{gid}.end.scenario.json"
        end.write_text(json.dumps(end_doc(doc, commanders), indent=1) + "\n")
        self.log.read()
        n = len(self.log.lines)
        rc, o = R.tacli("scenario", "apply", self.w.name, end, timeout=300)
        if rc:
            raise R.CouldNotRun(f"apply {end.name}: {R.last(o)}")
        R.resume(self.w, self.log)
        for rx in (re.escape(LEVEL_END), r"recorder: ended after", REC_CLOSED.pattern):
            self.log.wait_end(rx, n)
        self.log.read()
        self.w.stop(hard=True)
        return n

    def recording(self, fork: bool = False) -> Path:
        """The launch's recording in the instance's replays folder, made and closed (its writer's `closed` line, the
        line that says the file is whole): CouldNotRun otherwise, since THE CHEAT FLAG and a diagnosis read it.
        `fork`: the instance's copy of a taken-over recording, its fork appended."""
        R = self.R
        rec = recording_of(self.log.lines, fork)
        if rec is None:
            raise R.CouldNotRun("the game made no recording")
        if not rec[1]:
            raise R.CouldNotRun(f"the recording {rec[0]} was not closed: its writer logged no `closed`")
        src = self.w.gamedir / "replays" / rec[0].replace("\\", "/").rsplit("/", 1)[-1]
        if not src.is_file():
            raise R.CouldNotRun(f"the recording {rec[0]} is not in {src.parent}")
        return src

    def file_recording(self, gid: str, src: Path, keep: bool):
        """The recording `src` moved beside the game's result as `<gid>.imprec` when `keep`, its path returned;
        otherwise deleted, and None. Every file left in the instance's replays folder goes with it, the locks
        included, so the next launch on the lane starts from none."""
        replays = self.w.gamedir / "replays"
        path = None
        if keep:
            path = (self.out / f"{gid}.imprec").resolve()
            shutil.move(str(src), str(path))
        for f in replays.glob("*.imprec*") if replays.is_dir() else []:
            f.unlink(missing_ok=True)
        return str(path) if path else None

    def abandon(self):
        try:
            self.w.stop(hard=True)
        except self.R.CouldNotRun as e:
            self.R.say(f"lane {self.j}: {e}")

    def close(self):
        self.w.close()


_LANES, _LANES_LOCK, _DLL = {}, threading.Lock(), {}
# THE PACING of this process's lanes: `play`'s --draw, set before the first lane is made; one process plays at one
# pacing, as on one renderer
_PACING = {"draw": False}
# THE SETUP of this process's lanes: `play`'s --setup, its record from tools/compat/setups.json (None: the base
# game), set before the first lane is made; one process plays one setup
_SETUP = {"setup": None}
# `play`'s --keep-all: every game keeps its log and recording, a win included -- a paired bar (paired.py) pairs
# every game of one arm with its twin in the other, and a win the bar dropped would have no partner
_KEEP = {"all": False}
_AI_ONLY = {}


def _ai_only(setup, gamedir) -> set:
    """ai_only_types of the process's install, once: on the first lane's folder for a setup (each lane's is built
    alike, by the same prepare), on the retail install for the base game. Under the lanes' lock, since the first
    call builds tasim_run and no two lanes may build it at once."""
    with _LANES_LOCK:
        key = setup["name"] if setup else None
        if key not in _AI_ONLY:
            _AI_ONLY[key] = ai_only_types(gamedir if setup else None)
        return _AI_ONLY[key]


def setup_label(name) -> str:
    return f"setup {name}" if name else "the base game"


def setup_of(name):
    """The compat setup record `name` names (THE SETUP), None for none: BarError for no such setup, or for one whose
    goal is not Impure running."""
    if not name:
        return None
    import tacompat
    rec = {s["name"]: s for s in tacompat.SETUPS}.get(name)
    if rec is None:
        raise BarError(f"no compat setup {name!r} (tools/compat/setups.json)")
    if not tacompat.impai_stage_eligible(rec):
        raise BarError(f"setup {name}: its goal is not Impure running, so it plays no ImpAI game")
    return rec


def _dll():
    import tareplay as R
    with _LANES_LOCK:
        if "path" not in _DLL:
            dll, make = R.build_dll(False, False)
            if not Path(dll).exists():
                raise R.CouldNotRun(f"no DLL at {dll} ({make})")
            _DLL["path"] = Path(dll).resolve()
        return _DLL["path"]


def play_game(game: dict, lane: int, out_dir: Path, gpu: bool = False) -> dict:
    """One game on lane `lane` (one Worker a lane, made at its first game and kept), its result in tourney.py's
    format: outcome A, B, cap or could_not_run, the end tick, each seat's table hash from its `impai: table` line,
    a game at the cap each seat's value at the limit (value_at); and the bar's own (read_result, file_recording):
    each ImpAI seat's stalls, level end, first tasks and best income, and the kept recording's path. Called from
    one thread a lane; the caller holds the run lock, with a GPU token a lane when `gpu`; one process plays on
    one renderer and one setup (_SETUP)."""
    import tareplay as R
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    try:
        dll = _dll()
        with _LANES_LOCK:
            if lane not in _LANES:
                _LANES[lane] = Lane(lane, out_dir, dll, gpu, _SETUP["setup"])
            ln = _LANES[lane]
        ln.out = out_dir
        try:
            return ln.play(game)
        except (R.CouldNotRun, R.Failed, BarError):
            ln.abandon()
            if not R.STOP.is_set():
                ln.keep_log(game["id"])
            raise
    except (R.CouldNotRun, R.Failed, BarError) as e:
        return {"id": game["id"], "outcome": "could_not_run", "reason": "stopped" if R.STOP.is_set() else str(e),
                "renderer": renderer(gpu), "pacing": pacing(gpu, _PACING["draw"]), "cap": game.get("cap", CAP),
                "seed": game.get("seed"), "setup": game.get("setup")}


def close_lanes():
    with _LANES_LOCK:
        for ln in _LANES.values():
            ln.close()
        _LANES.clear()


def stop_lanes(js):
    """The game of each lane of `js` stopped through its own instance (Lane.abandon: tacli stop, by the PID its
    prefix runs), from the thread that took the signal: a lane's thread can sit in one tacli call for minutes (a
    launch, tareplay.LAUNCH_TIMEOUT), and its game ends now rather than when that call returns."""
    with _LANES_LOCK:
        lanes = [_LANES[j] for j in js if j in _LANES]
    for ln in lanes:
        ln.abandon()


def play_all(games: list, lanes: int, out: Path, gpu: bool, play=None, stop=None, give_up=None) -> list:
    """Every game of `games` on `lanes` lanes, each result written into `out` as its game ends; the results. One
    thread a lane, each taking the next game from one queue, so no two threads ever play on one lane's Worker. A
    game for which give_up(game, the results so far) is true when its lane takes it is not played and not written
    (THE CURTAILMENT).

    THE STOP. SIGINT and SIGTERM set tareplay's STOP while this runs. From then on no lane takes a game, every wait
    of a lane's game gives up (tareplay.nap raises), the game of every lane still playing is stopped through its
    own instance (`stop`, stop_lanes), and play_game writes each such game as could_not_run, reason "stopped". A
    game not taken is not written. The caller closes the lanes after this returns, which stops every lane's
    instance again once its thread has ended, so no game outlives the run whatever its lane was doing when told.
    A harness defect in one game (any other exception) stops the run the same way and is raised here after."""
    import tareplay as R
    play = play or play_game
    stop = stop or stop_lanes
    out = Path(out)
    todo = queue.Queue()
    for g in games:
        todo.put(g)
    results, busy, errors, lock = [], {}, [], threading.Lock()

    def lane(j):
        while not R.STOP.is_set():
            try:
                g = todo.get_nowait()
            except queue.Empty:
                return
            with lock:
                if give_up is not None and give_up(g, results):
                    continue
                busy[j] = g["id"]
            try:
                r = play(g, j, out, gpu)
            except Exception as e:
                with lock:
                    errors.append(e)
                R.STOP.set()
                r = {"id": g["id"], "outcome": "could_not_run", "reason": f"{type(e).__name__}: {e}",
                     "renderer": renderer(gpu), "pacing": pacing(gpu, _PACING["draw"]), "cap": g.get("cap", CAP),
                     "seed": g.get("seed"), "setup": g.get("setup")}
            (out / f"{g['id']}.json").write_text(json.dumps(r, indent=1) + "\n")
            with lock:
                results.append(r)
                busy.pop(j, None)

    # The handler only notes the signal: it runs on this thread between two bytecodes, where a lock this thread
    # holds -- STOP's own condition, the print lock -- would deadlock it. This loop sets STOP and says so.
    signalled = []
    threads = [threading.Thread(target=lane, args=(j,), name=f"bar-lane-{j}")
               for j in range(min(lanes, len(games)))]
    before = {s: signal.signal(s, lambda signum, frame: signalled.append(signum))
              for s in (signal.SIGINT, signal.SIGTERM)}
    try:
        for t in threads:
            t.start()
        told = False
        while any(t.is_alive() for t in threads):
            time.sleep(0.2)
            if told or not (signalled or R.STOP.is_set()):
                continue
            told = True
            R.STOP.set()
            if signalled:
                R.say(f"bar: {signal.Signals(signalled[0]).name}: no game starts; each lane's game is stopped")
            with lock:
                playing = sorted(busy)
            stop(playing)
    finally:
        for s, h in before.items():
            signal.signal(s, h)
    if signalled:
        R.STOP.set()                    # one that came as the last game ended still stops the run
    if errors:
        raise errors[0]
    return results


# ---- the verdict ----

def read_results(dirs) -> list:
    rows = []
    for d in dirs:
        for p in sorted(Path(d).glob("bar-*.json")):
            if not p.name.endswith(".scenario.json"):
                rows.append(json.loads(p.read_text()))
    return rows


def first_summary(rs) -> list:
    """For each side with first tasks in `rs`, a line a kind: in how many games it began one, the range and the
    median of the first tick, and the games it began none in."""
    out = []
    for side in ("A", "B"):
        got = [(r["id"], r["first"][side]) for r in rs if side in (r.get("first") or {})]
        for kind in ("attack", "raid", "scout") if got else ():
            ts = sorted(f[kind] for _, f in got if f.get(kind) is not None)
            none = [gid for gid, f in got if f.get(kind) is None]
            line = f"{side}'s first {kind}: in {len(ts)} of {len(got)} games"
            if ts:
                line += f", GameTime {ts[0]}..{ts[-1]}, median {ts[(len(ts) - 1) // 2]}"
            if none:
                line += f"; none in {' '.join(none[:12])}{' ...' if len(none) > 12 else ''}"
            out.append(line)
    return out


def by_value(r: dict):
    """(winner, detail) of a game at the cap, scored by THE VALUE AT THE LIMIT, the one rule for a game at the cap
    wherever one is scored by value (the ladder, the targets, the calibration, tourney.py's `value`): "A" or "B"
    for a seat whose value is at least VALUE_LEAD times the other's and above it, "draw" otherwise; (None, why)
    when the result carries no value of a seat's at the cap itself."""
    cap, v = r.get("cap", CAP), r.get("value") or {}
    live = {}
    for s in ("A", "B"):
        t = (v.get(s) or {}).get("t")
        if t != cap:
            return None, f"no value of {s}'s at the limit (one at {t}, the cap {cap})"
        live[s] = float(v[s]["live"])
    detail = f"value A {live['A']:.0f}, B {live['B']:.0f}"
    for s, o in (("A", "B"), ("B", "A")):
        if live[s] > live[o] and live[s] >= VALUE_LEAD * live[o]:
            return s, detail
    return "draw", detail


def outcome_of(r: dict):
    """(the game's outcome for A -- "A", "B" or "draw" -- or None, why not): a game at the cap by value
    (by_value), both sides gone at one look a draw; None for a game that could not run or a cap not scored."""
    o = r.get("outcome")
    if o in ("A", "B"):
        return o, ""
    if o == "cap":
        return ("draw", "both sides gone at one look") if r.get("draw") else by_value(r)
    return None, f"{o}: {r.get('reason', '')}"


POINTS = {"A": 1.0, "draw": 0.5, "B": 0.0}


def interval(means: list):
    """THE INTERVAL over map means: (mean, half-width of the two-sided 90 % t interval); the half-width None for
    fewer than two maps."""
    m = len(means)
    mean = sum(means) / m
    if m < 2:
        return mean, None
    sd = (sum((x - mean) ** 2 for x in means) / (m - 1)) ** 0.5
    return mean, T95[m - 1] * sd / m ** 0.5


def t_stat(means: list) -> float:
    """The one-sample t of map means against a share of one half; +-inf when every map has the same mean but 1/2."""
    mean, m = sum(means) / len(means), len(means)
    sd = (sum((x - mean) ** 2 for x in means) / (m - 1)) ** 0.5
    if sd == 0:
        return 0.0 if mean == 0.5 else math.copysign(math.inf, mean - 0.5)
    return (mean - 0.5) / (sd / m ** 0.5)


def look(k: int, looks: int, maps: int, t: float):
    """THE LOOKS: the verdict at look k (1-based) of `looks` on `maps` map means of t statistic `t`: "better" or
    "worse" when |t| reaches the look's O'Brien-Fleming t bound, "futile" at an earlier look whose conditional
    power under the current trend is below FUTILITY, else None; with the bound and that power (None at the last)."""
    bound = OBF_T[(looks, maps)][k - 1]
    if abs(t) >= bound:
        return ("better" if t > 0 else "worse"), bound, None
    if k == looks:
        return None, bound, None
    # the current trend's conditional power: the z of this look as B(f) = z sqrt(f) at the information fraction
    # f, its drift z / sqrt(f) carried to the end, against the last look's z bound
    f, z = k / looks, max(-50.0, min(50.0, t))
    cp = 0.5 * math.erfc((OBF_Z[looks][-1] - abs(z) / f ** 0.5) / (1 - f) ** 0.5 / 2 ** 0.5)
    return ("futile" if cp < FUTILITY else None), bound, cp


def map_of(r: dict) -> str:
    """A result's map, from its id: a game that could not run names none of its own."""
    return r["id"].split("-")[2]


def map_means(rs, maps) -> list:
    """Each map's mean points of A's (POINTS) over the games `rs` scored, in `maps` order."""
    got = {m: [] for m in maps}
    for r in rs:
        got[map_of(r)].append(POINTS[outcome_of(r)[0]])
    return [sum(v) / len(v) for v in got.values()]


def not_won(rs) -> list:
    """The ids of the games of `rs` A did not win, a game at the cap by value: what THE CURTAILMENT counts."""
    return [r["id"] for r in rs if outcome_of(r)[0] in ("B", "draw")]


def curtailed(pair: str, rs) -> bool:
    """THE CURTAILMENT: a ladder pair has failed for certain once LADDER_MISS games of its first LADDER_GAMES are
    not won."""
    first = set(ids(pair)[:LADDER_GAMES]) if ladder_pair(pair) else set()
    return len(not_won([r for r in rs if r["id"] in first])) >= LADDER_MISS


def judge_pairs(rows) -> tuple:
    """(failed, lacking) of one setup's results, each pair's line printed with its wins, losses, caps, stalls and
    first tasks (THE VERDICT): `failed` counts the pairs short of the rule, `lacking` those that cannot be judged."""
    by_pair = {}
    for r in rows:
        by_pair.setdefault(r["id"].split("-")[1], []).append(r)
    fail = lacking = 0
    for pair in [p for p in PAIRS if p in by_pair] + sorted(p for p in by_pair if p not in PAIRS):
        rs = by_pair[pair]
        wins = [r["id"] for r in rs if r.get("outcome") == "A"]
        losses = [r["id"] for r in rs if r.get("outcome") == "B"]
        capped = [r["id"] for r in rs if r.get("outcome") == "cap"]
        cnr = [r["id"] for r in rs if r.get("outcome") == "could_not_run"]
        stalled = [(r["id"], s, w) for r in rs for s, ws in (r.get("stalls") or {}).items() for w in ws]
        walls = sorted(r["wall_s"] for r in rs if "wall_s" in r)
        need = len(ids(pair))
        print(f"{pair}: {len(wins)} of {len(rs)} won by A, {len(losses)} lost, {len(capped)} at the cap, "
              f"{len(cnr)} could not run, {len(stalled)} stalls"
              + (f"; wall {walls[0]}..{walls[-1]} s, 95th {walls[int(0.95 * (len(walls) - 1))]} s" if walls else ""))
        for name, lst in (("lost", losses), ("cap", capped), ("could not run", cnr)):
            if lst:
                print(f"  {name}: {' '.join(lst[:12])}{' ...' if len(lst) > 12 else ''}")
        for gid, s, w in stalled[:12]:
            print(f"  stall: {gid} seat {s} {w['from']}..{w['to']}{' economy' if w['economy'] else ''}"
                  f"{' army' if w['army'] else ''}")
        for line in first_summary(rs):
            print(f"  {line}")
        kept = [r["id"] for r in rs if r.get("recording")]
        if kept:
            print(f"  recordings kept: {len(kept)}, beside their results")
        dead = [r["id"] for r in rs if r.get("commander_death") == 0]
        if dead:
            print(f"  commander-death rule off in: {' '.join(dead)}")
        if pair == "dry":
            continue
        if block_pair(pair):
            f, lk = judge_blocks(pair, rs)
            fail, lacking = fail + f, lacking + lk
            continue
        if len(rs) < need:
            lacking += 1
            print(f"  lacks {need - len(rs)} of its {need} games")
        if cnr:
            lacking += 1
        else:
            fail += len(wins) < len(rs) or bool(stalled)
    return fail, lacking


def _ids(lst, n=12) -> str:
    return " ".join(lst[:n]) + (" ..." if len(lst) > n else "")


def judge_blocks(pair: str, rs) -> tuple:
    """(failed, lacking) of a block pair's results (THE BLOCKS), its lines printed: each game at the cap scored by
    value (by_value); the points over the blocks complete, every game of each played and scored, with THE
    INTERVAL over its map means; and its rule. eoh: its 12 blocks' points at least EASY_BAND's floor; noh: at least NORMAL_MIN;
    each with no stall; a calibration row: its 4 blocks' points read against both, a verdict on neither; ne, hn
    and the mirror pairs: LADDER_WINS wins in their first LADDER_GAMES games, failed for certain at LADDER_MISS
    games not won (THE CURTAILMENT), and THE ORDERING, which is reported and judges nothing."""
    _diff, *seats = pair_spec(pair)
    # the pair's pool, and any other map its games were played on: a block is every one of them
    maps = [m for m in MAPS if m in pool(pair) or any(map_of(r) == m for r in rs)]
    by_id = {r["id"]: r for r in rs}
    want = run_blocks(pair)
    plan = ids(pair, maps, max([want] + [block_of(g) for g in by_id]))
    stray = sorted(set(by_id) - set(plan))
    other = [r["id"] for r in rs if r["id"] not in stray and any(
        ((r.get("seats") or {}).get(s) or {}).get("persona") != persona
        for s, (_ai, _level, persona) in zip(("A", "B"), seats))]
    scored, unscored = {}, []
    for g in plan:
        r = by_id.get(g)
        if r is not None and r.get("outcome") == "cap":
            who, why = outcome_of(r)
            if who is None:
                unscored.append(f"{g} ({why})")
            else:
                scored.setdefault(who, []).append(g)
    if scored:
        print(f"  at the cap (a {VALUE_LEAD}x lead in value at the limit, or both gone at one look): "
              + ", ".join(f"{k} {len(scored[k])}" for k in ("A", "B", "draw") if k in scored))
        for k in ("B", "draw"):
            if scored.get(k):
                print(f"  cap {'won by B' if k == 'B' else 'drawn'}: {_ids(scored[k])}")
    for line in unscored[:12]:
        print(f"  cap not scored: {line}")
    if stray:
        print(f"  not games of its blocks, not read: {_ids(stray)}")
    if other:
        print(f"  played on another persona than {pair}'s, not read: {_ids(other)}")
    complete = 0
    for b in range(1, len(plan) // (2 * len(maps)) + 1):
        if any(g not in by_id or g in other or outcome_of(by_id[g])[0] is None
               for g in plan if block_of(g) == b):
            break
        complete = b

    def share(blocks):
        means = map_means([by_id[g] for g in plan if block_of(g) <= blocks], maps)
        mean, half = interval(means)
        return means, mean, half

    if complete:
        means, mean, half = share(min(complete, want))
        print(f"  points: {mean:.3f} of A's over {min(complete, want)} blocks"
              + (f", t90 [{mean - half:.3f}, {mean + half:.3f}]" if half is not None else "")
              + f" over {len(maps)} map means (" + ", ".join(f"{m} {x:.2f}" for m, x in zip(maps, means)) + ")")
    if ladder_pair(pair):
        lacked, failed = ladder_verdict(pair, rs)
        order(pair, plan, by_id, maps, complete)
        return failed, lacked
    if complete < want:
        print(f"  lacks blocks: {complete} of its {want} complete")
        return 0, 1
    if pair in ("eoh", "noh"):
        if pair == "eoh":
            met = mean >= EASY_BAND[0]
            print(f"  Easy's floor {EASY_BAND[0]:.2f}: {'met' if met else 'missed'}"
                  + (f" (above the band's {EASY_BAND[1]:.2f}, accepted)" if mean > EASY_BAND[1] else ""))
        else:
            met = mean >= NORMAL_MIN
            print(f"  Normal's floor {NORMAL_MIN:.2f}: {'met' if met else 'missed'}")
        stalled = [r["id"] for r in rs if any((r.get("stalls") or {}).values())]
        if stalled:
            print(f"  stalls, each a failure (the no-stall rule): {_ids(stalled)}")
        return int(not met or bool(stalled)), 0
    if pair.startswith(HANDICAP):
        print(f"  against OTA AI at x{income_of(pair):.3f}: {'at or above' if mean >= 0.5 else 'below'} half the "
              f"points, reported; the ladder (ladder.py) reads every look's games together")
        return 0, 0
    if pair.startswith(GAUNTLET):
        a, b = gauntlet_rows(pair)
        print(f"  {a} against {b}: A's points reported; the gauntlet (gauntlet.py) judges a candidate's pairs together")
        return 0, 0
    easy = ("in it" if EASY_BAND[0] <= mean <= EASY_BAND[1] else
            "above it, a harsher row" if mean > EASY_BAND[1] else "below it, a softer row")
    normal = "at or above it" if mean >= NORMAL_MIN else "below it, a softer row"
    print(f"  as Easy (band {EASY_BAND[0]:.2f}..{EASY_BAND[1]:.2f}): {easy}; as Normal (floor {NORMAL_MIN:.2f}): "
          f"{normal}")
    return 0, 0


def ladder_verdict(pair: str, rs) -> tuple:
    """(lacking, failed) of a ladder pair, its line printed: LADDER_WINS wins of its first LADDER_GAMES games, the
    rest, once LADDER_MISS of them are not won, a failure for certain whatever is left (THE CURTAILMENT)."""
    first = set(ids(pair)[:LADDER_GAMES])
    mine = [r for r in rs if r["id"] in first and outcome_of(r)[0] is not None]
    wins = sum(outcome_of(r)[0] == "A" for r in mine)
    if curtailed(pair, mine):
        print(f"  ladder: curtailed, failed for certain: {len(not_won(mine))} of its first {LADDER_GAMES} games "
              f"not won ({_ids(not_won(mine))}), {LADDER_MISS} fail it")
        return 0, 1
    if len(mine) < LADDER_GAMES:
        print(f"  ladder: {wins} won of the {len(mine)} scored of its first {LADDER_GAMES}; lacks games")
        return 1, 0
    print(f"  ladder: {wins} of its first {LADDER_GAMES} won, {LADDER_WINS} asked")
    return 0, int(wins < LADDER_WINS)


def order(pair: str, plan, by_id, maps, complete: int):
    """THE ORDERING of a ladder pair, printed: at each look whose blocks (ORDER_LOOKS) are complete, the t of its
    map means against a share of 1/2, the look's bound and its verdict, to the first that gives one; at a look not
    yet reached, the t of the blocks complete, which judges nothing."""
    looks, m = len(ORDER_LOOKS), len(maps)
    if (looks, m) not in OBF_T:
        print(f"  ordering: no bounds for {m} maps")
        return
    for k, blocks in enumerate(ORDER_LOOKS, 1):
        if complete < blocks:
            so_far = (f"; t {t_stat(map_means([by_id[g] for g in plan if block_of(g) <= complete], maps)):.2f} "
                      f"over the {complete} complete, no look" if complete and m > 1 else "")
            print(f"  ordering: look {k} at {blocks * 2 * m} games not yet read ({complete} blocks complete{so_far})")
            return
        t = t_stat(map_means([by_id[g] for g in plan if block_of(g) <= blocks], maps))
        verdict, bound, cp = look(k, looks, m, t)
        print(f"  ordering: look {k} at {blocks * 2 * m} games: t {t:.2f}, bound {bound:.3f}"
              + (f", conditional power {cp:.2f}" if cp is not None else "") + f": {verdict or 'no verdict'}")
        if verdict:
            return


def judge(dirs) -> int:
    rows = read_results(dirs)
    if not rows:
        print("bar judge: no game in " + ", ".join(map(str, dirs)))
        return 2
    unseeded = [r.get("id", "?") for r in rows if r.get("seed") is None]
    if unseeded:
        print(f"bar judge: COULD NOT RUN: {len(unseeded)} results name no seed: {' '.join(unseeded[:12])}"
              f"{' ...' if len(unseeded) > 12 else ''}; a verdict reads games each played from its own (THE SEED)")
        return 2
    unnamed = [r.get("id", "?") for r in rows if not r.get("renderer")]
    if unnamed:
        print(f"bar judge: COULD NOT RUN: {len(unnamed)} results name no renderer: {' '.join(unnamed[:12])}"
              f"{' ...' if len(unnamed) > 12 else ''}; a verdict reads games of one renderer, as each result names it")
        return 2
    rs = sorted({r["renderer"] for r in rows})
    if len(rs) > 1:
        print(f"bar judge: COULD NOT RUN: the games were rendered by {' and '.join(rs)}; a verdict reads games "
              f"of one renderer")
        return 2
    by_setup = {}
    for r in rows:
        by_setup.setdefault(r.get("setup"), []).append(r)
    fail = lacking = stock = 0
    for name in sorted(by_setup, key=lambda s: (s is not None, s or "")):
        rs = by_setup[name]
        cheats = cheat_evidence(rs)
        read = sum("income_t1" in r for r in rs)
        print(f"{setup_label(name)}: " + ("CHEATS, its games not counted: " + "; ".join(cheats) if cheats else
                                           f"stock: no evidence of a cheat in the {read} of its games that carry it"))
        f, lk = judge_pairs(rs)
        if not cheats:
            fail, lacking, stock = fail + f, lacking + lk, stock + 1
    if not stock:
        print("bar judge: COULD NOT RUN: no stock setup's games to judge")
        return 2
    verdict = 2 if lacking else 1 if fail else 0
    print(f"bar judge: {('PASS', 'FAIL', 'COULD NOT RUN')[verdict]}")
    return verdict


# ---- A2b's row checks ----

# a2-spec.md section 9: the pair, the bound each game is played to, the games and maps it asks, and the games `row`
# plays -- b4's two of each start on the land and the mixed map, b5's one a map, both starts and all four side
# pairings among them
ROWS = {
    "b4": {"pair": "hota", "cap": 27000, "games": 4, "maps": 2,
           "ids": ["bar-hota-pincushion-ac-1", "bar-hota-pincushion-ca-2", "bar-hota-troutfarm-aa-1",
                   "bar-hota-troutfarm-cc-2"]},
    "b5": {"pair": "hota", "cap": 36000, "games": len(BAR_MAPS), "maps": len(BAR_MAPS),
           "ids": ["bar-hota-pincushion-aa-1", "bar-hota-redtriangle-ac-1", "bar-hota-troutfarm-ca-1",
                   "bar-hota-lavamania-cc-1", "bar-hota-plainspasses-ac-2"]},
}
# b5's "an army of each wanted role": the classes the seat's table wants depend on its picture and its factories'
# menus at each decision, and no line logs them, so the check asks for a composition of at least this many
# classes completed, and prints the classes for the reader.
ROLES_MIN = 2
SEA_MAPS = ("redtriangle",)             # the bar's sea map: a yard is asked there whatever ImpAI judged


def yard_verdict(r: dict):
    """(met, verdict, detail) of b5's yard for side A, judged at its second-factory decision (r["second"]["A"],
    THE ROW CHECKS): asked on the sea map and wherever ImpAI judged the map naval at that decision. "yard
    ordered": it chose a sea plant; "naval, no site": it chose a land plant because the decision's sea search found
    no candidate at all (met, and listed by check); "yard missing": any other way, a land plant or none ordered;
    "not asked" elsewhere."""
    sec, end = r["second"]["A"], r["end"]["A"]
    at = (f"{'the level end' if sec['chose'] == 'none' else 'the second factory'} at GameTime {sec['gametime']}, "
          f"naval={sec['naval']}, sea search {sec['sea_site']}; {end.get('yards') or 0} yards completed")
    if sec["chose"] == "sea":
        return True, "yard ordered", at
    if r.get("map") not in SEA_MAPS and sec["naval"] != 1:
        return True, "not asked", at
    if sec["chose"] == "land" and sec["naval"] == 1 and sec["sea_site"] == "none":
        return True, "naval, no site", at
    return False, "yard missing", at


def score(row: str, r: dict) -> list:
    """[(criterion, met, detail)] of row b4 or b5 for side A of a result that ran to the row's bound."""
    end, first = r["end"]["A"], (r.get("first") or {}).get("A") or {}
    if row == "b4":
        st = (r.get("stalls") or {}).get("A") or []
        return [("attack wave", first.get("attack") is not None,
                 f"GameTime {first['attack']}" if first.get("attack") is not None else "none"),
                ("scout", first.get("scout") is not None,
                 f"GameTime {first['scout']}" if first.get("scout") is not None else "none"),
                ("no stall", not st, f"{len(st)} stall windows, the first from {st[0]['from']}" if st else "none")]
    army = {k: n for k, n in (end.get("army") or {}).items() if n > 0}
    roles = ("roles", len(army) >= ROLES_MIN,
             " ".join(f"{k}:{n}" for k, n in sorted(army.items())) or "no army unit completed")
    met, verdict, at = yard_verdict(r)
    yard = ("yard", met, f"{verdict}: {at}")
    income, tech_m = (r.get("income_max") or {}).get("A"), end.get("tech_m")
    high = sorted(int(t) for t, n in (end.get("factories") or {}).items() if n > 0 and t.isdigit())
    factories = " ".join(f"tech {t}:{n}" for t, n in sorted((end.get("factories") or {}).items())) or "none"
    if income is not None and tech_m and income >= tech_m:
        tech = ("tech-up", bool(high) and high[-1] >= 2,
                f"best income {income} a pass against tech_m {tech_m}; factories {factories}")
    else:
        tech = ("tech-up", True, f"not asked: best income {income} a pass against tech_m {tech_m}; "
                                 f"factories {factories}")
    return [roles, yard, tech]


def check(row: str, dirs) -> int:
    """Row b4's or b5's criteria over every game in `dirs`: exit 0 when every game meets each, 1 when one does
    not, 2 when a game could not run, is not the row's (another pair, another bound, no level-end lines of A's, for
    b5 no `impai: second` line of A's), or the games are fewer than the row asks. Each game whose yard was met as
    "naval, no site" is listed by name."""
    spec = ROWS[row]
    rows = read_results(dirs)
    problems, played, fail, no_site = [], [], 0, []
    for r in rows:
        gid = r.get("id", "?")
        pair = gid.split("-")[1] if gid.count("-") == 4 else None
        if pair != spec["pair"]:
            problems.append(f"{gid}: not of the pair {spec['pair']} (ImpAI Hard against OTA AI Hard)")
        elif r.get("outcome") == "could_not_run":
            problems.append(f"{gid}: could not run: {r.get('reason')}")
        elif r.get("cap") != spec["cap"]:
            problems.append(f"{gid}: played to GameTime {r.get('cap')}; {row} plays to {spec['cap']} (--cap)")
        elif "A" not in (r.get("end") or {}):
            problems.append(f"{gid}: no level-end line of A's")
        elif row == "b5" and "A" not in (r.get("second") or {}):
            problems.append(f"{gid}: no `impai: second` line of A's, which the yard is judged from")
        else:
            got = score(row, r)
            played.append(r)
            fail += not all(ok for _, ok, _ in got)
            if any(c == "yard" and d.startswith("naval, no site") for c, _, d in got):
                no_site.append(gid)
            print(f"{gid}: {r['outcome']} at {r.get('end_tick')}: "
                  + "; ".join(f"{c} {'met' if ok else 'NOT MET'} ({d})" for c, ok, d in got))
    maps = {r["map"] for r in played}
    if len(played) < spec["games"] or len(maps) < spec["maps"]:
        problems.append(f"{len(played)} games on {len(maps)} maps; {row} asks {spec['games']} games on "
                        f"{spec['maps']} maps or more")
    if no_site:
        print(f"  naval, no site (the yard met without one): {' '.join(no_site)}")
    for p in problems:
        print(f"  {p}")
    verdict = 2 if problems else 1 if fail else 0
    print(f"bar check {row}: {('PASS', 'FAIL', 'COULD NOT RUN')[verdict]}"
          + (f": {fail} of {len(played)} games miss a criterion" if fail else ""))
    return verdict


# ---- the command line ----

def read_ids(arg: str) -> list:
    """`play --ids`: a pair's name (its ids), a file of ids, or a comma list."""
    try:
        return ids(arg)
    except BarError:
        pass
    try:
        is_file = Path(arg).is_file()
    except OSError:             # a comma list longer than a file name can be (ENAMETOOLONG) is no file
        is_file = False
    if is_file:
        return [x for x in Path(arg).read_text().split() if x]
    return [x for x in arg.split(",") if x]


def default_out(a, what=None) -> Path:
    """`--out`, else .data/impai/bar/[<setup>-][<what>-]<time>: a setup's games in a directory of their own (THE
    SETUP)."""
    name = "-".join(x for x in (a.setup, what, time.strftime("%Y%m%d-%H%M%S")) if x)
    return a.out or TREE / ".data" / "impai" / "bar" / name


def settings_override(items: list) -> dict:
    """{A or B: the persona lever's row or None} of `play --persona` items `<seat>=<settings>`, each a PERSONA_ROWS
    row or HARD_ROW (no persona: the DLL's own level); BarError for any other item or a seat named twice."""
    out = {}
    for item in items or []:
        seat, _, row = item.partition("=")
        if seat not in ("A", "B") or seat in out or (row != HARD_ROW and row not in PERSONA_ROWS):
            raise BarError(f"--persona {item!r}: <A|B>=<{HARD_ROW} or a row of {', '.join(PERSONA_ROWS)}>, a seat once")
        out[seat] = PERSONA_ROWS.get(row)
    return out


def with_settings(game: dict, override: dict) -> dict:
    """`game` with each seat of `override` on its settings (settings_override): the id, and so the seed, unchanged,
    so two settings play the same games; a result carries the seat's persona, which game_kept compares, so a result
    of other settings is never kept. BarError for a seat ImpAI does not play."""
    if not override:
        return game
    seats = {s: dict(v) for s, v in game["seats"].items()}
    for s, persona in override.items():
        if seats[s]["ai"] != "impai":
            raise BarError(f"{game['id']}: --persona {s}: seat {s} is {seats[s]['ai']}, not ImpAI")
        seats[s].pop("persona", None)
        if persona:
            seats[s]["persona"] = persona
    return dict(game, seats=seats)


def cmd_play(a) -> int:
    values = [int(t) for t in a.values.split(",") if t] if a.values else []
    override = settings_override(a.persona)
    return play_games([with_settings(dict(parse_id(g), cap=a.cap, setup=a.setup, values=values), override)
                       for g in read_ids(a.ids)], a, default_out(a))


def cmd_row(a) -> int:
    """Row a.row's games (ROWS) played to its bound into a directory of their own, then checked there: 2 when the
    play could not run or was stopped, else check's verdict."""
    spec = ROWS[a.row]
    out = default_out(a, a.row)
    rc = play_games([dict(parse_id(g), cap=spec["cap"], setup=a.setup) for g in spec["ids"]], a, out)
    return rc if rc else check(a.row, [out])


def play_games(games: list, a, out: Path) -> int:
    """`games` played on a.lanes lanes into `out`, those already there skipped (RESUMING), under the run lock, on
    the setup a.setup names (THE SETUP); BarError when `out` holds another setup's results"""
    import runlock
    setup = setup_of(a.setup)
    other = sorted({r.get("setup") or "" for r in read_results([out])} - {a.setup or ""}) if out.is_dir() else []
    if other:
        raise BarError(f"{out} holds games of {', '.join(setup_label(x) for x in other)}; a directory holds one "
                       f"setup's games")
    out.mkdir(parents=True, exist_ok=True)
    if a.dll:
        _DLL["path"] = Path(a.dll).resolve()
    _PACING["draw"] = a.draw
    _SETUP["setup"] = setup
    _KEEP["all"] = a.keep_all
    had = {g["id"]: game_kept(out, g, a.gpu, a.draw) for g in games}
    kept = [r for r in had.values() if r]
    left = [g for g in games if not had[g["id"]]]
    if len(left) < len(games):
        print(f"bar play: {len(games) - len(left)} games already in {out}, {len(left)} to play")
    games, skipped = left, []

    def give_up(g, done):
        """THE CURTAILMENT, unless --no-curtail: a ladder pair's game is not played once the pair has failed"""
        if getattr(a, "no_curtail", False) or not curtailed(g["pair"], kept + done):
            return False
        skipped.append(g["id"])
        return True
    runlock.hold(TREE, f"bar {a.verb} " + " ".join(sys.argv[2:]), lanes=a.lanes, gpu=a.gpu)
    import tareplay as R
    try:
        results = play_all(games, a.lanes, out, a.gpu, give_up=give_up)
    finally:
        close_lanes()
        runlock.release()
    if skipped:
        print(f"bar play: {len(skipped)} games not played: their ladder pair failed for certain (THE CURTAILMENT; "
              f"--no-curtail plays them)")
    bad = [r["id"] for r in results if r.get("outcome") == "could_not_run"]
    if R.STOP.is_set():
        print(f"bar play: STOPPED: {len(results)} games ended in {out}, {len(bad)} of them stopped or could not "
              f"run; {len(games) - len(results)} not started")
        return 2
    print(f"bar play: {len(results)} games played in {out}, {len(bad)} could not run")
    return 2 if bad else 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="verb", required=True)
    i = sub.add_parser("ids", help="the game ids of a pair")
    i.add_argument("--pair", required=True)
    i.add_argument("--maps", default=None)
    i.add_argument("--blocks", type=int, default=None,
                   help="a block pair's blocks (THE BLOCKS); by default its run set's: eoh and noh 12, ne and hn "
                        "10, a calibration row 4, a mirror pair 10")
    pl = sub.add_parser("play", help="play a list of games on L lanes")
    pl.add_argument("--ids", required=True, help="a file of ids, a comma list, or a pair's name")
    pl.add_argument("--cap", type=int, default=CAP,
                    help="the GameTime a game ends at: a row's bound (b4 27 000, b5 36 000, `check`) or a "
                         "shorter piece's; the bar's games end at 81 000")
    pl.add_argument("--persona", action="append", default=[], metavar="SEAT=SETTINGS",
                    help=f"play seat A or B on the persona row SETTINGS, or {HARD_ROW} for none, in every game, the "
                         f"ids and seeds unchanged (the paired margin test's settings arms)")
    pl.add_argument("--values", default=None, metavar="T,T",
                    help="also read every seat's value at these GameTimes from each game's recording, those the "
                         "game reached before its end (`values` in the result; the paired margin test's report)")
    pl.add_argument("--no-curtail", action="store_true",
                    help="play a ladder pair's games on after it has failed for certain (THE CURTAILMENT), for "
                         "the ordering's later looks")
    rw = sub.add_parser("row", help="A2b's row b4 or b5: its games played to its bound, then checked")
    rw.add_argument("row", choices=sorted(ROWS))
    for x in (pl, rw):
        x.add_argument("--lanes", type=int, required=True)
        x.add_argument("--setup", default=None,
                       help="play on this compat setup's folder (tools/compat/setups.json, THE SETUP); without it "
                            "the base game")
        x.add_argument("--out", type=Path, default=None)
        x.add_argument("--dll", default=None)
        x.add_argument("--gpu", action="store_true",
                       help="render on the default renderer, for debugging, taking a GPU token a lane; without it "
                            "the games render with GDI and take none")
        x.add_argument("--draw", action="store_true",
                       help="render every frame, for debugging (THE PACING); --gpu implies it")
        x.add_argument("--keep-all", action="store_true",
                       help="keep every game's log and recording, a win included (a paired bar's arms)")
    j = sub.add_parser("judge", help="the verdict over the games in DIR...")
    j.add_argument("dirs", nargs="+", type=Path)
    c = sub.add_parser("check", help="A2b's row b4 or b5 over the games in DIR...")
    c.add_argument("row", choices=sorted(ROWS))
    c.add_argument("dirs", nargs="+", type=Path)
    a = p.parse_args(argv)
    try:
        if a.verb == "ids":
            print("\n".join(ids(a.pair, a.maps.split(",") if a.maps else None, a.blocks)))
            return 0
        if a.verb == "judge":
            return judge(a.dirs)
        if a.verb == "check":
            return check(a.row, a.dirs)
        if a.verb == "row":
            return cmd_row(a)
        return cmd_play(a)
    except BarError as e:
        print(f"bar: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
