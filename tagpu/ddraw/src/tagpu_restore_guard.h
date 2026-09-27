#ifndef TAGPU_RESTORE_GUARD_H
#define TAGPU_RESTORE_GUARD_H
/* tagpu_restore_guard.h -- when the restorer turns itself off on a device,
   how that is remembered, and how the player is told
   (research/notes/compute-restorer.md D2, D12, D14). No rendering API in it.

   WHAT IS BLAMED ON THE RESTORER, and nothing else is:
     - a crash on the render thread while it is inside a restorer call that
       builds or dispatches (`tagpu_rguard_enter` / `_leave`) -- not the
       teardown, where a fault is the driver freeing objects and, at exit,
       would relaunch a game the player has just quit;
     - the device reporting itself lost while a frame that carries restorer
       work is unfinished (`_work` / `_fenced` / `_blame_lost`). A fence or an
       acquire that times out is not a loss by itself: a presentation stall
       does that too. The seam then asks the device (vkDeviceWaitIdle, which
       its teardown waits on anyway) and blames only when that says lost;
     - the launch self-test finding wrong bytes (`_turn_off`).
   A wrong blame costs only undithering on that driver, and the render options'
   row gives it back.

   THE RECORD is one `vendor:device:driver:build` key -- impure.cfg's
   `restoreoff=`, or, when tagpu_defaults.off keeps the store out of it, the
   file tagpu_restore_off.txt, which only a hand deletes: the render options
   grey that row out when the store is not theirs. The restorer is off while the key names the
   device it is about to run on and this build of the DLL; a different key is a
   new driver, another card or a new build, and the record is dropped so that
   it gets a fresh try. The build is in it because a DLL that fixes the fault
   must not inherit the record of the one that had it.

   A CRASH IS RECORDED BY THE PROCESS THAT CRASHED, WHICH THEN RELAUNCHES THE
   GAME. The filter writes tagpu_restore_crashed.txt from buffers it built at
   install -- nothing of ours on that path allocates, though CreateFileA and
   CreateProcessA are the system's and may take the process heap's lock, so a
   crash that holds it can stop there, with the marker on disk -- then starts
   TotalA.exe again with its own command line and ends this process. The new
   process's first bring-up turns a marker that names its own device and build
   into the record and shows the notice, once; a marker naming another key
   goes, as a record naming it would. The marker is deleted once the disk
   holds the record as this process last made it -- that key, or nothing after
   a retry or a drop: until then the marker is the record. A relaunched
   process never relaunches again
   (TAGPU_RESTORE_RELAUNCHED in its environment), and without a marker on disk
   there is no relaunch at all: a relaunch that could not be remembered would
   only crash the same way.

   THE RECORD IS THE RENDER THREAD'S: `_device`, `_turn_off` and `_tick` are
   its only writers, and `_tick` the epoch's. `_clear`, on the game thread,
   only raises a request that `_tick` carries out: the record first, then the
   epoch, then the request lowered. */

enum { TAGPU_RG_CRASH = 1, TAGPU_RG_LOST = 2, TAGPU_RG_SELFTEST = 3 };

/* DLL_PROCESS_ATTACH, before the log opens: a relaunch waits here until the
   process that crashed has ended, so that TotalA.exe's own single-instance
   test cannot find that process still holding its semaphore. Returns at once
   in any other launch. */
void tagpu_rguard_attach(void);

/* THE DEVICE the restorer is about to run on, at its bring-up (render thread).
   Installs the crash filter once, turns a marker the last run left into the
   record (and owes the notice), drops a record that names another device, and
   answers 1 when this one is recorded off. */
int  tagpu_rguard_device(unsigned vendor, unsigned device, unsigned driver);
/* 1 while the device `_device` last named is recorded off and no retry is
   pending. Any thread. */
int  tagpu_rguard_off(void);
/* The device's key as `_device` formatted it, "" before. */
const char* tagpu_rguard_key(void);
/* The self-test found wrong bytes: record this device off and say so. */
void tagpu_rguard_turn_off(int why);
/* The render options' On, on the game thread: ask for the record to be
   forgotten. `_tick` forgets it and then moves the epoch, so that the
   restorer and every consumer that gave up on it ask again
   (tagpu_vk_restore_epoch). */
void tagpu_rguard_clear(void);
unsigned tagpu_rguard_epoch(void);

/* The render thread is inside a restorer call. Nested calls count. */
void tagpu_rguard_enter(void);
void tagpu_rguard_leave(void);

/* Restorer work went into the command buffer of frame slot `slot`; that
   slot's fence has since signalled; the device is proven idle or destroyed,
   which finishes every slot, whatever the slot count becomes. Render thread;
   `_idle` also from the bring-up worker's teardown, while the lane is not
   ready. */
void tagpu_rguard_work(unsigned slot);
void tagpu_rguard_fenced(unsigned slot);
void tagpu_rguard_idle(void);
/* Render thread, once a frame under either backend (tagpu_menu_present,
   before the store's flush): carries out a retry the player asked for, and
   deletes the marker once the disk holds the record. */
void tagpu_rguard_tick(void);
/* The seam's fatal path, when the device reported itself lost -- directly,
   or when asked after a timeout -- and before it takes the lane down. When a
   frame carrying restorer work was unfinished this records the device off and
   relaunches the game, and does not return; where it cannot relaunch (this
   process is a relaunch, or CreateProcessA failed) it records the device off
   in this process, tells the player and returns 0, so the lane's retry comes
   up without the restorer. Otherwise 0. A device that hangs
   and never reports the loss is not blamed: the seam's wait on it does not
   return, and nothing tells that hang from a long presentation stall. */
int  tagpu_rguard_blame_lost(void);

/* THE FAULT LEVER, tagpu_restorefault.on, for testing the three paths above:
   `probe` spoils one byte of the self-test's readback, `crash` faults inside
   a restorer call, `lost` has the seam report a device loss while restorer
   work is in flight. Two more drive the terrain's per-tile fallback (D10):
   `nbfit` has the neighbourhood atlas not fit, `nballoc` has the device
   refuse it. Read once. */
int  tagpu_rguard_fault(const char* token);
/* The `lost` lever's one shot: 1 once, when armed and restorer work is in flight. */
int  tagpu_rguard_fault_lost(void);

#endif
