//  survey_report_test.cpp — host suite for patches 0029-0030, the survey that writes itself to
//  the stick and now also reports how far start() got while doing it.
//
//  Build (any machine, no Mac, no GPU):
//    zig c++ -std=c++17 -Wall -Wextra -O1 -I src tests/survey_report_test.cpp -o /tmp/survey_report_test.exe
//    /tmp/survey_report_test.exe .
//
//  argv[1] is the navi48-bringup root, used by the source-pin sections.
//
//  Why a suite for a formatter: the survey's report is the only thing this machine can carry back
//  from a boot - the -v console is gone before the boot ends. If a line stops being emitted, or
//  the record grows past the 1536 bytes the raw writer accepts, the next patch is aimed at a
//  boot that never reported. The worst-case section is the guard against exactly that.

#include "survey_report.h"

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

// The full picture a working Navi 44 survey produces, with realistic segment counts.
n48::SurveyReportFacts full_facts() {
	n48::SurveyReportFacts f = {};
	f.vendorId = 0x1002;
	f.deviceId = 0x7590;
	f.navi44 = true;
	f.bar5Bytes = 16u * 1024u * 1024u;
	f.vramMB = 16368;
	f.discoveryValid = true;
	f.discoveryIps = 46;
	f.discoveryOffset = 0x34900;
	f.fwSet = "navi44";
	f.fwSource = "IP discovery";
	f.fwConfirmed = true;

	struct Row { const char *name; uint16_t id; uint8_t maj, min, rev, nb; uint32_t base; };
	const Row rows[] = {
		{ "GC",    11, 12, 0, 0,  2, 0x00001260 },
		{ "MMHUB", 34,  1, 0, 0,  4, 0x00000000 },
		{ "PSP",  255, 14, 0, 2, 15, 0x00000000 },
		{ "SMU",    1, 14, 0, 2,  1, 0x00016000 },
		{ "SDMA",  42,  7, 0, 0,  2, 0x0001a000 },
		{ "IH",    40,  7, 0, 0,  2, 0x0001c000 },
		{ "NBIF", 108,  6, 3, 1,  1, 0x00000000 },
		{ "DCN",  271,  4, 1, 0,  5, 0x00000012 },
		{ "HDP",   41,  7, 0, 0,  1, 0x0000d000 },
		{ "UMC",  150,  8, 0, 0,  2, 0x00014000 },
	};
	f.ipCount = sizeof(rows) / sizeof(rows[0]);
	for (uint32_t i = 0; i < f.ipCount; i++) {
		n48::SurveyReportIp &r = f.ip[i];
		r.name = rows[i].name;
		r.hwId = rows[i].id;
		r.major = rows[i].maj;
		r.minor = rows[i].min;
		r.rev = rows[i].rev;
		r.numBases = rows[i].nb;
		r.shownBases = r.numBases < n48::kSurveyReportMaxIpBases
		             ? r.numBases : (uint8_t)n48::kSurveyReportMaxIpBases;
		for (uint8_t s = 0; s < r.shownBases; s++) r.bases[s] = rows[i].base + s;
	}

	f.dmuPresent = true;
	f.dmuSegCount = 5;
	f.dmuSegs[0] = 0x00000012;
	f.dmuSegs[1] = 0x000000c0;
	f.dmuSegs[2] = 0x000034c0;
	f.dmuSegs[3] = 0x00009000;
	f.dmuSegs[4] = 0x02403c00;
	f.dcnAllowArms = true;
	f.dcnRebased = false;
	f.dcnReason = "ok";

	f.geometryValid = true;
	f.gbAddrConfig = 0x0000a008;
	f.numSE = 4;
	f.numRbPerSe = 2;
	f.expSE = 2;
	f.expRB = 2;

	f.fbLocationValid = true;
	f.fbLocationBase = 0x00000001;
	f.fbLocationTop = 0x00000111;
	f.mcBase = 0x0000000001000000ull;
	f.mcTop = 0x0000000111000000ull;

	f.pspValuesValid = true;
	f.pspBoot = 0x00000000;
	f.pspSos = 0x00000000;
	f.pspRing64 = 0x00000000;

	f.requestedStage = 0;
	f.interruptsRequested = 0;
	f.interruptsArmed = false;
	f.accelExperimentArmed = false;
	f.accelFireAtBoot = false;
	return f;
}

