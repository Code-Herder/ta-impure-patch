"""The d69d0a5c review's fixes and the owner's decisions after it (tools/impai/mutate.py)."""
ECON = "impai_econ.c"
MIL = "impai_mil.c"
M = [
    ("air pick by rank", ECON, "            (air_only && !c[k].air))", "            (air_only && !c[k].air && 0))"),
    ("waived pass no wait", ECON, "        air_waits(d, d->plant_tw);\n", "\n"),
    ("first pass no wait", ECON, "        air_waits(d, d->plant_t);\n", "\n"),
    ("no shear", MIL, "SIGHT_BLOCKS_MAX) -\n                      SHEAR_BLOCKS;", "SIGHT_BLOCKS_MAX) -\n                      0;"),
    ("shear 2", MIL, "#define SHEAR_BLOCKS     4", "#define SHEAR_BLOCKS     2"),
    ("eyes unclamped", MIL, "(int32_t)(sight >> 5) < SIGHT_BLOCKS_MAX ? (int32_t)(sight >> 5) : SIGHT_BLOCKS_MAX)", "(int32_t)(sight >> 5) < 99 ? (int32_t)(sight >> 5) : SIGHT_BLOCKS_MAX)"),
    ("leg not tried", MIL, "        if (over) leg_tried(d, q->tx, q->tz);", "        if (over && d->now > 0xFFFFFF00u) leg_tried(d, q->tx, q->tz);"),
    ("dead scout counted", MIL, "        } else if (t == IMPAI_SQ_SCOUT && !squad_manned(d, k)) {", "        } else if (t == IMPAI_SQ_SCOUT && !squad_manned(d, k) && d->now > 0xFFFFFF00u) {"),
]
