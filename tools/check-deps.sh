#!/usr/bin/env bash
# check-deps.sh - fetch (once) the pinned inputs the host checks need and print them as shell assignments:
#   eval "$(tools/check-deps.sh)"   ->  N48_SDK, N48_VKH, N48_CC, N48_CXX, N48_SAN (host-test sanitizer flags, empty without an ASan runtime), N48_MAC_TARGET,
#                                       N48_LINUX (a Linux tree holding drivers/gpu/drm/amd/display at the commit tools/dcn41 cites)
# Nothing is installed system-wide. Everything lands in ${N48_CACHE:-~/.cache/navi48-check}:
#   macOS SDK headers   Linux only (a Mac uses xcrun's SDK): MacOSX26.5.sdk from a public header mirror, sparse (headers only)
#   Vulkan-Headers      Khronos, the tag below
#   Linux AMD display   drivers/gpu/drm/amd/display only (DC, DML2.1, DMUB; MIT), at LINUX_REV: the commit tools/dcn41/dcn41lib.py pins
#   compiler            a Mac uses Xcode's clang; Linux the system clang, or without one the clang inside the ziglang wheel (pip, in a private venv, no root)
# Downloaded trees are data: they are only passed to the compiler as include paths, never executed.
set -euo pipefail

CACHE="${N48_CACHE:-${HOME}/.cache/navi48-check}"
SDK_REPO="https://github.com/alexey-lysiuk/macos-sdk"
SDK_REV="896cd40df984b847d486723edce50e247385617e"
SDK_NAME="MacOSX26.5.sdk"
VKH_REPO="https://github.com/KhronosGroup/Vulkan-Headers"
VKH_TAG="v1.4.365"
ZIG_VER="0.17.0"
LINUX_REPO="https://github.com/torvalds/linux"
LINUX_REV="238650ef6c7c7cca08e032527329424c9fbd70e5"

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
MAC_TARGET="x86_64-apple-macos12.0"

mkdir -p "${CACHE}"

# Shallow, blob-filtered fetch of one revision, restricted to the given sparse patterns.
fetch_sparse() {
  local dir="${1}" repo="${2}" rev="${3}"
  shift 3
  # the marker names the revision and the patterns: a new pattern list fetches again
  local done="${dir}/.n48-done-${rev}-$(printf '%s\n' "${@}" | cksum | cut -d' ' -f1)"
  [ -f "${done}" ] && return 0
  rm -rf "${dir}"
  git init -q "${dir}"
  git -C "${dir}" remote add origin "${repo}"
  git -C "${dir}" sparse-checkout set --no-cone "${@}"
  git -C "${dir}" fetch -q --depth 1 --filter=blob:none origin "${rev}"
  git -C "${dir}" checkout -q FETCH_HEAD
  touch "${done}"
}

fetch_sparse "${CACHE}/vulkan-headers" "${VKH_REPO}" "${VKH_TAG}" '/include/' >&2
fetch_sparse "${CACHE}/linux" "${LINUX_REPO}" "${LINUX_REV}" '/drivers/gpu/drm/amd/display/' '/drivers/gpu/drm/amd/include/*.h' >&2

if [ "$(uname -s)" = "Darwin" ]; then
  SDK="$(xcrun --show-sdk-path)"
  CC="xcrun clang"
  CXX="xcrun clang++"
else
  fetch_sparse "${CACHE}/macos-sdk" "${SDK_REPO}" "${SDK_REV}" \
    "/${SDK_NAME}/usr/include/" "/${SDK_NAME}/System/Library/Frameworks/" "/${SDK_NAME}/SDKSettings.json" >&2
  SDK="${CACHE}/macos-sdk/${SDK_NAME}"
  if command -v clang > /dev/null && command -v clang++ > /dev/null; then
    CC="clang"
    CXX="clang++"
  else
    # ponytail: no root -> zig's bundled clang; it has no ASan runtime, so the host tests run without sanitizers. apt install clang to get them.
    if [ ! -x "${CACHE}/venv/bin/python" ] || ! "${CACHE}/venv/bin/python" -m ziglang version 2>/dev/null | grep -qx "${ZIG_VER}"; then
      python3 -m venv "${CACHE}/venv" >&2
      "${CACHE}/venv/bin/pip" -q install "ziglang==${ZIG_VER}" >&2
    fi
    CC="${CACHE}/venv/bin/python -m ziglang cc"
    CXX="${CACHE}/venv/bin/python -m ziglang c++"
    SAN=""
    MAC_TARGET="x86_64-macos.12.0"
  fi
fi

printf 'N48_SDK=%q\nN48_VKH=%q\nN48_CC=%q\nN48_CXX=%q\nN48_SAN=%q\nN48_MAC_TARGET=%q\nN48_LINUX=%q\n' "${SDK}" "${CACHE}/vulkan-headers/include" "${CC}" "${CXX}" "${SAN}" "${MAC_TARGET}" "${CACHE}/linux"
