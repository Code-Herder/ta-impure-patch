#!/usr/bin/env python3
"""G19c -- the shader pipeline: the fork's GLSL, translated to SPIR-V.

WHY THIS TOOL EXISTS AT ALL, AND WHY IT DOES NOT SIMPLY COMPILE A DIRECTORY OF
`.vert` / `.frag` FILES. The GL renderer's shaders live where they are used --
as C string literals inside the pass that owns them, next to the comment that
explains the maths. That is the one copy. A Vulkan lane that carried its own
edition of each shader would be a second copy of thirty-four programs that
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
to committing generated code. Every generated header carries, per shader, the
SHA-256 of the Vulkan GLSL it was compiled from, plus the hash of this tool's
own transform. `--check` re-runs the extraction and the transform -- both of
which need the C preprocessor and nothing else, never glslang -- and compares.
A shader edited in its C string without re-running this tool fails the build,
with the shader named. `tools/spirv-check.sh` is what the Makefile calls, in the
same place and for the same reason as `thread-split-check.sh`.

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

THE RESTORER IS NOT HERE, and the reason is a fact about it rather than a
shortcut. `tagpu_restore_glsl.h`'s five shaders are compiled at runtime under a
prefix the DEVICE decides -- `NK` is `GL_MAX_UNIFORM_BLOCK_SIZE` and
`GL_MAX_DRAW_BUFFERS` divided by the weights' widest k-block, and `WMAX` is
`NK` times a number read out of the weights file. `NK` changes how many
fragment outputs the conv pass declares, so it cannot be a specialisation
constant, and the variants are a cross product of a device limit and a data
file. Pre-compiling it means enumerating that cross product, which is a
decision about what the shipped weights are allowed to be -- G19e's, when the
restorer pass is actually ported, and stated in the roadmap rather than decided
here. The other thirty-four are all of the fork's fixed shaders.

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
    # tagpu_gui_surf.c:658-664 -- one quad vertex stage, six fragment stages
    ("gui_spr",      "tagpu_gui_surf::QVS",     "tagpu_gui_surf::SPR_FS"),
    ("gui_cpy",      "tagpu_gui_surf::QVS",     "tagpu_gui_surf::CPY_FS"),
    ("gui_lay",      "tagpu_gui_surf::LAY_VS",  "tagpu_gui_surf::LAY_FS"),
    ("gui_sharp",    "tagpu_gui_surf::QVS",     "tagpu_gui_surf::SHARP_FS"),
    ("gui_curs",     "tagpu_gui_surf::QVS",     "tagpu_gui_surf::CURS_FS"),
    ("gui_str",      "tagpu_gui_surf::QVS",     "tagpu_gui_surf::STR_FS"),
    ("gui_mm",       "tagpu_gui_surf::QVS",     "tagpu_gui_surf::MM_FS"),
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
    ("scaffold",     "tagpu_scaffold::VS",      "tagpu_scaffold::FS"),
    ("fx",           "tagpu_fx::VS",            "tagpu_fx::FS"),
    ("feat",         "tagpu_feat::VS",          "tagpu_feat::FS"),
    ("hires",        "tagpu_hires_draw::VS",    "tagpu_hires_draw::FS"),
]

# Every C source a shader is read out of, in the order the headers are emitted.
SOURCES = ["tagpu_gui_surf", "tagpu_native", "tagpu_shadow", "tagpu_terr",
           "tagpu_posedraw", "tagpu_fps", "tagpu_mark", "tagpu_scaffold",
           "tagpu_fx", "tagpu_feat", "tagpu_hires_draw"]

CC = os.environ.get("CC", "i686-w64-mingw32-gcc")

# ------------------------------------------------------------- the extraction


def preprocess(cfile):
    """The C source, macro-expanded, as the compiler would see it."""
    r = subprocess.run([CC, "-E", "-Iinc", "src/%s.c" % cfile],
                       cwd=DDRAW, capture_output=True, text=True)
    if r.returncode != 0:
        die("%s: the preprocessor refused it\n%s" % (cfile, r.stderr))
    return r.stdout.split("\n")


_DECL = re.compile(r'static\s+const\s+char\s*\*\s*(\w+)\s*=\s*$')
_LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


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


def extract(cfile):
    """Every `static const char* X = "#version ..."` in one C source.

    Line markers (`# 42 "src/foo.c"`) are skipped: the preprocessor emits them
    between the literals and their quoted FILE NAME would otherwise be spliced
    into the middle of a shader, which is exactly the kind of fault that would
    compile and then draw something wrong."""
    lines = preprocess(cfile)
    found, i = {}, 0
    while i < len(lines):
        if _DECL.search(lines[i].strip()) and i + 1 < len(lines) \
                and "#version" in lines[i + 1]:
            name = _DECL.search(lines[i].strip()).group(1)
            i += 1
            lits = []
            while i < len(lines):
                if not lines[i].startswith("#"):
                    lits.extend(_LIT.findall(lines[i]))
                if lines[i].rstrip().endswith(";"):
                    break
                i += 1
            found[name] = "".join(unescape(x) for x in lits)
        i += 1
    return found


# -------------------------------------------------------------- the transform

OPAQUE = re.compile(r'^(sampler|isampler|usampler|image|iimage|uimage|texture|'
                    r'itexture|utexture|subpassInput)')

# one declaration: qualifiers, type, name, optional array, optional initialiser
_VAR = re.compile(r'^\s*((?:(?:flat|smooth|noperspective|centroid|highp|mediump|lowp)\s+)*)'
                  r'(in|out|uniform)\s+'
                  r'([A-Za-z_]\w*)\s+'
                  r'([A-Za-z_]\w*)\s*'
                  r'((?:\[[^\]]*\])?)\s*$')

_LAYOUT_LOC = re.compile(r'layout\s*\(\s*location\s*=\s*(\d+)\s*\)')
_BLOCK_OPEN = re.compile(r'^\s*layout\s*\(\s*(std140|std430)\s*\)\s*uniform\s+(\w+)\s*\{\s*$')

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
                quals, kind, ty, name, arr = m.groups()
                n = arr_size(arr)
                if kind == "uniform":
                    if OPAQUE.match(ty):
                        sh.samplers.append((ty, name))
                    else:
                        sh.globals.append((ty, name, n))
                elif kind == "out" and sh.stage == "vert":
                    sh.outs[name] = (ty, n)
                    sh.out_order.append(name)
                elif kind == "in" and sh.stage == "frag":
                    sh.ins.append((name, ty, n))
        m = _BLOCK_OPEN.match(raw)
        if m:
            sh.blocks.append(m.group(2))
        depth += raw.count("{") - raw.count("}")


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
    every function body, unchanged. `build_all` asserts it for all thirty-four
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
                quals, kind, ty, name, arr = m.groups()
                n = arr_size(arr)
                # THE QUALIFIER ORDER IS NOT FREE: an interpolation qualifier
                # comes before the storage one (`flat out vec2`), so the pieces
                # are kept apart rather than pasted back together as they were
                # read.
                tail = "%s %s%s%s" % (ty, name, arr, ";" if semi else "")
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
                        die("%s: vertex attribute '%s' has no layout(location=)"
                            % (sh.key, name))
                    pieces.append(st)
                else:
                    pieces.append(st)
            if changed:
                joined = " ".join(p.strip() for p in pieces if p.strip())
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


def build_all():
    """Every shader, parsed, paired and transformed. Returns an ordered list of
    Shader with `.vk` filled in."""
    texts = {}
    for cfile in SOURCES:
        for name, src in extract(cfile).items():
            texts["%s::%s" % (cfile, name)] = src

    stage = {}
    for _, vs, fs in PROGRAMS:
        for key, st in ((vs, "vert"), (fs, "frag")):
            if key not in texts:
                die("the manifest names %s and the source does not have it" % key)
            if stage.get(key, st) != st:
                die("%s is used as both a vertex and a fragment stage" % key)
            stage[key] = st
    for key in texts:
        if key not in stage:
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
            ty, arr = sh.outs[name]
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


def emit(cfile, shaders, words):
    """One header per C source, so a shader edit moves one file and its diff is
    readable."""
    guard = "TAGPU_SPIRV_%s_H" % cfile.upper()
    L = ["/* GENERATED by tools/spirv-gen.py -- do not edit.",
         " *",
         " * The Vulkan (SPIR-V) edition of %s.c's shaders. The source of" % cfile,
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
        for i, (ty, name) in enumerate(sh.samplers):
            L.append(" * set 0 binding %d: %s %s"
                     % ((VERT_BASE if sh.stage == "vert" else FRAG_BASE) + 8 + i,
                        ty, name))
        L.append(" * glsl %s" % glsl_hash(sh))
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


_HASHLINE = re.compile(r'^ \* glsl ([0-9a-f]{32})$')
_TOOLLINE = re.compile(r'^ \* transform ([0-9a-f]{16})$')


def committed_hashes():
    """What the committed headers say they were generated from. Read with a
    regex rather than by compiling anything, because `--check` must run in a
    build that has no glslang -- which is every build."""
    out, tool = {}, {}
    for cfile in SOURCES:
        p = OUTDIR / ("%s.spv.h" % cfile)
        if not p.exists():
            return None, None
        key = None
        for line in p.read_text().split("\n"):
            m = _TOOLLINE.match(line)
            if m:
                tool[cfile] = m.group(1)
            if line.startswith("/* tagpu_"):
                key = line[3:].split(" --")[0]
            m = _HASHLINE.match(line)
            if m and key:
                out[key] = m.group(1)
                key = None
    return out, tool


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
        have, tool = committed_hashes()
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
