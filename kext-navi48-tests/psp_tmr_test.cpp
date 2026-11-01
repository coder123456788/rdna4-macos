//
//  tests/psp_tmr_test.cpp — host coverage for the psp_v14 TMR / LOAD_TOC stage.
//
//  Why this file exists: patch 0009 changed how psp_setup_tmr() names and selects the
//  PSP IP version, and no suite linked src/amd/psp_v14_0.cpp, so that edit was
//  compile-verified only. This suite runs the REAL module — psp_init, psp_ring_create,
//  psp_setup_tmr, psp_load_toc_subbin — against a simulated SOS: a BAR5 register file,
//  a 32 MiB BAR0 VRAM aperture, and a fake PSP that answers from dalshim_sleep (IOSleep)
//  by parsing the GPCOM ring frame and the 1 KB command buffer the way the firmware
//  does. Nothing here models the driver's decisions; the driver's own code makes them.
//
//  The fake models three protocol rules that a first boot will be diagnosed against,
//  because the kext's comments claim each one and only a slave that enforces it can
//  show the claim holding:
//    - C2PMSG_64 must be kicked with (PSP_RING_TYPE__KM << 16) = 0x20000, and the ring
//      address/size PSP later DMAs come from C2PMSG_69/70/71 written before the kick;
//    - a frame whose cmd_buf_size is non-zero is DROPPED, unanswered (upstream
//      amdgpu_psp.c:3485-3492 memsets the frame and sets five fields only);
//    - the response is read back from the SAME 1 KB command buffer at +864, and
//      LOAD_TOC's tmr_size at +864+16 = +880. The reference implementation read
//      cmd_buf+16 instead, so tmr_size is planted with a sentinel no other dword
//      can produce.
//
//  Scenarios run in one process with the machine rebuilt for each: psp_v14_0.cpp holds
//  no mutable file-scope state (only constexpr VRAM layout offsets), so a reset of
//  {registers, VRAM, DeviceContext, PSPContext, fake} is a complete fresh boot. The one
//  exception is amdgpu_hdp_flush()'s log-once flag, which this suite never asserts on.
//
//  build (host clang++/zig c++, no Mac, no GPU):
//    for f in src/fw/*.c; do zig cc -std=c11 -O1 -c $f -o $O/$(basename ${f%.c}).o; done
//    zig c++ -std=c++17 -Wall -Wextra -O1 -I tests/dalshim -I src -I src/amd \
//        -I src/fw tests/psp_tmr_test.cpp src/amd/psp_v14_0.cpp src/amd/fw_table.cpp \
//        $O/*.o -o psp_tmr_test
//    ./psp_tmr_test
//  tests/psp_tmr_plant.sh then plants breaks into the real sources and shows that every
//  one of them is caught here.
//
//  Covers:
//   A  psp_init guards: unresolved MP0, unresolved MMHUB, vramMcBase 0, VRAM span overrun
//   B  psp_ring_create guards: SOS not alive, psp_init never called, already created
//   C  the INIT_GPCOM_RING handshake: kick value, ring MC + size, C2PMSG_67 reset,
//      cmd/fence bus addresses, and the three windows zeroed - from residue, so the
//      zeroing is a claim about something, with the TMR slot left dirty on purpose
//   D  psp_setup_tmr guards: tmrSetUp already true, ring never created
//   E  the 0009 fix: the version is taken from discovery, per die, with the pin as the
//      fallback — and the two log lines that name it say the die's own number
//   F  v14 state: tmrBusAddr/tmrSize zeroed, tmrSetUp true, and NO SETUP_TMR ever sent
//   G  LOAD_TOC on the real Navi 44 container: the kext's own parser yields the
//      measured 2304-byte TOC, which reaches the wire as toc_size, staged at fw_pri
//      after the whole 1 MB was zeroed
//   H  the same on the real Navi 48 container (both dies carry a 2304-byte TOC)
//   I  no TOC sub-bin: success without a submit, and size 0 with a live pointer
//   J  failures: PSP status, fence timeout, and that tmrSetUp is NOT latched when
//      LOAD_TOC fails (the ordering that lets a dead PSP be treated as ready)
//   K  psp_load_toc_subbin's own argument guards
//   L  the wire format: every dword of the frame and of the LOAD_TOC union, the frame's
//      addresses composed against the layout literals (a half-by-half comparison cannot
//      see a missing MC base), and the kext's own constants pinned to this fake's numbers
//   M  the legacy branch: a non-v14 PSP does get SETUP_TMR, with upstream's fields
//   N  the dalshim kIOReturn values equal the kernel's, so a kr compared here is
//      the same number the kext will return on a Mac
//   O  psp_load_toc, which no caller uses yet: its header guards, and a real payload
//      staged at fw_pri and submitted as one LOAD_TOC frame
//
//  A frame SOS cannot address is a break, not a skip: the fake fails a named check when
//  a cmd_buf_addr / fence_addr is not an MC address, because a silent `continue` there
//  let a planted VRAM-offset-instead-of-MC frame pass the whole suite.
//
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <IOKit/IOLib.h>          // tests/dalshim: IOSleep -> dalshim_sleep
#include "amdgpu_psp.h"
#include "amdgpu_regs.h"
#include "amdgpu_ip.h"
#include "amdgpu_ucode_psp.h"   // common_firmware_header, for section O
#include "fw_table.h"

using namespace amdgpu;

#ifdef _WIN32
// bzero() is BSD; MSVC's CRT has no such symbol. This file is a host test — nothing in
// the shipped kext links it, and on macOS/Linux the real bzero is used.
extern "C" void bzero(void *dst, size_t n) { memset(dst, 0, n); }
#endif

// Captured here so a failed check can show what the kext actually logged; the writer
// (amdgpu::n48_logf) is defined further down with the log-ring commentary.
static std::vector<std::string> gLog;

static int gFail = 0, gRun = 0, gSectionFail = 0, gSectionRun = 0;
static const char *gSection = "";

static void dumpLogTail() {
    size_t n = gLog.size() > 8 ? gLog.size() - 8 : 0;
    for (; n < gLog.size(); n++) std::printf("   LOG| %s\n", gLog[n].c_str());
}
static void ok(const char *what, bool cond) {
    gRun++; gSectionRun++;
    if (!cond) {
        gFail++; gSectionFail++;
        std::printf("FAIL [%s] %s\n", gSection, what);
        dumpLogTail();
    }
}
static void eq(const char *what, uint64_t got, uint64_t want) {
    gRun++; gSectionRun++;
    if (got != want) {
        gFail++; gSectionFail++;
        std::printf("FAIL [%s] %s: got %#llx want %#llx\n", gSection, what,
                    (unsigned long long)got, (unsigned long long)want);
    }
}
static void section(const char *name) {
    if (*gSection)
        std::printf("-- %s: %u check(s), %u failed --\n", gSection, gSectionRun, gSectionFail);
    gSection = name; gSectionRun = 0; gSectionFail = 0;
}

// =====================================================================================================================================
// The log ring. psp_v14_0.cpp logs through n48_logf; capturing it is the only way to
// test which IP version the code believed it was talking to, and on a first boot that
// string is the evidence.
// =====================================================================================================================================
static std::string vfmt(const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    return std::string(buf);
}

namespace amdgpu {
void n48_logf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); std::string s = vfmt(fmt, ap); va_end(ap);
    gLog.push_back(s);
}
} // namespace amdgpu

