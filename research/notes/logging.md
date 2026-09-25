# Logs: the capped sink

*Every line the DLL logs goes through one writer, `tagpu/ddraw/src/tagpu_log.c` (contract in
`inc/tagpu_log.h`), into a `log\` folder beside `ddraw.dll`. The files rotate, and three caps bound
them: size per file, number of rotated files, and total size on disk. `tools/talog.py` reads them
back across rotations for tacli and the scripts.*

## Where the logs are

`log\` is found once at attach from the path of our own module (`GetModuleFileNameA(g_ddraw_module)`),
never from the working directory, which a shortcut's "Start in" can move. Windows loads `ddraw.dll`
only from beside `TotalA.exe`, so this is the game folder in every install that works.

| file | stream | line ending | written by |
|---|---|---|---|
| `log\tagpu.log` | `TLOG_MAIN` | CRLF | every module (`tagpu_log`, `tagpu_logf`, blocks) |
| `log\tagpu_cobtrace.log` | `TLOG_COBTRACE` | LF | `tagpu_cobtrace.c` only, when `tagpu_cobtrace.on` arms it |
| `log\tagpu.1.log` … `tagpu.10.log` | history, `.1` newest | | rotation |
| `log\tagpu_cobtrace.1.log` … `.10.log` | history | | rotation |
| `log\tagpu.lock` | the owner's lock, empty | | attach |

The line endings are the ones each stream had before the sink: the old text-mode `fopen` wrote
CRLF, and cobtrace opened its file `"wb"`. A bare `\n` inside a main-stream line becomes CRLF, as
text mode made it. `ErrorLog.txt` (the engine's crash report) and `cnc-ddraw-*.log` (upstream,
`DEBUG=1` builds only) are not ours and stay where they were. So do dumps and captures.

## The caps

| cap | value | held by |
|---|---|---|
| `TLOG_FILE_CAP` | 16 MB | a write that would not fit rotates the file first |
| `TLOG_KEEP` | 10 rotated files per stream | the shift deletes `<stream>.10.log` before it renames `.9` |
| `TLOG_TOTAL_CAP` | 128 MB, both streams together | the oldest rotated file of either stream is deleted to make room |

All three are compiled-in constants. **They hold by construction:** every byte reaches a file
through one function, `raw()`, and every call to it is preceded by both checks for exactly those
bytes. If `cur + n` would exceed the file cap, or the total plus `n` would exceed the total cap
after deleting as much history as can be deleted, the write is not made. A static assertion keeps
the constants consistent: the total cap is at least both current files at their cap, so deleting
history can always make room, and a fresh file holds its header, a gap line, one longest line
and the room for its `continued in` line.

`NOTE_MAX` (192 bytes) stays free at the end of every file for the `continued in` line its
rotation writes. One line is at most `LINE_MAX` (2048) bytes of text, above every buffer that
feeds the sink (the `packet:` heartbeat's is 1 700 bytes and its lines run to about 1 430 in play,
measured 2026-09-25 with the `wire:` section); a longer
one is cut and ends `...[truncated]`. A block is at most `TLOG_FILE_CAP - 4 × NOTE_MAX`; a longer one is cut and ends
with a `...[block truncated]` line.

**Only names the sink writes are counted or deleted.** Those are `<stream>.log` and
`<stream>.<n>.log`. Anything numbered past `TLOG_KEEP` is deleted at attach, because the numbering is
ours. A file that will not delete then is counted against the total but never written. Every
other file in `log\`, such as a player's `notes.txt` or a `tagpu.backup.log`, is invisible to the caps.

**The bookkeeping is in memory.** It is taken from the directory at attach, then updated by every
write, rename and delete. It is exact because the lock makes this process the only writer of those
names. A byte appended from outside, for example `echo >> log/tagpu.log`, is a byte the caps do not
know about. Nothing in this repository writes to these files except the sink.

## A run, and how the files join

**Every attach rotates.** Before the first line of a run, the previous run's `tagpu.log` becomes
`tagpu.1.log` and the history shifts up. An empty leftover is deleted instead. The first line of
every file the sink writes is its header:

```
log: run 20260923-152048-940 part 1 of tagpu.log, started 2026-09-23 15:20:48
log: run 20260923-152048-940 part 4 of tagpu.log, started 2026-09-23 15:20:48, continues from tagpu.1.log
```

The run id is local start time plus the Windows process id. A rotation ends the closed file
with `log: continued in tagpu.log (part N)`. On the cobtrace stream these lines start with `# `,
because its parsers skip comments. So a run is `tagpu.log` plus the rotated files in front of it
that carry the same run id, back to part 1 or to the oldest one the caps kept. A new current file
is created on the first line after a rotation, so an empty file never takes a history slot.

