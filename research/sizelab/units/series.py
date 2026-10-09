"""series.py -- per-field time series (tick x slot) of the hot unit fields, into series/*.npy."""
import os, numpy as np, sim
FIELDS = {  # name: (byte offset in the image, size, signed, dtype)
    "x": (0x6A, 4, True, np.int32), "y": (0x6E, 4, True, np.int32), "z": (0x72, 4, True, np.int32),
    "pitch": (0x64, 2, False, np.uint16), "heading": (0x66, 2, False, np.uint16),
    "roll": (0x68, 2, False, np.uint16),
    "vx": (0x120, 4, True, np.int32), "vy": (0x124, 4, True, np.int32), "vz": (0x128, 4, True, np.int32),
    "speed": (0x138, 4, True, np.int32), "turnrate": (0x13C, 2, True, np.int16),
    "m26": (0x13E, 2, False, np.uint16), "stamp": (0x142, 4, True, np.int32),
    "m28": (0x140, 2, False, np.uint16),
    "reload": (0x18, 2, False, np.uint16), "aimh": (0x1A, 2, False, np.uint16),
    "aimp": (0x1C, 2, False, np.uint16), "fa": (0xFA, 1, False, np.uint8),
    "fpx": (0x76, 2, True, np.int16), "fpz": (0x78, 2, True, np.int16),
    "layer": (0x110, 1, False, np.uint8), "mlayer": (0x146, 1, False, np.uint8),
    "tidx": (0xA6, 2, False, np.uint16), "mover": (0x00, 4, False, np.uint32),
}
z = sim.load("units.npz")
NT = int((z["ops"][:, 0] == 0).sum())
os.makedirs("series", exist_ok=True)
arr = {k: np.lib.format.open_memmap(f"series/{k}.npy", "w+", v[3], (NT, sim.SLOTS)) for k, v in FIELDS.items()}
live = np.lib.format.open_memmap("series/live.npy", "w+", bool, (NT, sim.SLOTS))
changed = np.lib.format.open_memmap("series/changed.npy", "w+", bool, (NT, sim.SLOTS))
gts = []
for i, tk in enumerate(sim.ticks(z)):
    gts.append(tk.gametime)
    for k, (off, size, sg, dt) in FIELDS.items():
        arr[k][i] = sim.field(tk.S, off, size, sg).astype(dt)
    live[i] = tk.live
    changed[i, tk.dslots] = True
np.save("series/gametime.npy", np.array(gts, np.int32))
for a in list(arr.values()) + [live, changed]:
    a.flush()
print("done", NT)
