// sdma_census_test.cpp — 0011: how many SDMA engines does this die have, and
// who decided?
//
// Build and run on the host (from src/navi48-bringup; no Mac, no GPU):
//     zig c++ -std=c++17 -Wall -Wextra -O1 \
//         -include tests/sdma_census_hostshim.h \
//         -I tests/dalshim -I src -I src/amd -I src/fw \
//         tests/sdma_census_test.cpp src/amd/amdgpu_discovery.cpp \
//         src/amd/sdma_v7_0.cpp src/ipdiscovery.cpp \
//         -o /tmp/sdma_census_test && /tmp/sdma_census_test
//     tests/sdma_census_plant.sh          # proves the suite can fail
//
// What is real here and what is pinned, and why both:
//   A-B  sdma_census_add / sdma_census_from_entries — the product's own
//        arithmetic, called directly, over synthetic {hw_id, instance} pairs.
//   C    sdma_census(IpDiscovery &) against forged IPDS ROM images parsed by
//        the product's own ipdiscovery.cpp. This is the section that proves the
//        headline: a die whose engines appear only under SDMA1/2/3_HWID is
//        invisible to an hwid-42 lookup and obvious to the census.
//   D    sdma_apply_census + sdma_addressable_instances — the state the ladder
//        sees, and the log lines the next die's report is read from.
//   E    source pins. Sections A-D cannot see sdma_init_full's loop bounds: the
//        clamp makes 2 the answer either way, so a reverted `kSDMAInstanceCount`
//        bound is behaviourally identical on every die this suite can forge.
//        Only a text pin catches it — and the plant script depends on that,
//        because a suite that cannot fail is what 0011 replaced.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include "amdgpu_sdma.h"
#include "amdgpu_discovery.h"
#include "ipdiscovery.hpp"
#include "amdgpu_sysmem.h"
#include "amdgpu_gmc.h"
#include "amdgpu_psp.h"
#include "amdgpu_ucode_extract.h"

// sdma_v7_0.cpp links against eight symbols that belong to the kernel side (the
// PSP ring, GMC binding, sysmem allocator, the log sink). The census reaches
// none of them, so they are defined here to count their own callers: any stub
// that fires is a test failure, which is the point — it proves the path under
// test does not need a Mac.
namespace amdgpu {
// The log sink, captured. Half of this patch's value is what the next Navi 44
// boot says out loud, so the lines are assertions, not decoration.
static char   gLog[16384];
static size_t gLogLen;
static int    gStubsCalled;

void n48_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(gLog + gLogLen, sizeof(gLog) - gLogLen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t room = sizeof(gLog) - gLogLen - 1;
        gLogLen += (size_t)n < room ? (size_t)n : room;
    }
}

kern_return_t sysmem_alloc(SysMem &m, uint64_t size, uint64_t align)
{
    (void)m; (void)size; (void)align; gStubsCalled++; return 0xE00002D1;
}
void sysmem_free(SysMem &m) { (void)m; gStubsCalled++; }
kern_return_t gmc_bind_existing(DeviceContext &dev, GMCContext &gmc, uint64_t busAddr,
                               uint64_t sizeBytes, uint64_t *outMcAddr)
{
    (void)dev; (void)gmc; (void)busAddr; (void)sizeBytes; (void)outMcAddr;
    gStubsCalled++; return 0xE00002D1;
}
kern_return_t psp_load_ip_fw(DeviceContext &dev, PSPContext &psp, uint64_t fwBusAddr,
                            uint32_t fwSize, uint32_t fwType)
{
    (void)dev; (void)psp; (void)fwBusAddr; (void)fwSize; (void)fwType;
    gStubsCalled++; return 0xE00002D1;
}
kern_return_t psp_fw_buf_stage(DeviceContext &dev, PSPContext &psp, const void *bytes,
                              uint32_t size, uint64_t *outBusAddr)
{
    (void)dev; (void)psp; (void)bytes; (void)size; (void)outBusAddr;
    gStubsCalled++; return 0xE00002D1;
}
uint32_t amdgpu_ucode_extract(uint64_t hostFwType, const uint8_t *bin, uint64_t size_bytes,
                             UcodePayload out[kMaxUcodePayloadsPerFile])
{
    (void)hostFwType; (void)bin; (void)size_bytes; (void)out;
    gStubsCalled++; return 0;
}
} // namespace amdgpu

extern "C" void dalshim_sleep(unsigned ms) { (void)ms; amdgpu::gStubsCalled++; }