std::string format(const n48::SurveyReportFacts &f) {
	char buf[n48::kSurveyReportCap];
	const size_t n = n48::survey_report_text(buf, sizeof buf, f);
	return std::string(buf, n < sizeof buf ? n : sizeof buf - 1);
}

} // namespace

int main(int argc, char **argv) {
	const std::string root = argc > 1 ? argv[1] : ".";

	// ---------------------------------------------------------------- A: the whole picture
	const n48::SurveyReportFacts full = full_facts();
	const std::string t = format(full);
	CHECK(!t.empty());
	CHECK(has(t, "navi48 survey report - patch 0030"));
	CHECK(has(t, "card: vendor 0x1002 device 0x7590 navi44=1"));
	CHECK(has(t, "bar5: 16777216 bytes"));
	CHECK(has(t, "vram: 16368 MiB"));
	CHECK(has(t, "discovery: valid=1 ips=46 offset=0x00034900"));
	CHECK(has(t, "firmware: navi44 via IP discovery, confirmed=1"));
	CHECK(has(t, "ip: GC v12.0.0 bases=2: 00001260 00001261"));
	// A 15-segment block keeps its first three and says how many were left out.
	CHECK(has(t, "ip: PSP v14.0.2 bases=15: 00000000 00000001 00000002 +12"));
	CHECK(has(t, "ip: DCN v4.1.0 bases=5: 00000012 00000013 00000014 +2"));
	CHECK(has(t, "dcn: dmu segs=5: 00000012 000000c0 000034c0 00009000 02403c00 allowlist=ARM rebased=0 reason=ok"));
	CHECK(has(t, "geometry: GB_ADDR_CONFIG=0x0000a008 SE=4 RB/SE=2 (die caps 2/2)"));
	CHECK(has(t, "fb_location: base=0x00000001 top=0x00000111 mc=0x0000000001000000 top=0x0000000111000000"));
	CHECK(has(t, "psp: C2PMSG_35=0x00000000 C2PMSG_81=0x00000000 C2PMSG_64=0x00000000"));
	CHECK(has(t, "mode: survey only; interrupts=0 (armed=0); accel-experiment=0 fire-at-boot=0"));
	CHECK(has(t, "writes: none requested beyond the IP-discovery read"));
	CHECK(has(t, "end of survey report"));
	CHECK(t.back() == '\n');
	CHECK(t.size() < n48::kSurveyReportCap);

	// ---------------------------------------------------------------- B: a denied allowlist
	n48::SurveyReportFacts denied = full_facts();
	denied.dcnAllowArms = false;
	denied.dcnReason = "outside every display range - the DEFAULT";
	const std::string td = format(denied);
	CHECK(has(td, "allowlist=REFUSE"));
	CHECK(has(td, "reason=outside every display range - the DEFAULT"));
	CHECK(!has(td, "allowlist=ARM"));

	// ---------------------------------------------------------------- C: a partial / empty survey
	n48::SurveyReportFacts empty = {};
	const std::string te = format(empty);
	CHECK(has(te, "card: vendor 0x0000 device 0x0000 navi44=0"));
	CHECK(has(te, "bar5: 0 bytes (not mapped)"));
	CHECK(has(te, "vram: 0 MiB (not read)"));
	CHECK(has(te, "discovery: valid=0 ips=0 offset=0x00000000"));
	CHECK(has(te, "firmware: (not selected)"));
	CHECK(!has(te, "ip: "));                 // no rows invented
	CHECK(has(te, "dcn: no DMU entry in discovery"));
	CHECK(has(te, "geometry: GB_ADDR_CONFIG not decoded"));
	CHECK(has(te, "fb_location: unreadable"));
	CHECK(has(te, "psp: not read"));
	CHECK(has(te, "mode: survey only"));
	CHECK(has(te, "writes: none requested"));
	CHECK(te.size() < 768);

	// A boot that asked for a writing stage must not claim it wrote nothing.
	n48::SurveyReportFacts staged = full_facts();
	staged.requestedStage = 17;
	staged.accelExperimentArmed = true;
	staged.accelFireAtBoot = true;
	staged.interruptsRequested = 1;
	staged.interruptsArmed = true;
	const std::string ts = format(staged);
	CHECK(has(ts, "mode: stage=17 requested; interrupts=1 (armed=1); accel-experiment=1 fire-at-boot=1"));
	CHECK(has(ts, "writes: a writing stage or the accel experiment was requested"));
	CHECK(!has(ts, "writes: none requested"));

	// ---------------------------------------------------------------- D: the cap contract
	{
		char canary[64];
		std::memset(canary, 0xAA, sizeof canary);
		const size_t need = n48::survey_report_text(canary, 10, full);
		CHECK(need > 10);
		CHECK(canary[9] == 0);                       // NUL inside the cap
		for (size_t i = 10; i < sizeof canary; i++) CHECK((unsigned char)canary[i] == 0xAA);

		std::memset(canary, 0xAA, sizeof canary);
		const size_t one = n48::survey_report_text(canary, 1, full);
		CHECK(one == need);
		CHECK(canary[0] == 0);
		for (size_t i = 1; i < sizeof canary; i++) CHECK((unsigned char)canary[i] == 0xAA);

		std::memset(canary, 0xAA, sizeof canary);
		const size_t zero = n48::survey_report_text(canary, 0, full);
		CHECK(zero == need);
		for (size_t i = 0; i < sizeof canary; i++) CHECK((unsigned char)canary[i] == 0xAA);

		// Counting without writing must agree with the real run.
		CHECK(n48::survey_report_text(nullptr, 0, full) == need);
		char whole[n48::kSurveyReportCap];
		const size_t n = n48::survey_report_text(whole, sizeof whole, full);
		CHECK(n == need);
		CHECK(n < sizeof whole);
		CHECK(whole[n] == 0);
	}

	// ---------------------------------------------------------------- E: the worst case
	// Ten rows that each report fifteen segments: the largest record the survey can hand over.
	// If this ever outgrows the raw writer's 1536-byte record, the tail (psp, mode, writes,
	// footer) would be silently cut and the boot would look complete and be wrong.
	n48::SurveyReportFacts worst = full_facts();
	for (uint32_t i = 0; i < n48::kSurveyReportMaxIps; i++) {
		n48::SurveyReportIp &r = worst.ip[i];
		r.name = "MMHUB";
		r.numBases = 15;
		r.shownBases = (uint8_t)n48::kSurveyReportMaxIpBases;
	}
	const std::string tw = format(worst);
	CHECK(tw.size() < n48::kSurveyReportCap);
	CHECK(has(tw, "psp: C2PMSG_35="));
	CHECK(has(tw, "mode: survey only"));
	CHECK(has(tw, "writes: none requested"));
	CHECK(has(tw, "end of survey report"));

	// ---------------------------------------------------------------- E2: the 0030 progress record
	{
		// The record is 135 bytes of text, so the buffer that carries it must be bigger than
		// that; a short cap keeps the NUL inside the cap, exactly as survey_report_text does (D).
		const size_t need0 = n48::survey_stage_text(nullptr, 0, 15u);
		CHECK(need0 > 0);
		// The raw writer accepts exactly kSurveyReportCap bytes; a progress line that filled the
		// record would be cut on the stick and read back as a complete line. Same guard as E,
		// for the line written when the full record does not exist yet.
		CHECK(need0 < n48::kSurveyReportCap);
		char rec[192];
		for (uint32_t mask : {1u, 3u, 7u, 15u}) {
			std::memset(rec, 0xAA, sizeof rec);
			const size_t need = n48::survey_stage_text(rec, sizeof rec, mask);
			CHECK(need == need0);                       // one length, whatever bits are set
			CHECK(rec[need] == 0);
			CHECK(std::string(rec, 4) == "navi");        // the raw writer's magic survives each stage
			CHECK((unsigned char)rec[need + 1] == 0xAA);  // nothing written past the record
		}
		char tight[16];
		std::memset(tight, 0xAA, sizeof tight);
		CHECK(n48::survey_stage_text(tight, 10, 15u) == need0);
		CHECK(tight[9] == 0);
		for (size_t i = 10; i < sizeof tight; i++) CHECK((unsigned char)tight[i] == 0xAA);
		// Counting without writing agrees with the real run, as in section D.
		std::memset(tight, 0xAA, sizeof tight);
		CHECK(n48::survey_stage_text(nullptr, 0, 15u) == need0);
		for (size_t i = 0; i < sizeof tight; i++) CHECK((unsigned char)tight[i] == 0xAA);

		char full[n48::kSurveyReportCap];
		n48::survey_stage_text(full, sizeof full, n48::kSurveyStageStart);
		CHECK(has(std::string(full), "start=1 mmio=0 discovery=0 report=0"));
		n48::survey_stage_text(full, sizeof full,
		                       n48::kSurveyStageStart | n48::kSurveyStageMmio |
		                       n48::kSurveyStageDiscovery | n48::kSurveyStageReport);
		CHECK(has(std::string(full), "start=1 mmio=1 discovery=1 report=1"));
	}

	// ---------------------------------------------------------------- F: the wiring
	const std::string src = slurp(root + "/src/Navi48Bringup.cpp");
	const std::string hpp = slurp(root + "/src/Navi48Bringup.hpp");
	const std::string take = slurp(root + "/src/accel_takeover.cpp");
	const std::string hdr = slurp(root + "/src/survey_report.h");
	CHECK(!src.empty() && !hpp.empty() && !take.empty() && !hdr.empty());

	CHECK(has(src, "PE_parse_boot_argn(\"navi48-survey-report\", &on, sizeof(on))"));
	CHECK(has(src, "n48::survey_report_text(text, sizeof(text), surveyFacts)"));
	// The writer is handed the length the formatter used, not the buffer size, and the PCI provider
	// rather than the service that is about to be aborted. A pin on the old call is a pin on a bug.
	CHECK(has(src, "n48::survey_report_start_writer(pciDevice, text, n);"));
	CHECK(!has(src, "survey_report_start_writer(this, text, sizeof(text))"));
	CHECK(has(src, "setProperty(\"Navi48,SurveyReport\""));
	// The report is started after the survey (and any stage) and before the display attaches.
	CHECK(src.find("surveyPSP();") < src.find("startSurveyReportIfRequested();"));
	CHECK(src.find("startSurveyReportIfRequested();") < src.find("n48dcn::attach(this);"));
	// A report must never pretend a writing stage ran when none was requested.
	CHECK(has(src, "f.requestedStage == 0 && !f.accelExperimentArmed && !f.accelFireAtBoot") ||
	      has(hdr, "f.requestedStage == 0 && !f.accelExperimentArmed && !f.accelFireAtBoot"));

	CHECK(has(hpp, "#include \"survey_report.h\""));
	CHECK(has(hpp, "void startSurveyReportIfRequested();"));
	CHECK(has(hpp, "n48::SurveyReportFacts surveyFacts"));
	CHECK(has(hpp, "bool               surveyReportStarted"));

	CHECK(has(take, "void survey_report_start_writer(::IOService *client, const char *text, size_t len)"));
	CHECK(has(take, "write_report_file(text, len)"));
	CHECK(has(take, "n48::report_raw_write(client, text, len)"));
	CHECK(has(take, "kernel_thread_start(&survey_report_thread"));
	CHECK(has(take, "if (__atomic_load_n(&gWriterStarted, __ATOMIC_ACQUIRE))"));
	// 0030: progress marks, the one-second grace, and the thread that carries both.
	CHECK(has(take, "kSurveyVfsGraceSeconds = 1;"));
	CHECK(has(take, "void survey_stage_mark(uint32_t stage, ::IOService *client)"));
	CHECK(has(take, "n48::survey_stage_text(stage, sizeof stage, mask)"));
	CHECK(has(take, "__atomic_fetch_or(&gSurveyMask, stage, __ATOMIC_ACQ_REL)"));
	CHECK(has(src, "n48::survey_stage_mark(n48::kSurveyStageStart, provider);"));
	CHECK(has(src, "n48::survey_stage_mark(n48::kSurveyStageMmio, pciDevice);"));
	CHECK(has(src, "n48::survey_stage_mark(n48::kSurveyStageDiscovery, pciDevice);"));
	// Every raw client is a service that outlives start(): the marks exist to describe the boots
	// where start() returns false, and a writer holding `this` through that abort is the bug.
	CHECK(!has(src, "survey_stage_mark(n48::kSurveyStageStart, this)"));
	CHECK(!has(src, "survey_stage_mark(n48::kSurveyStageMmio, this)"));
	CHECK(!has(src, "survey_stage_mark(n48::kSurveyStageDiscovery, this)"));
	// Marks must precede the work they vouch for, or a boot that dies there reports the wrong stage.
	// Anchored inside start(): the definitions of mapRegisters()/loadOnDieDiscovery()/surveyIPs()
	// sit earlier in the file, so an unanchored find() would match those and not the calls.
	const size_t startAt = src.find("bool Navi48Bringup::start(IOService *provider) {");
	CHECK(startAt != std::string::npos);
	auto after = [&](const char *needle) { return src.find(needle, startAt); };
	CHECK(after("kSurveyStageStart, provider);") < after("if (!mapRegisters())"));
	CHECK(after("kSurveyStageMmio, pciDevice);") < after("if (!loadOnDieDiscovery())"));
	CHECK(after("kSurveyStageDiscovery, pciDevice);") < after("surveyIPs();"));
	// provider is only safe after super::start() retained it; pciDevice only after it was assigned.
	CHECK(after("kSurveyStageStart, provider);") > after("if (!super::start(provider)) return false;"));
	CHECK(after("kSurveyStageMmio, pciDevice);") > after("pciDevice = OSDynamicCast(IOPCIDevice, provider);"));

	// The raw channel is opt-in. One second of grace means the block-device write would otherwise
	// run on every survey boot, before BAR5 is even mapped, and a boot that resets in it leaves no
	// report and no clue. navi48-raw-report=1 is the only thing that turns it on.
	CHECK(has(take, "PE_parse_boot_argn(\"navi48-raw-report\", &on, sizeof(on)) && on != 0"));
	CHECK(has(take, "if (err != 0 && rawOk && t >= kSurveyVfsGraceSeconds)"));
	CHECK(has(take, "if (rawOk && t == kSurveyVfsGraceSeconds && !saidGrace)"));
	const size_t rawAt = take.find("bool raw_report_wanted() {");
	const size_t wantedBefore = take.find("bool survey_wanted() {", rawAt);
	CHECK(rawAt != std::string::npos && wantedBefore != std::string::npos && rawAt < wantedBefore);
	const std::string rawBody = take.substr(rawAt, wantedBefore - rawAt);
	CHECK(count_of(rawBody, "PE_parse_boot_argn(") == 1);   // asks one argument, nothing else
	CHECK(has(rawBody, "navi48-raw-report"));
	CHECK(!has(rawBody, "navi48-survey-report"));
	// Reading the flag once, at thread entry, is the point: a boot-arg cannot change mid-boot, so
	// the survey either may or may not touch the media - never "depends on which loop turned up".
	const size_t threadAt2 = take.find("void survey_report_thread(", wantedBefore);
	CHECK(threadAt2 != std::string::npos && wantedBefore < threadAt2);
	const std::string surveyBody =
	    take.substr(threadAt2, take.find("void print_block(", threadAt2) - threadAt2);
	CHECK(count_of(surveyBody, "raw_report_wanted()") == 1);
	CHECK(count_of(surveyBody, "rawOk") >= 4);
	// A survey record must not overwrite a verdict record: the verdict is asked again immediately
	// before the complete record goes out, not only when the survey finished formatting it.
	CHECK(has(surveyBody, "__atomic_load_n(&gWriterStarted, __ATOMIC_ACQUIRE)"));
	CHECK(has(surveyBody, "survey report dropped: the verdict reporter took"));
	// A failed thread start gives back both the retain and the claim, or the pending bits would
	// wait forever for a writer that never ran.
	const size_t markAt2 = take.find("void survey_stage_mark(uint32_t stage, ::IOService *client)");
	// The slice ends at the first line that is only a closing brace; "\n}" works whatever the
	// checkout's line endings are, which the CRLF form of this needle would not.
	const std::string markBody2 = take.substr(markAt2, take.find("\n}", markAt2) - markAt2);
	CHECK(has(markBody2, "client->release();"));
	CHECK(has(markBody2, "__atomic_store_n(&gSurveyStarted, 0u, __ATOMIC_RELEASE);"));
	CHECK(has(markBody2, "__atomic_store_n(&gSurveyClient, 0, __ATOMIC_RELEASE);"));
	CHECK(markBody2.find("__atomic_fetch_or(&gSurveyMask") < markBody2.find("kernel_thread_start("));
	CHECK(count_of(markBody2, "client->retain();") == 1);
	CHECK(count_of(markBody2, "client->release();") == 1);

	// The boot-arg gate: like the survey report itself, the progress line exists only on a boot
	// that asked for it. survey_stage_mark() must ask survey_wanted() before it retains a client,
	// starts a thread, or sets a bit, so a boot with no argument leaves NAVI48.TXT untouched and
	// no survey thread runs at all.
	const size_t markAt = take.find("void survey_stage_mark(uint32_t stage, ::IOService *client)");
	const size_t markEnd = take.find("void survey_report_start_writer(", markAt);
	CHECK(markAt != std::string::npos && markEnd != std::string::npos && markAt < markEnd);
	const std::string markBody = take.substr(markAt, markEnd - markAt);
	const size_t gate = markBody.find("!survey_wanted()");
	const size_t retainAt = markBody.find("client->retain();");
	const size_t threadAt = markBody.find("kernel_thread_start(&survey_report_thread");
	const size_t bitAt = markBody.find("__atomic_fetch_or(&gSurveyMask");
	CHECK(gate != std::string::npos);                        // the refusal is there at all
	CHECK(retainAt != std::string::npos && threadAt != std::string::npos && bitAt != std::string::npos);
	CHECK(gate < retainAt && gate < threadAt && gate < bitAt);   // ...and it comes first
	CHECK(count_of(markBody, "PE_parse_boot_argn(") == 0);   // it asks survey_wanted(), not the args

	// The only boot argument consulted is navi48-survey-report. The arming arguments belong to
	// start(); reading one of them here would let an unrelated boot-arg turn this writer on.
	const size_t wantedAt = take.find("bool survey_wanted() {");
	const size_t wantedEnd = take.find("void survey_report_thread(", wantedAt);
	CHECK(wantedAt != std::string::npos && wantedEnd != std::string::npos && wantedAt < wantedEnd);
	const std::string wantedBody = take.substr(wantedAt, wantedEnd - wantedAt);
	CHECK(has(wantedBody, "PE_parse_boot_argn(\"navi48-survey-report\", &on, sizeof(on)) && on != 0"));
	CHECK(count_of(wantedBody, "PE_parse_boot_argn(") == 1);
	CHECK(!has(wantedBody, "navi48-accel-experiment"));
	CHECK(!has(wantedBody, "navi48-accel-fire-at-boot"));
	CHECK(!has(wantedBody, "navi48-report-hold"));
	CHECK(!has(wantedBody, "navi48-interrupts"));

	CHECK(has(hdr, "kSurveyReportCap = 1536"));
	CHECK(has(hdr, "survey_report_text"));
	CHECK(!has(hdr, "#include <IOKit"));
	CHECK(!has(hdr, "setProperty"));
	CHECK(!has(hdr, "IOLog("));
	CHECK(!has(hdr, "pe_parse_boot_argn"));
	// The runtime declaration is here; the implementation belongs to the writer, not the formatter.
	CHECK(has(hdr, "void survey_report_start_writer(::IOService *client, const char *text, size_t len);"));

	if (gFailures == 0) printf("OK: %d checks, 0 failures\n", gChecks);
	else                printf("%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
