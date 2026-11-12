#ifndef WIKIHOST_H
#define WIKIHOST_H
/* wikihost.h -- the wiki's simulation host: the request loop that answers the mockup and the
   checks (tools/wiki/mockup/simhost.js through tools/wiki/hostchan.cjs or tools/wiki/serve.py's
   /sim), and the JSON writers the loop and tests/wikitest.c's replay modes print with. wikihost.c
   states the protocol. Everything prints to stdout. */
#include <stdio.h>
#include "tagpu_wikisim.h"

/* a string as JSON, NULL as null */
void wh_str(const char* s);
/* a double as JSON: %.17g gives back the same double when read */
void wh_num(double v);
/* an object's key `k`, after a comma unless `first` */
void wh_key(const char* k, int first);
/* the view as `seat` sees it in the drop-in's view(seat) shape, its map only with `map` */
void wh_view_as(WIKISIM* s, int map, int seat);
/* a unit's orders in the drop-in's shape, null for no such unit */
void wh_orders(const WIKISIM* s, const char* id);
/* the state hash as a JSON string of 16 hex digits */
void wh_hash(const WIKISIM* s);
/* a players game's sight per seat: its lit grid's FNV-1a 32, its lit blocks and its contacts
   (tools/wiki/simparity.mjs sightOut), over the layout's table `t` (NULL none) */
void wh_sight_out(WIKISIM* s, const WIKISIGHT* t);

#define WH_INSTANCE_MAX 64      /* an instance id's characters */

/* The request loop over `in` to its end: one answer line on stdout a request line, flushed. Its
   instance id `instance` (NULL: "") and its packs' root `packs_root` (NULL: none), wikihost.c's
   THE INSTANCE and THE PACKS' ROOT. 0. */
int wikihost_serve(FILE* in, const char* instance, const char* packs_root);

#endif
