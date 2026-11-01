//  accel_boot_report_test.cpp — host suite for the on-screen report (patch 0019).
//
//  Build (any machine, no Mac, no GPU):
//    zig c++ -std=c++17 -Wall -Wextra -O1 -I tests/dalshim -I src \
//        tests/accel_boot_report_test.cpp -o /tmp/accel_boot_report_test.exe
//    /tmp/accel_boot_report_test.exe .
//
//  argv[1] is the navi48-bringup root, used by the source-pin sections.
//
//  Why a suite for a log block: this is the only channel that survives a machine with no
//  keyboard - no mount, no terminal, no userspace binary. If the block stops printing, or the
//  headline stops agreeing with the verdict, the owner's photograph says the wrong thing and
//  the next patch is aimed at the wrong failure. The headline is therefore derived from the
//  gate's own verdict rather than from a second copy of the rules, and that is what section C
//  pins.

#include "accel_boot_report.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using n48::AccelBootReport;
using n48::AccelFireAtBoot;

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

// The rules in prose, written out by hand — deliberately not the header's switch order, so a
// reordered switch changes an expectation here.
const char *expect_headline(AccelFireAtBoot v, bool fired, int32_t kr, uint32_t gate) {
	switch (v) {
		case AccelFireAtBoot::SkipNotRequested:
			return "nothing was asked for: add navi48-accel-fire-at-boot=1 to boot-args";
		case AccelFireAtBoot::SkipNotArmed:
			return "not fired: navi48-accel-experiment=1 is absent, so the experiment was never armed";
		case AccelFireAtBoot::Refuse:
			return gate == 0
				? "REFUSED: navi48-accel-gate=0 forbids publishing - the card stays on the software path"
				: "REFUSED: the card was not handed to Apple this boot - the reason line below says why";
		case AccelFireAtBoot::Call:
			if (!fired) return "the gate said Call but the fire path did not run - nothing is hooked, investigate";
			if (kr != 0) return "the fire path returned an error - the TTL hook was NOT installed";
			if (gate == 2) return "LoadAccelerator PUBLISHED (gate forced open by navi48-accel-gate=2)";
			return "LoadAccelerator PUBLISHED - Apple's accelerator may now match this card";
	}
	return "";
}

} // namespace

