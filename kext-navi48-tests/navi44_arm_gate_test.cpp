//
//  navi44_arm_gate_test.cpp — host suite for patch 0012's writing-stage gate.
//
//  Build (from src/navi48-bringup, on any machine — no Mac, no GPU):
//    zig c++ -std=c++17 -Wall -Wextra -O1 -I tests/dalshim -I src \
//        tests/navi44_arm_gate_test.cpp -o /tmp/navi44_arm_gate_test.exe
//    /tmp/navi44_arm_gate_test.exe .
//
//  The argv[1] directory is the navi48-bringup root, used by the source-pin section.
//
//  Why a suite for a dozen lines of if: the gate is the difference between a Navi 44
//  boot that can reach the PSP and one that cannot, and it is decided by facts the
//  survey gathers. The wrong shape of that decision is not a crash — it is a stage that
//  silently loads the other die's firmware and then fails three stages later with a
//  status code that names nothing. So every branch is executed here, and the pins in
//  section C/D check the two things a pure function cannot see for itself: that the
//  kext really consults it, and that our vendored clear-state table has not drifted from
//  upstream's.
//

#include "navi44_arm_gate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int gChecks = 0, gFailures = 0;
static void report(const char *file, int line, const char *what, bool ok) {
	gChecks++;
	if (ok) return;
	gFailures++;
	printf("FAIL %s:%d: %s\n", file, line, what);
}
#define CHECK(cond) report(__FILE__, __LINE__, #cond, (cond))
#define CHECK_MSG(cond, msg) report(__FILE__, __LINE__, msg, (cond))

using n48::Navi44Arm;
using n48::Navi44ArmFacts;

// A Navi 44 that has answered every question the survey asks. Everything the gate wants
// to see is on; individual cases switch one fact off.
static Navi44ArmFacts healthy() {
	Navi44ArmFacts f {};
	f.matchedNavi44 = true;
	f.requestedStage = 5;
	f.discoveryValid = true;
	f.gcVersionKnown = true;
	f.gcMajor = 12; f.gcMinor = 0; f.gcRev = 0;
	f.fwSetFromDiscovery = true;
	f.sosBlobPresent = true;
	return f;
}

static const char *run(const Navi44ArmFacts &f) {
	const char *why = nullptr;
	Navi44Arm v = navi44_arm_gate(f, &why);
	(void)v;
	return why;
}

static Navi44Arm verdict(const Navi44ArmFacts &f) {
	return navi44_arm_gate(f, nullptr);
}

