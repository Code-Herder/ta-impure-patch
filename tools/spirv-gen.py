#!/usr/bin/env python3
"""G19c -- the shader pipeline: the fork's GLSL, translated to SPIR-V.

WHY THIS TOOL EXISTS AT ALL, AND WHY IT DOES NOT SIMPLY COMPILE A DIRECTORY OF
`.vert` / `.frag` FILES. The GL renderer's shaders live where they are used --
as C string literals inside the pass that owns them, next to the comment that
explains the maths. That is the one copy. A Vulkan lane that carried its own
edition of each shader would be a second copy of the fork's programs that
nothing forces to agree, and they would drift within a landing or two: the whole
point of Phase G is that the GL lane is the ORACLE for the Vulkan one, and two
shaders that disagree cannot be each other's oracle.

So the GLSL source of truth does not move. This tool READS the shaders back out
of the C sources -- through the C preprocessor, so a spliced macro like
`TAGPU_EDGE_NUDGE` expands exactly as the compiler expands it -- applies a
documented, mechanical GL-330-to-Vulkan transform, and compiles the result with
glslang. Editing a shader means editing the C string, as it always did; the
Vulkan edition follows on the next `spirv-gen`.

  the C string  --(cc -E)-->  GL 3.3 GLSL  --(transform)-->  Vulkan GLSL
                                                    --(glslang)--> SPIR-V
                                                    --(this)--> uint32_t[] .h

WHERE THE TRANSLATION RUNS: NOT IN THE BUILD. The roadmap's gate asked for
build-time translation and named the fallback in the same breath, and the
fallback is what this is -- deliberately, not because CI could not be taught to
install glslang. The build has four entry points (this Makefile, the CI job that
runs it, `build.cmd`, and the MSVC project), and only the first two are ours. A
compiler in the build would break the other two outright and would put a 30 MB
toolchain between a contributor and a DLL, to translate text that changes when a
shader changes -- which is to say, almost never. So the SPIR-V is generated
here, committed as text, and the build only CHECKS it.

WHAT STOPS THE COMMITTED HEADERS FROM ROTTING, which is the only real objection
to committing generated code. Every generated header carries, per shader, THREE
hashes: the SHA-256 of the Vulkan GLSL it was compiled from, the SHA-256 of the
WORDS themselves, and the hash of this tool's own transform. `--check` re-runs
the extraction and the transform -- both of which need the C preprocessor and
nothing else, never glslang -- and compares all three. A shader edited in its C
string without re-running this tool fails the build with the shader named; so
does a word array that has been hand-edited, truncated or badly merged, which
the GLSL hash alone could not see and which `-fsyntax-only` would accept.
`tools/spirv-check.sh` is what the Makefile calls, in the same place and for the
same reason as `thread-split-check.sh`.

What is still NOT asserted, and cannot be without the compiler: that these words
are what glslang would emit TODAY from that GLSL. The compiler is pinned by
version and by hash instead (`tools/glslang-vendor.json`).

THE TRANSFORM, in full. Each item is mechanical and applies to every shader:

  1. `#version 330 core` becomes `#version 450`.
  2. NON-OPAQUE UNIFORMS MOVE INTO A BLOCK. Vulkan GLSL has no default uniform
     block, so every `uniform vec2 uGame;` and friends are collected, in
     declaration order, into one unnamed std140 block. Unnamed, so every
     reference in the body still reads `uGame` and no line of the shader body
     is touched. The block's std140 offsets are computed here and written into
     the header beside the code, because the C side has to fill that buffer and
     the offsets are the contract.
  3. OPAQUE UNIFORMS (samplers) GET A BINDING, in declaration order.
  4. BINDINGS ARE A PROPERTY OF THE SHADER, NOT OF THE PROGRAM. A shader
     compiles once and may be used by several programs -- `QVS` is the vertex
     stage of six of them -- so a per-program allocation would need the same
     source compiled once per program, and two programs could then disagree
     about what binding 3 is. The allocation is therefore fixed by STAGE:

         vertex    set 0, binding  0 = the globals block
                                  1.. = named blocks, in declaration order
                                  8.. = samplers, in declaration order
         fragment  set 0, binding 32 = the globals block
                                 33.. = named blocks
                                 40.. = samplers

     Sparse, and deliberately so: a binding NUMBER costs nothing (the limits
     are on counts), and the gap means a vertex stage and a fragment stage can
     never collide whatever either one declares.
  5. VARYINGS GET EXPLICIT LOCATIONS, AND THE VERTEX STAGE DECIDES THEM. Each
     vertex `out` takes locations in declaration order; the fragment stage's
     `in` of the same name takes the NUMBER ITS VERTEX STAGE GAVE IT, so a
     fragment stage that declares a subset, or declares them in another order,
     still matches. A fragment `in` with no matching vertex `out` is an error
     here rather than a link failure in a driver. Where one shader is used with
     two different vertex stages -- `tagpu_native`'s fragment shader is shared
     with `tagpu_posedraw`'s vertex stage -- both mappings are computed and
     must agree, which is a real check on a real pairing.
  6. FRAGMENT OUTPUTS take locations in declaration order, which is the order
     `glDrawBuffers` addresses them in under GL.
  7. `gl_VertexID` / `gl_InstanceID` become `gl_VertexIndex` /
     `gl_InstanceIndex`. These are the same value for every draw the fork
     issues (`firstVertex` and `firstInstance` are 0 everywhere), and the day
     one is not, this line is where it is written down.
  8. Vertex `in` declarations are NOT touched: every vertex stage in the fork
     already carries `layout(location=)` on each attribute. One that does not
     is an error here.

WHAT THE TRANSFORM DOES NOT DO, and these are pipeline-state questions rather
than source ones -- they belong to the pass being ported, not here:

  * THE Y FLIP. GL's clip space has +Y up and Vulkan's has +Y down, so the same
    `gl_Position` draws the frame upside down. The fix is a negative-height
    viewport (core since Vulkan 1.1), NOT a source edit, because a source edit
    would make the Vulkan shader disagree with its own GL oracle.
  * THE DEPTH RANGE. GL maps clip z [-1, 1] to [0, 1]; Vulkan takes [0, 1]
    directly. Every shader here writes a z already in [0, 1] (they all
    `clamp(1.0 - enc/uDepthScale, 0.0, 1.0)`), so under GL the near half of
    that range is thrown away and under Vulkan it is not. Passes that depth-
    test must account for it; `tagpu_fps` does not depth-test at all, which is
    part of why it is G19d's pass.
  * `gl_FragCoord`. GL's origin is the lower left and Vulkan's the upper left.
    A flipped viewport puts it back, but a pass that reads it (the GUI sharp
    layer, the restorer) should say so where it sets its viewport up.

THE RESTORER IS HERE SINCE LANDING 7, and it is the one VARIANT set. Its five
shaders live in `tagpu_restore_glsl.h` and are compiled at runtime under a prefix
the DEVICE decides -- `NK` is `GL_MAX_UNIFORM_BLOCK_SIZE` and
`GL_MAX_DRAW_BUFFERS` divided by the weights' widest k-block, and `WMAX` is `NK`
times that same number. `NK` changes how many fragment outputs the conv pass
declares, so it cannot be a specialisation constant, and a variant is therefore
an (`NK`, `kmax`) PAIR -- `WMAX` cannot be compiled once at the largest `kmax`
and shared, because `NK` is derived from the very limit the block has to fit.
`kmax` is READ OUT OF THE WEIGHT BINARIES (`restore_kmax`) rather than written
down, so the weights are part of what these headers are generated from. The
consequence is a constraint on the project and is stated in
research/notes/vulkan-only-plan.md landing 7 rather than here: the Vulkan lane
restores with the shipped models and no others.

THE ONE INVARIANT THAT MAKES THIS SAFE, and it is checked on every run rather
than argued: strip the global-scope `in` / `out` / `uniform` declarations out of
a shader and out of its translation, and the two must be BYTE-IDENTICAL --
every function, every constant, every line of every body. The transform may
touch declarations and the two built-in renames of item 7, and the build fails
if it ever touches anything else. `residual()` below is that check.

USAGE
    tools/spirv-gen.py                 regenerate the headers (needs glslang)
    tools/spirv-gen.py --check         verify the committed headers (no glslang)
    tools/spirv-gen.py --dump DIR      write the transformed GLSL for reading
"""

