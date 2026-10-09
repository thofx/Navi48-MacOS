#!/usr/bin/env bash
# mutants.sh - planted breaks for the dptrain harness: build it (run.sh), then mutants.py plants one mistake at a time in a
# copy of src/dcn41/dcn41_dp_train.c and requires the harness to fail on each.
#   tools/dcn41/dptrain/mutants.sh [out-dir]
set -euo pipefail

D="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${D}/../../.." && pwd)"
eval "$("${ROOT}/tools/check-deps.sh")"
OUT="${1:-${N48_CACHE:-${HOME}/.cache/navi48-check}/dptrain-out}"
mkdir -p "${OUT}"
"${D}/run.sh" "${OUT}" > "${OUT}/baseline.log"
python3 -I "${D}/mutants.py" "${ROOT}" "${OUT}" ${N48_CC}
