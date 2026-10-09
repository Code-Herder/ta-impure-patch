#!/bin/bash
# run1.sh <libdir> <npz> <outdir> <args...> -- one codec run, decoded back, from a copied lib
LIB=$1; NPZ=$2; OUT=$3; shift 3
S=$SIZELAB
export PYTHONPATH="$LIB:$S/l0/units/lib:<repo>/research/sizelab/units"
export SIZELAB_SLOTS=5001
export TA_PRISTINE="$(git -C <repo> rev-parse --git-common-dir)/../pristine/TotalA.exe.pristine"
mkdir -p $OUT; ln -sf $NPZ $OUT/units.npz
cd $OUT && <main checkout>/.venv-undither/bin/python $LIB/run3.py "$@" > run.out 2>&1