// ---------------------------------------------------------------- assertions
static int gRun, gFail;
static void ok(bool cond, const char *what)
{
    gRun++;
    if (!cond) { gFail++; std::printf("FAIL  %s\n", what); }
}
static void eq_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-58s got %#llx want %#llx\n", what,
                                            (unsigned long long)got, (unsigned long long)want); }
}
static bool log_has(const char *needle)
{
    return std::strstr(amdgpu::gLog, needle) != nullptr;
}
static void log_reset() { amdgpu::gLogLen = 0; amdgpu::gLog[0] = '\0'; }

// The upstream hw_ids, spelled out here rather than trusted from the driver's
// own constant names: soc15_hw_ip.h:59-60 and :85-86.
static const uint16_t kSdma0Hwid = 42, kSdma1Hwid = 43, kSdma2Hwid = 68, kSdma3Hwid = 69;
static const uint16_t kGcHwid = 11, kMp0Hwid = 255, kMmhubHwid = 34, kDmuHwid = 271;

// ---------------------------------------------------------------- A: add()
static void section_A()
{
    using namespace amdgpu;
    SdmaCensus c = {};
    // A1: the four SDMA hw_ids count; nothing else does.
    sdma_census_add(c, kSdma0Hwid, 0);
    eq_u("A1 after SDMA0/0: count", c.count, 1);
    eq_u("A1 after SDMA0/0: mask", c.mask, 0x1);
    sdma_census_add(c, kSdma1Hwid, 1);
    eq_u("A1 after SDMA1/1: count", c.count, 2);
    eq_u("A1 after SDMA1/1: mask", c.mask, 0x3);
    sdma_census_add(c, kSdma2Hwid, 2);
    sdma_census_add(c, kSdma3Hwid, 3);
    eq_u("A1 all four hw_ids: count", c.count, 4);
    eq_u("A1 all four hw_ids: mask", c.mask, 0xF);
    eq_u("A1 no overflow yet", c.overflow, 0);

    SdmaCensus n = {};
    sdma_census_add(n, kGcHwid, 0);
    sdma_census_add(n, kMp0Hwid, 0);
    sdma_census_add(n, kMmhubHwid, 0);
    sdma_census_add(n, kDmuHwid, 0);
    sdma_census_add(n, 44, 0);       // one past SDMA1_HWID — not an engine
    sdma_census_add(n, 67, 0);       // one before SDMA2_HWID
    sdma_census_add(n, 70, 0);
    eq_u("A2 non-SDMA hw_ids leave count at 0", n.count, 0);
    eq_u("A2 non-SDMA hw_ids leave mask at 0", n.mask, 0);

    // A3: the same hw_id twice with two instance numbers is two engines — the
    // census keys on the entry, exactly like the upstream walk.
    SdmaCensus d = {};
    sdma_census_add(d, kSdma0Hwid, 0);
    sdma_census_add(d, kSdma0Hwid, 1);
    eq_u("A3 SDMA0 twice: count", d.count, 2);
    eq_u("A3 SDMA0 twice: mask", d.mask, 0x3);

    // A4: the cap. AMDGPU_MAX_SDMA_INSTANCES is 16 (amdgpu_sdma_types.h:29) and
    // upstream counts the first 16 and prints "Too many SDMA instances" for the
    // rest; here the rest lands in overflow so the caller can refuse.
    SdmaCensus over = {};
    for (uint32_t i = 0; i < kSDMAMaxInstances; i++) sdma_census_add(over, kSdma0Hwid, (uint8_t)i);
    eq_u("A4 exactly at the cap: count", over.count, kSDMAMaxInstances);
    eq_u("A4 exactly at the cap: overflow", over.overflow, 0);
    sdma_census_add(over, kSdma1Hwid, 16);
    sdma_census_add(over, kSdma1Hwid, 17);
    eq_u("A4 past the cap: count stays", over.count, kSDMAMaxInstances);
    eq_u("A4 past the cap: overflow", over.overflow, 2);
    eq_u("A4 past the cap: mask gains nothing", over.mask, 0xFFFF);

    // A5: the shift guard. Upstream writes `1U << ip->instance_number` with an
    // unchecked u8 from the card's ROM — undefined for >= 32. The engine still
    // counts; only the bit is dropped.
    SdmaCensus big = {};
    sdma_census_add(big, kSdma0Hwid, 31);
    eq_u("A5 instance 31: count", big.count, 1);
    eq_u("A5 instance 31: mask keeps the bit", big.mask, 0x80000000u);
    sdma_census_add(big, kSdma0Hwid, 32);
    eq_u("A5 instance 32: still counted", big.count, 2);
    eq_u("A5 instance 32: no shift, mask unchanged", big.mask, 0x80000000u);
    sdma_census_add(big, kSdma0Hwid, 255);
    eq_u("A5 instance 255: counted", big.count, 3);
    eq_u("A5 instance 255: mask unchanged", big.mask, 0x80000000u);
    ok(kSDMAMaxInstances == 16, "A6 the array ceiling is upstream's cap, not our own number");
    ok(kSDMAInstanceCount == 2, "A7 the addressable window stays 2 (sdma_v7_0.c:125-137)");
    ok(kSDMAInstanceCount < kSDMAMaxInstances, "A7b window < ceiling: the two constants differ on purpose");
}

