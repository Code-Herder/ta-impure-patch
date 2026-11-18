"""THE HELD GROUND's judgement at the decision's start (tools/impai/mutate.py): impai_brain_test.c,
check_held_every_decision and check_held_ground."""
M = [
    ("the held ground is judged at the decision's start", "impai_brain.c",
     "    impai_held_judge(d);\n",
     ""),
    ("a decision with no constructor judges", "impai_build.c",
     "    if (d->held_built < 0) return;\n    gw = d->maps.gw;",
     "    if (d->held_built < 0 || !d->count[IMPAI_C_CONS]) return;\n    gw = d->maps.gw;"),
]
