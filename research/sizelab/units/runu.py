"""runu.py <script in research/sizelab/units> args... -- run a committed units script unchanged,
with L0/units/lib (the SLOTS-parameterised sim.py) ahead of the units folder on sys.path."""
import os, runpy, sys
LIB = os.path.dirname(os.path.abspath(__file__))
U = os.environ.get("UNITS", LIB)
sys.path[:0] = [LIB, U]
script = sys.argv[1]
sys.argv = sys.argv[1:]
runpy.run_path(os.path.join(U, script) if not os.path.isabs(script) else script, run_name="__main__")
