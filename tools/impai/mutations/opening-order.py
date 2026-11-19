"""THE OPENING'S ENERGY and THE STALL YIELDS TO THE FIRST PLANT (tools/impai/mutate.py): impai_brain_test.c,
check_open_energy, check_stall_yields, check_stall_yield_reclaimer and check_opening_order.

NOT LISTED, an equivalent mutant: stall_yields without `!d->pending[IMPAI_C_FACTORY]`. Every plant want is 0 beside
an ordered factory (impai_brain.c, economy: want_factory, want_sea and want_tech each hold `!pending`), the
opening's energy is not owed beside one and the frame resumed is vetoed in a stall, so a builder passed on is
given no row between the stall's two and the row under the plant asks it the stall's energy in the same
assignment: the orders are the unmutated ones. The term states the bound; no fixture can part on it."""
ECON = "impai_econ.c"
OWED = ("    return !st->opened && !st->open_e_refused && !d->pending[IMPAI_C_FACTORY] && impai_econ_open_mex(d) &&\n"
        "           !impai_econ_open_energy(d, 1);")
YIELDS = ("    return !st->opened && !st->open_stall_over && !d->pending[IMPAI_C_FACTORY] && "
          "impai_econ_open_energy(d, 1);")
M = [
    ("the opening's energy before open_mex extractors are finished", ECON,
     OWED, OWED.replace("impai_econ_open_mex(d) &&", "(impai_econ_open_mex(d) || 1) &&")),
    ("the opening's energy past the first factory", ECON,
     OWED, OWED.replace("!st->opened &&", "(!st->opened || 1) &&", 1)),
    ("the opening's energy beside an ordered plant", ECON,
     OWED, OWED.replace("!d->pending[IMPAI_C_FACTORY] &&", "(!d->pending[IMPAI_C_FACTORY] || 1) &&")),
    ("the opening's energy counts no order", ECON,
     OWED, OWED.replace("!impai_econ_open_energy(d, 1)", "!impai_econ_open_energy(d, 0)")),
    ("the opening's energy never met", ECON,
     OWED, OWED.replace("!impai_econ_open_energy(d, 1)", "(!impai_econ_open_energy(d, 1) || 1)")),
    ("a refused walk leaves the opening's energy owed", ECON,
     "        d->in->st->open_e_refused = 1;",
     "        d->in->st->open_e_refused = 0;"),
    ("the opening's energy asked after its walk was refused", ECON,
     OWED, OWED.replace("!st->open_e_refused &&", "(!st->open_e_refused || 1) &&")),
    ("the stall never yields", ECON,
     "    if (stall_yields(d)) {\n        a->yielded = 1;",
     "    if (stall_yields(d) && 0) {\n        a->yielded = 1;"),
    ("the stall yields past the opening", ECON,
     YIELDS, YIELDS.replace("!st->opened &&", "(!st->opened || 1) &&")),
    ("the stall yields before open_energy energy buildings", ECON,
     YIELDS, YIELDS.replace("impai_econ_open_energy(d, 1);", "(impai_econ_open_energy(d, 1) || 1);")),
    ("the stall yields on the buildings counted alone", ECON,
     YIELDS, YIELDS.replace("impai_econ_open_energy(d, 1);", "impai_econ_open_energy(d, 0);")),
    ("a plant walk that orders nothing leaves the yield", ECON,
     "    if (a->plant_asked) d->in->st->open_stall_over = 1;",
     "    if (a->plant_asked && 0) d->in->st->open_stall_over = 1;"),
    ("the yield ends with no plant asked", ECON,
     "    if (a->plant_asked) d->in->st->open_stall_over = 1;",
     "    if (a->plant_asked || 1) d->in->st->open_stall_over = 1;"),
    ("the yield stands after it ended", ECON,
     YIELDS, YIELDS.replace("!st->open_stall_over &&", "(!st->open_stall_over || 1) &&")),
    ("a builder passed on is asked no energy", ECON,
     "    if (a->plant_asked) d->in->st->open_stall_over = 1;\n    return stall_energy(d, a);",
     "    if (a->plant_asked) d->in->st->open_stall_over = 1;\n    return 0 && stall_energy(d, a);"),
    ("the opening's energy ordered is not counted in the decision's income", ECON,
     "        return 0;\n    }\n    d->pe_in += d->in->ti[t].egain;\n    return 1;",
     "        return 0;\n    }\n    d->pe_in += 0 * d->in->ti[t].egain;\n    return 1;"),
    ("the stall's rung passes a reclaimer on", ECON,
     "        if (a.n > 0 && stall_energy(d, &a)) {",
     "        if (a.n > 0 && rung_stall_energy(d, &a)) {"),
    ("the plant's rung does not say it was asked", ECON,
     "    a->plant_asked = 1;",
     "    a->plant_asked = 0;"),
]
