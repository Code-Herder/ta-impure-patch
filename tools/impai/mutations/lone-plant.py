"""THE LONE COMMANDER'S PLANT and A TYPE PASSED OVER GIVES THE RING BACK (tools/impai/mutate.py): impai_brain_test.c,
check_plantwalk and check_ring_reach."""
BUILD = "impai_build.c"
M = [
    ("a lone commander's plant ring is left for its reach", BUILD,
     "    if (lone_plant) beyond = 0;",
     "    if (lone_plant && 0) beyond = 0;"),
    ("a constructor begun leaves the commander lone", BUILD,
     "    const int lone_plant = commander && t->cat == IMPAI_C_FACTORY && !at && out < 0 && !d->count[IMPAI_C_CONS] &&",
     "    const int lone_plant = commander && t->cat == IMPAI_C_FACTORY && !at && out < 0 && !(d->count[IMPAI_C_CONS] && 0) &&"),
    ("a factory standing leaves the commander lone", BUILD,
     "                           !d->count[IMPAI_C_FACTORY];",
     "                           !(d->count[IMPAI_C_FACTORY] && 0);"),
    ("a lone commander's energy ring grows past its reach", BUILD,
     "    const int lone_plant = commander && t->cat == IMPAI_C_FACTORY && !at && out < 0 && !d->count[IMPAI_C_CONS] &&",
     "    const int lone_plant = commander && (t->cat == IMPAI_C_FACTORY || 1) && !at && out < 0 && !d->count[IMPAI_C_CONS] &&"),
    ("a type passed over leaves the ring at its widest", BUILD,
     "        plant_pass_over(d, type);\n        st->ring_grow = 0;",
     "        plant_pass_over(d, type);"),
]
