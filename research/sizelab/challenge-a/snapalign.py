import sys
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
sizes = []
bl, _ = T.blocks(sys.argv[1], sizes)
prev = None
for (k, f, l, raw, pk) in sizes:
    if k == T.RB_SNAPSHOT:
        print("snapshot", f, l, "after stream block", prev)
    if k == T.RB_STREAM: prev = (f, l)