// ------------------------------------------------------- B: from_entries()
static void section_B()
{
    using namespace amdgpu;
    SdmaCensus c = sdma_census_from_entries(nullptr, 0);
    eq_u("B1 empty table: count", c.count, 0);
    eq_u("B1 empty table: mask", c.mask, 0);
    eq_u("B1 empty table: overflow", c.overflow, 0);

    const SdmaInstanceRef navi44ish[] = {
        { kGcHwid, 0 }, { kSdma0Hwid, 0 }, { kMp0Hwid, 0 }, { kSdma1Hwid, 1 }, { kMmhubHwid, 0 } };
    c = sdma_census_from_entries(navi44ish, 5);
    eq_u("B2 engines interleaved with other IPs: count", c.count, 2);
    eq_u("B2 engines interleaved with other IPs: mask", c.mask, 0x3);

    const SdmaInstanceRef numberedFromOne[] = { { kSdma0Hwid, 1 }, { kSdma0Hwid, 2 } };
    c = sdma_census_from_entries(numberedFromOne, 2);
    eq_u("B3 die numbering 1 and 2: count", c.count, 2);
    eq_u("B3 die numbering 1 and 2: mask", c.mask, 0x6);
    ok((c.mask & 1u) == 0, "B3 mask bit 0 clear — the case count alone cannot express");

    const SdmaInstanceRef one[] = { { kGcHwid, 0 }, { kSdma0Hwid, 0 } };
    c = sdma_census_from_entries(one, 2);
    eq_u("B4 harvested to one engine: count", c.count, 1);
    eq_u("B4 harvested to one engine: mask", c.mask, 0x1);

    std::vector<SdmaInstanceRef> many(40, SdmaInstanceRef{kSdma0Hwid, 0});
    for (size_t i = 0; i < many.size(); i++) many[i].instance = (uint8_t)i;
    c = sdma_census_from_entries(many.data(), (uint32_t)many.size());
    eq_u("B5 40 entries: count clamps", c.count, kSDMAMaxInstances);
    eq_u("B5 40 entries: overflow", c.overflow, 40 - kSDMAMaxInstances);

    // B6: the census is a pure function of the pairs — calling it twice on the
    // same table gives the same answer (it is re-run per ladder pass).
    SdmaCensus a = sdma_census_from_entries(navi44ish, 5), b = sdma_census_from_entries(navi44ish, 5);
    ok(a.count == b.count && a.mask == b.mask && a.overflow == b.overflow,
       "B6 idempotent over the same table");
}

// ------------------------------------------------- C: forged IPDS ROM images
// binary_header (v2): u32 signature, u16 vmajor, u16 vmin, u16 checksum,
// u16 bin_size, u16 num_tables, u16 pad, then table_info{off,csum,size,pad}[].
// ip_discovery_header: u32 "IPDS", u16 ver, u16 size, u32 id, u16 num_dies,
// die_info[16]{u16 die_id, u16 die_off}. die_header: pad[2], u16 num_ips, then
// ip_v3 records {u16 hw_id, u8 inst, u8 num_base, u8 major, u8 minor, u8 rev,
// u8 sub, u32 bases[]}. All of it read out of src/ipdiscovery.cpp.
static constexpr size_t kRomSize = 512;   // IpDiscovery::init refuses < 512

struct RomBuilder {
    std::vector<uint8_t> buf { std::vector<uint8_t>(kRomSize, 0) };
    // die_header sits at 118 (see finish()); its 2 pad bytes + u16 num_ips come
    // first, so the ip records start at 122.
    size_t dieRec { 122 };
    size_t cursor { 122 };
    size_t nrec   { 0 };      // every record actually written, fillers included