import argparse
import hashlib
import os
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
DDRAW = REPO / "tagpu" / "ddraw"
OUTDIR = DDRAW / "inc" / "spirv"
GLSLANG = REPO / "tools" / "glslang" / "bin" / "glslang"

# ---------------------------------------------------------------- the manifest
#
# THE PROGRAMS, as the C code links them. This table is the one thing here that
# is not derived: which vertex stage goes with which fragment stage is a fact
# about `mkprog` call sites, and there is no honest way to read it out of the
# source. Each entry is (program name, vertex "file::symbol", fragment one).
# The line number beside each group is where that pairing is made.
PROGRAMS = [
    # tagpu_gui_surf.c -- two vertex stages (QVS and LAY_VS) and eight fragment
    # stages, from ten GLSL strings in that file. RESTORED
    # by the UI rebuild: the clean cut took these with the composite, and the
    # composite is what is not coming back. `LAY_FS` no longer samples TA's
    # frame, so none of the eight puts an engine pixel anywhere -- `TINT_FS`
    # included: what it samples is a copy of OUR OWN twin, made one command
    # earlier, and the table it indexes through is the engine's palette-derived
    # LUT and not a picture.
    ("gui_spr",      "tagpu_gui_surf::QVS",     "tagpu_gui_surf::SPR_FS"),
    ("gui_cpy",      "tagpu_gui_surf::QVS",     "tagpu_gui_surf::CPY_FS"),
    ("gui_tint",     "tagpu_gui_surf::QVS",     "tagpu_gui_surf::TINT_FS"),
    ("gui_lay",      "tagpu_gui_surf::LAY_VS",  "tagpu_gui_surf::LAY_FS"),
    ("gui_sharp",    "tagpu_gui_surf::QVS",     "tagpu_gui_surf::SHARP_FS"),
    ("gui_curs",     "tagpu_gui_surf::QVS",     "tagpu_gui_surf::CURS_FS"),
    ("gui_str",      "tagpu_gui_surf::QVS",     "tagpu_gui_surf::STR_FS"),
    ("gui_mm",       "tagpu_gui_surf::QVS",     "tagpu_gui_surf::MM_FS"),
    # THE FORK'S OWN BASE BLIT IS NOT RESTORED WITH THEM, and that is the whole
    # distinction the rebuild rests on. `surf_pal` -- PASSTHROUGH_VERT_SHADER +
    # PALETTE_FRAG_SHADER out of `inc/openglshader.h` -- resolved TA's entire
    # 8-bit frame through the palette and put it on the screen as the bottom
    # layer. That is an engine pixel by definition and it stays deleted, with
    # `openglshader` out of SOURCES and ATTR_LOCATIONS empty.
    # tagpu_native.c:686,734,754
    ("native_unit",  "tagpu_native::VS",        "tagpu_native::FS"),
    ("native_c",     "tagpu_native::CVS",       "tagpu_native::CFS"),
    ("native_d",     "tagpu_native::DVS",       "tagpu_native::DFS"),
    # tagpu_shadow.c:174-175 -- both depth-only, one empty fragment stage
    ("shadow_unit",  "tagpu_shadow::VS_U",      "tagpu_shadow::FS_NONE"),
    ("shadow_hires", "tagpu_shadow::VS_H",      "tagpu_shadow::FS_NONE"),
    # tagpu_terr.c:441
    ("terr",         "tagpu_terr::VS",          "tagpu_terr::FS"),
    # tagpu_posedraw.c:432-434 -- the fragment stage is tagpu_native's, shared
    # deliberately (tagpu_native_unit_fs()), which is why item 5 above checks
    # that two vertex stages agree about the varyings.
    ("pose_unit",    "tagpu_posedraw::VS",      "tagpu_native::FS"),
    ("pose_depth",   "tagpu_posedraw::VS",      "tagpu_posedraw::DFS"),
    # one pass each
    ("fps",          "tagpu_fps::VS",           "tagpu_fps::FS"),
    ("mark",         "tagpu_mark::VS",          "tagpu_mark::FS"),
    # the selection rect: the engine's Bresenham decided per game pixel
    ("mark_sel",     "tagpu_mark::SVS",         "tagpu_mark::SFS"),
    ("scaffold",     "tagpu_scaffold::VS",      "tagpu_scaffold::FS"),
    ("fx",           "tagpu_fx::VS",            "tagpu_fx::FS"),
    ("feat",         "tagpu_feat::VS",          "tagpu_feat::FS"),
    ("hires",        "tagpu_hires_draw::VS",    "tagpu_hires_draw::FS"),
]

# ---------------------------------------------------------------- the restorer
#
# THE ONE VARIANT SHADER SET, and the only one read out of a HEADER rather than a
# .c. `tagpu_restore_glsl.h` holds five shaders as macros under a prefix the
# consumer builds. SINCE 11-5e-2 NO C FILE INCLUDES IT: the GLSL backend that
# compiled these was deleted, so the macros' only readers are this tool (which
# turns them into the SPIR-V the Vulkan restorer runs) and `tools/tascene`
# (which extracts the same macros for the browser lab). The GLSL is the
# SOURCE OF TRUTH for both, and neither lane compiles it as GLSL any more:
#
#     #define NK   <n>    output channel-tiles per conv draw
#     #define WMAX <m>    mat4s in the bound weight range = NK * kmax
#
# `NK` guards extra `out` locations with `#if`, so it is a shape of the fragment
# interface and cannot be a specialisation constant. `WMAX` is only the declared
# length of `uniform WBlock { mat4 w[WMAX]; }` -- but it CANNOT be compiled at the
# largest kmax and shared, because `NK` is derived from the device limit that
# block has to fit: nk = MAX_UNIFORM_BLOCK_SIZE / (kmax*64), clamped to
# MAX_DRAW_BUFFERS and rounded down to a power of two. Inflating kmax declares a
# block past the very limit that chose NK. So a variant is an (NK, kmax) PAIR.
#
# KMAX IS A PROPERTY OF THE SHIPPED WEIGHTS, read out of the binaries rather than
# taken from a comment (`unditherer/models/{tiny,full}.w32.bin`, the layer table's
# widest kstride / 4). THE CONSEQUENCE IS A CONSTRAINT ON THE PROJECT and is
# stated in research/notes/vulkan-only-plan.md landing 7 rather than here: the
# Vulkan lane restores with these two models and no others, and a third needs its
# four variants generated and committed.
# THE SWITCH THAT WIRES THE RESTORER INTO SOURCES AND PROGRAMS, and it is True.
# It exists because the two shapes CONV_FS needs had to be taught to `transform`
# before the set could be generated at all, and a commit that leaves `make`
# failing its own shader gate is a trap rather than a checkpoint -- so the
# machinery landed gated and this went True in the change that taught them:
#
#   1. `uniform highp sampler2DArray uAct;` -- a precision qualifier AFTER the
#      storage qualifier. `_VAR` took `highp uniform ...` and not
#      `uniform highp ...`, so the declaration did not match, passed through
#      unchanged, and glslang refused it: "sampler/texture/image requires
#      layout(binding=X)". It is its own capture group now, re-emitted where it
#      was read.
#   2. `layout(std140) uniform WBlock { mat4 w[WMAX]; };` -- a named block
#      written on ONE line, which `_BLOCK_OPEN` never saw. `normalise_blocks`
#      splits it on the way in.
#
# Both were the tool's to learn rather than the shader's to reformat:
# `tagpu_restore_glsl.h` is the ONE copy of that text, shared with
# tools/tascene's browser pack, and reflowing it for a generator's convenience is
# the sort of thing its own header forbids. Kept as a named switch because the
# next variant set will want the same staging.
RESTORE_READY = True

