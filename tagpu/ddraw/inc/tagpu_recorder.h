#ifndef TAGPU_RECORDER_H
#define TAGPU_RECORDER_H
/* tagpu_recorder -- the demo recorder (research/notes/tadr-port/demo-recorder-skirmish-slice.md).
   tagpu_recorder.c is the capture, on the game thread. tagpu_recwriter.c is the writer thread; it
   reads no engine memory, and since it is not on thread-split.allow the build refuses an engine
   address in it. The records that pass between them are tagpu_recfmt.h's. A skirmish is recorded
   only while tagpu_recorder.on is in the game's folder at the level's first iteration. */

#include <windows.h>

/* B2's stub at the sim step's call of the unit tick (0x4954ED) calls this first, on the game
   thread, between two iterations. */
void __cdecl tagpu_rec_capture(void);

/* tagpu_reclaim's teardown wrap calls this on entry to 0x491B60, on the game thread, before the
   cascade frees the unit array: the recording in progress ends, complete once its writer has
   closed the file. */
void tagpu_rec_level_end(void);

/* THE EXIT waits for the files of the recordings that have ended: a recording is complete once
   its writer has closed the file, and ExitProcess would kill a writer still writing its last
   block. Every orderly exit calls this first, on whatever thread exits: the exe's CRT, and each
   DLL of its folder, through the fork's ExitProcess hook (hook.c, winapi_hooks.c); the fork's own
   exits, which the hook does not reach, call it themselves -- SC_CLOSE (wndproc.c), tagpu_refuse,
   and the out-of-memory and unit-type refusals (tagpu_patches.c). A crash exit calls
   TerminateProcess and does not wait: the crashed thread may hold a lock a writer needs. A
   recording still in progress has no end to finish, so nothing waits for it and its file reads to
   its last block, incomplete. The wait ends when the files do: a writer never waits for another
   thread and takes no lock the exiting thread holds -- the exe's CRT holds its exit lock across
   ExitProcess, and a writer never enters the exe's CRT. An exit from inside a DllMain holds the
   loader lock, which a writer thread still starting needs, so no exit that waits runs there: the
   fork's exits in its DllMain (the registry store's refusal at attach, the terminate at detach)
   do not call this, and at attach no recording has ended, so there is nothing to wait for. */
void tagpu_rec_exit(void);

/* THE RING, one per recording, between its capture and its writer. The capture writes records
   past `wpub` and then publishes them by moving `wpub` (InterlockedExchange, so every byte it
   wrote is visible first); the writer reads up to `wpub` and gives the bytes back by moving `rpub`
   the same way. Both count bytes modulo 2^32 and the capacity is a power of two, so a count's
   position is `count & (REC_RING_BYTES - 1)` across the wrap. A record never crosses the ring's
   end: an RK_PAD sends the reader back to the start. The capture never writes past
   `rpub + REC_RING_BYTES` and always keeps REC_RING_RESERVE bytes back, so an RK_END fits
   whatever the ring holds: a recording cannot be left without its end.

   THE HAND-OVER. The capture's last access to the ring is the store that publishes RK_END, and it
   writes `wend` and `ended` before that store. The writer frees the ring once `ended` is set and
   `wpub` equals `wend` -- by then the capture is done with it, whatever the writer could parse. The
   capture signals the writer through a duplicate of `wake` that it owns and closes itself, so its
   signal after that store never touches memory or a handle the writer frees. */
#define REC_RING_BYTES    (16u << 20)
#define REC_RING_RESERVE  32u          /* an RK_END (16 bytes) after an RK_PAD of up to 12 */

/* WAKING THE WRITER. The capture signals `wake` at the start, once REC_WAKE_BYTES have been
   published since its last signal, and at the end; the writer also wakes every
   REC_WRITER_POLL_MS on its own. A signal under Wine is a round trip to the wineserver, which a
   busy server can hold for milliseconds, and the capture must not pay that every iteration. The
   poll only bounds how late the writer sees bytes: what it may read, and when it may free the
   ring, rest on `wpub` and the hand-over alone. REC_WAKE_BYTES is a small part of the ring, so a
   signal comes long before the ring could fill between two polls. */
#define REC_WAKE_BYTES     (1u << 20)
#define REC_WRITER_POLL_MS 100

typedef struct RecRing {
    unsigned char* buf;                /* REC_RING_BYTES, from VirtualAlloc                    */
    volatile LONG  wpub;               /* bytes the capture has published                      */
    volatile LONG  rpub;               /* bytes the writer has given back                      */
    volatile LONG  failed;             /* set by the writer when it cannot write: the capture
                                          then ends the recording                              */
    volatile LONG  wend;               /* the `wpub` that publishes RK_END ...                 */
    volatile LONG  ended;              /* ... valid once this is 1                             */
    HANDLE         wake;               /* auto-reset; the writer's handle                      */
    HANDLE         done;               /* manual-reset; the writer's handle, set once the file
                                          is closed: *The exit* waits on the capture's own    */
    char           path[MAX_PATH];     /* the recording's file, named by the capture           */
} RecRing;

/* Starts the writer thread on a ring whose first record is RK_HEADER. From then on the writer
   owns the ring's lifetime and frees the buffer, its handles of the events and the ring by the
   hand-over above, and sets `done` last. 0 when no thread could be started, and the ring is still
   the caller's. */
int tagpu_recwriter_start(RecRing* ring);

#endif