    void put16(size_t off, uint16_t v) { buf[off] = (uint8_t)v; buf[off + 1] = (uint8_t)(v >> 8); }
    void put32(size_t off, uint32_t v) { buf[off] = (uint8_t)v; buf[off + 1] = (uint8_t)(v >> 8);
        buf[off + 2] = (uint8_t)(v >> 16); buf[off + 3] = (uint8_t)(v >> 24); }

    // One ip_v3 record. Bases past the end of the image are simply not written —
    // that is what makes a record the parser can start but not finish.
    void record(uint16_t hw, uint8_t inst, uint8_t nb)
    {
        put16(cursor, hw); buf[cursor + 2] = inst; buf[cursor + 3] = nb;
        buf[cursor + 4] = 12; buf[cursor + 5] = 0; buf[cursor + 6] = 0; buf[cursor + 7] = 0;
        for (uint8_t s = 0; s < nb && cursor + 8 + 4u * s + 4 <= buf.size(); s++)
            put32(cursor + 8 + 4u * s, 0x1000u + 0x40u * s);
        cursor += 8 + 4u * nb;
        nrec++;
    }

    // num_ips counts EVERY record — the fillers included, because ipAt() walks
    // records by size rather than by index, so a lie here mis-lands every read
    // after the first.
    RomBuilder &ips(const std::vector<amdgpu::SdmaInstanceRef> &list, uint8_t basesPerRec,
                    size_t padTo = 0, uint8_t overrunLast = 0)
    {
        record(kGcHwid, 0, basesPerRec);
        while (padTo && cursor < padTo) record(44, 0, 1);      // 44 is not an SDMA hw_id
        if (padTo && cursor != padTo) { std::printf("BAD   forge: padTo %zu missed (cursor %zu)\n",
                                                    padTo, cursor); gFail++; }
        for (size_t i = 0; i < list.size(); i++) {
            const bool last = (i + 1 == list.size());
            record(list[i].hwId, list[i].instance,
                   last && overrunLast ? overrunLast : basesPerRec);
        }
        put16(dieRec - 2, (uint16_t)nrec);
        return *this;
    }

    // Seal: write the header fields and the 16-bit byte sum the parser wants.
    std::vector<uint8_t> &finish(size_t binSize = kRomSize)
    {
        put32(0, 0x28211407);                       // binary_header signature
        put16(4, 2);                                // version_major -> table list at +16
        put16(6, 1);
        put16(8, 0);                                // checksum, filled below
        put16(10, (uint16_t)binSize);
        put16(12, 1);                               // num_tables
        put16(16, 40);                              // table_info[0].offset -> IPDS
        put16(18, 0);                               // table_info[0].checksum
        put16(20, 144);                             // table_info[0].size (>= 80)
        put32(40, 0x53445049);                      // "IPDS"
        put16(44, 4);                               // ip_discovery_header.version
        put16(46, 78);                              // .size
        put32(48, 0);                               // .ip_discovery_id
        put16(52, 1);                               // .num_dies
        put16(54, 0);                               // die_info[0].die_id
        put16(56, 118);                             // die_info[0].die_offset (binary-relative)
        uint16_t sum = 0;
        for (size_t i = 10; i < binSize; i++) sum = (uint16_t)(sum + buf[i]);
        put16(8, sum);
        return buf;
    }
};

static amdgpu::SdmaInstanceRef ref(uint16_t hw, uint8_t inst)
{
    amdgpu::SdmaInstanceRef r; r.hwId = hw; r.instance = inst; return r;
}

