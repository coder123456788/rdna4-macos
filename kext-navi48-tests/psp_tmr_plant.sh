#!/usr/bin/env bash
#
# tests/psp_tmr_plant.sh - planted breaks for tests/psp_tmr_test.cpp (patch 0010, host coverage for psp_v14_0.cpp).
#
# For each plant: copy the real sources into a scratch tree, apply ONE break (the script first proves the break changes the text exactly once),
# compile the real test against the scratch tree and demand that it FAILS - a compile error, a failing check, a crash or a hang cut off by the
# 60 s alarm all count. A plant the suite lets through is a hole: the script exits non-zero and says which.
#
# Every needle here is a single line with no newline in it. The working tree is checked out CRLF on Windows and LF on a Mac; a multi-line needle
# would match on one and not the other. Insertions append to the end of the same line for the same reason. Where two functions share a line
# character-for-character - psp_load_toc and psp_load_toc_subbin build the same LOAD_TOC union, and three places zero fw_pri the same way - the
# plant names which occurrence it means as a sixth argument, so a break in the boot path and a break in the dead loader are separate claims.
#
# No plant targets src/fw/*.c: those are generated firmware arrays, they compile once into a cache the whole run shares, and nothing in this
# suite's subject (the GPCOM protocol) lives in them.
#
# Check that a plant CHANGES the value before believing a pass. An earlier plant 12 put psp.cmdVRAMOffset where psp.cmdBusAddr & 0xFFFFFFFF
# already stood and "escaped" - the MC base (0x8000000000) has no low bits, so the low half of a bus address IS the VRAM offset and the two
# expressions are the same number: the tree was not broken, so nothing could be caught. The breakable half of an address is the high one.
#
#   run from anywhere:  src/navi48-bringup/tests/psp_tmr_plant.sh [first-plant-id last-plant-id]
#   on a Mac:           (needs nothing but clang++ and perl)
#   on Windows/Linux:   ZIG=/path/to/zig.exe src/navi48-bringup/tests/psp_tmr_plant.sh
#
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/psp140-plant.$$"
FWC=$SCR/fwcache
trap 'rm -rf "$SCR"' EXIT
total=0; escaped=0
FIRST=${1:-0}; LAST=${2:-9999}

# The Mac has clang++ in PATH; this Windows box has only zig, which is passed in as one quoted token so a path with spaces survives.
if [ -n "${ZIG:-}" ]; then CCW=("$ZIG" cc); CXXW=("$ZIG" c++); else CCW=(clang); CXXW=(clang++); fi
# macOS ships no coreutils timeout; there perl's alarm does the job (and does survive exec there).
if command -v timeout >/dev/null 2>&1; then TIMEOUT=(timeout 60); else TIMEOUT=(perl -e 'alarm 60; exec @ARGV'); fi

