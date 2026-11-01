//  accel_report_raw_test.cpp — host suite for the mount-independent channel (patch 0028).
//
//  Build (any machine, no Mac, no disk):
//    zig c++ -std=c++17 -Wall -Wextra -O1 -I tests/dalshim -I src \
//        tests/accel_report_raw_test.cpp -o /tmp/accel_report_raw_test.exe
//    /tmp/accel_report_raw_test.exe .
//
//  argv[1] is the navi48-bringup root, used by the source-pin sections.
//
//  Why this suite exists: a raw write that picks the wrong media, the wrong cluster or the wrong
//  file corrupts a volume instead of reporting. The parser decides all three, so every acceptance
//  and every refusal here is fed synthetic sectors - an MBR, a FAT16 BPB, an LFN entry, a deleted
//  entry, a directory that happens to share the name - and the runtime's write-safety predicate is
//  checked against its boundaries. No field is trusted because it "looked plausible".

#include "accel_report_raw.h"
#include "accel_report_file.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int gChecks = 0, gFailures = 0;
void report(const char *file, int line, const char *what, bool ok) {
	gChecks++;
	if (ok) return;
	gFailures++;
	printf("FAIL %s:%d: %s\n", file, line, what);
}
#define CHECK(cond) report(__FILE__, __LINE__, #cond, (cond))

std::string slurp(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return "";
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

bool has(const std::string &hay, const char *needle) {
	return hay.find(needle) != std::string::npos;
}

void wr16(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)(v >> 8);
}
void wr32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
	p[2] = (uint8_t)((v >> 16) & 0xFF);
	p[3] = (uint8_t)((v >> 24) & 0xFF);
}
void put(uint8_t *p, int off, const char *s, size_t n) { memcpy(p + off, s, n); }

// The real stick's shape: 512-byte sectors, 8 KiB clusters, FAT32 with two FATs, label OPENCORE.
constexpr uint32_t kBps = 512;
constexpr uint32_t kSpc = 16;
constexpr uint32_t kReserved = 32;
constexpr uint32_t kFatSize = 16384;
constexpr uint32_t kTotalSectors = 500000;  // 256 MB at 512 B: also exercises tiny-media bounds
constexpr uint64_t kMediaBytes = (uint64_t)kTotalSectors * kBps;
constexpr uint64_t kDataStart = (uint64_t)(kReserved + 2 * kFatSize) * kBps;  // 16793600

void make_bpb(uint8_t *s) {
	memset(s, 0, 512);
	s[0] = 0xEB;
	s[1] = 0x58;
	s[2] = 0x90;
	put(s, 3, "MSWIN4.1", 8);
	wr16(s + 0x0B, (uint16_t)kBps);
	s[0x0D] = (uint8_t)kSpc;
	wr16(s + 0x0E, (uint16_t)kReserved);
	s[0x10] = 2;      // FATs
	wr16(s + 0x11, 0);  // root entries: 0 in FAT32
	wr16(s + 0x13, 0);  // 16-bit total: 0
	s[0x15] = 0xF8;     // media descriptor
	wr16(s + 0x16, 0);  // 16-bit FAT size: 0 in FAT32
	wr32(s + 0x20, kTotalSectors);
	wr32(s + 0x24, kFatSize);
	wr32(s + 0x2C, 2);  // root cluster
	wr16(s + 0x30, 1);  // FSInfo sector
	wr16(s + 0x32, 6);  // backup boot sector
	put(s, 0x47, n48::kRawReportLabel, 11);
	put(s, 0x52, "FAT32   ", 8);
	s[510] = 0x55;
	s[511] = 0xAA;
}

void make_entry(uint8_t *e, const char *name11, uint8_t attr, uint32_t cluster, uint32_t size) {
	memset(e, 0, 32);
	memcpy(e, name11, 11);
	e[0x0B] = attr;
	wr16(e + 0x14, (uint16_t)(cluster >> 16));
	wr16(e + 0x1A, (uint16_t)(cluster & 0xFFFF));
	wr32(e + 0x1C, size);
}

} // namespace

