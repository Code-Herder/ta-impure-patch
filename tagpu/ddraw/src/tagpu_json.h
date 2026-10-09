#ifndef TAGPU_JSON_H
#define TAGPU_JSON_H
/* tagpu_json -- a JSON reader for data the DLL carries as JSON text: art/hud/layout.json and
   art/hud/kit.json, read as they are so that the game and the mockup read one file.

   A parse builds a tree in one arena and nothing is freed until tagpu_json_free. A tree is
   IMMUTABLE once returned: tagpu_hudlay reads one from the game thread and the render thread
   at once, which is safe because neither writes it and its owner never frees it while the HUD
   can draw.

   Pure C99 and the C library: no Windows header, so tools/hudart/ compiles it natively to
   check the layout against the mockup. */
#include <stddef.h>

enum { TJ_NULL, TJ_FALSE, TJ_TRUE, TJ_NUM, TJ_STR, TJ_ARR, TJ_OBJ };

typedef struct TJV {
    int               type;
    int               n;        /* TJ_ARR, TJ_OBJ: the children                       */
    double            num;      /* TJ_NUM                                             */
    const char*       str;      /* TJ_STR: UTF-8, NUL-terminated; escapes decoded     */
    const char*       key;      /* a member of an object: its name; otherwise NULL    */
    struct TJV*       first;    /* TJ_ARR, TJ_OBJ: the first child, in document order */
    struct TJV*       next;     /* the next sibling                                   */
} TJV;

typedef struct TJDOC TJDOC;

/* The tree of `len` bytes of `text`, or NULL with a message in `err` naming the byte offset. */
TJDOC*     tagpu_json_parse(const char* text, size_t len, char* err, size_t errn);
const TJV* tagpu_json_root(const TJDOC* d);
void       tagpu_json_free(TJDOC* d);

/* Lookups. Each returns NULL (or `dflt`) when `v` is NULL, is of another type, or lacks the
   member or index, so a chain of them needs one check at its end. */
const TJV* tj_get(const TJV* obj, const char* key);
const TJV* tj_at(const TJV* arr, int i);
double     tj_num(const TJV* v, double dflt);
const char* tj_str(const TJV* v, const char* dflt);
int        tj_is(const TJV* v, int type);

#endif