static void section_C()
{
    using namespace amdgpu;
    const std::vector<SdmaInstanceRef> none;
    IpDiscovery disc;

    // C1: the Navi 44 / Navi 48 shape — GC plus SDMA0/0 and SDMA0/1.
    auto b1 = RomBuilder().ips({ ref(kSdma0Hwid, 0), ref(kSdma0Hwid, 1) }, 1).finish();
    ok(disc.init(b1.data(), b1.size()), "C1 forged ROM parses");
    SdmaCensus c = {};
    ok(sdma_census(disc, c), "C1 census runs on a valid table");
    eq_u("C1 count", c.count, 2);
    eq_u("C1 mask", c.mask, 0x3);
    eq_u("C1 overflow", c.overflow, 0);
    IpDiscovery::IpEntry e;
    ok(disc.findIp(kSdma0Hwid, 0, e), "C1 the hwid-42 lookup finds instance 0");

    // C2: THE POINT OF THE PATCH. Engines under SDMA1/2/3_HWID only. The
    // survey's `IP,SDMA` key comes from findIp(42, 0) and reports nothing; the
    // census reports two engines. If this ever stops being true, 0011 is a
    // no-op dressed up as a fix.
    auto b2 = RomBuilder().ips({ ref(kSdma1Hwid, 0), ref(kSdma2Hwid, 0), ref(kSdma3Hwid, 0) }, 1).finish();
    IpDiscovery d2;
    ok(d2.init(b2.data(), b2.size()), "C2 forged ROM parses");
    ok(!d2.findIp(kSdma0Hwid, 0, e), "C2 findIp(42,0) sees nothing — the blind spot");
    SdmaCensus c2 = {};
    ok(sdma_census(d2, c2), "C2 census still runs");
    eq_u("C2 count sees all three", c2.count, 3);
    eq_u("C2 mask", c2.mask, 0x1);   // every one of them is instance_number 0
    ok(c2.count > kSDMAInstanceCount, "C2 the count exceeds the addressable window");

    // C3: the mask tracks instance_number, not order of appearance.
    auto b3 = RomBuilder().ips({ ref(kSdma0Hwid, 1), ref(kSdma1Hwid, 2) }, 1).finish();
    IpDiscovery d3; d3.init(b3.data(), b3.size());
    SdmaCensus c3 = {};
    sdma_census(d3, c3);
    eq_u("C3 count", c3.count, 2);
    eq_u("C3 mask from instance numbers, not order", c3.mask, 0x6);

    // C4: no engine at all.
    auto b4 = RomBuilder().ips(none, 1).finish();
    IpDiscovery d4; d4.init(b4.data(), b4.size());
    SdmaCensus c4 = {};
    ok(sdma_census(d4, c4), "C4 census runs on an engine-less table");
    eq_u("C4 count is zero, not two", c4.count, 0);
    eq_u("C4 mask", c4.mask, 0);

    // C5: a truncated walk. 28 filler records push the last one's base list off
    // the end of the image, so ipAt() fails there — the census keeps what it
    // read and still reports a real answer, because "1 engine and a short table"
    // is not "0 engines".
    auto b5 = RomBuilder().ips({ ref(kSdma0Hwid, 0), ref(kSdma1Hwid, 1) }, 1, 470, 32).finish();
    IpDiscovery d5; d5.init(b5.data(), b5.size());
    SdmaCensus c5 = {};
    ok(sdma_census(d5, c5), "C5 truncated walk still returns true");
    eq_u("C5 the engine before the fault is kept", c5.count, 1);
    eq_u("C5 mask", c5.mask, 0x1);
    ok(!d5.ipAt((uint16_t)(d5.ipCount() - 1), e), "C5 the record that faults really does fault");

    // C6: no table at all -> false, and out is reset, never left at a stale
    // count from a previous call.
    SdmaCensus c6 = {};
    c6.count = 7; c6.mask = 0xFF;
    IpDiscovery d6;
    ok(!sdma_census(d6, c6), "C6 an unparsed table reports false");
    eq_u("C6 out was reset: count", c6.count, 0);
    eq_u("C6 out was reset: mask", c6.mask, 0);

    // C7: a buffer that is not a discovery table at all.
    std::vector<uint8_t> junk(kRomSize, 0xA5);
    IpDiscovery d7;
    ok(!d7.init(junk.data(), junk.size()), "C7 junk does not parse");
    ok(!sdma_census(d7, c6), "C7 census refuses it");
    eq_u("C7 count stays 0", c6.count, 0);
}

