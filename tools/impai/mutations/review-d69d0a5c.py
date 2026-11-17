"""The d69d0a5c review's fixes and the owner's decisions after it (tools/impai/mutate.py)."""
ECON = "impai_econ.c"
MIL = "impai_mil.c"
M = [
    ("air pick by rank", ECON, "            (air_only && !c[k].air))", "            (air_only && !c[k].air && 0))"),
    ("waived pass no wait", ECON, "        air_waits(d, d->plant_tw);\n", "\n"),
    ("first pass no wait", ECON, "        air_waits(d, d->plant_t);\n", "\n"),
]
