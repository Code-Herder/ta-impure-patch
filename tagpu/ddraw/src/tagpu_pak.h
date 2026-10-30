#ifndef TAGPU_PAK_H
#define TAGPU_PAK_H
/* tagpu_pak -- the archives tools/hudpak.py packs and res.rc embeds as RCDATA resources: art/hud/
   as HUDPAK (tagpu_hudart.c) and art/wiki/ as WIKIPAK (tagpu_wikidata.c). The format is
   hudpak.py's docstring.

   tagpu_pak_file is pure C99 and the C library, so the wiki's host parity check
   (tools/wiki/simparity.mjs) reads the same archive the DLL embeds; tagpu_pak_resource is the
   DLL's half and exists only in a Windows build. */
#include <stddef.h>

/* A file of the archive `pak` of `npak` bytes by its path under the packed folder, its length in
   `*len`; NULL when the archive lacks it or is not one. Every offset is checked against `npak`,
   so a short or damaged archive names nothing rather than reading past it. */
const unsigned char* tagpu_pak_file(const unsigned char* pak, size_t npak, const char* path, size_t* len);

#ifdef _WIN32
/* The RCDATA resource `name` of this DLL, its size in `*len`; NULL when it is absent. The
   resource is mapped with the module and lives as long as it does. */
const unsigned char* tagpu_pak_resource(const char* name, size_t* len);
#endif

#endif
