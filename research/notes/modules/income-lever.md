# The income lever

One seat's production times a multiplier: the handicap ladder's lever (`tagpu_income.c`).

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`). `main` = `*(void**)0x511DE8`.

## What it is

A harness lever, never a player setting. The handicap ladder (`tools/impai/ladder.py`, [the bar](../impai/bar.html), *The handicap ladder*) needs OTA AI richer than a player: the Difficulty only scales a computer seat down, ×0.5 on Easy and ×0.7 on Medium, and leaves Hard unscaled ([sim](../engine/sim.html), *Difficulty and the AI handicap*). No table or setting of the engine's sets one seat's income.

The test lever `tagpu_impai_income.on` in the game folder names seats and their multipliers m, one item `<seat>=<m>` a line or comma-separated, m a decimal with at most three places in 1..8. `tools/impai/level_lever.py --income` writes it, refusing what the DLL would refuse. Any seat can be named, whoever plays it. `bar.py`'s handicap pairs `h:<m>` arm OTA AI's seat.

It sits in `tagpu_impai_*.on`'s name space so that the harnesses' lever sweeps remove it with ImpAI's: tareplay's `clear_impai_levers` before every fixture, and bar.py's lever writes before every game. A lever left behind would multiply the next game's seat without a word.

## The engine

The economy pass `0x401360(player)` (stdcall, `ret 4` at `0x401C1C`) runs once a 30-tick block. It is called for a record of type 1 or 2 only (`0x465539..0x46553F`), from its only call `0x46555A`, in the players phase, on the GAME thread ([network](../engine/network.html), the economy pass).

It does five things, in order:

1. writes the pass's production as the rates `+0x90` (energy) and `+0x9C` (metal);
2. adds the rates to the double totals `+0xAC`/`+0xB4`;
3. adds them to the stores `+0x8C`/`+0x98`;
4. takes the consumption up to what is stored, keeping the fraction met (`0x401A4D..0x401AB1`);
5. clamps each store to its maximum `+0xA4`/`+0xA8`, adding the excess to the double wasted `+0xCC`/`+0xD4`.

**Each rate includes the player's ledger.** The ledger is the `+0xEC` record. Its produced float, `+0x00` for energy and `+0x18` for metal, is folded into the units' sum (`0x401914`/`0x401927`; `0x40191F`/`0x401954`). A transfer credits exactly that float on the recipient (`0x464C44`, `0x464D77`), so a give is counted as the recipient's production at its next pass.

## The wrapper

The call at `0x46555A` is redirected at every attach, byte-matched. `econ_pass(rec)` works in three steps:

1. It bounds `rec` to one of main's ten records.
2. For an armed seat, it reads the ledger's two produced floats, then calls the pass.
3. For a seat whose row k is not 0 (m = 1 + k / 1000) and which is not a puppet seat, it computes B = the rate less the ledger's float, for each resource. B is the units' production. It then adds (m − 1) × B to the rate, the total and the store, and clamps the store as the pass does: strictly above the maximum, the excess to wasted.

A B that is not finite, or not positive, adds nothing. A seat with no row gets the stock call and nothing else, and with no lever every row is 0.

**What is multiplied** is everything the pass counts as production, less the gives. That covers:

- structures' and mobile units' negative EnergyUse;
- extractors, makers, wind and tide;
- reclaim payouts and refunds, which reach the pass through the units' ledgers.

This is the same set the Difficulty scales at its rows 1 to 12 and 17. The stock AI's planners read the rates (`0x408691`, `0x40874A`, `0x40BC53..0x40BCC5`), so the seat plans on its multiplied income, as on a mod that scales it.

**The stall, a property of the design.** The bonus reaches the store after the pass's own consumption step. In a pass whose consumption exceeds the store plus the production, the fraction met is the stock one, and the bonus is spent at the next pass, 30 ticks later.

- It is deterministic and monotone in m.
- The total bonus is the same; only the pass in which it is spent differs.

Applying the bonus before that step needs a patch inside the pass's x87 sequence, which is not taken. At `0x4019B0` the energy rate exists only as the FPU stack's top, and the pass stores it at `0x4019C6`. Crediting the ledger instead would delay every bonus by a pass, not only a stalled one.

## State and replay

`s_bonus`, ten u16 rows by player, is `SIM_STATE`.

- At every level init, on the LOADER thread at the PLAYERS call (`tagpu_replay.c`), it is cleared.
- On a live skirmish level it is then written from the lever, logging `income: seat=<k> mult=<m> from=lever` for each armed seat.
- On a puppet level the recording is the only writer, and the pass never runs for a puppet seat (type 3), which the wrapper also refuses.
- In a network game the lever is ignored and says so, because each seat's pass runs on its own peer.

A recording therefore carries the rows. A replay, a round trip, a seek or a takeover multiplies by the recorded rows with no lever, and a takeover goes on at the recorded multipliers. The lever itself (`SIM_INSTALL`) is read once, at attach.

The level end logs `income: level end seat=<k> mult=<m> passes=<n> energy=<sum> metal=<sum> gametime=<t>` for each armed seat; those totals are `SIM_PRIVATE`.

## The test group

`income-mult` (`scenarios/income-mult.json`, `tools/replay/fixtures.json`) is played on Town & Country at Hard:

- seats 1 and 2 are OTA AI on CORE, and the content arms seat 1 at ×2.5;
- the idle human gives each 100 energy and 100 metal at GameTime 35;
- at 75 `tools/impai/income_check.py` asserts that seat 1's rates less the give are 2.5 times seat 2's;
- at 300 the handicap check's economy structures and two A.K.s a seat are applied;
- it is recorded and played as record, puppet, round trip, seek and takeover.

MEASURED 2026-11-16, the reference setup: the assertion read energy 162.5 against 125.0 and metal 102.5 against 101.0. That is 100 given plus 2.5 × the commander's 25 and 1, the give unmultiplied. Every test of the group passed, and the level end counted 181 armed passes. The group also covers the stock path: seat 2's pass and the human's go through the wrapper with no row.

## Gaps

- `tasim_econ.c`'s port of the pass has no lever: tasim tunes nothing against OTA AI (`tools/tasim/engine-features.tsv`).
- The cheat flag in `bar.py` reads an armed OTA AI seat's income divided by its multiplier. A mod that also scales the seat's income still shows, but only as the ratio beyond m.
