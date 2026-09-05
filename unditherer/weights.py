"""The GLSL restorer's weight file: the model's convolutions, BatchNorm already
folded, laid out exactly as the fragment shaders index them, so the shader does
no reshaping at all.

    python -m unditherer export-weights --model full      # -> models/full.w32.bin

THE SHADER'S VIEW.  Activations are stored four channels per texel ("channel
tiles": tile j holds channels 4j..4j+3), and one conv pass computes output tile
k from the 9 taps x Jin input tiles of the previous layer.  For every (k, tap,
j) the shader wants a 4x4 matrix M with M[o][i] = W[4k+o][4j+i][tap], applied
as `acc += M * a` where `a` is the input tile's texel -- in GLSL a mat4 is four
COLUMN vectors, so column c is the weights of input channel 4j+c across the
four output channels.  A layer's k-block is therefore:

    mat4 0            bias in column 0 (the other three columns are 0)
    mat4 1 + t*Jin + j    the 4x4 for tap t (t = ky*3 + kx, ONNX order:
                          offset (kx-1, ky-1)) and input tile j

padded to a multiple of 256 bytes, because the blocks are bound one (or a few)
at a time as a std140 uniform-block range and 256 is the largest
UNIFORM_BUFFER_OFFSET_ALIGNMENT any driver reports.  Channels that do not
exist (the 4th input of layer 0, the 4th output of the last layer) are zero
weights, so the shader never special-cases them.

THE FILE.  Little-endian:

    "TAW1"  u32 depth  u32 ch  u32 ntex          ntex = body length in vec4s
    depth x { u32 offset, u32 jin, u32 kout, u32 kstride }   per layer, in vec4s
    ntex x 4 x f32                                the body

`offset` is the layer's first vec4 from the start of the body, `kstride` the
vec4s per k-block (4 x mat4 count, padded).  Layer l's k-block k starts at
offset + k * kstride.  Everything a consumer needs is in the header; the only
formula it must know is the mat4 index above.

`run_reference` below runs the model FROM THE PACKED BLOCKS, tap by tap,
exactly as the shader will, so a layout mistake shows up here against
onnxruntime before any shader exists.
"""
import struct
from pathlib import Path

import numpy as np

MAGIC = b"TAW1"
ALIGN_BYTES = 256
TEXEL_BYTES = 16                              # one vec4 of f32


def layer_geometry(depth, ch, layer):
    """(cin, cout, jin, kout, kstride_texels) for one layer of a depth x ch model."""
    cin = 3 if layer == 0 else ch
    cout = 3 if layer == depth - 1 else ch
    jin, kout = (cin + 3) // 4, (cout + 3) // 4
    mats = 1 + 9 * jin                          # bias + one mat4 per (tap, j)
    nbytes = mats * 4 * TEXEL_BYTES
    nbytes = -(-nbytes // ALIGN_BYTES) * ALIGN_BYTES
    return cin, cout, jin, kout, nbytes // TEXEL_BYTES


def read_onnx_layers(path):
    """[(W [cout, cin, 3, 3], b [cout])] in graph order, from the ONNX initializers.
    The export folded BatchNorm, so the graph is Conv/Relu/Sub only."""
    import onnx
    from onnx import numpy_helper
    m = onnx.load(str(path))
    inits = {t.name: numpy_helper.to_array(t) for t in m.graph.initializer}
    layers = []
    for n in m.graph.node:
        if n.op_type != "Conv":
            continue
        pads = next((list(a.ints) for a in n.attribute if a.name == "pads"), None)
        if pads != [1, 1, 1, 1]:
            raise ValueError(f"{n.name}: pads {pads}, expected [1, 1, 1, 1]")
        w, b = inits[n.input[1]].astype(np.float32), inits[n.input[2]].astype(np.float32)
        if w.shape[2:] != (3, 3):
            raise ValueError(f"{n.name}: kernel {w.shape[2:]}, expected 3x3")
        layers.append((w, b))
    return layers


def read_pt_layers(path):
    """The same list from a checkpoint, folding each BatchNorm into the conv
    before it -- the independent derivation `export-weights` asserts against."""
    import torch
    ck = torch.load(path, map_location="cpu", weights_only=False)
    sd = ck["model"]
    layers, i = [], 0
    n = max(int(k.split(".")[1]) for k in sd if k.startswith("net.")) + 1
    while i < n:
        w = sd.get(f"net.{i}.weight")
        if w is None or w.dim() != 4:              # a BatchNorm's gamma is 1-D
            i += 1
            continue
        w = w.detach().numpy().astype(np.float64)
        b = sd.get(f"net.{i}.bias")
        b = np.zeros(w.shape[0]) if b is None else b.detach().numpy().astype(np.float64)
        if f"net.{i + 1}.running_mean" in sd:      # conv, then its BatchNorm
            g = sd[f"net.{i + 1}.weight"].numpy().astype(np.float64)
            beta = sd[f"net.{i + 1}.bias"].numpy().astype(np.float64)
            mean = sd[f"net.{i + 1}.running_mean"].numpy().astype(np.float64)
            var = sd[f"net.{i + 1}.running_var"].numpy().astype(np.float64)
            s = g / np.sqrt(var + 1e-5)
            w = w * s[:, None, None, None]
            b = (b - mean) * s + beta
        layers.append((w.astype(np.float32), b.astype(np.float32)))
        i += 1
    return layers


def pack(layers):
    """[(W, b)] -> (header bytes, body float32 [ntex, 4], meta dict)."""
    depth = len(layers)
    ch = layers[0][0].shape[0]
    blocks, table, off = [], [], 0
    for l, (w, b) in enumerate(layers):
        cin, cout, jin, kout, kstride = layer_geometry(depth, ch, l)
        if w.shape != (cout, cin, 3, 3) or b.shape != (cout,):
            raise ValueError(f"layer {l}: W {w.shape} b {b.shape}, expected {(cout, cin, 3, 3)}")
        wp = np.zeros((4 * kout, 4 * jin, 3, 3), np.float32)
        wp[:cout, :cin] = w
        bp = np.zeros(4 * kout, np.float32)
        bp[:cout] = b
        for k in range(kout):
            blk = np.zeros((kstride, 4), np.float32)
            blk[0] = bp[4 * k:4 * k + 4]
            for t in range(9):
                ky, kx = divmod(t, 3)
                for j in range(jin):
                    m = 1 + t * jin + j
                    for c in range(4):          # column c = input channel 4j+c
                        blk[4 * m + c] = wp[4 * k:4 * k + 4, 4 * j + c, ky, kx]
            blocks.append(blk)
        table.append((off, jin, kout, kstride))
        off += kout * kstride
    body = np.concatenate(blocks, 0)
    assert body.shape == (off, 4)
    header = MAGIC + struct.pack("<3I", depth, ch, off)
    for row in table:
        header += struct.pack("<4I", *row)
    meta = {"depth": depth, "ch": ch, "texels": int(off), "bytes": len(header) + off * TEXEL_BYTES,
            "layers": [{"offset": o, "jin": j, "kout": k, "kstride": s} for o, j, k, s in table]}
    return header, body, meta


def write(path, layers):
    header, body, meta = pack(layers)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + body.astype("<f4").tobytes())
    meta["file"] = str(path)
    return meta


