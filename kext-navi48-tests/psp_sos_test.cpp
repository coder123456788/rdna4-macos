// psp_sos_test.cpp — host-side checks for the PSP pre-SOS chain (psp.cpp)
// against the real Navi 44 and Navi 48 SOS containers.
//
//     zig c++ -std=c++17 -I hosttest/shim -I src -I src/amd \
//         tests/psp_sos_test.cpp src/psp.cpp -o psp_sos_test
//     ./psp_sos_test [firmware dir]
//
// This is the first stage that WRITES to the GPU, and it is the stage whose
// firmware set changes per die (patch 0002). Everything below the hardware
// boundary is real code: PspLoader::parseContainer, preflight, runChain. The
// only fiction is an HwAccess that remembers MP0 register writes and a VRAM
// byte array instead of BAR0 — enough to prove, on Windows and with no GPU,
// that the 14.0.2 container produces the same bootloader sequence as the
// 14.0.3 one and that every staged image lands where the protocol says.
#include "psp.hpp"
#include "regs.hpp"
#include "ipdiscovery.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace amdgpu {
// psp.cpp does not log through this, but the shim header chain may; keep the
// symbol present so the link matches the other host tests.
void n48_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
} // namespace amdgpu

namespace {

int gFail = 0, gRun = 0;

void ok(const char *what, bool cond)
{
    gRun++;
    if (!cond) { gFail++; std::printf("FAIL  %s\n", what); }
}

void eq(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-58s got %llu want %llu\n", what,
                    (unsigned long long)got, (unsigned long long)want);
    }
}

void eqs(const char *what, const char *got, const char *want)
{
    gRun++;
    if (std::strcmp(got, want) != 0) {
        gFail++;
        std::printf("FAIL  %-58s got \"%s\" want \"%s\"\n", what, got, want);
    }
}

