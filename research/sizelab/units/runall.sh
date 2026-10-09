#!/bin/bash
# runall.sh <rec dir> -- today's baseline, then frame2's ladder, each decoded back (verify=1)
source "$(dirname "$0")/env.sh"
cd "$1"
$PY $LIB/runu.py run.py baseline > base.out 2>&1 &
$PY $LIB/runu.py run2.py rules=0 > f2-rules0.out 2>&1 &
$PY $LIB/runu.py run2.py rules=1 > f2-rules1.out 2>&1 &
$PY $LIB/runu.py run2.py ruleset=stamp,counts > f2-sc.out 2>&1 &
$PY $LIB/runu.py run2.py ruleset=stamp,counts,heading > f2-sch.out 2>&1 &
$PY $LIB/runu.py run2.py ruleset=stamp,counts,heading,velocity > f2-schv.out 2>&1 &
wait
