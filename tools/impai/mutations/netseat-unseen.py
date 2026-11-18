"""THE RING'S UNSEEN SITES and THE RING'S LOOK (tools/impai/mutate.py): impai_brain_test.c, check_ring_unseen."""
ECON = "impai_econ.c"
BUILD = "impai_build.c"
M = [
    ("the ring grows past unseen ground", BUILD,
     "    if (at || out >= 0 || beyond || threat_skipped || unseen) return 0;",
     "    if (at || out >= 0 || beyond || threat_skipped || (unseen && 0)) return 0;"),
    ("an unseen site is given to the site test", BUILD,
     "            !d->q->ops->explored(d->q, cx + fx / 2, cz + fz / 2)) {\n            int32_t px, pz;",
     "            0 && !d->q->ops->explored(d->q, cx + fx / 2, cz + fz / 2)) {\n            int32_t px, pz;"),
    ("a site no builder walks to see is kept unseen", BUILD,
     "            if (!unseen && !look_point(d, b, cx + fx / 2, cz + fz / 2, &px, &pz)) continue;",
     "            if (!unseen && !look_point(d, b, cx + fx / 2, cz + fz / 2, &px, &pz) && 0) continue;"),
    ("the unseen site is kept for no builder", BUILD,
     "            if (!unseen++ && !at && out < 0 && d->unseen_slot != b->slot) {",
     "            if (!unseen++ && !at && out < 0 && d->unseen_slot != b->slot && 0) {"),
    ("the look rung gives no order", ECON,
     "static int rung_look(IMPAI_DEC* d, IMPAI_ASK* a) { return impai_build_look(d, a->b); }",
     "static int rung_look(IMPAI_DEC* d, IMPAI_ASK* a) { return 0 && impai_build_look(d, a->b); }"),
    ("the look's job keeps another cell", BUILD,
     "    if (order_mode(d, b, 2, (int)WQ_NONE, pos, IMPAI_JOB_EXPLORE, cx, cz)) return 1;",
     "    if (order_mode(d, b, 2, (int)WQ_NONE, pos, IMPAI_JOB_EXPLORE, cx - 32, cz)) return 1;"),
    ("an idle look waits for its timeout", "impai_brain.c",
     "            else if (d->jobbed_idle[i] || age > in->plan->job_timeout) {",
     "            else if ((d->jobbed_idle[i] && 0) || age > in->plan->job_timeout) {"),
]