bool readFile(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    out.resize((size_t)sz);
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

// ---------------------------------------------------------------- the fake

// Stands in for the kext's HwBridge: registers live in a map, "VRAM" is a
// byte array reached at a VRAM byte offset (BAR0 == VRAM on this design), and
// the PSP bootloader is a small state machine that acks every load command it
// is handed and raises the SOS sign-of-life when it is handed LoadSOSDrv.
struct FakeGpu : n48::HwAccess {
    static constexpr uint32_t kMp0   = IpDiscovery::HwMp0;
    static constexpr uint32_t kMmhub = IpDiscovery::HwMmhub;
    static constexpr uint32_t kDmu   = IpDiscovery::HwDmu;
    // psp.cpp keeps this one file-local; mirrored so the recorder knows which
    // MP0 dword carries the fw_pri address.
    static constexpr uint32_t kC2PMsg36 = 0x0064;

    std::map<uint64_t, uint32_t> regs;      // key: hw<<32 | seg<<24 | dword
    std::vector<uint8_t> vram;

    // protocol recorder
    std::vector<uint32_t> cmds;             // every value written to C2PMSG_35
    std::vector<uint32_t> addr20;           // every value written to C2PMSG_36
    std::vector<std::pair<uint64_t,uint32_t>> memsets;   // (off, len)
    std::vector<std::pair<uint64_t,uint32_t>> writes;    // (off, len)
    std::vector<uint8_t>  lastStaged;       // bytes of the most recent vramWrite
    uint64_t              lastStagedOff = ~0ull;

    // knobs
    bool     bootloaderReady = true;
    bool     answerLoads     = true;   // false: status never returns ready again
    bool     raiseSos        = true;   // false: SOS never reports alive
    uint32_t sosPollsToLive  = 2;
    uint32_t sosPolls        = 0;
    bool     regsUnreadable  = false;  // every read returns 0xFFFFFFFF

    uint32_t fbBaseUnits = 0x9000;     // 16 MiB units -> MC base 0x9000_0000_00
    uint32_t fbTopUnits  = 0x9100;     // 256 MiB of VRAM
    uint32_t fbOffsetUnits = 0x9000;

    explicit FakeGpu(uint64_t vramBytes) : vram((size_t)vramBytes, 0) {}

    static uint64_t key(uint32_t hw, uint8_t seg, uint32_t dword)
    {
        return ((uint64_t)hw << 32) | ((uint64_t)seg << 24) | dword;
    }

    void preload()
    {
        regs[key(kMp0, 0, n48::kPspC2PMsg35)] = bootloaderReady ? 0x80000000u : 0u;
        regs[key(kMp0, 0, n48::kPspC2PMsg81)] = 0u;
        regs[key(kMmhub, 0, 0x0554)] = fbBaseUnits;
        regs[key(kMmhub, 0, 0x0555)] = fbTopUnits;
        regs[key(kMmhub, 0, 0x04c7)] = fbOffsetUnits;
        regs[key(kDmu, 2, 0x0475)]   = fbBaseUnits;
    }

    uint32_t regReadIp(uint16_t hw, uint8_t inst, uint8_t seg, uint32_t dword) override
    {
        (void)inst;
        if (regsUnreadable) return 0xFFFFFFFFu;
        auto it = regs.find(key(hw, seg, dword));
        return it == regs.end() ? 0u : it->second;
    }

    bool regWriteIp(uint16_t hw, uint8_t inst, uint8_t seg, uint32_t dword, uint32_t value) override
    {
        (void)inst;
        regs[key(hw, seg, dword)] = value;
        if (hw != kMp0 || seg != 0) return true;
        if (dword == kC2PMsg36) { addr20.push_back(value); return true; }
        if (dword != n48::kPspC2PMsg35) return true;

        cmds.push_back(value);
        // The bootloader consumes the command: it drops "ready", then raises
        // it again when the load is done. LoadSOSDrv additionally starts the
        // SOS and eventually posts the sign-of-life.
        uint32_t status = 0x80000000u;
        if (!answerLoads) status = 0u;
        else if (value == 0x00020000u /*LoadSOSDrv*/) {
            if (raiseSos) {
                regs[key(kMp0, 0, n48::kPspC2PMsg81)] = 0x0A0B0C0Du;
                status = 0x80000000u;
            } else {
                status = 0u;
            }
        }
        regs[key(kMp0, 0, n48::kPspC2PMsg35)] = status;
        return true;
    }

    bool vramWrite(uint64_t off, const void *src, size_t len) override
    {
        if (off + len > vram.size()) return false;
        std::memcpy(vram.data() + off, src, len);
        writes.push_back({off, (uint32_t)len});
        lastStaged.assign((const uint8_t *)src, (const uint8_t *)src + len);
        lastStagedOff = off;
        return true;
    }

    bool vramMemset(uint64_t off, uint8_t value, size_t len) override
    {
        if (off + len > vram.size()) return false;
        std::memset(vram.data() + off, value, len);
        memsets.push_back({off, (uint32_t)len});
        return true;
    }
};

constexpr uint32_t kCmdKDB      = 0x00080000u;
constexpr uint32_t kCmdSPL      = 0x10000000u;
constexpr uint32_t kCmdSYS      = 0x00010000u;
constexpr uint32_t kCmdSOC      = 0x000B0000u;
constexpr uint32_t kCmdINTF     = 0x000D0000u;
constexpr uint32_t kCmdHAD      = 0x000C0000u;
constexpr uint32_t kCmdRAS      = 0x000E0000u;
constexpr uint32_t kCmdIPKEYMGR = 0x000F0000u;
constexpr uint32_t kCmdSOS      = 0x00020000u;

const uint32_t kExpectedChain[] = {
    kCmdKDB, kCmdSPL, kCmdSYS, kCmdSOC, kCmdINTF, kCmdHAD, kCmdRAS, kCmdIPKEYMGR, kCmdSOS,
};

// The chain skips components the container does not carry, so the expected
// command list is the fixed order filtered by presence. This is how the two
// dies differ: 14.0.2 ships no RAS image, 14.0.3 does.
std::vector<uint32_t> expectedChain(const n48::PspLoader &psp)
{
    const bool present[] = {
        psp.kdb.present(), psp.spl.present(), psp.sys.present(), psp.soc.present(),
        psp.intf.present(), psp.dbg.present(), psp.ras.present(), psp.ipkey.present(),
        psp.sos.present(),
    };
    std::vector<uint32_t> out;
    for (size_t i = 0; i < sizeof(present) / sizeof(present[0]); i++)
        if (present[i]) out.push_back(kExpectedChain[i]);
    return out;
}

// ------------------------------------------------------------------- tests

struct Die {
    const char *label;
    const char *file;
    bool navi44;
};

void containerTests(const std::string &fwDir, const Die &die,
                    std::vector<uint8_t> &blobOut)
{
    std::printf("\n-- %s --\n", die.label);
    const std::string path = fwDir + "/" + die.file;
    std::vector<uint8_t> blob;
    if (!readFile(path, blob)) {
        std::printf("FAIL  cannot read %s\n", path.c_str());
        gFail++; gRun++;
        return;
    }
    blobOut = blob;

    FakeGpu gpu(256ull << 20);
    gpu.preload();
    n48::PspLoader psp(gpu);

    ok("container parses", psp.parseContainer(blob.data(), blob.size()));
    ok("SOS sub-image present", psp.sos.present());
    ok("SOS fits the 1 MiB fw_pri window", psp.sos.size <= n48::PspLoader::kFwPriSize);
    ok("SOS points inside the container",
       psp.sos.data >= blob.data() && psp.sos.data + psp.sos.size <= blob.data() + blob.size());

    // The pre-SOS chain can only load what the container actually carries.
    // Both dies are expected to carry the same eight components plus SOS;
    // TOC/RL/SPDM are present in some containers but are not loaded pre-SOS
    // (psp_14_0_5 is the first PSP that loads TOC).
    std::printf("     %s: KDB %u SPL %u SYS %u SOC %u INTF %u HAD %u RAS %u IPKEY %u "
                "SOS %u (v0x%08x) TOC %u RL %u SPDM %u\n",
                die.label, psp.kdb.size, psp.spl.size, psp.sys.size, psp.soc.size,
                psp.intf.size, psp.dbg.size, psp.ras.size, psp.ipkey.size,
                psp.sos.size, psp.sos.version, psp.toc.size, psp.rl.size, psp.spdm.size);

    ok("KDB in container", psp.kdb.present());
    ok("SPL in container", psp.spl.present());
    ok("SYS_DRV in container", psp.sys.present());
    ok("SOC_DRV in container", psp.soc.present());
    ok("INTF_DRV in container", psp.intf.present());
    ok("HAD_DRV in container", psp.dbg.present());
    ok("IPKEYMGR in container", psp.ipkey.present());

    // The one component the two dies disagree on: 14.0.2 (Navi 44) ships no
    // RAS image, 14.0.3 (Navi 48) does. The chain skips what is absent, which
    // is also what Linux does (`if (adev->psp.ras.ucode_size)`).
    if (die.navi44) ok("navi44 14.0.2 ships no RAS image", !psp.ras.present());
    else            ok("navi48 14.0.3 ships a RAS image", psp.ras.present());

    // Nothing the chain stages may exceed the single 1 MiB window.
    ok("every staged component fits fw_pri",
       psp.kdb.size <= n48::PspLoader::kFwPriSize &&
       psp.spl.size <= n48::PspLoader::kFwPriSize &&
       psp.sys.size <= n48::PspLoader::kFwPriSize &&
       psp.soc.size <= n48::PspLoader::kFwPriSize &&
       psp.intf.size <= n48::PspLoader::kFwPriSize &&
       psp.dbg.size <= n48::PspLoader::kFwPriSize &&
       psp.ras.size <= n48::PspLoader::kFwPriSize &&
       psp.ipkey.size <= n48::PspLoader::kFwPriSize);

    (void)die.navi44;
}

// Drives preflight + runChain against the fake and checks the protocol.
void chainTests(const Die &die, const std::vector<uint8_t> &blob)
{
    std::printf("\n-- %s: chain --\n", die.label);
    constexpr uint64_t kVramBytes = 256ull << 20;
    constexpr uint64_t kFwPriOff   = 0x40ull << 20;   // 1 MiB aligned, 64 MiB in

    FakeGpu gpu(kVramBytes);
    gpu.preload();
    n48::PspLoader psp(gpu);
    ok("prep: container parses", psp.parseContainer(blob.data(), blob.size()));
    ok("prep: fw_pri offset accepted", psp.setFwPriOffset(kFwPriOff));

    ok("preflight passes on a posted FB", psp.preflight());
    const uint64_t mcBase = (uint64_t)gpu.fbBaseUnits << 24;
    eq("mcBase follows FB_LOCATION_BASE", psp.mcBase(), mcBase);
    eq("fwPriMC = MC base + VRAM offset", psp.fwPriMC(), mcBase + kFwPriOff);

    ok("runChain reports SOS alive", psp.runChain());

    // The exact bootloader conversation: every component the container carries,
    // in upstream order, then SOS.
    const std::vector<uint32_t> want = expectedChain(psp);
    eq("command count", gpu.cmds.size(), want.size());
    for (size_t i = 0; i < gpu.cmds.size() && i < want.size(); i++)
        eq("command order", gpu.cmds[i], want[i]);

    // Every command carries the same fw_pri address, in 1 MiB units.
    eq("one address per command", gpu.addr20.size(), gpu.cmds.size());
    for (size_t i = 0; i < gpu.addr20.size(); i++)
        eq("C2PMSG_36 = fw_pri MC >> 20", gpu.addr20[i], (uint32_t)((mcBase + kFwPriOff) >> 20));

    // Each load zeroes the window first, then copies image bytes at its start.
    eq("one memset per load", gpu.memsets.size(), gpu.cmds.size());
    eq("one copy per load", gpu.writes.size(), gpu.cmds.size());
    for (auto &m : gpu.memsets) {
        eq("memset at fw_pri", m.first, kFwPriOff);
        eq("memset covers fw_pri", m.second, n48::PspLoader::kFwPriSize);
    }
    for (auto &w : gpu.writes) eq("copy at fw_pri", w.first, kFwPriOff);

    // The last staged image is SOS, byte for byte.
    eq("SOS is the last thing staged", gpu.lastStaged.size(), (uint64_t)psp.sos.size);
    ok("SOS bytes landed unchanged",
       gpu.lastStaged.size() == psp.sos.size &&
       std::memcmp(gpu.lastStaged.data(), psp.sos.data, psp.sos.size) == 0);
    ok("SOS bytes are in the VRAM array",
       std::memcmp(gpu.vram.data() + kFwPriOff, psp.sos.data, psp.sos.size) == 0);
    eqs("last step", psp.lastStep(), "SOS");
    ok("sign-of-life recorded", psp.lastBootloaderStatus() != 0);
}

void refusalTests(const Die &die, const std::vector<uint8_t> &blob, bool slow)
{
    std::printf("\n-- %s: refusals --\n", die.label);
    constexpr uint32_t kAligned = 0x4000000u;

    // Unaligned fw_pri: C2PMSG_36 only carries addr>>20, so a byte-granular
    // offset would silently stage the image somewhere else.
    {
        FakeGpu gpu(256ull << 20);
        gpu.preload();
        n48::PspLoader psp(gpu);
        (void)psp.parseContainer(blob.data(), blob.size());
        ok("unaligned fw_pri refused", !psp.setFwPriOffset(kAligned + 0x1000));
        eq("unaligned offset did not take", psp.fwPriOffset(), 0u);
    }

    // MP0/MMHUB unreadable.
    {
        FakeGpu gpu(256ull << 20);
        gpu.preload();
        gpu.regsUnreadable = true;
        n48::PspLoader psp(gpu);
        (void)psp.parseContainer(blob.data(), blob.size());
        (void)psp.setFwPriOffset(kAligned);
        ok("preflight refuses unreadable registers", !psp.preflight());
    }

    // FB_LOCATION never posted (base 0): the MC base would be wrong, and
    // C2PMSG_36 would point the bootloader at the wrong memory.
    {
        FakeGpu gpu(256ull << 20);
        gpu.fbBaseUnits = 0; gpu.fbTopUnits = 0;
        gpu.preload();
        n48::PspLoader psp(gpu);
        (void)psp.parseContainer(blob.data(), blob.size());
        (void)psp.setFwPriOffset(kAligned);
        ok("preflight refuses an unposted FB", !psp.preflight());
    }

    // fw_pri outside the FB window.
    {
        FakeGpu gpu(256ull << 20);
        gpu.fbTopUnits = gpu.fbBaseUnits + 1;   // 16 MiB of VRAM
        gpu.preload();
        n48::PspLoader psp(gpu);
        (void)psp.parseContainer(blob.data(), blob.size());
        (void)psp.setFwPriOffset(0x4000000u);
        ok("preflight refuses fw_pri outside VRAM", !psp.preflight());
    }

    // The two timeout paths below spend the loader's real budget (10 s per
    // bootloader wait, 5 s for SOS) because psp.cpp is driven with wall-clock
    // IOSleep and the host shim maps that to usleep. They are correct, just
    // slow (~4 min on Windows), so they run only with --slow.
    if (slow) {
        // Bootloader never acks the first command: the chain must stop at KDB,
        // not spin through the rest.
        {
            FakeGpu gpu(256ull << 20);
            gpu.preload();
            n48::PspLoader psp(gpu);
            (void)psp.parseContainer(blob.data(), blob.size());
            (void)psp.setFwPriOffset(kAligned);
            ok("prep: preflight passes", psp.preflight());
            gpu.answerLoads = false;
            ok("chain fails when the bootloader stops acking", !psp.runChain());
            eqs("chain stopped at the first step", psp.lastStep(), "KDB");
            eq("only one command was issued", gpu.cmds.size(), 1u);
        }

        // SOS never comes alive.
        {
            FakeGpu gpu(256ull << 20);
            gpu.preload();
            n48::PspLoader psp(gpu);
            (void)psp.parseContainer(blob.data(), blob.size());
            (void)psp.setFwPriOffset(kAligned);
            ok("prep: preflight passes", psp.preflight());
            gpu.raiseSos = false;
            ok("chain fails when SOS stays down", !psp.runChain());
            eqs("failure is attributed to SOS", psp.lastStep(), "SOS");
        }
    } else {
        std::printf("     (timeout paths skipped; pass --slow to run them)\n");
    }

    // Truncated and garbage containers.
    {
        FakeGpu gpu(256ull << 20);
        n48::PspLoader psp(gpu);
        ok("empty container refused", !psp.parseContainer(blob.data(), 16));

        std::vector<uint8_t> junk(blob.size(), 0xA5);
        ok("garbage container refused", !psp.parseContainer(junk.data(), junk.size()));

        std::vector<uint8_t> cut(blob.begin(), blob.begin() + blob.size() / 2);
        ok("truncated container refused", !psp.parseContainer(cut.data(), cut.size()));
    }

    (void)die;
}

void comparisonTests(const std::vector<uint8_t> &n44, const std::vector<uint8_t> &n48blob)
{
    std::printf("\n-- die comparison --\n");
    FakeGpu a(256ull << 20), b(256ull << 20);
    a.preload(); b.preload();
    n48::PspLoader pa(a), pb(b);
    ok("navi44 container parses", pa.parseContainer(n44.data(), n44.size()));
    ok("navi48 container parses", pb.parseContainer(n48blob.data(), n48blob.size()));

    // Same shape, different ucode: the two dies agree on every component
    // except RAS, which 14.0.2 does not ship, and the chain adapts to that.
    ok("both containers carry the same component set apart from RAS",
       pa.kdb.present() == pb.kdb.present() && pa.spl.present() == pb.spl.present() &&
       pa.sys.present() == pb.sys.present() && pa.soc.present() == pb.soc.present() &&
       pa.intf.present() == pb.intf.present() && pa.dbg.present() == pb.dbg.present() &&
       pa.ipkey.present() == pb.ipkey.present() && pa.sos.present() == pb.sos.present() &&
       !pa.ras.present() && pb.ras.present());
    ok("SOS differs between the dies", pa.sos.version != pb.sos.version ||
       pa.sos.size != pb.sos.size ||
       std::memcmp(pa.sos.data, pb.sos.data, pa.sos.size < pb.sos.size ? pa.sos.size : pb.sos.size) != 0);
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string fwDir = "firmware";
    bool slow = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--slow") == 0) slow = true;
        else fwDir = argv[i];
    }
    std::printf("psp_sos_test -- PSP pre-SOS chain on the real 14.0.2 / 14.0.3 SOS containers%s\n",
                slow ? " (--slow)" : "");

    const Die navi44{"navi44 psp_14_0_2_sos", "psp_14_0_2_sos.bin", true};
    const Die navi48{"navi48 psp_14_0_3_sos", "psp_14_0_3_sos.bin", false};

    std::vector<uint8_t> blob44, blob48;
    containerTests(fwDir, navi44, blob44);
    containerTests(fwDir, navi48, blob48);

    if (!blob44.empty()) chainTests(navi44, blob44);
    if (!blob48.empty()) chainTests(navi48, blob48);
    if (!blob44.empty()) refusalTests(navi44, blob44, slow);
    if (!blob44.empty() && !blob48.empty()) comparisonTests(blob44, blob48);

    std::printf("\n================ SUMMARY ================\n");
    std::printf("%d check(s) failed out of %d\n", gFail, gRun);
    return gFail == 0 ? 0 : 1;
}
