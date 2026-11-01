//
//  ttl_gc_topo_test.cpp — host suite for TTL slot 14's geometry decision (patch 0018).
//
//  Build (from src/navi48-bringup, on any machine — no Mac, no GPU):
//    zig c++ -std=c++17 -Wall -Wextra -O1 -I tests/dalshim -I src \
//        tests/ttl_gc_topo_test.cpp -o /tmp/ttl_gc_topo_test.exe
//    /tmp/ttl_gc_topo_test.exe .
//
//  The argv[1] directory is the navi48-bringup root, used by the source-pin sections.
//
//  Why: slot 14 is the answer Apple builds its CU mask, ring assignment and work-group
//  limits out of, and until now an unmeasured die got the Navi 48 numbers 4 / 2 / 8 with a
//  log warning. On a 12.0.0 card that is Apple told it has 64 CUs when it has 32 — and the
//  path that reaches it (GFXConfig never populated) is the path a hanging boot takes. The
//  suite checks the die-specific arithmetic against the shapes written in amdgpu_gfx.h, not
//  against the header's own expressions.
//

#include "apple/ttl_gc_topo.h"

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

using n48::TtlGcTopo;
using n48::TtlGcTopoFacts;
using n48::TtlGcTopoAnswer;

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

bool has(const std::string &hay, const std::string &needle) {
	return hay.find(needle) != std::string::npos;
}

size_t count_of(const std::string &hay, const std::string &needle) {
	size_t n = 0, p = 0;
	while ((p = hay.find(needle, p)) != std::string::npos) { n++; p += needle.size(); }
	return n;
}

// A full map for a die of se x sa arrays holding cu CUs each.
TtlGcTopoFacts measured(uint32_t se, uint32_t sa, uint32_t cuPerSh, uint32_t rb = 0xF) {
	TtlGcTopoFacts f;
	f.inited = true;
	f.maxShaderEngines = se;
	f.maxShPerSe = sa;
	f.maxCuPerSh = cuPerSh;
	f.globalRbBitmap = rb;
	const uint32_t mask = (cuPerSh >= 32) ? 0xFFFFFFFFu : ((1u << cuPerSh) - 1u);
	for (uint32_t i = 0; i < se && i < n48::kTtlGcTopoStructRows; i++)
		for (uint32_t j = 0; j < sa && j < n48::kTtlGcTopoStructCols; j++)
			f.cuBitmap[i][j] = mask;
	return f;
}

// The rules in prose, applied by hand — deliberately not a copy of the header's order.
TtlGcTopo expect_verdict(const TtlGcTopoFacts &f) {
	if (!f.inited) return TtlGcTopo::RefuseNotInited;
	if (f.maxShaderEngines == 0 || f.maxShPerSe == 0 || f.maxCuPerSh < 2)
		return TtlGcTopo::RefuseNoShape;
	if (f.maxShaderEngines > 4 || f.maxShPerSe > 2) return TtlGcTopo::RefuseTooWide;
	uint32_t any = 0;
	for (uint32_t i = 0; i < 4; i++)
		for (uint32_t j = 0; j < 2; j++) any |= f.cuBitmap[i][j];
	if (any == 0) return TtlGcTopo::RefuseNoHarvestMap;
	return TtlGcTopo::Answer;
}

