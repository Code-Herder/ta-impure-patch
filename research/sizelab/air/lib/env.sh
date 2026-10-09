# the air study's environment: committed units scripts, L0's SLOTS-parameterised sim.py, this lib first
export UNITS=<repo>/research/sizelab/units
export L0LIB=$SIZELAB/l0/units/lib
export AIRLIB=$SIZELAB/air/lib
export PY=<main checkout>/.venv-undither/bin/python
export TA_PRISTINE="$(git -C <repo> rev-parse --git-common-dir)/../pristine/TotalA.exe.pristine"
export SIZELAB_SLOTS=5001
export PYTHONPATH="$AIRLIB:$L0LIB:$UNITS"
export NPZ=$SIZELAB/l0/units/air/units.npz
