"""THE WIDEST RING STARTS OVER (tools/impai/mutate.py): impai_brain_test.c, check_ring_widest."""
BUILD = "impai_build.c"
M = [
    ("the widest ring keeps the shared growth", BUILD,
     "    *grown = 0;     /* THE WIDEST RING STARTS OVER */",
     "    if (plant) *grown = 0;     /* THE WIDEST RING STARTS OVER */"),
    ("the widest ring starts over at ring 1", BUILD,
     "    *grown = 0;     /* THE WIDEST RING STARTS OVER */",
     "    *grown = !plant;     /* THE WIDEST RING STARTS OVER */"),
    ("the ring below the widest starts over", BUILD,
     "    w->exit = *grown < IMPAI_RING_WIDEST ? IMPAI_PW_GREW : IMPAI_PW_WIDEST;\n    if (*grown < IMPAI_RING_WIDEST) {",
     "    w->exit = *grown < IMPAI_RING_WIDEST ? IMPAI_PW_GREW : IMPAI_PW_WIDEST;\n    if (*grown < IMPAI_RING_WIDEST - !plant) {"),
    ("a far record's sites leave the widest ring", BUILD,
     "    if (at || out >= 0 || beyond || threat_skipped) {",
     "    if (at || out >= 0 || beyond || threat_skipped || (!plant && *grown == IMPAI_RING_WIDEST && w->far_rec)) {"),
    ("a cut walk keeps the widest ring", BUILD,
     "    if (cut && (key || !tested || passed)) return 0;",
     "    if (cut && (key || !tested || passed || (!plant && *grown == IMPAI_RING_WIDEST))) return 0;"),
]
