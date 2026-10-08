#!/bin/bash
# usage: run.sh <name>   -- runs the discrete lab copy on $L0/<name>.imprec into $L0/discrete/<name>/
set -u
L0=$SIZELAB/l0
PY=<main checkout>/.venv-undither/bin/python
n=$1
export REC_FILE=$L0/$n.imprec REC_OUT=$L0/discrete/$n
mkdir -p $REC_OUT
cd $L0/discrete/lab
for s in kinds units_pass cells cells_codec cells_predict orders_fields orders_codec orders_codec2 events events_snapreset small_codec globals predict snapshots snapshots2 combined; do
  echo "== $s $(date +%T)"
  /usr/bin/time -f "%e s, %M KB maxrss" $PY $s.py > $REC_OUT/$s.txt 2>&1
  echo "rc $? $(tail -1 $REC_OUT/$s.txt)"
done
echo done