// ---------------------------------------------------------------------------
// A. the gate itself
// ---------------------------------------------------------------------------
static void sectionA() {
	printf("== A: gate truth table ==\n");

	// A1: Navi 48 is not this gate's business. This is the case that must not regress:
	// the RX 9070 XT has been armed by boot-arg for hundreds of builds.
	{
		Navi44ArmFacts f = healthy();
		f.matchedNavi44 = false;
		f.discoveryValid = false;
		f.fwSetFromDiscovery = false;
		f.sosBlobPresent = false;
		CHECK(verdict(f) == Navi44Arm::Allow);
		CHECK(std::strcmp(run(f), "not Navi 44 - existing behaviour") == 0);
	}

	// A2: nothing asked -> not a refusal. targetStage == 0 is the default boot, and a
	// gate that reported Refuse there would spam a reason into the log on every card.
	{
		Navi44ArmFacts f = healthy();
		f.requestedStage = 0;
		CHECK(verdict(f) == Navi44Arm::NothingAsked);
	}

	// A3: the healthy Navi 44 arms. Without this case passing, 0012 has not moved the
	// project at all - it is the entire point of the patch.
	{
		Navi44ArmFacts f = healthy();
		CHECK(verdict(f) == Navi44Arm::Allow);
		CHECK_MSG(std::strcmp(run(f), "allowed") == 0, "healthy Navi 44 must be allowed");
	}

	// A4: no discovery table and no override -> refuse, and name the override.
	{
		Navi44ArmFacts f = healthy();
		f.discoveryValid = false;
		CHECK(verdict(f) == Navi44Arm::Refuse);
		CHECK(std::strstr(run(f), "navi44-force=1") != nullptr);
		f.forceOverride = true;
		CHECK_MSG(verdict(f) != Navi44Arm::Refuse,
		          "navi44-force=1 must carry the no-discovery case (SOS blob is still checked)");
	}

	// A5: a die that matches the Navi 44 PCI id but reports a GC version that is not
	// 12.0.0 is not a card this port has numbers for.
	{
		Navi44ArmFacts f = healthy();
		f.gcRev = 1;   // 12.0.1 is Navi 48's GC: driving it with Navi 44 constants is wrong
		const char *why = run(f);
		CHECK(verdict(f) == Navi44Arm::Refuse);
		CHECK(std::strstr(why, "12.0.0") != nullptr);
		f.forceOverride = true;
		CHECK_MSG(verdict(f) == Navi44Arm::Refuse,
		          "the override must NOT arm a die whose GC version is unknown - that is a "
		          "register-table problem, not a firmware-set gamble");
	}

	// A5b: but GC absent from discovery (gcVersionKnown false) is not a contradiction -
	// it is the same unknown as A4, and the override carries it.
	{
		Navi44ArmFacts f = healthy();
		f.gcVersionKnown = false;
		CHECK(verdict(f) == Navi44Arm::Allow);
	}

	// A6: discovery rejected the PCI-id pick -> refuse, because the blobs would be the
	// other die's. Override allowed (owner watching the console).
	{
		Navi44ArmFacts f = healthy();
		f.fwSetFromDiscovery = false;
		CHECK(verdict(f) == Navi44Arm::Refuse);
		CHECK(std::strstr(run(f), "other die") != nullptr);
		f.forceOverride = true;
		CHECK(verdict(f) == Navi44Arm::Allow);
	}

	// A7: no SOS blob is fatal even under the override. Loading nothing is not a risk
	// anyone should be allowed to take, and the kext would fail later with a vaguer
	// message than this one.
	{
		Navi44ArmFacts f = healthy();
		f.sosBlobPresent = false;
		CHECK(verdict(f) == Navi44Arm::Refuse);
		f.forceOverride = true;
		CHECK_MSG(verdict(f) == Navi44Arm::Refuse, "no override for a missing firmware blob");
	}

	// A8: every verdict carries a reason, and no two refusals share one - otherwise the
	// registry property PSP,GateReason cannot tell the owner which precondition to fix.
	{
		Navi44ArmFacts a = healthy(); a.discoveryValid = false;
		Navi44ArmFacts b = healthy(); b.gcRev = 1;
		Navi44ArmFacts c = healthy(); c.fwSetFromDiscovery = false;
		Navi44ArmFacts d = healthy(); d.sosBlobPresent = false;
		const char *ra = run(a), *rb = run(b), *rc = run(c), *rd = run(d);
		const char *ok = run(healthy());   // the reason an Allow carries
		CHECK(ra && rb && rc && rd);
		CHECK(ra && rb && std::strcmp(ra, rb) != 0);
		CHECK(ra && rc && std::strcmp(ra, rc) != 0);
		CHECK(rd && std::strcmp(ra, rd) != 0 && std::strcmp(rb, rd) != 0 &&
		      std::strcmp(rc, rd) != 0);
		// And none of them may be the allow text: PSP,GateReason is read by the owner at
		// the console, and a refusal that says "allowed" is worse than no reason at all.
		CHECK(ok && ra && std::strcmp(ra, ok) != 0);
		CHECK(ok && rb && std::strcmp(rb, ok) != 0);
		CHECK(ok && rc && std::strcmp(rc, ok) != 0);
		CHECK_MSG(ok && rd && std::strcmp(rd, ok) != 0,
		          "a refusal cannot borrow the allow reason");
	}

	// A9: default-constructed facts are "some other card, nothing learned yet" - which must
	// land on the pre-0012 path untouched. The Navi 44-shaped version of the same empty
	// facts must NOT arm: a die labelled 0x7590 with no discovery and no confirmed
	// firmware set is exactly the guess the gate exists to stop.
	{
		Navi44ArmFacts f {};
		f.requestedStage = 5;
		CHECK_MSG(verdict(f) == Navi44Arm::Allow,
		          "an unlabelled die (matchedNavi44=false) stays on the existing path");
		f.matchedNavi44 = true;
		CHECK_MSG(verdict(f) == Navi44Arm::Refuse,
		          "a Navi 44 with nothing confirmed by discovery is refused");
	}

	// A10: reasonOut == nullptr must be safe (the kext logs the verdict before it needs
	// the text in one call path).
	{
		Navi44ArmFacts f = healthy();
		f.discoveryValid = false;
		CHECK(verdict(f) == Navi44Arm::Refuse);
	}
}

