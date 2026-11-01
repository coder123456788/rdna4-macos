#!/bin/bash
# build-survey.sh - build Cobalt64 with the Navi 44 survey patch, offline.
#
# Run ON macOS, from the root of the KIT volume:
#     cd /Volumes/OPENCORE/KIT && ./build-survey.sh
#
# It writes ~/navi44/ and leaves a built kext there. Nothing is installed,
# loaded or signed with a personal certificate (ad-hoc only, as upstream does).

set -euo pipefail

KIT="$(cd "$(dirname "$0")" && pwd)"
WORK="$HOME/navi44"
LOG="$WORK/build.log"

say() { printf '\n==> %s\n' "$*"; }
die() { printf '\n!! %s\n' "$*" >&2; exit 1; }

say "checking the toolchain"
command -v xcrun >/dev/null || die "no xcrun: install the Xcode Command Line Tools (xcode-select --install)"
SDK="$(xcrun --sdk macosx --show-sdk-path)" || die "no macOS SDK: xcode-select --install"
[ -d "$SDK" ] || die "SDK path does not exist: $SDK"
echo "    sdk: $SDK"
command -v make >/dev/null || die "no make: install the Command Line Tools"
command -v git  >/dev/null || die "no git: install the Command Line Tools"

[ -f "$KIT/cobalt64-src.tar.gz" ] || die "cobalt64-src.tar.gz missing from $KIT"
[ -d "$KIT/MacKernelSDK" ]        || die "MacKernelSDK/ missing from $KIT"
[ -d "$KIT/firmware/navi48" ]     || die "firmware/navi48/ missing from $KIT"
[ -d "$KIT/firmware/navi44" ]     || die "firmware/navi44/ missing from $KIT"

say "unpacking source into $WORK"
mkdir -p "$WORK"
if [ -d "$WORK/Cobalt64" ]; then
    echo "    already present, leaving it alone"
else
    tar xzf "$KIT/cobalt64-src.tar.gz" -C "$WORK"
fi
cd "$WORK/Cobalt64"

say "applying the Navi 44 patches, in order"
#   0000  link libkmodc++ from Library/universal (upstream looks in x86_64 only)
#   0001  identity: match 0x7590, survey-only
#   0002  both dies' firmware sets, selected per device in probe()
#   0003  Navi 44 die geometry (per-GC-version caps; GB_ADDR_CONFIG wins)
#   0004  host tests for the die-select firmware, geometry and harvest masks
#   0005  survey properties: DMU segment bases, allowlist arming, GB_ADDR_CONFIG, FB_LOCATION
#   0006  host test for the PSP pre-SOS chain on both SOS containers
#   0007  DCN 4.1: insist only on the DMU base Linux also hardcodes
#   0008  ABI 1.13: QueryInfo reports this die (GFXTopo tail) so Mesa stops assuming gfx1201
#   0009  the firmware set is re-picked from IP discovery's GC/MP0/MP1 versions, not only from
#         the PCI id, and FW,SetSource says which authority decided
#   0010  host test for the PSP TMR/LOAD_TOC stage against a simulated SOS. Tests only: it moves
#         no kext source, so the already-applied probe below cannot see it and does not need to
#   0011  the SDMA engine census comes from IP discovery (count + sdma_mask) instead of the
#         compile-time 2; it moves kext sources, so the prebuilt .kext is only current after a
#         rebuild, and the already-applied probe below gains an sdma_census term.
#   0012  the Navi 44 writing stage is gated on discovery + GC version + confirmed firmware set
#         + an embedded SOS blob (navi44_arm_gate.h), and refusals publish PSP,GateReason; also
#         moves kext sources, so the probe gains a navi44_arm_gate term.
if grep -q 'navi44_arm_gate' src/navi48-bringup/src/Navi48Bringup.cpp &&
   grep -q 'sdma_census' src/navi48-bringup/src/amd/amdgpu_discovery.cpp &&
   grep -q 'fw_select_for_device' src/navi48-bringup/src/Navi48Bringup.cpp &&
   grep -q 'gfx_default_caps' src/navi48-bringup/src/amd/gfx_v12_0.cpp &&
   grep -q '0x7590' src/navi48-bringup/src/Navi48Bringup.cpp; then
    echo "    already applied"
else
    for p in "$KIT"/patches/00*.patch; do
        name="$(basename "$p")"
        if git apply --check "$p" 2>/dev/null; then
            git apply "$p"
            echo "    $name"
        else
            die "$name does not apply - this tree is partly patched, and the series is
                 cumulative (a patch only applies to the tree its predecessors produced).
                 Move $WORK/Cobalt64 away and re-run to unpack a fresh one."
        fi
    done
fi

say "copying firmware (both dies: the kext embeds both sets)"
mkdir -p src/navi48-bringup/firmware
cp "$KIT"/firmware/navi48/*.bin src/navi48-bringup/firmware/
cp "$KIT"/firmware/navi44/*.bin src/navi48-bringup/firmware/
ls -1 src/navi48-bringup/firmware | sed 's/^/    /'

say "building (this takes a minute; log at $LOG)"
make -C src/navi48-bringup MKSDK="$KIT/MacKernelSDK" all 2>&1 | tee "$LOG"

KEXT="$WORK/Cobalt64/src/navi48-bringup/build/Navi48Bringup.kext"
[ -d "$KEXT" ] || die "build finished but $KEXT is missing; read $LOG"

say "done"
echo "    kext: $KEXT"
echo
echo "Next, in this KIT:"
echo
echo "    ./install-survey-kext.sh \"$KEXT\""
echo
echo "That copies it into EFI/OC/Kexts, adds the Kernel > Add entry and sets"
echo "boot-args to exactly 'navi48bringup=1 -v keepsyms=1'. Do NOT add"
echo "navi48-psp=1 or navi48-stage=N yourself: those write to the GPU."
echo "Then reboot and run ./collect-survey.sh."
