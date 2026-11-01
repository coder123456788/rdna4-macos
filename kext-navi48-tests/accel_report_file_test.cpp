//  accel_report_file_test.cpp — host suite for the on-disk verdict (patch 0027).
//
//  Build (any machine, no Mac, no GPU):
//    zig c++ -std=c++17 -Wall -Wextra -O1 -I tests/dalshim -I src \
//        tests/accel_report_file_test.cpp -o /tmp/accel_report_file_test.exe
//    /tmp/accel_report_file_test.exe .
//
//  argv[1] is the navi48-bringup root, used by the source-pin sections.
//
//  Why a suite for a text file: the owner has no camera in the loop, so this file is the only
//  thing that comes back from a boot. A file that says the wrong verdict, drops the cause, or
//  silently writes nothing is worse than no file - it sends the next fix at the wrong layer.

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

std::string render(const n48::ReportFileInputs &in) {
	char buf[2048];
	n48::report_file_text(buf, sizeof buf, in);
	return std::string(buf);
}

n48::ReportFileInputs base() {
	n48::ReportFileInputs in = {};
	in.fireHeadline = "LoadAccelerator PUBLISHED - Apple's accelerator may now match this card";
	in.armed = true;
	in.started = false;
	in.appleKextLoaded = true;
	in.accelAttached = false;
	in.deviceId = 0x7590;
	in.vendorId = 0x1002;
	in.waited = 20;
	in.ttlBound = 1;
	in.ttlCalls = 7;
	in.ttlSlots = 2;
	in.firstUnsupported = -1;
	in.reachedMask = (1ull << 6) | (1ull << 10);
	return in;
}

} // namespace

