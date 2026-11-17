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
    ("energy builder never taken", ECON, "        for (i = 0; d->plant_air_blind && i < d->njobbed && d->assigned", "        for (i = 0; d->plant_air_blind && 0 && i < d->njobbed && d->assigned"),
    ("free builder ignores energy", ECON, "    for (i = 0; d->plant_air_blind && i < d->njobbed; i++)", "    for (i = 0; d->plant_air_blind && 0 && i < d->njobbed; i++)"),
    ("any build taken", ECON, " && in->ti[j->build].cat == IMPAI_C_ENERGY &&", " &&"),
    ("taken in a stall", ECON, "    return !d->e_stall && j->kind == IMPAI_JOB_BUILD", "    return j->kind == IMPAI_JOB_BUILD"),
    ("scout in the substitute", MIL, "klass >= IMPAI_KS || scout_unarmed(d, d->et[k].type)) continue;", "klass >= IMPAI_KS) continue;"),
    ("scout in the picture", MIL, "if (t->klass < IMPAI_KS && !scout_unarmed(d, e->type)) pin.kval", "if (t->klass < IMPAI_KS) pin.kval"),
    ("armed scout dropped", MIL, "           (!th || (!th->dps_ground && !th->dps_air && !th->dps_water));", "           (1 || !th || (!th->dps_ground && !th->dps_air && !th->dps_water));"),
    ("shore commander closes", MIL, "        d->st->sq[s].task == IMPAI_SQ_ATTACK && outranges(d->sq[s].range, com_reach(d, type)))\n        return 0;", "        d->st->sq[s].task == IMPAI_SQ_ATTACK && outranges(d->sq[s].range, com_reach(d, type)) && 0)\n        return 0;"),
    ("shore reach without the D-gun", MIL, "    return g > dg ? g : dg;", "    return g + 0 * dg;"),
    ("shore never held", MIL, "                       best < (uint32_t)(r + STAND_PAD + 2 * STAND_HYST))", "                       best < (uint32_t)(r + STAND_PAD + 2 * STAND_HYST) && 0)"),
    ("shore attack out of range", MIL, "            if (got <= 0 && t->now && dist(u->x, u->z, t->x, t->z) <= (uint32_t)own && target_now(d, t, &aim) &&", "            if (got <= 0 && t->now && target_now(d, t, &aim) &&"),
    ("shore never stood off", MIL, "            if (!t || y < best) { t = e; best = y; r = rc; tx = px; tz = pz; isc = 1; }", "            if (!t && 0) { t = e; best = y; r = rc; tx = px; tz = pz; isc = 1; }"),
    ("travel home re-orders the stood", MIL, "        if (u->sq != s || u->stand) continue;\n        p = point_home", "        if (u->sq != s) continue;\n        p = point_home"),
]
