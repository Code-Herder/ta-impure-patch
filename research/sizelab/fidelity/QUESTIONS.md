# Questions for ta-scout agents (fidelity research, demo recorder size)

Context: a reduced demo-recording stream would keep a unit's position x, altitude, z (three
int32 16.16 values at unit +0x6A, +0x6E, +0x72; the high word of each is whole world units)
exact in the HIGH WORD at every tick and drop the low bits of the fraction. That is safe for a
puppet (a unit driven by the recording on a playback host) only if no engine code that runs on
a puppet host reads the low 16 bits of those three dwords in a way that reaches recorded state
or the derived fields +0x76/+0x78 (footprint cell), +0x7A/+0x7C/+0xF8 (sight stamp), +0x82 (sort
bucket), +0x10A (water state). Already established by disassembly (no need to redo): set-position
0x48A9F0 (cell at 0x48AA0C..0x48AA32), the bucket choice 0x47CCA9..0x47CCC6, the ray-fan sight
emitter 0x4825B0 (0x4825D4..0x48261C) and its wrapper 0x4827B0 (raise at 0x48280D..0x48281B),
the water state 0x43DB50 (0x43DB71), the stock unit draw 0x459233..0x459277.

Binary: `"$(git rev-parse --git-common-dir)/../pristine/TotalA.exe.pristine"`, image base
0x400000, `i686-w64-mingw32-objdump -d -M intel --start-address=0x… --stop-address=0x… <exe>`.
Engine map: `research/notes/exe-reverse-engineering.md`. Report every fact with its address and
DISASSEMBLED / MEASURED / [INFERRED].

## Batch 1 (independent): readers of the position on a puppet host

For each function below, list every read of unit +0x6A..+0x75 (or of a copy of those dwords),
and say whether the value used depends on the low 16 bits of x, altitude or z (a full-dword
compare, an add before a shift, a float conversion), or only on the high words (`movsx word
[+0x6C]`, `sar 16` or more after adding only multiples of 0x10000, etc.):

1. The circle-mode branch of the sight emitter 0x4825B0 (from 0x4826B7) and the stamp 0x482270
   and removal 0x481D50 as they use the record built by 0x4827B0.
2. The claim 0x47CC30 beyond the bucket (its cell and yard reads), and the release 0x47D0E0.
3. The altitude fix-up 0x48A870 and 0x48A490 (corner heights), when they run on a type-3
   owner's unit after the set-position sets +0x110 bit 16: does the unit tick call 0x48A870 for
   type-3 owners (0x48AFB0's condition)?
4. The minimap picture 0x466DC0 and the radar pass 0x467440.
5. Any other function reachable on a puppet host every tick that reads +0x6A..+0x75 of an
   arbitrary unit: the players phase 0x464F80's callees, the projectile tick 0x49B720's
   collision 0x49B090 and guidance 0x49B48E (overwritten by the replay afterwards, but say if a
   result is stored outside the projectile pool), and the COB getters that read a unit's
   position (UNIT_XZ 0x480868 and any others) -- whether any of their results reaches a
   recorded field on a puppet, given that COB's engine writes are gated there
   (tagpu/ddraw/src/tagpu_puppet.c, THE GATES).

## Batch 2 (independent): the mover's velocity

6. Where the local ground mover (step 0x44F1A0, the ground step 0x43CD20, 0x43CC20) writes the
   move object's velocity +0x08/+0x0C/+0x10 and the speed +0x20: the formula from the speed and
   the heading +0x66, including the floating-point calls (0x4FB440, 0x4E43A0, 0x4E4400,
   0x4E43D0, 0x4E44F0, 0x4E4440) -- what each is, and whether the result is a pure function of
   (speed, heading, def fields) that a C or Python decoder could reproduce bit-exactly. The data
   says: direction = heading + 180 degrees within 0.52 degrees (p99), length = speed.
