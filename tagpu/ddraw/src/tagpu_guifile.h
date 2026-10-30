#ifndef TAGPU_GUIFILE_H
#define TAGPU_GUIFILE_H
/* tagpu_guifile -- a TDF-grammar file read BY POSITION, as the engine's parser 0x4C2F60 builds it:
   the sections in file order, a name given twice kept twice. The in-game wiki's sidebar loader
   (tagpu_wikiasset.c) reads the mod's .GUI pages, sidedata.tdf, the download TDFs and the units'
   FBIs with it. Its Python twin is tools/wiki/tavfs.py `tdf_sections`, which was checked against an
   instruction-for-instruction emulation of the engine's parser on 1581 .GUI files of six installs.

   NOT tagpu_tdf: that reader merges sections that share a name (tagpu_tdf.h), and the engine's .GUI
   reader 0x4AEAC0 walks the top-level sections by index (0x4C3490(i)), one gadget each, so retail
   ARMCK1.GUI's repeated GADGET2..GADGET6 are ten gadgets, not five.

   THE GRAMMAR IS THE ENGINE'S (DISASSEMBLED, 0x4C2F60, 0x4C33A0, 0x4C3E40):
     - the text ends at its first NUL byte (0x4C30A8 makes the file a C string);
     - comments are blanked once over the whole text before the parse, inside values too: a line
       comment (two slashes) up to, not including, the next LF; a block comment (slash, star) up to
       and including the first star-slash, not nested, and an unterminated one blanks to the end but
       for the text's last byte;
     - blanks are space, TAB, CR and LF only;
     - `[name]` searches for `]` through the rest of the text; the name is trimmed and may be empty
       or repeated; the next non-blank byte must be `{`;
     - `}` closes a section; at the top level a stray `}` ends the whole parse silently;
     - anything else is a key: `=` and then `;` are searched through the rest of the text, so a value
       missing its `;` runs on to the next `;` anywhere later (retail ARMGEN.GUI's `attribs=256`
       swallows `colorf=15`);
     - a key given twice in one section keeps the LAST value, matched without case
       (0x4C3F98..0x4C403C replaces it);
     - end of text inside a section and a missing `]`, `{`, `=` or `;` are refused: the engine's
       parser shows "Parse error in .TDF File!" and exits.
   A 0-byte file is refused too: the engine's load fails on size 0 (0x4C2F9A) and parses nothing.

   BOUNDS: every index the reader stores is one it made, every search is bounded by the text's
   length, and the nesting is walked with a stack on the heap, not by recursion. A file's bytes are
   data only: the worst a file can make is a refusal or a parse as large as itself.
   No engine memory, no Win32, no state: any thread, one parse each. */
#include <stddef.h>
#include <stdint.h>

typedef struct TAGPU_GUIFILE TAGPU_GUIFILE;

/* The parse of `n` bytes of `blob`, or NULL with the reason in `why` (the file is refused, or
   memory ran out). */
TAGPU_GUIFILE* tagpu_guifile_parse(const unsigned char* blob, size_t n, char* why, size_t nwhy);
void           tagpu_guifile_free(TAGPU_GUIFILE* f);

/* Sections are numbered by the parse; 0 is the root, which holds the top-level sections. */
int         tagpu_guifile_count(const TAGPU_GUIFILE* f, int sec);           /* its child sections */
int         tagpu_guifile_child(const TAGPU_GUIFILE* f, int sec, int i);    /* the i-th, or -1    */
const char* tagpu_guifile_name(const TAGPU_GUIFILE* f, int sec);            /* trimmed, "" root   */

/* 0x4C3410: the first child of `sec` whose name equals `name` without case, or -1. */
int         tagpu_guifile_find(const TAGPU_GUIFILE* f, int sec, const char* name);

/* The value of `key` in `sec` (the last given, matched without case), trimmed; NULL when absent. */
const char* tagpu_guifile_get(const TAGPU_GUIFILE* f, int sec, const char* key);

/* A NUMBER as the engine reads every number of a TDF: C's atoi (0x4C46C0), leading blanks (space,
   TAB, CR, LF), a sign, the digits; 0 when there are none and for NULL. `num` is the value
   saturated at +-TAGPU_GUIFILE_NUMSAT, for comparisons; `bits` its low 32 bits exactly, as a
   32-bit two's complement, for flag tests and byte truncations. Either may be NULL. Returns 1 when
   the text is a plain integer (blanks, sign, digits and nothing else), 0 when atoi reads only a
   prefix of it. */
#define TAGPU_GUIFILE_NUMSAT (1LL << 40)
int         tagpu_guifile_num(const char* v, long long* num, uint32_t* bits);

#endif