static bool hasLog(const char *needle) {
    for (const std::string &l : gLog) if (l.find(needle) != std::string::npos) return true;
    return false;
}
static int countLog(const char *needle) {
    int n = 0;
    for (const std::string &l : gLog) if (l.find(needle) != std::string::npos) n++;
    return n;
}
static void logClear() { gLog.clear(); }

// =====================================================================================================================================
// The simulated card. Geometry is the kext's own: MP0/MMHUB bases are arbitrary
// dword indices into the BAR5 window, but the VRAM map is not — it is dev.vramBase
// plus psp_v14_0.cpp's layout offsets, so a wrong layout lands outside these windows
// and the fake refuses to play.
// =====================================================================================================================================
static const uint32_t kRegDwords  = 0x17000;          // 352 KiB of BAR5 registers
static const uint64_t kVramBytes  = 0x02000000ull;    // 32 MiB visible aperture
static const uint64_t kMcBase     = 0x8000000000ull;  // MMHUB FB_LOCATION_BASE << 24
static const uint64_t kVramBase   = 0x00800000ull;    // the bring-up region starts at 8 MiB
static const uint64_t kVramLimit  = kVramBytes;
static const uint32_t kMp0Base    = 0x008000;
static const uint32_t kMmhubBase  = 0x004000;

static const uint32_t R64 = kMp0Base + MP0Regs::C2PMSG_64;
static const uint32_t R67 = kMp0Base + MP0Regs::C2PMSG_67;
static const uint32_t R69 = kMp0Base + MP0Regs::C2PMSG_69;
static const uint32_t R70 = kMp0Base + MP0Regs::C2PMSG_70;
static const uint32_t R71 = kMp0Base + MP0Regs::C2PMSG_71;
static const uint32_t R81 = kMp0Base + MP0Regs::C2PMSG_81;
static const uint32_t RFB = kMmhubBase + MMHUBRegs::MMMC_VM_FB_LOCATION_BASE;

static std::vector<uint32_t> gReg;
static std::vector<uint8_t>  gVram;
static DeviceContext gDev;
static PSPContext    gPsp;

static uint32_t rd(uint32_t dwordIdx) { return gReg[dwordIdx]; }
static void     wr(uint32_t dwordIdx, uint32_t v) { gReg[dwordIdx] = v; }
static uint32_t vramU32(uint64_t off) {
    if (off + 4 > gVram.size()) return 0xDEADBEEFu;
    uint32_t v; memcpy(&v, gVram.data() + off, 4); return v;
}

// Fill a VRAM window with filth, then count what is left. The fake's aperture starts
// zeroed, so "the driver zeroed buffer X" is a claim about nothing unless something
// made it dirty first - and a driver that zeroed only part of it would still pass.
static void dirtyRange(uint64_t off, uint64_t len) {
    for (uint64_t i = 0; i < len && off + i < gVram.size(); i++) gVram[off + i] = 0xA5;
}
static uint64_t dirtyBytes(uint64_t off, uint64_t len) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < len && off + i < gVram.size(); i++) n += gVram[off + i] != 0;
    return n;
}

// What the fake PSP saw, per frame it accepted.
struct Seen {
    uint32_t slot;
    uint32_t frame[16];      // the 64-byte PSPGfxRBFrame, as the ring holds it
    uint32_t hdr[7];         // the 1 KB command buffer's header dwords
    uint32_t u[8];           // the command union at +28
};
static std::vector<Seen> gSeen;

static Seen gEmptySeen{};
// A break in the driver can leave the fake with nothing to read, and gSeen[i] would
// then be undefined behaviour - the plant script would report a crash instead of
// naming the check that failed. An out-of-range frame reads as zeroes and fails a
// check with a name.
static const Seen &frameAt(size_t i) {
    if (i < gSeen.size()) return gSeen[i];
    ok("the driver put a frame on the ring", false);
    return gEmptySeen;
}

struct Fake {
    bool     answering  = true;    // false: SOS is deaf — every fence poll times out
    uint32_t ringStatus = 0;       // low-16 status to answer INIT_GPCOM_RING with
    uint32_t tocStatus  = 0;       // resp.status for LOAD_TOC
    uint32_t setupStatus = 0;      // resp.status for SETUP_TMR
    uint32_t tmrSentinel = 0xABCD1234u;  // resp.tmr_size for LOAD_TOC (+880 only)
    uint32_t consumedWptr = 0;     // PSP's own read pointer
    uint32_t drops = 0;            // frames refused for a non-zero cmd_buf_size
    uint32_t unreadable = 0;       // frames whose ring/command/fence address SOS cannot use
    uint64_t ringMc = 0;           // latched from C2PMSG_69/70 at the kick
    uint32_t ringSize = 0;         // latched from C2PMSG_71
    bool     kicked = false;
    uint32_t kicks = 0;
};
static Fake F;

static bool mcToVram(uint64_t mc, uint64_t *out) {
    if (mc < kMcBase) return false;
    const uint64_t off = mc - kMcBase;
    if (off + 1024 > gVram.size()) return false;
    *out = off;
    return true;
}

// One step of the firmware: look at the mailbox and at the ring, answer what is new.
static void wr32_at(uint64_t off, uint32_t v) {
    if (off + 4 <= gVram.size()) memcpy(gVram.data() + off, &v, 4);
}

// The fake is the firmware, so every wire number below is the literal from
// upstream's psp_gfx_if.h / amdgpu_psp.c — never one of the kext's own constants.
// If the fake echoed kPSPRingTypeKM or kPSPGfxRespOffset, a plant on those headers
// would move both sides at once and the suite would pass on a broken protocol.
// Section L pins the kext's constants to these same numbers from the other side.
static const uint32_t kWakeKickKM     = 0x20000u;  // PSP_RING_TYPE__KM (2) << 16
static const uint32_t kWakeRespBit    = 0x80000000u;  // PSP_GFX_CMD__COMPLETE
static const uint32_t kWireFrameDw    = 16u;     // sizeof(psp_gfx_rb_frame) / 4
static const uint32_t kWireCmdUnion   = 28u;     // psp_gfx_cmd_resp.cmd
static const uint32_t kWireRespOff    = 864u;    // psp_gfx_cmd_resp.resp
static const uint32_t kWireRespTmrSize = 880u;   // + resp.tmr_size
static const uint32_t kWireCmdLoadToc = 0x20u;
static const uint32_t kWireCmdSetupTmr = 0x5u;

