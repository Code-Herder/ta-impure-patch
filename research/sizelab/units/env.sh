# Sourced by runall.sh: the units folder, the Python, the exe and the slot count (5001 at unit limit 500).
UNITS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; export UNITS
export LIB="$UNITS"
export PY="${PY:-python3}"
export TA_PRISTINE="${TA_PRISTINE:-$(git -C "$UNITS" rev-parse --git-common-dir)/../pristine/TotalA.exe.pristine}"
export SIZELAB_SLOTS="${SIZELAB_SLOTS:-5001}"
