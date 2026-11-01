#!/bin/bash
# Build + run the Mesa Darwin host unit tests with zig cc (no Mac needed).
# acd_kext_test.c brings its own acd_mock_open/acd_kext_open_iokit stubs, so it
# must not link ac_darwin_mock.c; it also needs the hostshim/compat.c
# posix_memalign definition that acd_drm_test.c carries inline.
set -e
# Derive the workspace from this script's own location: the drive letter has
# moved (F: -> D:) once already and a hardcoded path silently builds nothing.
WS="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
Z="$WS/build/zig-x86_64-windows-0.15.2/zig.exe"
export ZIG_GLOBAL_CACHE_DIR="$WS/build/zig-cache/global"
export ZIG_LOCAL_CACHE_DIR="$WS/build/zig-cache/local"
SH="$WS/build/hostshim"
OUT="$WS/build/tmp"
cd "$WS/build/xsrc/mesa"

INC=(-std=gnu11 -O1 -fno-sanitize=undefined
     -I"$SH" -include "$SH/compat.h"
     -Iinclude -Iinclude/darwin -Isrc -Isrc/amd/common -Isrc/amd
     -Isrc/amd/common/darwin)
COMMON=(src/amd/common/darwin/ac_darwin_drm.c src/amd/common/darwin/ac_darwin_kext.c
        src/amd/common/darwin/ac_darwin_sync.c
        src/amd/common/darwin/amdgpu_vamgr.c src/amd/common/amdgpu_devices.c)

"$Z" cc "${INC[@]}" -o "$OUT/acd_drm_test.exe" \
  src/amd/common/darwin/test/acd_drm_test.c "${COMMON[@]}" \
  src/amd/common/darwin/ac_darwin_mock.c
echo "== built acd_drm_test =="
"$Z" cc "${INC[@]}" -o "$OUT/acd_kext_test.exe" \
  src/amd/common/darwin/test/acd_kext_test.c "$SH/compat.c" "${COMMON[@]}"
echo "== built acd_kext_test =="

for t in acd_drm_test acd_kext_test; do
  echo "---- run $t ----"
  "$OUT/$t.exe" 2>&1 | tail -4
done
