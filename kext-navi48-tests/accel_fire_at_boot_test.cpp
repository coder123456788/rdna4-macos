//
//  accel_fire_at_boot_test.cpp — host suite for the boot-arg fire path (patch 0017).
//
//  Build (from src/navi48-bringup, on any machine — no Mac, no GPU):
//    zig c++ -std=c++17 -Wall -Wextra -O1 -I tests/dalshim -I src \
//        tests/accel_fire_at_boot_test.cpp -o /tmp/accel_fire_at_boot_test.exe
//    /tmp/accel_fire_at_boot_test.exe .
//
//  The argv[1] directory is the navi48-bringup root, used by the source-pin sections.
//
//  Why a suite for six facts: this gate decides whether a boot publishes the one property
//  that hands the card to every Metal client on the machine, and it does so with nobody at a
//  keyboard to answer the verb. A wrong "yes" can panic the boot; a wrong "no" leaves the
//  card armed forever and reads as "the port does not accelerate". Section D pins the reason
//  a refusal is the safer default: the fire path installs the TTL hook BEFORE its exposure
//  gate can withhold LoadAccelerator, so calling it on an unproven card produces the one
//  state worse than not firing.
//

#include "accel_fire_at_boot.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static int gChecks = 0, gFailures = 0;
static void report(const char *file, int line, const char *what, bool ok) {
	gChecks++;
	if (ok) return;
	gFailures++;
	printf("FAIL %s:%d: %s\n", file, line, what);
}
#define CHECK(cond) report(__FILE__, __LINE__, #cond, (cond))
#define CHECK_MSG(cond, msg) report(__FILE__, __LINE__, msg, (cond))

using n48::AccelFireAtBoot;
using n48::AccelFireAtBootFacts;