int main(int argc, char **argv) {
	const std::string root = argc > 1 ? std::string(argv[1]) : std::string(".");

	// ---------------------------------------------------------------- A: the text
	{
		const std::string s = render(base());
		CHECK(has(s, "navi48 report - written by the kext; no photograph needed"));
		CHECK(has(s, "LoadAccelerator PUBLISHED"));
		CHECK(has(s, "takeover: NO TAKEOVER - no announce within the wait"));
		// The only case that is about the device id: kext loaded, nothing attached.
		CHECK(has(s, "cause: Apple's AMDRadeonX6000 kext is loaded but nothing attached"));
		CHECK(has(s, "kext loaded=1  accelerator child=0"));
		CHECK(has(s, "card: device-id 0x7590  vendor-id 0x1002"));
		CHECK(has(s, "ttl: bound=1 calls=7 slots=2/43 first unsupported=-1"));
		CHECK(has(s, "ttl slots reached: 06 0a"));
		CHECK(has(s, "waited: 20 s"));
	}
	{
		n48::ReportFileInputs in = base();
		in.started = true;
		in.accelAttached = true;
		const std::string s = render(in);
		CHECK(has(s, "takeover: STARTED - Apple's accelerator announced itself"));
		// A takeover needs no excuse line.
		CHECK(!has(s, "cause:"));
		CHECK(has(s, "accelerator child=1"));
	}
	{
		n48::ReportFileInputs in = base();
		in.appleKextLoaded = false;
		const std::string s = render(in);
		CHECK(has(s, "AMDRadeonX6000 kext is not loaded on this boot"));
		CHECK(has(s, "kext loaded=0"));
	}
	{
		n48::ReportFileInputs in = base();
		in.accelAttached = true;
		const std::string s = render(in);
		CHECK(has(s, "attached to this card but its start() failed before it announced itself"));
	}
	{
		n48::ReportFileInputs in = base();
		in.armed = false;
		const std::string s = render(in);
		CHECK(has(s, "takeover: not tracked - no watcher was armed on this boot"));
		// Unarmed is still not proof of a takeover; the cause must survive.
		CHECK(has(s, "cause: "));
	}
	{
		n48::ReportFileInputs in = base();
		in.deviceId = 0;
		in.vendorId = 0;
		const std::string s = render(in);
		CHECK(has(s, "card: device-id 0x0000  vendor-id 0x0000"));
	}
	{
		n48::ReportFileInputs in = base();
		in.firstUnsupported = 14;
		const std::string s = render(in);
		CHECK(has(s, "first unsupported=14"));
	}
	{
		n48::ReportFileInputs in = base();
		in.reachedMask = 0;
		const std::string s = render(in);
		CHECK(!has(s, "ttl slots reached:"));
	}
	{
		// Slot 42 is the last of the 43, and two hex digits must survive the shift.
		n48::ReportFileInputs in = base();
		in.reachedMask = (1ull << 9) | (1ull << 42);
		const std::string s = render(in);
		CHECK(has(s, "ttl slots reached: 09 2a"));
	}
	{
		// A headline-less caller still produces a line, not a crash.
		n48::ReportFileInputs in = base();
		in.fireHeadline = nullptr;
		const std::string s = render(in);
		CHECK(has(s, "acceleration report: (not reported)"));
	}

	// ---------------------------------------------------------------- B: cap semantics
	{
		const n48::ReportFileInputs in = base();
		const size_t need = n48::report_file_text(nullptr, 0, in);
		char small[64];
		memset(small, 'X', sizeof small);
		const size_t got = n48::report_file_text(small, sizeof small, in);
		CHECK(need > sizeof small);
		CHECK(got == need);                     // snprintf semantics: full length, not the cap
		CHECK(small[sizeof small - 1] == 0);    // always NUL-terminated when cap > 0
		CHECK(strlen(small) == sizeof small - 1);
	}
	{
		char exact[2048];
		const n48::ReportFileInputs in = base();
		const size_t need = n48::report_file_text(nullptr, 0, in);
		n48::report_file_text(exact, need + 1, in);
		CHECK(strlen(exact) == need);
	}

	// ---------------------------------------------------------------- C: the runtime
	{
		const std::string rt = slurp(root + "/src/accel_takeover.cpp");
		const std::string hdr = slurp(root + "/src/accel_report_file.h");
		const std::string bringup = slurp(root + "/src/Navi48Bringup.cpp");
		CHECK(!rt.empty());
		CHECK(!hdr.empty());
		CHECK(!bringup.empty());

		CHECK(has(rt, "#include \"accel_report_file.h\""));
		// The file is written with the BSD VFS entry points, from our own thread.
		CHECK(has(rt, "vnode_open(n48::kReportFilePath"));
		CHECK(has(rt, "kO_WRONLY | kO_CREAT"));
		// 0028: the Windows-created 8 KiB target keeps its size; the fixed-length write replaces
		// the text region instead of truncating the file.
		CHECK(!has(rt, "kO_TRUNC"));
		CHECK(has(rt, "const int vfsErr = write_report_file(text, sizeof text);"));
		CHECK(has(rt, "n48::report_raw_write(client, text, sizeof text)"));
		CHECK(has(rt, "n48::raw_write_error_text(err)"));
		CHECK(has(rt, "char text[n48::kReportFileWriteCap];"));
		CHECK(has(rt, "for (size_t i = n; i < sizeof text; i++) text[i] = ' ';"));
		CHECK(has(rt, "in.fireHeadline = (const char *)__atomic_load_n(&gFireHeadline"));
		CHECK(has(rt, "vn_rdwr(kUIO_WRITE, vp, (char *)text"));
		CHECK(has(rt, "vn_rdwr(kUIO_WRITE, vp, (char *)text, (int)len, 0, kUIO_SYSSPACE, 0,"));
		CHECK(has(rt, "vnode_close(vp, kFWRITE, ctx)"));
		CHECK(has(rt, "vfs_context_create(nullptr)"));
		CHECK(has(rt, "vfs_context_rele(ctx)"));
		CHECK(has(rt, "kernel_thread_start(&report_file_thread, nullptr, &th)"));
		CHECK(has(rt, "if (err == 0 && resid != 0) err = kEIO;"));
		// One shot per boot, and the headline is stored before the thread can read it.
		const size_t cas = rt.find("__atomic_compare_exchange_n(&gWriterStarted");
		const size_t store = rt.find("__atomic_store_n(&gFireHeadline, (uintptr_t)fireHeadline");
		const size_t start = rt.find("kernel_thread_start(&report_file_thread");
		CHECK(cas != std::string::npos && store != std::string::npos && start != std::string::npos);
		CHECK(store > cas && store < start);
		// The thread waits for the verdict, then for the volume.
		CHECK(has(rt, "while (waited < n48::kTakeoverWaitSeconds)"));
		CHECK(has(rt, "IOSleep(n48::kReportFileSettleSeconds * 1000)"));   // IOSleep takes ms
		// An unarmed boot is never final on a timeout: the pair boot can announce minutes later.
		CHECK(has(rt, "if (in.started || in.armed) return;"));
		CHECK(has(rt, "if (waited < n48::kTakeoverWaitSeconds) waited++;"));
		CHECK(has(rt, "for (uint32_t t = 0; t < n48::kReportFileRetrySeconds; t++)"));
		CHECK(has(rt, "in.waited = waited;"));
		// Facts and census are read at write time, not at arm time.
		const size_t writeLoop = rt.find("for (uint32_t t = 0; t < n48::kReportFileRetrySeconds; t++)");
		const size_t gather = rt.find("gather_report_inputs()", writeLoop);
		CHECK(gather != std::string::npos && gather > writeLoop);
		CHECK(has(rt, "in.deviceId = injected_id(prov, \"device-id\")"));
		CHECK(has(rt, "mask |= (1ull << s)"));

		// Started on every requested boot - the pair boot never arms the watcher - and handed a
		// real registry service so the raw channel can open the stick as that client.
		CHECK(has(bringup, "#include \"accel_report_file.h\""));
		CHECK(has(bringup, "if (ff.argRequested) n48::takeover_start_writer(head, this);"));
		CHECK(has(rt, "void takeover_start_writer(const char *fireHeadline, ::IOService *client) {"));
		CHECK(has(rt, "__atomic_store_n(&gWriterClient, (uintptr_t)client, __ATOMIC_RELEASE);"));
		const size_t headAt = bringup.find("const char *head = n48::accel_report_headline(");
		const size_t writerAt = bringup.find("n48::takeover_start_writer(head, this)");
		const size_t armAt = bringup.find("n48::accel_takeover_armed(ff.argRequested");
		CHECK(headAt != std::string::npos && writerAt != std::string::npos && armAt != std::string::npos);
		CHECK(headAt < writerAt && writerAt < armAt);
		CHECK(!has(bringup, "n48::takeover_start_writer(head);\n\t\t\t\tif (fireCalled"));

		// ------------------------------------------------------------ D: header hygiene
		CHECK(!has(hdr, "IOKit/"));    // the text builder stands alone: no IOKit headers
		CHECK(!has(hdr, "IOSleep"));
		CHECK(!has(hdr, "TAKELOG"));
		CHECK(!has(hdr, "kernel_thread_start"));
		CHECK(has(hdr, "\"/Volumes/OPENCORE/NAVI48.TXT\""));
		CHECK(has(hdr, "kReportFileWriteCap = 1536"));
		CHECK(has(hdr, "kReportFileRetrySeconds = 120"));
		CHECK(has(hdr, "kReportFileSettleSeconds = 2"));
		// The raw channel opens this file as the registry service that asked for the report.
		CHECK(has(hdr, "void takeover_start_writer(const char *fireHeadline, ::IOService *client);"));
		CHECK(has(hdr, "class IOService;"));
	}

	if (gFailures == 0) printf("OK: %d checks, 0 failures\n", gChecks);
	else                printf("%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