// ---------------------------------------------------------------- A: verdicts
void sectionA() {
	printf("== A: which facts get an answer, which get a refusal ==\n");
	// The two real dies and the four refusal preconditions, each on its own axis.
	const TtlGcTopoFacts cases[] = {
		measured(4, 2, 8),                       // Navi 48 as surveyed
		measured(2, 2, 8),                       // Navi 44 at full width
		measured(1, 1, 2),                       // the smallest die that answers
		measured(4, 2, 8),                       // duplicate on purpose: grid below
	};
	for (const auto &f : cases) {
		TtlGcTopoAnswer a; const char *why = "";
		const TtlGcTopo v = n48::ttl_gc_topology(f, &a, &why);
		CHECK(v == expect_verdict(f));
		CHECK(v == TtlGcTopo::Answer);
		CHECK(std::strcmp(why, n48::ttlGcTopoReason(v)) == 0);
	}

	TtlGcTopoFacts unmeasured;                    // every field default
	TtlGcTopoAnswer a; const char *why = "";
	CHECK(n48::ttl_gc_topology(unmeasured, &a, &why) == TtlGcTopo::RefuseNotInited);
	CHECK(has(why, "never populated"));
	CHECK(has(why, "Navi 48"));
	CHECK(has(why, "4x2x8"));
	CHECK(a.shaderEngines == 0 && a.totalCus == 0);   // a refusal leaves the answer alone

	// A die measured but with each shape field zeroed in turn.
	for (uint32_t which = 0; which < 3; which++) {
		TtlGcTopoFacts f = measured(4, 2, 8);
		if (which == 0) f.maxShaderEngines = 0;
		if (which == 1) f.maxShPerSe = 0;
		if (which == 2) f.maxCuPerSh = 0;
		CHECK_MSG(n48::ttl_gc_topology(f, &a, &why) == TtlGcTopo::RefuseNoShape,
		          "a zeroed shape field must refuse, not substitute");
		CHECK(has(why, "SE=0"));
	}
	// CU/SH == 1 is the same class: half a WGP is not a shape.
	TtlGcTopoFacts one = measured(4, 2, 8); one.maxCuPerSh = 1;
	CHECK(n48::ttl_gc_topology(one, &a, &why) == TtlGcTopo::RefuseNoShape);

	// Wider than the struct: the old code clamped these down and answered.
	TtlGcTopoFacts wide_se = measured(4, 2, 8); wide_se.maxShaderEngines = 5;
	TtlGcTopoFacts wide_sa = measured(4, 2, 8); wide_sa.maxShPerSe = 3;
	CHECK(n48::ttl_gc_topology(wide_se, &a, &why) == TtlGcTopo::RefuseTooWide);
	CHECK(has(why, "4x2 WGP bitmap"));
	CHECK(has(why, "describe a different card"));
	CHECK(n48::ttl_gc_topology(wide_sa, &a, &why) == TtlGcTopo::RefuseTooWide);

	// Measured shape, but the harvest map was never read.
	TtlGcTopoFacts noharvest = measured(4, 2, 8);
	for (uint32_t i = 0; i < 4; i++) for (uint32_t j = 0; j < 2; j++) noharvest.cuBitmap[i][j] = 0;
	CHECK(n48::ttl_gc_topology(noharvest, &a, &why) == TtlGcTopo::RefuseNoHarvestMap);
	CHECK(has(why, "no CU harvest map was read"));
	CHECK(has(why, "reported as full"));

	// reasonOut may be null, and the accessor covers every enumerator.
	CHECK(n48::ttl_gc_topology(noharvest, &a, nullptr) == TtlGcTopo::RefuseNoHarvestMap);
	const TtlGcTopo allv[] = { TtlGcTopo::Answer, TtlGcTopo::RefuseNotInited,
	                           TtlGcTopo::RefuseNoShape, TtlGcTopo::RefuseTooWide,
	                           TtlGcTopo::RefuseNoHarvestMap };
	for (auto v : allv) CHECK(std::strlen(n48::ttlGcTopoReason(v)) > 12);
}

// ---------------------------------------------------------------- B: die shapes
void sectionB() {
	printf("== B: the numbers Apple is told, per die ==\n");
	TtlGcTopoAnswer a; const char *why = "";

	// amdgpu_gfx.h: gfx1201 Navi 48 = 4 SE x 2 SH x 4 WGP = 64 CUs.
	CHECK(n48::ttl_gc_topology(measured(4, 2, 8), &a, &why) == TtlGcTopo::Answer);
	CHECK(a.shaderEngines == 4);
	CHECK(a.shPerSe == 2);
	CHECK(a.wgpsPerSh == 4);
	CHECK(a.totalCus == 64);
	CHECK(a.rbBitmap == 0xF);
	CHECK(a.halfPopulatedWgps == 0);
	for (uint32_t i = 0; i < 4; i++)
		for (uint32_t j = 0; j < 2; j++)
			CHECK_MSG(a.wgpBitmap[i][j] == 0x0Fu, "full map is 4 WGP bits per array");

	// gfx1200 Navi 44 = 2 SE x 2 SH x 4 WGP = 32 CUs — half the card, and the row
	// count is what a 4-row struct would have padded with Navi 48's shape.
	CHECK(n48::ttl_gc_topology(measured(2, 2, 8), &a, &why) == TtlGcTopo::Answer);
	CHECK(a.shaderEngines == 2);
	CHECK(a.totalCus == 32);
	CHECK(a.wgpBitmap[0][0] == 0x0Fu);
	CHECK(a.wgpBitmap[1][1] == 0x0Fu);
	CHECK_MSG(a.wgpBitmap[2][0] == 0 && a.wgpBitmap[3][1] == 0,
	          "rows outside the die stay zero - a popcount must not see padding");

	// A harvested Navi 44: one whole array off, one CU pair off. Not the full mask.
	TtlGcTopoFacts h = measured(2, 2, 8);
	h.cuBitmap[1][0] = 0;              // an array the fuse blew
	h.cuBitmap[0][1] = 0x0F;           // 4 of 8 CUs -> 2 WGPs
	CHECK(n48::ttl_gc_topology(h, &a, &why) == TtlGcTopo::Answer);
	CHECK(a.wgpBitmap[1][0] == 0);
	CHECK(a.wgpBitmap[0][1] == 0x03u);
	CHECK(a.wgpBitmap[0][0] == 0x0Fu);
	CHECK(a.totalCus == 8 + 0 + 4 + 8);
	CHECK(a.halfPopulatedWgps == 0);
	CHECK_MSG(a.wgpBitmap[0][0] != 0x0Fu || a.totalCus != 32,
	          "the harvest map must change the answer, not just the log line");

	// The old fallback shape, reached honestly: an unmeasured 12.0.0 boot answers nothing.
	TtlGcTopoFacts blank; blank.inited = false;
	blank.maxShaderEngines = 4; blank.maxShPerSe = 2; blank.maxCuPerSh = 8;
	for (uint32_t i = 0; i < 4; i++) for (uint32_t j = 0; j < 2; j++) blank.cuBitmap[i][j] = 0xFF;
	CHECK(n48::ttl_gc_topology(blank, &a, &why) == TtlGcTopo::RefuseNotInited);
}

