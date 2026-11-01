#!/bin/bash
# xbuild-kext.sh - build Cobalt64's Navi48Bringup.kext on Windows, no macOS and no make.
#
# Portable clang (zig) compiles to Mach-O; LLVM's ld64.lld links the kext bundle.
# MacKernelSDK supplies every kernel header and libkmod/libkmodc++, so no Apple SDK is needed.
#
#   TOOLS=... ./xbuild-kext.sh <path-to-Cobalt64> <path-to-MacKernelSDK> <out-dir>
#
# What it does NOT do: codesign (OpenCore injects unsigned kexts), or run anything.

set -euo pipefail

SRC="${1:?usage: xbuild-kext.sh <Cobalt64 dir> <MacKernelSDK dir> <out dir>}"
MKSDK="${2:?missing MacKernelSDK dir}"
OUT="${3:?missing out dir}"

HERE="$(cd "$(dirname "$0")" && pwd)"
ZIG="${ZIG:-$HERE/../build/zig-x86_64-windows-0.15.2/zig.exe}"
LD64="${LD64:-$HERE/../build/llvm-bin/ld64.lld.exe}"
NM="${NM:-$HERE/../build/llvm-bin/llvm-nm.exe}"
PY="${PY:-$HERE/../build/tools/python/python.exe}"

[ -x "$ZIG" ] || { echo "no zig at $ZIG" >&2; exit 1; }
[ -x "$LD64" ] || { echo "no ld64.lld at $LD64" >&2; exit 1; }
[ -d "$MKSDK/Headers" ] || { echo "no MacKernelSDK headers at $MKSDK" >&2; exit 1; }

KSRC_PRE="$SRC/src/navi48-bringup"
[ -d "$KSRC_PRE" ] || { echo "not a Cobalt64 tree: $SRC" >&2; exit 1; }
[ -d "$MKSDK/Headers" ] || { echo "no MacKernelSDK headers at $MKSDK" >&2; exit 1; }
SRC="$(cd "$SRC" && pwd)"
MKSDK="$(cd "$MKSDK" && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
KSRC="$SRC/src/navi48-bringup"
OBJ="$OUT/obj"
BUNDLE="$OUT/Navi48Bringup.kext"
rm -rf "$OBJ" "$BUNDLE"
mkdir -p "$OBJ/dcn41" "$KSRC/src/fw" "$BUNDLE/Contents/MacOS"

XLAT="$SRC/src/xlat12"
SC="$SRC/src/shadercache"
DCN="$SRC/src/dcn41"

TARGET=(-target x86_64-macos.11.0)
INC=(-nostdinc "-I$MKSDK/Headers" "-I$KSRC/src" "-I$KSRC/src/amd" "-I$XLAT" "-I$SC" "-I$DCN")
DEFS=(-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -D_FORTIFY_SOURCE=0)
WARN=(-fno-builtin -fno-common -fno-stack-protector -mkernel -fapple-kext -Wall -Os -g)
CFLAGS=("${TARGET[@]}" "${INC[@]}" "${DEFS[@]}" "${WARN[@]}")
CXXFLAGS=("${CFLAGS[@]}" -std=c++17 -fno-exceptions -fno-rtti -fcheck-new -fno-c++-static-destructors)

fails=0
say() { printf '%s\n' "$*"; }