def read(path):
    """-> (meta, body float32 [ntex, 4]) -- the consumer's view of the file."""
    blob = Path(path).read_bytes()
    if blob[:4] != MAGIC:
        raise ValueError(f"{path}: not a TAW1 weight file")
    depth, ch, ntex = struct.unpack_from("<3I", blob, 4)
    table = [struct.unpack_from("<4I", blob, 16 + 16 * l) for l in range(depth)]
    start = 16 + 16 * depth
    body = np.frombuffer(blob, "<f4", ntex * 4, start).reshape(ntex, 4)
    if len(blob) != start + ntex * TEXEL_BYTES:
        raise ValueError(f"{path}: body is {len(blob) - start} bytes, header says {ntex * TEXEL_BYTES}")
    meta = {"depth": depth, "ch": ch, "texels": ntex,
            "layers": [{"offset": o, "jin": j, "kout": k, "kstride": s} for o, j, k, s in table]}
    return meta, body


def run_reference(meta, body, rgb, wrap, depth_pad=None):
    """The shader's arithmetic in numpy: rgb float32 [h, w, 3] in [0, 1] ->
    the same, through the packed blocks.  `wrap` pads by the receptive radius
    with wrap-around first and crops after (infer.py); zero padding at every
    layer is the rect mask.  Returns float32, unrounded."""
    depth, ch = meta["depth"], meta["ch"]
    pad = (depth_pad if depth_pad is not None else depth) if wrap else 0
    x = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="wrap") if pad else rgb
    h, w = x.shape[:2]
    act = np.zeros((h, w, 4), np.float32)
    act[..., :3] = x
    act = act[None]                                      # [tiles=1, h, w, 4]
    for l, L in enumerate(meta["layers"]):
        jin, kout, ks, off = L["jin"], L["kout"], L["kstride"], L["offset"]
        out = np.zeros((kout, h, w, 4), np.float32)
        for k in range(kout):
            blk = body[off + k * ks: off + (k + 1) * ks]
            acc = np.broadcast_to(blk[0], (h, w, 4)).astype(np.float32).copy()
            for t in range(9):
                ky, kx = divmod(t, 3)
                dy, dx = ky - 1, kx - 1
                # tap (dx, dy): input shifted so that acc[y, x] sees in[y+dy, x+dx]; 0 outside
                for j in range(jin):
                    m = 1 + t * jin + j
                    M = blk[4 * m:4 * m + 4]             # rows = columns c: M[c] = weights of in channel c
                    a = np.zeros((h, w, 4), np.float32)
                    ys, ye = max(0, -dy), min(h, h - dy)
                    xs, xe = max(0, -dx), min(w, w - dx)
                    a[ys:ye, xs:xe] = act[j, ys + dy:ye + dy, xs + dx:xe + dx]
                    acc += a @ M                         # sum_c a[c] * M[c]
            out[k] = np.maximum(acc, 0) if l < depth - 1 else acc
        act = out
    net = act[0][..., :3]
    y = x - net
    return y[pad:pad + rgb.shape[0], pad:pad + rgb.shape[1]] if pad else y
