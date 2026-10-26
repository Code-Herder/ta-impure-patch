#ifndef TAGPU_HUDART_H
#define TAGPU_HUDART_H
/* tagpu_hudart -- the replay HUD's files, loaded: art/hud/ as the RCDATA resource HUDPAK
   (tools/hudpak.py), its JSON resolved by tagpu_hudlay, its parts decoded, its faces opened.
   Plan: research/notes/hud-in-game.md.

   LOADED ONCE, OFF THE FRAME, AND THEN IMMUTABLE. tagpu_hudart_start begins the load on a
   thread of its own (decoding the parts takes a few hundred milliseconds, which no frame may
   spend). That thread writes everything below and then publishes it with one interlocked
   store; tagpu_hudart_get returns it only after that store, and nothing writes it again or
   frees it for the life of the process. So any thread may read a HUDART it was given -- the
   render thread draws from it and the game thread hit-tests with it -- without a lock. The
   decoded parts stay for the session because every teardown and resize brings the HUD pass
   down, and its images are uploaded again from these (35.2 MB of RGBA, MEASURED for the
   66 parts of 2026-10-25, fitted by tools/hudart/fit.py). */
#include "tagpu_hudlay.h"
#include "tagpu_wikidata.h"
#include "stb_truetype.h"

#define HUD_MAXFACE 8

typedef struct {
    int            w, h;
    unsigned char* rgba;        /* premultiplied by its alpha, w * h * 4 bytes */
} HUDIMG;

typedef struct {
    char           file[48];    /* fonts/<family, lower case, no spaces>-<weight>.ttf */
    stbtt_fontinfo info;
    float          ascent;      /* the vertical metrics the browser lays a line out by, */
    float          descent;     /* in em, descent positive (tagpu_hudart.c face_open)  */
} HUDFACE;

typedef struct {
    const HUDLAY*  lay;
    HUDIMG         img[128];    /* by kit index (tagpu_hudlay_part)                     */
    int            nface;
    HUDFACE        face[HUD_MAXFACE];
    int            style_face[24];  /* by style index (HUDGLOBALS.style)               */
} HUDART;

/* Begin the load, once per process; later calls do nothing. Any thread. */
void          tagpu_hudart_start(void);

/* The loaded HUD, or NULL while it loads and for good when it failed (the log says why). */
const HUDART* tagpu_hudart_get(void);

/* 1 once the load has failed, for good. Any thread. */
int           tagpu_hudart_failed(void);

/* THE IN-GAME WIKI'S FILES, loaded by the same thread right after the HUD's and published by a
   second interlocked store, under the same contract: immutable once published and never freed, so
   the game thread (its simulation) and the render thread (its layout and text) read them without
   a lock. `draw` is the window's kit as tagpu_huddraw draws it -- draw.lay holds the kit's parts
   with the HUD's styles (tagpu_hudlay_load_kit), draw.img the parts decoded, draw.face the HUD's
   faces -- and `data` the tree and the entries (tagpu_wikidata.h). It fails on its own while the
   HUD stands, and with the HUD, whose faces it draws in. */
typedef struct {
    HUDART         draw;
    WIKIDATA*      data;
} WIKIART;

/* The loaded wiki, or NULL while it loads and for good when it failed. Any thread. */
const WIKIART* tagpu_wikiart_get(void);

/* 1 once the wiki's load has ended, loaded or failed. Any thread. */
int            tagpu_wikiart_done(void);

#endif
