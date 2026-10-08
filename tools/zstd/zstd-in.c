/**
 * \file zstd.c
 * Single-file Zstandard library.
 *
 * Generate using:
 * \code
 *	tools/zstd/amalgamate.sh zstd-1.5.7.tar.gz
 * \endcode
 * which runs upstream's build/single_file_libs/combine.py over the release's lib/ with this file
 * as its input.
 */
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
/*
 * The demo recorder's configuration of upstream's build/single_file_libs/zstd-in.c (zstd 1.5.7).
 * The recorder compresses at level 3 and decompresses, nothing else, so the amalgamation keeps
 * only what those two need; each line below that differs from upstream's says what it removes.
 *
 * The strategies: level 3 is ZSTD_dfast in every source size class (lib/compress/clevels.h), and
 * fast and dfast stay in, so these remove the greedy, lazy and binary-tree match finders and
 * change no byte the recorder writes. A level that would need an excluded strategy cascades down
 * to dfast (ZSTD_adjustCParams_internal), so a wrong level costs ratio, never a failure.
 */
#define ZSTD_EXCLUDE_GREEDY_BLOCK_COMPRESSOR
#define ZSTD_EXCLUDE_LAZY_BLOCK_COMPRESSOR
#define ZSTD_EXCLUDE_LAZY2_BLOCK_COMPRESSOR
#define ZSTD_EXCLUDE_BTLAZY2_BLOCK_COMPRESSOR
#define ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR
#define ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR
/* ZSTD_getErrorName returns one fixed string: the recorder names each refusal itself. */
#define ZSTD_STRIP_ERROR_STRINGS
/*
 * Settings to bake for the single library file.
 *
 * Note: It's important that none of these affects 'zstd.h' (only the
 * implementation files we're amalgamating).
 *
 * Note: MEM_MODULE stops xxhash redefining BYTE, U16, etc., which are also
 * defined in mem.h (breaking C99 compatibility).
 *
 * Note: the undefs for xxHash allow Zstd's implementation to coincide with
 * standalone xxHash usage (with global defines).
 *
 * Note: if you enable ZSTD_LEGACY_SUPPORT the combine.py script will need
 * re-running without the "-x legacy/zstd_legacy.h" option (it excludes the
 * legacy support at the source level).
 */
#define DEBUGLEVEL 0
#define MEM_MODULE
#undef  XXH_NAMESPACE
#define XXH_NAMESPACE ZSTD_
#undef  XXH_PRIVATE_API
#define XXH_PRIVATE_API
#undef  XXH_INLINE_ALL
#define XXH_INLINE_ALL
#define ZSTD_LEGACY_SUPPORT 0
/* No ZSTD_MULTITHREAD: the recorder compresses on its own writer thread, one frame at a time,
   so zstdmt_compress.c and the thread pool are left out. */
#define ZSTD_TRACE 0
/* TODO: Can't amalgamate ASM function */
#define ZSTD_DISABLE_ASM 1

/* Include zstd_deps.h first with all the options we need enabled. */
#define ZSTD_DEPS_NEED_MALLOC
#define ZSTD_DEPS_NEED_MATH64
#include "common/zstd_deps.h"

#include "common/debug.c"
#include "common/entropy_common.c"
#include "common/error_private.c"
#include "common/fse_decompress.c"
#include "common/threading.c"
#include "common/pool.c"
#include "common/zstd_common.c"

#include "compress/fse_compress.c"
#include "compress/hist.c"
#include "compress/huf_compress.c"
#include "compress/zstd_compress_literals.c"
#include "compress/zstd_compress_sequences.c"
#include "compress/zstd_compress_superblock.c"
#include "compress/zstd_preSplit.c"
#include "compress/zstd_compress.c"
#include "compress/zstd_double_fast.c"
#include "compress/zstd_fast.c"
#include "compress/zstd_lazy.c"
#include "compress/zstd_ldm.c"
#include "compress/zstd_opt.c"

#include "decompress/huf_decompress.c"
#include "decompress/zstd_ddict.c"
#include "decompress/zstd_decompress.c"
#include "decompress/zstd_decompress_block.c"

/* No dictBuilder: the recorder trains no dictionary. */
