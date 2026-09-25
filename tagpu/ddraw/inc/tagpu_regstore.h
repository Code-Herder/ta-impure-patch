#ifndef TAGPU_REGSTORE_H
#define TAGPU_REGSTORE_H
/* tagpu_regstore -- in a tacli test folder, TotalA.exe's registry is a file.

   TEST MODE is the presence of `tacli-state\registry.txt` in the folder of the running exe,
   found from GetModuleFileNameW(NULL) and never from the working directory. Only tacli
   writes that file (`remote add` seeds it by reading the player's key; `launch` puts its
   test values in it), and tacli never writes into a player's folder, so a player's game
   never has it. Without it this module logs one line, `registry: real ...`, and does
   nothing else: no hook, no lock, no file.

   WHAT IT ANSWERS. TotalA.exe's nine ADVAPI32 imports (RegOpenKeyExA, RegOpenKeyA,
   RegCreateKeyA, RegCreateKeyExA, RegQueryValueExA, RegQueryValueA, RegSetValueExA,
   RegFlushKey, RegCloseKey -- the exe resolves no registry function by name) and the two of
   the `win32.dll` the Steam install loads (RegOpenKeyExA, RegQueryValueExA: it reads the
   music volume from TA's key) are replaced in their import tables at attach, before
   TotalA.exe's entry point runs. By the key's full path:

     HKCU\Software\Cavedog Entertainment and below   the store: every open, create, read
                                                     and write is served from memory, and
                                                     the file follows each change
     HKCU, HKCU\Software                             a store handle that holds no values;
                                                     a value read through it goes to the
                                                     real key, read-only
     every other key                                 READ-ONLY: an open asking for any
                                                     right beyond reading, a create and a
                                                     value write are refused with
                                                     ERROR_ACCESS_DENIED; a read-only open,
                                                     a query and a close go to the real
                                                     registry

   So no call from TotalA.exe or win32.dll reaches a registry function that writes: the hooks
   call only RegOpenKeyExA with read rights, RegQueryValueExA, RegQueryValueA and
   RegCloseKey. RegFlushKey on a real key returns success without a call, since nothing in
   test mode has changed one. A test folder whose TotalA.exe has a registry import these
   hooks do not answer is not run at all: the process is terminated at attach, with a log
   line, before the game's first instruction.

   The game's one registry write outside its imports, the `-r` switch's DirectPlay
   registration through dsetup.dll, is closed in test mode by tagpu_patches.c
   (close_register_switch), under the same rule: an exe that differs there is not run.

   HANDLES. A store key's handle is 0x6D5A0000 + 4 * its index, one per key path for the
   life of the process: interned and never freed, so a caller that never closes (win32.dll)
   costs nothing. A kernel handle is below 0x04000000 (the handle table holds 2^24 entries)
   and a predefined key is 0x8000000x, so the range cannot hold a real HKEY. Every other
   handle goes to the real function for a read and is refused for a write.

   THE FILE. ASCII lines, CRLF (LF is read too); a line starting with `#` is a comment.
     <key>                                        a key
     <key> TAB <name> TAB <type> TAB <data>       a value of that key
   <key> is the full path (`HKCU\Software\Cavedog Entertainment\...`); <name> is empty for a
   key's default value. <type> and <data>:
     dword    decimal, 0..4294967295              REG_DWORD, 4 bytes
     sz       the text without its terminator     REG_SZ, the text plus one NUL
     hex(N)   the bytes as hex pairs, maybe none  type N, any bytes
   In <key>, <name> and sz text, `%`, every byte below 0x20 and every byte from 0x7F up is
   written %XX (uppercase hex), so the file is ASCII whatever the code page; the bytes are
   the A functions', which are ANSI. tools/taremote.py reads and writes the same format.

   PERSISTENCE. Every change (a value that differs from the one held, a key created) rewrites
   the whole file as `registry.txt.tmp`, flushes it, and moves it over the file (MoveFileExW,
   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH): a kill at any moment leaves the old
   file or the new one, whole. A file that did not load whole (unreadable, a bad line, over
   a cap) is served as far as it parsed and NEVER written back, so what could not be read is
   not destroyed.

   THREADS. One critical section covers the store, its file and the first-seen log; any
   thread may call the hooks (win32.dll's music thread among them). Under it run this
   module's code, kernel32 file calls and tagpu_log, whose own lock is a leaf; nothing under
   it calls back into the engine or takes another lock of ours. Real registry calls run
   outside it.

   EXIT. tagpu_regstore_final logs the counters, taking the lock with TryEnter: the loader
   has killed every other thread by then, possibly one inside it. The file is whole either
   way (above). */

/* DllMain, DLL_PROCESS_ATTACH, right after tagpu_log_init. */
void tagpu_regstore_init(void);

/* 1 in test mode, from tagpu_regstore_init on. */
int tagpu_regstore_active(void);

/* DllMain, DLL_PROCESS_DETACH, after tagpu_log_detaching. */
void tagpu_regstore_final(void);

#endif
