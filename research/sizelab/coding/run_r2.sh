#!/bin/bash
C="$(cd "$(dirname "$0")" && pwd)"
cd $C
P="${PY:-python3}"
i=0
while read -r args; do
  i=$((i+1))
  $P reencode2.py battle $args > out_r2_$i.txt 2>&1 &
done <<'LIST'
0,0,0,1 0 --more --nocheck
3,1,0,1 0
3,4,1,2 0 --more --dump
3,1,1,2 0
3,4,1,2 1 --persist
3,4,1,2 30 --persist
3,1,1,2 30
3,4,1,2 300 --persist --dump
3,4,1,2 900 --persist
3,1,0,1 1
3,4,1,3 30 --persist
LIST
wait
