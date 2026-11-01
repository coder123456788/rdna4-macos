//  accel_takeover_report_test.cpp — host suite for the takeover report (patch 0020).
//
//  Build (any machine, no Mac, no GPU):
//    zig c++ -std=c++17 -Wall -Wextra -O1 -I tests/dalshim -I src \
//        tests/accel_takeover_report_test.cpp -o /tmp/accel_takeover_report_test.exe
//    /tmp/accel_takeover_report_test.exe .
//
//  argv[1] is the navi48-bringup root, used by the source-pin sections.
//
//  What this suite is for: 0019's block proves the gate opened; this one has to prove that the
//  card was actually taken. The failure that matters is the quiet one — a verdict printed on a
//  boot where LoadAccelerator never went up ("Apple did not match" blaming the personality for
//  the gate's decision), or a timeout that fires while the positive signal was already recorded.
//  Section A is the first, section C pins the second out of accel_takeover.cpp.

#include "accel_takeover_report.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using n48::TakeoverVerdict;

static int gChecks = 0, gFailures = 0;
static void report(const char *file, int line, const char *what, bool ok) {
	gChecks++;
	if (ok) return;
	gFailures++;
	printf("FAIL %s:%d: %s\n", file, line, what);
}
#define CHECK(cond) report(__FILE__, __LINE__, #cond, (cond))

