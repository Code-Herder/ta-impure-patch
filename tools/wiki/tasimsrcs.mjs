/* tasimsrcs.mjs -- the shared simulation core's sources, every tagpu/ddraw/src/tasim_*.c, as each native or Wine build
   of the in-game wiki's simulation lists them beside tagpu_wikisim.c (research/notes/impai/shared-core.md). The
   Makefile and tools/wiki/wikilaycheck.py take the same files by the same name, so a module added to the core is
   built by every check with no list to edit. Paths are relative to tagpu/ddraw, as the checks' own lists are. */
import fs from 'node:fs';
import path from 'node:path';

const SRC = path.join(new URL('../../tagpu/ddraw/src', import.meta.url).pathname);
export const TASIM_SRCS = fs.readdirSync(SRC).filter(f => /^tasim_\w+\.c$/.test(f)).sort().map(f => `src/${f}`);
