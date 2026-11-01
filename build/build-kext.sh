#!/bin/bash
# Gate 0 kext build: the patched 0000-0028 tree, zig cc -> Mach-O, ld64.lld -> kext.
# No macOS, no codesign. Everything stays under D:\Work\gate0.
set -euo pipefail
G0="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$G0/cobalt64-navi44}"
OUT="${2:-$G0/build/kext-0028}"
MKSDK="${MKSDK:-$G0/tools/MacKernelSDK}"
export ZIG_GLOBAL_CACHE_DIR="$G0/zig-cache/global"
export ZIG_LOCAL_CACHE_DIR="$G0/zig-cache/local"
export ZIG="${ZIG:-$G0/tools/zig-x86_64-windows-0.15.2/zig.exe}"
export LD64="${LD64:-$G0/tools/llvm20-bin/ld64.lld.exe}"
export NM="${NM:-$G0/tools/llvm20-bin/llvm-nm.exe}"
export PY="${PY:-$(command -v python)}"
mkdir -p "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR" "$(dirname "$OUT")"
exec "$G0/tools/xbuild-kext.sh" "$SRC" "$MKSDK" "$OUT"