// ---------------------------------------------------------------------------
// B. the clear-state table, against upstream
// ---------------------------------------------------------------------------
// Patch 0012 settled an open question offline: drivers/gpu/drm/amd/amdgpu/clearstate_gfx12.h
// is 6 SECT_CONTEXT clusters totalling 75 dwords (1 + sum(2 + reg_count)) - and every
// register value in it is 0, so no Navi 48 constant can leak into it and Navi 44 needs no
// different table. Upstream's layout (gfx_v12_0.c:681-730) is re-implemented here
// independently of our port, then compared against the table rlc_v12_0.cpp vendored.
static void sectionB(const std::string &root) {
	printf("== B: gfx12 clear state vs upstream ==\n");

	struct Cluster { uint32_t regIndex; uint32_t count; };
	const Cluster upstream[] = {
		{ 0x0000a03e, 34 }, { 0x0000a0cc, 2 }, { 0x0000a0d8, 1 },
		{ 0x0000a0db, 6 }, { 0x0000a2e5, 11 }, { 0x0000a3c0, 8 },
	};
	// Upstream's own arithmetic, written out rather than hard-coded, so a future
	// upstream change to the algorithm shows up as a diff instead of a wrong constant.
	uint32_t dwords = 1;
	for (const auto &c : upstream) dwords += 2 + c.count;
	CHECK_MSG(dwords == 75, "the 75-dword CSB is derived from the table, not remembered");

	std::string src;
	{
		std::ifstream in(root + "/src/amd/rlc_v12_0.cpp", std::ios::binary);
		CHECK_MSG(in.good(), "can read src/amd/rlc_v12_0.cpp");
		std::ostringstream ss; ss << in.rdbuf(); src = ss.str();
		src.erase(std::remove(src.begin(), src.end(), '\r'), src.end());
	}
	if (src.empty()) return;

	// Every cluster line our port must carry, exactly as upstream has it - parsed out of
	// our table rather than string-matched, so a wrong reg_count is caught: the dword
	// total is what gets programmed into RLC_CSIB_LENGTH, and a cluster whose count lies
	// makes the hardware read past the buffer or leave registers uncleared.
	std::vector<Cluster> ours;
	{
		std::istringstream in(src);
		std::string line;
		while (std::getline(in, line)) {
			const char *p = std::strstr(line.c_str(), "{ kGfx12_SECT_CONTEXT_def_");
			if (!p) continue;
			uint32_t base = 0, cnt = 0;
			if (std::sscanf(p, "{ kGfx12_SECT_CONTEXT_def_%*u, 0x%x, %u }", &base, &cnt) == 2)
				ours.push_back(Cluster{ base, cnt });
		}
	}
	CHECK_MSG(ours.size() == sizeof(upstream) / sizeof(upstream[0]),
	          "our table has the same six clusters as upstream's");
	if (ours.size() == sizeof(upstream) / sizeof(upstream[0])) {
		for (size_t i = 0; i < ours.size(); i++) {
			CHECK_MSG(ours[i].regIndex == upstream[i].regIndex, "cluster register base matches");
			CHECK_MSG(ours[i].count == upstream[i].count, "cluster register count matches");
		}
		uint32_t oursDwords = 1;
		for (const auto &c : ours) oursDwords += 2 + c.count;
		CHECK_MSG(oursDwords == dwords, "our table computes to the same CSB length as upstream's");
	}
	CHECK_MSG(src.find("kGfx12_cs_data") != std::string::npos, "the table is named as upstream's");

	// And the count the kext programs into RLC_CSIB_LENGTH comes out of the same arithmetic.
	CHECK_MSG(src.find("rlc_get_csb_size(kGfx12_cs_data)") != std::string::npos,
	          "clear_state_dwords is computed from the table, not hard-coded to 75");
}