rm -rf "$FWC"; mkdir -p "$FWC" || { echo "cannot make $FWC"; exit 2; }
n=0
for f in "$ROOT/$SR"/fw/*.c; do
    "${CCW[@]}" -std=c11 -O1 -I "$ROOT/$SR/fw" -I "$ROOT/$SR/amd" -I "$ROOT/$SR" -c "$f" -o "$FWC/$(basename "${f%.c}").o" \
        || { echo "CONTROL: the embedded firmware objects do not compile"; exit 2; }
    n=$((n+1))
done
echo "fw objects built once: $n (src/fw is generated data; no plant touches it)"

reset_tree() {
    rm -rf "$SCR/$SR" "$SCR/$K/tests"
    mkdir -p "$SCR/$K/tests" || return 1
    # The whole src tree, minus the 24 MB of generated firmware blobs: n48log.h reaches
    # ../Navi48UserClientABI.h and fw_table.h reaches ../amd/amdgpu_ip.h, so partial
    # copies break on include order rather than on the plant. fw/*.c is replaced by $FWC.
    cp -R "$ROOT/$SR" "$SCR/$SR" || return 1
    rm -f "$SCR/$SR"/fw/*.c
    cp -R "$ROOT/$K/tests/dalshim" "$SCR/$K/tests/" || return 1
    cp "$ROOT/$K/tests/psp_tmr_test.cpp" "$SCR/$K/tests/" || return 1
}

compile() {   # compile <out binary> - the same line the header of psp_tmr_test.cpp documents
    ( cd "$SCR" && "${CXXW[@]}" -std=c++17 -Wall -Wextra -O1 -I $K/tests/dalshim -I $SR -I $SR/amd -I $SR/fw \
        $K/tests/psp_tmr_test.cpp $SR/amd/psp_v14_0.cpp $SR/amd/fw_table.cpp "$FWC"/*.o -o "$1" 2>&1 )
}

# A plant must only ever touch the scratch copy. An early version of this script passed
# the repo-relative path to the editor and broke the real psp_v14_0.cpp; the checksum
# guard is what makes that mistake loud instead of silent.
pristine_sum() { ( cd "$ROOT" && { find $SR -type f ! -path '*/fw/*'; find $K/tests/dalshim -type f; echo $K/tests/psp_tmr_test.cpp; } | sort | xargs cksum ) | sort; }
SNAP="$SCR/pristine.sum"; pristine_sum > "$SNAP" || { echo "cannot checksum the pristine tree"; exit 2; }
check_pristine() {
    local now="$SCR/after.sum"
    pristine_sum > "$now"
    if cmp -s "$SNAP" "$now"; then echo "pristine tree untouched by the planting"; return 0; fi
    echo "*** THE PLANT SCRIPT MODIFIED THE REAL TREE - restore it from git before anything else ***"
    diff "$SNAP" "$now" | head -8
    return 1
}

plant() {   # plant <id> <file relative to repo root> <old text> <new text> <description> [nth]
  local id="$1" file="$2" old="$3" new="$4" desc="$5" nth="${6:-}"
  if [ "$id" -lt "$FIRST" ] || [ "$id" -gt "$LAST" ]; then return; fi
  reset_tree || { echo "PLANT $id: $desc: COULD NOT STAGE THE SCRATCH TREE"; escaped=$((escaped+1)); return; }
  if [ ! -f "$SCR/$file" ]; then echo "PLANT $id: $desc: SCRATCH COPY $file MISSING"; escaped=$((escaped+1)); return; fi
  # `nth` picks one copy of a line that appears more than once - psp_load_toc and
  # psp_load_toc_subbin build the same LOAD_TOC union with character-identical lines,
  # so a needle without a selector is ambiguous. index()/substr() keep both the search
  # and the replacement literal: no regex, no $1 interpolation.
  FILE="$SCR/$file" OLD="$old" NEW="$new" NTH="$nth" perl -e '
      local $/; open F, "<", $ENV{FILE} or exit 4; my $s = <F>; close F;
      my ($o, $n, $k) = ($ENV{OLD}, $ENV{NEW}, $ENV{NTH});
      my @p; my $i = 0;
      while (($i = index($s, $o, $i)) >= 0) { push @p, $i; $i += length($o) }
      if ($k) {
          if (@p < $k) { printf "PLANT OCCURRENCE %d OF %d FOR: %s\n", $k, scalar(@p), substr($o, 0, 70); exit 3; }
          substr($s, $p[$k-1], length($o)) = $n;
      } else {
          if (@p != 1) { printf "PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %s\n", scalar(@p), substr($o, 0, 70); exit 3; }
          substr($s, $p[0], length($o)) = $n;
      }
      open O, ">", $ENV{FILE} or exit 4; print O $s; close O;
  ' || { total=$((total+1)); echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; }
  total=$((total+1))
  local out
  out=$(compile "$SCR/t")
  if [ $? -ne 0 ]; then
    echo "PLANT $id: $desc: CAUGHT at compile time: $(echo "$out" | grep -m1 -E 'error|static assertion' | cut -c1-140)"; return
  fi
  out=$( cd "$SCR" && "${TIMEOUT[@]}" ./t 2>&1 ); local trc=$?
  if [ $trc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED (the suite passed) ***"; escaped=$((escaped+1)); return; fi
  if [ $trc -ge 128 ] || [ $trc -eq 124 ]; then echo "PLANT $id: $desc: CAUGHT by a crash or hang (status $trc)"; return; fi
  echo "PLANT $id: $desc: CAUGHT by $(echo "$out" | grep -c '^FAIL') check(s); first: $(echo "$out" | grep -m1 '^FAIL' | cut -c1-120)"
}

control() {
  reset_tree || { echo "CONTROL: could not stage the scratch tree"; exit 2; }
  local out
  out=$(compile "$SCR/t") || { echo "CONTROL: the unplanted tree does not compile: $out"; exit 2; }
  out=$( cd "$SCR" && "${TIMEOUT[@]}" ./t 2>&1 ) || { echo "CONTROL: the unplanted tree does not pass: $(echo "$out" | grep '^FAIL' | head -3)"; exit 2; }
  echo "CONTROL: the unplanted copy compiles and passes ($(echo "$out" | tail -1))"
}
control

P=$SR/amd/psp_v14_0.cpp
H=$SR/amd/amdgpu_psp.h
I=$SR/amd/amdgpu_ip.h
SH=$K/tests/dalshim/IOKit/IOReturn.h

# ---- ring creation: the mailbox handshake a first boot lives or dies on ----
plant 1  $P '    WREG32(dev, reg64, kPSPRingTypeKM << 16);' '    WREG32(dev, reg64, kPSPRingTypeUM << 16);' "INIT_GPCOM_RING kicks with the RBI ring type (0x10000) - the value that is answered OK and then silently ignored"
plant 2  $I 'constexpr uint32_t kPSPRingTypeKM = 2;' 'constexpr uint32_t kPSPRingTypeKM = 1;' "PSP_RING_TYPE__KM itself is wrong in the header"
plant 3  $I 'constexpr uint32_t kPSPKMRingSize    = 0x1000;' 'constexpr uint32_t kPSPKMRingSize    = 0x2000;' "the ring is advertised as 8 KB"
plant 4  $I 'constexpr uint32_t kPSPGfxCmdStatusMask   = 0x0000FFFFu;' 'constexpr uint32_t kPSPGfxCmdStatusMask   = 0x0000FFFEu;' "the mailbox poll mask loses bit 0 of the status field"
plant 5  $P '    WREG32(dev, reg71, static_cast<uint32_t>(psp.ringSize));' '    WREG32(dev, reg71, 0x2000u);' "C2PMSG_71 is written with a hard-coded size, not the context's"
plant 6  $P '    WREG32(dev, reg69, static_cast<uint32_t>(psp.ringBusAddr & 0xFFFFFFFFu));' '    WREG32(dev, reg69, 0u);' "C2PMSG_69 never gets the ring's low address half"
plant 7  $P '    WREG32(dev, reg70, static_cast<uint32_t>(psp.ringBusAddr >> 32));' '    WREG32(dev, reg70, 0u);' "C2PMSG_70 never gets the ring's high address half"
plant 8  $P '    bar0_memset_vram(dev, psp.ringVRAMOffset, 0, kPSPKMRingBufSize);' '    bar0_memset_vram(dev, psp.ringVRAMOffset, 0, 64u);' "only the first frame slot of the ring is zeroed"
plant 9  $P '    WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_67), 0);' '    WREG32(dev, SOC15_REG_OFFSET(dev, IPBlock::MP0, MP0Regs::C2PMSG_67), 16);' "the stale wptr mailbox is not reset after the ring exists"
plant 10 $P 'static constexpr uint64_t kRingVRAMOffset   = 0x100000;   // 1 MB' 'static constexpr uint64_t kRingVRAMOffset   = 0x200000;   // 1 MB' "the ring is laid over the reserved TMR slot"

# ---- the GPCOM frame: the five fields, and nothing else ----
plant 11 $P '    frame.fence_value     = fence_index;' '    frame.fence_value     = fence_index; frame.cmd_buf_size = kPSPCmdBufSize;' "the frame sets cmd_buf_size - PSP drops every frame with a non-zero one"
plant 12 $P '    frame.cmd_buf_addr_hi = static_cast<uint32_t>(psp.cmdBusAddr >> 32);' '    frame.cmd_buf_addr_hi = 0u;' "the command buffer loses its MC base - PSP would DMA from the wrong window"
plant 13 $P '    frame.fence_addr_lo   = static_cast<uint32_t>(psp.fenceBusAddr & 0xFFFFFFFFu);' '    frame.fence_addr_lo   = static_cast<uint32_t>(psp.cmdBusAddr & 0xFFFFFFFFu);' "the fence address points at the command buffer"
plant 14 $P '    uint32_t fence_index = ++psp.fenceCounter;' '    uint32_t fence_index = psp.fenceCounter;' "the fence value never advances, so a stale zero acks every submit"
plant 15 $P '    wptr_dw = (wptr_dw + frameSizeDw) % ringSizeDw;' '    wptr_dw = (wptr_dw + 1u) % ringSizeDw;' "wptr is published in frames, not dwords"
plant 16 $P '        observed = RBAR2_32(dev, psp.fenceVRAMOffset);' '        observed = RBAR2_32(dev, psp.fenceVRAMOffset + 4);' "the fence is polled one dword past where PSP writes it"
plant 17 $P '        psp.cmdVRAMOffset + kPSPGfxRespStatusOffset);' '        psp.cmdVRAMOffset + 16);' "resp.status is read at the reference's cmd_buf+16"
plant 18 $H 'constexpr uint32_t kPSPGfxRespOffset          = 864;' 'constexpr uint32_t kPSPGfxRespOffset          = 832;' "the response block is 32 bytes early"
plant 19 $H 'constexpr uint32_t kPSPGfxCmdUnionOffset = 28;' 'constexpr uint32_t kPSPGfxCmdUnionOffset = 32;' "the command union is written 4 bytes late"

# ---- setup_tmr: which version, and what is (not) submitted ----
plant 20 $P '    const IPVersion discovered = dev.ip.getVersion(IPBlock::MP0);' '    const IPVersion discovered = IPVersion{0, 0, 0};' "patch 0009 undone: the PSP version is never taken from discovery"
plant 21 $P '    const IPVersion pspVer = discovered.major ? discovered : kIP_PSP;' '    const IPVersion pspVer = kIP_PSP;' "the pinned Navi 48 version is used for every die"
plant 22 $P '    if (pspVer.major == 14) {' '    if (false) {' "v14 silicon is sent SETUP_TMR anyway (SOS already owns the TMR)"
plant 23 $P '        if (psp.toc.start_addr != nullptr && psp.toc.size_bytes > 0) {' '        if (false) {' "LOAD_TOC is never submitted, so TMR-resident LOAD_IP_FW will be refused"
plant 24 $P '        psp.tmrBusAddr = 0;  // SOS-managed; we have no MC address' '        psp.tmrBusAddr = dev.vramMC(psp.tmrVRAMOffset);  // SOS-managed; we have no MC address' "the driver claims a TMR address SOS never gave it"
plant 25 $P '            if (tr != kIOReturnSuccess) {' '            if (false) {' "a failed LOAD_TOC still latches tmrSetUp (the ordering hazard)"
plant 26 $P '                return tr;' '                return kIOReturnSuccess;' "the LOAD_TOC failure is swallowed and reported as success"

# ---- LOAD_TOC payload + the legacy SETUP_TMR frame ----
plant 27 $P '    toc->toc_size        = subSize;' '    toc->toc_size        = static_cast<uint32_t>(psp.fwPriSize);' "toc_size is the staging buffer, not the payload (upstream: the payload)"
plant 28 $P '    toc->toc_phy_addr_lo = static_cast<uint32_t>(psp.fwPriBusAddr & 0xFFFFFFFFu);' '    toc->toc_phy_addr_lo = static_cast<uint32_t>(psp.ringBusAddr & 0xFFFFFFFFu);' "the sub-bin TOC is staged at the ring's address" 2
plant 29 $P '    bar0_memcpy_to_vram(dev, psp.fwPriVRAMOffset, subBin, subSize);' '    bar0_memcpy_to_vram(dev, psp.fwPriVRAMOffset + 64, subBin, subSize);' "the payload lands at fw_pri+64 instead of +0"
plant 30 $P '    if (subSize > psp.fwPriSize) {' '    if (false) {' "a payload bigger than fw_pri is staged anyway (it would overwrite the ring)"
plant 31 $P '        psp.cmdVRAMOffset + kPSPGfxRespTmrSizeOffset);' '        psp.cmdVRAMOffset + 16);' "tmr_size is read at the reference's cmd_buf+16"
plant 32 $P '    hdr->cmd_id = PSPGfxCmd::SETUP_TMR;' '    hdr->cmd_id = PSPGfxCmd::LOAD_TOC;' "the legacy frame is submitted as the wrong command"
plant 33 $P '    tmr->tmr_flags          = 0x2;  // bit 1 = virt_phy_addr per upstream' '    tmr->tmr_flags          = 0x0;  // bit 1 = virt_phy_addr per upstream' "the legacy SETUP_TMR drops virt_phy_addr"
plant 34 $P '    tmr->buf_size           = static_cast<uint32_t>(psp.tmrSize);' '    tmr->buf_size           = 0u;' "the legacy SETUP_TMR advertises a zero-length TMR"

# ---- the host shim: a wrong error value would make every kr comparison a lie ----
plant 35 $SH '#define kIOReturnTimeout         iokit_common_err(0x2d6)' '#define kIOReturnTimeout         iokit_common_err(0x2d7)' "the shim's kIOReturnTimeout is not the kernel's"
plant 36 $SH '#define kIOReturnNotReady        iokit_common_err(0x2d8)' '#define kIOReturnNotReady        iokit_common_err(0x2d6)' "the shim's kIOReturnNotReady is the timeout code"

# ---- the fw_pri zeroing and the file loader: the lines the two builders share ----
plant 37 $P '    toc->toc_phy_addr_lo = static_cast<uint32_t>(psp.fwPriBusAddr & 0xFFFFFFFFu);' '    toc->toc_phy_addr_lo = static_cast<uint32_t>(psp.ringBusAddr & 0xFFFFFFFFu);' "psp_load_toc stages the TOC at the ring's address" 1
plant 38 $P '    toc->toc_size        = payload_size;' '    toc->toc_size        = tocSize;' "psp_load_toc submits the whole file, not the payload its header names"
plant 39 $P '    if (payload_offset == 0 ||' '    if (payload_offset > tocSize ||' "psp_load_toc accepts a header whose payload offset is 0"
plant 40 $P '    bar0_memset_vram(dev, psp.fwPriVRAMOffset, 0, psp.fwPriSize);' '    bar0_memset_vram(dev, psp.fwPriVRAMOffset, 0, 64u);' "psp_load_toc zeroes 64 B of fw_pri, so stale bytes survive behind the payload" 1
plant 41 $P '    bar0_memset_vram(dev, psp.fwPriVRAMOffset, 0, psp.fwPriSize);' '    bar0_memset_vram(dev, psp.fwPriVRAMOffset, 0, 64u);' "the boot path's psp_copy_fw analogue zeroes 64 B of fw_pri" 2
plant 42 $P '    bar0_memset_vram(dev, psp.cmdVRAMOffset,   0, kPSPCmdBufSize);' '    bar0_memset_vram(dev, psp.cmdVRAMOffset,   0, psp.fwPriSize);' "the command-buffer zeroing uses the 1 MB size and wipes the TMR slot too"
plant 43 $P '    frame.fence_addr_hi   = static_cast<uint32_t>(psp.fenceBusAddr >> 32);' '    frame.fence_addr_hi   = 0u;' "the fence address loses its MC base, so the ack lands outside the aperture"

check_pristine || escaped=$((escaped+1))
echo "psp_tmr_plant: $total plants, $escaped escaped"
[ $escaped -eq 0 ]
