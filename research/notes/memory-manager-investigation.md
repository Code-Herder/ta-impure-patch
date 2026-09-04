# Memory Manager Investigation

*Research date: 2026-09-04. The question that started it: **do we use the whole 4 GB a 32-bit
process can address?** The answer is no — and the more useful finding is where the low 2 GB
actually goes, which is neither TA nor us.*

Every number here was measured on this machine in one session, against the live stack
(wine-9.0, NVIDIA 595.84 on an RTX 4070, `tacli` instance `spike1`, pid 1446775, windowed
`renderer=openglcore`, game resolution 1024×768, native pass at `ss=2`). The exe is the
untouched Steam build — 1,178,624 bytes, md5 `8e74a1dffa1f5988624c52048f5b20cd`, matching
`pristine/manifest.md5`.

Evidence tags: **[MEASURED]** = observed live on this machine, command given. **[PE-VERIFIED]** =
read out of the PE header. **[CORPUS]** = read from the TADR clone at `13d71dd` (2026-08-31).
**[UNTESTED]** = stated as an open question, not a claim.

---

## Summary

1. **`TotalA.exe` is not large-address-aware**, so the process is capped at 2 GB of user
   address space. [PE-VERIFIED]
2. **Wine 9.0 honours the LAA bit** — flipping it in a test binary moves the ceiling from
   2047 MiB to 4095 MiB. The 4 GB option is real on our stack, not just on Windows. [MEASURED]
3. **The ceiling is not binding today**: 726.4 MiB free below 2 GiB with the game running. [MEASURED]
4. **The free space is not fragmented** — 691.7 of those 726.4 MiB sit in exactly two holes.
   Contiguity is not our problem; capacity might become one. [MEASURED]
5. **What consumes the low 2 GB is the host graphics stack, not the game.** ~700 MiB of
   file-backed driver images (`libLLVM`, the NVIDIA blobs, Vulkan ICDs) load before TA allocates
   anything. TA's own heap is ~334 MiB, our DLL's static pools 19.7 MiB, our GPU objects 235 MiB
   of VRAM (which is not address space). [MEASURED]

---

## 1. The flag, and the one byte that would change it

```
PE @ 0xB0   machine 0x014C   PE32   ImageBase 0x00400000
Characteristics    = 0x010B  at file offset 0xC6
                     RELOCS_STRIPPED | EXECUTABLE_IMAGE | LOCAL_SYMS_STRIPPED | 32BIT_MACHINE
                     IMAGE_FILE_LARGE_ADDRESS_AWARE (0x0020) — ABSENT
DllCharacteristics = 0x0000  (no DYNAMIC_BASE, no NX_COMPAT)
```
[PE-VERIFIED]

Enabling LAA is a **same-length one-byte edit: `0x0B → 0x2B` at file offset `0xC6`**. It is not
the community's import-table edit at `0x004FF618` (see [The core mechanism](binary-patches.html))
and has nothing to do with proxy-DLL naming — it is a header bit the loader reads.

**Why we cannot do it the way we do everything else.** The loader reads the bit before any DLL
of ours exists, so there is no runtime path to it — unlike every other patch we apply, which
`tagpu_patches.c` writes into the loaded image from `DllMain`. Our instances symlink
`TotalA.exe` straight to the Steam file (`tools/tacli:418-421`), so setting the flag requires
materialising a real per-instance copy. See the TODO below.

## 2. Wine honours the flag — measured, not assumed

A 32-bit mingw probe calling `GetSystemInfo` / `GlobalMemoryStatusEx`, then reserving 16 MiB
chunks with `MEM_RESERVE | MEM_TOP_DOWN` until failure. Built once, then copied with the LAA bit
set, and both run in the same prefix under the same wine:

| | `lpMaximumApplicationAddress` | `ullTotalVirtual` | actually reserved | highest block |
|---|---|---|---|---|
| as TA ships | `0x7ffeffff` | 2047 MiB | 1952 MiB | `0x7eb60000` |
| LAA bit set | `0xfffeffff` | 4095 MiB | 3984 MiB | `0xfebc0000` |

[MEASURED]

The LAA run places blocks above `0x80000000`, which is the whole point: wine's non-LAA fence is
lifted, and the extra ~2 GB is genuinely usable.

## 3. The memory layout of a live game

`/proc/<pid>/status`, game running: [MEASURED]

```
VmPeak  3,484,480 kB      VmSize  3,450,288 kB
VmHWM     456,076 kB      VmRSS     390,416 kB      VmData  323,508 kB
```

**`VmSize` is misleading here** and must not be quoted as "TA uses 3.4 GB". Most of it is wine's
own ceiling fence — two `PROT_NONE` reservations that exist precisely to make the 2 GB limit
unreachable:

```
0x7FFE1000-0xED830000   1752.3 MiB   ---p     wine's non-LAA fence
0xEDB90000-0xFFBC0000    288.2 MiB   ---p
```

Below 2 GiB, where everything real lives:

