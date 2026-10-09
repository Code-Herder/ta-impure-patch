"""Shared loader: the blocks extracted by extract.py."""
import pickle, struct, os
HERE = os.path.dirname(os.path.abspath(__file__))
RB_HEADER, RB_STREAM, RB_SNAPSHOT, RB_END, RB_MINIMAP = 1, 2, 3, 4, 5

def load(name="battle"):
    idx = pickle.load(open(os.path.join(HERE, name + ".idx"), "rb"))
    data = open(os.path.join(HERE, name + ".bin"), "rb").read()
    out = []
    for kind, first, last, raw, packed, off in idx:
        out.append((kind, first, last, data[off:off + raw], packed))
    return out

def records(payload):
    at = 0
    n = len(payload)
    while at + 4 <= n:
        kind, ln = struct.unpack_from("<HH", payload, at)
        yield kind, payload[at + 4:at + 4 + ln]
        at += (4 + ln + 3) & ~3

def iterations(payload):
    """Split a stream payload into its iterations: list of (tick, bytes) at RK_TICK boundaries."""
    at = 0; n = len(payload); starts = []
    while at + 4 <= n:
        kind, ln = struct.unpack_from("<HH", payload, at)
        if kind == 1:
            starts.append(at)
        at += (4 + ln + 3) & ~3
    out = []
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else n
        tick = struct.unpack_from("<I", payload, s + 4)[0]
        out.append((tick, payload[s:e]))
    assert not starts or starts[0] == 0
    return out
