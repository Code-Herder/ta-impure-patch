"""A UNIT SEEN GONE (tools/impai/mutate.py): impai_brain_test.c, check_seen_gone."""
KNOW = "impai_know.c"
M = [
    ("a unit that dies in sight stays a ghost", KNOW,
     "    } else if (by_sight && !seen) {",
     "    } else if (by_sight && !seen && 0) {"),
    ("a live unit that leaves sight leaves no row", KNOW,
     "    } else if (by_sight && !seen) {",
     "    } else if (by_sight && (!seen || 1)) {"),
    ("a unit that dies out of sight leaves no row", KNOW,
     "    const int by_sight = (n.how & IMPAI_LOS) != 0;",
     "    const int by_sight = n.how != 0;"),
    ("a contact's answer is taken as sight", KNOW,
     "    if (seen && seen != WQ_SEEN_CONTACT) {",
     "    if (seen && (seen != WQ_SEEN_CONTACT || 1)) {"),
]