```
PROT_NONE reserve   299.5 MiB
anon (heap)         334.4 MiB     <- TA's own memory, plus ours
file-backed         687.7 MiB     <- the graphics stack
                  ----------
consumed          1,321.6 MiB          FREE  726.4 MiB
```

The file-backed half, which is the finding: [MEASURED]

| MiB | image |
|---|---|
| 154.5 | `libLLVM.so.20.1` (the software-Vulkan/Mesa probe) |
| 125.8 | `libnvidia-gpucomp.so.595.84` |
| 71.6 | `nvidiactl` |
| 30.4 | `libnvidia-glcore.so.595.84` |
| 29.4 | `libicudata.so.74.2` |
| 20.6 | `libvulkan_intel.so` |

**Roughly a third of the address space a 1997 game is allowed to have is spent before it
allocates its first byte**, on driver images we do not control. That, not TA's appetite, is the
argument for LAA.

Anchors of the layout, all verified in `/proc/<pid>/maps`:

```
0x00400000  TotalA.exe image
0x20000000  ]  510.0 MiB free
0x3FE00000  ]
0x40000000  ]  181.7 MiB free
0x4B5B0000  ]
0x59DC2000  libLLVM (149 MiB, r-xp)
0x6E8B0000  libnvidia-gpucomp .. 0x76445000
0x7FFE1000  wine's fence begins  ==== the 2 GB ceiling ====
```

### Fragmentation: measured, and it is not a problem

726.4 MiB free in **64 holes**, but the distribution is the opposite of fragmented: [MEASURED]

| hole size | count | total |
|---|---|---|
| < 64 K | 16 | 0.3 MiB |
| 64 K – 1 M | 39 | 6.8 MiB |
| 1 M – 16 M | 7 | 27.6 MiB |
| 16 M – 128 M | 0 | 0.0 MiB |
| > 128 M | **2** | **691.7 MiB** |

95 % of the free space is two contiguous blocks (510.0 MiB at `0x20000000`, 181.7 MiB at
`0x40000000`). The driver images load as contiguous mappings in wine's DLL band and leave the
middle of the space intact. **A large single allocation — a 4096² composite buffer, say — has
room today.** An earlier claim in conversation that the GL driver "fragments" the space was
wrong and is corrected here.

## 4. What our GL renderer uses

### Host side — we are already a pool architecture

```
   text     data       bss
 746,524   54,352   20,673,044     i686 `size` on tagpu/ddraw/ddraw.dll
```
[MEASURED] — **19.7 MiB of `.bss`**, committed at image load. The per-frame geometry paths are
fixed static arrays with caps, never allocations:

| MiB | pool | where |
|---|---|---|
| 4.50 | `s_verts[4][32768 × 9]` — fx buckets (under, lines, flash, sprites) | `tagpu_fx.c:229-230` |
| 1.69 | `s_vShadow[16384 × 9]` + `s_vBody[32768 × 9]` — features | `tagpu_feat.c:232-233` |
| 0.84 | `s_verts[24576 × 9]` — unit geometry per frame | `tagpu_render3do.c:102` |
| 0.66 | `s_verts[24588 × 7]` — markers and bars | `tagpu_mark.c:172` |
| 0.39 | `s_pixels[640 × 640]` — readback scratch | `tagpu_render3do.c:103` |
| 0.25 | `s_dec[512 × 512]` — GAF frame decode | `tagpu_gaf.c:31` |

The eight surviving `malloc` sites are all amortised, not per-frame churn: the scaffold buffer
reallocs only when the viewport size changes (`tagpu_scaffold.c:368`), the 3DO cache grows only
`if (need > v->cap)` and converges (`tagpu_r3dcache.c:68`), the marker blend table is built once
(`tagpu_markown.c:207`), the terrain atlas is per tile-set load (`tagpu_terr.c:345`), and the
capture buffer is behind a trigger file (`tagpu_overlay.c:160`).

### GPU side — 235 MiB, and not address space

`nvidia-smi` with the game running: **`TotalA.exe` 235 MiB** of the RTX 4070's 12,282 MiB.
[MEASURED] Textures we create, sized from the constants and the live log line
`native: FBO 1024x768 ss=2`:

| MiB | object | where |
|---|---|---|
| 12.0 + 12.0 | supersampled colour RGBA8 + depth24 at 2048×1536 | `tagpu_native.c:615-618` |
| 3.0 + 3.0 | game-res colour + depth at 1024×768 | `tagpu_native.c:603-605` |
| 4.0 | feature atlas R8 2048² | `tagpu_feat.c:113` |
| 4.0 | effects atlas R8 2048² | `tagpu_fx.c:120` |
| 1.6 + 1.6 + 0.4 | unit FBO RGBA8 + depth24 + R8 at 640² | `tagpu_render3do.c:446-458` |
| 1.0 | unit texture atlas R8 1024² | `tagpu_render3do.c:417` |
| ~1.1 | terrain atlas R8 2176 × (rows × 34), map-dependent | `tagpu_terr.c:377` |
| 0.75 | scaffold R8 at viewport size | `tagpu_scaffold.c:492` |