RESTORE_HDR   = "tagpu_restore_glsl"
RESTORE_NK    = (1, 2, 4, 8)
RESTORE_MODELS = ("tiny", "full")


def restore_kmax():
    """`(kmax, model)` for each shipped model, READ OUT OF THE WEIGHT BINARY.

    IT IS READ AND NOT WRITTEN DOWN, AND THAT IS THE WHOLE POINT. `kmax` decides
    the declared length of `uniform WBlock { mat4 w[WMAX]; }`, so a literal here
    would sit outside the freshness chain: retrain `full.w32.bin` with a
    different widest k-block and neither the shader text nor this tool's hash
    moves, `--check` stays green, and the committed SPIR-V declares a block of
    the wrong length -- found on a device, months later. Reading the file makes
    the weights part of what the headers are generated FROM, so changing them
    fails the build until the headers are regenerated. [Landing 7's review,
    2026-09-17, which found the literal and named exactly that failure.]

    The format is `unditherer/weights.py`: u32 magic, then u32 depth, ch, ntex,
    then `depth` x {offset, jin, kout, kstride} in vec4 texels. kmax is the
    widest k-block in mat4s, `max(kstride) / 4` -- the same reduction
    `tagpu_restore_core.c:139` does at load time. (It was cited as
    `tagpu_restoreglsl.c:268` until 11-5e-2 deleted that file; the loader is
    backend-neutral and never moved.)
    """
    import struct
    out = []
    for model in RESTORE_MODELS:
        path = REPO / "unditherer" / "models" / ("%s.w32.bin" % model)
        if not path.exists():
            die("%s is not there, and the restorer's shaders are generated from "
                "it -- its widest k-block is the length of WBlock" % path)
        raw = path.read_bytes()
        try:
            depth, ch, ntex = struct.unpack_from("<III", raw, 4)
            ks = [struct.unpack_from("<IIII", raw, 16 + 16 * l)[3]
                  for l in range(depth)]
        except struct.error:
            die("%s is truncated" % path)
        if not (1 <= depth <= 32) or not ks or min(ks) == 0 or max(ks) % 4:
            die("%s: depth %d and k-strides %s are not a weight table"
                % (path, depth, ks[:4]))
        out.append((max(ks) // 4, model))
    return tuple(out)

# Which of the five actually reads NK or WMAX -- measured over the QUOTED shader
# text of each macro, not over the macro's region in the file: the doc comment
# that introduces CONV mentions NK and is not shader source, which is what makes
# a careless scan say FILL_FS depends on it. Only CONV_FS does.
RESTORE_VARIANT = ("CONV_FS",)


def restore_keys(name):
    """Every key one restorer shader contributes: one, or one per (NK, kmax)."""
    if name not in RESTORE_VARIANT:
        return [(name, None, None)]
    return [("%s_NK%d_K%d" % (name, nk, kmax), nk, nk * kmax)
            for nk in RESTORE_NK for kmax, _ in restore_kmax()]


def _restore_programs():
    out = []
    # `restore_mip` IS GENERATED BEFORE ITS VULKAN CONSUMER EXISTS, and that is
    # this manifest's rule rather than an exception to it: a shader in
    # `tagpu_restore_glsl.h` that no program uses fails the gate, precisely so
    # that a shader and its consumer cannot drift apart while one of them is
    # unwritten. The emitted header is text nothing #includes until landing
    # 7e-2 wires it, at a cost of one small array in the tree and none at run
    # time.
    #
    # THE JUSTIFICATION USED TO READ "the shader is real -- the GL lane draws it
    # as of landing 7e-1". That lane is gone: the vulkan-only plan's 11-5e-2
    # deleted `tagpu_restoreglsl.c`, which was the only code that compiled these
    # shaders at run time. `tagpu_restore_glsl.h` itself is untouched and stays
    # the ONE copy of the text -- this tool reads the five shaders straight out
    # of the header (see extract_restore) rather than out of any .c, and
    # tools/tascene extracts the same macros for the browser pack -- so nothing
    # about this manifest changes. Only the reason does.
    for prog, vs, fs in (("restore_fill", "FS_VS", "FILL_FS"),
                         ("restore_conv", "FS_VS", "CONV_FS"),
                         ("restore_out",  "OUT_VS", "OUT_FS"),
                         ("restore_mip",  "FS_VS", "MIP_FS")):
        for fkey, _, _ in restore_keys(fs):
            vkey = restore_keys(vs)[0][0]
            out.append(("%s%s" % (prog, fkey[len(fs):]),
                        "%s::%s" % (RESTORE_HDR, vkey),
                        "%s::%s" % (RESTORE_HDR, fkey)))
    return out


# Every C source a shader is read out of, in the order the headers are emitted.
SOURCES = ["tagpu_gui_surf", "tagpu_native", "tagpu_shadow", "tagpu_terr",
           "tagpu_posedraw", "tagpu_fps", "tagpu_mark", "tagpu_scaffold",
           "tagpu_fx", "tagpu_feat", "tagpu_hires_draw"]

# WHERE A VERTEX ATTRIBUTE'S LOCATION COMES FROM WHEN THE GLSL DOES NOT SAY.
# Our passes all write `layout(location = N) in ...` and need nothing here, so
# this table is empty -- and since the clean cut it has nothing that could fill
# it either. Its one entry was the upstream fork's `PASSTHROUGH_VERT_SHADER`,
# `#version 130` with no location qualifiers, where `render_ogl.c` asked the
# LINKER where each attribute landed and Vulkan needed the answer written down.
# The pass that drew it was the engine's frame on the screen; the cut deleted
# it. Kept rather than removed because the difference it documents is real --
# in GL the linker chooses and the C side queries, in Vulkan whoever builds the
# pipeline chooses -- and the next vendored `#version 130` shader will need a
# row here. [The vulkan-only plan, landing 4c-1 and THE CLEAN CUT.]
ATTR_LOCATIONS = {}

# Sources that are not `src/<name>.c`. `tagpu_restore_glsl` is a header because
# its shaders are macros (see extract_restore).
#
# `openglshader` -- the UPSTREAM fork's own shader collection in `inc/` -- USED
# TO BE HERE, because the Vulkan lane drew one pair out of it: the base blit
# that put TA's 8-bit surface on the frame through the palette. The clean cut
# deleted that draw, so no Vulkan pass reads a shader out of the fork's header
# any more and nothing is generated from it. `inc/openglshader.h` itself is
# untouched -- it is vendored, and this tool only ever read it.
# [The vulkan-only plan, landing 4c-1 and THE CLEAN CUT.]
HEADER_SOURCES = {# LIFTED SO THE GL FILE CAN GO AND THE SHADERS CAN STAY.
                  # `tagpu_shadow.c` and `tagpu_hires_draw.c` are deleted by
                  # landing 11; their GLSL is not, because the Vulkan lane
                  # draws it. `SOURCES` and `PROGRAMS` are UNCHANGED on
                  # purpose -- removing a name there stops generating the
                  # `inc/spirv/*.spv.h` that the surviving `tagpu_vk_*.c`
                  # files include. Both headers sit in `src/` because the
                  # hires fragment shader pulls `TAGPU_GLSL_*` out of
                  # `src/tagpu_glsl.h` and this preprocess runs `-Iinc` only.
                  # [The vulkan-only plan, landing 11 D1.]
                  "tagpu_shadow":     "src/tagpu_shadow_glsl.h",
                  "tagpu_hires_draw": "src/tagpu_hires_glsl.h"}

# The restorer's pairings are appended rather than written out: eight of the ten
# are the same conv program at a different (NK, kmax), and spelling them by hand
# is how the list and `restore_keys` would drift apart.
if RESTORE_READY:
    SOURCES.append(RESTORE_HDR)
    PROGRAMS += _restore_programs()

CC = os.environ.get("CC", "i686-w64-mingw32-gcc")

# ------------------------------------------------------------- the extraction


def preprocess(cfile):
    """The C source, macro-expanded, as the compiler would see it."""
    r = subprocess.run([CC, "-E", "-Iinc",
                        HEADER_SOURCES.get(cfile, "src/%s.c" % cfile)],
                       cwd=DDRAW, capture_output=True, text=True)
    if r.returncode != 0:
        die("%s: the preprocessor refused it\n%s" % (cfile, r.stderr))
    return r.stdout.split("\n")


# The literal may begin on the declaration's own line. It never does today --
# the fork's style puts `#version` on the next one -- but a tool that silently
# SKIPS a shader is worse than one that refuses it, and the "a shader no program
# uses" guard below is what forces every new shader through the pipeline: a
# shader the extractor cannot see escapes that guard too.
# BOTH SPELLINGS OF "a string literal at file scope". Our passes write
# `static const char* NAME =`; the upstream fork's `inc/openglshader.h` writes
# `static char NAME[] =`. This only ever matches MORE declarations, and a
# declaration that is not a shader is still ignored -- the caller requires a
# `#version` literal on the line or the next one, and the census counts exactly
# those. [Widened for the vulkan-only plan's landing 4c-1.]
_DECL = re.compile(r'static\s+(?:const\s+)?char\s*(?:\*\s*)?(\w+)\s*'
                   r'(?:\[\s*\]\s*)?=\s*(.*)$')
_LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')
_VERSION_LIT = re.compile(r'"#version')


def unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append({"n": "\n", "t": "\t", "r": "\r", '"': '"',
                        "\\": "\\"}.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


_MACRO = re.compile(r'^#define\s+(TAGPU_RESTORE_(\w+_(?:VS|FS)))\b')


def extract_restore():
    """The five shaders out of tagpu_restore_glsl.h, one entry per variant.

    READ OUT OF THE HEADER AND NOT OUT OF A PREPROCESSED .c, because these are
    macros pasted at call sites rather than `static const char*` initialisers --
    there is no translation unit in which they appear as a literal with a
    `#version` on the front. The prefix the runtime builds is reconstructed here
    instead, one (NK, WMAX) at a time."""
    path = DDRAW / "src" / ("%s.h" % RESTORE_HDR)
    if not path.exists():
        die("%s is not there" % path)
    lines = path.read_text().split("\n")
    bodies, i = {}, 0
    while i < len(lines):
        m = _MACRO.match(lines[i])
        if not m:
            i += 1
            continue
        short, lits = m.group(2), []
        while i < len(lines):
            lits.extend(_LIT.findall(lines[i]))
            if not lines[i].rstrip().endswith("\\"):
                break
            i += 1
        bodies[short] = "".join(unescape(x) for x in lits)
        i += 1
    if not bodies:
        die("%s has no TAGPU_RESTORE_*_{VS,FS} macros" % path)
    out = {}
    for short, body in bodies.items():
        for key, nk, wmax in restore_keys(short):
            pre = "#version 330 core\n"
            if nk is not None:
                pre += "#define NK %d\n#define WMAX %d\n" % (nk, wmax)
            out[key] = pp_expand(pre + body, key)
    return out


_IFGT = re.compile(r'^\s*#if\s+(\w+)\s*>\s*(\d+)\s*$')
_DEF = re.compile(r'^\s*#define\s+(\w+)\s+(\d+)\s*$')


def pp_expand(src, key):
    """Resolve `#define <ID> <int>` and `#if <ID> > <int>` / `#endif`, and
    substitute the identifiers.

    THE TOOL'S OWN PARSER HAS TO SEE THE FINAL INTERFACE. `transform` assigns
    locations from the global `out` declarations it can see, and CONV_FS declares
    a different number of them per NK -- so the conditionals cannot survive into
    the text that is parsed, and pre-expanding them here is what makes each
    variant ordinary GLSL from that point on.

    IT REFUSES EVERYTHING IT DOES NOT UNDERSTAND. A hand-rolled preprocessor that
    guesses is worse than none: `#else`, `#elif`, a nested `#if`, any other
    directive or any surviving `#` line but `#version` is an error that names
    itself, so a shader edit that reaches for one fails the build here instead of
    silently compiling a different interface than GL does."""
    defs, out, depth = {}, [], []
    for ln in src.split("\n"):
        # THE REGION TEST COMES FIRST, for every directive and not only for
        # text. A `#define` inside a FALSE `#if` used to be honoured -- it was
        # matched and consumed before the test below -- which is the one hole in
        # this function's promise to refuse what it does not understand, and it
        # would have been silent. Nothing in the header triggers it today.
        # [Landing 7's review, 2026-09-17.]
        if depth and not depth[0]:
            if re.match(r'^\s*#if\b', ln):
                die("%s: nested `#if` is not supported: %s" % (key, ln.strip()))
            if re.match(r'^\s*#endif\b', ln):
                depth.pop()
            continue
        m = _DEF.match(ln)
        if m:
            defs[m.group(1)] = int(m.group(2))
            continue
        m = _IFGT.match(ln)
        if m:
            if m.group(1) not in defs:
                die("%s: `%s` tests %s, which is not defined" % (key, ln.strip(), m.group(1)))
            if depth:
                die("%s: nested `#if` is not supported: %s" % (key, ln.strip()))
            depth.append(defs[m.group(1)] > int(m.group(2)))
            continue
        if re.match(r'^\s*#endif\b', ln):
            if not depth:
                die("%s: `#endif` with no `#if`" % key)
            depth.pop()
            continue
        if re.match(r'^\s*#(else|elif|if|ifdef|ifndef|undef|define)\b', ln):
            die("%s: this tool resolves only `#define <ID> <int>` and "
                "`#if <ID> > <int>` / `#endif`, and got: %s" % (key, ln.strip()))
        out.append(ln)
    if depth:
        die("%s: an `#if` was not closed" % key)
    text = "\n".join(out)
    for name, val in defs.items():
        text = re.sub(r'\b%s\b' % name, str(val), text)
    for ln in text.split("\n"):
        if ln.lstrip().startswith("#") and not ln.lstrip().startswith("#version"):
            die("%s: a directive survived expansion: %s" % (key, ln.strip()))
    return text


def extract(cfile):
    """Every `static const char* X = "#version ..."` in one C source.

    Line markers (`# 42 "src/foo.c"`) are skipped: the preprocessor emits them
    between the literals and their quoted FILE NAME would otherwise be spliced
    into the middle of a shader, which is exactly the kind of fault that would
    compile and then draw something wrong."""
    if cfile == RESTORE_HDR:
        return extract_restore()
    lines = preprocess(cfile)
    found, i = {}, 0
    while i < len(lines):
        m = _DECL.search(lines[i].strip())
        here = bool(m) and "#version" in m.group(2)
        nxt = bool(m) and i + 1 < len(lines) and "#version" in lines[i + 1]
        if here or nxt:
            name, lits = m.group(1), []
            if here:
                lits.extend(_LIT.findall(m.group(2)))
                if not lines[i].rstrip().endswith(";"):
                    i += 1
                    while i < len(lines):
                        if not lines[i].startswith("#"):
                            lits.extend(_LIT.findall(lines[i]))
                        if lines[i].rstrip().endswith(";"):
                            break
                        i += 1
            else:
                i += 1
                while i < len(lines):
                    if not lines[i].startswith("#"):
                        lits.extend(_LIT.findall(lines[i]))
                    if lines[i].rstrip().endswith(";"):
                        break
                    i += 1
            found[name] = "".join(unescape(x) for x in lits)
        i += 1

    # THE CENSUS, AND IT IS WHY THE SHAPE ABOVE DOES NOT HAVE TO BE EXHAUSTIVE.
    # Every `"#version` in the preprocessed text is the start of a shader's first
    # literal (the preprocessor has already removed the comments, and none of
    # these files ASSEMBLES a version line at run time -- they all carry it in
    # the literal. `tagpu_restoreglsl.c` did assemble one, which is why it was
    # deliberately kept out of SOURCES; 11-5e-2 deleted it, so the exclusion is
    # history rather than a live exception). So the count must equal the number
    # of shaders extracted, and a shader written in a shape this tool cannot read
    # is an ERROR rather than a silent omission.
    seen = sum(len(_VERSION_LIT.findall(l)) for l in lines if not l.startswith("#"))
    if seen != len(found):
        die("%s.c: the preprocessed source holds %d shader literals and %d were "
            "extracted (%s). A shader written in a shape extract() does not read "
            "would otherwise be skipped in silence -- and would escape the "
            "'a shader no program uses' guard as well."
            % (cfile, seen, len(found), ", ".join(sorted(found)) or "none"))
    return found


# -------------------------------------------------------------- the transform

OPAQUE = re.compile(r'^(sampler|isampler|usampler|image|iimage|uimage|texture|'
                    r'itexture|utexture|subpassInput)')

# One declaration: qualifiers, storage qualifier, an optional PRECISION
# qualifier, type, name, optional array.
#
# THE PRECISION QUALIFIER IS CAPTURED SEPARATELY BECAUSE IT SITS ON THE OTHER
# SIDE OF THE STORAGE ONE. GLSL takes it either way round and this tree writes
# both: the varyings use `flat out vec2`, and `tagpu_restore_glsl.h` writes
# `uniform highp sampler2DArray uAct`. Folding the second into `quals` would put
# it back in the FIRST position, which is legal but is not what the shader said,
# and the rewriter below re-emits each group where it was read.
# [Landing 7, 2026-09-17: CONV_FS is the first shader here to use this order, and
# until now the declaration simply did not match, passed through untouched and
# reached glslang with no binding on it.]
_VAR = re.compile(r'^\s*((?:(?:flat|smooth|noperspective|centroid|highp|mediump|lowp)\s+)*)'
                  r'(in|out|uniform)\s+'
                  r'((?:(?:highp|mediump|lowp)\s+)?)'
                  r'([A-Za-z_]\w*)\s+'
                  r'([A-Za-z_]\w*)\s*'
                  r'((?:\[[^\]]*\])?)\s*$')

_LAYOUT_LOC = re.compile(r'layout\s*\(\s*location\s*=\s*(\d+)\s*\)')
_BLOCK_OPEN = re.compile(r'^\s*layout\s*\(\s*(std140|std430)\s*\)\s*uniform\s+(\w+)\s*\{\s*$')

# The same block written on ONE line, `{ members } ;` and all. Split rather than
# parsed in place: every reader below already handles the multi-line spelling, and
# one normalisation is a smaller thing to get right than a second code path in the
# parser, the transform and the emitter.
# [Landing 7, 2026-09-17. `tagpu_restore_glsl.h`'s `layout(std140) uniform WBlock
# { mat4 w[WMAX]; };` is the first in this tree; before this the block was never
# seen at all and so never got its set/binding.]
_BLOCK_1LINE = re.compile(
    r'^(\s*layout\s*\(\s*(?:std140|std430)\s*\)\s*uniform\s+\w+\s*\{)'
    r'(.+?)'
    r'(\}\s*;)\s*$')

# The same thing with an INSTANCE name -- `... { mat4 w[N]; } wb;`. No shader here
# writes one, and the point of matching it is to REFUSE it: without this it slips
# past `_BLOCK_1LINE`, past `_BLOCK_OPEN`, and out to glslang with no set/binding,
# which is precisely the failure landing 7 spent a run diagnosing.
# [Landing 7's review, 2026-09-17.]
_BLOCK_1LINE_INST = re.compile(
    r'^\s*layout\s*\(\s*(?:std140|std430)\s*\)\s*uniform\s+\w+\s*\{.+?\}\s*\w+\s*;\s*$')


def normalise_blocks(src):
    """A one-line named uniform block, spread over three lines.

    THE SHADER TEXT IS NOT EDITED ON DISK, and that is the point: this header is
    the one copy of it and is shared with tools/tascene's browser pack, so
    reflowing it to suit a generator is what its own header forbids. The
    normalisation happens on the way IN, to both the GL side and the translation,
    so `residual()` still compares like with like."""
    out = []
    for ln in src.split("\n"):
        if _BLOCK_1LINE_INST.match(ln):
            die("a one-line uniform block with an instance name is not handled: "
                "%s" % ln.strip())
        m = _BLOCK_1LINE.match(ln)
        if not m:
            out.append(ln)
            continue
        out.append(m.group(1))
        for st in m.group(2).split(";"):
            if st.strip():
                out.append("  %s;" % st.strip())
        out.append(m.group(3))
    return "\n".join(out)

# std140: (size, alignment) for every type the fork's shaders use.
_STD140 = {
    "float": (4, 4), "int": (4, 4), "uint": (4, 4), "bool": (4, 4),
    "vec2": (8, 8), "ivec2": (8, 8), "uvec2": (8, 8),
    "vec3": (12, 16), "ivec3": (12, 16), "uvec3": (12, 16),
    "vec4": (16, 16), "ivec4": (16, 16), "uvec4": (16, 16),
    "mat2": (32, 16), "mat3": (48, 16), "mat4": (64, 16),
}

# how many interface LOCATIONS one variable of this type consumes
_LOCS = {"mat2": 2, "mat3": 3, "mat4": 4}


def locs_of(ty, arr):
    n = _LOCS.get(ty, 1)
    return n * arr if arr else n


def std140_place(offset, ty, arr):
    """(offset of this member, offset after it). Arrays and matrices take a
    16-byte stride per element, which is the rule that bites a `float u[4]`."""
    if ty not in _STD140:
        die("std140: no rule for type '%s'" % ty)
    size, align = _STD140[ty]
    if arr:
        align = max(align, 16)
        size = align_up(size, 16) * arr
    offset = align_up(offset, align)
    return offset, offset + size


def align_up(v, a):
    return (v + a - 1) // a * a


def split_statements(line):
    """A global-scope line into its `;`-separated statements, `;` kept."""
    out, cur = [], ""
    for ch in line:
        cur += ch
        if ch == ";":
            out.append(cur)
            cur = ""
    if cur.strip():
        out.append(cur)
    return out


class Shader(object):
    def __init__(self, key, stage, src):
        self.key = key            # "tagpu_fps::VS"
        self.stage = stage        # "vert" | "frag"
        self.src = src
        self.globals = []         # (type, name, arrsize) in declaration order
        self.samplers = []        # (type, name)
        self.blocks = []          # named block names, in declaration order
        self.outs = {}            # name -> (type, arr) for a vertex stage
        self.out_order = []
        self.ins = []             # (name, type, arr) for a fragment stage
        self.vk = None            # the transformed source
        self.offsets = []         # (name, type, arr, offset) in the block
        self.block_size = 0


def parse(sh):
    """Collect what the transform needs, at global scope only."""
    depth = 0
    for raw in sh.src.split("\n"):
        line = raw
        if depth == 0 and not line.lstrip().startswith("#"):
            for st in split_statements(line):
                body = st.rstrip().rstrip(";")
                if _BLOCK_OPEN.match(body + "\n") or _BLOCK_OPEN.match(st):
                    continue
                m = _VAR.match(strip_layout(body))
                if not m:
                    continue
                quals, kind, _prec, ty, name, arr = m.groups()
                n = arr_size(arr)
                if kind == "uniform":
                    if OPAQUE.match(ty):
                        sh.samplers.append((ty, name))
                    else:
                        sh.globals.append((ty, name, n))
                elif kind == "out" and sh.stage == "vert":
                    sh.outs[name] = (ty, n, norm_quals(quals))
                    sh.out_order.append(name)
                elif kind == "in" and sh.stage == "frag":
                    sh.ins.append((name, ty, n, norm_quals(quals)))
        m = _BLOCK_OPEN.match(raw)
        if m:
            sh.blocks.append(m.group(2))
        depth += raw.count("{") - raw.count("}")


def norm_quals(q):
    """The interpolation qualifier, order- and whitespace-independent. It has to
    MATCH between the two stages: `flat out vec2` against a plain `in vec2` is a
    link failure in a driver, and the generator can say so instead."""
    return " ".join(sorted(q.split()))


def strip_layout(s):
    return re.sub(r'^\s*layout\s*\([^)]*\)\s*', "", s)


def arr_size(arr):
    if not arr:
        return 0
    inner = arr.strip()[1:-1].strip()
    if not inner:
        die("an unsized array is not supported here: %s" % arr)
    try:
        return int(eval(inner, {"__builtins__": {}}, {}))    # constants only
    except Exception:
        die("array size '%s' is not a constant expression" % inner)


_VKBLOCK = re.compile(r'^layout\(set = 0, binding = \d+, (std140|std430)\) uniform (\w+) \{$')
_GLOBALS_OPEN = "uniform _Globals {"


def residual(text):
    """Everything in a shader that is NOT a global-scope in/out/uniform
    declaration and not the inserted globals block.

    THIS IS WHAT MAKES "THE TRANSFORM CANNOT CHANGE WHAT A SHADER COMPUTES" A
    FACT RATHER THAN A CLAIM. The transform is allowed to touch exactly those
    declarations and nothing else, so a shader and its translation must have
    byte-identical residuals -- every statement, every constant, every line of
    every function body, unchanged. `build_all` asserts it for every one of them
    on every run, including `--check`, so a future edit to the transform that
    started rewriting a body would fail the build rather than draw something
    subtly wrong.

    The one exception is item 7 of the transform (gl_VertexID/gl_InstanceID),
    which is applied to both sides before comparing -- it IS a body edit, it is
    the only one, and naming it here is the point."""
    out, depth, skip = [], 0, False
    for raw in text.split("\n"):
        if raw.startswith("#version"):
            continue
        if _GLOBALS_OPEN in raw:
            skip = True
            continue
        if skip:
            if raw.strip() == "};":
                skip = False
            continue
        m = _BLOCK_OPEN.match(raw) or _VKBLOCK.match(raw)
        if m:
            out.append("uniform %s {" % m.group(2))
            depth += 1
            continue
        if depth == 0 and not raw.lstrip().startswith("#"):
            keep = []
            for st in split_statements(raw):
                m = _VAR.match(strip_layout(st.rstrip().rstrip(";")))
                if m:
                    continue
                keep.append(st)
            joined = " ".join(x.strip() for x in keep if x.strip())
            depth += raw.count("{") - raw.count("}")
            if joined:
                out.append(joined)
            continue
        depth += raw.count("{") - raw.count("}")
        out.append(raw)
    return "\n".join(out)


def builtins_renamed(text):
    text = re.sub(r'\bgl_VertexID\b', "gl_VertexIndex", text)
    return re.sub(r'\bgl_InstanceID\b', "gl_InstanceIndex", text)


VERT_BASE, FRAG_BASE = 0, 32


def transform(sh, varying_loc):
    """The GL-330 source into Vulkan GLSL. `varying_loc` maps a varying's name
    to the location its vertex stage gave it."""
    base = VERT_BASE if sh.stage == "vert" else FRAG_BASE
    smp_base = base + 8
    # THE GAPS IN THE ALLOCATION ARE BOUNDS, SO THEY ARE CHECKED. Named blocks
    # run from base+1 and samplers from base+8, and a stage with seven named
    # blocks is the last one that cannot collide; the stages run 32 apart, so a
    # stage with twenty-four samplers is the last one that stays in its half.
    # Neither is close today (one named block, four samplers at most) and
    # neither would announce itself: two descriptors at one binding is a
    # validation error nobody here is running a layer to see.
    if len(sh.blocks) > 7:
        die("%s: %d named uniform blocks would run into this stage's sampler "
            "bindings at %d -- the allocation in this file's header needs "
            "widening" % (sh.key, len(sh.blocks), smp_base))
    if len(sh.samplers) > 24:
        die("%s: %d samplers would run past this stage's half of the binding "
            "space" % (sh.key, len(sh.samplers)))
    blk = {n: base + 1 + i for i, n in enumerate(sh.blocks)}
    smp = {n: smp_base + i for i, (_, n) in enumerate(sh.samplers)}

    # the globals block, and its std140 offsets -- the C side's contract
    off = 0
    for ty, name, arr in sh.globals:
        at, off = std140_place(off, ty, arr)
        sh.offsets.append((name, ty, arr, at))
    sh.block_size = align_up(off, 16)

    out, depth, frag_loc, emitted = [], 0, 0, False
    for raw in sh.src.split("\n"):
        line = raw

        if line.strip().startswith("#version"):
            out.append("#version 450")
            continue

        # the globals block goes in as soon as the version line is behind us,
        # before anything can refer to a uniform
        if not emitted and depth == 0 and not line.lstrip().startswith("#") \
                and line.strip():
            if sh.globals:
                out.append("layout(set = 0, binding = %d, std140) uniform _Globals {" % base)
                for name, ty, arr, at in sh.offsets:
                    out.append("  %s %s%s;  // offset %d" %
                               (ty, name, "[%d]" % arr if arr else "", at))
                out.append("};")
            emitted = True

        m = _BLOCK_OPEN.match(line)
        if m:
            out.append("layout(set = 0, binding = %d, %s) uniform %s {"
                       % (blk[m.group(2)], m.group(1), m.group(2)))
            depth += 1
            continue

        if depth == 0 and not line.lstrip().startswith("#"):
            pieces, changed = [], False
            for st in split_statements(line):
                body = st.rstrip()
                semi = body.endswith(";")
                body = body.rstrip(";")
                m = _VAR.match(strip_layout(body))
                if not m:
                    pieces.append(st)
                    continue
                quals, kind, prec, ty, name, arr = m.groups()
                n = arr_size(arr)
                # THE QUALIFIER ORDER IS NOT FREE: an interpolation qualifier
                # comes before the storage one (`flat out vec2`) and a precision
                # qualifier after it (`uniform highp sampler2DArray`), so the
                # pieces are kept apart rather than pasted back together as they
                # were read, and each goes back where it came from.
                tail = "%s%s %s%s%s" % (prec, ty, name, arr, ";" if semi else "")
                if kind == "uniform" and OPAQUE.match(ty):
                    pieces.append("layout(set = 0, binding = %d) uniform %s%s"
                                  % (smp[name], quals, tail))
                    changed = True
                elif kind == "uniform":
                    changed = True            # it moved into the block above
                elif kind == "out" and sh.stage == "vert":
                    pieces.append("layout(location = %d) %sout %s"
                                  % (varying_loc[name], quals, tail))
                    changed = True
                elif kind == "in" and sh.stage == "frag":
                    if name not in varying_loc:
                        die("%s: fragment input '%s' has no matching vertex output"
                            % (sh.key, name))
                    pieces.append("layout(location = %d) %sin %s"
                                  % (varying_loc[name], quals, tail))
                    changed = True
                elif kind == "out" and sh.stage == "frag":
                    have = _LAYOUT_LOC.search(st)
                    loc = int(have.group(1)) if have else frag_loc
                    frag_loc = loc + locs_of(ty, n)
                    pieces.append("layout(location = %d) %sout %s"
                                  % (loc, quals, tail))
                    changed = True
                elif kind == "in" and sh.stage == "vert":
                    if not _LAYOUT_LOC.search(st):
                        loc = ATTR_LOCATIONS.get(sh.key, {}).get(name)
                        if loc is None:
                            die("%s: vertex attribute '%s' has no layout(location=)"
                                " and ATTR_LOCATIONS does not give it one"
                                % (sh.key, name))
                        pieces.append("layout(location = %d) %s" % (loc, st))
                        changed = True
                    else:
                        pieces.append(st)
                else:
                    pieces.append(st)
            if changed:
                joined = " ".join(p.strip() for p in pieces if p.strip())
                # THE DEPTH IS UPDATED ON THIS PATH TOO. It is a `continue`, and
                # an earlier version skipped the count -- so a line carrying both
                # a declaration and a brace would have left every later line
                # looking like global scope. No shader does that today; the
                # reason to fix it is that the failure would be silent and would
                # rewrite something inside a function body.
                depth += line.count("{") - line.count("}")
                if joined.strip():
                    out.append(joined)
                continue

        depth += line.count("{") - line.count("}")
        out.append(line)

    text = "\n".join(out)
    text = re.sub(r'\bgl_VertexID\b', "gl_VertexIndex", text)
    text = re.sub(r'\bgl_InstanceID\b', "gl_InstanceIndex", text)
    sh.vk = text
    return text


# ------------------------------------------------------------------- the build


def die(msg):
    sys.stderr.write("spirv-gen: %s\n" % msg)
    sys.exit(2)


def tool_hash():
    """This file's own hash, so a change to the TRANSFORM invalidates every
    committed header and not only the shaders whose text moved."""
    return hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()[:16]


# EVERY SHADER THIS TOOL SEES IS A PROGRAM AGAIN, so the exemption list is
# empty. The rule -- a shader with no program fails the gate -- exists so our
# two lanes cannot drift apart while one of them is unwritten, and it holds for
# every shader WE own. The exemptions were all `inc/openglshader.h`'s: the
# upstream fork's collection, read here because one pair of it put TA's 8-bit
# surface on the Vulkan frame, with the GL-110 editions and the fork's
# upscaling filters exempted beside it. The clean cut deleted that draw and the
# header left `SOURCES` with it, so there is nothing left to exempt.
# [The vulkan-only plan, landing 4c-1 and THE CLEAN CUT.]
NOT_PROGRAMS = frozenset()


def build_all():
    """Every shader, parsed, paired and transformed. Returns an ordered list of
    Shader with `.vk` filled in."""
    texts = {}
    for cfile in SOURCES:
        for name, src in extract(cfile).items():
            texts["%s::%s" % (cfile, name)] = normalise_blocks(src)

    stage = {}
    for _, vs, fs in PROGRAMS:
        for key, st in ((vs, "vert"), (fs, "frag")):
            if key not in texts:
                die("the manifest names %s and the source does not have it" % key)
            if stage.get(key, st) != st:
                die("%s is used as both a vertex and a fragment stage" % key)
            stage[key] = st
    for key in texts:
        if key not in stage and key not in NOT_PROGRAMS:
            die("%s is a shader no program in the manifest uses -- add it, or "
                "say here why it is not a program" % key)

    shaders = {}
    for key, st in stage.items():
        sh = Shader(key, st, texts[key])
        parse(sh)
        shaders[key] = sh

    # locations, decided by each vertex stage and inherited by its fragments
    vloc = {}
    for key, sh in shaders.items():
        if sh.stage != "vert":
            continue
        at, m = 0, {}
        for name in sh.out_order:
            ty, arr, _ = sh.outs[name]
            m[name] = at
            at += locs_of(ty, arr)
        vloc[key] = m

    floc = {}
    for prog, vs, fs in PROGRAMS:
        m = vloc[vs]
        if fs in floc and floc[fs] != m:
            die("%s takes its varyings from two vertex stages that disagree "
                "(%s); one of them has to change" % (fs, vs))
        floc[fs] = m
        # THE STAGES ARE MATCHED BY NAME, SO THE REST OF THE DECLARATION IS
        # CHECKED. A fragment input whose type, array size or interpolation
        # qualifier differs from the vertex output of the same name links
        # against nothing -- and under Vulkan that is a pipeline-creation
        # failure at run time, on a device, rather than an error here.
        for name, ty, arr, q in shaders[fs].ins:
            if name not in shaders[vs].outs:
                die("%s (program %s): fragment input '%s' has no matching "
                    "output in %s" % (fs, prog, name, vs))
            vty, varr, vq = shaders[vs].outs[name]
            if (vty, varr, vq) != (ty, arr, q):
                die("%s (program %s): '%s' is `%s%s%s` in %s and `%s%s%s` here "
                    % (fs, prog, name, vq + " " if vq else "", vty,
                       "[%d]" % varr if varr else "", vs,
                       q + " " if q else "", ty, "[%d]" % arr if arr else ""))

    for key, sh in shaders.items():
        transform(sh, vloc[key] if sh.stage == "vert" else floc[key])
        a, b = residual(builtins_renamed(sh.src)), residual(sh.vk)
        if a != b:
            import difflib
            sys.stderr.write("\n".join(difflib.unified_diff(
                a.split("\n"), b.split("\n"), "gl", "vulkan", lineterm="")) + "\n")
            die("%s: the transform changed something that is not a declaration "
                "-- see the diff above" % key)

    return [shaders[k] for k in sorted(shaders)]


def compile_spv(sh):
    if not GLSLANG.exists():
        die("glslang is not here (%s).\nRun tools/glslang-fetch.sh first -- it "
            "is a dev-loop tool, gitignored like tools/ghidra/, and no build "
            "needs it." % GLSLANG)
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / ("s." + sh.stage)
        spv = pathlib.Path(d) / "s.spv"
        src.write_text(sh.vk)
        r = subprocess.run([str(GLSLANG), "-V", "--target-env", "vulkan1.0",
                            "-o", str(spv), str(src)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write("---- %s (%s) ----\n%s\n" % (sh.key, sh.stage, sh.vk))
            die("%s: glslang refused it\n%s%s" % (sh.key, r.stdout, r.stderr))
        data = spv.read_bytes()
    if len(data) % 4:
        die("%s: SPIR-V is not a whole number of words" % sh.key)
    import struct
    return list(struct.unpack("<%dI" % (len(data) // 4), data))


def sym(key):
    return key.replace("::", "_")


def sym_key(name):
    """`tagpu_fps_VS` back to `tagpu_fps::VS` -- the inverse of `sym`, which is
    unambiguous because a shader symbol never contains `_` at the join and the
    C file names are fixed in SOURCES."""
    for cfile in SOURCES:
        if name.startswith(cfile + "_"):
            return "%s::%s" % (cfile, name[len(cfile) + 1:])
    return name


def emit(cfile, shaders, words):
    """One header per C source, so a shader edit moves one file and its diff is
    readable."""
    guard = "TAGPU_SPIRV_%s_H" % cfile.upper()
    L = ["/* GENERATED by tools/spirv-gen.py -- do not edit.",
         " *",
         " * The Vulkan (SPIR-V) edition of %s's shaders. The source of"
         % (HEADER_SOURCES.get(cfile,
                               "%s%s" % (cfile, ".h" if cfile == RESTORE_HDR else ".c"))),
         " * truth is the GLSL string in that file; this is its translation, and",
         " * `make` fails if the two have drifted (tools/spirv-check.sh).",
         " *",
         " * transform " + tool_hash(),
         " */",
         "#ifndef %s" % guard,
         "#define %s" % guard,
         "",
         "#include <stdint.h>",
         ""]
    for sh in shaders:
        w = words[sh.key]
        L.append("/* %s -- %s stage, %d words" % (sh.key, sh.stage, len(w)))
        if sh.globals:
            L.append(" *")
            L.append(" * set 0 binding %d, std140, %d bytes:"
                     % (VERT_BASE if sh.stage == "vert" else FRAG_BASE,
                        sh.block_size))
            for name, ty, arr, at in sh.offsets:
                L.append(" *   %4d  %-6s %s%s" % (at, ty, name,
                                                  "[%d]" % arr if arr else ""))
        # NAMED BLOCKS ARE REPORTED TOO, and until landing 7 they were not --
        # because until landing 7 no shader in this tree had one. The header is
        # the CONTRACT for the C side (that is what the offsets above are for),
        # and `tagpu_restore_glsl`'s `WBlock` had its binding written down
        # nowhere: the allocation rule is in this file's own header comment, so
        # the only way to learn it was to read the generator or to decode the
        # module by hand. Both were done before this line existed.
        # The block's SIZE is deliberately not reported: `WBlock` is
        # `mat4 w[WMAX]` and WMAX is NK x kmax, which varies per variant and
        # which the C side already computes to size its buffer.
        for i, bn in enumerate(sh.blocks):
            L.append(" * set 0 binding %d: uniform block %s"
                     % ((VERT_BASE if sh.stage == "vert" else FRAG_BASE) + 1 + i, bn))
        for i, (ty, name) in enumerate(sh.samplers):
            L.append(" * set 0 binding %d: %s %s"
                     % ((VERT_BASE if sh.stage == "vert" else FRAG_BASE) + 8 + i,
                        ty, name))
        L.append(" * glsl %s" % glsl_hash(sh))
        L.append(" * words %s" % words_hash(w))
        L.append(" */")
        L.append("static const uint32_t tagpu_spv_%s[] = {" % sym(sh.key))
        for i in range(0, len(w), 6):
            L.append("    " + " ".join("0x%08Xu," % x for x in w[i:i + 6]))
        L.append("};")
        L.append("")
    L.append("#endif")
    (OUTDIR / ("%s.spv.h" % cfile)).write_text("\n".join(L) + "\n")


def glsl_hash(sh):
    return hashlib.sha256(sh.vk.encode()).hexdigest()[:32]


def words_hash(w):
    """The SPIR-V itself. The GLSL hash says the INPUT has not moved; this says
    the OUTPUT in the file is the output that was generated from it -- which is
    the half a hand edit, a truncation or a bad merge lands in, and the half
    `-fsyntax-only` happily accepts."""
    import struct
    return hashlib.sha256(struct.pack("<%dI" % len(w), *w)).hexdigest()[:32]


_HASHLINE = re.compile(r'^ \* glsl ([0-9a-f]{32})$')
_WORDLINE = re.compile(r'^ \* words ([0-9a-f]{32})$')
_TOOLLINE = re.compile(r'^ \* transform ([0-9a-f]{16})$')
_ARRAY = re.compile(r'^static const uint32_t tagpu_spv_(\w+)\[\] = \{$')
_WORD = re.compile(r'0x([0-9A-F]{8})u')


def committed_hashes():
    """What the committed headers say, and what they actually CONTAIN. Read with
    a regex rather than by compiling anything, because `--check` must run in a
    build that has no glslang -- which is every build. Returns
    (glsl hash by key, words hash CLAIMED by key, words hash MEASURED by key,
    transform hash by file)."""
    claim, said, got, tool = {}, {}, {}, {}
    for cfile in SOURCES:
        p = OUTDIR / ("%s.spv.h" % cfile)
        if not p.exists():
            return None, None, None, None
        key, arr, words = None, None, []
        for line in p.read_text().split("\n"):
            m = _TOOLLINE.match(line)
            if m:
                tool[cfile] = m.group(1)
                continue
            # THE SHADER'S OWN COMMENT LINE, `/* <file>::<NAME> -- <stage>...`.
            # This used to test for the `tagpu_` prefix, which quietly made the
            # reader blind to any source not named that way: every hash in the
            # header was then attributed to no key, and the shader reported as
            # "has no `words` hash -- regenerate" however freshly it had been
            # generated. Found when `openglshader` joined SOURCES.
            # [The vulkan-only plan, landing 4c-1.]
            if line.startswith("/* ") and "::" in line and " -- " in line:
                key = line[3:].split(" --")[0]
                continue
            m = _HASHLINE.match(line)
            if m and key:
                claim[key] = m.group(1)
                continue
            m = _WORDLINE.match(line)
            if m and key:
                said[key] = m.group(1)
                continue
            m = _ARRAY.match(line)
            if m:
                # KEYED BY THE ARRAY'S OWN SYMBOL, cross-checked against the
                # comment above it. Keying purely off the comment would let a
                # header whose comment and array had drifted apart -- a bad
                # merge is exactly how that happens -- hash one shader's words
                # under another shader's name and pass.
                arr, words = sym_key(m.group(1)), []
                if key is not None and key != arr:
                    die("%s.spv.h: the comment names %s and the array under it "
                        "is %s" % (cfile, key, arr))
                key = arr
                continue
            if arr is not None:
                if line.startswith("};"):
                    got[arr] = words_hash(words)
                    arr, key = None, None
                else:
                    words.extend(int(x, 16) for x in _WORD.findall(line))
    return claim, said, got, tool


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--dump")
    a = ap.parse_args()

    shaders = build_all()

    if a.dump:
        d = pathlib.Path(a.dump)
        d.mkdir(parents=True, exist_ok=True)
        for sh in shaders:
            (d / ("%s.%s" % (sym(sh.key), sh.stage))).write_text(sh.vk)
        print("dumped %d shaders to %s" % (len(shaders), d))
        return 0

    if a.check:
        have, said, got, tool = committed_hashes()
        if have is None:
            die("tagpu/ddraw/inc/spirv/ is not generated -- run tools/spirv-gen.py")
        bad = []
        for cfile, t in tool.items():
            if t != tool_hash():
                bad.append("%s.spv.h was generated by a different spirv-gen.py "
                           "(%s, now %s)" % (cfile, t, tool_hash()))
        for sh in shaders:
            if have.get(sh.key) != glsl_hash(sh):
                bad.append("%s has changed since its SPIR-V was generated" % sh.key)
            if sh.key not in said:
                bad.append("%s has no `words` hash -- regenerate" % sh.key)
            elif said[sh.key] != got.get(sh.key):
                bad.append("%s's SPIR-V does not match the hash beside it -- the "
                           "words in the header have been edited or truncated" % sh.key)
        for key in have:
            if key not in {s.key for s in shaders}:
                bad.append("%s is in the headers and not in the source" % key)
        if bad:
            sys.stderr.write(
                "spirv-gen --check: the committed SPIR-V is stale.\n"
                "  " + "\n  ".join(bad) + "\n"
                "Run tools/spirv-gen.py and commit tagpu/ddraw/inc/spirv/.\n")
            return 1
        print("spirv: %d shaders, %d programs, headers current"
              % (len(shaders), len(PROGRAMS)))
        return 0

    OUTDIR.mkdir(parents=True, exist_ok=True)
    words = {}
    total = 0
    for sh in shaders:
        words[sh.key] = compile_spv(sh)
        total += len(words[sh.key])
    for cfile in SOURCES:
        mine = [s for s in shaders if s.key.startswith(cfile + "::")]
        emit(cfile, mine, words)
    print("spirv: %d shaders in %d programs -> %d words (%d KB) in %d headers"
          % (len(shaders), len(PROGRAMS), total, total * 4 // 1024, len(SOURCES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
