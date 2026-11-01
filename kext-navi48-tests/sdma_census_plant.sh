#!/usr/bin/env bash
#
# tests/sdma_census_plant.sh - planted breaks for tests/sdma_census_test.cpp (patch 0011, the SDMA engine census).
#
# For each plant: copy the real sources into a scratch tree, apply ONE break (the script first proves the break changes the text exactly once),
# compile the real test against the scratch tree and demand that it FAILS - a compile error, a failing check, a crash or a hang cut off by the
# 60 s alarm all count. A plant the suite lets through is a hole: the script exits non-zero and says which.
#
# Why this suite needs plants in particular: sections A-D are behavioural, and the census clamp makes num_instances and the loop bound agree at
# 2 for every die a host can forge - so the one break that matters most, a per-instance loop reverted to `kSDMAInstanceCount`, is invisible to
# behaviour and only section E's text pins catch it. Plants 18 and 19 exist to prove that, one per occurrence, and they are the reason the pin
# section is not decoration.
#
# Every needle here is a single line with no newline in it. The working tree is checked out CRLF on Windows and LF on a Mac; a multi-line needle
# would match on one and not the other. `nEngines` appears five times in sdma_init_full, so those plants pass an occurrence index as $6.
#
# Nothing here links src/fw/*.c: the census is arithmetic over the IP discovery table, so the suite compiles three translation units
# (sdma_v7_0.cpp, amdgpu_discovery.cpp, ipdiscovery.cpp) and no generated firmware arrays at all. That makes this the fastest plant run in the
# tree - and it means a plant in a firmware table would be pointless rather than merely slow.
#
#   run from anywhere:  src/navi48-bringup/tests/sdma_census_plant.sh [first-plant-id last-plant-id]
#   on a Mac:           (needs nothing but clang++ and perl)
#   on Windows/Linux:   ZIG=/path/to/zig.exe src/navi48-bringup/tests/sdma_census_plant.sh
#
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
K=src/navi48-bringup
SR=$K/src
SCR="${TMPDIR:-/tmp}/sdmacensus-plant.$$"
mkdir -p "$SCR" || { echo "cannot make $SCR"; exit 2; }
trap 'rm -rf "$SCR"' EXIT
total=0; escaped=0
FIRST=${1:-0}; LAST=${2:-9999}

if [ -n "${ZIG:-}" ]; then CXXW=("$ZIG" c++); else CXXW=(clang++); fi
if command -v timeout >/dev/null 2>&1; then TIMEOUT=(timeout 60); else TIMEOUT=(perl -e 'alarm 60; exec @ARGV'); fi

reset_tree() {
    rm -rf "$SCR/$SR" "$SCR/$K/tests"
    mkdir -p "$SCR/$K/tests" || return 1
    # The whole src tree: the test reads sdma_v7_0.cpp, amdgpu_init.cpp, amdgpu_ip.h,
    # amdgpu_sdma.h and Navi48Bringup.cpp for its pin section, and every one of those
    # is also a plant target, so a partial copy would hide a plant from the suite.
    cp -R "$ROOT/$SR" "$SCR/$SR" || return 1
    cp -R "$ROOT/$K/tests/dalshim" "$SCR/$K/tests/" || return 1
    cp "$ROOT/$K/tests/sdma_census_test.cpp" "$SCR/$K/tests/" || return 1
    cp "$ROOT/$K/tests/sdma_census_hostshim.h" "$SCR/$K/tests/" || return 1
}

compile() {   # compile <out binary> - the same line the header of sdma_census_test.cpp documents
    ( cd "$SCR" && "${CXXW[@]}" -std=c++17 -Wall -Wextra -O1 -include $K/tests/sdma_census_hostshim.h \
        -I $K/tests/dalshim -I $SR -I $SR/amd -I $SR/fw \
        $K/tests/sdma_census_test.cpp $SR/amd/amdgpu_discovery.cpp $SR/amd/sdma_v7_0.cpp $SR/ipdiscovery.cpp \
        -o "$1" 2>&1 )
}