// --------------------------------------------- D: apply + addressable clamp
static void section_D()
{
    using namespace amdgpu;
    SDMAContext s = {};
    ok(s.census_from_discovery == false, "D1 a fresh context has no census");
    eq_u("D1 fresh num_instances is 0, not 2", s.num_instances, 0);

    SdmaCensus c = {}; c.count = 2; c.mask = 0x3;
    log_reset();
    sdma_apply_census(s, c);
    ok(s.census_from_discovery, "D2 the census is recorded as having happened");
    eq_u("D2 num_instances", s.num_instances, 2);
    eq_u("D2 sdma_mask", s.sdma_mask, 0x3);
    eq_u("D2 addressable", sdma_addressable_instances(s), 2);
    ok(log_has("2 SDMA engine(s) in IP discovery"), "D2 the count is logged");
    ok(!log_has("NOT brought up"), "D2 two engines need no warning");
    ok(!log_has("instance_number 0"), "D2 a mask with bit 0 set needs no warning");
    eq_u("D2 nothing else was reached for", (uint64_t)gStubsCalled, 0);

    // D3: one engine.
    SDMAContext one = {}; one.instance_present[0] = true; one.instance_present[1] = true;
    c = SdmaCensus(); c.count = 1; c.mask = 0x1;
    log_reset(); sdma_apply_census(one, c);
    eq_u("D3 addressable drops to 1", sdma_addressable_instances(one), 1);
    ok(!one.instance_present[1], "D3 the engine the card does not name is not present");
    ok(one.instance_present[0], "D3 the engine it does name is left for the probe to decide");

    // D4: three engines — counted truthfully, acted on only for the two the
    // register window can address.
    SDMAContext three = {};
    c = SdmaCensus(); c.count = 3; c.mask = 0x7;
    log_reset(); sdma_apply_census(three, c);
    eq_u("D4 num_instances keeps the truth", three.num_instances, 3);
    eq_u("D4 addressable clamps", sdma_addressable_instances(three), kSDMAInstanceCount);
    ok(log_has("engines 2..2 are NOT brought up"), "D4 the un-brought-up engine is named in the log");
    ok(log_has("sdma_reg_offset"), "D4 with the reason");

    // D5: engines numbered 1 and 2 — count 2, mask 0b110.
    SDMAContext weird = {};
    c = SdmaCensus(); c.count = 2; c.mask = 0x6;
    log_reset(); sdma_apply_census(weird, c);
    eq_u("D5 addressable (count 2)", sdma_addressable_instances(weird), 2);
    ok(log_has("no engine has instance_number 0"), "D5 the numbering mismatch is warned about");
    ok(log_has("lowest is 1"), "D5 naming the lowest instance number it did see");

    // D6: zero engines.
    SDMAContext zero = {}; zero.instance_present[0] = true; zero.instance_present[1] = true;
    c = SdmaCensus(); c.count = 0; c.mask = 0;
    log_reset(); sdma_apply_census(zero, c);
    eq_u("D6 addressable is 0", sdma_addressable_instances(zero), 0);
    ok(!zero.instance_present[0] && !zero.instance_present[1],
       "D6 no engine can be present when the card names none");
    ok(log_has("names no SDMA engine at all"), "D6 the refuse reason is logged");

    // D7: overflow.
    SDMAContext of = {};
    c = SdmaCensus(); c.count = kSDMAMaxInstances; c.mask = 0xFFFF; c.overflow = 5;
    log_reset(); sdma_apply_census(of, c);
    eq_u("D7 census_overflow recorded", of.census_overflow, 5);
    eq_u("D7 addressable still clamps to the window", sdma_addressable_instances(of), 2);
    ok(log_has("beyond the 16-instance array"), "D7 the overflow is logged");
    ok(log_has("and more past the array"), "D7 and flagged on the count line");

    // D8: idempotence — the ladder can be run twice in one boot.
    SDMAContext twice = {};
    c = SdmaCensus(); c.count = 1; c.mask = 0x1;
    sdma_apply_census(twice, c); sdma_apply_census(twice, c);
    eq_u("D8 applying twice leaves count 1", twice.num_instances, 1);
    eq_u("D8 and mask 1", twice.sdma_mask, 1);

    // D9: the clamp is a ceiling on the ARRAY, not on the answer. Every census
    // value the walk can produce must map to an addressable count in [0, 2].
    for (uint32_t n = 0; n <= 40; n++) {
        SDMAContext t = {};
        SdmaCensus x = {};
        x.count = n; x.mask = 0x1;
        sdma_apply_census(t, x);
        const uint32_t a = sdma_addressable_instances(t);
        if (a != (n < kSDMAInstanceCount ? n : kSDMAInstanceCount)) {
            ok(false, "D9 addressable is min(count, window) for every count");
            std::printf("      at count %u: addressable %u\n", n, a);
            break;
        }
        log_reset();
    }
    ok(true, "D9 addressable == min(count, kSDMAInstanceCount) for count 0..40");

    // D10: the instance array is 16 wide, so a census up to the cap can never
    // index out of it — the reason kSDMAInstanceCount stopped being the array
    // size in this patch.
    ok(sizeof(s.instance) / sizeof(s.instance[0]) == kSDMAMaxInstances,
       "D10 SDMAContext.instance is sized by the cap, not by the window");
    ok(sizeof(s.instance_present) / sizeof(s.instance_present[0]) == kSDMAMaxInstances,
       "D10b instance_present likewise");
    eq_u("D10c no kernel stub was reached in section D", (uint64_t)gStubsCalled, 0);
}

