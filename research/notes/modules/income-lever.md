# The income lever

One seat's production times a multiplier: the handicap ladder's lever (`tagpu_income.c`).

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`). `main` = `*(void**)0x511DE8`.

## What it is

A harness lever, never a player setting. The handicap ladder (`tools/impai/ladder.py`, [the bar](../impai/bar.html), *The handicap ladder*) needs OTA AI richer than a player: the Difficulty only scales a computer seat down, ×0.5 on Easy and ×0.7 on Medium, and leaves Hard unscaled ([sim](../engine/sim.html), *Difficulty and the AI handicap*). No table or setting of the engine's sets one seat's income.

The test lever `tagpu_impai_income.on` in the game folder, honoured only in a test launch (`tagpu_regstore_active`, as the pacing levers are) and only at a new skirmish's init, names seats and their multipliers m, one item `<seat>=<m>` a line or comma-separated, m a decimal with at most three places in 1..8. `tools/impai/level_lever.py --income` writes it, refusing what the DLL would refuse. Any seat can be named, whoever plays it. `bar.py`'s handicap pairs `h:<m>` arm OTA AI's seat.

It sits in `tagpu_impai_*.on`'s name space so that the harnesses' lever sweeps remove it with ImpAI's: tareplay's `clear_impai_levers` before every fixture and when a worker closes, and bar.py's lever writes before every game. A lever left behind would multiply the next game's seat without a word.

## The engine

The economy pass `0x401360(player)` (stdcall, `ret 4` at `0x401C1C`) runs once a 30-tick block. It is called for a record of type 1 or 2 only (`0x465539..0x46553F`), from its only call `0x46555A`, in the players phase ([network](../engine/network.html), the economy pass). The players phase runs on the GAME thread in play, and once on the LOADER thread during a load (`0x497C10`), after the level init and before the game thread's first step (bit 1 of `main+0x38D75`, set `0x497C5F`, polled `0x498342`).

It does six things, in order:

1. sums the units' production and the ledger's produced floats (below), and writes the sums as the rates `+0x90` (energy) and `+0x9C` (metal);
2. adds the rates to the double totals `+0xAC`/`+0xB4`;
3. adds them to the stores `+0x8C`/`+0x98`;
4. takes the consumption up to what is stored, keeping the fraction met (`0x401A4D..0x401AB1`);
5. clamps each store to its maximum `+0xA4`/`+0xA8`, adding the excess to the double wasted `+0xCC`/`+0xD4`;
6. zeroes the ledger's produced floats (`0x401BAE..0x401C16`).

**Each rate includes the player's ledger.** The ledger is the `+0xEC` record. Its produced float, `+0x00` for energy and `+0x18` for metal, is folded into the units' sum (`0x401914`/`0x401927`; `0x40191F`/`0x401954`). A transfer credits exactly that float on the recipient (`0x464C44`, `0x464D77`), so a give is counted as the recipient's production at its next pass.

## The wrapper

The call at `0x46555A` is redirected at every attach, byte-matched. **The bonus goes in through the ledger, as a give does.** `econ_pass(rec)` works in three steps:

1. It bounds `rec` to one of main's ten records.
2. For a seat whose row k is not 0 (m = 1 + k / 1000) and which is not a puppet seat, it adds (m − 1) × B_prev to each of the ledger's produced floats, B_prev the seat's units' production in its previous pass. Then it calls the pass.
3. For that seat, it keeps B = the rate less the ledger's float as the pass read it (the gives and the bonus both in it), for each resource: the units' production of this pass, the next pass's B_prev. A B that is not finite, or not positive, is kept as 0.

The pass then treats the bonus as production in every step: the rate, the totals, the store, the stall fraction, the clamp and the waste. The stock AI's planners read the rates (`0x408691`, `0x40874A`, `0x40BC53..0x40BCC5`), so the seat plans on its multiplied income, as on a mod that scales it. A seat with no row gets the stock call and nothing else, and with no lever every row is 0.

**Neither a give nor the bonus is multiplied**, because B is taken net of the whole ledger, and the bonus is never compounded. **Every bonus arrives one pass, 30 ticks, after the production it multiplies**, and none is lost: a level's first pass carries none, and a seat's last production before the level ends is never paid. That delay is uniform. In a stall, the bonus is spent at the pass that credits it, exactly as production would be. The alternative, adding the bonus in the same pass, needs a patch inside the pass's x87 sequence: at `0x4019B0` the energy rate exists only as the FPU stack's top, and the pass stores it at `0x4019C6`.

**The ledger, bounded.** The pointer at `+0xEC` is the one the pass dereferences unconditionally in the same call (`0x40190E`). The player init makes it when there is none (`0x4647CD..0x4647D7`), and only the level teardown's `0x464A00` frees it (`0x464A76`, called at `0x491BA9`), which never runs inside the players phase. The wrapper reads it on the thread about to make that call, and leaves a NULL one alone.

**What is multiplied** is everything the pass counts as the units' production. That covers:

- structures' and mobile units' negative EnergyUse;
- extractors, makers, wind and tide;
- reclaim payouts (Difficulty rows 11, 12 and 17, [sim](../engine/sim.html), *Difficulty and the AI handicap*);
- the refunds of a cancelled build and of unbuilding (rows 9 and 10): at m > 1 a seat gets back m times the metal it put in;
- the Deathmatch respawn's grants to the new commander (rows 15 and 16).

That is the set the Difficulty scales at its rows 1 to 12 and 15 to 17; the gives (rows 13 and 14) are left out.

## State and replay

`s_bonus`, ten u16 rows by player, and `s_prev`, the ten seats' B_prev pairs, are `SIM_STATE`.

- At every level init, on the LOADER thread at the PLAYERS call (`tagpu_replay.c`), both are cleared.
- In a test launch, at a new skirmish's init (GameingState's mode 2 and no saved game, the loader's own test at `0x4978DA..0x497902`), `s_bonus` is then written from the lever, logging `income: seat=<k> mult=<m> from=lever` for each armed seat. Any other level (a campaign, a saved game, a network game, whose passes run on each seat's own peer) ignores the lever and says so; outside a test launch the lever is ignored at attach.
- On a puppet level the recording is the only writer, and the pass never runs for a puppet seat (type 3), which the wrapper also refuses.

A recording therefore carries both. A replay, a round trip, a seek or a takeover multiplies as recorded with no lever, and a takeover goes on at the recorded multipliers. The lever itself (`SIM_INSTALL`) is read once, at attach.

The level end, on the GAME thread in the teardown, logs `income: level end seat=<k> mult=<m> passes=<n> energy=<sum> metal=<sum> gametime=<t>` for each armed seat, the sums being the bonus credited; those totals are `SIM_PRIVATE`.

## The test groups

Both play on Town & Country at Hard and are recorded and played as record, puppet, round trip and seek; `income-mult` also as takeover.

**`income-mult`, the multiplier and the gives** (`scenarios/income-mult.json`). Seats 1 and 2 are OTA AI on CORE, and the content arms seat 1 at ×2.5.

- The idle human gives each 100 energy and 100 metal at GameTime 35.
- At 75, `tools/impai/income_check.py check` asserts that seat 1's rates less the give are 2.5 times seat 2's. The passes fall at 1 (the loader's), 31 and 61, so the pass at 61 carries 1.5 × the commander's production of the pass at 31, and the give unmultiplied.
- At 300 the handicap check's economy structures and two A.K.s a seat are applied, so every kind of production the pass sums runs under the multiplier.

MEASURED 2026-11-16, the reference setup: the assertion read energy 162.5 against 125.0 and metal 102.5 against 101.0. That is 100 given plus 2.5 × the commander's 25 and 1: exact, one pass late, the give unmultiplied. The group also covers the stock path: seat 2's pass and the human's go through the wrapper with no row.

**`income-stall`, a stalled seat** (`scenarios/income-stall.json`). Seats 1 and 2 are CORE with the AI off, a base energy storage of 10 and nothing stored, and each has an Automatic Radar Targeting (EnergyUse 1300). Seat 1 has one solar collector, 45 energy a pass with its commander's, armed at ×5; seat 2 has ten, 225 a pass of its own. From GameTime 330, `income_check.py stall` advances 600 ticks and asserts that both seats' rates are equal and that seat 1 consumed what seat 2 consumed, within the two ends' phase (twice the rate plus the storage). A bonus credited after the pass would have been clamped to the storage of 10 and wasted, leaving seat 1 under a quarter of seat 2's.

MEASURED 2026-11-16, the reference setup: seat 1 consumed 4294.0 and seat 2 4304.0 over the 600 ticks (bound 470.0), each wasting 196.0. The effective multiplier in a stall is the armed one.

The stalled seats have no takeover test. MEASURED 2026-11-16 with them in `income-mult`: after a takeover at 391, `tarec compare-fork` found the two seats' player images differing from the recording at `+0x1E0`, 01 recorded and 02 in the fork [INFERRED: the seat's type, a computer player again], and the takeover's AI check found no orders from them. Both are the takeover's handling of a seat the console switched off (`ai: "off"`), not the lever's: every other test passed with those seats.

## Gaps

- `tasim_econ.c`'s port of the pass has no lever: tasim tunes nothing against OTA AI (`tools/tasim/engine-features.tsv`).
- The cheat flag in `bar.py` reads an armed seat's income at the recording's first iteration, which the lever's first bonus never reaches. A mod that also scales the seat's income shows there as itself.