// ---------------------------------------------------------------- C: pairs
void sectionC() {
	printf("== C: WGP pairs and the width the map implies ==\n");
	TtlGcTopoAnswer a; const char *why = "";

	// One CU of a pair cannot be expressed; the suite pins which way it rounds.
	TtlGcTopoFacts lo = measured(1, 1, 8); lo.cuBitmap[0][0] = 0x01;   // CU0 alone
	CHECK(n48::ttl_gc_topology(lo, &a, &why) == TtlGcTopo::Answer);
	CHECK(a.wgpBitmap[0][0] == 0x01u);
	CHECK(a.totalCus == 1);
	CHECK(a.halfPopulatedWgps == 1);

	TtlGcTopoFacts hi = measured(1, 1, 8); hi.cuBitmap[0][0] = 0x02;   // CU1 alone
	CHECK(n48::ttl_gc_topology(hi, &a, &why) == TtlGcTopo::Answer);
	CHECK(a.wgpBitmap[0][0] == 0x01u);
	CHECK(a.halfPopulatedWgps == 1);
	CHECK(a.totalCus == 1);

	// Both halves of every pair set = no half-populated WGP at all.
	TtlGcTopoFacts both = measured(1, 1, 8); both.cuBitmap[0][0] = 0x03;
	CHECK(n48::ttl_gc_topology(both, &a, &why) == TtlGcTopo::Answer);
	CHECK(a.halfPopulatedWgps == 0);
	CHECK(a.wgpBitmap[0][0] == 0x01u);
	CHECK(a.totalCus == 2);

	// The table width is a maximum, not a claim: a die whose table says 2 CUs/SH
	// but whose map holds two WGPs gets the measured width.
	TtlGcTopoFacts raise = measured(1, 1, 2); raise.cuBitmap[0][0] = 0x0F;  // 4 CUs
	CHECK(n48::ttl_gc_topology(raise, &a, &why) == TtlGcTopo::Answer);
	CHECK(a.wgpsPerSh == 2);
	CHECK_MSG(a.wgpsPerSh == 2, "map narrower than max only ever lowers nothing");
	// And 32-bit maps: cu_per_sh == 32 is the clamp boundary.
	TtlGcTopoFacts wide = measured(1, 1, 32);
	CHECK(n48::ttl_gc_topology(wide, &a, &why) == TtlGcTopo::Answer);
	CHECK(a.wgpBitmap[0][0] == 0xFFFFu);
	CHECK(a.wgpsPerSh == 16);
	CHECK(a.totalCus == 32);
	// Odd bit count at the top of a word: bit 31 has no high half.
	TtlGcTopoFacts top = measured(1, 1, 32); top.cuBitmap[0][0] = 0x80000000u;
	CHECK(n48::ttl_gc_topology(top, &a, &why) == TtlGcTopo::Answer);
	CHECK(a.wgpBitmap[0][0] == 0x8000u);
	CHECK(a.halfPopulatedWgps == 1);
	CHECK(a.totalCus == 1);
}