static void psp_service() {
    if (!F.answering) return;

    // 1. INIT_GPCOM_RING. The driver kicks by writing the ring type into C2PMSG_64;
    //    the ring's address and size were written just before it.
    if (rd(R64) == kWakeKickKM) {
        F.ringMc   = ((uint64_t)rd(R70) << 32) | rd(R69);
        F.ringSize = rd(R71);
        F.kicked = true; F.kicks++;
        wr(R64, kWakeRespBit | F.ringStatus);
    }

    // 2. GPCOM ring frames between the consumer pointer and the published wptr.
    for (;;) {
        const uint32_t w = rd(R67);
        if (w == F.consumedWptr) break;
        const uint32_t slot = F.consumedWptr / kWireFrameDw;
        Seen s{}; s.slot = slot;
        const uint64_t frameOff = (F.ringMc >= kMcBase ? F.ringMc - kMcBase : 0) +
                                  (uint64_t)slot * kWireFrameDw * 4;
        if (frameOff + sizeof(PSPGfxRBFrame) > gVram.size()) {
            ok("the frame SOS reads sits inside the aperture", false);
            F.unreadable++; F.consumedWptr = w; break;
        }
        for (int i = 0; i < 16; i++)
            memcpy(&s.frame[i], gVram.data() + frameOff + (size_t)i * 4, 4);

        // Upstream leaves cmd_buf_size at 0; a non-zero value makes PSP drop the frame.
        if (s.frame[2] != 0) { F.drops++; F.consumedWptr = w; continue; }

        const uint64_t cmdMc = ((uint64_t)s.frame[1] << 32) | s.frame[0];
        uint64_t cmdOff = 0;
        // A frame whose command buffer SOS cannot address is a protocol break, not
        // something to skip: silently continuing made the suite pass on a frame that
        // carried a VRAM offset where the firmware needs an MC address. Name it.
        if (!mcToVram(cmdMc, &cmdOff)) {
            ok("cmd_buf_addr is an MC address SOS can DMA from", false);
            F.unreadable++; F.consumedWptr = w; continue;
        }
        for (int i = 0; i < 7; i++) memcpy(&s.hdr[i], gVram.data() + cmdOff + (size_t)i * 4, 4);
        for (int i = 0; i < 8; i++) memcpy(&s.u[i], gVram.data() + cmdOff + kWireCmdUnion + (size_t)i * 4, 4);

        const uint32_t cmd_id = s.hdr[2];
        uint32_t status = 0;
        if (cmd_id == kWireCmdLoadToc) status = F.tocStatus;
        else if (cmd_id == kWireCmdSetupTmr) status = F.setupStatus;

        // The response goes back into the same buffer, at +864; LOAD_TOC's
        // tmr_size is the dword at +880 and nowhere else.
        wr32_at(cmdOff + kWireRespOff + 0, status);
        if (cmd_id == kWireCmdLoadToc) wr32_at(cmdOff + kWireRespTmrSize, F.tmrSentinel);

        // ...and the fence is the ack.
        const uint64_t fenceMc = ((uint64_t)s.frame[4] << 32) | s.frame[3];
        if (fenceMc >= kMcBase) {
            wr32_at(fenceMc - kMcBase, s.frame[5]);
        } else {
            ok("fence_addr is an MC address SOS can write the ack to", false);
            F.unreadable++;
        }
        gSeen.push_back(s);
        F.consumedWptr = w;
    }
}

extern "C" void dalshim_sleep(unsigned ms) {
    for (unsigned i = 0; i < ms; i++) psp_service();
}

// A fresh boot: BAR5 all-zero except the two things SOS publishes before any driver
// runs — C2PMSG_64 idle-ready and C2PMSG_81 the SOS sign of life.
static void machineReset(uint8_t mp0Maj = 14, uint8_t mp0Min = 0, uint8_t mp0Rev = 2) {
    gReg.assign(kRegDwords, 0);
    gVram.assign(kVramBytes, 0);
    gSeen.clear();
    F = Fake{};
    logClear();

    wr(R64, kPSPMboxRespFlag);          // "SOS is ready to take a ring"
    wr(R81, 0x0191ae05);                // sign of life seen on this card
    wr(RFB, (uint32_t)((kMcBase >> MMHUBRegs::kFBBaseShift) & MMHUBRegs::kFBBaseMask));

    gDev = DeviceContext{};
    gDev.rmmio = gReg.data(); gDev.rmmioSize = (size_t)kRegDwords * 4;
    gDev.bar0 = gVram.data(); gDev.bar0Size = (size_t)kVramBytes;
    gDev.vramSizeBytes = 16ull << 30;
    gDev.vramMcBase = kMcBase; gDev.vramBase = kVramBase; gDev.vramLimit = kVramLimit;
    gDev.ip.set(IPBlock::MP0, kMp0Base);
    gDev.ip.set(IPBlock::MMHUB, kMmhubBase);
    if (mp0Maj) gDev.ip.setVersion(IPBlock::MP0, IPVersion{mp0Maj, mp0Min, mp0Rev});

    gPsp = PSPContext{};
}

// The kext's own init + ring create, so `ringCreated` is earned rather than asserted.
static kern_return_t bootToRing(uint8_t maj = 14, uint8_t min = 0, uint8_t rev = 2) {
    machineReset(maj, min, rev);
    kern_return_t r = psp_init(gDev, gPsp);
    if (r != kIOReturnSuccess) return r;
    gPsp.sosAlive = true;              // psp_init clears it; SOS boot sets it
    return psp_ring_create(gDev, gPsp);
}

static bool parseDie(uint16_t gcMaj, uint8_t gcMin, uint8_t gcRev,
                     uint8_t mp0Maj, uint8_t mp0Min, uint8_t mp0Rev) {
    if (!fw_select_from_ip(IPVersion{(uint8_t)gcMaj, gcMin, gcRev},
                           IPVersion{mp0Maj, mp0Min, mp0Rev},
                           IPVersion{mp0Maj, mp0Min, mp0Rev})) return false;
    const FwBlob *sos = fw_get(FwId::PSP_SOS);
    if (!sos || !sos->data) return false;
    return psp_parse_sos_microcode(gPsp, sos->data, sos->size) == kIOReturnSuccess;
}