**No file of ours lacks its header.** `put()` reserves room under the total cap for the header
and any pending gap line together with the line they precede, so all of them are written or
none is. If the header still cannot be written, the new file is removed again. Readers know a
file by that first line, so a file without it would drop out of every reader's view. The drop
count is cleared only when its gap line has actually been written.

**A file closed by a rotation takes no more lines.** Once its `continued in` line is written
and its handle closed, the file must rotate before anything else is written. If the rename is
refused, lines are dropped until it succeeds, rather than appended after the line that says the
log continued elsewhere.

**The shift starts at the lowest free slot.** With no free slot, it first deletes
`<stream>.10.log`. After a refused rename the numbering is still in order with one gap, and the
next attempt resumes from that gap instead of deleting a second file. A source that has already
gone counts as moved.

## When a rename or delete is refused

On Windows, a file held open by another process without `FILE_SHARE_DELETE` cannot be renamed
or deleted. Examples are an editor, a tail tool, or an antivirus scan. Wine enforces the same
rule (error 32, measured with a probe on wine-9.0). The sink's own handle shares delete, so it never
blocks itself. When a rotation or an eviction is refused:

- **Lines are dropped and counted.** Nothing is written past a cap and nothing already written is
  destroyed.
- **The rotation is retried at most once a second** (`RETRY_MS`), on the next line after that.
- **The first line written afterwards records the gap:**
  `log: 1022684 lines dropped over 2531 ms -- a rename, delete or create in log\ was refused`.
- An eviction that cannot delete the oldest file tries the next oldest, and so on.

**The caps never depend on the retry succeeding.** The retry only decides how many lines are
lost.

## One owner per folder

At attach the sink opens `log\tagpu.lock` with **no sharing** and never closes it. The OS releases it
when the process ends, however it ends, so a crash cannot leave a stale lock. A second process
started from the same folder cannot open it. That process writes nothing, rotates nothing and
deletes nothing for its whole run. Without the lock, the second process's rotation at attach would
rename the first process's live file, and each would keep its own count of the total.

## Threads and lifetime

- **One `CRITICAL_SECTION` for both streams**, because the total cap spans them. It is held for the
  size checks, a rotation or an eviction, and one `WriteFile`. A caller's line is formatted before
  the lock is taken, into a stack buffer or a block's heap buffer. Only the sink's own notes (the
  header, the gap line, `continued in`) are formatted under it, from string and integer conversions only.
- **It is a leaf lock.** Under it run only kernel32 file calls and the CRT formatting of those
  notes. msvcrt's `_vsnprintf` can take its own locale lock and allocate the thread's CRT data
  on first use, but nothing that holds a CRT lock logs. Nothing under it logs, calls back into
  our code, or takes a lock of ours, so it cannot close a lock cycle with any lock a caller
  holds.
- **Process exit.** `ExitProcess` kills every other thread before `DLL_PROCESS_DETACH`. For
  example, the close button's `SC_CLOSE` calls it on the game thread without joining the
  render thread, so a thread killed inside `put()` leaves the lock owned by a dead thread. At
  detach, the settings store logs the window frame it keeps. Waiting on that lock would end
  the exit there: Windows terminates the process, and Wine grants the lock over half-updated
  state. So `tagpu_log_detaching()`, the first call in `DLL_PROCESS_DETACH`, switches every
  call to `TryEnterCriticalSection`: a line that cannot take the lock at once is dropped.
  This is the same rule the settings store uses for its own lock.
