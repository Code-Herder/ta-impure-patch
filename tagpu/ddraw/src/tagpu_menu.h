#ifndef TAGPU_MENU_H
#define TAGPU_MENU_H
/* tagpu_menu — the render-options screen (Phase F, G18). Design: renderers.md
   §2.10 "How it is assembled"; the engine facts: exe-reverse-engineering.md
   "The screen lifecycle" and "Setting a gadget's state".

   THE SHAPE IN ONE LINE. `RENDER.GUI` is a real TA `.GUI` screen in a `.ufo`
   the DLL writes itself, pushed by copying Cavedog's own idiom at `0x495207`,
   with six live rows and no Apply, non-modal, never pausing. The engine does
   the hit-testing, the dispatch, the plate art, the fonts and the save-under;
   we supply two bytes of gadget state, one frame's pixels, one 28x28 trigger
   and one small archive.

   ARMING. The archive is written at `DLL_PROCESS_ATTACH` unconditionally, with
   a version stamp, so it can never be stale after a DLL upgrade. The screen
   itself is armed unless `tagpu_menu.off` exists; `tagpu_menu.on` may carry
   tokens (`rows=N` to build fewer rows, for bisecting). A `.ufo` the engine
   will not read is SILENT on screen and loud in `tagpu.log`: a rendering menu
   failing to appear must never cost a game. */

/* The engine calls this: __stdcall void(GUIInfo*), parked at GUIMEMSTRUCT+0x08.
   The actuated index arrives in gi->UIChange_f (main+0x579), not as an
   argument, and -1 is the pop path. */
void __stdcall tagpu_menu_oncommand(void* gi);

/* DLL_PROCESS_ATTACH: write the archive, install the observer. */
void tagpu_menu_init(void);

/* 1 when the observer is installed and the screen can be opened. */
int  tagpu_menu_installed(void);

/* Is this GAME-space point one the render-options UI owns -- the sprocket, or
   the panel while it is open? The zoom asks, because a click inside the world
   viewport is UNZOOMED on its way to the engine (tagpu_zoom.h) and our panel
   hangs over the world: at any zoom != 1 every row click would be bent to a
   different point before the engine hit-tested it, and no row would work.
   The zoom cannot see GUI screens, so the screen has to say. */
int  tagpu_menu_owns_point(int gx, int gy);

/* The sprocket's hit-test, in GAME-space coordinates. Returns 1 when the
   trigger consumed the click, in which case the caller must not pass it on.
   Called from BOTH of the shield's input paths (tagpu_shield.c): the injected
   one sees only injected clicks and the real one only real ones, so a
   hit-test on either alone works for exactly half its users. */
int  tagpu_menu_click(int gx, int gy, int down);

/* The render thread's frame, from render_vk.c. Does the DEFERRED WRITE: a row
   click sets an in-memory value on the game thread and the cfg is written
   here, off it. TA is lockstep and a synchronous write inside `OnCommand` is
   an unbounded stall — a slow disk, a scanner touching a just-written file, a
   network drive — which can drop a player from a session whatever the content
   was. Deferring also coalesces rapid clicks into one write, and the cfg
   poller re-reads on mtime either way. */
void tagpu_menu_present(void);

/* The WINDOW column's four rows -- Display mode, Monitor, UI scale, Frame cap
   -- change the window the picture is presented in, not what is drawn into it,
   so each one ends in a window call. A cross-thread window call is a wait on a
   message pump rather than a visible error, so a click POSTS this and the
   wndproc does the work: the thread that owns the window is the thread that
   changes it, by construction. Same contract as tagpu_shield.c's injected
   input, which is where the WM_TAGPU_* block comes from. */
#define WM_TAGPU_DISPLAY (WM_APP + 143)   /* wParam = the row index           */
BOOL tagpu_menu_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result);

/* The monitor the Monitor row has selected, as a desktop rect. FALSE means
   nobody has chosen one yet -- the screen has never been opened -- and the
   caller falls back to the monitor the window is on (`util_target_monitor`).

   The resolution list is built from this rather than from where the window
   actually is, because the two disagree for exactly as long as it matters: the
   window move behind the Monitor row is POSTED, so when the row rebuilds the
   list the window has not moved yet. Following the model is an ordering, not a
   delay -- the list is right on the first frame it is drawn, and stays right
   when the move lands. */
BOOL tagpu_menu_monitor(RECT* out);

#endif
