#!/usr/bin/env bash
# run.sh - build and run the dptrain harness: Linux's DP link training (unmodified) against src/dcn41/dcn41_dp_train.c.
#   tools/dcn41/dptrain/run.sh [-v SCENARIO] [out-dir]     (default out-dir: ${N48_CACHE:-~/.cache/navi48-check}/dptrain-out)
# Linux objects, at the commit tools/check-deps.sh pins: dc/link/protocols/link_dp_training.c, link_dp_training_8b_10b.c,
# link_dp_phy.c, link_dp_capability.c (the fallback policy), link_edp_panel_control.c (the panel-mode step) and
# dc/link/link_validation.c (the link bandwidth). The
# symbols they reference that linux_side.c does not define get a generated stub that aborts with the symbol's name if a
# scenario ever calls it (the scenarios reach none of them).
set -euo pipefail

D="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${D}/../../.." && pwd)"
VERBOSE=()   # expanded as ${VERBOSE[@]+"${VERBOSE[@]}"}: an empty array is "unbound" to macOS's bash 3.2 under set -u
if [ "${1:-}" = "-v" ]; then
  VERBOSE=(-v "${2}")
  shift 2
fi
eval "$("${ROOT}/tools/check-deps.sh")"
OUT="${1:-${N48_CACHE:-${HOME}/.cache/navi48-check}/dptrain-out}"
mkdir -p "${OUT}"
DISP="${N48_LINUX}/drivers/gpu/drm/amd/display"

LINC=(-I"${ROOT}/tools/dcn41/linuxshim/include" -I"${N48_LINUX}/include")
for d in dc/inc dc/inc/hw dc/clk_mgr dc/hwss dc/resource dc/dsc dc/optc dc/dpp dc/hubbub dc/dccg dc/hubp dc/dio dc/dwb dc/hpo \
         dc/mmhubbub dc/mpc dc/opp dc/pg dc/soc_and_ip_translator modules/inc dmub/inc . include dc amdgpu_dm dc/link dc/link/protocols; do
  LINC+=(-I"${DISP}/${d}")
done
LINC+=(-I"${N48_LINUX}/drivers/gpu/drm/amd/include")
LCFLAGS=(-std=gnu11 -O1 -g -w -fno-strict-aliasing -fwrapv -include "${ROOT}/tools/dcn41/dml/kernel_compat.h")
# our side: the dcn41 unit-test warning set (tools/dcn41/build.py WARN)
OCFLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wconversion
         -Wno-sign-conversion)

OBJS=()
for f in protocols/link_dp_training protocols/link_dp_training_8b_10b protocols/link_dp_phy protocols/link_dp_capability \
         protocols/link_edp_panel_control link_validation; do
  o="${OUT}/$(basename "${f}").o"
  ${N48_CC} "${LCFLAGS[@]}" "${LINC[@]}" -c "${DISP}/dc/link/${f}.c" -o "${o}"
  OBJS+=("${o}")
done
${N48_CC} "${LCFLAGS[@]}" "${LINC[@]}" -I"${D}" -c "${D}/linux_side.c" -o "${OUT}/linux_side.o"
${N48_CC} "${OCFLAGS[@]}" -c "${ROOT}/src/dcn41/dcn41_dp_train.c" -o "${OUT}/dcn41_dp_train.o"
for f in sink main; do
  ${N48_CC} -std=c11 -O1 -g -Wall -Wextra -Werror -I"${D}" -I"${ROOT}/src/dcn41" -c "${D}/${f}.c" -o "${OUT}/${f}.o"
done
OBJS+=("${OUT}/linux_side.o" "${OUT}/dcn41_dp_train.o" "${OUT}/sink.o" "${OUT}/main.o")

# the stubs: whatever a first link still misses (that link is expected to fail; sed and sort are not). GNU ld says
# "undefined reference to `sym'", Apple's ld "\"_sym\", referenced from:" (the C symbol with a leading underscore).
LINK_OUT="$(${N48_CC} "${OBJS[@]}" -o "${OUT}/dptrain" 2>&1 || true)"
MISSING="$(printf '%s\n' "${LINK_OUT}" \
  | sed -n -e "s/.*undefined reference to \`\([A-Za-z_][A-Za-z0-9_]*\)'.*/\1/p" -e 's/^ *"_\([A-Za-z_][A-Za-z0-9_]*\)", referenced from:.*/\1/p' \
  | sort -u)"
{
  echo '#include <stdio.h>'
  echo '#include <stdlib.h>'
  echo 'static void stub(const char *n) { fprintf(stderr, "dptrain: Linux called %s, which no scenario should reach\n", n); abort(); }'
  for s in ${MISSING}; do
    echo "void ${s}(void); void ${s}(void) { stub(\"${s}\"); }"
  done
} > "${OUT}/stubs.c"
${N48_CC} -std=c11 -O1 -w -c "${OUT}/stubs.c" -o "${OUT}/stubs.o"
${N48_CC} "${OBJS[@]}" "${OUT}/stubs.o" -o "${OUT}/dptrain"
"${OUT}/dptrain" ${VERBOSE[@]+"${VERBOSE[@]}"}