// ---------------------------------------------------------------------------
// C. the kext really consults the gate
// ---------------------------------------------------------------------------
// A pure function can be correct and unused. These pins are what the plant script
// mutates: they are the only way this suite sees stagePSP() and probe() at all.
static void sectionC(const std::string &root) {
	printf("== C: the kext consults the gate ==\n");

	auto slurp = [&](const char *rel) {
		std::ifstream in(std::string(root) + rel, std::ios::binary);
		std::ostringstream ss; ss << in.rdbuf();
		std::string s = ss.str();
		s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
		return s;
	};

	const std::string cpp = slurp("/src/Navi48Bringup.cpp");
	CHECK_MSG(!cpp.empty(), "read Navi48Bringup.cpp");
	if (cpp.empty()) return;

	CHECK_MSG(cpp.find("#include \"navi44_arm_gate.h\"") != std::string::npos,
	          "the gate header is included");
	CHECK_MSG(cpp.find("navi44_arm_gate(facts") != std::string::npos,
	          "stagePSP() calls the gate");
	CHECK_MSG(cpp.find("facts.matchedNavi44      = navi44ByPciId") != std::string::npos,
	          "the gate is fed probe()'s Navi 44 label");
	CHECK_MSG(cpp.find("navi44ByPciId = true;") != std::string::npos,
	          "probe() labels the 0x7590 match");
	CHECK_MSG(cpp.find("navi44-gate-refused") != std::string::npos,
	          "a refusal is published as a PSP,Stage value");
	CHECK_MSG(cpp.find("PSP,GateReason") != std::string::npos,
	          "the refusal text reaches the registry");

	// The claim 0012 exists to remove. If it comes back, someone re-broke the doc AND
	// probably the arming path.
	CHECK_MSG(cpp.find("SURVEY ONLY, no writing stage armed") == std::string::npos,
	          "the old \"arms NOTHING\" claim must stay gone");

	// Order matters, and the check has to see an EARLIER call too: the gate must run
	// before mapVramAperture() retunes BAR0, so it is anchored on the start of stagePSP()
	// rather than on the gate's own position.
	const size_t stageAt = cpp.find("void Navi48Bringup::stagePSP()");
	const size_t gateAt = cpp.find("navi44_arm_gate(facts");
	const size_t bar0First = cpp.find("mapVramAperture())", stageAt);
	CHECK_MSG(stageAt != std::string::npos && gateAt != std::string::npos &&
	              bar0First != std::string::npos && stageAt < gateAt && gateAt < bar0First,
	          "the gate sits above the first BAR0 side effect in stagePSP()");

	// The facts must be read, not assumed. Each of these is planted by
	// navi44_arm_gate_plant.sh: a gate fed a constant is a gate that always agrees.
	CHECK_MSG(cpp.find("facts.discoveryValid     = ipDiscovery.isValid();") != std::string::npos,
	          "discovery validity comes from the parsed table");
	CHECK_MSG(cpp.find("facts.gcVersionKnown = true;") != std::string::npos,
	          "GC version is marked known only inside the findIp() success path");
	CHECK_MSG(cpp.find("ipDiscovery.findIp(IpDiscovery::HwGc, 0, gcEntry)") != std::string::npos,
	          "GC version is read from IP discovery");
	CHECK_MSG(cpp.find("facts.sosBlobPresent = sosProbe && sosProbe->data && sosProbe->size != 0;")
	              != std::string::npos,
	          "the SOS blob check looks at the pointer, the bytes and the size");
	CHECK_MSG(cpp.find("PE_parse_boot_argn(\"navi44-force\", &force, sizeof(force))") != std::string::npos,
	          "the override is read from the documented boot-arg name");
	size_t reasonProps = 0, from = 0;
	while ((from = cpp.find("setProperty(\"PSP,GateReason\", why);", from)) != std::string::npos) {
		reasonProps++; from += 1;
	}
	CHECK_MSG(reasonProps == 2, "both the refusal and the Navi 44 allow publish the reason");

	// FW,SetSource is now two states with a variable behind them, not just a string.
	const std::string selTrue = "fwSetConfirmed = true;";
	const std::string selFalse = "fwSetConfirmed = false;";
	CHECK_MSG(cpp.find(selTrue) != std::string::npos, "selectFwFromDiscovery records confirmation");
	CHECK_MSG(cpp.find(selFalse) != std::string::npos, "...and records refusal");

	const std::string hpp = slurp("/src/Navi48Bringup.hpp");
	CHECK_MSG(hpp.find("bool               navi44ByPciId { false };") != std::string::npos,
	          "the members exist and default to off");
	CHECK_MSG(hpp.find("bool               fwSetConfirmed { false };") != std::string::npos,
	          "fwSetConfirmed defaults to off");

	// The stale-comment fix in cp_v12_0.cpp: gfx12 DOES have a clearstate table.
	const std::string cp = slurp("/src/amd/cp_v12_0.cpp");
	CHECK_MSG(cp.find("there is no gfx12_cs_data table in the tree") == std::string::npos,
	          "the false \"no gfx12_cs_data table\" claim is gone");
	CHECK_MSG(cp.find("clearstate_gfx12.h") != std::string::npos,
	          "cp_v12_0.cpp names the real upstream table");
}

