#!/usr/bin/env bash
#
#  navi44_arm_gate_plant.sh — prove the 0012 suite can actually fail.
#
#  Usage (from anywhere):  ZIG=/path/to/zig.exe bash tests/navi44_arm_gate_plant.sh
#
#  One break at a time, planted into a COPY of the real sources, and the suite must catch
#  it. A plant the suite lets through is a hole in the suite, not a pass for the driver -
#  which for this patch is the whole point: section A executes a pure function and section
#  C reads text, so the only thing that proves the two are wired to the same reality is a
#  mutated kext source that makes a pin fail.
#
#  Rules this script follows (same as the other *_plant.sh in this directory):
#    * needles are SINGLE-LINE, because the checkout is CRLF and a multi-line needle
#      never matches;
#    * every needle must be found, or the run aborts - a drifted needle silently
#      becomes "no plant applied", which reads exactly like a passing suite;
#    * the pristine tree's checksum is taken up front and re-checked at the end, so a
#      bug in this script cannot leave mutated sources behind;
#    * `control` runs the untouched copy first: if that does not pass, the copy step is
#      broken and no plant result means anything.
#
set -u

ZIG="${ZIG:?set ZIG to the zig (or zig c++) driver to use}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SCR="$(mktemp -d)"
mkdir -p "$SCR" || { echo "cannot make $SCR"; exit 2; }
trap 'rm -rf "$SCR"' EXIT

cp -R "$ROOT/src" "$SCR/src" || { echo "copy of src failed"; exit 2; }
cp -R "$ROOT/shaders" "$SCR/shaders" || { echo "copy of shaders failed"; exit 2; }
cp -R "$ROOT/tests/dalshim" "$SCR/dalshim" 2>/dev/null