// ---------------------------------------------------------------- D: the kext wiring
void sectionD(const std::string &root) {
	printf("== D: the TTL slot actually consults the gate ==\n");
	const std::string s = slurp(root + "/src/apple/Navi48Ttl.cpp");
	CHECK_MSG(!s.empty(), "read src/apple/Navi48Ttl.cpp");
	const size_t at = s.find("uint32_t Navi48Ttl::queryGcHardwareInfo(");
	CHECK_MSG(at != std::string::npos, "slot 14 is defined");
	const std::string body = s.substr(at, 3000);

	CHECK_MSG(has(s, "#include \"ttl_gc_topo.h\""), "the gate is included");
	CHECK(has(body, "n48::ttl_gc_topology(tf, &an, &twhy)"));
	CHECK(has(body, "tf.inited           = g.inited;"));
	CHECK(has(body, "tf.maxShaderEngines = g.max_shader_engines;"));
	CHECK(has(body, "tf.maxShPerSe       = g.max_sh_per_se;"));
	CHECK(has(body, "tf.maxCuPerSh       = g.max_cu_per_sh;"));
	CHECK(has(body, "tf.globalRbBitmap   = g.global_active_rb_bitmap;"));
	CHECK_MSG(has(body, "tf.cuBitmap[i][j] = g.active_cu_bitmap[i][j];"),
	          "the harvest map is read, not skipped");
	CHECK(has(body, "tv != n48::TtlGcTopo::Answer"));
	CHECK_MSG(has(body, "return kTtlUnsupported;"),
	          "a refusal declines the slot instead of answering kTtlOk");
	CHECK_MSG(count_of(body, "return kTtlOk;") == 1,
	          "exactly one success exit");

	// The published facts, and the guard that keeps an unbound TTL from writing.
	CHECK(has(body, "mOwner->setProperty(\"Navi48,TtlGcTopo\", twhy)"));
	CHECK(has(body, "if (mOwner) mOwner->setProperty(\"Navi48,TtlGcTopo\", twhy);"));
	CHECK(has(body, "\"Navi48,TtlGcSe\""));
	CHECK(has(body, "\"Navi48,TtlGcShPerSe\""));
	CHECK(has(body, "\"Navi48,TtlGcWgpPerSh\""));
	CHECK(has(body, "\"Navi48,TtlGcCus\""));

	// What must NOT come back.
	CHECK_MSG(!has(body, "g.max_shader_engines ? g.max_shader_engines : 4"),
	          "the Navi 48 SE fallback is gone");
	CHECK_MSG(!has(body, "g.max_sh_per_se      ? g.max_sh_per_se      : 2"),
	          "the Navi 48 SA fallback is gone");
	CHECK_MSG(!has(body, "g.max_cu_per_sh : 8) / 2"), "the Navi 48 CU fallback is gone");
	CHECK_MSG(!has(body, "if (se > 4) se = 4;"), "silent truncation is gone");
	CHECK_MSG(!has(body, "these are fallback constants"),
	          "warn-and-answer is gone");

	// The struct contract the gate encodes.
	CHECK(has(s, "static_assert(sizeof(GcHardwareInfoLayout) == 0x40"));
	CHECK(has(s, "uint32_t wgpBitmap[4][2];"));
	CHECK_MSG(has(body, "o->wgpBitmap[i][j] = an.wgpBitmap[i][j];"),
	          "the answer's rows are copied verbatim, padding and all");
	CHECK_MSG(count_of(s, "queryGcHardwareInfo") >= 2, "the slot is still named for the log");
}

// ---------------------------------------------------------------- E: header hygiene
void sectionE(const std::string &root) {
	printf("== E: the gate is a pure header ==\n");
	std::string h = slurp(root + "/src/apple/ttl_gc_topo.h");
	CHECK_MSG(!h.empty(), "read the header");
	std::string code;
	for (auto &line : [&]() {
		std::vector<std::string> out; std::istringstream in(h); std::string l;
		while (std::getline(in, l)) out.push_back(l);
		return out;
	}()) {
		const size_t c = line.find("//");
		if (c != std::string::npos) line = line.substr(0, c);
		code += line;
		code += "\n";
	}
	CHECK_MSG(!has(code, "#include <IOKit"), "no IOKit");
	CHECK(!has(code, "PE_parse_boot_argn"));
	CHECK(!has(code, "setProperty"));
	CHECK(!has(code, "amdgpu::"));
	CHECK_MSG(count_of(h, "\n#include <cstdint>") == 1, "one std include");
	CHECK_MSG(!has(code, ": 4;") && !has(code, ": 2;") && !has(code, ": 8) / 2"),
	          "no Navi 48 literals anywhere in the code");
	CHECK(has(h, "0018"));
	CHECK(has(h, "Navi 48's numbers"));
	CHECK(has(h, "12.0.0"));
}

}  // namespace

int main(int argc, char **argv) {
	const std::string root = (argc > 1) ? argv[1] : ".";
	sectionA();
	sectionB();
	sectionC();
	sectionD(root);
	sectionE(root);
	printf("\n%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