**This is the part pooling cannot help.** Driver-side storage is allocated by the driver, and its
host-side mappings are the 700 MiB above. Converting more of our own `malloc`s into statics
changes none of it — and since a static costs its address space *always* rather than on demand,
more pooling makes the ceiling question marginally worse, not better.

## 5. What "extending memory" means in TADR — a different axis

TADR raises **engine pool ceilings**; it never touches the address-space ceiling.
`grep -li largeaddress` over the whole clone returns nothing. [CORPUS]

Two mechanisms, both worth knowing:

- **DLL-static replacement pools** (`src/DDraw/EngineLimits.cpp`): projectiles 300 → 3000
  (`0x6B` each ≈ 313 KB), explosions 300 → 3000 (`0x54` each ≈ 246 KB), model effects 100 → 1000
  with the pool grown `0x186A0` → `1000000` bytes, plus 3000 aux debris slots. The arrays are
  plain statics and the install rewrites the absolute addresses and bound constants embedded in
  TA's instruction stream to point at them — the DLL's `.bss` is the code cave. Guarded by
  `static_assert(sizeof(void*) == 4, "Engine limits require a 32-bit build")`.
- **Rewriting the sizes TA hands its own allocator** (`src/DDraw/LimitCrack.cpp`): unit types
  512 → 16000 (`0x249` each ≈ 9.4 MB), unit limit 250 → 3663, SFX 400 → 16000 via an inline hook
  on the malloc size (`Sfx_mallocBufSizeRouter`), composite buffer 600² → 1280² (class default
  4096²), AI search-map entries 1333 → 66650.

Total added: on the order of **1.7 MB of statics plus ~10-15 MB of larger engine allocations**.
Nothing there needs a third gigabyte — which is exactly why nobody in the scene bothered with
LAA, and why their precedent says nothing about whether we need it.

## What this means

- **LAA is the lever for headroom. Pooling is not.** They solve different problems: pools remove
  allocator churn and make failure modes deterministic (worth having, and we already have them);
  the flag is the only thing that raises the ceiling.
- **Nothing is currently starved.** 726 MiB free, unfragmented, with a map loaded and the native
  pass at 2× supersampling. The case for LAA is headroom for what we add next, not a fix.
- **The growth we should watch is ours plus the driver's**, not TA's. Our supersampled FBO alone
  is 24 MiB of driver storage; at 4K with `ss=2` the same two textures would be ~250 MiB, and the
  driver's host-side mappings scale with what is resident.

## TODO

1. **`tacli --laa`**: materialise a real per-instance copy of `TotalA.exe` instead of the symlink
   and flip `0xC6` (`0x0B → 0x2B`), verifying the source md5 against `pristine/manifest.md5`
   first. Steam stays untouched, nothing is redistributed, and `mirror_gamedir()` already
   special-cases the exe (`tools/tacli:418-421`). ~10 lines behind a flag, default off.
2. **Prove TA survives above 2 GB before relying on it.** The Windows trick for this
   (`AllocationPreference = 0x100000`) is an NT memory-manager key wine does not implement, so
   the wine-native equivalent is ours to build: reserve the free low space from `DllMain` behind
   a trigger file, forcing every subsequent engine allocation above `0x80000000`, then play a
   game and watch for the failure. Until that runs, "TA is LAA-safe" is [UNTESTED] — 1997 MSVC
   code may treat pointers as signed.
3. **Budget logging at load**: print total `.bss` plus the high-water of the guarded allocations
   into `tagpu.log`, so a future pool raise cannot quietly eat the headroom. Today our 19.7 MiB
   is whatever the constants happen to add up to.
4. **Re-measure at the real ceiling**: the 726 MiB figure comes from one instance on one map.
   A large map with a full unit count, hires models loaded and the megamap open is the case that
   would actually approach the limit, and it has not been measured.
5. **Audit our own pointer arithmetic** if LAA lands. A shallow scan of `tagpu_*.c` found no
   pointer→signed-`int` casts, and the patch helpers take `unsigned int addr`
   (`tagpu_patches.c:7,23`) — but that scan was pattern-matching, not a proof.

## Gaps this investigation did not close

- **No community-patched exe was checked** for the LAA bit: the TADR trees ship `Launcher.exe`,
  `modstool.exe` and `Server.exe`, but no `TotalA.exe`. Whether the 3.9.02 patched exe sets the
  flag is unknown. The separately-recorded "TA 4GB Patch" (2020) remains a LEAD in
  [Feature-first sweep](candidates-features.html), unexamined.
- **The `.bss` attribution is computed from source constants, not read from symbols** — our
  release DLL is linked `-s`, so the 19.7 MiB total is measured but its breakdown is arithmetic.
- **The driver's host-side appetite was not correlated with our texture set.** We know it is
  ~700 MiB with 235 MiB of VRAM in use; whether it grows with our allocations or is essentially
  fixed startup cost is unmeasured, and it decides whether LAA is urgent or merely prudent.
