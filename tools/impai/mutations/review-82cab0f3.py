"""The 82cab0f3 review's fixes and the owner's answers after it (tools/impai/mutate.py)."""
ECON = "impai_econ.c"
BUILD = "impai_build.c"
MIL = "impai_mil.c"
MAPS = "impai_maps.c"
M = [
    # MED-1: every builder that can take the air plant is asked, and the ladder never orders it
    ("idle builder not asked", ECON,
     "                !impai_econ_room(d) || !air_place(d, &d->idle[i], t))",
     "                !impai_econ_room(d) || 1)"),
    ("energy builder not asked", ECON,
     "        if (energy_leaver(d, i - d->nidle - d->nhelpers) && air_place(",
     "        if (0 && energy_leaver(d, i - d->nidle - d->nhelpers) && air_place("),
    ("the ladder keeps the air plant", ECON,
     "            if (d->plant_air_blind) air_let_go(d);",
     "            if (d->plant_air_blind && 0) air_let_go(d);"),
    # the owner's answer 1: the air plant's own search outside the base ring, on its own site tests
    ("no ring outside the base ring", BUILD,
     "    for (g = g0; !ok && g <= g0 + AIR_OUT_RINGS &&",
     "    for (g = g0; !ok && g <= g0 &&"),
    ("the search spends the decision's site tests", BUILD,
     "    int* const tests = out >= 0 ? &d->air_sites : &d->sites;\n"
     "    const int cap = out >= 0 ? AIR_OUT_TESTS : (int)in->plan->site_tests;",
     "    int* const tests = out >= 0 ? &d->sites : &d->sites;\n"
     "    const int cap = out >= 0 ? (int)in->plan->site_tests : (int)in->plan->site_tests;"),
    ("an outer site the builder cannot reach", BUILD,
     "        if (out >= 0 && !builder_reaches(d, b, type, cx, cz)) continue;",
     "        if (out >= 0 && !builder_reaches(d, b, type, cx, cz) && 0) continue;"),
    # MED-2: the hold dropped; travel, the fight's moves and the arrival cleared of the commander's reach
    ("travel not cleared", MIL,
     "    if (!nc) return move_to(d, u->slot, u->type, u->x, u->z, x, z);",
     "    if (1) return move_to(d, u->slot, u->type, u->x, u->z, x, z);"),
    ("the arrival not cleared", MIL,
     "        if (p > 0 && nc && !com_clear(d, u, com, nc, &px, &pz)) p = 0;",
     "        if (0 && p > 0 && nc && !com_clear(d, u, com, nc, &px, &pz)) p = 0;"),
    ("the clearance not moved out", MIL,
     "        *x = px;\n        *z = pz;\n        break;\n    }\n    return !com_near(d, u, com, nc, *x, *z);",
     "        *x = px + 0 * *x;\n        *z = pz + 0 * *z;\n        if (d->now) return 0;\n        break;\n    }\n"
     "    return !com_near(d, u, com, nc, *x, *z);"),
    ("the fight closes on the commander", MIL,
     "    if (!nc || own <= 0 || y <= (uint32_t)own) return 1;",
     "    if (1 || !nc || own <= 0 || y <= (uint32_t)own) return 1;"),
    # MED-3: contact_closes per member, and the contact's other members
    ("the squad's longest range stands for every member", MIL,
     "        if (!outranges(ground_reach(d, u->type), com_reach(d, type)) || !hurts_type(d, u->type, type)) return 0;",
     "        if (!outranges(d->sq[s].range, com_reach(d, type))) return 0;"),
    ("the contact's other members not read", MIL,
     "        if (k != e && member_closes(d, &d->en[k], s, &other)) return 1;",
     "        if (k != e && member_closes(d, &d->en[k], s, &other) && 0) return 1;"),
    # LOW-1: the commander's point is the Euclidean nearest block
    ("the first ring hit", MAPS,
     "    for (ring = 0; ring <= radius && (best < 0 || (int64_t)ring * ring < best); ring++)",
     "    for (ring = 0; ring <= radius && best < 0; ring++)"),
]