// ---------------------------------------------------------------------------
// D. 0016: the compute ladder on a die the shader was not assembled for
// ---------------------------------------------------------------------------
// Stage 17 is the only rung that runs code this port wrote itself, and the blob was
// assembled for the other die. Two things have to be true for that to be honest, and
// one of them is not a claim anyone can eyeball: the opcodes. So the whitelist below is
// derived from the .s sources on every run — add a gfx12_1-class instruction to
// store_magic.s and this suite fails, instead of a Navi 44 boot hanging four stages
// later with a log that names nothing.
static void sectionD(const std::string &root) {
	printf("== D: the stage-17 shader blob on GC 12.0.0 ==\n");

	auto slurp = [&](const char *rel) {
		std::ifstream in(std::string(root) + rel, std::ios::binary);
		std::ostringstream ss; ss << in.rdbuf();
		std::string s = ss.str();
		s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
		return s;
	};

	// D1: the gate's own truth table for the new branch.
	{
		Navi44ArmFacts f = healthy();
		f.requestedStage = 16;
		f.computeBlobBytes = 0;
		CHECK_MSG(verdict(f) == Navi44Arm::Allow,
		          "stage 16 (PM4Test) uploads no shader, so a 0-byte blob is not its problem");
		f.requestedStage = 17;
		CHECK_MSG(verdict(f) == Navi44Arm::Refuse,
		          "stage 17 with a 0-byte blob is refused");
		const char *why = run(f);
		CHECK_MSG(std::strstr(why, "0 bytes") != nullptr, "and the refusal says 0 bytes");
		f.forceOverride = true;
		CHECK_MSG(verdict(f) == Navi44Arm::Refuse,
		          "navi44-force=1 does not buy back a shader that does not exist");
		f.forceOverride = false;
		f.computeBlobBytes = 32;
		CHECK_MSG(verdict(f) == Navi44Arm::Allow, "a real blob arms stage 17");
		// A Navi 48 boot is never gated on this either — same rule as the rest of the gate.
		Navi44ArmFacts n48 = f;
		n48.matchedNavi44 = false;
		n48.computeBlobBytes = 0;
		CHECK_MSG(verdict(n48) == Navi44Arm::Allow, "the RX 9070 XT keeps its behaviour");
		// Nothing asked for: the stage-17 branch must not fire on a survey boot.
		Navi44ArmFacts survey = healthy();
		survey.requestedStage = 0;
		survey.computeBlobBytes = 0;
		CHECK_MSG(verdict(survey) == Navi44Arm::NothingAsked,
		          "a survey boot stays 'nothing asked for'");
	}

	// D2: the note the kext publishes has to say both halves of the argument.
	{
		const std::string note = n48::navi44ComputeIsaNote();
		CHECK_MSG(note.find("gfx1201") != std::string::npos, "the note names the assembly target");
		CHECK_MSG(note.find("12.0.0") != std::string::npos, "the note names the die it runs on");
		CHECK_MSG(note.find("gfx_v12_0.c") != std::string::npos,
		          "the note cites the Linux source the fall-through claim came from");
	}

	// D3: the opcode census — the load-bearing check.
	for (const char *rel : {"/shaders/store_magic.s", "/shaders/store_magic_hsa.s"}) {
		const std::string src = slurp(rel);
		CHECK_MSG(!src.empty(), "the shader source is readable");
		if (src.empty()) continue;
		// The two spellings differ (hand-written uses `--gfx1201`, LLVM's clang emits
		// `-unknown-gfx1201`), so what is pinned is that the target line names gfx1201.
		CHECK_MSG(src.find(".amdgcn_target") != std::string::npos &&
		          src.find("gfx1201\"") != std::string::npos,
		          "and really is assembled for gfx1201, as the note claims");

		static const char *kBase[] = {
			"v_and_b32", "v_lshlrev_b32", "v_add_nc_u32", "global_store_b32",
			"s_wait_storecnt", "global_wb", "s_endpgm", "s_load_b64",
			"v_add_nc_u32_e32", "v_lshlrev_b32_e32", "s_wait_kmcnt",
		};
		int seen = 0;
		std::istringstream in(src);
		std::string line;
		while (std::getline(in, line)) {
			size_t p = line.find_first_not_of(" \t");
			if (p == std::string::npos) continue;
			// Directives, labels, comments and register-units are not instructions.
			char c = line[p];
			if (c == '.' || c == ';' || c == '/' || c == '#') continue;
			size_t e = line.find_first_of(" \t,", p);
			std::string tok = line.substr(p, e - p);
			if (tok.size() > 1 && tok.back() == ':') continue;  // label definition
			if (tok.compare(0, 3, "s[0") == 0 || tok.find('=') != std::string::npos) continue;
			if (std::strchr("vsg", c) == nullptr && tok != "global_store_b32" &&
			    tok != "global_wb") continue;
			const bool base = std::any_of(std::begin(kBase), std::end(kBase),
			                             [&](const char *b) { return tok == b; });
			CHECK_MSG(base, ("opcode " + tok + " is not in the GFX12 base whitelist").c_str());
			if (base) seen++;
		}
		CHECK_MSG(seen >= 5, "the census actually looked at instructions");
	}

	// D4: the kext feeds the new facts and publishes them; the dispatch log names the die.
	{
		const std::string cpp = slurp("/src/Navi48Bringup.cpp");
		CHECK_MSG(!cpp.empty(), "read Navi48Bringup.cpp");
		CHECK_MSG(cpp.find("facts.computeBlobBytes = static_cast<uint32_t>(") != std::string::npos,
		          "the blob size is read, not assumed");
		CHECK_MSG(cpp.find("hsaAbi ? amdgpu::fw_shader_store_magic_hsa_size : amdgpu::fw_shader_store_magic_size")
		          != std::string::npos,
		          "and read from the two real blob symbols, not from a constant");
		CHECK_MSG(cpp.find("PE_parse_boot_argn(\"navi48-hsa-abi\", &hsaAbi, sizeof(hsaAbi))")
		          != std::string::npos,
		          "the gate judges the ABI this boot selected, the same arg the ladder reads");
		CHECK_MSG(cpp.find("extern const size_t fw_shader_store_magic_size;") != std::string::npos,
		          "the blob's own declaration is in scope");
		CHECK_MSG(cpp.find("setProperty(\"Navi44,ComputeBlob\", facts.computeBlobName)")
		          != std::string::npos, "the blob is published by name");
		CHECK_MSG(cpp.find("setProperty(\"Navi44,ComputeIsaNote\", n48::navi44ComputeIsaNote())")
		          != std::string::npos, "and the ISA decision is published as one owned string");
		CHECK_MSG(cpp.find("Navi44,ComputeBlobBytes") != std::string::npos,
		          "the byte count reaches the registry, where collect-survey.sh can see it");

		const std::string ct = slurp("/src/amd/compute_test.cpp");
		CHECK_MSG(ct.find("assembled for gfx1201, running on GC %u.%u.%u") != std::string::npos,
		          "the dispatch log names the blob target and the die together");
		CHECK_MSG(ct.find("dev.ip.getVersion(IPBlock::GC)") != std::string::npos,
		          "and takes the die from IP discovery, not from a boot-arg");
	}
}

int main(int argc, char **argv) {
	const std::string root = argc > 1 ? argv[1] : ".";
	sectionA();
	sectionB(root);
	sectionC(root);
	sectionD(root);
	printf("\n%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
