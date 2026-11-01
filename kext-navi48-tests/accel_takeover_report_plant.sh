#!/usr/bin/env bash
#
#  accel_takeover_report_plant.sh — prove the 0020 suite can actually fail.
#
#  Usage (from anywhere):  ZIG=/path/to/zig.exe bash tests/accel_takeover_report_plant.sh
#
#  One break at a time, planted into a COPY of the real sources, and the suite must catch it.
#  This block answers the one question 0019 cannot: whether Apple's accelerator took the card
#  after LoadAccelerator went up. A wrong answer here is worse than no answer — it either
#  accuses the personality for the gate's refusal, or reports a takeover that never happened.
#
#  Rules (same as the other *_plant.sh here):
#    * needles are SINGLE-LINE, because the checkout is CRLF and a multi-line needle never
#      matches;
#    * every needle must be found, or the run aborts - a drifted needle silently becomes
#      "no plant applied", which reads exactly like a passing suite;
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
	# "$ZIG" stays quoted: the toolchain path contains spaces on this workstation.
	"$ZIG" c++ -std=c++17 -O1 -I "$SCR/dalshim" -I "$SCR/src" \
		"$HERE/accel_takeover_report_test.cpp" -o "$SCR/t" >"$SCR/build.log" 2>&1 || {
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

H=src/accel_takeover_report.h
R=src/accel_takeover.cpp
C=src/Navi48Bringup.cpp
P=src/apple/Navi48AccelPeer.cpp

declare -a PASS FAIL MISS
PASSN=0 FAILN=0 MISSN=0
run_plant() { # LABEL FILE NEEDLE REPL [OCC]
	local label="$1"; shift
	printf '  %-56s' "$label"
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

echo "== plants: when a takeover may be reported at all =="
run_plant "armed: a closed gate is still watched"    "$H" 'return reportRequested && published;' 'return reportRequested;'
run_plant "armed: always on"                         "$H" 'return reportRequested && published;' 'return true;'
run_plant "verdict: the armed guard is dropped"      "$H" 'if (!armed) return TakeoverVerdict::NotArmed;' 'if (false) return TakeoverVerdict::NotArmed;'
run_plant "verdict: the two outcomes are swapped"    "$H" 'return appleStarted ? TakeoverVerdict::Started : TakeoverVerdict::TimedOut;' 'return appleStarted ? TakeoverVerdict::TimedOut : TakeoverVerdict::Started;'
run_plant "wait: 20 s becomes 2 s"                   "$H" 'constexpr uint32_t kTakeoverWaitSeconds = 20;' 'constexpr uint32_t kTakeoverWaitSeconds = 2;'
run_plant "wait: 20 s becomes a very long wait"      "$H" 'constexpr uint32_t kTakeoverWaitSeconds = 20;' 'constexpr uint32_t kTakeoverWaitSeconds = 1000;'
run_plant "hold: 5 becomes one flash"                "$H" 'constexpr uint32_t kTakeoverHold        = 5;' 'constexpr uint32_t kTakeoverHold        = 1;'
run_plant "hold: 5 becomes forty"                    "$H" 'constexpr uint32_t kTakeoverHold        = 5;' 'constexpr uint32_t kTakeoverHold        = 40;'

echo "== plants: the headline an owner photographs =="
run_plant "headline: started loses the handover"     "$H" 'return "TAKEOVER: Apple'"'"'s accelerator started - the handover happened";' 'return "TAKEOVER: Apple'"'"'s accelerator started";'
run_plant "headline: timeout loses the personality"  "$H" '"announced itself - the line below names the reason";' '"announced itself";'
run_plant "headline: timeout loses never-started"    "$H" '"announced itself - the line below names the reason";' '"came back with the wrong device id";'
run_plant "headline: not-armed reads as the enum"    "$H" 'return "takeover: not tracked - the report was not requested, or nothing was published";' 'return "TakeoverVerdict::NotArmed";'
run_plant "headline: an empty screen"                "$H" 'return "TAKEOVER: Apple'"'"'s accelerator started - the handover happened";' 'return "";'
run_plant "headline: TimedOut case no longer handled" "$H" 'case TakeoverVerdict::TimedOut:' 'case TakeoverVerdict::TimedOut :'

echo "== plants: header hygiene (the decision stays pure) =="
run_plant "header: IOKit leaks in"                   "$H" '//  gate'"'"'s decision. accel_takeover_armed() is that rule, and it is the one decision here that' '//  gate'"'"'s decision (IOKit is not available here). accel_takeover_armed() is that rule, and it is the one decision here that'
run_plant "header: IOSleep leaks in"                 "$H" '//  can produce a wrong verdict on its own.' '//  can produce a wrong verdict on its own. IOSleep is not.'
run_plant "header: setProperty leaks in"             "$H" '//    positive — GraphicsAccelerator::start() announces itself over SpecialAMDKey with type 0x00' '//    positive — setProperty is published, then GraphicsAccelerator::start() announces itself with type 0x00'

echo "== plants: the positive signal (it must be recorded where Apple sends it) =="
run_plant "peer: header not included"                "$P" '#include "accel_takeover_report.h"' '#include "accel_takeover_report_gone.h"'
run_plant "peer: the mark is dropped"                "$P" 'n48::takeover_mark_started(p2);' ';'
run_plant "peer: the mark gets the wrong object"     "$P" 'n48::takeover_mark_started(p2);' 'n48::takeover_mark_started(nullptr);'

echo "== plants: the negative signal (one thread, one block, stands down on success) =="
run_plant "rt: the thread is never started"          "$R" 'kernel_thread_start(&takeover_poller' 'kernel_thread_start(&takeover_poller_gone'
run_plant "rt: the thread handle is leaked"          "$R" 'thread_deallocate(th)' 'th = nullptr'
run_plant "rt: the wait ignores the positive signal" "$R" 'if (__atomic_load_n(&gStarted, __ATOMIC_ACQUIRE)) break;' 'if (false) break;'
run_plant "rt: the wait is not the tested constant"  "$R" 'while (waited < n48::kTakeoverWaitSeconds) {' 'while (waited < 5) {'
run_plant "rt: the first sleep is gone"              "$R" 'IOSleep(1000);' 'IOSleep(0);' 1
run_plant "rt: the wait-loop sleep is gone"          "$R" 'IOSleep(1000);' 'IOSleep(0);' 2
run_plant "rt: the verdict ignores the signal"       "$R" 'n48::accel_takeover_verdict(__atomic_load_n(&gArmed, __ATOMIC_ACQUIRE) != 0,' 'n48::accel_takeover_verdict(true,'
run_plant "rt: the started flag is never set"        "$R" '__atomic_store_n(&gStarted, 1u, __ATOMIC_RELEASE);' '__atomic_store_n(&gStarted, 0u, __ATOMIC_RELEASE);'
run_plant "rt: the mark sleeps on Apple's thread"    "$R" '__atomic_store_n(&gStarted, 1u, __ATOMIC_RELEASE);' '__atomic_store_n(&gStarted, 1u, __ATOMIC_RELEASE); IOSleep(500);'
run_plant "rt: the arming never takes"               "$R" '__atomic_store_n(&gArmed, 1u, __ATOMIC_RELEASE);' '__atomic_store_n(&gArmed, 0u, __ATOMIC_RELEASE);'
run_plant "rt: the mark ignores arming"              "$R" 'if (__atomic_load_n(&gArmed, __ATOMIC_ACQUIRE)) {' 'if (false) {'
run_plant "rt: the block is never printed"           "$R" 'print_block(v, waited, facts);' ';'
run_plant "rt: the block runs one second long"       "$R" 'if (t >= n48::kTakeoverHold) break;' 'if (t > n48::kTakeoverHold) break;'
run_plant "rt: two blocks can print"                 "$R" '__atomic_compare_exchange_n(&gBlockPrinted' '__atomic_compare_exchange_n(&gBlockPrintedX'

echo "== plants: the TTL census (0023 - where Apple's driver went) =="
run_plant "rt: the census is never printed"          "$R" 'print_ttl_census();' ';'
run_plant "rt: the total is not read"                "$R" 'const uint32_t calls = gNavi48Ttl.totalCalls();' 'const uint32_t calls = 0;'
run_plant "rt: the first unsupported is hidden"      "$R" 'const int first = gNavi48Ttl.firstUnsupported();' 'const int first = -1;'
run_plant "rt: the hook binding is not reported"     "$R" 'gNavi48Ttl.bound() ? 1u : 0u' '1u'
run_plant "rt: the slot scan is dropped"             "$R" 'for (int s = 0; s < 43; s++) if (gNavi48Ttl.callCount(s) != 0) touched++;' 'for (int s = 0; s < 43; s++) if (gNavi48Ttl.callCount(s) != 0) { }'
run_plant "rt: the census line loses the first slot" "$R" 'ttl: bound=%u calls=%u slots=%u/43 first unsupported=%d' 'ttl: bound=%u calls=%u slots=%u/43'

echo "== plants: the unarmed announce (0025 - proof by itself) =="
run_plant "rt: the unarmed announce is silent"       "$R" 'TAKELOG("========== NAVI 44 TAKEOVER REPORT (no watcher armed) ==========");' ';'
run_plant "rt: the unarmed announce skips the census" "$R" 'print_ttl_census();' ';' 2
run_plant "rt: the unarmed announce misreads the verdict" "$R" 'n48::accel_takeover_headline(n48::TakeoverVerdict::Started)' 'n48::accel_takeover_headline(n48::TakeoverVerdict::TimedOut)'
run_plant "rt: the announce can still return unarmed" "$R" '__atomic_store_n(&gStarted, 1u, __ATOMIC_RELEASE);' 'if (!__atomic_load_n(&gArmed, __ATOMIC_ACQUIRE)) return; __atomic_store_n(&gStarted, 1u, __ATOMIC_ACQUIRE);'

echo "== plants: the xsrc wiring (armed only after a real publish) =="
run_plant "xsrc: header not included"                "$C" '#include "accel_takeover_report.h"' '#include "accel_takeover_report_gone.h"'
run_plant "xsrc: armed on any requested boot"        "$C" 'n48::accel_takeover_armed(ff.argRequested, fireCalled && fireKr == 0)' 'n48::accel_takeover_armed(ff.argRequested, true)'
run_plant "xsrc: armed on every boot"                "$C" 'n48::accel_takeover_armed(ff.argRequested, fireCalled && fireKr == 0)' 'n48::accel_takeover_armed(true, true)'
run_plant "xsrc: an errored fire is still watched"   "$C" 'n48::accel_takeover_armed(ff.argRequested, fireCalled && fireKr == 0)' 'n48::accel_takeover_armed(ff.argRequested, fireCalled)'
run_plant "xsrc: arming is dropped"                  "$C" 'n48::takeover_arm(pciDevice);' ';'
run_plant "xsrc: the poller is never started"        "$C" 'n48::takeover_start_poller();' ';'
run_plant "xsrc: the property is renamed"            "$C" 'setProperty("Navi48,AccelTakeover"' 'setProperty("Navi48,AccelTakeoverX"'

echo "== summary =="
# Plain integers: `${#ARR[@]}` on an empty array is an unbound-variable fatal under set -u.
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