# A plant must only ever touch the scratch copy. The checksum guard is what makes a
# mistake like editing the real tree loud instead of silent.
pristine_sum() { ( cd "$ROOT" && { find $SR -type f ! -path '*/fw/*'; find $K/tests/dalshim -type f; echo $K/tests/sdma_census_test.cpp; echo $K/tests/sdma_census_hostshim.h; } | sort | xargs cksum ) | sort; }
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
  # index()/substr() keep both the search and the replacement literal: no regex, no $1 interpolation.
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
  out=$( cd "$SCR" && "${TIMEOUT[@]}" ./t $K 2>&1 ); local trc=$?
  if [ $trc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED (the suite passed) ***"; escaped=$((escaped+1)); return; fi
  if [ $trc -ge 128 ] || [ $trc -eq 124 ]; then echo "PLANT $id: $desc: CAUGHT by a crash or hang (status $trc)"; return; fi
  echo "PLANT $id: $desc: CAUGHT by $(echo "$out" | grep -c '^FAIL') check(s); first: $(echo "$out" | grep -m1 '^FAIL' | cut -c1-120)"
}

control() {
  reset_tree || { echo "CONTROL: could not stage the scratch tree"; exit 2; }
  local out
  out=$(compile "$SCR/t") || { echo "CONTROL: the unplanted tree does not compile: $out"; exit 2; }
  out=$( cd "$SCR" && "${TIMEOUT[@]}" ./t $K 2>&1 ) || { echo "CONTROL: the unplanted tree does not pass: $(echo "$out" | grep '^FAIL' | head -3)"; exit 2; }
  echo "CONTROL: the unplanted copy compiles and passes ($(echo "$out" | tail -1))"
}

D=$SR/amd/amdgpu_discovery.cpp
S=$SR/amd/sdma_v7_0.cpp
H=$SR/amd/amdgpu_sdma.h
I=$SR/amd/amdgpu_init.cpp
P=$SR/amd/amdgpu_ip.h
B=$SR/Navi48Bringup.cpp

echo "== 0011 SDMA census: planted breaks =="
control
check_pristine || exit 2

# ---- the census arithmetic (amdgpu_discovery.cpp) ----
plant 0 "$D" 'hwId != kHwSdma1 &&' ''                'SDMA1_HWID 43 no longer counts as an engine'
plant 1 "$D" 'hwId != kHwSdma2 && ' ''               'SDMA2_HWID 68 no longer counts as an engine'
plant 2 "$D" 'hwId != kHwSdma3)' 'hwId == kHwSdma3)' 'SDMA3_HWID 69 inverted: it alone is skipped'
plant 3 "$D" 'if (c.count >= kSDMAMaxInstances) { c.overflow++; return; }' \
                     'if (c.count >= kSDMAMaxInstances) { c.overflow++; }' 'the array ceiling stops refusing entries'
plant 4 "$D" 'if (instance < 32) c.mask |= (1u << instance);' 'c.mask |= (1u << instance);' \
                     'the shift is unguarded again, like upstream'
plant 5 "$D" 'c.mask |= (1u << instance);' 'c.mask = (1u << instance);' 'the mask is assigned, so only the last engine survives'
plant 6 "$D" 'out = SdmaCensus();' '(void)0;'        'a stale census survives a fresh call'
plant 7 "$D" 'if (!disc.isValid()) return false;' 'if (disc.isValid()) return false;' 'the valid/invalid answer is inverted'
plant 8 "$D" 'if (!disc.ipAt(i, e)) break;' 'if (!disc.ipAt(i, e)) return false;' 'a truncated walk reports no answer instead of a partial one'
plant 9 "$D" 'for (uint32_t i = 0; i < n; i++)' 'for (uint32_t i = 0; n && i < n - 1; i++)' 'from_entries skips the final pair'
plant 10 "$D" 'sdma_census_add(c, entries[i].hwId, entries[i].instance);' 'sdma_census_add(c, entries[i].hwId, 0);' \
                     'the mask is built from the loop, not the instance number'
plant 11 "$D" 'kHwSdma2 = 68, kHwSdma3 = 69;' 'kHwSdma2 = 43, kHwSdma3 = 42;' 'the two extra hw_ids are aliases of 42/43'