int main(int argc, char **argv) {
	const std::string root = argc > 1 ? std::string(argv[1]) : std::string(".");

	// ---------------------------------------------------------------- A: the BPB
	{
		uint8_t s[512];
		make_bpb(s);
		const n48::Fat32Geometry g = n48::fat32_parse_bpb(s, sizeof s, kMediaBytes);
		CHECK(g.valid);
		CHECK(g.bytesPerSector == kBps);
		CHECK(g.sectorsPerCluster == kSpc);
		CHECK(g.clusterBytes == 8192);
		CHECK(g.fatStart == 16384);
		CHECK(g.dataStart == kDataStart);
		CHECK(g.rootCluster == 2);
		CHECK(n48::fat32_cluster_offset(g, 2) == kDataStart);
		CHECK(n48::fat32_cluster_offset(g, 5) == kDataStart + 3 * 8192);
		CHECK(g.mediaBytes == kMediaBytes);
		CHECK(n48::fat32_cluster_in_fat(g, 5));
		CHECK(n48::fat32_cluster_in_fat(g, (uint32_t)((uint64_t)kFatSize * kBps / 4 - 1)));
		CHECK(!n48::fat32_cluster_in_fat(g, (uint32_t)((uint64_t)kFatSize * kBps / 4)));
	}
	{
		// Every one of these is a plausible-looking sector the parser must refuse.
		uint8_t s[512];
		n48::Fat32Geometry g;

		make_bpb(s);
		s[510] = 0;
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // no 55AA

		make_bpb(s);
		put(s, 0x52, "FAT16   ", 8);
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // FAT16 text

		make_bpb(s);
		memset(s, 0, 512);  // an MBR: no BPB fields at all
		s[510] = 0x55;
		s[511] = 0xAA;
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);

		make_bpb(s);
		wr16(s + 0x0B, 1000);
		wr32(s + 0x20, 200000);  // keeps the media bound out of the answer: only the PO2 rule may refuse
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // bps not a power of two

		make_bpb(s);
		wr16(s + 0x0B, 256);
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // bps below 512

		make_bpb(s);
		wr16(s + 0x0B, 8192);
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // bps above 4096

		make_bpb(s);
		s[0x0D] = 0;
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // no clusters

		make_bpb(s);
		s[0x0D] = 3;
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // spc not a power of two

		make_bpb(s);
		s[0x0D] = 128;
		CHECK(n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);   // the largest legal spc
		s[0x0D] = 0x80 | 0x01;
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // 129, still not a power of two

		make_bpb(s);
		wr16(s + 0x16, 512);  // a 16-bit FAT size means FAT12/16
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);

		make_bpb(s);
		wr16(s + 0x11, 512);  // fixed root directory means FAT12/16
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);

		make_bpb(s);
		wr32(s + 0x24, 0);
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // no FAT

		make_bpb(s);
		s[0x10] = 0;
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // no FAT copies

		make_bpb(s);
		s[0x10] = 5;
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // too many FATs

		make_bpb(s);
		wr16(s + 0x0E, 0);
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // no reserved sectors

		make_bpb(s);
		wr32(s + 0x2C, 1);
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // cluster 1 is not data

		make_bpb(s);
		wr32(s + 0x20, 0);
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);  // no sectors at all

		make_bpb(s);
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes - 1).valid);  // partition exceeds media

		make_bpb(s);
		CHECK(!n48::fat32_parse_bpb(s, 511, kMediaBytes).valid);  // short sector

		make_bpb(s);
		g = n48::fat32_parse_bpb(s, sizeof s, 0);  // 0 = media size unknown: skip that bound
		CHECK(g.valid);

		make_bpb(s);
		wr32(s + 0x2C, 2097152);  // the first cluster the FAT here cannot describe
		CHECK(!n48::fat32_parse_bpb(s, sizeof s, kMediaBytes).valid);
	}
	{
		uint8_t s[512];
		make_bpb(s);
		CHECK(n48::fat32_label_matches(s, sizeof s, n48::kRawReportLabel));
		put(s, 0x47, "opencore   ", 11);
		CHECK(n48::fat32_label_matches(s, sizeof s, n48::kRawReportLabel));  // case-insensitive
		put(s, 0x47, "OPENCOREX  ", 11);
		CHECK(!n48::fat32_label_matches(s, sizeof s, n48::kRawReportLabel));
		put(s, 0x47, "OPENCORE   ", 11);
		CHECK(!n48::fat32_label_matches(s, 0x51, n48::kRawReportLabel));  // not even the label fits
	}

	// ---------------------------------------------------------------- B: the directory
	{
		uint8_t dir[8192];
		memset(dir, 0xE5, sizeof dir);  // "free" entries that cannot match
		memset(dir, 0, 32 * 8);          // then a run of terminators past entry 7
		uint8_t *e = dir;
		make_entry(e, n48::kRawReportName, 0x0F, 11, 8192);       // a long-name fragment
		make_entry(e + 32, n48::kRawReportName, 0x20, 9, 8192);   // deleted
		(e + 32)[0] = 0xE5;
		make_entry(e + 64, n48::kRawReportLabel, 0x08, 0, 0);     // volume id
		make_entry(e + 96, n48::kRawReportName, 0x10, 7, 0);      // a DIRECTORY with the same name
		make_entry(e + 128, "NOTES   TXT", 0x20, 8, 100);         // another file
		make_entry(e + 160, n48::kRawReportName, 0x20, 5, 8192);  // the file

		const n48::Fat32DirHit h = n48::fat32_scan_dir(dir, sizeof dir, n48::kRawReportName);
		CHECK(h.found);
		CHECK(h.firstCluster == 5);
		CHECK(h.fileSize == 8192);
		CHECK(h.entryOffset == 160);
		CHECK(!h.end);
	}
	{
		uint8_t dir[8192];
		memset(dir, 0, sizeof dir);  // first entry 0x00: nothing after it can exist
		make_entry(dir + 32, n48::kRawReportName, 0x20, 5, 8192);
		const n48::Fat32DirHit h = n48::fat32_scan_dir(dir, sizeof dir, n48::kRawReportName);
		CHECK(!h.found);
		CHECK(h.end);
	}
	{
		uint8_t dir[8192];
		memset(dir, 0xE5, sizeof dir);  // no match and no terminator in this cluster
		const n48::Fat32DirHit h = n48::fat32_scan_dir(dir, sizeof dir, n48::kRawReportName);
		CHECK(!h.found);
		CHECK(!h.end);
	}
	{
		uint8_t dir[8192];
		memset(dir, 0, sizeof dir);
		make_entry(dir, n48::kRawReportName, 0x20, 0x20003, 4096);  // high word of the cluster
		const n48::Fat32DirHit h = n48::fat32_scan_dir(dir, sizeof dir, n48::kRawReportName);
		CHECK(h.found);
		CHECK(h.firstCluster == 0x20003);
		CHECK(h.fileSize == 4096);
	}
	{
		uint8_t dir[8192];
		memset(dir, 0xE5, sizeof dir);
		make_entry(dir, n48::kRawReportName, 0x20, 5, 8192);
		const n48::Fat32DirHit whole = n48::fat32_scan_dir(dir, 32, n48::kRawReportName);
		CHECK(whole.found);
		const n48::Fat32DirHit none = n48::fat32_scan_dir(dir, 31, n48::kRawReportName);
		CHECK(!none.found);  // a partial entry is not an entry
	}
	{
		// 0x28 is "volume id + archive": still metadata, never the file.
		uint8_t dir[8192];
		memset(dir, 0xE5, sizeof dir);
		make_entry(dir, n48::kRawReportName, 0x28, 5, 8192);
		CHECK(!n48::fat32_scan_dir(dir, sizeof dir, n48::kRawReportName).found);
	}
	{
		// The names themselves: the 8.3 form has no dot and no long-name entry.
		CHECK(sizeof n48::kRawReportName == 11);
		CHECK(memcmp(n48::kRawReportName, "NAVI48  TXT", 11) == 0);
		CHECK(memcmp(n48::kRawReportLabel, "OPENCORE   ", 11) == 0);
		CHECK(memcmp(n48::kRawReportMagic, "navi", 4) == 0);
		// The text builder's first line is the magic, so a report written by an earlier boot
		// keeps the file valid for the next one.
		n48::ReportFileInputs in = {};
		in.fireHeadline = "x";
		char buf[2048];
		n48::report_file_text(buf, sizeof buf, in);
		CHECK(memcmp(buf, n48::kRawReportMagic, 4) == 0);
	}

	// ---------------------------------------------------------------- C: the write target
	{
		uint8_t s[512];
		make_bpb(s);
		const n48::Fat32Geometry g = n48::fat32_parse_bpb(s, sizeof s, kMediaBytes);
		const uint32_t len = n48::kReportFileWriteCap;
		uint64_t off = 0;
		CHECK(n48::fat32_file_target(g, 5, 8192, len, &off));
		CHECK(off == kDataStart + 3 * 8192);
		CHECK(n48::fat32_file_target(g, 5, 8192, len, nullptr));  // out-pointer is optional
	}
	{
		uint8_t s[512];
		make_bpb(s);
		n48::Fat32Geometry g = n48::fat32_parse_bpb(s, sizeof s, kMediaBytes);
		const uint32_t len = n48::kReportFileWriteCap;
		uint64_t off = 0;
		CHECK(!n48::fat32_file_target(g, 5, len - 1, len, &off));       // file too small
		CHECK(!n48::fat32_file_target(g, 5, 8192, 0, &off));            // nothing to write
		CHECK(!n48::fat32_file_target(g, 0, 8192, len, &off));          // cluster 0
		CHECK(!n48::fat32_file_target(g, 1, 8192, len, &off));          // cluster 1
		// A cluster the FAT cannot describe is refused even when the media is big enough that the
		// byte offset alone would look fine.
		g.mediaBytes = (uint64_t)1 << 40;
		CHECK(!n48::fat32_file_target(g, (uint32_t)((uint64_t)kFatSize * kBps / 4), 8192, len, &off));
		g.mediaBytes = kMediaBytes;
		g.valid = false;
		CHECK(!n48::fat32_file_target(g, 5, 8192, len, &off));          // unparsed geometry
		g.valid = true;
		g.clusterBytes = 512;
		CHECK(!n48::fat32_file_target(g, 5, 8192, len, &off));          // cluster smaller than the write
		g.clusterBytes = 8192;
		g.mediaBytes = n48::fat32_cluster_offset(g, 5) + len - 1;
		CHECK(!n48::fat32_file_target(g, 5, 8192, len, &off));          // the write would leave the media
		g.mediaBytes = n48::fat32_cluster_offset(g, 5) + len;
		CHECK(n48::fat32_file_target(g, 5, 8192, len, &off));           // ends exactly at the edge
	}
	{
		// A 4 KiB-sector stick rounds the fixed write up to 4 KiB, never past the file.
		uint8_t s[512];
		make_bpb(s);
		wr16(s + 0x0B, 4096);
		s[0x0D] = 2;                    // 8 KiB clusters again
		wr32(s + 0x20, 62500);          // 62500 * 4096 = the same media size
		const n48::Fat32Geometry g = n48::fat32_parse_bpb(s, sizeof s, kMediaBytes);
		CHECK(g.valid);
		CHECK(g.clusterBytes == 8192);
		CHECK(n48::fat32_file_target(g, 5, 8192, n48::kReportFileWriteCap, nullptr));
		CHECK(n48::fat32_file_target(g, 5, 4096, 4096, nullptr));
	}
	{
		n48::Fat32Geometry g = {};
		CHECK(!n48::fat32_file_target(g, 5, 8192, 1536, nullptr));
		CHECK(n48::fat32_chain_value(0xFFFFFFFFu) == 0x0FFFFFFFu);
		CHECK(n48::fat32_chain_done(0x0FFFFFF8u));
		CHECK(!n48::fat32_chain_done(0x0FFFFFF7u));
		CHECK(n48::fat32_chain_ok(2));
		CHECK(!n48::fat32_chain_ok(1));
		CHECK(!n48::fat32_chain_ok(0x0FFFFFF7u));
	}

	// ---------------------------------------------------------------- D: error codes
	{
		const char *codes[] = { n48::raw_write_error_text(n48::kRawNoStick),
			                    n48::raw_write_error_text(n48::kRawNoFile),
			                    n48::raw_write_error_text(n48::kRawBadMagic),
			                    n48::raw_write_error_text(n48::kRawIoError) };
		CHECK(strcmp(n48::raw_write_error_text(n48::kRawOk), "ok") == 0);
		for (const char *c : codes) CHECK(c && *c && strcmp(c, "bad arguments") != 0);
		CHECK(strcmp(n48::raw_write_error_text(n48::kRawBadArgs), "bad arguments") == 0);
		CHECK(strcmp(n48::raw_write_error_text(999), "bad arguments") == 0);
		CHECK(strcmp(codes[0], codes[1]) != 0 && strcmp(codes[1], codes[2]) != 0);
	}

	// ---------------------------------------------------------------- E: the runtime
	{
		const std::string rt = slurp(root + "/src/accel_report_raw.cpp");
		const std::string hdr = slurp(root + "/src/accel_report_raw.h");
		const std::string sh = slurp(root + "/src/accel_report_file.h");
		const std::string tk = slurp(root + "/src/accel_takeover.cpp");
		const std::string bringup = slurp(root + "/src/Navi48Bringup.cpp");
		CHECK(!rt.empty());
		CHECK(!hdr.empty());
		CHECK(!sh.empty());
		CHECK(!tk.empty());
		CHECK(!bringup.empty());

		// Header hygiene: the FAT32 logic is host-testable and IOKit-free.
		CHECK(!has(hdr, "IOKit/"));
		CHECK(!has(hdr, "IOSleep"));
		CHECK(!has(hdr, "TAKELOG"));
		CHECK(!has(hdr, "kernel_thread_start"));
		CHECK(has(hdr, "#include \"accel_report_file.h\""));
		CHECK(has(sh, "\"/Volumes/OPENCORE/NAVI48.TXT\""));
		CHECK(has(sh, "kReportFileWriteCap = 1536"));

		// The discovery: every IOMedia in the stick's size class, one probe per media.
		CHECK(has(rt, "#include \"accel_report_raw.h\""));
		CHECK(has(rt, "::IOService::serviceMatching(\"IOMedia\")"));
		CHECK(has(rt, "::IOService::getMatchingServices(match)"));
		CHECK(has(rt, "if (size < n48::kRawReportMinMediaBytes || size > n48::kRawReportMaxMediaBytes) return false;"));
		CHECK(has(rt, "if (!stick_geometry(media, client, &g)) continue;"));
		CHECK(has(rt, "IOMedia *media = OSDynamicCast(IOMedia, svc);"));
		CHECK(has(rt, "match->release();"));
		CHECK(has(rt, "it->release();"));
		CHECK(has(rt, "if (result == kRawOk) break;"));

		// The five gates, in order: reader open, BPB, label, file entry, magic.
		CHECK(has(rt, "if (!media->open(client, 0, kIOStorageAccessReader)) return false;"));
		CHECK(has(rt, "read_at(media, client, 0, boot, sizeof boot)"));
		CHECK(has(rt, "n48::fat32_parse_bpb(boot, sizeof boot, size)"));
		CHECK(has(rt, "n48::fat32_label_matches(boot, sizeof boot, n48::kRawReportLabel)"));
		CHECK(has(rt, "n48::fat32_scan_dir(p, (size_t)g.clusterBytes, n48::kRawReportName)"));
		CHECK(has(rt, "if (!n48::fat32_file_target(g, hit.firstCluster, hit.fileSize, (uint32_t)len, &fileOff))"));
		CHECK(has(rt, "if (head[i] != n48::kRawReportMagic[i]) return n48::kRawBadMagic;"));

		// The walk: bounded, only through valid chain values, and only within the partition.
		CHECK(has(rt, "for (uint32_t hops = 0; hops < n48::kRawReportMaxChainClusters; hops++)"));
		CHECK(has(rt, "if (!n48::fat32_cluster_in_fat(g, cluster)) return n48::kRawIoError;"));
		CHECK(has(rt, "if (off + g.clusterBytes > g.mediaBytes) return n48::kRawIoError;"));
		CHECK(has(rt, "if (!n48::fat32_chain_ok(next)) return n48::kRawOk;"));

		// The write: reader-writer open, sector-aligned, spaces after the text, then closed.
		CHECK(has(rt, "if (!media->open(client, 0, kIOStorageAccessReaderWriter)) return n48::kRawIoError;"));
		CHECK(has(rt, "((uint64_t)len + g.bytesPerSector - 1) / g.bytesPerSector * g.bytesPerSector;"));
		CHECK(has(rt, "if (aligned > hit.fileSize || aligned > g.clusterBytes) return n48::kRawNoFile;"));
		CHECK(has(rt, "memcpy(p, text, len);"));
		CHECK(has(rt, "memset(p + len, ' ', (size_t)(aligned - len));"));
		CHECK(has(rt, "media->write(client, fileOff, out, nullptr, &actual)"));
		CHECK(has(rt, "kr == kIOReturnSuccess && actual == aligned"));
		CHECK(has(rt, "media->synchronize(client, fileOff, aligned)"));
		CHECK(has(rt, "media->close(client, 0);"));
		CHECK(has(rt, "return kRawBadArgs;"));

		// The writer thread: VFS first, block device second, same bytes on both.
		CHECK(has(tk, "#include \"accel_report_raw.h\""));
		CHECK(has(tk, "const int vfsErr = write_report_file(text, sizeof text);"));
		CHECK(has(tk, "err = n48::report_raw_write(client, text, sizeof text);"));
		const size_t vfsAt = tk.find("write_report_file(text, sizeof text)");
		const size_t rawAt = tk.find("report_raw_write(client, text, sizeof text)");
		CHECK(vfsAt != std::string::npos && rawAt != std::string::npos && vfsAt < rawAt);
		CHECK(has(tk, "const char *channel = \"the mounted volume\";"));
		CHECK(has(tk, "channel = \"the block device (stick not mounted)\";"));
		CHECK(has(tk, "report written to %s through %s"));
		CHECK(has(tk, "for (size_t i = n; i < sizeof text; i++) text[i] = ' ';"));
		// One client, stored before the thread can read it.
		const size_t store = tk.find("__atomic_store_n(&gWriterClient, (uintptr_t)client");
		const size_t start = tk.find("kernel_thread_start(&report_file_thread");
		CHECK(store != std::string::npos && start != std::string::npos && store < start);
		CHECK(has(bringup, "n48::takeover_start_writer(head, this);"));
	}

	if (gFailures == 0) printf("OK: %d checks, 0 failures\n", gChecks);
	else                printf("%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
