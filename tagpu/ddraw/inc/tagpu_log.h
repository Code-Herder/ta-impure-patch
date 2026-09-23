#ifndef TAGPU_LOG_H
#define TAGPU_LOG_H
/* tagpu_log -- the one sink for every line this DLL logs. Nothing else opens a log file
   (tools/log-check.sh fails the build when something does).

   WHERE. `log\` beside our ddraw.dll -- found once at attach from the module's own path,
   never from the working directory, which a shortcut's "Start in" can move. Two streams:

     log\tagpu.log            TLOG_MAIN      every module's lines, CRLF
     log\tagpu_cobtrace.log   TLOG_COBTRACE  the COB oracle's records, LF (tagpu_cobtrace.c)

   and their history, `tagpu.1.log` (newest) .. `tagpu.10.log`, the same for cobtrace. The
   extension stays `.log` so a double-click opens an editor.

   THE CAPS, compiled in and held by construction -- every write is checked BEFORE it is made:
     TLOG_FILE_CAP   16 MB  no file exceeds it; a line that would not fit rotates first
     TLOG_KEEP       10     rotated files per stream; the eleventh is deleted
     TLOG_TOTAL_CAP  128 MB both streams, current and rotated together; the oldest rotated
                            file is deleted to make room, whichever stream it belongs to
   Only files whose names the sink writes are counted or deleted; anything else in `log\`
   is left alone.

   A RUN. Every attach rotates the previous run's files into history, so a run's lines are
   `tagpu.log` plus the rotated parts in front of it. The first line of each file is
   `log: run <id> part <n> ...` (with a "# " in front on the cobtrace stream, whose
   parsers skip comments), and a file closed by a rotation ends with `log: continued in
   ...`. tools/talog.py reads a run across its parts.

   BLOCKED. When a rename or delete is refused (another process holds the file open without
   FILE_SHARE_DELETE), lines are DROPPED and counted rather than written past a cap; the
   rotation is retried at most once a second, and the first line after it succeeds says
   how many were lost. The caps never depend on the retry succeeding.

   ONE OWNER. `log\tagpu.lock` is held open with no sharing for the life of the process. A
   second process started from the same folder cannot take it and logs nothing for its run.

   THREADS. Any thread may call any of these. One critical section covers both streams (the
   total cap spans them); it is held for the size checks, a rotation and one WriteFile. A
   caller's line is formatted before it is taken. Under it run only kernel32 file calls and
   the CRT formatting of the sink's own notes: nothing logs, calls back into our code or
   takes a lock of ours, so it is a leaf and cannot close a lock cycle.

   EXIT. ExitProcess kills every other thread before DLL_PROCESS_DETACH, possibly inside the
   sink with the lock held. From tagpu_log_detaching() on, a call that cannot take the lock
   at once drops its line instead of waiting on a dead owner. The lock is never deleted and
   the files are never closed: the OS closes the handles.

   Before tagpu_log_init, in the config tool's load (cnc_ddraw_config_init), and in a process
   that does not own `log\`, every call returns without writing. */

#define TLOG_MAIN      0
#define TLOG_COBTRACE  1

/* DllMain, DLL_PROCESS_ATTACH, before any other tagpu_* init: takes the lock, rotates the
   previous run into history, enforces the caps. */
void tagpu_log_init(void);

/* DllMain, DLL_PROCESS_DETACH, first: from here a held lock is an orphan (EXIT above). */
void tagpu_log_detaching(void);

/* One line to TLOG_MAIN; the sink adds the line ending. Longer than 1 KB is cut and marked. */
void tagpu_log(const char* line);
void tagpu_logf(const char* fmt, ...);
void tagpu_log_stream(int stream, const char* line);

/* A BLOCK stays in one file: its lines are collected in a heap buffer and written with one
   WriteFile, rotating first when the block would not fit in the current file. A block
   larger than a file can hold is cut and marked. begin returns NULL when nothing would be
   written (the sink is off, or no memory); blockf and end accept NULL. */
typedef struct TLOG_BLOCK TLOG_BLOCK;
TLOG_BLOCK* tagpu_log_block_begin(int stream);
void        tagpu_log_blockf(TLOG_BLOCK* b, const char* fmt, ...);
void        tagpu_log_block_end(TLOG_BLOCK* b);

#endif
