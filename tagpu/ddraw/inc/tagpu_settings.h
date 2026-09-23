#ifndef TAGPU_SETTINGS_H
#define TAGPU_SETTINGS_H
/* tagpu_settings -- the player's settings store, `impure.cfg` beside TotalA.exe.
   Design: renderers.md 2.10b.

   ONE STORE, AND THE MENU IS ITS ONLY WRITER. Every row of the render-options
   screens reads its value from here and writes it back here; nothing on those
   screens persists through the registry, ddraw.ini or a lever file any more.

   PRECEDENCE, per setting, and every consumer asks in this order:
     1. a LEVER -- the setting's own file (`tagpu_ss.off`, a key inside
        `tagpu_classicpp.cfg`, a ddraw.ini key the player typed, ...). The
        consumer owns that test; the menu greys a row a lever holds.
     2. this store -- `tagpu_settings_get` answers 1.
     3. the consumer's compiled default -- `tagpu_settings_get` answers 0.
   With `tagpu_defaults.off` (every tacli control launch) the store has no say
   at all and `tagpu_settings_get` always answers 0, so such a launch behaves
   exactly as it did before the store existed.

   THREADS. Every value a consumer reads is an aligned LONG, written by
   InterlockedExchange and BOUNDED against its key's own table on every read,
   so a racing read returns the old value or the new one and never an index
   nobody validated. `tagpu_settings_gen` is bumped AFTER the value, so a
   reader that caches can compare it. The two strings (the GPU name, the
   window rect's monitor names) never cross a thread: see the functions. The
   FILE is written by `tagpu_settings_flush`, on the render thread only: TA is
   lockstep, and a disk write on the game thread is an unbounded stall. */

typedef enum {
    TS_STYLE,       /* TS_STYLE_*                                         */
    TS_ASSETS,      /* 0 | 1                                              */
    TS_LIGHT,       /* 0 | 1                                              */
    TS_SHADOWS,     /* TAGPU_SHADOWS_OFF | TAGPU_SHADOWS_HARD             */
    TS_SHADOWRES,   /* 512 | 1024 | 2048 | 4096                           */
    TS_SS,          /* 1 | 2                                              */
    TS_FPS,         /* 0 | 1: the frame-rate readout                      */
    TS_MAXFPS,      /* 60 | 120 | 0 (uncapped)                            */
    TS_HUDSCALE,    /* -1 off | 0 auto | 100 150 200 300 400              */
    TS_DISPLAY,     /* 0 window | 1 fullscreen (borderless)               */
    TS_MONITOR,     /* -1 none chosen | an index into the registered list */
    TS_NKEYS
} TagpuSetting;

enum { TS_STYLE_CLASSIC, TS_STYLE_PP, TS_STYLE_CUSTOM };

/* Called from cfg_init (config.c), BEFORE ddraw.ini is parsed and before any
   other thread exists: the first-run migration may strip keys from that very
   file. Migrates when `impure.cfg` is absent, then loads it. Idempotent. */
void tagpu_settings_attach(const char* ini_path);

/* 1 and the value in *out when the store supplies one; 0 when it has no say
   (tagpu_defaults.off), in which case the caller's own default stands. Any
   thread. While style is Classic or Classic++, the four render keys answer
   the Classic++ preset, whatever the file holds (renderers.md 2.10b). */
int  tagpu_settings_get(TagpuSetting key, int* out);

/* 1 when the store has no say at all: tagpu_defaults.off is present. */
int  tagpu_settings_ignored(void);

/* Any thread. Records the value (bounded: an invalid one is refused and
   logged) and marks the store for the next flush. */
void tagpu_settings_set(TagpuSetting key, int value);

/* Bumped after every set, for readers that cache what they derived. */
long tagpu_settings_gen(void);

/* The Classic++ preset's value for one of the four render keys. */
int  tagpu_settings_preset(TagpuSetting key);

/* RENDER THREAD ONLY: writes `impure.cfg` if anything changed since the last
   write. Temporary file renamed over the target, so a reader never sees a
   stub. */
void tagpu_settings_flush(void);

/* The monitor list, by device name (`\\.\DISPLAY2`). Registered ONCE at
   attach, before any other thread exists, and immutable after -- which is
   what lets the flush turn TS_MONITOR's index back into a name on another
   thread. The stored name is resolved against it here. */
void tagpu_settings_monitors(const char* const* names, int n);

/* The stored GPU name, "" for none. Read at attach only. */
const char* tagpu_settings_gpu(void);
/* RENDER THREAD ONLY: the GPU the lane will bind, by name. The string is
   written and read (by the flush) on this one thread. */
void tagpu_settings_set_gpu(const char* name);

/* The windowed frame, as ddraw.ini's posX/posY/width/height carry it. 0 when
   the store has none or no say. Read at attach (tagpu_cfg.c). */
int  tagpu_settings_window(int* x, int* y, int* w, int* h);
/* At shutdown: remember the windowed frame and write the store now -- the
   render thread is gone by then, so this is the one write off it. */
void tagpu_settings_save_window(int x, int y, int w, int h);

/* Supersampling (1 | 2) and the frame-rate readout (0 | 1), in precedence
   order: `tagpu_ss.off` / `tagpu_fps.on`, then the store, then 2 and 0. A
   file-attribute read each, so a caller polls them on its own cadence. */
int  tagpu_settings_ss(void);
int  tagpu_settings_fps(void);

#endif