int main(int argc, char **argv) {
	const std::string root = argc > 1 ? argv[1] : ".";

	// ---------------------------------------------------------------- A: the clamp
	CHECK(n48::accel_report_hold_seconds(0) == 0);
	CHECK(n48::accel_report_hold_seconds(1) == 1);
	CHECK(n48::accel_report_hold_seconds(19) == 19);
	CHECK(n48::accel_report_hold_seconds(20) == 20);
	CHECK(n48::accel_report_hold_seconds(21) == 20);
	CHECK(n48::accel_report_hold_seconds(600) == 20);
	CHECK(n48::accel_report_hold_seconds(0xFFFFFFFFu) == 20);

	// ---------------------------------------------------------------- B: when it prints
	CHECK(n48::accel_report_mode(false, 0)  == AccelBootReport::Off);
	CHECK(n48::accel_report_mode(false, 15) == AccelBootReport::Off);   // a hold cannot invent a fire
	CHECK(n48::accel_report_mode(false, 0xFFFFFFFFu) == AccelBootReport::Off);
	CHECK(n48::accel_report_mode(true, 0)   == AccelBootReport::Print);
	CHECK(n48::accel_report_mode(true, 1)   == AccelBootReport::PrintAndHold);
	CHECK(n48::accel_report_mode(true, 20)  == AccelBootReport::PrintAndHold);
	CHECK(n48::accel_report_mode(true, 0xFFFFFFFFu) == AccelBootReport::PrintAndHold);

	// ---------------------------------------------------------------- C: the headline
	const AccelFireAtBoot allv[] = { AccelFireAtBoot::SkipNotRequested, AccelFireAtBoot::SkipNotArmed,
	                                 AccelFireAtBoot::Refuse, AccelFireAtBoot::Call };
	const uint32_t gates[] = { 0, 1, 2 };
	const int32_t krs[] = { 0, -536870911 /*kIOReturnError*/, 1 };
	int n = 0;
	for (AccelFireAtBoot v : allv)
		for (uint32_t g : gates)
			for (int32_t kr : krs)
				for (int firedI = 0; firedI < 2; firedI++) {
					const bool fired = firedI != 0;
					const char *got = n48::accel_report_headline(v, fired, kr, g);
					const char *want = expect_headline(v, fired, kr, g);
					if (std::strcmp(got, want) != 0) {
						printf("FAIL headline v=%d g=%u kr=%d fired=%d\n  got  %s\n  want %s\n",
						       (int)v, g, kr, firedI, got, want);
					}
					CHECK(std::strcmp(got, want) == 0);
					CHECK(got && got[0]);          // never an empty screen
					n++;
				}
	CHECK(n == 4 * 3 * 3 * 2);

	// The published sentence must not be a re-print of the gate's internal wording: an owner
	// photographing the screen reads English, not a verdict enum name.
	CHECK(!has(n48::accel_report_headline(AccelFireAtBoot::Call, true, 0, 1), "AccelFireAtBoot"));
	CHECK(!has(n48::accel_report_headline(AccelFireAtBoot::Refuse, false, 0, 1), "Refuse"));
	CHECK(has(n48::accel_report_headline(AccelFireAtBoot::Call, true, 0, 1), "PUBLISHED"));
	CHECK(has(n48::accel_report_headline(AccelFireAtBoot::Call, true, -536870911, 1), "NOT installed"));
	CHECK(has(n48::accel_report_headline(AccelFireAtBoot::Call, false, 0, 1), "did not run"));
	CHECK(has(n48::accel_report_headline(AccelFireAtBoot::Refuse, false, 0, 0), "navi48-accel-gate=0"));
	CHECK(has(n48::accel_report_headline(AccelFireAtBoot::Refuse, false, 0, 1), "reason line"));
	CHECK(has(n48::accel_report_headline(AccelFireAtBoot::SkipNotArmed, false, 0, 1), "navi48-accel-experiment=1"));
	CHECK(has(n48::accel_report_headline(AccelFireAtBoot::Call, true, 0, 2), "forced open"));
	CHECK(!has(n48::accel_report_headline(AccelFireAtBoot::Call, true, 0, 1), "forced open"));

	// ---------------------------------------------------------------- D: xsrc wiring
	const std::string src = slurp(root + "/src/Navi48Bringup.cpp");
	CHECK(!src.empty());
	CHECK(has(src, "#include \"accel_boot_report.h\""));
	CHECK(has(src, "PE_parse_boot_argn(\"navi48-report-hold\", &holdRequested, sizeof(holdRequested));"));
	CHECK(has(src, "n48::accel_report_mode(ff.argRequested, holdRequested)"));
	CHECK(has(src, "n48::accel_report_headline(fv, fireCalled, fireKr, ff.gateMode)"));
	CHECK(has(src, "if (rmode != n48::AccelBootReport::Off)"));
	CHECK(has(src, "IOSleep(1000);"));
	CHECK(has(src, "for (uint32_t t = 0; ; t++) {"));
	CHECK(has(src, "N48LOG(\"%s\", head);"));
	CHECK(has(src, "if (t >= secs) break;"));
	CHECK(has(src, "setProperty(\"Navi48,AccelReport\", head);"));
	CHECK(has(src, "setProperty(\"Navi48,AccelReportHold\", static_cast<uint64_t>(secs), 32);"));
	CHECK(has(src, "NAVI 44 ACCELERATION REPORT %u/%u"));
	CHECK(has(src, "photograph this screen"));
	CHECK(has(src, "ttl installed %llu; gate reason: %s"));
	CHECK(has(src, "n48::accel_report_hold_seconds(holdRequested)"));
	// The report must be derived from what the fire call actually returned, not from the
	// verdict alone: a Call that errored has to say so, and fireKr is only set on that path.
	CHECK(has(src, "fireKr = (int32_t)fkr;"));
	CHECK(has(src, "fireCalled = true;"));
	// ...and it must print AFTER the call, or a hold would stall the screen before the answer
	// it is holding up exists.
	CHECK(src.find("const IOReturn fkr = accelExperiment(1 /*fire*/") <
	      src.find("n48::accel_report_mode(ff.argRequested, holdRequested)"));

	// ---------------------------------------------------------------- E: header hygiene
	const std::string hdr = slurp(root + "/src/accel_boot_report.h");
	CHECK(!hdr.empty());
	CHECK(has(hdr, "kAccelReportHoldCap = 20"));
	CHECK(has(hdr, "#include \"accel_fire_at_boot.h\""));
	CHECK(!has(hdr, "IOKit"));
	CHECK(!has(hdr, "setProperty"));
	CHECK(!has(hdr, "IOLog("));
	CHECK(has(hdr, "case AccelFireAtBoot::Refuse:"));
	// The headline must not grow a hardcoded verdict for the self-test case: the gate owns that
	// decision, and a second copy of it here is exactly how the two drift apart.
	CHECK(!has(hdr, "selfTestPassed"));

	// The cap must be the one the suite tested: a header that says 20 while start() sleeps for
	// a different number is the kind of quiet mismatch this pins.
	CHECK(count_of(hdr, "kAccelReportHoldCap") == 3);   // declaration + the comparison twice

	if (gFailures == 0) printf("OK: %d checks, 0 failures\n", gChecks);
	else                printf("%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
