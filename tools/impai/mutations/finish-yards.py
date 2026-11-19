"""THE FINISH'S YARDS (tools/impai/mutate.py): impai_brain_test.c, check_finish_yards."""
BRAIN = "impai_brain.c"
MORE = ("        more = d->pos.dry && d->over_m && d->plant_e && sea_busy == (int)d->sea_plants && mil &&\n"
        "               (impai_tg_known(mil, d->now) & ((1u << IMPAI_TG_FLOAT) | (1u << IMPAI_TG_UNDER)));\n")
M = [
    ("the finish never wants a yard past its first", BRAIN,
     "(!d->sea_plants || more) && d->count[IMPAI_C_FACTORY] >= 1",
     "(!d->sea_plants || (more && 0)) && d->count[IMPAI_C_FACTORY] >= 1"),
    ("a further yard beside an idle or unfinished one", BRAIN,
     MORE, MORE.replace("sea_busy == (int)d->sea_plants", "sea_busy >= 1")),
    ("a further yard without the overflow", BRAIN,
     MORE, MORE.replace("d->pos.dry && d->over_m && ", "d->pos.dry && ")),
    ("a further yard without its energy", BRAIN,
     MORE, MORE.replace("d->over_m && d->plant_e && ", "d->over_m && ")),
    ("a further yard outside the dry hunt", BRAIN,
     MORE, MORE.replace("d->pos.dry && d->over_m && ", "d->over_m && ")),
    ("a further yard with no target on or under the water known", BRAIN,
     MORE, MORE.replace(" && mil &&\n               (impai_tg_known(mil, d->now) & ((1u << IMPAI_TG_FLOAT) | (1u << IMPAI_TG_UNDER)))", " && mil")),
    ("a target on dry ground counts as one on the water", BRAIN,
     MORE, MORE.replace("(1u << IMPAI_TG_FLOAT) | (1u << IMPAI_TG_UNDER)", "(1u << IMPAI_TG_DRY) | (1u << IMPAI_TG_FLOAT) | (1u << IMPAI_TG_UNDER)")),
]