- **Never torn down.** The critical section is never deleted and the handles are never closed;
  the OS closes them.
- **`s_live` is the gate every call reads.** It is set once, as the last step of init, and never
  cleared. A call before init, in the config tool's load, or in a process that does not own the
  folder returns without writing.
- **Where init runs:** `DllMain`, `DLL_PROCESS_ATTACH`, right after the
  `cnc_ddraw_config_init` return and before `cfg_load`. `cfg_load` already logs (`tagpu_cfg_defaults`), and opening the
  config tool must not rotate the player's logs.
- **One `WriteFile` per line**, to a handle that stays open, with no buffer of ours. A killed
  process loses nothing it had logged; only an OS crash could lose any.

## The API

```c
tagpu_log(line);                 /* TLOG_MAIN, the sink adds the ending */
tagpu_logf(fmt, ...);            /* formatted, cut at 2 KB */
tagpu_log_stream(TLOG_COBTRACE, line);
TLOG_BLOCK* b = tagpu_log_block_begin(TLOG_MAIN);   /* NULL when nothing would be written */
tagpu_log_blockf(b, fmt, ...);   /* NULL-safe */
tagpu_log_block_end(b);          /* one write, never split across a rotation */
```

The per-module helpers (`plog`, `zlog`, `vklog`, …) are one-line wrappers around these calls, and
they keep their module prefixes (`vk: `, `weapons: `, `cobtrace: `). The roster dump
(`tagpu_packet_pub.c`) and the tracer's raw dump (`tagpu_tracer.c`) are blocks. The build refuses
any other writer. `tools/log-check.sh`, an order-only prerequisite of the link beside
`thread-split` and `spirv`, fails when a source other than `tagpu_log.c` passes a `.log` string
literal to `fopen`, `_open`, `OpenFile` or `CreateFile*` (comments stripped).

## Reading it back: `tools/talog.py`

A reader that remembers a byte offset in `tagpu.log` loses its place at the first rotation.
`talog.Cursor` records the **`run/part` key** of the current file and the offset in it, plus the
keys of the history files that already existed. `read()` walks newest first: it reads unknown
files whole, reads the cursor's own file from the saved offset, and stops at a known file. Only
the first 256 bytes of a file are read to key it.

