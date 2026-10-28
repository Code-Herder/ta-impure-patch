#ifndef TAGPU_TDF_H
#define TAGPU_TDF_H
/* tagpu_tdf -- TA's text definitions (.tdf, .fbi, .ota): `[NAME] { key=value; [SUB] { ... } }`,
   read for the in-game wiki's 3D asset loader (tagpu_wikiasset.c). The DLL's JSON reader is not a
   TDF reader.

   THE RULES ARE tools/tascene's parse_tdf, the Python the stage tools read the same files with,
   so the two agree on what a file holds:
     - block comments go first, then line comments, each replaced by a space;
     - a `[name]` opens a section under the current one (an unclosed `[` is skipped), `{` enters
       the last section opened, `}` leaves it;
     - a run of text up to `;`, a brace or a bracket is a `key=value` when it holds a `=`, split at
       the first one; key and value are trimmed of Python's whitespace for Latin-1;
     - a section named twice in one place is one section, its keys merged, and a key given twice
       keeps its last value; a key that names a section of its own scope replaces that section.
   Names and keys compare as the engine's _stricmp does (tagpu_hpi_stricmp).

   BOUNDED BY LENGTH: the text is read within the `len` bytes it is given and need not be
   terminated; the parse copies it. A file with more sections or keys than the bounds below is
   refused: each lookup is a scan of one scope, so the bounds cap a hostile file's cost (the stock
   files' most is 94 sections and 1 692 keys, features\mars\rocks.tdf, MEASURED 2026-10-26). No
   engine memory, no Win32, no state of its own. */
#include <stddef.h>

#define TAGPU_TDF_SECTIONS 2048
#define TAGPU_TDF_KEYS     32768

typedef struct TAGPU_TDF TAGPU_TDF;

/* The parse of `len` bytes of `text`, or NULL when it is past the bounds or memory runs out. */
TAGPU_TDF*  tagpu_tdf_parse(const char* text, size_t len);
void        tagpu_tdf_free(TAGPU_TDF* t);

/* The section `name` directly under `parent` (0 is the file itself), or -1. A section holding no
   key and no live section is not one, as an empty dict is not one to parse_tdf's callers. */
int         tagpu_tdf_section(const TAGPU_TDF* t, int parent, const char* name);

/* The value of `key` in section `node`, NUL-terminated and alive as long as `t`, or NULL. */
const char* tagpu_tdf_get(const TAGPU_TDF* t, int node, const char* key);

/* `key` as the engine's integer reader takes it [INFERRED: atoi]: leading blanks, a sign, the
   digits; `dflt` when the key is absent. */
long        tagpu_tdf_int(const TAGPU_TDF* t, int node, const char* key, long dflt);

#endif
