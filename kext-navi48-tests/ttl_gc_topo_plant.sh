#!/usr/bin/env bash
#
#  ttl_gc_topo_plant.sh — prove the 0018 suite can actually fail.
#
#  Usage (from anywhere):  ZIG=/path/to/zig.exe bash tests/ttl_gc_topo_plant.sh
#
#  One break at a time, planted into a COPY of the real sources, and the suite must catch
#  it. This slot tells Apple how many shader engines, arrays and CUs exist; the break being
#  guarded against is the one the code shipped with until now — an unmeasured die answered
#  with Navi 48's 4x2x8 and a log warning. So half of these plants put a fallback back.
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
		"$HERE/ttl_gc_topo_test.cpp" -o "$SCR/t" >"$SCR/build.log" 2>&1 || {
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

H=src/apple/ttl_gc_topo.h
C=src/apple/Navi48Ttl.cpp

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
run_plant "gate: inited check dropped"        "$H" 'if (!f.inited) return verdict(TtlGcTopo::RefuseNotInited);' 'if (false) return verdict(TtlGcTopo::RefuseNotInited);'
run_plant "gate: zero-shape check dropped"    "$H" 'if (se == 0 || sa == 0 || cu < 2) return verdict(TtlGcTopo::RefuseNoShape);' 'if (false) return verdict(TtlGcTopo::RefuseNoShape);'
run_plant "gate: struct-width check dropped"  "$H" 'if (se > kTtlGcTopoStructRows || sa > kTtlGcTopoStructCols)' 'if (false)'
run_plant "gate: no-harvest-map check dropped" "$H" 'if (anyCus == 0) return verdict(TtlGcTopo::RefuseNoHarvestMap);' 'if (false) return verdict(TtlGcTopo::RefuseNoHarvestMap);'
run_plant "gate: every reason becomes ok"      "$H" 'if (reasonOut) *reasonOut = ttlGcTopoReason(v);' 'if (reasonOut) *reasonOut = "ok";'
run_plant "gate: SE comes from the struct"    "$H" 'a.shaderEngines = se;' 'a.shaderEngines = kTtlGcTopoStructRows;'
run_plant "gate: rb bitmap dropped"           "$H" 'a.rbBitmap      = f.globalRbBitmap;' 'a.rbBitmap      = 0;'
run_plant "gate: half pair not counted"       "$H" 'else if (lo || hi) { wgp |= (1u << bit); a.halfPopulatedWgps++; cus += 1; }' 'else if (lo || hi) { }'
run_plant "gate: one CU of a pair is enough"  "$H" 'if (lo && hi) { wgp |= (1u << bit); cus += 2; }' 'if (lo || hi) { wgp |= (1u << bit); cus += 2; }'
run_plant "gate: pair loop wraps past bit 31" "$H" 'for (uint32_t bit = 0; bit < 16; bit++)' 'for (uint32_t bit = 0; bit < 32; bit++)'
run_plant "gate: measured width never raises" "$H" 'if (widest > a.wgpsPerSh) a.wgpsPerSh = widest;' ''
run_plant "gate: CU total is the table math"  "$H" 'a.totalCus = cus;' 'a.totalCus = se * sa * cu;'
run_plant "gate: struct rows become 3"        "$H" 'constexpr uint32_t kTtlGcTopoStructRows = 4;' 'constexpr uint32_t kTtlGcTopoStructRows = 3;'
run_plant "gate: struct cols become 1"        "$H" 'constexpr uint32_t kTtlGcTopoStructCols = 2;' 'constexpr uint32_t kTtlGcTopoStructCols = 1;'
run_plant "facts: default inited becomes true" "$H" 'bool inited = false;' 'bool inited = true;'

echo "== plants: the reason text an owner reads =="
run_plant "why: answer text stops being a sentence" "$H" 'case TtlGcTopo::Answer:            return "answered from the measured die";' 'case TtlGcTopo::Answer:            return "answered";'
run_plant "why: not-inited loses the die name"  "$H" 'Navi 48 fallbacks 4x2x8 - refusing";' 'the fallbacks - refusing";'
run_plant "why: too-wide loses the struct size" "$H" '"the die is wider than the 4x2 WGP bitmap TTL slot 14 "' '"the die is wider than the WGP bitmap TTL slot 14 "'
run_plant "why: harvest refusal loses the consequence" "$H" '"would be reported as full whether or not it is";' '"would be reported";'
run_plant "why: accessor and gate disagree"     "$H" 'case TtlGcTopo::RefuseNotInited:   return "GFXConfig was never populated, so the only numbers "' 'case TtlGcTopo::RefuseNoShape:   return "GFXConfig was never populated, so the only numbers "'

echo "== plants: the TTL slot is actually wired to the gate =="
run_plant "xsrc: header not included"          "$C" '#include "ttl_gc_topo.h"' '#include "ttl_gc_topo_off.h"'
run_plant "xsrc: gate never consulted"         "$C" 'n48::ttl_gc_topology(tf, &an, &twhy)' 'n48::ttl_gc_topology_gone(tf, &an, &twhy)'
run_plant "xsrc: verdict never honoured"       "$C" 'if (tv != n48::TtlGcTopo::Answer) {' 'if (false) {'
run_plant "xsrc: decline becomes success"      "$C" '        return kTtlUnsupported;' '        return kTtlOk;'
run_plant "xsrc: SE fallback comes back"       "$C" 'tf.maxShaderEngines = g.max_shader_engines;' 'tf.maxShaderEngines = g.max_shader_engines ? g.max_shader_engines : 4;'
run_plant "xsrc: SA fallback comes back"       "$C" 'tf.maxShPerSe       = g.max_sh_per_se;' 'tf.maxShPerSe       = g.max_sh_per_se ? g.max_sh_per_se : 2;'
run_plant "xsrc: CU fallback comes back"       "$C" 'tf.maxCuPerSh       = g.max_cu_per_sh;' 'tf.maxCuPerSh       = g.max_cu_per_sh ? g.max_cu_per_sh : 8;'
run_plant "xsrc: harvest map ignored"          "$C" 'tf.cuBitmap[i][j] = g.active_cu_bitmap[i][j];' 'tf.cuBitmap[i][j] = 0xFFu;'
run_plant "xsrc: inited fact not passed"       "$C" 'tf.inited           = g.inited;' 'tf.inited           = true;'
run_plant "xsrc: verdict not published"        "$C" 'mOwner->setProperty("Navi48,TtlGcTopo", twhy);' 'mOwner->setProperty("Navi48,TtlGcTopoX", twhy);'
run_plant "xsrc: publish guard dropped"        "$C" 'if (mOwner) mOwner->setProperty("Navi48,TtlGcTopo", twhy);' 'mOwner->setProperty("Navi48,TtlGcTopo", twhy);'
run_plant "xsrc: CU count not published"       "$C" '"Navi48,TtlGcCus"' '"Navi48,TtlGcCusX"'
run_plant "xsrc: SE key renamed"               "$C" '"Navi48,TtlGcSe"' '"Navi48,TtlGcSeX"'
run_plant "xsrc: rows written from a full mask" "$C" 'o->wgpBitmap[i][j] = an.wgpBitmap[i][j];' 'o->wgpBitmap[i][j] = 0xFFFFFFFFu;'
run_plant "xsrc: struct layout resized"        "$C" 'uint32_t wgpBitmap[4][2];' 'uint32_t wgpBitmap[4][3];'

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
