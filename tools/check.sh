#!/usr/bin/env bash
# check.sh - the checks that need no GPU and no Mac: run before every push (CI runs it too).
#   tools/check.sh [--update-protocols] [out-dir]      (default out-dir: ${N48_CACHE:-~/.cache/navi48-check}/check-out; never inside the tree)
#   1. navi48metal bundle: Navi48Device.m compiled for x86_64 macOS against the macOS 26.5 SDK headers, -Werror, in the three
#      build.sh variants (default, N48_LAZY=0, N48_9D=0): missing declarations, type errors, unused code of a variant.
#   2. the bundle's classes against the MTL* protocols they claim at run time (tools/check-protocols.py): signatures the ABI
#      sees differently from the SDK, and misspelt or dropped protocol methods (the required methods left to the Apple base
#      classes must match tools/native/navi48metal/protocol-baseline.txt; --update-protocols rewrites it after a deliberate change).
#   3. the bundle's pure host tests (test-*.c, run against this tree's sources).
#   4. the dcn41 unit tests (the tools/dcn41/build.py `unit` compile line).
# Not here: the kext suites (tools/conductor/suites.sh; many need captured fixtures that are not in the public tree), anything that
# needs RADV / the GPU, and the planted-break scripts (CI runs tools/native/navi48metal/test-gate-plant.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "${0}")/.." && pwd)"
UPDATE_PROTOCOLS=""
if [ "${1:-}" = "--update-protocols" ]; then
  UPDATE_PROTOCOLS="--update"
  shift
fi
eval "$("${ROOT}/tools/check-deps.sh")"
OUT="${1:-${N48_CACHE:-${HOME}/.cache/navi48-check}/check-out}"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"
case "${OUT}/" in
  "${ROOT}"/*) echo "refusing: out-dir is inside the tree (${OUT})" >&2; exit 2 ;;
esac

NFAIL=0
step() {
  local name="${1}" log="${OUT}/${1}.log"
  shift
  if "${@}" > "${log}" 2>&1; then
    echo "ok    ${name} :: $(tail -1 "${log}" | cut -c1-120)"
  else
    echo "FAIL  ${name} (log: ${log})"
    tail -15 "${log}" | sed 's/^/      /'
    NFAIL=$((NFAIL + 1))
  fi
}

# ---- 1. the bundle, cross-compiled for the PC ----
M="${ROOT}/tools/native/navi48metal"
# -Wno-date-time: zig's clang turns -Wdate-time on (reproducible builds), Apple clang does not, and Navi48Device.m keys its pipeline cache on __DATE__ / __TIME__ on purpose.
OBJC=(${N48_CC} -target "${N48_MAC_TARGET}" -isysroot "${N48_SDK}" -isystem "${N48_SDK}/usr/include" -iframework "${N48_SDK}/System/Library/Frameworks"
      -fobjc-arc -Wall -Wextra -Wno-date-time -I"${M}" -I"${N48_VKH}")
step "bundle-x86_64"        "${OBJC[@]}" -O2 -Werror -DN48_LAZY=1 -DN48_9D=1 -c "${M}/Navi48Device.m" -o "${OUT}/Navi48Device.o"
step "bundle-x86_64-lazy0"  "${OBJC[@]}" -O2 -Werror -DN48_LAZY=0 -DN48_9D=1 -c "${M}/Navi48Device.m" -o "${OUT}/Navi48Device-lazy0.o"
step "bundle-x86_64-9d0"    "${OBJC[@]}" -O2 -Werror -DN48_LAZY=1 -DN48_9D=0 -c "${M}/Navi48Device.m" -o "${OUT}/Navi48Device-9d0.o"

# ---- 2. the bundle's classes against the Metal protocols ----
step "bundle-protocols" python3 -I "${ROOT}/tools/check-protocols.py" "${M}/Navi48Device.m" "${M}/protocol-baseline.txt" \
  "${N48_SDK}/System/Library/Frameworks/Metal.framework/Headers" ${UPDATE_PROTOCOLS} -- "${OBJC[@]}" -O0

# ---- 3. the bundle's host tests ----
# Not run here, and why:
#   test-dumpacl            needs macOS <sys/acl.h>
#   test-vkdepth, -vkimage  link RADV (run them on the PC)
#   test-xlate*             need macOS <mach-o/*.h> and libn48xlate
#   test-m6route, test-m6x  read INSTALL.md, which is not part of the public tree
bundle_test() {
  local t="${1}"
  (cd "${M}" && ${N48_CC} -O1 -Wall -Wextra -Werror ${N48_SAN} -I. -I"${N48_VKH}" -o "${OUT}/test-${t}" "test-${t}.c" -lm && "${OUT}/test-${t}" .)
}
for t in cblog cienv crc depth dispflip drawcache gate heapalloc hotswap impcache intfmt ioalias ledger occ plane pool t1 texdesc; do
  step "test-${t}" bundle_test "${t}"
done

# ---- 4. dcn41 unit tests ----
DCN="${ROOT}/src/dcn41"
dcn_test() {
  local t="${1}"
  ${N48_CC} -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wconversion -Wno-sign-conversion \
    ${N48_SAN} -I"${DCN}" "${DCN}"/*.c "${DCN}/tests/${t}.c" -o "${OUT}/${t}" && "${OUT}/${t}"
}
for t in test_dcn41 test_dcn41_dmub test_dcn41_modes test_dcn41_allow test_dcn41_otg_timing; do
  step "${t}" dcn_test "${t}"
done

echo "check: ${NFAIL} failed (logs: ${OUT})"
[ "${NFAIL}" -eq 0 ]
