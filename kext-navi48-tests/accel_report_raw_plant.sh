#!/usr/bin/env bash
#
#  accel_report_raw_plant.sh — prove the 0028 suite can actually fail.
#
#  Usage (from anywhere):  ZIG=/path/to/zig.exe bash tests/accel_report_raw_plant.sh
#
#  One break at a time, planted into a COPY of the real sources, and the suite must catch it.
#  The raw channel writes into a FAT32 volume with no filesystem above it, so a wrong parser
#  decision is a corruption, not a misprinted line: every gate that keeps the write inside
#  NAVI48.TXT is planted here, and the suite is what makes that recoverable.
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
	"$ZIG" c++ -std=c++17 -O1 -I "$SCR/dalshim" -I "$SCR/src" \
		"$HERE/accel_report_raw_test.cpp" -o "$SCR/t" >"$SCR/build.log" 2>&1 || {
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

H=src/accel_report_raw.h
R=src/accel_report_raw.cpp
T=src/accel_takeover.cpp
X=src/Navi48Bringup.cpp

declare -a FAIL MISS
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

echo "== plants: what counts as a FAT32 volume =="
run_plant "bpb: the boot signature is not checked"   "$H" 'if (!s || len < 512 || s[510] != 0x55 || s[511] != 0xAA) return g;' 'if (!s || len < 512) return g;'
run_plant "bpb: a 16-bit FAT passes as FAT32"        "$H" 'if (fat16 != 0 || rootEntries != 0 || g.fatSizeSectors == 0) return g;' 'if (rootEntries != 0 || g.fatSizeSectors == 0) return g;'
run_plant "bpb: the FAT32 string is not checked"     "$H" "if (s[0x52] != 'F' || s[0x53] != 'A' || s[0x54] != 'T' || s[0x55] != '3' || s[0x56] != '2')" 'if (0)'
run_plant "bpb: a non-power-of-two sector passes"    "$H" '(g.bytesPerSector & (g.bytesPerSector - 1)) != 0)' '(g.bytesPerSector & (g.bytesPerSector - 1)) == 0xFFFF)'
run_plant "bpb: a non-power-of-two cluster passes"   "$H" '(g.sectorsPerCluster & (g.sectorsPerCluster - 1)) != 0)' '(g.sectorsPerCluster & (g.sectorsPerCluster - 1)) == 0xFFFF)'
run_plant "bpb: zero reserved sectors pass"          "$H" 'if (g.reservedSectors == 0 || g.fatCount == 0 || g.fatCount > 4) return g;' 'if (g.fatCount == 0 || g.fatCount > 4) return g;'
run_plant "bpb: the partition may exceed the media"  "$H" 'if (mediaBytes != 0 && totalBytes > mediaBytes) return g;' ';'
run_plant "label: case is significant"               "$H" "if (a >= 'a' && a <= 'z') a = (uint8_t)(a - 32);" ';'
run_plant "label: only eight bytes are compared"     "$H" 'for (int i = 0; i < 11; i++) {' 'for (int i = 0; i < 8; i++) {'

echo "== plants: what counts as the file's directory entry =="
# An LFN fragment (attr 0x0F) and a deleted entry (first byte 0xE5) are both already skipped by
# the volume/directory test and by the name comparison, so removing those two checks is a no-op
# by construction - no plant for them here. The checks that change behaviour are below.
run_plant "dir: directories are files"               "$H" 'if (attr & 0x18) continue;' ';'
run_plant "dir: the terminator is ignored"           "$H" 'if (e[0] == 0x00) {' 'if (e[0] == 0x01) {'
run_plant "dir: the high cluster word is dropped"    "$H" 'hit.firstCluster = (uint32_t)raw_rd16(e + 0x1A) | ((uint32_t)raw_rd16(e + 0x14) << 16);' 'hit.firstCluster = (uint32_t)raw_rd16(e + 0x1A);'
run_plant "dir: the size is read from the wrong field" "$H" 'hit.fileSize = raw_rd32(e + 0x1C);' 'hit.fileSize = raw_rd32(e + 0x10);'

echo "== plants: the write-safety predicate =="
run_plant "target: a short file is accepted"         "$H" 'if (fileSize < writeLen || g.clusterBytes < writeLen) return false;' 'if (g.clusterBytes < writeLen) return false;'
run_plant "target: a zero-length write is allowed"   "$H" 'if (!g.valid || firstCluster < 2 || writeLen == 0) return false;' 'if (!g.valid || firstCluster < 2) return false;'
run_plant "target: unparsed geometry is accepted"    "$H" 'if (!g.valid || firstCluster < 2 || writeLen == 0) return false;' 'if (firstCluster < 2 || writeLen == 0) return false;'
run_plant "target: a write may leave the media"      "$H" 'if (off + writeLen > g.mediaBytes) return false;' ';'
run_plant "chain: the FAT mask keeps all 32 bits"    "$H" 'inline uint32_t fat32_chain_value(uint32_t entry) { return entry & 0x0FFFFFFFu; }' 'inline uint32_t fat32_chain_value(uint32_t entry) { return entry; }'
run_plant "chain: 0x0FFFFFF7 is a next cluster"      "$H" 'inline bool fat32_chain_ok(uint32_t c) { return c >= 2 && c < 0x0FFFFFF7u; }' 'inline bool fat32_chain_ok(uint32_t c) { return c >= 2 && c < 0x0FFFFFF8u; }'
run_plant "chain: 0x0FFFFFF8 is not the end"         "$H" 'inline bool fat32_chain_done(uint32_t c) { return c >= 0x0FFFFFF8u; }' 'inline bool fat32_chain_done(uint32_t c) { return c >= 0x0FFFFFFFu; }'
run_plant "header: the 8.3 name is misspelled"       "$H" "constexpr char kRawReportName[11] = { 'N', 'A', 'V', 'I', '4', '8', ' ', ' ', 'T', 'X', 'T' };" "constexpr char kRawReportName[11] = { 'N', 'A', 'V', 'I', '4', '8', ' ', ' ', 'T', 'X', 'Y' };"
run_plant "header: the magic stops being navi"       "$H" "constexpr uint8_t kRawReportMagic[4] = { 'n', 'a', 'v', 'i' };" "constexpr uint8_t kRawReportMagic[4] = { 'n', 'a', 'v', 'j' };"

echo "== plants: the block-device runtime =="
run_plant "runtime: the size class is ignored"       "$R" 'if (size < n48::kRawReportMinMediaBytes || size > n48::kRawReportMaxMediaBytes) return false;' ';'
run_plant "runtime: the reader-writer open is gone"  "$R" 'if (!media->open(client, 0, kIOStorageAccessReaderWriter)) return n48::kRawIoError;' ';'
run_plant "runtime: sector 0 is read elsewhere"      "$R" 'read_at(media, client, 0, boot, sizeof boot)' 'read_at(media, client, 512, boot, sizeof boot)'
run_plant "runtime: the label is not checked"        "$R" 'ok = g.valid && n48::fat32_label_matches(boot, sizeof boot, n48::kRawReportLabel);' 'ok = g.valid;'
run_plant "runtime: the directory walk is unbounded" "$R" 'for (uint32_t hops = 0; hops < n48::kRawReportMaxChainClusters; hops++)' 'for (uint32_t hops = 0; hops < 1000000; hops++)'
run_plant "runtime: a bad chain value is followed"   "$R" 'if (!n48::fat32_chain_ok(next)) return n48::kRawOk;' 'if (next == 0) return n48::kRawOk;'
run_plant "runtime: the scan uses another name"      "$R" 'n48::fat32_scan_dir(p, (size_t)g.clusterBytes, n48::kRawReportName)' 'n48::fat32_scan_dir(p, (size_t)g.clusterBytes, "NAVI48  TXT")'
run_plant "runtime: the file magic is not required"  "$R" 'if (head[i] != n48::kRawReportMagic[i]) return n48::kRawBadMagic;' ';'
run_plant "runtime: the write length is not rounded" "$R" '((uint64_t)len + g.bytesPerSector - 1) / g.bytesPerSector * g.bytesPerSector;' '(uint64_t)len;'
run_plant "runtime: the pad is NULs"                 "$R" "memset(p + len, ' ', (size_t)(aligned - len));" 'memset(p + len, 0, (size_t)(aligned - len));'
run_plant "runtime: the write lands at the partition" "$R" 'media->write(client, fileOff, out, nullptr, &actual)' 'media->write(client, 0, out, nullptr, &actual)'
run_plant "runtime: a short write is success"        "$R" 'if (kr == kIOReturnSuccess && actual == aligned) {' 'if (kr == kIOReturnSuccess) {'
run_plant "runtime: the iterator is leaked"          "$R" 'it->release();' ';'
run_plant "runtime: the match dictionary is leaked"  "$R" 'match->release();' ';'
run_plant "runtime: the scan stops at a failure"     "$R" 'if (result == kRawOk) break;' 'return result;'

echo "== plants: the writer thread uses it as the fallback =="
run_plant "writer: the block device is never tried"  "$T" 'err = n48::report_raw_write(client, text, sizeof text);' 'err = -1;'
run_plant "writer: the vfs is skipped"               "$T" 'const int vfsErr = write_report_file(text, sizeof text);' 'const int vfsErr = -1;'
run_plant "writer: the client is dropped"            "$T" '__atomic_store_n(&gWriterClient, (uintptr_t)client, __ATOMIC_RELEASE);' ';'
run_plant "writer: the stale tail is kept"           "$T" "for (size_t i = n; i < sizeof text; i++) text[i] = ' ';" ';'
run_plant "writer: the raw channel gets a short len" "$T" 'n48::report_raw_write(client, text, sizeof text)' 'n48::report_raw_write(client, text, n)'

echo "== plants: the xsrc wiring =="
run_plant "xsrc: no client is passed"                "$X" 'if (ff.argRequested) n48::takeover_start_writer(head, this);' 'if (ff.argRequested) n48::takeover_start_writer(head, nullptr);'
run_plant "xsrc: the writer is never started"        "$X" 'if (ff.argRequested) n48::takeover_start_writer(head, this);' ';'

echo "== summary =="
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