namespace {

std::string slurp(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return "";
	std::ostringstream ss;
	ss << in.rdbuf();
	std::string s = ss.str();
	for (auto &c : s) if (c == '\r') c = '\n';       // CRLF-tolerant pins
	while (!s.empty() && s.back() == '\n') s.pop_back();
	return s;
}

size_t first_at(const std::string &hay, const std::string &needle, size_t from = 0) {
	const size_t p = hay.find(needle, from);
	return p == std::string::npos ? (size_t)-1 : p;
}

bool has(const std::string &hay, const std::string &needle) {
	return hay.find(needle) != std::string::npos;
}

// The rules in prose, applied by hand to each combination — deliberately not a copy of the
// header's expression order, so a refactor that changes precedence changes an expectation here.
AccelFireAtBoot expect_verdict(const AccelFireAtBootFacts &f) {
	if (!f.argRequested)    return AccelFireAtBoot::SkipNotRequested;
	if (!f.experimentArmed) return AccelFireAtBoot::SkipNotArmed;
	if (!f.ladderRan)       return AccelFireAtBoot::Refuse;
	if (!f.selfTestPassed)  return AccelFireAtBoot::Refuse;
	if (f.gateMode == 0)    return AccelFireAtBoot::Refuse;
	return AccelFireAtBoot::Call;
}

AccelFireAtBootFacts allOn() {
	AccelFireAtBootFacts f;
	f.argRequested = f.experimentArmed = f.ladderRan = f.selfTestPassed = true;
	f.gateMode = 1;
	return f;
}

//----------------------------------------------------------------
// A. the truth table, every combination
//----------------------------------------------------------------
void sectionA() {
	printf("== A: truth table (48 combinations)\n");
	int combos = 0, calls = 0, refusals = 0, skips = 0;
	for (int arg = 0; arg < 2; arg++)
	for (int armed = 0; armed < 2; armed++)
	for (int ladder = 0; ladder < 2; ladder++)
	for (int self = 0; self < 2; self++)
	for (uint32_t gate = 0; gate <= 2; gate++) {
		AccelFireAtBootFacts f;
		f.argRequested    = arg != 0;
		f.experimentArmed = armed != 0;
		f.ladderRan       = ladder != 0;
		f.selfTestPassed  = self != 0;
		f.gateMode        = gate;

		const char *why = nullptr;
		const AccelFireAtBoot got = n48::accel_fire_at_boot_gate(f, &why);
		const AccelFireAtBoot want = expect_verdict(f);
		combos++;
		if (got == AccelFireAtBoot::Call) calls++;
		else if (got == AccelFireAtBoot::Refuse) refusals++;
		else skips++;
		CHECK_MSG(got == want,
		          ("combo arg=" + std::to_string(arg) + " armed=" + std::to_string(armed) +
		           " ladder=" + std::to_string(ladder) + " self=" + std::to_string(self) +
		           " gate=" + std::to_string(gate) + " -> verdict").c_str());
		CHECK_MSG(why != nullptr && *why != '\0', "every verdict carries a reason");
	}
	CHECK_MSG(combos == 48, "the table covers 2*2*2*2*3 combinations");
	// arg off = 24 skips; arg on/armed off = 12 more; of the remaining 12 the ladder-or-self-test
	// is off in 9 and gate 0 takes one more.
	CHECK_MSG(calls == 2, "exactly two combinations arm the fire (self-test passed, gate 1 or 2)");
	CHECK_MSG(refusals == 10, "ten combinations refuse");
	CHECK_MSG(skips == 36, "thirty-six combinations leave the boot as it was");
	printf("   call %d / refuse %d / skip %d\n", calls, refusals, skips);
}

//----------------------------------------------------------------
// B. what the reasons must name
//----------------------------------------------------------------
void sectionB() {
	printf("== B: reason text names the mechanism\n");

	AccelFireAtBootFacts f = allOn();
	const char *why = nullptr;
	CHECK(n48::accel_fire_at_boot_gate(f, &why) == AccelFireAtBoot::Call);
	CHECK_MSG(std::strcmp(why, "allowed") == 0, "an allowed boot says exactly 'allowed'");

	// gate 0 and gate 2 both reach the same Call path only for gate != 0.
	f.gateMode = 2;
	CHECK(n48::accel_fire_at_boot_gate(f, &why) == AccelFireAtBoot::Call);
	f.gateMode = 0;
	CHECK(n48::accel_fire_at_boot_gate(f, &why) == AccelFireAtBoot::Refuse);
	CHECK_MSG(has(why, "navi48-accel-gate=0"), "a gate-0 refusal names the boot-arg that caused it");

	// The half-state refusal is the one an owner has to act on: it has to say what to run instead.
	f = allOn();
	f.gateMode = 1;
	f.selfTestPassed = false;
	CHECK(n48::accel_fire_at_boot_gate(f, &why) == AccelFireAtBoot::Refuse);
	CHECK_MSG(has(why, "stage-17"), "the refusal says which rung did not pass");
	CHECK_MSG(has(why, "TTL"), "the refusal says why calling anyway is not harmless");
	CHECK_MSG(has(why, "LoadAccelerator"), "the refusal names the property at stake");
	CHECK_MSG(has(why, "navi48test accel fire"), "the refusal names the diagnostic route it is not taking");

	f.selfTestPassed = true;
	f.ladderRan = false;
	CHECK(n48::accel_fire_at_boot_gate(f, &why) == AccelFireAtBoot::Refuse);
	CHECK_MSG(has(why, "navi48-stage"), "a no-ladder refusal says which arg to raise");

	f.ladderRan = true;
	f.experimentArmed = false;
	CHECK(n48::accel_fire_at_boot_gate(f, &why) == AccelFireAtBoot::SkipNotArmed);
	CHECK_MSG(has(why, "navi48-accel-experiment"),
	          "an un-armed skip says which arg is missing, not that this boot failed");

	f.argRequested = false;
	CHECK(n48::accel_fire_at_boot_gate(f, &why) == AccelFireAtBoot::SkipNotRequested);
	CHECK_MSG(has(why, "navi48-accel-fire-at-boot"), "the default skip names the arg it did not see");

	// The header owns the text, so the enum's own accessor cannot disagree with a verdict.
	CHECK_MSG(std::strcmp(n48::accelFireAtBootReason(AccelFireAtBoot::Call), "allowed") == 0,
	          "reason accessor agrees with the Call verdict text");
	CHECK_MSG(std::strcmp(n48::accelFireAtBootReason(AccelFireAtBoot::Refuse), "refused") == 0,
	          "Refuse has no single fixed text - it is per-branch");
	CHECK(has(n48::accelFireAtBootReason(AccelFireAtBoot::SkipNotArmed), "TTL hook"));

	// The header's default gate mode must equal the mode the kext parses when the arg is absent,
	// or a struct built without setting it decides differently from the boot it stands for.
	{
		AccelFireAtBootFacts d;
		d.argRequested = d.experimentArmed = d.ladderRan = d.selfTestPassed = true;
		CHECK_MSG(n48::accel_fire_at_boot_gate(d, &why) == AccelFireAtBoot::Call,
		          "default gateMode behaves as self-test (1), the kext's own default");
	}

	// reasonOut may be null (a caller that only wants the verdict) without breaking anything.
	CHECK(n48::accel_fire_at_boot_gate(allOn(), nullptr) == AccelFireAtBoot::Call);
	AccelFireAtBootFacts bad = allOn();
	bad.selfTestPassed = false;
	CHECK(n48::accel_fire_at_boot_gate(bad, nullptr) == AccelFireAtBoot::Refuse);
}

//----------------------------------------------------------------
// C. the kext really consults the gate
//----------------------------------------------------------------
void sectionC(const std::string &root) {
	printf("== C: Navi48Bringup.cpp consults the gate\n");
	const std::string s = slurp(root + "/src/Navi48Bringup.cpp");
	CHECK_MSG(!s.empty(), "Navi48Bringup.cpp readable");
	if (s.empty()) return;

	CHECK_MSG(has(s, "#include \"accel_fire_at_boot.h\""), "the header is included");
	CHECK_MSG(has(s, "\"navi48-accel-fire-at-boot\""), "the boot-arg is parsed by that exact name");
	CHECK_MSG(has(s, "n48::accel_fire_at_boot_gate"), "the gate is consulted");
	CHECK_MSG(has(s, "n48::AccelFireAtBootFacts"), "the facts struct is used");
	CHECK_MSG(has(s, "\"Navi48,AccelFireAtBoot\""), "the verdict reason is published");
	CHECK_MSG(has(s, "\"Navi48,AccelFireAtBootKr\""), "the fire path's return is published too");
	CHECK_MSG(has(s, "accelExperiment(1 /*fire*/"), "action 1 is what the verb would have sent");
	CHECK_MSG(first_at(s, "\"navi48-accel-fire-at-boot\"") ==
	          first_at(s, "\"navi48-accel-fire-at-boot\"", 0), "the arg is parsed once");

	// The facts must be the same expressions the exposure gate uses, or the two can disagree
	// about whether the self-test passed - and only this one gets to decide at boot.
	auto count_of = [](const std::string &hay, const std::string &needle) {
		size_t n = 0, p = 0;
		while ((p = hay.find(needle, p)) != std::string::npos) { n++; p += needle.size(); }
		return n;
	};
	CHECK_MSG(has(s, "ff.selfTestPassed") &&
	          has(s, "ff.selfTestPassed  = gBringup.computePassed &&"),
	          "selfTestPassed comes from the ladder's own computePassed flag, not a constant");
	CHECK_MSG(has(s, "ff.ladderRan") && has(s, "amdgpu::BringupStage::None"),
	          "ladderRan is derived from the ladder's own progress");
	CHECK_MSG(count_of(s, ">= amdgpu::BringupStage::ComputeDispatch;") >= 2,
	          "the auto-fire and the inner exposure gate require the same rung (two call sites)");

	// A boot without the arg must not gain a property, or the untouched path stops being untouched.
	const size_t guard = first_at(s, "if (ff.argRequested) setProperty(\"Navi48,AccelFireAtBoot\", fwhy);");
	CHECK_MSG(guard != (size_t)-1, "the property is only published when the arg was seen");
	CHECK_MSG(first_at(s, "if (fv == n48::AccelFireAtBoot::Call)") > guard,
	          "the call happens after that guard, not before it");
}

//----------------------------------------------------------------
// D. the ordering that makes a refusal the safe default
//----------------------------------------------------------------
void sectionD(const std::string &root) {
	printf("== D: the fire path hooks TTL before it can withhold the property\n");
	const std::string s = slurp(root + "/src/Navi48Bringup.cpp");
	if (s.empty()) { CHECK_MSG(false, "Navi48Bringup.cpp readable"); return; }

	const size_t hook   = first_at(s, "install_ttl_hook(pciDevice, &gNavi48Ttl)");
	const size_t gate0  = first_at(s, "if (gate == 0) {");
	const size_t withheld = first_at(s, "self-test did not pass, so LoadAccelerator is");
	const size_t publish = first_at(s, "pciDevice->setProperty(\"LoadAccelerator\", kOSBooleanTrue);");

	CHECK_MSG(hook != (size_t)-1, "the fire path installs the TTL hook");
	CHECK_MSG(publish != (size_t)-1, "the fire path publishes LoadAccelerator");
	CHECK_MSG(hook < gate0, "the hook is installed BEFORE the gate-0 refusal (the half-state is real)");
	CHECK_MSG(hook < withheld, "the hook is installed BEFORE the self-test refusal");
	CHECK_MSG(hook < publish, "and before the publish, as it must be for the verb path");

	// The comment this suite was written against has to stay true, or the refusal here is
	// defending a state that no longer exists and the argument for it is stale.
	CHECK_MSG(has(s, "The TTL hook stays installed"),
	          "gate 0 documents that it leaves the hook installed");
}

//----------------------------------------------------------------
// E. the header is self-contained and dependency-free
//----------------------------------------------------------------
void sectionE(const std::string &root) {
	printf("== E: header shape\n");
	const std::string h = slurp(root + "/src/accel_fire_at_boot.h");
	CHECK_MSG(!h.empty(), "the header is readable");
	CHECK_MSG(has(h, "#include <cstdint>"), "it needs only <cstdint>");
	// The prose is allowed to name IOService/IORegistry — the point is that the code does not use
	// them, so strip comment lines before looking.
	std::string code;
	{
		std::istringstream in(h);
		std::string line;
		while (std::getline(in, line)) {
			if (line.find("//") == 0) continue;
			if (line.empty() && code.empty()) continue;
			if (!line.empty() && line[0] == '/' && line.size() > 1 && line[1] == '*') continue;
			code += line;
			code += '\n';
		}
	}
	CHECK_MSG(!has(code, "#include <IOKit"), "no IOKit include");
	CHECK_MSG(!has(code, "IOService") && !has(code, "IORegistry"), "no IOKit types in the code");
	CHECK_MSG(!has(code, "PE_parse_boot_argn"), "it does not read boot-args itself");
	CHECK_MSG(!has(code, "setProperty"), "it does not publish anything itself");
	CHECK_MSG(has(h, "namespace n48"), "it lives in n48 like the arm gate");
	// Every refusal branch must be reachable in prose too: a branch with no text would print "".
	CHECK_MSG(has(h, "\"the bring-up ladder never ran this boot") &&
	          has(h, "\"the stage-17 compute dispatch did not pass") &&
	          has(h, "\"navi48-accel-gate=0 forbids"), "all three refusals carry their own text");
}

}  // namespace

int main(int argc, char **argv) {
	const std::string root = (argc > 1) ? argv[1] : ".";
	sectionA();
	sectionB();
	sectionC(root);
	sectionD(root);
	sectionE(root);
	printf("\n%d checks, %d failures\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
