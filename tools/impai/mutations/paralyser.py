"""A PARALYSER DEALS NO DAMAGE (tools/impai/mutate.py): impai_brain_test.c, check_paralyser."""
M = [
    ("a paralyser adds damage a second, range and the artillery test", "tasim_roles.c",
     "        if (paralyses(w)) continue;",
     "        if (paralyses(w) && 0) continue;"),
    ("every regular weapon is passed over as a paralyser", "tasim_roles.c",
     "        if (paralyses(w)) continue;",
     "        if (paralyses(w) || 1) continue;"),
    ("a CommandFire paralyser disarms its type", "tasim_roles.c",
     "        if (!regular(w)) continue;",
     "        if (!regular(w) && !paralyses(w)) continue;\n        if (paralyses(w)) { o->dps_ground = 0.0; continue; }"),
    ("a paralyser is not regular", "tasim_roles.c",
     "    return !(w->flags & (TASIM_WF_COMMANDFIRE | TASIM_WF_STOCKPILE | TASIM_WF_INTERCEPTOR));",
     "    return !(w->flags & (TASIM_WF_COMMANDFIRE | TASIM_WF_STOCKPILE | TASIM_WF_INTERCEPTOR | TASIM_WF_PARALYZER));"),
    ("a paralyser is in the ground reach", "impai_maps.c",
     "TASIM_WF_INTERCEPTOR | TASIM_WF_TOAIR |\n                         TASIM_WF_PARALYZER));",
     "TASIM_WF_INTERCEPTOR | TASIM_WF_TOAIR |\n                         (TASIM_WF_PARALYZER & 0)));"),
    ("the predictor takes a paralyser's damage", "impai_fight.c",
     "    if (w->flags & TASIM_WF_PARALYZER) return 0;",
     "    if ((w->flags & TASIM_WF_PARALYZER) && 0) return 0;"),
    ("the predictor zeroes every weapon", "impai_fight.c",
     "    if (w->flags & TASIM_WF_PARALYZER) return 0;",
     "    if ((w->flags & TASIM_WF_PARALYZER) || 1) return 0;"),
    ("the predictor zeroes a paralyser's matched row alone", "impai_fight.c",
     "    if (w->flags & TASIM_WF_PARALYZER) return 0;\n"
     "    while (lo < hi) {\n"
     "        const unsigned mid = lo + (hi - lo) / 2;\n"
     "        if (c->d[mid].type < j) lo = mid + 1;\n"
     "        else hi = mid;\n"
     "    }\n"
     "    return lo < w->d0 + w->nd && c->d[lo].type == j ? c->d[lo].dmg : (int32_t)w->dmg;",
     "    while (lo < hi) {\n"
     "        const unsigned mid = lo + (hi - lo) / 2;\n"
     "        if (c->d[mid].type < j) lo = mid + 1;\n"
     "        else hi = mid;\n"
     "    }\n"
     "    return lo < w->d0 + w->nd && c->d[lo].type == j ? ((w->flags & TASIM_WF_PARALYZER) ? 0 : c->d[lo].dmg)\n"
     "                                                     : (int32_t)w->dmg;"),
    ("the paralyser test stands below the anti-air branch", "tasim_roles.c",
     "        if (paralyses(w)) continue;\n"
     "        if (!a.has_damaging) a.first_regular_aa = tasim_roles_slot_antiair(u, k);\n"
     "        a.has_damaging = 1;\n"
     "        if (tasim_roles_slot_antiair(u, k)) {\n"
     "            o->dps_air += dps((double)w->damage_air, w);\n"
     "            continue;\n"
     "        }\n",
     "        if (!a.has_damaging && !paralyses(w)) a.first_regular_aa = tasim_roles_slot_antiair(u, k);\n"
     "        if (!paralyses(w)) a.has_damaging = 1;\n"
     "        if (tasim_roles_slot_antiair(u, k)) {\n"
     "            o->dps_air += dps((double)w->damage_air, w);\n"
     "            continue;\n"
     "        }\n"
     "        if (paralyses(w)) continue;\n"),
    ("the anti-air slot is the first regular slot's, a paralyser's included", "tasim_roles.c",
     "        a.has_regular = 1;\n"
     "        if (paralyses(w)) continue;\n"
     "        if (!a.has_damaging) a.first_regular_aa = tasim_roles_slot_antiair(u, k);\n",
     "        if (!a.has_regular) a.first_regular_aa = tasim_roles_slot_antiair(u, k);\n"
     "        a.has_regular = 1;\n"
     "        if (paralyses(w)) continue;\n"),
    ("a CommandFire paralyser is a regular one", "tasim_roles.c",
     "        if (!regular(w)) continue;",
     "        if (!regular(w) && !paralyses(w)) continue;"),
]
