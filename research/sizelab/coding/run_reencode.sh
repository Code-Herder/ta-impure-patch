#!/bin/bash
C="$(cd "$(dirname "$0")" && pwd)"
cd $C
P="${PY:-python3}"
for v in 0,0,0,1 1,0,0,1 2,0,0,1 3,0,0,1 3,1,0,1 3,2,0,1 3,3,0,1 3,4,0,1 3,4,1,1 3,4,2,1 3,4,1,2 3,4,1,3 3,4,1,0 3,4,2,2; do
  $P reencode.py battle $v > out_reencode_$v.txt 2>&1 &
done
wait
