#!/usr/bin/env bash
#
#  accel_fire_at_boot_plant.sh — prove the 0017 suite can actually fail.
#
#  Usage (from anywhere):  ZIG=/path/to/zig.exe bash tests/accel_fire_at_boot_plant.sh
#
#  One break at a time, planted into a COPY of the real sources, and the suite must catch
#  it. This gate decides whether a boot hands the card to every Metal client on the machine
#  with nobody at a keyboard, so the plants are the whole review: five of them remove one
#  refusal each, and each removal has to make the suite go red.
#
#  Rules (same as the other *_plant.sh here):
#    * needles are SINGLE-LINE, because the checkout is CRLF and a multi-line needle
#      never matches;
#    * every needle must be found, or the run aborts - a drifted needle silently
#      becomes "no plant applied", which reads exactly like a passing suite;
#    * the pristine checksum is taken up front and re-checked at the end;
#    * `control` runs the untouched copy first.
#
set -u

ZIG="${ZIG:?set ZIG to the zig (or zig c++) driver to use}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SCR="$(mktemp -d)"
mkdir -p "$SCR" || { echo "cannot make $SCR"; exit 2; }
trap 'rm -rf "$SCR"' EXIT

cp -R "$ROOT/src" "$SCR/src" || { echo "copy of src failed"; exit 2; }
cp -R "$ROOT/tests/dalshim" "$SCR/dalshim" 2>/dev/null

PRISTINE=$(find "$SCR/src" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) \
    | sort | xargs cat | cksum | awk '{print $1}')

build_run() {
	# "$ZIG" stays quoted: on this workstation the toolchain path contains spaces, and a
	# word-split $ZIG fails with "D:\Work: No such file or directory" - which a plant run
	# reports as a build failure, indistinguishable from a broken source tree.
	"$ZIG" c++ -std=c++17 -O1 -I "$SCR/dalshim" -I "$SCR/src" \
		"$HERE/accel_fire_at_boot_test.cpp" -o "$SCR/t" >"$SCR/build.log" 2>&1 || {
		echo "    BUILD FAILED"; sed -n '1,6p' "$SCR/build.log"; return 2; }
	"$SCR/t" "$SCR" >"$SCR/run.log" 2>&1
	return $?
}

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
	# Counting is done with N="..." perl ... FILE, never with `perl -sne '...' N=...`:
	# without a preceding --, perl treats N=<needle> as another INPUT FILE, so $ENV{N} is
	# empty, the regex matches at every position, and both the "needle exists" and the
	# "plant landed" guards report success no matter what the tree contains.
	N="$needle" R="$repl" O="$occ" perl -0777 -i -pe '
		my $n = $ENV{N}; my $r = $ENV{R}; my $want = $ENV{O}; my $i = 0;
		s/\Q$n\E/$i++; ($i == $want) ? $r : $n/ge;
	' "$path"
	local after afterRepl
	after=$(count_matches "$path" "$needle")
	afterRepl=$(count_matches "$path" "$repl")
	if [ "$after" -ge "$occ" ] && [ "${afterRepl:-0}" -eq 0 ]; then
		echo "    PLANT DID NOT LAND"
		return 9
	fi
	return 0
}

restore_all() { rm -rf "$SCR/src"; cp -R "$ROOT/src" "$SCR/src"; }

H=src/accel_fire_at_boot.h
C=src/Navi48Bringup.cpp

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

echo "== plants: the pure gate (each one removes a refusal) =="
run_plant "gate: opt-in arg check dropped"     "$H" 'if (!f.argRequested)' 'if (false)'
run_plant "gate: arm check dropped"            "$H" 'if (!f.experimentArmed)' 'if (false)'
run_plant "gate: no-ladder check dropped"      "$H" 'if (!f.ladderRan)' 'if (false)'
run_plant "gate: self-test check dropped"      "$H" 'if (!f.selfTestPassed)' 'if (false)'
run_plant "gate: gate-0 check dropped"         "$H" 'if (f.gateMode == 0)' 'if (false)'
run_plant "gate: arm test reads the wrong fact" "$H" 'if (!f.experimentArmed)' 'if (!f.ladderRan)'
run_plant "gate: refusals all become skips"    "$H" 'return verdict(AccelFireAtBoot::Refuse,' 'return verdict(AccelFireAtBoot::SkipNotArmed,' 2
run_plant "gate: Call downgraded to skip"      "$H" 'return verdict(AccelFireAtBoot::Call, accelFireAtBootReason(AccelFireAtBoot::Call));' 'return verdict(AccelFireAtBoot::SkipNotRequested, "no");'

