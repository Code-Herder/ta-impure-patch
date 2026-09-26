#ifndef TAGPU_REGSTORE_H
#define TAGPU_REGSTORE_H
/* tagpu_regstore -- in a tacli test launch, TotalA.exe's registry is a file beside it.

   TEST MODE has two signals, and either is enough: the token `-xtacli-test` on the command
   line, which tacli passes on every launch of a DLL that has this module (a remote
   instance's scheduled task, a local instance's `wine TotalA.exe`) and the engine ignores,
   and a `tacli-state` folder beside the running exe (GetModuleFileNameW(NULL), never the
   working directory), which tacli makes (a remote test folder's, a local instance's gamedir)
   and a player's folder never has. REAL MODE needs both absent: no token, and the folder
   not there. An error that cannot tell (a share, an access rule) without the token is real
   mode, so a player's folder stays inert whatever its file system answers; tacli never
   launches this DLL without the token, so that doubt cannot reach one of its launches.
   Real mode logs one line, `registry: real ...`, and does nothing else: no hook, no lock,
   no file.

   THE STORE IS SERVED ONLY WITH BOTH SIGNALS. The token without the folder has no store;
   the folder without the token is a launch that put its values somewhere else (a tacli
   from before the per-instance store writes them into the registry every wine prefix
   shares), or one by hand. Both are test mode, and both are refused below.

   IN TEST MODE EVERYTHING FAILS CLOSED. `tacli-state\registry.txt` holds the registry
   (tacli seeds it: a remote instance's by reading the player's key, a local instance's by
   reading the template prefix's user.reg; `launch` puts its test values in it). A signal
   alone, a store that is missing, a folder, unreadable or not loaded whole, no memory, an
   exe path that cannot be read, a registry import these hooks do not answer, or a
   win32.dll not loaded at attach (it is a static import of TotalA.exe) ends the process at
   attach (the line names the process: a cnc-ddraw config tool started in a test folder is
   refused too), with a log line first and before the game's first instruction: a test
   launch never runs against the real registry.

   The decision (tagpu_regstore_decide) comes before DllMain's return for cnc-ddraw's config
   tool, so an inherited cnc_ddraw_config_init cannot skip test mode.

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
   test mode has changed one.

   The one registry write of TotalA.exe's own code outside those imports, the `-r` switch's
   DirectPlay registration through dsetup.dll, is closed in test mode by tagpu_patches.c
   (close_register_switch), under the same rule: an exe that differs there is not run.

   THE GUARANTEE, AND ITS EDGE. TA's settings key is never written in test mode: the registry
   imports of TotalA.exe and win32.dll are served from the file, and `-r` is closed. Nothing
   here reaches:
     - the system DLLs the game uses (DirectPlay, DirectSound) and loads by name
       (IMAGEHLP.DLL, psapi.dll);
     - the other DLLs it loads at run time: online.dll, the extension DLLs online.dll loads
       into the game's process (tamplayx, takalix, taheatx, tawirepx, tadwngox, tatenx; the
       Steam install's import no registry function but RegOpenKeyExA, RegQueryValueExA and
       RegCloseKey), and reporter.dll and DebugHelper.dll (neither is in the Steam install);
     - the programs the game starts: what ShellExecuteA opens, and what online.dll starts;
     - Windows' own records: the Task Scheduler's of the instance's task while it exists,
       and those of the programs it runs.

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
   file or the new one, whole.

   THREADS. One critical section covers the store, its file and the first-seen log; any
   thread may call the hooks (win32.dll's music thread among them). Under it run this
   module's code, kernel32 file calls and tagpu_log, whose own lock is a leaf; nothing under
   it calls back into the engine or takes another lock of ours. Real registry calls run
   outside it.

   EXIT. tagpu_regstore_final logs the counters, taking the lock with TryEnter: the loader
   has killed every other thread by then, possibly one inside it. The file is whole either
   way (above). */

/* DllMain, DLL_PROCESS_ATTACH, first: whether this is a test launch (1) or not (0), from
   the command line and the exe's folder. Logs nothing and installs nothing. */
int tagpu_regstore_decide(void);

/* DllMain, DLL_PROCESS_ATTACH, right after tagpu_log_init: logs the mode; in test mode
   loads the store and installs the hooks, or ends the process. */
void tagpu_regstore_init(void);

/* 1 in test mode, from tagpu_regstore_init on. */
int tagpu_regstore_active(void);

/* DllMain, DLL_PROCESS_DETACH, after tagpu_log_detaching. */
void tagpu_regstore_final(void);

#endif
