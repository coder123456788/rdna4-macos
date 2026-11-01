#!/usr/bin/env bash
#
#  accel_report_file_plant.sh — prove the 0027 suite can actually fail.
#
#  Usage (from anywhere):  ZIG=/path/to/zig.exe bash tests/accel_report_file_plant.sh
#
#  One break at a time, planted into a COPY of the real sources, and the suite must catch it.
#  This block writes the boot's verdict to the boot stick instead of the screen, so this file is
#  the only thing that comes back from a boot: a wrong line, a lost cause, or a writer that never
#  starts would send the next fix at the wrong layer. The suite is what makes that recoverable.
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
		"$HERE/accel_report_file_test.cpp" -o "$SCR/t" >"$SCR/build.log" 2>&1 || {
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

F=src/accel_report_file.h
R=src/accel_takeover.cpp
C=src/Navi48Bringup.cpp

declare -a PASS FAIL MISS
PASSN=0 FAILN=0 MISSN=0
run_plant() { # LABEL FILE NEEDLE REPL [OCC]
	local label="$1"; shift
	printf '  %-58s' "$label"
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

echo "== plants: the text a boot comes back as =="
run_plant "text: the banner drops no-photograph"     "$F" 'written by the kext; no photograph needed' 'written by the kext'
run_plant "text: a missing headline reads empty"     "$F" 'w.str(in.fireHeadline ? in.fireHeadline : "(not reported)");' 'w.str(in.fireHeadline ? in.fireHeadline : "");'
run_plant "text: armed timeout reads as STARTED"     "$F" '"NO TAKEOVER - no announce within the wait"' '"STARTED - Apple'"'"'s accelerator announced itself"'
run_plant "text: unarmed reads as NO TAKEOVER"       "$F" '"not tracked - no watcher was armed on this boot"' '"NO TAKEOVER - no announce within the wait"'
run_plant "text: a takeover carries a cause line"    "$F" 'if (!in.started) {' 'if (true) {'
run_plant "text: the cause line goes blank"          "$F" 'w.str(accel_takeover_diagnosis(f));' 'w.str("");'
run_plant "text: the wait is always zero"            "$F" 'w.dec(in.waited);' 'w.dec(0);'
run_plant "text: kext-loaded bit is inverted"        "$F" 'w.dec(in.appleKextLoaded ? 1u : 0u);' 'w.dec(in.appleKextLoaded ? 0u : 1u);'
run_plant "text: device and vendor swapped"          "$F" 'w.hex4(in.deviceId);' 'w.hex4(in.vendorId);'
run_plant "text: the ids print decimal"              "$F" 'w.hex4(in.deviceId);' 'w.dec(in.deviceId);'
run_plant "text: first unsupported loses its sign"   "$F" 'w.ch('"'"'-'"'"');' ';'
run_plant "text: slot scan stops at 42"              "$F" 'for (int s = 0; s < 43; s++) {' 'for (int s = 0; s < 42; s++) {'
run_plant "text: reached mask shifts by one"         "$F" 'if ((in.reachedMask >> s) & 1) {' 'if ((in.reachedMask >> (s + 1)) & 1) {'
run_plant "text: the buffer is never terminated"     "$F" 'void finish() { if (out && cap) out[len < cap ? len : cap - 1] = 0; }' 'void finish() { }'

echo "== plants: the write itself (VFS from our own thread) =="
run_plant "writer: the path leaves the stick"        "$R" 'vnode_open(n48::kReportFilePath' 'vnode_open("/tmp/navi48-report.txt"'
run_plant "writer: opened without write access"      "$R" 'kO_WRONLY | kO_CREAT' 'kO_CREAT'
run_plant "writer: the 8 KiB target is truncated"    "$R" 'kO_WRONLY | kO_CREAT' 'kO_WRONLY | kO_CREAT | kO_TRUNC'
run_plant "writer: the uio is a read"                "$R" 'vn_rdwr(kUIO_WRITE, vp, (char *)text' 'vn_rdwr(0, vp, (char *)text'
run_plant "writer: a user-space buffer"              "$R" '0, kUIO_SYSSPACE, 0,' '0, 0, 0,'
run_plant "writer: the vnode is leaked"              "$R" 'vnode_close(vp, kFWRITE, ctx);' ';'
run_plant "writer: the vfs context is leaked"        "$R" 'vfs_context_rele(ctx);' ';'
run_plant "writer: a short write passes as success"  "$R" 'if (err == 0 && resid != 0) err = kEIO;' ';'
run_plant "writer: our thread is never started"      "$R" 'kernel_thread_start(&report_file_thread, nullptr, &th)' 'kernel_thread_start(&report_file_thread_gone, nullptr, &th)'
run_plant "writer: two threads can start"            "$R" '__atomic_compare_exchange_n(&gWriterStarted' '__atomic_compare_exchange_n((uint32_t *)&gWriterStarted'
run_plant "writer: the headline is dropped"          "$R" '__atomic_store_n(&gFireHeadline, (uintptr_t)fireHeadline, __ATOMIC_RELEASE);' ';'
run_plant "writer: the headline is not gathered"     "$R" 'in.fireHeadline = (const char *)__atomic_load_n(&gFireHeadline, __ATOMIC_ACQUIRE);' 'in.fireHeadline = nullptr;'
run_plant "writer: the settle is two milliseconds"   "$R" 'IOSleep(n48::kReportFileSettleSeconds * 1000);' 'IOSleep(n48::kReportFileSettleSeconds);'
run_plant "writer: unarmed freezes on first write"   "$R" 'if (in.started || in.armed) return;' 'if (true) return;'
run_plant "writer: the device id is not gathered"    "$R" 'in.deviceId = injected_id(prov, "device-id")' 'in.deviceId = 0'
run_plant "writer: the slot mask is a single bit"    "$R" 'mask |= (1ull << s)' 'mask |= (1ull << 0)'
run_plant "header: the volume is misspelled"         "$F" '"/Volumes/OPENCORE/NAVI48.TXT"' '"/Volumes/OPENCOREX/NAVI48.TXT"'
run_plant "header: the retry window is one second"   "$F" 'kReportFileRetrySeconds = 120' 'kReportFileRetrySeconds = 1'

echo "== plants: the xsrc wiring (every requested boot writes) =="
run_plant "xsrc: the header is not included"         "$C" '#include "accel_report_file.h"' '#include "accel_report_file_gone.h"'
run_plant "xsrc: the writer is never started"        "$C" 'if (ff.argRequested) n48::takeover_start_writer(head, this);' ';'
run_plant "xsrc: only a fired boot writes"           "$C" 'if (ff.argRequested) n48::takeover_start_writer(head, this);' 'if (fireCalled) n48::takeover_start_writer(head, this);'

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
