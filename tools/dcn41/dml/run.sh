#!/usr/bin/env bash
# run.sh - build Linux's DML2.1 (unmodified) with dml_modes.c on the build host and run it on src/dcn41/dcn41_modes.tsv.
#   tools/dcn41/dml/run.sh [--gpuvm] [out-dir]     (default out-dir: ${N48_CACHE:-~/.cache/navi48-check}/dml-out)
# The Linux tree and the compiler come from tools/check-deps.sh (the commit tools/dcn41 cites). The DML2.1 object list
# is read from Linux's own dc/dml2_0/Makefile (its src/ entries; the dml21_* DC glue is not used), so a newer tree
# brings its own file list.
set -euo pipefail

D="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${D}/../../.." && pwd)"
GPUVM=""
if [ "${1:-}" = "--gpuvm" ]; then
  GPUVM="--gpuvm"
  shift
fi
eval "$("${ROOT}/tools/check-deps.sh")"
OUT="${1:-${N48_CACHE:-${HOME}/.cache/navi48-check}/dml-out}"
mkdir -p "${OUT}"
DISP="${N48_LINUX}/drivers/gpu/drm/amd/display"
DML="${DISP}/dc/dml2_0/dml21"

INC=(-I"${ROOT}/tools/dcn41/linuxshim/include" -I"${DML}/inc" -I"${DML}/inc/bounding_boxes" -I"${DML}/src/inc" -I"${DML}"
     -I"${DML}/src/dml2_core" -I"${DML}/src/dml2_dpmm" -I"${DML}/src/dml2_mcg" -I"${DML}/src/dml2_pmo" -I"${DML}/src/dml2_top"
     -I"${DML}/src/dml2_standalone_libraries" -I"${DML}/src/dml2_utm_soc_bb" -I"${DML}/src/dml2_cga"
     -I"${DISP}/dc/dml2_0" -I"${DISP}/dc" -I"${DISP}/include" -I"${DISP}/dmub/inc" -I"${N48_LINUX}/drivers/gpu/drm/amd/include")
CFLAGS=(-std=gnu11 -O1 -g -w -fno-strict-aliasing -fwrapv -include "${D}/kernel_compat.h")

SRCS=$(sed -n 's|^DML21 [:+]= \(src/[^ ]*\)\.o$|\1.c|p' "${DISP}/dc/dml2_0/Makefile")
OBJS=()
for s in ${SRCS}; do
  o="${OUT}/$(basename "${s}" .c).o"
  ${N48_CC} "${CFLAGS[@]}" "${INC[@]}" -c "${DML}/${s}" -o "${o}"
  OBJS+=("${o}")
done
${N48_CC} "${CFLAGS[@]}" -Wall -Wextra "${INC[@]}" "${D}/dml_modes.c" "${OBJS[@]}" -lm -o "${OUT}/dml_modes"

# name signal pix_clk_100hz h_active h_front h_sync h_total v_active v_front v_sync v_total max_vstartup
awk -F'\t' 'NR > 1 && $1 !~ /^#/ && $1 != "monitor" { printf "%s/%s %s %s %s %s %s %s %s %s %s %s %s\n", $1, $10, $4, $12, $14, $15, $16, $18, $19, $20, $21, $23, $26 }' \
  "${ROOT}/src/dcn41/dcn41_modes.tsv" | "${OUT}/dml_modes" ${GPUVM}
