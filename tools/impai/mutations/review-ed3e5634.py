"""The ed3e5634 review's fixes and the owner's "reserve room early" after it (tools/impai/mutate.py)."""
ECON = "impai_econ.c"
BUILD = "impai_build.c"
MIL = "impai_mil.c"
M = [
    # MED-1: the clearance's undecided answer never decides an arrival or stops a unit
    ("the clearance's undecided read as no clear point", MIL,
     "        if ((p = point_for(d, u->type, u->x, u->z, &px, &pz)) <= 0) return p;",
     "        if ((p = point_for(d, u->type, u->x, u->z, &px, &pz)) <= 0) return 0;"),
    ("the arrival reads an undecided clearance as arrived", MIL,
     "        if (p > 0 && nc) p = com_clear(d, u, com, nc, &px, &pz);",
     "        if (p > 0 && nc && com_clear(d, u, com, nc, &px, &pz) <= 0) p = 0;"),
    ("travel moves a member its clearance left undecided", MIL,
     "(c = com_clear(d, u, com, nc, &x, &z)) < 0) {",
     "(c = com_clear(d, u, com, nc, &x, &z)) < -1) {"),
    # LOW-4: a strait narrower than the reach has no clear point, and the arrival counts the member arrived
    ("no clear point read as undecided", MIL,
     "        break;\n    }\n    return !com_near(d, u, com, nc, *x, *z);",
     "        break;\n    }\n    return com_near(d, u, com, nc, *x, *z) ? -1 : 1;"),
    # LOW-3: a builder that reaches no site spends none of the decision's site tests
    ("the reach refusal spends a site test", BUILD,
     "            if (++d->air_reach >= AIR_REACH_REFUSALS) return 0;",
     "            if (++d->air_reach >= AIR_REACH_REFUSALS || ++*tests >= cap) return 0;"),
]