PRISTINE=$(find "$SCR/src" "$SCR/shaders" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.s' \) \
    | sort | xargs cat | cksum | awk '{print $1}')

build_run() {
	# "$ZIG" stays quoted: on this workstation the toolchain path contains spaces, and a
	# word-split $ZIG fails with "F:\Work: No such file or directory" - which a plant run
	# reports as a build failure, indistinguishable from a broken source tree.
	"$ZIG" c++ -std=c++17 -O1 -I "$SCR/dalshim" -I "$SCR/src" \
		"$HERE/navi44_arm_gate_test.cpp" -o "$SCR/t" >"$SCR/build.log" 2>&1 || {
		echo "    BUILD FAILED"; sed -n '1,6p' "$SCR/build.log"; return 2; }
	"$SCR/t" "$SCR" >"$SCR/run.log" 2>&1
	return $?
}

# plant FILE NEEDLE REPLACEMENT [OCCURRENCE]
#
# Counting is done with N="..." perl ... FILE, never with `perl -sne '...' N=...`:
# without a preceding --, perl treats N=<needle> as another INPUT FILE, so $ENV{N} is empty,
# the regex matches at every position, and both the "needle exists" and the "plant landed"
# guards report success no matter what the tree contains. That turns the whole harness into
# a machine that prints "caught" while planting nothing.
count_matches() { # file needle -> echoes the number of occurrences
	N="$2" perl -0777 -ne 'my $c = () = $_ =~ /\Q$ENV{N}\E/g; print $c' "$1"
}

plant() {
	local file="$1" needle="$2" repl="$3" occ="${4:-1}"
	local path="$SCR/$file"
	cp "$ROOT/$file" "$path"                       # always plant from pristine
	local found
	found=$(count_matches "$path" "$needle")
	if [ "${found:-0}" -lt "$occ" ]; then
		echo "    NEEDLE MISSING (occ $occ of $found): $needle"
		return 9
	fi
	N="$needle" R="$repl" O="$occ" perl -0777 -i -pe '
		my $n = $ENV{N}; my $r = $ENV{R}; my $want = $ENV{O}; my $i = 0;
		s/\Q$n\E/$i++; ($i == $want) ? $r : $n/ge;
	' "$path"
	# confirm the plant landed, or this run is a false "caught"
	local after afterRepl
	after=$(count_matches "$path" "$needle")
	afterRepl=$(count_matches "$path" "$repl")
	if [ "$after" -ge "$occ" ] && [ "${afterRepl:-0}" -eq 0 ]; then
		echo "    PLANT DID NOT LAND"
		return 9
	fi
	return 0
}

restore_all() { rm -rf "$SCR/src" "$SCR/shaders"; cp -R "$ROOT/src" "$SCR/src"; cp -R "$ROOT/shaders" "$SCR/shaders"; }

H=src/navi44_arm_gate.h
C=src/Navi48Bringup.cpp
P=src/Navi48Bringup.hpp
R=src/amd/rlc_v12_0.cpp
X=src/amd/cp_v12_0.cpp

declare -a PASS FAIL MISS
PASSN=0 FAILN=0 MISSN=0
run_plant() { # LABEL FILE NEEDLE REPL [OCC]
	local label="$1"; shift
	printf '  %-46s' "$label"
	if ! plant "$@"; then echo "PLANT-ERROR"; MISS+=("$label"); MISSN=$((MISSN + 1)); restore_all; return; fi
	build_run
	local rc=$?
	if [ "$rc" -eq 0 ]; then echo "ESCAPED (suite still passed)"; FAIL+=("$label"); FAILN=$((FAILN + 1));
	else echo "caught (rc=$rc)"; PASS+=("$label"); PASSN=$((PASSN + 1)); fi
	restore_all
}

echo "== control: the untouched copy =="
if build_run; then echo "  control PASSED - plants below are meaningful"
else echo "  control FAILED - the copy/build step is broken, stopping"; exit 1; fi

echo "== plants =="
# --- the gate's own decisions (section A executes these) ---
run_plant "gate: non-Navi44 refuses"           "$H" 'if (!f.matchedNavi44) return verdict(Navi44Arm::Allow,' 'if (!f.matchedNavi44) return verdict(Navi44Arm::Refuse,'
run_plant "gate: stage-0 becomes Allow"        "$H" 'if (f.requestedStage == 0) return verdict(Navi44Arm::NothingAsked,' 'if (f.requestedStage == 0) return verdict(Navi44Arm::Allow,'
run_plant "gate: no-discovery check dropped"   "$H" 'if (!f.discoveryValid && !f.forceOverride)' 'if (false && !f.forceOverride)'
run_plant "gate: GC-version check dropped"     "$H" 'if (f.gcVersionKnown && !(f.gcMajor == 12 && f.gcMinor == 0 && f.gcRev == 0))' 'if (false)'
run_plant "gate: override arms a wrong GC"     "$H" '!(f.gcMajor == 12 && f.gcMinor == 0 && f.gcRev == 0))' '!(f.gcMajor == 12 && f.gcMinor == 0 && f.gcRev == 0) && !f.forceOverride)'
run_plant "gate: fw-set check dropped"         "$H" 'if (!f.fwSetFromDiscovery && !f.forceOverride)' 'if (false)'
run_plant "gate: missing-blob check dropped"   "$H" 'if (!f.sosBlobPresent)' 'if (false)'
run_plant "gate: allow text changed"           "$H" 'return verdict(Navi44Arm::Allow, "allowed");' 'return verdict(Navi44Arm::Allow, "ok");'
run_plant "gate: passthrough text changed"     "$H" '"not Navi 44 - existing behaviour"' '"passthrough"'
run_plant "gate: refusal text duplicated"      "$H" '"no PSP SOS blob embedded for the selected firmware set - nothing to load"' '"allowed"'

# --- 0016: the stage-17 compute blob on a die it was not assembled for (section D) ---
run_plant "gate: compute-blob check dropped"   "$H" 'if (f.requestedStage >= kNavi44ComputeStage && f.computeBlobBytes == 0)' 'if (false)'
run_plant "gate: stage-17 threshold moved"     "$H" 'constexpr uint32_t kNavi44ComputeStage = 17;' 'constexpr uint32_t kNavi44ComputeStage = 16;'
run_plant "gate: ISA note lost its target"     "$H" '"gfx1201-assembled blob runs on GC 12.0.0: every opcode in both store_magic "' '"the blob runs on this die: "'
run_plant "kext: blob size faked"              "$C" 'hsaAbi ? amdgpu::fw_shader_store_magic_hsa_size : amdgpu::fw_shader_store_magic_size' '32u : 32u'
run_plant "kext: ABI boot-arg renamed"         "$C" 'PE_parse_boot_argn("navi48-hsa-abi", &hsaAbi, sizeof(hsaAbi))' 'PE_parse_boot_argn("navi48-hsa", &hsaAbi, sizeof(hsaAbi))'
run_plant "kext: ISA note not published"       "$C" 'setProperty("Navi44,ComputeIsaNote", n48::navi44ComputeIsaNote());' 'setProperty("Navi44,ComputeNote", n48::navi44ComputeIsaNote());'
run_plant "shader log drops the die"           "src/amd/compute_test.cpp" '"s[0:1] = %s; assembled for gfx1201, running on GC %u.%u.%u",' '"s[0:1] = %s",'
# The census whitelist is the check that can actually fail on a real future mistake: an
# LLVM bump that starts emitting a gfx12_1-class opcode into store_magic.s.
run_plant "shaders: non-base opcode planted"   "shaders/store_magic.s" '	s_endpgm' '	v_add_u32_e64 v2, v2, s[0:1]'

# --- the kext's use of it (section C pins these) ---
run_plant "kext: label not fed to the gate"    "$C" 'facts.matchedNavi44      = navi44ByPciId;' 'facts.matchedNavi44      = false;'
run_plant "kext: discovery validity faked"     "$C" 'facts.discoveryValid     = ipDiscovery.isValid();' 'facts.discoveryValid     = true;'
run_plant "kext: GC never marked known"        "$C" 'facts.gcVersionKnown = true;' 'facts.gcVersionKnown = false;'
run_plant "kext: GC read elsewhere"            "$C" 'ipDiscovery.findIp(IpDiscovery::HwGc, 0, gcEntry)' 'ipDiscovery.findIp(IpDiscovery::HwMmhub, 0, gcEntry)'
run_plant "kext: blob check vacuous"           "$C" 'facts.sosBlobPresent = sosProbe && sosProbe->data && sosProbe->size != 0;' 'facts.sosBlobPresent = true;'
run_plant "kext: override boot-arg renamed"    "$C" 'PE_parse_boot_argn("navi44-force", &force, sizeof(force))' 'PE_parse_boot_argn("navi44-arm", &force, sizeof(force))'
run_plant "kext: gate not consulted"           "$C" 'const n48::Navi44Arm verdict = n48::navi44_arm_gate(facts, &why);' 'const n48::Navi44Arm verdict = n48::Navi44Arm::Allow;'
run_plant "kext: refusal stage string changed" "$C" '"navi44-gate-refused"' '"gate-refused"'
run_plant "kext: reason property renamed"      "$C" 'setProperty("PSP,GateReason", why);' 'setProperty("PSP,GateWhy", why);' 2
run_plant "kext: probe stops labelling 7590"   "$C" 'navi44ByPciId = true;' 'navi44ByPciId = false;'
run_plant "kext: header include broken"        "$C" '#include "navi44_arm_gate.h"' '#include "navi44_gate.h"'
run_plant "kext: old arms-NOTHING claim back"  "$C" '"boot-arg gated (navi48-stage=N, decided by navi44_arm_gate)"' '"SURVEY ONLY, no writing stage armed"'
run_plant "kext: gate runs after BAR0 work"    "$C" '	setProperty("PSP,Stage", "requested");' '	setProperty("PSP,Stage", "requested");
	if (mapVramAperture()) { /* moved above the gate by a plant */ }'
run_plant "fw-select: records false"           "$C" 'fwSetConfirmed = true;   // 0012' 'fwSetConfirmed = false;   // 0012'
run_plant "hpp: label defaults to on"          "$P" 'bool               navi44ByPciId { false };' 'bool               navi44ByPciId { true };'
run_plant "hpp: confirm defaults to on"        "$P" 'bool               fwSetConfirmed { false };' 'bool               fwSetConfirmed { true };'

# --- the clear-state table (section B re-derives 75 from it) ---
run_plant "csb: cluster count 6 -> 7"          "$R" '0x0000a0db,  6' '0x0000a0db,  7'
run_plant "csb: cluster count 11 -> 1"         "$R" '0x0000a2e5, 11' '0x0000a2e5,  1'
run_plant "csb: register base off by one"      "$R" '0x0000a03e, 34' '0x0000a03f, 34'
run_plant "csb: size computed from other die"  "$R" 'rlc_get_csb_size(kGfx12_cs_data)' 'rlc_get_csb_size(kGfx11_cs_data)'
run_plant "cs: table named wrongly again"      "$X" 'clearstate_gfx12.h' 'clearstate_gfx11.h'

echo "== summary =="
# Counts are plain integers: `${#ARR[@]}` on an empty array is an unbound-variable fatal
# under `set -u` in bash 3.2, which would kill the run right where the tally prints.
echo "caught:   $PASSN"
echo "escaped:  $FAILN  ${FAIL[*]:-}"
echo "plant-errors: $MISSN  ${MISS[*]:-}"

AFTER=$(find "$SCR/src" "$SCR/shaders" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.s' \) \
    | sort | xargs cat | cksum | awk '{print $1}')
if [ "$AFTER" != "$PRISTINE" ]; then
	echo "PRISTINE CHECKSUM CHANGED - the copy is polluted; investigate before trusting any result"
	exit 3
fi
echo "pristine tree unchanged after the run (checksum $PRISTINE)"

[ "$FAILN" -eq 0 ] && [ "$MISSN" -eq 0 ]
