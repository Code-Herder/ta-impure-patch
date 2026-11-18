# The duel lever

A network game that does not end for want of a human: the code duel's lever (`patch_duel_lever`, `tagpu_patches.c`).

*A module note: it states the module as it is now. A landing that changes the module edits its section here in place; what the landing measured and what its review found go to the commit, its `landing-review:` note and the gate's plan note.*

Addresses are VAs for our pristine build (ImageBase `0x400000`). `main` = `*(void**)0x511DE8`.

## What it is

A harness lever, never a player setting. A code duel (`tools/impai/codeduel.py`, [the bar](../impai/bar.html), *The champion gauntlet*) plays two builds of ImpAI against each other in a two-peer network game, each peer running the computer seat it added. The engine ends a network game once no human still in it holds a unit, whatever the computer seats hold ([network](../engine/network.html), *The network game's end test*), so without the lever each duel keeps both humans on the map. With it the two computer seats play alone and the harness ends the game.

The test lever `tagpu_impai_duel.on` in the game folder, read once at attach and honoured only in a test launch (`tagpu_regstore_active`, as the other test levers are), arms it; the file's text is not read. `codeduel.py --no-humans` writes it into both peers' game folders before their launch (`TwoPeers.launch`'s `levers`).

It sits in `tagpu_impai_*.on`'s name space so that the harnesses' lever sweeps remove it with ImpAI's: tareplay's `clear_impai_levers` before every fixture and when a worker closes. A code duel's instances are made for the game and removed after it.

## The engine

After its loop over the ten player records, the players phase `0x464F80` makes the end test on every call:

| VA | Bytes | What it is |
|---|---|---|
| `0x4655C0` | `E8 3B FB FC FF` | `call 0x435100`, GameingState's mode |
| `0x4655C5` | `83 F8 03` / `75 71` | `cmp eax,3` / `jne 0x46563B`: a network game only |
| `0x4655CA` | `8B 0D E8 1D 51 00` / `83 B9 F6 7E 03 00 02` / `74 62` | `cmp dword [main+0x37EF6],2` / `je 0x46563B`: not while ActiveCommanderDeath is 2, Deathmatch |
| `0x4655D9` | `E8 D2 26 FF FF` | `call 0x457CB0`, the humans still in the game; a computer seat is never counted |
| `0x4655DE` | `85 C0` / `75 59` | `test eax,eax` / `jne 0x46563B`: some, so the phase returns |
| `0x4655E2..0x46563A` | | none: the countdown `main+0x39239`, then the stop and lost bits of `main+0x3923B` |

The phase runs on the GAME thread in play, and once on the LOADER thread during a load (`0x497C10`). DISASSEMBLED; the count and the tail are in [network](../engine/network.html), *The network game's end test*.

## The patch

One byte: `0x4655E0`'s opcode `75` becomes `EB`. The `jmp 0x46563B` is always taken, so the phase returns as it does while a human still plays, and the tail `0x4655E2..0x46563A`, which no jump from outside enters, never runs. The count is still called and its answer ignored. Its other call, `0x4657E8` (whether a player who is out sees the watch notice), keeps its answer.

**Safe by construction, two ways.**

- *An ordering.* The byte is written from DllMain, by `tagpu_apply_patches`, before `TotalA.exe`'s entry point: no level exists and no thread runs the players phase.
- *The write itself.* It is one byte, the opcode of a two-byte instruction whose displacement stays, so at every moment the site holds a whole `jne 0x46563B` or a whole `jmp 0x46563B`.

**Installed only on the bytes read above.** `0x4655C5..0x4655E1`, the whole test from the mode's compare to the `jne`, is matched byte for byte first. On any other bytes — another executable, a patch loader that rewrote the test — nothing is written, the log says `duel: tagpu_impai_duel.on NOT installed: 0x4655C5..0x4655E1 does not hold the retail exe's network end test`, and the game ends as that executable ends it. With no lever file, or outside a test launch (`refused: not a test launch`), nothing is written either. The installed line is `duel: TEST LEVER tagpu_impai_duel.on -- installed: a network game no longer ends for want of a human (0x4655E0 jne -> jmp)`; `codeduel.py`'s judge fails a `--no-humans` game whose two logs do not both hold it.

## What still ends the level

Everything but the end test, as stock ([network](../engine/network.html), *The other ends of a network level*):

- **A human whose record holds no unit goes out** on its own 30-tick pass: with a computer seat of its own peer still in the game it becomes a watcher of the level, which goes on; with none, the level ends on that peer, lost.
- **A human who is out wins with its allies**: once every seat that still holds units is its ally, allied to all the others, the level ends on that peer, won. Two computer seats that are not allied never satisfy it.
- **A battle-room watcher runs neither test**: the phase passes over its record. With every human watching, nothing in the phase ends the level.
- **A peer that leaves** by its menu ends its own level; its seats are removed on the other peers, whose levels go on. That is how the harness ends a duel.

## State and replay

None of its own. The lever (`SIM_INSTALL`, the registry's lever list) is read once, at attach, and the site is engine code written before any level, as every byte patch of `tagpu_patches.c` is. The feature is `duel_lever`, `SIM_PUPPET_RUNS`: a level played under the lever runs the patched test on a puppet as on its owner. A network game keeps no recording today, so no replay test group plays an armed level; the function is reached at attach by every group.

## Gaps

- No replay test group arms the lever: what an armed level does is measured by `codeduel.py --no-humans` alone.
