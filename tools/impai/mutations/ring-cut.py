"""THE CUT RING GROWS (tools/impai/mutate.py): impai_brain_test.c, check_ring_cut and check_def_want."""
BUILD = "impai_build.c"
M = [
    ("a walk the site tests cut never grows the ring", BUILD,
     "    if (cut && (key || !tested || passed)) return 0;",
     "    if (cut) return 0;"),
    ("a walk that tested no site grows the ring", BUILD,
     "    if (cut && (key || !tested || passed)) return 0;",
     "    if (cut && (key || passed)) return 0;"),
    ("a site that passed and was dropped lets the cut ring grow", BUILD,
     "    if (cut && (key || !tested || passed)) return 0;",
     "    if (cut && (key || !tested)) return 0;"),
    ("a keyed walk the site tests cut grows the ring", BUILD,
     "    if (cut && (key || !tested || passed)) return 0;",
     "    if (cut && (!tested || passed)) return 0;"),
    ("a refusal is counted as a site that passed", BUILD,
     "        if (!d->q->ops->site_ok(d->q, type, cx, cz, 0, &alt)) continue;\n        passed++;",
     "        passed++;\n        if (!d->q->ops->site_ok(d->q, type, cx, cz, 0, &alt)) continue;"),
]
