#!/usr/bin/env bash
# run.sh - the browser Metal gap list: which MTL* selectors WebKit, ANGLE, Dawn, Skia and Chromium send that navi48metal's classes do not implement.
#   tools/native/mtlgap/run.sh [out-dir]      (default out-dir: ${N48_CACHE:-~/.cache/navi48-check}/mtlgap-out)
# Output: gap.tsv (one row per protocol x selector, with a file:line sample) and gap.md (grouped by protocol).
# A row whose class derives from an Apple base class ("census?") may still be implemented there: confirm it with the bundle's runtime census
# (N48M_CENSUS=1, n48_census.h) on the PC. Browser sources are sparse, shallow clones at the HEAD of the day; they are only read, never built or run.
set -euo pipefail

D="$(cd "$(dirname "${0}")" && pwd)"
ROOT="$(cd "${D}/../../.." && pwd)"
eval "$("${ROOT}/tools/check-deps.sh")"
CACHE="${N48_CACHE:-${HOME}/.cache/navi48-check}"
SRC="${CACHE}/browsers"
OUT="${1:-${CACHE}/mtlgap-out}"
mkdir -p "${SRC}" "${OUT}"

clone() {
  local name="${1}" repo="${2}"
  shift 2
  if [ ! -d "${SRC}/${name}/.git" ]; then
    git clone -q --depth 1 --filter=blob:none --sparse "${repo}" "${SRC}/${name}"
    git -C "${SRC}/${name}" sparse-checkout set --no-cone "${@}"
  else
    git -C "${SRC}/${name}" pull -q --depth 1
  fi
  echo "${name} $(git -C "${SRC}/${name}" log -1 --format='%h %cs')"
}

clone webkit https://github.com/WebKit/WebKit '/Source/WebGPU/' '/Source/WebCore/platform/graphics/' '/Source/WebCore/PAL/pal/spi/' \
  '/Source/WebKit/GPUProcess/' '/Source/WebKit/Shared/' '/Source/ThirdParty/ANGLE/src/libANGLE/renderer/metal/' '/Source/WebCore/Modules/WebGPU/' '/Source/WebKit/WebProcess/GPU/'
clone angle https://github.com/google/angle '/src/libANGLE/renderer/metal/' '/src/common/apple/'
clone dawn https://dawn.googlesource.com/dawn '/src/dawn/native/metal/' '/src/dawn/common/' '/src/dawn/native/Surface_metal.mm'
clone skia https://skia.googlesource.com/skia '/src/gpu/mtl/' '/src/gpu/graphite/mtl/' '/src/gpu/ganesh/mtl/' '/include/gpu/mtl/' '/include/gpu/graphite/mtl/'
clone chromium https://github.com/chromium/chromium '/gpu/' '/ui/gl/' '/ui/gfx/mac/' '/ui/accelerated_widget_mac/' '/components/viz/' \
  '/media/gpu/mac/' '/third_party/blink/renderer/platform/graphics/' '/services/webnn/coreml/'

M="${ROOT}/tools/native/navi48metal/Navi48Device.m"
python3 -I "${D}/mtlgap.py" "${N48_SDK}/System/Library/Frameworks/Metal.framework/Headers" "${M}" "${OUT}/gap.json" \
  "webkit=${SRC}/webkit" "angle=${SRC}/angle" "dawn=${SRC}/dawn" "skia=${SRC}/skia" "chromium=${SRC}/chromium"
python3 -I "${D}/report.py" "${OUT}/gap.json" "${OUT}/gap.md" "${OUT}/gap.tsv" "${M}"
echo "gap list: ${OUT}/gap.tsv ${OUT}/gap.md"
