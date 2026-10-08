#!/bin/bash
# runall3b.sh -- the final candidates on the air war, each decoded back (verify=1)
source "$(dirname "$0")/env.sh"
OUT=${1:-$AIRLIB/../runs/full}
mkdir -p $OUT
B=heading,velocity,position,stamp,counts
X=speed,accs,bank,y
run() { mkdir -p $OUT/$1; ln -sf $NPZ $OUT/$1/units.npz; (cd $OUT/$1 && $PY $AIRLIB/run3.py tag=$1 ${@:2} > run.out 2>&1); }
run f1-best ruleset=$B,$X,air_v,air_vy,vz_sat,air_rot2,foot,sight,m26 &
run f1b-best ruleset=$B,$X,air_v,air_vy,vz_sat,air_rot2,foot,sight,sight_edge,m26 &
run f2-norot2 ruleset=$B,$X,air_v,air_vy,vz_sat,foot,sight,m26 &
run f3-nosight ruleset=$B,$X,air_v,air_vy,vz_sat,air_rot2,foot,m26 &
run f4-exactonly ruleset=$B,$X,foot,sight,m26 &
run f5-cellsonly ruleset=$B,foot,sight,m26 &
run f6-nocells ruleset=$B,$X,air_v,air_vy,vz_sat,air_rot2 &
wait