namespace {

std::string slurp(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return "";
	std::ostringstream ss;
	ss << in.rdbuf();
	std::string s = ss.str();
	for (auto &c : s) if (c == '\r') c = '\n';
	while (!s.empty() && s.back() == '\n') s.pop_back();
	return s;
}

bool has(const std::string &hay, const std::string &needle) {
	return hay.find(needle) != std::string::npos;
}

size_t count_of(const std::string &hay, const std::string &needle) {
	size_t n = 0, p = 0;
	while ((p = hay.find(needle, p)) != std::string::npos) { n++; p += needle.size(); }
	return n;
}

// The text of one definition, from `def` up to the next definition `next`. A count taken inside
// a function says what that path does; a count over the whole file only says how many writers
// happen to exist today (0027 and 0030 each added two).
std::string body_of(const std::string &hay, const std::string &def, const std::string &next) {
	const size_t a = hay.find(def);
	if (a == std::string::npos) return "";
	const size_t b = hay.find(next, a);
	return hay.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// The rules in prose, written out by hand rather than reusing the header's expression.
TakeoverVerdict expect_verdict(bool armed, bool started) {
	if (!armed) return TakeoverVerdict::NotArmed;
	if (started) return TakeoverVerdict::Started;
	return TakeoverVerdict::TimedOut;
}

} // namespace

int main(int argc, char **argv) {
	const std::string root = argc > 1 ? argv[1] : ".";

	// ---------------------------------------------------------------- A: arming and verdict
	// A report about a takeover must not exist unless the card was offered to Apple: the
	// headline for TimedOut names the personality, so printing it after a closed gate would
	// accuse the wrong thing.
	CHECK(!n48::accel_takeover_armed(false, false));
	CHECK(!n48::accel_takeover_armed(false, true));    // a publish nobody asked to watch
	CHECK(!n48::accel_takeover_armed(true, false));    // asked, but LoadAccelerator never went up
	CHECK(n48::accel_takeover_armed(true, true));

	for (int armedI = 0; armedI < 2; armedI++)
		for (int startedI = 0; startedI < 2; startedI++) {
			const bool a = armedI != 0, s = startedI != 0;
			CHECK(n48::accel_takeover_verdict(a, s) == expect_verdict(a, s));
			// An unarmed watch can never report a takeover, whatever the signal says.
			if (!a) CHECK(n48::accel_takeover_verdict(a, s) == TakeoverVerdict::NotArmed);
		}

	// The watch must outlast the 0019 hold, or the two blocks overwrite each other on screen.
	CHECK(n48::kTakeoverWaitSeconds >= 20);
	CHECK(n48::kTakeoverWaitSeconds <= 120);
	CHECK(n48::kTakeoverHold >= 3);
	CHECK(n48::kTakeoverHold <= 20);

	// ---------------------------------------------------------------- B: the headline
	const TakeoverVerdict allv[] = { TakeoverVerdict::NotArmed, TakeoverVerdict::Started,
	                                 TakeoverVerdict::TimedOut };
	for (TakeoverVerdict v : allv) {
		const char *h = n48::accel_takeover_headline(v);
		CHECK(h && h[0]);                       // never an empty screen
		CHECK(!has(h, "TakeoverVerdict"));      // an owner photographs English, not an enum
	}
	const char *started = n48::accel_takeover_headline(TakeoverVerdict::Started);
	const char *timedOut = n48::accel_takeover_headline(TakeoverVerdict::TimedOut);
	const char *notArmed = n48::accel_takeover_headline(TakeoverVerdict::NotArmed);
	CHECK(std::strcmp(started, timedOut) != 0);
	CHECK(std::strcmp(started, notArmed) != 0);
	CHECK(std::strcmp(timedOut, notArmed) != 0);
	CHECK(has(started, "TAKEOVER"));
	CHECK(has(started, "started"));
	CHECK(has(started, "handover"));
	CHECK(!has(started, "never"));
	CHECK(has(timedOut, "NO TAKEOVER"));
	CHECK(has(timedOut, "never"));
	CHECK(has(timedOut, "reason"));
	CHECK(!has(timedOut, "handover happened"));
	CHECK(has(notArmed, "not tracked"));

	// ---------------------------------------------------------------- B2: the diagnosis
	// A timeout has three causes and they need different fixes, so the block must name the
	// one that happened. The kext-absent case must not accuse the id: on a recovery boot
	// there is no accelerator class at all.
	const n48::TakeoverFacts fNoKext = { false, false };
	const n48::TakeoverFacts fSilent = { true, true };
	const n48::TakeoverFacts fNoMatch = { true, false };
	const char *dNoKext = n48::accel_takeover_diagnosis(fNoKext);
	const char *dSilent = n48::accel_takeover_diagnosis(fSilent);
	const char *dNoMatch = n48::accel_takeover_diagnosis(fNoMatch);
	CHECK(dNoKext && dSilent && dNoMatch);
	CHECK(std::strcmp(dNoKext, dSilent) != 0);
	CHECK(std::strcmp(dNoKext, dNoMatch) != 0);
	CHECK(std::strcmp(dSilent, dNoMatch) != 0);
	CHECK(has(dNoKext, "not loaded"));
	CHECK(!has(dNoKext, "personality"));
	CHECK(!has(dNoKext, "device id"));
	CHECK(has(dSilent, "start()"));
	CHECK(has(dNoMatch, "personality"));
	CHECK(has(dNoMatch, "device id"));

	// ---------------------------------------------------------------- C: xsrc wiring
	const std::string bringup = slurp(root + "/src/Navi48Bringup.cpp");
	const std::string peer = slurp(root + "/src/apple/Navi48AccelPeer.cpp");
	const std::string rt = slurp(root + "/src/accel_takeover.cpp");
	CHECK(!bringup.empty());
	CHECK(!peer.empty());
	CHECK(!rt.empty());

	// armed from the fire block, and only when the fire call published
	CHECK(has(bringup, "#include \"accel_takeover_report.h\""));
	CHECK(has(bringup, "n48::accel_takeover_armed(ff.argRequested, fireCalled && fireKr == 0)"));
	CHECK(has(bringup, "n48::takeover_arm(pciDevice);"));
	CHECK(has(bringup, "n48::takeover_start_poller();"));
	CHECK(has(bringup, "setProperty(\"Navi48,AccelTakeover\""));
	// ...and after the 0019 block, so the screen carries the decision before the watch starts
	CHECK(bringup.find("n48::accel_report_mode(ff.argRequested, holdRequested)") <
	      bringup.find("n48::accel_takeover_armed(ff.argRequested, fireCalled && fireKr == 0)"));
	CHECK(bringup.find("const IOReturn fkr = accelExperiment(1 /*fire*/") <
	      bringup.find("n48::accel_takeover_armed(ff.argRequested, fireCalled && fireKr == 0)"));

	// the positive signal is recorded inside kReqAccelStarted, before anything else can fail
	CHECK(has(peer, "#include \"accel_takeover_report.h\""));
	CHECK(has(peer, "n48::takeover_mark_started(p2);"));
	const size_t caseStart = peer.rfind("case kReqAccelStarted:");   // the handler's label, not req_name's
	const size_t mark = peer.find("n48::takeover_mark_started(p2);");
	const size_t caseNext = peer.find("case kReqIsDeviceValid:", mark);
	CHECK(caseStart != std::string::npos);
	CHECK(mark != std::string::npos && caseStart < mark);
	CHECK(caseNext != std::string::npos && mark < caseNext);
	CHECK(mark < peer.find("force_accelerator_enabled(p2);"));

	// the negative signal: a thread that waits, one second at a time, and stands down the
	// moment the positive one is recorded.
	// Everything below is counted in the function that owns it: 0027's verdict writer repeats the
	// poller's wait loop and 0030 added a third thread, so a whole-file has() passes on the
	// writer's copy while the poller's own copy is broken.
	const std::string pollerBody = body_of(rt, "void takeover_poller(", "namespace n48 {");
	const std::string writerBody = body_of(rt, "void report_file_thread(", "void survey_report_thread(");
	const std::string surveyBody = body_of(rt, "void survey_report_thread(", "void print_block(");
	const std::string holdBody = body_of(rt, "void print_block(", "void takeover_poller(");
	CHECK(!pollerBody.empty() && !writerBody.empty() && !surveyBody.empty() && !holdBody.empty());
	CHECK(has(rt, "kernel_thread_start(&takeover_poller, nullptr, &th)"));
	CHECK(has(pollerBody, "while (waited < n48::kTakeoverWaitSeconds) {"));
	CHECK(has(pollerBody, "if (__atomic_load_n(&gStarted, __ATOMIC_ACQUIRE)) break;"));
	// ...and the writer's own wait ends on the same flag, so a late announce is not waited out.
	// Both loops use the tested constant, not a literal that drifts from kTakeoverWaitSeconds.
	CHECK(has(writerBody, "while (waited < n48::kTakeoverWaitSeconds) {"));
	CHECK(has(writerBody, "if (__atomic_load_n(&gStarted, __ATOMIC_ACQUIRE)) break;"));
	// Every thread this file starts is also released - the poller's, 0027's writer, 0030's survey.
	CHECK(count_of(rt, "thread_deallocate(th)") == count_of(rt, "kernel_thread_start("));
	// One second per tick, counted inside the function that owns the tick. 0027's verdict writer
	// and 0030's survey writer each sleep once per retry second on their own path, so a literal
	// count over the whole file would only pin how many writers exist, not that they wait.
	CHECK(count_of(pollerBody, "IOSleep(1000);") == 1);   // the timeout wait loop
	CHECK(count_of(writerBody, "IOSleep(1000);") == 2);   // the verdict writer: wait loop + retry loop
	CHECK(count_of(surveyBody, "IOSleep(1000);") == 2);   // 0030: idle tick + retry loop
	CHECK(count_of(holdBody, "IOSleep(1000);") == 1);     // one second per hold, never two
	// ...and the poller's tick is inside the wait loop, not merely somewhere in the thread.
	const size_t pollWait = pollerBody.find("while (waited < n48::kTakeoverWaitSeconds) {");
	const size_t pollTick = pollerBody.find("IOSleep(1000);");
	CHECK(pollWait != std::string::npos && pollTick != std::string::npos && pollWait < pollTick);
	// No path outside those four sleeps a second, so a new writer has to be pinned here by name.
	CHECK(count_of(pollerBody, "IOSleep(1000);") + count_of(writerBody, "IOSleep(1000);") +
	      count_of(surveyBody, "IOSleep(1000);") + count_of(holdBody, "IOSleep(1000);") ==
	      count_of(rt, "IOSleep(1000);"));
	CHECK(has(rt, "__atomic_store_n(&gArmed, 1u, __ATOMIC_RELEASE);"));
	CHECK(has(rt, "__atomic_store_n(&gStarted, 1u, __ATOMIC_RELEASE);"));
	// 0025: the announce is proof by itself. The flag is stored before anything can return, so
	// an unarmed boot still reports it; the armed check only selects which block is printed.
	const size_t storeAt = rt.find("__atomic_store_n(&gStarted, 1u, __ATOMIC_RELEASE);");
	const size_t armedAt = rt.find("if (__atomic_load_n(&gArmed, __ATOMIC_ACQUIRE)) {");
	CHECK(storeAt != std::string::npos && armedAt != std::string::npos && storeAt < armedAt);
	CHECK(has(rt, "========== NAVI 44 TAKEOVER REPORT (no watcher armed) =========="));
	CHECK(has(rt, "n48::accel_takeover_headline(n48::TakeoverVerdict::Started)"));
	CHECK(has(rt, "n48::accel_takeover_verdict(__atomic_load_n(&gArmed, __ATOMIC_ACQUIRE) != 0,"));
	CHECK(has(rt, "print_block(v, waited, facts);"));
	CHECK(has(rt, "n48::TakeoverFacts gather_facts()"));
	CHECK(has(rt, "OSMetaClass::getMetaClassWithName(sym)"));
	CHECK(has(rt, "AMDRadeonX6000_AMDNavi21GraphicsAccelerator"));
	CHECK(has(rt, "getChildIterator(gIOServicePlane)"));
	CHECK(has(rt, "__atomic_store_n(&gProvider, (uintptr_t)provider, __ATOMIC_RELEASE);"));
	CHECK(has(rt, "n48::accel_takeover_diagnosis(f)"));
	// 0022: the injected id must be reported on a timeout; without it a spoof that never
	// reached the device is indistinguishable from Apple refusing the card.
	CHECK(has(rt, "injected_id(prov, \"device-id\")"));
	CHECK(has(rt, "card: device-id 0x%04x"));
	// The id line belongs to the negative branch: a takeover needs no excuse line.
	const size_t startedBranch = rt.find("if (v == n48::TakeoverVerdict::Started) {");
	const size_t idLine = rt.find("card: device-id 0x%04x");
	CHECK(startedBranch != std::string::npos && idLine != std::string::npos && idLine > startedBranch);
	// 0023: the TTL census is where Apple's driver went - the counters Navi48Ttl recorded during
	// the wait, printed on both verdicts before the screen is declared photographable. A verdict
	// without it says Apple failed but not at which of the 43 entry points.
	CHECK(has(rt, "#include \"apple/Navi48Ttl.hpp\""));
	CHECK(has(rt, "const uint32_t calls = gNavi48Ttl.totalCalls();"));
	// 0027's file writer reads the same counters for the report, so the screen's copy is pinned
	// inside the function that prints it rather than anywhere in the file.
	const std::string censusBody = body_of(rt, "void print_ttl_census() {", "uint16_t injected_id(");
	const std::string inputsBody = body_of(rt, "n48::ReportFileInputs gather_report_inputs() {",
	                                       "int write_report_file(");
	CHECK(!censusBody.empty() && !inputsBody.empty());
	CHECK(has(censusBody, "const int first = gNavi48Ttl.firstUnsupported();"));
	CHECK(has(censusBody, "gNavi48Ttl.bound() ? 1u : 0u"));
	CHECK(has(inputsBody, "const int first = gNavi48Ttl.firstUnsupported();"));
	CHECK(has(inputsBody, "in.ttlBound = gNavi48Ttl.bound() ? 1u : 0u;"));
	CHECK(has(rt, "for (int s = 0; s < 43; s++) if (gNavi48Ttl.callCount(s) != 0) touched++;"));
	CHECK(has(rt, "ttl: bound=%u calls=%u slots=%u/43 first unsupported=%d"));
	const size_t censusCall = rt.find("print_ttl_census();");
	const size_t censusDef = rt.find("void print_ttl_census() {");
	CHECK(censusDef != std::string::npos && censusCall != std::string::npos && censusDef < censusCall);
	CHECK(count_of(rt, "print_ttl_census();") == 2);   // print_block + the unarmed announce (0025)
	CHECK(censusCall > startedBranch);   // both verdicts carry it, not just the timeout
	CHECK(censusCall < rt.find("no terminal needed: photograph this screen"));
	CHECK(has(rt, "if (t >= n48::kTakeoverHold) break;"));
	// The positive side runs inside Apple's start(): it must not hold the screen for kTakeoverHold.
	const size_t markDef = rt.find("void takeover_mark_started(");
	const size_t nextDef = rt.find("void takeover_start_poller(");
	CHECK(markDef != std::string::npos && markDef < nextDef);
	CHECK(!has(rt.substr(markDef, nextDef - markDef), "IOSleep"));
	// ...and the unarmed block carries the census too, so a no-watcher boot still names where
	// Apple's driver went (0025).
	const size_t censusSecond = rt.find("print_ttl_census();", censusCall + 1);
	CHECK(censusSecond != std::string::npos && markDef < censusSecond && censusSecond < nextDef);
	// One block per boot, whichever signal wins the race.
	CHECK(has(rt, "__atomic_compare_exchange_n(&gBlockPrinted, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)"));

	// ---------------------------------------------------------------- D: header hygiene
	const std::string hdr = slurp(root + "/src/accel_takeover_report.h");
	CHECK(!hdr.empty());
	CHECK(!has(hdr, "IOKit"));
	CHECK(!has(hdr, "setProperty"));
	CHECK(!has(hdr, "IOLog("));
	CHECK(!has(hdr, "IOSleep"));
	CHECK(!has(hdr, "kernel_thread_start"));
	CHECK(count_of(hdr, "kTakeoverWaitSeconds") == 1);
	CHECK(count_of(hdr, "kTakeoverHold") == 1);
	CHECK(has(hdr, "case TakeoverVerdict::TimedOut:"));
	CHECK(has(hdr, "return reportRequested && published;"));
	CHECK(has(hdr, "accel_takeover_diagnosis"));

	if (gFailures == 0) printf("OK: %d checks, 0 failures\n", gChecks);
	else                printf("%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
