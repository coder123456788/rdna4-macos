#!/usr/bin/env bash
#
#  accel_boot_report_plant.sh — prove the 0019 suite can actually fail.
#
#  Usage (from anywhere):  ZIG=/path/to/zig.exe bash tests/accel_boot_report_plant.sh
#
#  One break at a time, planted into a COPY of the real sources, and the suite must catch
#  it. This block is the whole report on a machine with no keyboard: a photograph of the
#  verbose boot is the only evidence that comes back. So the plants are the ways that
#  photograph can lie — a headline that disagrees with the gate, a hold that never holds,
#  a report that prints when nobody asked, or a report derived from the verdict alone while
#  the fire path actually errored.
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
		"$HERE/accel_boot_report_test.cpp" -o "$SCR/t" >"$SCR/build.log" 2>&1 || {
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

H=src/accel_boot_report.h
C=src/Navi48Bringup.cpp

declare -a PASS FAIL MISS
PASSN=0 FAILN=0 MISSN=0
run_plant() { # LABEL FILE NEEDLE REPL [OCC]
	local label="$1"; shift
	printf '  %-50s' "$label"
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

echo "== plants: the hold cap (a mistyped arg must not stall every boot) =="
run_plant "cap: 20 becomes 200"                "$H" 'constexpr uint32_t kAccelReportHoldCap = 20;' 'constexpr uint32_t kAccelReportHoldCap = 200;'
run_plant "cap: 20 becomes 60"                 "$H" 'constexpr uint32_t kAccelReportHoldCap = 20;' 'constexpr uint32_t kAccelReportHoldCap = 60;'
run_plant "cap: clamp ignored"                 "$H" 'return requested > kAccelReportHoldCap ? kAccelReportHoldCap : requested;' 'return requested;'
run_plant "cap: an extra occurrence of the name" "$H" 'constexpr uint32_t kAccelReportHoldCap = 20;' 'constexpr uint32_t kAccelReportHoldCap = 20;
constexpr uint32_t kAccelReportHoldAlias = kAccelReportHoldCap;'

echo "== plants: when the report prints =="
run_plant "mode: the fire-arg guard is dropped" "$H" 'if (!argRequested) return AccelBootReport::Off;' 'if (false) return AccelBootReport::Off;'
run_plant "mode: always Off"                    "$H" 'if (!argRequested) return AccelBootReport::Off;' 'return AccelBootReport::Off;'
run_plant "mode: a hold never holds"            "$H" 'return holdRequested ? AccelBootReport::PrintAndHold : AccelBootReport::Print;' 'return AccelBootReport::Print;'
run_plant "mode: every print holds"             "$H" 'return holdRequested ? AccelBootReport::PrintAndHold : AccelBootReport::Print;' 'return AccelBootReport::PrintAndHold;'

echo "== plants: the headline an owner photographs =="
run_plant "headline: not-requested loses the arg"  "$H" 'return "nothing was asked for: add navi48-accel-fire-at-boot=1 to boot-args";' 'return "nothing was asked for";'
run_plant "headline: not-armed loses the arg"      "$H" 'return "not fired: navi48-accel-experiment=1 is absent, so the experiment was never armed";' 'return "not fired";'
run_plant "headline: gate-0 branch taken at gate 1" "$H" 'if (gateMode == 0)' 'if (gateMode == 1)'
run_plant "headline: gate-0 sentence softened"     "$H" 'the card stays on the software path";' 'the card stays on the software mode";'
run_plant "headline: refusal reads as the enum"    "$H" 'return "REFUSED: the card was not handed to Apple this boot - the reason line below says why";' 'return "Refuse";'
run_plant "headline: did-not-run softened"         "$H" 'return "the gate said Call but the fire path did not run - nothing is hooked, investigate";' 'return "the fire path was skipped";'
run_plant "headline: error loses NOT installed"    "$H" 'return "the fire path returned an error - the TTL hook was NOT installed";' 'return "the fire path returned an error";'
run_plant "headline: gate-2 branch taken at gate 1" "$H" 'if (gateMode == 2)' 'if (gateMode == 1)'
run_plant "headline: forced-open sentence dropped" "$H" 'return "LoadAccelerator PUBLISHED (gate forced open by navi48-accel-gate=2)";' 'return "LoadAccelerator PUBLISHED";'
run_plant "headline: published sentence shortened" "$H" 'return "LoadAccelerator PUBLISHED - Apple'"'"'s accelerator may now match this card";' 'return "LoadAccelerator PUBLISHED";'
run_plant "headline: an empty screen"              "$H" 'return "nothing was asked for: add navi48-accel-fire-at-boot=1 to boot-args";' 'return "";'
run_plant "headline: Refuse case no longer handled" "$H" 'case AccelFireAtBoot::Refuse:' 'case AccelFireAtBoot::Refuse :'

echo "== plants: header hygiene (the gate stays the gate's) =="
run_plant "header: include pin drifts"          "$H" '#include "accel_fire_at_boot.h"' '#include "accel_fire_at_boot_gone.h"'
run_plant "header: IOKit leaks in"              "$H" '//  needs typing.' '//  needs typing (IOKit and IOLog( are not available here).'
run_plant "header: setProperty leaks in"        "$H" '//    * whether to print at all (only when someone asked the boot to fire), and' '//    * whether to print at all (only when someone asked the boot to fire), setProperty, and'
run_plant "header: the gate's self-test copied" "$H" '// The one line an owner photographs.' '// selfTestPassed is the gate'"'"'s; the one line an owner photographs.'

echo "== plants: the xsrc wiring (the block is actually printed, after the fire) =="
run_plant "xsrc: header not included"           "$C" '#include "accel_boot_report.h"' '#include "accel_boot_report_gone.h"'
run_plant "xsrc: the hold arg is renamed"       "$C" 'PE_parse_boot_argn("navi48-report-hold"' 'PE_parse_boot_argn("navi48-reporthold"'
run_plant "xsrc: a hold invents a report"       "$C" 'n48::accel_report_mode(ff.argRequested, holdRequested)' 'n48::accel_report_mode(true, holdRequested)'
run_plant "xsrc: print guard dropped"           "$C" 'if (rmode != n48::AccelBootReport::Off) {' 'if (true) {'
run_plant "xsrc: fired hardcoded true"          "$C" 'n48::accel_report_headline(fv, fireCalled, fireKr, ff.gateMode)' 'n48::accel_report_headline(fv, true, fireKr, ff.gateMode)'
run_plant "xsrc: the error code hardcoded 0"    "$C" 'n48::accel_report_headline(fv, fireCalled, fireKr, ff.gateMode)' 'n48::accel_report_headline(fv, fireCalled, 0, ff.gateMode)'
run_plant "xsrc: fireKr never recorded"         "$C" 'fireKr = (int32_t)fkr;' 'fireKr = 0;'
run_plant "xsrc: fireCalled never recorded"     "$C" 'fireCalled = true;' 'fireCalled = false;'
run_plant "xsrc: the hold never sleeps"         "$C" 'IOSleep(1000);' 'IOSleep(0);'
run_plant "xsrc: the hold bypasses the cap"     "$C" '? n48::accel_report_hold_seconds(holdRequested) : 0;' '? holdRequested : 0;'
run_plant "xsrc: pattern repeats only once"     "$C" 'for (uint32_t t = 0; ; t++) {' 'for (uint32_t t = 0; t < 1; t++) {'
run_plant "xsrc: the hold runs one second long" "$C" 'if (t >= secs) break;' 'if (t > secs) break;'
run_plant "xsrc: the headline line is not head" "$C" 'N48LOG("%s", head);' 'N48LOG("%s", "published");'
run_plant "xsrc: the report property renamed"   "$C" 'setProperty("Navi48,AccelReport", head);' 'setProperty("Navi48,AccelReportX", head);'
run_plant "xsrc: the hold property renamed"     "$C" 'setProperty("Navi48,AccelReportHold"' 'setProperty("Navi48,AccelReportHoldX"'
run_plant "xsrc: the banner renamed"            "$C" 'NAVI 44 ACCELERATION REPORT %u/%u' 'NAVI 44 ACCEL REPORT %u/%u'
run_plant "xsrc: the photograph hint dropped"   "$C" 'photograph this screen' 'the screen'
run_plant "xsrc: the reason line loses a field" "$C" 'ttl installed %llu; gate reason: %s' 'installed %llu; reason: %s'

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