echo "== plants: the reason text an owner reads =="
run_plant "why: half-state loses the TTL word" "$H" 'the fire path installs the TTL "' 'the fire path installs the hook "'
run_plant "why: no diagnostic route offered"   "$H" '(use `navi48test accel fire` to probe anyway)' '(no alternative offered)'
run_plant "why: gate-0 stops naming the arg"   "$H" '"navi48-accel-gate=0 forbids' '"the gate forbids'
run_plant "why: ladder refusal loses the arg"  "$H" '"the bring-up ladder never ran this boot (navi48-stage too low, or the PSP chain "' '"the bring-up ladder never ran this boot (the chain "'
run_plant "why: accessor disagrees with Call"  "$H" 'case AccelFireAtBoot::Call:             return "allowed";' 'case AccelFireAtBoot::Call:             return "ok";'
run_plant "why: reasonOut gets the wrong text"  "$H" 'if (reasonOut) *reasonOut = r;' 'if (reasonOut) *reasonOut = "refused";'
run_plant "facts: default gate mode becomes 0" "$H" 'uint32_t gateMode        = 1;' 'uint32_t gateMode        = 0;'
# No "swap <cstdint> for an IOKit header" plant: that would stop the copy compiling, which the
# harness reports as a build failure rather than a caught break. Section E's dependency pin is
# already executed by the control run - the copy builds and runs on a machine with no IOKit.

echo "== plants: the kext is actually wired to the gate =="
run_plant "xsrc: header not included"          "$C" '#include "accel_fire_at_boot.h"' '#include "accel_fire_at_boot_off.h"'
run_plant "xsrc: boot-arg renamed"             "$C" '"navi48-accel-fire-at-boot"' '"navi48-accel-fire-at-biot"'
run_plant "xsrc: gate never consulted"         "$C" 'n48::accel_fire_at_boot_gate(ff, &fwhy)' 'n48::accel_fire_at_boot_gone(ff, &fwhy)'
run_plant "xsrc: verdict not published"        "$C" 'setProperty("Navi48,AccelFireAtBoot", fwhy);' 'setProperty("Navi48,AccelFireAtBootX", fwhy);'
run_plant "xsrc: fire return not published"    "$C" 'setProperty("Navi48,AccelFireAtBootKr"' 'setProperty("Navi48,AccelFireKr"'
run_plant "xsrc: wrong accel action sent"      "$C" 'accelExperiment(1 /*fire*/' 'accelExperiment(0 /*fire*/'
run_plant "xsrc: publish guard dropped"        "$C" 'if (ff.argRequested) setProperty("Navi48,AccelFireAtBoot", fwhy);' 'setProperty("Navi48,AccelFireAtBoot", fwhy);'
run_plant "xsrc: facts read PM4Test, not 17"   "$C" 'gBringup.reached >= amdgpu::BringupStage::ComputeDispatch;' 'gBringup.reached >= amdgpu::BringupStage::PM4Test;' 1
run_plant "xsrc: ladderRan hardcoded true"     "$C" 'ff.ladderRan       = gBringup.reached != amdgpu::BringupStage::None;' 'ff.ladderRan       = true;'
run_plant "xsrc: selfTest stops reading ladder" "$C" 'ff.selfTestPassed  = gBringup.computePassed &&' 'ff.selfTestPassed  = true &&'
run_plant "xsrc: the half-state claim edited"  "$C" 'The TTL hook stays installed' 'The TTL hook is rolled back'

echo "== summary =="
# Counts are plain integers: `${#ARR[@]}` on an empty array is an unbound-variable fatal
# under `set -u` in bash 3.2, which would kill the run right where the tally prints.
echo "caught:   $PASSN"
echo "escaped:  $FAILN  ${FAIL[*]:-}"
echo "plant-errors: $MISSN  ${MISS[*]:-}"

AFTER=$(find "$SCR/src" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) \
    | sort | xargs cat | cksum | awk '{print $1}')
if [ "$AFTER" != "$PRISTINE" ]; then
	echo "PRISTINE CHECKSUM CHANGED - the copy is polluted; investigate before trusting any result"
	exit 3
fi
echo "pristine tree unchanged after the run (checksum $PRISTINE)"

[ "$FAILN" -eq 0 ] && [ "$MISSN" -eq 0 ]
