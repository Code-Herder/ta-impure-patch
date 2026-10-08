"""codec5.py -- L0: codec4 (the final tier) plus one engine fact for the survivors' velocity, installed
by monkeypatching codec/codec3 (the committed files are unchanged): the guided rule, DISASSEMBLED
0x49BA74..0x49BABE, velocity rebuilt from the record's own heading +0x36, pitch +0x38 and speed +0x3A
(guided.py). The decode order puts d13..d15 (which hold them) before the velocity. Mode 'gl', per row:
the rule on the new record when the old record's velocity followed the rule on the old record,
else the linear prediction. Offered for d7..d9 beside prev and lin; the encoder's per-column choice
decides. Everything else is codec4's."""
import numpy as np
import codec as C
import codec3 as C3
from guidedrule import rule

C.ORDER[:] = [13, 14, 15, 7, 8, 9] + [d for d in range(27) if d not in (7, 8, 9, 13, 14, 15)]

_modes, _pred = C3.modes_for, C3.predict3

def modes_for(col, stamp=True):
    ms = _modes(col, stamp)
    return ms + ["gl"] if col in (7, 8, 9) else ms

def predict3(col, mode, prev, prev2, valid, cur, m):
    if mode != "gl":
        return _pred(col, mode, prev, prev2, valid, cur, m)
    lin = _pred(col, "lin", prev, prev2, valid, cur, m)
    if not len(prev):
        return lin
    held = (rule(prev) == prev[:, 7:10]).all(1)
    return np.where(held, rule(cur)[:, col - 7], lin).astype(np.uint32)

C3.modes_for, C3.predict3 = modes_for, predict3