**Every pass is checked against a rotation.** Listing the files and then opening them are two
separate steps, so a rotation in between renames files under the reader, and a pass could read
a part twice or miss one. Each pass (`mark`, `read`, a run's parts) lists the files' (name, key)
pairs before and after itself, and runs again if they differ. A rotation moves every key to
another name and the sink never reuses a key, so an unchanged list proves nothing rotated during
the pass. The names are needed: the new current file appears only with its first line, so just
after a rotation the keys alone read the same, shifted one name up. After eight moved passes the
last result stands; that takes a rotation per pass, sustained. `tail_lines` and `roster` stop reading parts as soon as
they have enough, inside the same checked pass.

**Why not the inode.** The first cursor keyed files by inode. Once history is full the sink deletes
files, and the filesystem gives the next new `tagpu.log` a deleted file's inode. The cursor had
recorded that inode as already read, so `read()` stopped at the newest file and returned nothing.
Measured on a live instance with a 64 KB cap: `tacli wait` for a line two parts ahead timed out
after 180 s while the log went from part 9 to part 27. The header key is unique by construction:
the sink never writes the same `run/part` twice.

| caller | how |
|---|---|
| `tacli wait`, `peek`, `scenario load`'s live wait | a `Cursor` taken before the trigger |
| `tacli log -n N` | `tail_lines`: back through the run's parts until N lines |
| `tacli log -g RX` | every part of the run |
| `tacli roster` | newest part first, one more part only while no roster line has been found |
| `tools/uiwalk.py` | a `Cursor` that advances for the census window, `Cursor.run_start` after a relaunch |
| `tools/cobtrace_fixtures.py` | `run_text` of both streams |
| `tools/gatec.sh`, `tools/barwobble.sh` | `talog.py mark <gamedir>` before, `talog.py since <gamedir> <token>` after |

Command line: `talog.py mark|since|run|tail <gamedir> [--stream tagpu_cobtrace]`. `tacli`
reads **this run only**. The previous launch is `tagpu.1.log` and older, by name.

tacli no longer renames `tagpu.log` at launch; the DLL rotates. The instance mirror excludes
`log` (any case), and a launch unlinks a `log` symlink, so an instance's log folder is its own
and its lock does not silence other instances.

## Verified by running it

**The sink under Wine** (`make -C tagpu/ddraw logtest && tagpu/ddraw/tests/logtest.py`) runs the
real `tagpu_log.c` with a 64 KB file cap and a 512 KB total, in a scratch folder per case. The caps
are checked from inside the writing process every 50 lines, under the sink's lock. A listing made
without the lock can add sizes from before and after a delete, and once read 235 bytes over the
total for exactly that reason.

- **stress**: two threads, 20 000 numbered lines each on the main stream, a cobtrace line every 7,
  and a block of 1–300 lines every 97. Also three stray files (`notes.txt`, `tagpu.backup.log`,
  `tagpu.11.log`). Result: parts consecutive (82–88 survive), each thread's surviving lines a
  gapless run ending at 19 999 (a thread that finishes first can age out entirely), 32 blocks each whole in one file, a 5 000-byte line cut to 2 048 + marker. The two
  strays are untouched and `tagpu.11.log` is deleted at attach. A second launch starts run part 1
  and the previous run's last part becomes `tagpu.1.log`.
- **block**: a handle without `FILE_SHARE_DELETE` holds `tagpu.log` for 2.5 s. 1 022 684 lines
  were dropped, the gap line counted 1 022 684, and the surviving range is missing exactly
  that many. Writing resumed once the handle closed.
- **second**: while one process holds the lock, a second writes 1 000 lines: `log\` is unchanged
  (names, sizes, mtimes).
- **orphan**: a thread exits while holding the sink's lock. After `tagpu_log_detaching()` a
  line and a block both return at once and are dropped; the line before is kept.
- **kill**: `SIGKILL` mid-stream. The surviving lines are contiguous and the file ends on a whole
  line.
- **cursor**: a slow writer (one line a millisecond). A cursor taken at part 14, after history is
  full, returned exactly the 192 678 bytes written through part 17.

**Live, through tacli**, with a DLL built with a 64 KB file cap and 1 MB total, on `crowd-static`
(257 units, Two Continents) with `native.on=all spxlog.on cobtrace.on`. Rotation came about every
10 s:

- `talog.Cursor` across parts 58→61 returned 141 932 bytes, identical to the run's own bytes from
  the mark.
- `tacli wait … "part 63 of tagpu.log"` from part 61 matched in 20 s.
- `peek` 40/40, one of them straddling a rotation. `roster` returned all 257 units 40/40.
- `log -n 3000` read back across 5 parts.
- The cobtrace stream rotated through 6 parts; part 1 carries `# log: run …` and then
  `# tagpu_cobtrace v1 …`.
- On disk: largest file 65 384 B, total of ours 888 012 B.

With the shipped caps, the same scenario wrote 153 KB in about 40 s and did not rotate. The
previous launch became `tagpu.1.log`.

## Not covered

- **Logs written before this sink stay where they were.** Those are `tagpu.log`, `tagpu.log.prev`
  and `tagpu_cobtrace.log` in the game folder or in an old instance's gamedir. Nothing reads or
  deletes them; that was the owner's decision.
- **A second process from the same folder has no log at all.** Per-process files would need their
  own eviction rule. That can be added if running two copies from one install turns out to
  matter.
- **An unwritable `log\`** (for example an install under `Program Files` without write access)
  means no logs for that run, silently: there is nowhere to say so.
- **Eviction is by age across both streams.** A quiet stream's history goes before a busy
  stream's newer history. In the stress case, cobtrace's rotated files were all older than the
  main stream's and were evicted first.
- **Compression** was decided against for now. The caps already bound the disk. If 128 MB turns
  out to cut history someone needed, the shape would be `.zip` files for `.2` and older, written
  by a worker thread.