// =====================================================================================================================================
int main() {
    // ---------------------------------------------------------------- A
    section("A psp_init guards");
    {
        machineReset();
        gDev.ip.set(IPBlock::MP0, 0xFFFFFFFFu);
        eq("unresolved MP0 -> NotReady", psp_init(gDev, gPsp), kIOReturnNotReady);
        ok("and it says discovery is missing", hasLog("MP0 IP base not resolved"));
    }
    {
        machineReset();
        gDev.ip.set(IPBlock::MMHUB, 0u);   // IONewZero shape: 0 also means unresolved
        eq("unresolved MMHUB -> NotReady", psp_init(gDev, gPsp), kIOReturnNotReady);
    }
    {
        machineReset();
        gDev.vramMcBase = 0;
        eq("vramMcBase 0 -> NotReady", psp_init(gDev, gPsp), kIOReturnNotReady);
        ok("and it names the register", hasLog("vram_start is 0"));
    }
    {
        machineReset();
        gDev.vramLimit = kVramBase + 0x100000;   // a BAR0 too small to hold the PSP span
        eq("span overrun -> NoSpace", psp_init(gDev, gPsp), kIOReturnNoSpace);
        ok("and it refuses before writing anything", hasLog("overruns the bring-up region"));
    }
    {
        machineReset();
        eq("the real geometry inits", psp_init(gDev, gPsp), kIOReturnSuccess);
        // The layout the whole suite depends on: fw_pri, ring, cmd, fence, TMR, fw_buf.
        eq("fw_pri vram off", gPsp.fwPriVRAMOffset, kVramBase + 0x0);
        eq("ring vram off", gPsp.ringVRAMOffset, kVramBase + 0x100000);
        eq("cmd vram off", gPsp.cmdVRAMOffset, kVramBase + 0x104000);
        eq("fence vram off", gPsp.fenceVRAMOffset, kVramBase + 0x108000);
        eq("tmr vram off", gPsp.tmrVRAMOffset, kVramBase + 0x200000);
        eq("fw_buf vram off", gPsp.fwBufVRAMOffset, kVramBase + 0x1000000);
        eq("fw_pri size", gPsp.fwPriSize, 1024u * 1024u);
        eq("ring size advertised to PSP", gPsp.ringSize, 0x1000u);
        eq("fw_pri mc is vramMC(of it)", gPsp.fwPriBusAddr, kMcBase + gPsp.fwPriVRAMOffset);
        ok("init clears ringCreated", !gPsp.ringCreated);
        ok("init clears tmrSetUp", !gPsp.tmrSetUp);
        ok("init clears sosAlive", !gPsp.sosAlive);
        ok("no MMHUB disagreement warning at the real geometry",
           !hasLog("disagrees with dev.vramMcBase"));
    }

    // ---------------------------------------------------------------- B
    section("B psp_ring_create guards");
    {
        machineReset();
        ok("init", psp_init(gDev, gPsp) == kIOReturnSuccess);
        eq("SOS not alive -> NotReady", psp_ring_create(gDev, gPsp), kIOReturnNotReady);
        ok("and it says so", hasLog("ring_create: SOS not alive yet"));
        gPsp.fwPriSize = 0;
        eq("psp_init never called -> NotReady", psp_ring_create(gDev, gPsp), kIOReturnNotReady);
    }
    {
        machineReset();
        gPsp.sosAlive = true; gPsp.ringCreated = true;
        eq("already created -> immediate success", psp_ring_create(gDev, gPsp), kIOReturnSuccess);
        eq("no kick written", F.kicks, 0u);
    }
    {
        machineReset();
        gPsp.sosAlive = true;   // fwPriSize still 0: init not called
        F.answering = false;
        eq("deaf SOS -> NotReady before any wait", psp_ring_create(gDev, gPsp), kIOReturnNotReady);
    }

    // ---------------------------------------------------------------- C
    section("C the INIT_GPCOM_RING handshake");
    {
        // Boot by hand rather than with bootToRing(), because the three buffers have
        // to be dirty *before* ring_create runs for this section's zeroing checks to
        // mean anything. On real silicon VRAM holds whatever the previous session left;
        // a ring_create that zeroed only the first frame slot would pass on a clean fake.
        machineReset();
        ok("init", psp_init(gDev, gPsp) == kIOReturnSuccess);
        gPsp.sosAlive = true;
        dirtyRange(gPsp.ringVRAMOffset, kPSPKMRingBufSize);
        dirtyRange(gPsp.cmdVRAMOffset,  kPSPCmdBufSize);
        dirtyRange(gPsp.fenceVRAMOffset, kPSPFenceBufSize);
        // The TMR slot is the neighbour the zeroing spans could overrun into, so it
        // gets the opposite treatment: dirt that must still be there afterwards.
        dirtyRange(gPsp.tmrVRAMOffset, 4096);
        eq("the residue is in place first",
           dirtyBytes(gPsp.ringVRAMOffset, kPSPKMRingBufSize) +
           dirtyBytes(gPsp.cmdVRAMOffset,  kPSPCmdBufSize) +
           dirtyBytes(gPsp.fenceVRAMOffset, kPSPFenceBufSize),
           (uint64_t)(kPSPKMRingBufSize + kPSPCmdBufSize + kPSPFenceBufSize));
        logClear();
        eq("boot to a live ring", psp_ring_create(gDev, gPsp), kIOReturnSuccess);
        ok("ring_create reports the created ring", hasLog("ring_created mc=0x8000900000"));
        ok("SOS saw the KM kick", F.kicked);
        eq("exactly one kick", F.kicks, 1u);
        eq("the kick value was (PSP_RING_TYPE__KM<<16)", rd(R64), kPSPMboxRespFlag);
        eq("ring MC latched from C2PMSG_69/70", F.ringMc, kMcBase + kVramBase + 0x100000);
        eq("ring size latched from C2PMSG_71", F.ringSize, 0x1000u);
        eq("wptr reset to 0 after the ring exists", rd(R67), 0u);
        ok("ringCreated", gPsp.ringCreated);
        eq("ringBusAddr agrees with what SOS latched", gPsp.ringBusAddr, F.ringMc);
        eq("cmdBusAddr", gPsp.cmdBusAddr, kMcBase + gPsp.cmdVRAMOffset);
        eq("fenceBusAddr", gPsp.fenceBusAddr, kMcBase + gPsp.fenceVRAMOffset);
        eq("fenceCounter starts at 0", gPsp.fenceCounter, 0u);
        eq("the ring is zeroed for PSP to DMA",
           dirtyBytes(gPsp.ringVRAMOffset, kPSPKMRingBufSize), 0ull);
        eq("the command buffer is zeroed",
           dirtyBytes(gPsp.cmdVRAMOffset, kPSPCmdBufSize), 0ull);
        eq("the fence is zeroed",
           dirtyBytes(gPsp.fenceVRAMOffset, kPSPFenceBufSize), 0ull);
        eq("and the spans stop short of the TMR slot",
           dirtyBytes(gPsp.tmrVRAMOffset, 4096), 4096ull);
        eq("a second create is a no-op", psp_ring_create(gDev, gPsp), kIOReturnSuccess);
        eq("and does not re-kick SOS", F.kicks, 1u);
    }
    {
        machineReset();
        ok("init", psp_init(gDev, gPsp) == kIOReturnSuccess);
        gPsp.sosAlive = true;
        F.ringStatus = 0x000A;
        eq("SOS answers INIT_GPCOM_RING with an error -> IOError",
           psp_ring_create(gDev, gPsp), kIOReturnIOError);
        ok("and the ring is not marked created", !gPsp.ringCreated);
    }

    // ---------------------------------------------------------------- D
    section("D psp_setup_tmr guards");
    {
        machineReset();
        eq("no ring yet -> NotReady", psp_setup_tmr(gDev, gPsp), kIOReturnNotReady);
        eq("nothing was submitted", gSeen.size(), 0ull);
        ok("and it never claimed a version", !hasLog("setup_tmr: PSP IP v"));
    }
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        gPsp.tmrSetUp = true;
        logClear();
        eq("already set up -> success, silently", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("no second LOAD_TOC", gSeen.size(), 0ull);
        ok("no version line logged", !hasLog("setup_tmr:"));
    }
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        eq("still no TOC sub-bin -> success", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("no frame reached SOS", gSeen.size(), 0ull);
        ok("it says NO TOC sub-bin", hasLog("setup_tmr: NO TOC sub-bin"));
        ok("and still skips SETUP_TMR", hasLog("setup_tmr: SKIP SETUP_TMR"));
    }

    // ---------------------------------------------------------------- E
    section("E the version comes from discovery (patch 0009)");
    {
        // Navi 44 says MP0 14.0.2. Before 0009 these two lines said 14.0.3 on a card
        // that has never seen a 14.0.3 PSP — the lines a first boot is diagnosed from.
        ok("boot on a 14.0.2 die", bootToRing(14, 0, 2) == kIOReturnSuccess);
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        ok("names 14.0.2 as discovered", hasLog("setup_tmr: PSP IP v14.0.2 from discovery (pinned default v14.0.3)"));
        ok("and the skip line says psp_v14_0_2", hasLog("setup_tmr: SKIP SETUP_TMR (psp_v14_0_2"));
        eq("the version is stated exactly once", countLog("setup_tmr: PSP IP v"), 1);
        eq("the skip is stated exactly once", countLog("SKIP SETUP_TMR"), 1);
        ok("never mentions 14_0_3 as the running version",
           !hasLog("SKIP SETUP_TMR (psp_v14_0_3"));
    }
    {
        ok("boot on a 14.0.3 die", bootToRing(14, 0, 3) == kIOReturnSuccess);
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        ok("names 14.0.3 as discovered", hasLog("setup_tmr: PSP IP v14.0.3 from discovery (pinned default v14.0.3)"));
        ok("and the skip line says psp_v14_0_3", hasLog("setup_tmr: SKIP SETUP_TMR (psp_v14_0_3"));
    }
    {
        // Discovery never filled MP0's version: the pin is the only answer, and the log
        // has to say so, or a reader assumes the die reported it.
        ok("boot with no MP0 version", bootToRing(0, 0, 0) == kIOReturnSuccess);
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        ok("falls back to the pin", hasLog("setup_tmr: PSP IP v14.0.3 from pin (pinned default v14.0.3)"));
        ok("and the skip line says psp_v14_0_3", hasLog("setup_tmr: SKIP SETUP_TMR (psp_v14_0_3"));
    }

    // ---------------------------------------------------------------- F
    section("F v14 takes the TMR from SOS and never sends SETUP_TMR");
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        ok("tmrSetUp latched", gPsp.tmrSetUp);
        eq("tmrBusAddr cleared (SOS owns it)", gPsp.tmrBusAddr, 0ull);
        eq("tmrSize cleared", gPsp.tmrSize, 0ull);
        eq("no command of any kind reached SOS", gSeen.size(), 0ull);
        ok("the skip is explicit", hasLog("SKIP SETUP_TMR"));
        ok("and names the reserved slot it left alone", hasLog("the reserved TMR slot at vram+0xa00000 stays unused"));
        // psp_init pre-fills tmrBusAddr/tmrSize as if SETUP_TMR were coming; a v14
        // success must undo both, or LOAD_IP_FW trusts a TMR that does not exist.
        ok("the slot address was there before the skip", gPsp.tmrVRAMOffset == kVramBase + 0x200000);
    }
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        logClear();
        eq("second call still succeeds", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("and submits nothing new", gSeen.size(), 0ull);
    }

    // ---------------------------------------------------------------- G
    section("G LOAD_TOC on the real Navi 44 container");
    {
        ok("boot on a 14.0.2 die", bootToRing(14, 0, 2) == kIOReturnSuccess);
        ok("discovery names Navi 44", fw_select_from_ip(IPVersion{12, 0, 0}, IPVersion{14, 0, 2}, IPVersion{14, 0, 2}));
        const FwBlob *sos = fw_get(FwId::PSP_SOS);
        ok("and its SOS blob is embedded", sos && sos->data && sos->size > 0);
        eq("blob is psp_14_0_2_sos", std::string(sos->name) == "psp_14_0_2_sos", 1u);
        eq("the kext's own parser accepts it",
           psp_parse_sos_microcode(gPsp, sos->data, sos->size), kIOReturnSuccess);
        // The inventory patch 0006 measured: a 2304-byte TOC inside psp_14_0_2_sos.
        eq("TOC sub-bin size", gPsp.toc.size_bytes, 2304ull);
        ok("TOC points inside the container",
           gPsp.toc.start_addr >= sos->data &&
           gPsp.toc.start_addr + gPsp.toc.size_bytes <= sos->data + sos->size);
        eq("Navi 44 ships no RAS image", gPsp.ras_drv.size_bytes, 0ull);

        const uint64_t toc0 = (uint64_t)gPsp.toc.start_addr[0] |
                              ((uint64_t)gPsp.toc.start_addr[1] << 8);
        // Dirty fw_pri before the submit. Upstream's psp_copy_fw wipes the whole
        // 1 MB and then copies the payload at offset 0; on a freshly reset machine
        // there is nothing to wipe, so that step would be unobservable. A previous
        // LOAD_IP_FW pass leaves exactly this kind of residue on a real card.
        for (uint64_t i = 0; i < gPsp.fwPriSize; i += 4)
            wr32_at(gPsp.fwPriVRAMOffset + i, 0xA5A5A5A5u);
        eq("the residue is in place first",
           vramU32(gPsp.fwPriVRAMOffset + gPsp.fwPriSize - 4), 0xA5A5A5A5ull);
        logClear();
        eq("setup_tmr submits it", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("exactly one frame", gSeen.size(), 1ull);
        const Seen &s = frameAt(0);
        eq("and it is LOAD_TOC", s.hdr[2], (uint64_t)PSPGfxCmd::LOAD_TOC);
        eq("toc_size on the wire", s.u[2], 2304u);
        eq("toc_phy_addr_lo is fw_pri's MC", s.u[0], gPsp.fwPriBusAddr & 0xFFFFFFFFull);
        eq("toc_phy_addr_hi is fw_pri's MC", s.u[1], gPsp.fwPriBusAddr >> 32);
        eq("buf_version left 0 (upstream)", s.hdr[1], 0u);
        eq("buf_size left 0 (upstream)", s.hdr[0], 0u);
        // tmr_size must come from resp+880. cmd_buf+16 (what the reference read) is
        // resp_buf_addr_hi, which the driver always writes as 0 — so a nonzero,
        // planted, distinctive value proves the offset.
        ok("the reported tmr_size is the planted resp.tmr_size",
           hasLog("PSP reported tmr_size=2882343476 (0xabcd1234)"));
        ok("and the reference's offset would have read 0",
           hasLog("resp.tmr_size@880=2882343476 (0xabcd1234); "
                  "reference's cmd_buf+16 dword=0"));
        ok("the skip line is still there", hasLog("setup_tmr: SKIP SETUP_TMR (psp_v14_0_2"));
        // Staging: the whole 1 MB fw_pri is zeroed, then the payload lands at +0.
        uint64_t bad = 0;
        for (uint64_t i = 0; i < gPsp.toc.size_bytes; i++)
            if (gVram[gPsp.fwPriVRAMOffset + i] != gPsp.toc.start_addr[i]) bad++;
        eq("fw_pri holds the TOC payload byte for byte", bad, 0ull);
        uint64_t tail = 0;
        for (uint64_t i = gPsp.toc.size_bytes; i < 4096; i++)
            tail += gVram[gPsp.fwPriVRAMOffset + i];
        eq("and everything after it is still zero", tail, 0ull);
        uint64_t far = 0;
        for (uint64_t i = 64 * 1024; i < gPsp.fwPriSize; i++) far += gVram[gPsp.fwPriVRAMOffset + i];
        eq("the zeroing covered all of fw_pri", far, 0ull);
        ok("the payload's own first bytes are the TOC header", toc0 != 0);
    }

    // ---------------------------------------------------------------- H
    section("H LOAD_TOC on the real Navi 48 container");
    {
        ok("boot on a 14.0.3 die", bootToRing(14, 0, 3) == kIOReturnSuccess);
        ok("discovery names Navi 48", fw_select_from_ip(IPVersion{12, 0, 1}, IPVersion{14, 0, 3}, IPVersion{14, 0, 3}));
        const FwBlob *sos = fw_get(FwId::PSP_SOS);
        eq("blob is psp_14_0_3_sos", std::string(sos->name) == "psp_14_0_3_sos", 1u);
        ok("parse", psp_parse_sos_microcode(gPsp, sos->data, sos->size) == kIOReturnSuccess);
        eq("Navi 48's TOC is the same 2304 bytes", gPsp.toc.size_bytes, 2304ull);
        eq("and it does ship a RAS image", gPsp.ras_drv.size_bytes, 12704ull);
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("one frame", gSeen.size(), 1ull);
        eq("toc_size", frameAt(0).u[2], 2304u);
        ok("named as 14.0.3", hasLog("SKIP SETUP_TMR (psp_v14_0_3"));
        // The 0009 correction path end to end: a Navi 44 triple re-selects and the
        // next submit is the other die's TOC.
        ok("re-select to Navi 44", fw_select_from_ip(IPVersion{12, 0, 0}, IPVersion{14, 0, 2}, IPVersion{14, 0, 2}));
        eq("set name", std::string(fw_set_name()) == "navi44", 1u);
        ok("parse the other container",
           psp_parse_sos_microcode(gPsp, fw_get(FwId::PSP_SOS)->data,
                                   fw_get(FwId::PSP_SOS)->size) == kIOReturnSuccess);
        const uint32_t navi44TocSize = (uint32_t)gPsp.toc.size_bytes;
        machineReset(14, 0, 2);
        ok("boot again", psp_init(gDev, gPsp) == kIOReturnSuccess);
        gPsp.sosAlive = true;
        ok("ring", psp_ring_create(gDev, gPsp) == kIOReturnSuccess);
        ok("parse", psp_parse_sos_microcode(gPsp, fw_get(FwId::PSP_SOS)->data,
                                            fw_get(FwId::PSP_SOS)->size) == kIOReturnSuccess);
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("the corrected die's toc_size", gSeen.size() == 1 ? frameAt(0).u[2] : 0u,
           (uint64_t)navi44TocSize);
    }

    // ---------------------------------------------------------------- I
    section("I a container with no TOC sub-bin");
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        gPsp.toc.size_bytes = 2304;          // a size with no bytes behind it
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("no submit: start_addr is null", gSeen.size(), 0ull);
        ok("says which half is missing", hasLog("NO TOC sub-bin in SOS package"));
        ok("tmrSetUp still latched", gPsp.tmrSetUp);
    }
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        uint8_t dummy[16] = {0};
        gPsp.toc.start_addr = dummy;         // bytes with no size behind them
        gPsp.toc.size_bytes = 0;
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("no submit: size is 0", gSeen.size(), 0ull);
        ok("and it prints the size it saw", hasLog("size=0"));
        ok("tmrSetUp still latched", gPsp.tmrSetUp);
    }

    // ---------------------------------------------------------------- J
    section("J a failed LOAD_TOC does not latch the TMR");
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        ok("parse Navi 44", parseDie(12, 0, 0, 14, 0, 2));
        F.tocStatus = 0x0011;                // PSP: signature/size mismatch
        logClear();
        eq("setup_tmr propagates the failure", psp_setup_tmr(gDev, gPsp), kIOReturnError);
        ok("tmrSetUp was NOT latched", !gPsp.tmrSetUp);
        ok("LOAD_IP_FW would now refuse", psp_load_ip_fw(gDev, gPsp, gPsp.fwPriBusAddr, 64, 0) == kIOReturnNotReady);
        ok("the log carries kr and the PSP status",
           hasLog("setup_tmr: LOAD_TOC sub-bin FAILED kr=0xe00002bc resp=0x11"));
        ok("and says what breaks next", hasLog("TMR-resident LOAD_IP_FW will likely reject"));
        eq("the frame still went out", gSeen.size(), 1ull);
        eq("toc_size is what was refused", frameAt(0).u[2], 2304u);
    }
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        ok("parse Navi 44", parseDie(12, 0, 0, 14, 0, 2));
        F.answering = false;                 // SOS never touches the fence
        eq("fence timeout propagates", psp_setup_tmr(gDev, gPsp), kIOReturnTimeout);
        ok("tmrSetUp was NOT latched", !gPsp.tmrSetUp);
        ok("the timeout log names C2PMSG_64 and _81", hasLog("ring_cmd_submit: fence timeout"));
        ok("and the skip line never printed", !hasLog("SKIP SETUP_TMR"));
    }
    {
        // The frame the kext builds must keep cmd_buf_size at 0, because a non-zero
        // value makes PSP drop it silently (upstream amdgpu_psp.c:3485-3492). The fake
        // enforces that rule, so a planted "set cmd_buf_size" break shows up here as
        // the timeout it would be on real hardware — see psp_tmr_plant.sh.
        ok("boot", bootToRing() == kIOReturnSuccess);
        ok("parse Navi 44", parseDie(12, 0, 0, 14, 0, 2));
        eq("a clean frame still lands", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("no frames were dropped", F.drops, 0u);
        eq("cmd_buf_size on the wire is 0", frameAt(0).frame[2], 0u);
    }

    // ---------------------------------------------------------------- K
    section("K psp_load_toc_subbin argument guards");
    {
        machineReset();
        ok("init", psp_init(gDev, gPsp) == kIOReturnSuccess);
        uint8_t buf[64] = {0};
        eq("ring not created -> NotReady",
           psp_load_toc_subbin(gDev, gPsp, buf, 64, nullptr, nullptr), kIOReturnNotReady);
        ok("and it says the ring is missing", hasLog("load_toc_subbin: ring not created"));
    }
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        eq("null sub-bin -> BadArgument",
           psp_load_toc_subbin(gDev, gPsp, nullptr, 64, nullptr, nullptr), kIOReturnBadArgument);
        uint8_t buf[64] = {0};
        eq("zero size -> BadArgument",
           psp_load_toc_subbin(gDev, gPsp, buf, 0, nullptr, nullptr), kIOReturnBadArgument);
        ok("and it logs both halves", hasLog("load_toc_subbin: empty (subBin=0"));
        std::vector<uint8_t> big((size_t)gPsp.fwPriSize + 4, 0);
        eq("bigger than fw_pri -> NoSpace",
           psp_load_toc_subbin(gDev, gPsp, big.data(), (uint32_t)big.size(), nullptr, nullptr),
           kIOReturnNoSpace);
        ok("and says how much space it needed",
           hasLog("load_toc_subbin: payload 1048580 B > fw_pri 1048576 B"));
        eq("nothing was submitted", gSeen.size(), 0ull);
        // The out-parameters are optional; the accepted path must still fill them.
        uint32_t tmr = 0, resp = 0xDEAD;
        eq("a real submit succeeds",
           psp_load_toc_subbin(gDev, gPsp, buf, sizeof(buf), &tmr, &resp), kIOReturnSuccess);
        eq("outTmrSize from resp.tmr_size", tmr, F.tmrSentinel);
        eq("outRespStatus from resp.status", resp, 0u);
        eq("fence counter advanced", gPsp.fenceCounter, 1u);
        eq("second submit uses fence_value 2",
           psp_load_toc_subbin(gDev, gPsp, buf, sizeof(buf), &tmr, &resp), kIOReturnSuccess);
        eq("fence counter", gPsp.fenceCounter, 2u);
        eq("frame 2 fence_value", frameAt(1).frame[5], 2u);
        eq("frame 1 slot", frameAt(0).slot, 0u);
        eq("frame 2 slot", frameAt(1).slot, 1u);
    }

    // ---------------------------------------------------------------- L
    section("L the wire format SOS parses");
    {
        ok("boot", bootToRing() == kIOReturnSuccess);
        ok("parse Navi 44", parseDie(12, 0, 0, 14, 0, 2));
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("one frame", gSeen.size(), 1ull);
        const Seen &s = frameAt(0);
        eq("frame[0] cmd_buf_addr_lo", s.frame[0], gPsp.cmdBusAddr & 0xFFFFFFFFull);
        eq("frame[1] cmd_buf_addr_hi", s.frame[1], gPsp.cmdBusAddr >> 32);
        // ...and the pair composed against the layout literals. FB_LOCATION_BASE's MC
        // base has no low bits at all, so the frame's low half *is* the VRAM offset and
        // a per-half comparison against gPsp's own fields says only that the copy was
        // faithful. The composed number is what pins the base: it is the one part of
        // the address a plant can lose (cmd_buf_addr_hi / fence_addr_hi = 0) and the
        // part this suite would otherwise never look at.
        eq("cmd_buf_addr is the full MC address of the cmd window",
           (uint64_t)s.frame[0] | ((uint64_t)s.frame[1] << 32),
           kMcBase + kVramBase + 0x104000ull);
        eq("fence_addr is the full MC address of the fence window",
           (uint64_t)s.frame[3] | ((uint64_t)s.frame[4] << 32),
           kMcBase + kVramBase + 0x108000ull);
        eq("frame[2] cmd_buf_size", s.frame[2], 0u);
        eq("frame[3] fence_addr_lo", s.frame[3], gPsp.fenceBusAddr & 0xFFFFFFFFull);
        eq("frame[4] fence_addr_hi", s.frame[4], gPsp.fenceBusAddr >> 32);
        eq("frame[5] fence_value", s.frame[5], 1u);
        for (int i = 6; i < 16; i++) eq("every other frame dword is zero", (uint64_t)s.frame[i], 0ull);
        eq("wptr advanced by one frame", rd(R67), 16u);
        eq("the fence in VRAM holds the ack", vramU32(gPsp.fenceVRAMOffset), 1u);
        eq("resp.status was read as 0", vramU32(gPsp.cmdVRAMOffset + kPSPGfxRespStatusOffset), 0u);
        eq("resp.tmr_size is the planted dword", vramU32(gPsp.cmdVRAMOffset + kPSPGfxRespTmrSizeOffset), F.tmrSentinel);
        // The kext's constants must equal the numbers this fake speaks as the
        // firmware. Section L's other checks compare the driver against gSeen,
        // which is written with the literals above — so without these pins a plant
        // on a header constant would move both sides and look like a pass.
        eq("PSP_RING_TYPE__KM << 16 is 0x20000", (uint64_t)(kPSPRingTypeKM << 16), kWakeKickKM);
        eq("the ring-complete bit is bit 31", kPSPMboxRespFlag, kWakeRespBit);
        eq("the mailbox poll mask is bit31 | the low 16", kPSPMboxRespMask, 0x8000FFFFull);
        eq("the cmd union starts at +28", kPSPGfxCmdUnionOffset, kWireCmdUnion);
        eq("resp starts at +864", kPSPGfxRespOffset, kWireRespOff);
        eq("resp.tmr_size starts at +880", kPSPGfxRespTmrSizeOffset, kWireRespTmrSize);
        eq("LOAD_TOC is command id 0x20", PSPGfxCmd::LOAD_TOC, kWireCmdLoadToc);
        eq("SETUP_TMR is command id 0x5", PSPGfxCmd::SETUP_TMR, kWireCmdSetupTmr);
        eq("a ring frame is 16 dwords", sizeof(PSPGfxRBFrame) / 4, kWireFrameDw);
        eq("the 4 KB ring is 1024 dwords", kPSPKMRingSize / 4u, 1024u);
        eq("PSP_RING_TYPE__UM is the RBI ring, not ours", kPSPRingTypeUM, 1u);
        ok("SOS was not asked to drop anything", F.drops == 0);
        // The command buffer the fake read is the one the driver wrote: same MC the
        // ring frame names, and no leftover from the ring_create zeroing.
        ok("the log records the staging address",
           hasLog("LOAD_TOC (sub-bin): size=2304 staged at vram+0x800000 mc=0x8000800000"));
    }

    // ---------------------------------------------------------------- M
    section("M the legacy branch: a non-v14 PSP does get SETUP_TMR");
    {
        // Unreachable on this silicon, kept for fidelity — which is exactly why it
        // needs a test: nothing else in the tree can tell whether it still works.
        ok("boot with MP0 13.0.0", bootToRing(13, 0, 0) == kIOReturnSuccess);
        gPsp.toc.start_addr = nullptr;       // keep the branch's own submit in focus
        eq("setup_tmr", psp_setup_tmr(gDev, gPsp), kIOReturnSuccess);
        eq("one frame", gSeen.size(), 1ull);
        const Seen &s = frameAt(0);
        eq("and it is SETUP_TMR", s.hdr[2], (uint64_t)PSPGfxCmd::SETUP_TMR);
        eq("buf_phy_addr_lo = vramMC(tmrVRAMOffset)", s.u[0],
           (kMcBase + gPsp.tmrVRAMOffset) & 0xFFFFFFFFull);
        eq("buf_phy_addr_hi", s.u[1], (kMcBase + gPsp.tmrVRAMOffset) >> 32);
        eq("buf_size = the 4 MB default", s.u[2], (uint64_t)kPSPTMRDefaultSize);
        eq("tmr_flags = 0x2 (virt_phy_addr)", s.u[3], 2u);
        eq("system_phy_addr_lo mirrors buf", s.u[4], s.u[0]);
        eq("system_phy_addr_hi mirrors buf", s.u[5], s.u[1]);
        eq("tmrBusAddr programmed", gPsp.tmrBusAddr, kMcBase + gPsp.tmrVRAMOffset);
        eq("tmrSize programmed", gPsp.tmrSize, kPSPTMRDefaultSize);
        ok("tmrSetUp latched", gPsp.tmrSetUp);
        ok("and it says so", hasLog("SETUP_TMR ok — TMR at mc=0x8000a00000"));
        ok("no skip line on this path", !hasLog("SKIP SETUP_TMR"));
    }
    {
        // The legacy branch's own guard: with no MC base there is no TMR to advertise,
        // and the driver must refuse rather than submit a frame pointing at address 0.
        ok("boot with MP0 13.0.0", bootToRing(13, 0, 0) == kIOReturnSuccess);
        gDev.vramMcBase = 0;                 // MMHUB's FB_LOCATION_BASE went away
        eq("vram_start 0 -> NotReady", psp_setup_tmr(gDev, gPsp), kIOReturnNotReady);
        ok("and it blames MMHUB", hasLog("setup_tmr: vram_start=0; MMHUB not ready"));
        ok("tmrSetUp not latched", !gPsp.tmrSetUp);
        eq("nothing submitted", gSeen.size(), 0ull);
    }

    // ---------------------------------------------------------------- N
    // Patch 0010 adds the kIOReturn codes this suite compares against to
    // tests/dalshim. A host shim is only honest if its error values equal the
    // kernel's, and every one below is read off
    // MacKernelSDK/Headers/IOKit/IOReturn.h: sys_iokit (0xe0000000) |
    // sub_iokit_common (err_sub(0) = 0) | the code.
    section("N the shim's IOReturn values are the kernel's");
    {
        // kern_return_t is a signed int, so 0xe00002bc arrives here negative: the
        // casts are what make these bit-for-bit comparisons of the wire value.
        eq("kIOReturnSuccess is KERN_SUCCESS", (uint64_t)(uint32_t)kIOReturnSuccess, 0ull);
        eq("kIOReturnError is iokit_common_err(0x2bc)", (uint64_t)(uint32_t)kIOReturnError, 0xE00002BCull);
        eq("kIOReturnBadArgument is 0x2c2", (uint64_t)(uint32_t)kIOReturnBadArgument, 0xE00002C2ull);
        eq("kIOReturnInternalError is 0x2c9", (uint64_t)(uint32_t)kIOReturnInternalError, 0xE00002C9ull);
        eq("kIOReturnIOError is 0x2ca", (uint64_t)(uint32_t)kIOReturnIOError, 0xE00002CAull);
        eq("kIOReturnNotAligned is 0x2d0", (uint64_t)(uint32_t)kIOReturnNotAligned, 0xE00002D0ull);
        eq("kIOReturnTimeout is 0x2d6", (uint64_t)(uint32_t)kIOReturnTimeout, 0xE00002D6ull);
        eq("kIOReturnNotReady is 0x2d8", (uint64_t)(uint32_t)kIOReturnNotReady, 0xE00002D8ull);
        eq("kIOReturnNoSpace is 0x2db", (uint64_t)(uint32_t)kIOReturnNoSpace, 0xE00002DBull);
        eq("kIOReturnNotFound is 0x2f0", (uint64_t)(uint32_t)kIOReturnNotFound, 0xE00002F0ull);
        eq("a PSP failure is never mistaken for success",
           (uint64_t)(kIOReturnTimeout != kIOReturnSuccess), 1ull);
    }

    // ---------------------------------------------------------------- O
    section("O psp_load_toc: a whole .bin file, only its payload on the wire");
    {
        // Nothing in the kext calls psp_load_toc today - the boot path takes the TOC
        // out of the SOS package with the sub-bin parser - so without this section its
        // header guards are dead code nobody has ever run. It shares psp_ring_cmd_submit
        // and the LOAD_TOC union with the sub-bin path, and its toc_phy_addr line is
        // character-for-character identical, which is also why the plant script has to
        // aim at one of the two copies rather than one unique line.
        machineReset();
        ok("init", psp_init(gDev, gPsp) == kIOReturnSuccess);
        eq("ring not created -> NotReady",
           psp_load_toc(gDev, gPsp, nullptr, 0, nullptr, nullptr), kIOReturnNotReady);
        ok("and it says the ring is missing", hasLog("load_toc: ring not created"));

        ok("boot", bootToRing() == kIOReturnSuccess);
        std::vector<uint8_t> file(320, 0);
        auto *h = reinterpret_cast<common_firmware_header *>(file.data());
        h->size_bytes = (uint32_t)file.size();
        h->header_size_bytes = sizeof(common_firmware_header);
        h->ucode_array_offset_bytes = 64;
        h->ucode_size_bytes = 256;
        for (uint32_t i = 0; i < h->ucode_size_bytes; i++)
            file[h->ucode_array_offset_bytes + i] = (uint8_t)(i ^ 0x5Au);

        eq("no file -> BadArgument",
           psp_load_toc(gDev, gPsp, nullptr, 320, nullptr, nullptr), kIOReturnBadArgument);
        eq("a file shorter than the header -> BadArgument",
           psp_load_toc(gDev, gPsp, file.data(), 31, nullptr, nullptr), kIOReturnBadArgument);
        eq("nothing was submitted by a refused call", gSeen.size(), 0ull);
        std::vector<uint8_t> noOff = file;
        reinterpret_cast<common_firmware_header *>(noOff.data())->ucode_array_offset_bytes = 0;
        eq("payload offset 0 -> BadArgument",
           psp_load_toc(gDev, gPsp, noOff.data(), (uint32_t)noOff.size(), nullptr, nullptr),
           kIOReturnBadArgument);
        ok("and it prints the three numbers it compared",
           hasLog("load_toc: header bad — off=0 size=256 file=320"));
        std::vector<uint8_t> overrun = file;
        reinterpret_cast<common_firmware_header *>(overrun.data())->ucode_size_bytes = 257;
        eq("payload running past the file -> BadArgument",
           psp_load_toc(gDev, gPsp, overrun.data(), (uint32_t)overrun.size(), nullptr, nullptr),
           kIOReturnBadArgument);
        std::vector<uint8_t> huge((size_t)gPsp.fwPriSize + 8192, 0);
        auto *hh = reinterpret_cast<common_firmware_header *>(huge.data());
        hh->ucode_array_offset_bytes = 4096;
        hh->ucode_size_bytes = gPsp.fwPriSize + 4096;
        eq("payload bigger than fw_pri -> NoSpace",
           psp_load_toc(gDev, gPsp, huge.data(), (uint32_t)huge.size(), nullptr, nullptr),
           kIOReturnNoSpace);
        ok("and it says how big it was", hasLog("load_toc: payload 1052672 B > fw_pri 1048576 B"));
        eq("still nothing submitted", gSeen.size(), 0ull);

        // The accepted call: staging plus one LOAD_TOC frame naming fw_pri.
        dirtyRange(gPsp.fwPriVRAMOffset, gPsp.fwPriSize);
        logClear();
        uint32_t tmr = 0, resp = 0xDEAD;
        eq("a real file loads",
           psp_load_toc(gDev, gPsp, file.data(), (uint32_t)file.size(), &tmr, &resp),
           kIOReturnSuccess);
        eq("outTmrSize from resp.tmr_size", tmr, F.tmrSentinel);
        eq("outRespStatus from resp.status", resp, 0u);
        eq("one frame", gSeen.size(), 1ull);
        const Seen &s = frameAt(0);
        eq("and it is LOAD_TOC", s.hdr[2], (uint64_t)PSPGfxCmd::LOAD_TOC);
        eq("toc_size is the payload, not the file", s.u[2], 256u);
        eq("toc_phy_addr is fw_pri's MC address",
           (uint64_t)s.u[0] | ((uint64_t)s.u[1] << 32), kMcBase + kVramBase + 0x0ull);
        eq("frame cmd_buf_size stays 0", s.frame[2], 0u);
        eq("wptr advanced by one frame", rd(R67), 16u);
        ok("the log names the payload it staged",
           hasLog("LOAD_TOC: file=320 payload off=64 size=256 staged at vram+0x800000 "
                  "mc=0x8000800000"));
        uint64_t bad = 0;
        for (uint32_t i = 0; i < h->ucode_size_bytes; i++)
            if (gVram[gPsp.fwPriVRAMOffset + i] != file[h->ucode_array_offset_bytes + i]) bad++;
        eq("fw_pri holds the payload the header points at", bad, 0ull);
        eq("the rest of the 1 MB was wiped first",
           dirtyBytes(gPsp.fwPriVRAMOffset + h->ucode_size_bytes,
                      gPsp.fwPriSize - h->ucode_size_bytes), 0ull);
    }

    section("");
    std::printf("%d check(s) failed out of %d\n", gFail, gRun);
    return gFail ? 1 : 0;
}
