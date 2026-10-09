import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
s = s.replace("""    first = True
    for kind, f, l, payload in bl:""", """    ticks = 0
    for kind, f, l, payload in bl:""")
s = s.replace("""        for rk, b in T.records(payload):
            if rk in (T.RK_EV_FEATURE, T.RK_EV_DEATH):""", """        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                ticks += 1
            first = ticks <= 1
            if rk in (T.RK_EV_FEATURE, T.RK_EV_DEATH):""")
s = s.replace("""        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                break
        first = False
""", "")
p.write_text(s)