# ---- the state the ladder sees (sdma_v7_0.cpp, amdgpu_sdma.h) ----
plant 12 "$H" 'constexpr uint32_t kSDMAMaxInstances      = 16;' 'constexpr uint32_t kSDMAMaxInstances      = 8;' \
                     'the ceiling is no longer AMDGPU_MAX_SDMA_INSTANCES'
plant 13 "$H" 'return s.num_instances < kSDMAInstanceCount ? s.num_instances : kSDMAInstanceCount;' 'return s.num_instances;' \
                     'the addressable count is the census, clamp and all'
plant 14 "$S" 'sdma.sdma_mask = c.mask;' 'sdma.sdma_mask = c.count;' 'the mask reports a count'
plant 15 "$S" 'sdma.census_from_discovery = true;' 'sdma.census_from_discovery = false;' 'the census never claims to have happened'
plant 16 "$S" 'sdma.census_overflow = c.overflow;' 'sdma.census_overflow = 0;' 'the overflow is dropped on the floor'
plant 17 "$S" 'for (uint32_t i = c.count; i < kSDMAMaxInstances; i++)' 'for (uint32_t i = c.count; i < 0; i++)' \
                     'instances past the census keep whatever presence they had'
plant 18 "$S" 'for (uint32_t i = 0; i < nEngines; i++) {' 'for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {' \
                     'the FIRST init_full loop bounded by the constant again' 1
plant 19 "$S" 'for (uint32_t i = 0; i < nEngines; i++) {' 'for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {' \
                     'the LAST init_full loop bounded by the constant again' 5
plant 20 "$S" 'if (nEngines == 0) {' 'if (false) {' 'a zero census proceeds to program an engine anyway'
plant 21 "$S" 'if (!sdma.census_from_discovery) {' 'if (sdma.census_from_discovery) {' 'the fallback fires exactly when a census exists'
plant 22 "$S" 'if (c.count > kSDMAInstanceCount)' 'if (false)' 'the un-brought-up engine is no longer named'
plant 23 "$S" 'if (c.count && (c.mask & 1u) == 0)' 'if (false)' 'the numbering mismatch is no longer warned about'

# ---- the wiring (Navi48Bringup.cpp, amdgpu_init.cpp) ----
plant 24 "$B" 'amdgpu::sdma_apply_census(ctx.sdma, cens);' '(void)cens;' 'the ladder never learns the census'
plant 25 "$B" 'setProperty("SDMA,Count", v);' '(void)v;' 'the survey stops publishing SDMA,Count'
plant 26 "$B" 'setProperty("SDMA,HwidMask", v);' '(void)v;' 'the survey stops publishing SDMA,HwidMask'
plant 27 "$I" 'for (uint32_t i = 0; i < kSDMAMaxInstances; i++)' 'for (uint32_t i = 0; i < kSDMAInstanceCount; i++)' \
                     'the release loop shrinks to the addressable window'
plant 28 "$I" 'if (sl.instance >= kSDMAInstanceCount ||' 'if (sl.instance >= kSDMAMaxInstances ||' \
                     'the qn-test bound widens past the registers that exist'

# ---- the doorbell literals the suite pins against the fetched upstream header ----
plant 29 "$P" 'uint32_t gfx_ring0           = 0x08B;' 'uint32_t gfx_ring0           = 0x08A;' 'GFX ring0 moves off AMDGPU_NAVI10_DOORBELL_GFX_RING0'
plant 30 "$P" '{0x100, 0x10A, 0x114, 0x11E}' '{0x100, 0x10A, 0x114, 0x120}' 'sDMA ENGINE3 moves off the Navi 10 map'
plant 31 "$P" 'uint32_t ih                  = 0x178;' 'uint32_t ih                  = 0x179;' 'the IH doorbell moves off AMDGPU_NAVI10_DOORBELL_IH'
plant 32 "$P" 'db_start_dw_offset + i * 2' 'db_start_dw_offset' 'the citation for how upstream derives aggregated doorbells is gutted'

echo
echo "plants run: $total   escaped: $escaped"
check_pristine || exit 2
[ $escaped -eq 0 ] || { echo "RESULT: the suite has holes - $escaped plant(s) passed"; exit 1; }
echo "RESULT: every planted break was caught"