# --- firmware blobs -> C arrays ------------------------------------------------
say "embedding firmware"
for b in "$KSRC"/firmware/*.bin; do
    n="$(basename "$b" .bin)"
    "$PY" "$KSRC/tools/embed-firmware.py" "$b" "$KSRC/src/fw/fw_$n.c" "fw_$n" >/dev/null
done
for b in "$KSRC"/shaders/*.bin; do
    n="$(basename "$b" .bin)"
    "$PY" "$KSRC/tools/embed-firmware.py" "$b" "$KSRC/src/fw/fw_shader_$n.c" "fw_shader_$n" >/dev/null
done
# Optional blobs the public tree does not ship: empty arrays, exactly as the Makefile does.
for n in shadercache shader_blit_copy_gfx1201 shader_blit_diag_gfx1201 shader_blit_diagmin_gfx1201 shader_blit_copy_offen_gfx1201; do
    [ -f "$KSRC/firmware/$n.bin" ] && continue
    cat > "$KSRC/src/fw/fw_$n.c" <<EOF
#include <stddef.h>
#include <stdint.h>
const size_t fw_${n}_size = 0;
const uint8_t fw_${n}[64] __attribute__((aligned(64))) = { 0 };
EOF
done
say "  $(ls "$KSRC"/src/fw/fw_*.c | wc -l) generated"

# --- compile ------------------------------------------------------------------
build_c() {  # $1 = source, $2 = object
    mkdir -p "$(dirname "$2")"
    if ! "$ZIG" cc "${CFLAGS[@]}" -c "$1" -o "$2" 2>"$2.err"; then
        say "FAIL cc $1"; sed -n '1,12p' "$2.err" | sed 's/^/    /'; fails=$((fails+1)); return 1
    fi
    [ -s "$2.err" ] && grep -v '^warning: ignoring -fapple-kext' "$2.err" | sed -n '1,6p' | sed 's/^/    /' || true
    rm -f "$2.err"
}
build_cxx() {
    mkdir -p "$(dirname "$2")"
    if ! "$ZIG" c++ "${CXXFLAGS[@]}" -c "$1" -o "$2" 2>"$2.err"; then
        say "FAIL c++ $1"; sed -n '1,15p' "$2.err" | sed 's/^/    /'; fails=$((fails+1)); return 1
    fi
    [ -s "$2.err" ] && sed -n '1,6p' "$2.err" | sed 's/^/    /' || true
    rm -f "$2.err"
}

say "compiling"
objs=()
cd "$KSRC"
for f in src/kmod_info.c src/fw/fw_*.c "$XLAT"/xlat12.c "$XLAT"/xlat12_ib.c "$XLAT"/xlat12_headless.c "$SC"/shadercache.c; do
    o="$OBJ/$(basename "$f" .c).o"; objs+=("$o"); build_c "$f" "$o" || true
done
for f in "$DCN"/*.c; do
    o="$OBJ/dcn41/$(basename "$f" .c).o"; objs+=("$o"); build_c "$f" "$o" || true
done
for f in src/*.cpp src/amd/*.cpp src/apple/*.cpp src/dcn/*.cpp; do
    [ -f "$f" ] || continue
    o="$OBJ/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')"; objs+=("$o"); build_cxx "$f" "$o" || true
done

if [ "$fails" -ne 0 ]; then say "$fails file(s) failed to compile"; exit 1; fi
say "  ${#objs[@]} objects"

# --- link ---------------------------------------------------------------------
say "linking with ld64.lld -kext"
EXEC="$BUNDLE/Contents/MacOS/Navi48Bringup"
# libkmodc++.a exists only in Library/universal in current MacKernelSDK revisions;
# the upstream Makefile's -L .../x86_64 alone cannot find it.
# IOKit/libkern calls (_IOSleep, _current_thread, IOPCIDevice methods) are kernel exports with no
# stub library in MacKernelSDK, so they stay undefined and the kernel resolves them at load.
"$LD64" -arch x86_64 -platform_version macos 11.0 11.0 -kext -undefined dynamic_lookup \
    -o "$EXEC" "${objs[@]}" \
    -L"$MKSDK/Library/universal" -lkmodc++ -lkmod -L"$MKSDK/Library/x86_64" \
    -no_deduplicate 2>&1 | sed 's/^/    /' || { say "LINK FAILED"; exit 1; }

cp "$KSRC/Info.plist" "$BUNDLE/Contents/Info.plist"

# lld accepts -kext but does not implement it: the output comes out MH_EXECUTE (2) with LC_MAIN and
# LC_LOAD_DYLINKER, which a kext must not carry. Set the filetype to MH_KEXT_BUNDLE (11) and drop
# those two load commands. The command block sits in page-aligned slack before the first section,
# so shrinking it in place moves no segment, symbol or string table offset.
"$PY" - "$EXEC" <<'PY'
import struct, sys
DROP = {0x80000028: 'LC_MAIN', 0x0e: 'LC_LOAD_DYLINKER'}
p = sys.argv[1]
with open(p, 'r+b') as f:
    data = bytearray(f.read())
magic, cpu, sub, ftype, ncmds, sizeofcmds = struct.unpack('<IiiIII', data[:24])
assert magic == 0xfeedfacf, f"not a 64-bit Mach-O: {magic:#x}"
assert ftype == 2, f"expected MH_EXECUTE, got {ftype}"
hdr_end = 32
cmds = []
off = hdr_end
for _ in range(ncmds):
    c, size = struct.unpack('<II', data[off:off + 8])
    cmds.append((c, size, off))
    off += size
kept = bytearray()
dropped = []
for c, size, o in cmds:
    if c in DROP:
        dropped.append(f"{DROP[c]}({size}b)")
        continue
    kept += data[o:o + size]
assert len(kept) <= sizeofcmds, "load commands grew; refusing to shift"
out = bytearray(data)
out[hdr_end:hdr_end + sizeofcmds] = kept + bytes(sizeofcmds - len(kept))
struct.pack_into('<I', out, 12, 11)                    # filetype = MH_KEXT_BUNDLE
struct.pack_into('<I', out, 16, ncmds - len(dropped))  # ncmds
struct.pack_into('<I', out, 20, len(kept))             # sizeofcmds
with open(p, 'wb') as f:
    f.write(out)
print(f"    mach-o: filetype 2 -> 11 (MH_KEXT_BUNDLE), dropped {', '.join(dropped)}, {len(kept)} bytes of load commands")
PY

say "  $(ls -l "$EXEC" | awk '{print $5}') bytes"

# --- the Makefile's own guard: our symbols must all be defined -----------------
if [ -x "$NM" ]; then
    bad=$("$NM" -u "$EXEC" 2>/dev/null | grep -E 'Navi48|AmdTtl|amdgpu::|n48::|n48dcn::|xlat12|_sc_|dcn41_' || true)
    if [ -n "$bad" ]; then
        say "LINK ERROR: defined nowhere, the kernel would refuse the kext:"; echo "$bad" | sed 's/^/    /'; exit 1
    fi
    say "  no undefined symbols of our own"
else
    say "  (no llvm-nm, skipped the undefined-symbol guard)"
fi

say "built $BUNDLE"
