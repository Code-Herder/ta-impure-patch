#!/bin/bash
# runall3.sh -- the air study's ladder on the air war, each run decoded back (verify=1), one folder each
source "$(dirname "$0")/env.sh"
OUT=${1:-$AIRLIB/../runs/full}
mkdir -p $OUT
B=heading,velocity,position,stamp,counts
run() { mkdir -p $OUT/$1; ln -sf $NPZ $OUT/$1/units.npz; (cd $OUT/$1 && $PY $AIRLIB/run3.py tag=$1 ${@:2} > run.out 2>&1); }
run r0-frame2 ruleset=$B noair=1 &
run r1-exact ruleset=$B,speed,accs,bank,y &
run r2-airv ruleset=$B,speed,accs,bank,y,air_v,air_vy &
run r3-rot ruleset=$B,speed,accs,bank,y,air_v,air_vy,air_rot &
run r4-vzsat ruleset=$B,speed,accs,bank,y,air_v,air_vy,vz_sat &
run r5-cells ruleset=$B,speed,accs,bank,y,air_v,air_vy,foot,sight,m26 &
run r6-all ruleset=$B,speed,accs,bank,y,air_v,air_vy,air_rot,vz_sat,foot,sight,m26 &
run r7-cellsonly ruleset=$B,foot,sight,m26 &
wait