// ------------------------------------------------------- E: source pins
static std::string slurp(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    // The pins below cross line breaks, and a Windows checkout hands back CRLF:
    // strip the carriage returns so the same needles match on either platform.
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}
static size_t count_of(const std::string &s, const std::string &n)
{
    size_t c = 0, p = 0;
    while ((p = s.find(n, p)) != std::string::npos) { c++; p += n.size(); }
    return c;
}
static const size_t NP = std::string::npos;

static void section_E(const std::string &K)
{
    const std::string sdma = slurp(K + "amd/sdma_v7_0.cpp");
    const std::string init = slurp(K + "amd/amdgpu_init.cpp");
    const std::string brg  = slurp(K + "Navi48Bringup.cpp");
    const std::string iph  = slurp(K + "amd/amdgpu_ip.h");
    const std::string sdh  = slurp(K + "amd/amdgpu_sdma.h");
    ok(!sdma.empty() && !init.empty() && !brg.empty() && !iph.empty() && !sdh.empty(),
       "E0 the pinned sources are readable (pass the navi48-bringup dir as argv[1])");
    if (sdma.empty()) return;

    // E1: sdma_init_full's five per-instance loops iterate nEngines. This is the
    // break sections A-D cannot see — with the clamp at 2 the two bounds agree
    // on every die a host can forge — so it is pinned on the text.
    const size_t fs = sdma.find("sdma_init_full(DeviceContext &dev,");
    const size_t fe = sdma.find("\n} // namespace amdgpu", fs);
    ok(fs != NP && fe != NP && fe > fs, "E1 the sdma_init_full body is located");
    const std::string body = (fs != NP && fe != NP) ? sdma.substr(fs, fe - fs) : std::string();
    eq_u("E1 five per-instance loops in init_full", count_of(body, "for (uint32_t i = 0; i < nEngines; i++)"), 5);
    eq_u("E1 none of them bounded by the constant", count_of(body, "i < kSDMAInstanceCount; i++)"), 0);
    ok(body.find("const uint32_t nEngines = sdma_addressable_instances(sdma);") != NP,
       "E2 nEngines comes from the census-derived helper");
    ok(body.find("if (!sdma.census_from_discovery)") != NP,
       "E2 the no-census fallback is gated on the flag, not on a default");
    ok(body.find("pre-0011 behaviour") != NP, "E2 the fallback calls itself an assumption");
    ok(body.find("no IP discovery census recorded") != NP, "E2 with its own log line");
    ok(body.find("refusing to") != NP && body.find("if (nEngines == 0)") != NP,
       "E3 a zero census aborts instead of assuming");

    // E4: the census really reaches the ladder and the survey.
    const size_t rs = brg.find("Navi48Bringup::runStages");
    ok(rs != NP && brg.find("sdma_apply_census(ctx.sdma, cens)", rs) != NP,
       "E4 runStages hands the census to the ladder context");
    const size_t ap = brg.find("sdma_apply_census(ctx.sdma, cens)", rs);
    const size_t st = brg.find("ctx.reached = amdgpu::BringupStage::IPDiscovery", rs);
    ok(ap != NP && st != NP && ap > st, "E4b the apply follows the stage reset (before any stage runs)");
    ok(brg.find("sdma_census(ipDiscovery, cens)", rs) != NP, "E4c taken from the live discovery table");
    const size_t sv = brg.find("Navi48Bringup::surveyIPs");
    ok(sv != NP && brg.find("\"SDMA,Count\"", sv) != NP, "E5 the survey publishes SDMA,Count");
    ok(sv != NP && brg.find("\"SDMA,HwidMask\"", sv) != NP, "E5b SDMA,HwidMask");
    ok(sv != NP && brg.find("\"SDMA,Census\"", sv) != NP, "E5c SDMA,Census");
    ok(sv != NP && brg.find("no valid IP discovery table", sv) != NP,
       "E5d a missing table says so instead of publishing 0");

    // E6: the constants, and the bounds that must NOT move.
    ok(sdh.find("constexpr uint32_t kSDMAMaxInstances      = 16;") != NP,
       "E6 the array ceiling is the upstream cap");
    ok(sdh.find("SDMAInstance instance[kSDMAMaxInstances];") != NP,
       "E6b the instance array is sized by the ceiling");
    ok(sdh.find("SDMAInstance instance[kSDMAInstanceCount];") == NP,
       "E6c and no longer by the window");
    ok(sdh.find("bool         instance_present[kSDMAMaxInstances];") != NP, "E6d presence array widened");
    ok(init.find("if (sl.instance >= kSDMAInstanceCount ||") != NP,
       "E7 the qn-test slot bound stays on the ADDRESSABLE window (it drives MMIO)");
    ok(init.find("for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {\n            if (!ctx.sdma.instance[i].inited) continue;") != NP,
       "E7b the MMIO teardown loop stays window-bounded on purpose");
    ok(init.find("for (uint32_t i = 0; i < kSDMAMaxInstances; i++)\n        sdma_release_storage") != NP,
       "E7c the release loop, which touches no register, walks the whole array");
    ok(init.find("sdma_engine_halt() reaches the engine") != NP,
       "E7d the reason for keeping a bound is written next to it");

    // E8: the doorbell map, against the fetched AMDGPU_NAVI10_DOORBELL_ASSIGNMENT
    // (amdgpu_doorbell.h:186-236). These literals are the ones upstream ships;
    // a silent edit here moves a ring's doorbell window.
    ok(iph.find("uint32_t kiq             = 0x000;") != NP, "E8 KIQ 0x000 (NAVI10 DOORBELL_KIQ)");
    ok(iph.find("uint32_t hiq             = 0x001;") != NP, "E8 HIQ 0x001");
    ok(iph.find("uint32_t diq             = 0x002;") != NP, "E8 DIQ 0x002");
    ok(iph.find("uint32_t mes_ring0       = 0x00B;") != NP, "E8 MES ring0 0x00B");
    ok(iph.find("uint32_t mes_ring1       = 0x00C;") != NP, "E8 MES ring1 0x00C");
    ok(iph.find("uint32_t userqueue_start = 0x00D;") != NP, "E8 userqueue start 0x00D");
    ok(iph.find("uint32_t userqueue_end   = 0x08A;") != NP, "E8 userqueue end 0x08A");
    ok(iph.find("uint32_t gfx_ring0           = 0x08B;") != NP, "E8 GFX ring0 0x08B");
    ok(iph.find("uint32_t gfx_ring1           = 0x08C;") != NP, "E8 GFX ring1 0x08C");
    ok(iph.find("uint32_t gfx_userqueue_end   = 0x0FF;") != NP, "E8 GFX userqueue end 0x0FF");
    ok(iph.find("{0x100, 0x10A, 0x114, 0x11E}") != NP,
       "E8 sDMA ENGINE0-3 0x100/0x10A/0x114/0x11E (the four the Navi 10 map defines)");
    ok(iph.find("sdma_doorbell_range = 20;") != NP, "E8 SDMA doorbell range 20");
    ok(iph.find("uint32_t ih                  = 0x178;") != NP, "E8 IH 0x178");
    // And the two the fetched header does NOT back: upstream's own
    // MAX_ASSIGNMENT is VPE (0x190) and it has no mes_aggregated field at all
    // (amdgpu_mes.c:61 derives those at run time). The comment must keep saying
    // so, or the next reader takes them for ASIC facts.
    ok(iph.find("no\n// `mes_aggregated` field at all") != NP,
       "E9 the header says upstream has no mes_aggregated field");
    ok(iph.find("db_start_dw_offset + i * 2") != NP,
       "E9b and cites how upstream really derives them (amdgpu_mes.c:61)");
    ok(iph.find("module-parameter defaults") != NP,
       "E9c and labels the amdgpu_drv.c numbers for what they are");
    ok(iph.find("sdma_engine[16]") != NP, "E9d upstream's array is 16 wide, ours 4");
}

int main(int argc, char **argv)
{
    const std::string arg = argc > 1 ? argv[1] : ".";
    const std::string K = arg + "/src/";
    std::printf("== 0011 SDMA census: A-D behavioural, C against forged IPDS ROMs, E source pins ==\n");
    section_A();
    section_B();
    section_C();
    section_D();
    section_E(K);
    std::printf("\n%d checks, %d failures\n", gRun, gFail);
    if (amdgpu::gStubsCalled)
        std::printf("NOTE %d kernel-side stub call(s) — the census was not supposed to reach any\n",
                    amdgpu::gStubsCalled);
    return gFail == 0 && amdgpu::gStubsCalled == 0 ? 0 : 1;
}
