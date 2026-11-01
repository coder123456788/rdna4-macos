// navi44_die_test.cpp — host-side checks for the two Navi 44 deltas that do
// not need a GPU: the per-die firmware set (fw_table, patch 0002) and the
// per-die geometry fallback + GB_ADDR_CONFIG override (gfx_v12_0, patch 0003).
//
//     zig c++ -std=c++17 -I hosttest/shim -I src/amd -I src/fw \
//         -o navi44_die_test tests/navi44_die_test.cpp src/amd/fw_table.cpp \
//         src/amd/gfx_v12_0.cpp <fw objects compiled with zig cc -std=c11>
//     ./navi44_die_test [firmware dir]        # from src/navi48-bringup
//
// The firmware cases compare the embedded blobs against firmware/*.bin
// byte-for-byte, so they also catch an embed step that silently picked up
// the wrong file. The geometry cases drive the real public entry points:
// gfx_gpu_early_init() on a synthetic IP table, and gfx_get_gb_addr_config()
// against a scratch MMIO window standing in for BAR5. The topology cases run
// the whole gfx_constants_init() sequence and pin the record the kext appends
// to QueryInfo (ABI 1.13) against the Linux ioctl it reproduces.
#include "fw_table.h"
#include "amdgpu_gfx.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace amdgpu {
// The kernel log ring lives in n48log.cpp, which is not part of a host
// build. Everything else in this test is the real implementation.
void n48_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
} // namespace amdgpu

using namespace amdgpu;

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
    if (sz < 0) { std::fclose(f); return false; }
    out.resize((size_t)sz);
    const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

uint64_t fileSize(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return ~0ull;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fclose(f);
    return sz < 0 ? ~0ull : (uint64_t)sz;
}

// ---------------------------------------------------------------- firmware

struct Role {
    FwId id;
    const char *navi44;
    const char *navi48;
};

const Role kRoles[] = {
    {FwId::PSP_SOS,    "psp_14_0_2_sos",    "psp_14_0_3_sos"},
    {FwId::PSP_TA,     "psp_14_0_2_ta",     "psp_14_0_3_ta"},
    {FwId::GC_IMU,     "gc_12_0_0_imu",     "gc_12_0_1_imu"},
    {FwId::GC_PFP,     "gc_12_0_0_pfp",     "gc_12_0_1_pfp"},
    {FwId::GC_ME,      "gc_12_0_0_me",      "gc_12_0_1_me"},
    {FwId::GC_MEC,     "gc_12_0_0_mec",     "gc_12_0_1_mec"},
    {FwId::GC_UNI_MES, "gc_12_0_0_uni_mes", "gc_12_0_1_uni_mes"},
    {FwId::GC_RLC,     "gc_12_0_0_rlc",     "gc_12_0_1_rlc"},
    {FwId::SMU,        "smu_14_0_2",        "smu_14_0_3"},
    {FwId::SDMA,       "sdma_7_0_0",        "sdma_7_0_1"},
};

void checkSet(const char *label, bool navi44, const std::string &fwDir)
{
    std::printf("\n-- firmware set: %s --\n", label);
    std::string pfx;
    pfx += "  [";
    pfx += label;
    pfx += "] ";

    eq((pfx + "fw_is_navi44").c_str(), fw_is_navi44() ? 1u : 0u, navi44 ? 1u : 0u);

    for (const Role &r : kRoles) {
        const FwBlob *b = fw_get(r.id);
        ok((pfx + "entry exists").c_str(), b != nullptr);
        if (!b) continue;
        const char *want = navi44 ? r.navi44 : r.navi48;
        eqs((pfx + "name " + want).c_str(), b->name ? b->name : "(null)", want);
        eq((pfx + "size " + want).c_str(), b->size,
           fileSize(fwDir + "/" + std::string(want) + ".bin"));
        ok((pfx + "data " + want).c_str(), b->data != nullptr);
    }

    // No embedded blob on either die: the split MES pair is superseded by
    // uni_mes, and TOC is only loaded for psp_14_0_5.
    for (FwId id : {FwId::GC_MES, FwId::GC_MES1, FwId::GC_TOC}) {
        const FwBlob *b = fw_get(id);
        ok((pfx + "absent row exists").c_str(), b != nullptr);
        if (!b) continue;
        ok((pfx + "absent row has no data").c_str(), b->data == nullptr && b->size == 0);
    }

    // The other die's names must not resolve in this set.
    for (const Role &r : kRoles) {
        const char *other = navi44 ? r.navi48 : r.navi44;
        ok((pfx + "no foreign name " + other).c_str(),
           fw_find_by_name(other) == nullptr);
        ok((pfx + "own name resolves " + (navi44 ? r.navi44 : r.navi48)).c_str(),
           fw_find_by_name(navi44 ? r.navi44 : r.navi48) != nullptr);
    }
}

void firmwareTests(const std::string &fwDir)
{
    std::printf("\n================ firmware set (0002) ================\n");
    std::printf("firmware dir: %s\n", fwDir.c_str());

    // The default table, before any selection, is the historical Navi 48 set.
    checkSet("default", false, fwDir);

    fw_select_for_device(0x7590);              // Navi 44 / RX 9060 XT
    checkSet("0x7590", true, fwDir);

    fw_select_for_device(0x7550);              // Navi 48 / RX 9070
    checkSet("0x7550", false, fwDir);

    fw_select_for_device(0x7590);              // switching back rebuilds
    checkSet("0x7590 again", true, fwDir);

    std::printf("\n-- fw_table edges --\n");
    fw_select_for_device(0x1234);              // unknown id: keep Navi 48
    ok("unknown id selects Navi 48", !fw_is_navi44());
    eqs("unknown id PSP_SOS", fw_get(FwId::PSP_SOS)->name, "psp_14_0_3_sos");

    ok("fw_get(Count) is null",
       fw_get(static_cast<FwId>(static_cast<uint32_t>(FwId::Count))) == nullptr);
    ok("fw_find_by_name(nullptr) is null", fw_find_by_name(nullptr) == nullptr);
    ok("fw_find_by_name(\"\") is null", fw_find_by_name("") == nullptr);
    ok("fw_find_by_name(prefix) is null", fw_find_by_name("psp_14_0_3_so") == nullptr);
    ok("fw_find_by_name(suffix) is null", fw_find_by_name("sp_14_0_3_sos") == nullptr);

    // Byte-identity of the embedded copy against the file it was generated
    // from, for both sets. This is what proves the .c embed step took the
    // right firmware, not merely a same-sized one.
    for (int set = 0; set < 2; set++) {
        fw_select_for_device(set == 0 ? 0x7550 : 0x7590);
        for (const Role &r : kRoles) {
            const FwBlob *b = fw_get(r.id);
            const char *name = set == 0 ? r.navi48 : r.navi44;
            std::vector<uint8_t> file;
            if (!b || !b->data || !readFile(fwDir + "/" + name + ".bin", file)) {
                ok((std::string("file readable: ") + name).c_str(), false);
                continue;
            }
            eq((std::string("bytes match file: ") + name).c_str(),
               (file.size() == b->size &&
                std::memcmp(file.data(), b->data, b->size) == 0) ? 1u : 0u, 1u);
        }
    }
}

// ---------------------------------------------------------------- geometry

constexpr uint32_t kGcBase = 0x8000;   // arbitrary but >0 dword index

struct Scratch {
    DeviceContext dev;
    std::vector<uint32_t> mmio;

    Scratch()
    {
        // Covers GC segment 0 (GB_ADDR_CONFIG, CC_* at 0x13dd) and segment 1
        // (GRBM_GFX_INDEX, the USER_* harvest masks at 0x5b9x), the two the
        // geometry and harvest paths read.
        mmio.assign(kGcBase + 0x9000, 0);
        dev.rmmio = mmio.data();
        dev.rmmioSize = mmio.size() * sizeof(uint32_t);
        dev.ip.set(IPBlock::GC, kGcBase);
        dev.ip.setBase(IPBlock::GC, 1, kGcBase + 0x2000);
    }
    void reg(uint32_t value)
    {
        mmio[kGcBase + GFXRegs::GB_ADDR_CONFIG] = value;
    }
    void gc0(uint32_t off, uint32_t value) { mmio[kGcBase + off] = value; }
    void gc1(uint32_t off, uint32_t value) { mmio[kGcBase + 0x2000 + off] = value; }
};

// Realistic Navi 44 values: NUM_PIPES=1 (2 tile pipes), NUM_SHADER_ENGINES=1
// (2 SEs), NUM_RB_PER_SE=1 (2 RBs/SE), NUM_PKRS=3 (8), PIPE_INTERLEAVE=1
// (512 B), MAX_COMPRESSED_FRAGS=1 (2).
static uint32_t packGb(uint32_t pipes, uint32_t ses, uint32_t rb, uint32_t pkrs,
                       uint32_t interleave, uint32_t frags)
{
    return (pipes << GB_ADDR_CONFIG__NUM_PIPES__SHIFT) |
           (interleave << GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE__SHIFT) |
           (frags << GB_ADDR_CONFIG__MAX_COMPRESSED_FRAGS__SHIFT) |
           (pkrs << GB_ADDR_CONFIG__NUM_PKRS__SHIFT) |
           (ses << GB_ADDR_CONFIG__NUM_SHADER_ENGINES__SHIFT) |
           (rb << GB_ADDR_CONFIG__NUM_RB_PER_SE__SHIFT);
}

void geometryTests()
{
    std::printf("\n================ die geometry (0003) ================\n");

    // ---- fallback caps, before GB_ADDR_CONFIG is readable ----
    {
        struct Case { IPVersion v; uint32_t se, sh, rb, cu, ctx; const char *label; };
        const Case cases[] = {
            {{12, 0, 0}, 2, 2, 2, 8, 8, "Navi 44 GC 12.0.0"},
            {{12, 0, 1}, 4, 2, 4, 8, 8, "Navi 48 GC 12.0.1"},
            {{12, 0, 5}, 4, 2, 4, 8, 8, "GC 12.0.5 (unnamed, Navi 48 caps)"},
            {{11, 0, 3}, 4, 2, 4, 8, 8, "GC 11.0.3 (not RDNA4, Navi 48 caps)"},
        };
        for (const Case &c : cases) {
            Scratch s;
            s.dev.ip.setVersion(IPBlock::GC, c.v);
            GFXConfig cfg{};
            const kern_return_t kr = gfx_gpu_early_init(s.dev, cfg);
            const std::string p = std::string("  [") + c.label + "] ";
            eq((p + "kIOReturn").c_str(), kr, kIOReturnSuccess);
            eq((p + "max_shader_engines").c_str(), cfg.max_shader_engines, c.se);
            eq((p + "max_sh_per_se").c_str(), cfg.max_sh_per_se, c.sh);
            eq((p + "max_backends_per_se").c_str(), cfg.max_backends_per_se, c.rb);
            eq((p + "max_cu_per_sh").c_str(), cfg.max_cu_per_sh, c.cu);
            eq((p + "max_hw_contexts").c_str(), cfg.max_hw_contexts, c.ctx);
            eq((p + "sc_prim_fifo_size_frontend").c_str(),
               cfg.sc_prim_fifo_size_frontend, 0x20);
            eq((p + "sc_prim_fifo_size_backend").c_str(),
               cfg.sc_prim_fifo_size_backend, 0x100);
            eq((p + "sc_hiz_tile_fifo_size").c_str(), cfg.sc_hiz_tile_fifo_size, 0);
            eq((p + "sc_earlyz_tile_fifo_size").c_str(),
               cfg.sc_earlyz_tile_fifo_size, 0x4C0);
            eq((p + "CUs = SE*SH*CU").c_str(),
               cfg.max_shader_engines * cfg.max_sh_per_se * cfg.max_cu_per_sh,
               c.se * c.sh * c.cu);
        }
    }

    // ---- GB_ADDR_CONFIG ----
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        GFXConfig cfg{};
        (void)gfx_gpu_early_init(s.dev, cfg);
        ok("unresolved GC base -> kIOReturnNotReady",
           gfx_get_gb_addr_config(s.dev, cfg) == kIOReturnNotReady);
    }
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        GFXConfig cfg{};
        (void)gfx_gpu_early_init(s.dev, cfg);
        const uint32_t before = cfg.max_shader_engines;
        s.reg(0);
        ok("GB_ADDR_CONFIG==0 -> kIOReturnNotReady",
           gfx_get_gb_addr_config(s.dev, cfg) == kIOReturnNotReady);
        eq("GB_ADDR_CONFIG==0 keeps caps", cfg.max_shader_engines, before);
        s.reg(0xFFFFFFFFu);
        ok("GB_ADDR_CONFIG==~0 -> kIOReturnNotReady",
           gfx_get_gb_addr_config(s.dev, cfg) == kIOReturnNotReady);
        eq("GB_ADDR_CONFIG==~0 keeps caps", cfg.max_shader_engines, before);
    }

    // Navi 44-shaped register on a Navi 44 fallback: agrees, and every
    // derived field decodes to the documented value.
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        GFXConfig cfg{};
        (void)gfx_gpu_early_init(s.dev, cfg);
        const uint32_t gb = packGb(1, 1, 1, 3, 1, 1);
        s.reg(gb);
        eq("navi44 gb: kIOReturn", gfx_get_gb_addr_config(s.dev, cfg), kIOReturnSuccess);
        eq("navi44 gb: raw", cfg.gb_addr_config_raw, gb);
        eq("navi44 gb: num_pipes", cfg.num_pipes, 2);
        eq("navi44 gb: num_se", cfg.num_shader_engines, 2);
        eq("navi44 gb: num_rb_per_se", cfg.num_rb_per_se, 2);
        eq("navi44 gb: num_pkrs", cfg.num_pkrs, 8);
        eq("navi44 gb: pipe_interleave", cfg.pipe_interleave_size, 512);
        eq("navi44 gb: max_compressed_frags", cfg.max_compressed_frags, 2);
        eq("navi44 gb: max_shader_engines <- NUM_SHADER_ENGINES",
           cfg.max_shader_engines, 2);
        eq("navi44 gb: max_backends_per_se <- NUM_RB_PER_SE",
           cfg.max_backends_per_se, 2);
    }

    // The register wins over a wrong fallback, and it is NUM_SHADER_ENGINES
    // (not NUM_PIPES) that sets the SE count. A Navi 48-shaped register on
    // the Navi 44 fallback is the case where the two fields differ from
    // each other too, so a pipe-based override cannot pass by accident:
    // NUM_PIPES=2 -> 4 tile pipes, NUM_SHADER_ENGINES=2 -> 4 SEs (equal),
    // then NUM_PIPES=3 -> 8 pipes, NUM_SHADER_ENGINES=1 -> 2 SEs (differ).
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        GFXConfig cfg{};
        (void)gfx_gpu_early_init(s.dev, cfg);
        s.reg(packGb(2, 2, 2, 3, 1, 1));
        eq("navi48-shaped gb: kIOReturn", gfx_get_gb_addr_config(s.dev, cfg),
           kIOReturnSuccess);
        eq("navi48-shaped gb: SEs from register", cfg.max_shader_engines, 4);
        eq("navi48-shaped gb: RBs/SE from register", cfg.max_backends_per_se, 4);
        eq("navi48-shaped gb: num_pipes", cfg.num_pipes, 4);
    }
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 1});
        GFXConfig cfg{};
        (void)gfx_gpu_early_init(s.dev, cfg);
        s.reg(packGb(3, 1, 3, 3, 1, 1));   // 8 tile pipes, 2 SEs, 8 RBs/SE
        eq("pipes!=SEs: kIOReturn", gfx_get_gb_addr_config(s.dev, cfg),
           kIOReturnSuccess);
        eq("pipes!=SEs: num_pipes decodes to 8", cfg.num_pipes, 8);
        eq("pipes!=SEs: SEs follow NUM_SHADER_ENGINES, not pipes",
           cfg.max_shader_engines, 2);
        eq("pipes!=SEs: RBs/SE follow NUM_RB_PER_SE", cfg.max_backends_per_se, 8);
    }

    // Implausible field values keep the fallback instead of zeroing the
    // masks. NUM_SHADER_ENGINES is 4 bits, so 0 is the only in-range value
    // that decodes below 1; 15 decodes to 32768.
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        GFXConfig cfg{};
        (void)gfx_gpu_early_init(s.dev, cfg);
        s.reg(packGb(1, 0, 1, 3, 1, 1));   // NM_SHADER_ENGINES=0 -> 1 SE
        eq("NUM_SHADER_ENGINES=0: kIOReturn", gfx_get_gb_addr_config(s.dev, cfg),
           kIOReturnSuccess);
        eq("NUM_SHADER_ENGINES=0 decodes to 1", cfg.num_shader_engines, 1);
        eq("NUM_SHADER_ENGINES=0 is in range, so it wins",
           cfg.max_shader_engines, 1);
    }
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        GFXConfig cfg{};
        (void)gfx_gpu_early_init(s.dev, cfg);
        s.reg(packGb(1, 15, 15, 3, 1, 1)); // 32768 SEs: out of range
        eq("out-of-range SEs: kIOReturn", gfx_get_gb_addr_config(s.dev, cfg),
           kIOReturnSuccess);
        eq("out-of-range SEs keep fallback", cfg.max_shader_engines, 2);
        // NUM_RB_PER_SE is only 2 bits wide, so 15 truncates to 3 -> 8: in
        // range, and the register wins.
        eq("RB field truncates to 3 -> 8 RBs/SE", cfg.num_rb_per_se, 8);
        eq("2-bit RB field stays in range, so it wins",
           cfg.max_backends_per_se, 8);
    }
}

// ------------------------------------------------------- harvest masks (setup_rb)
//
// The GB_ADDR_CONFIG tests above pin the SE and RB counts; this pins what
// gfx_setup_rb does with them, which is what every later stage consumes.
// Both dies are covered because the mask arithmetic is width-dependent:
//
//   Navi 44  2 SE x 2 SH/SE x 2 RB/SE -> SA bitmap 4 bits, RB bitmap 4 bits,
//            RB bits per SA = 2/2 = 1
//   Navi 48  4 SE x 2 SH/SE x 4 RB/SE -> SA bitmap 8 bits, RB bitmap 16 bits,
//            RB bits per SA = 4/2 = 2
//
// A width mistake here is silent: the masks are still "some" number, the
// ladder still runs, and only a harvested part would expose it. The disable
// field widths are the Linux header's own (gc_12_0_0_sh_mask.h):
// BACKEND_DISABLE is 4 bits at shift 4, SA_DISABLE 16 bits at shift 8.
static GFXConfig prepareRb(Scratch &s, IPVersion v, uint32_t gb)
{
    s.dev.ip.setVersion(IPBlock::GC, v);
    s.reg(gb);
    GFXConfig cfg{};
    (void)gfx_gpu_early_init(s.dev, cfg);
    (void)gfx_get_gb_addr_config(s.dev, cfg);
    return cfg;
}

void setupRbTests()
{
    std::printf("\n================ RB harvest masks (gfx_setup_rb) ================\n");

    // ---- Navi 44, nothing harvested ----
    {
        Scratch s;
        GFXConfig cfg = prepareRb(s, {12, 0, 0}, packGb(1, 1, 1, 3, 1, 1));
        eq("navi44 rb: SEs decoded", cfg.max_shader_engines, 2);
        eq("navi44 rb: RBs/SE decoded", cfg.max_backends_per_se, 2);
        eq("navi44 rb: setup_rb", gfx_setup_rb(s.dev, cfg), kIOReturnSuccess);
        eq("navi44 rb: active_sa = 4 SAs", cfg.active_sa_bitmap, 0xFu);
        eq("navi44 rb: global_active_rb = 4 RBs", cfg.global_active_rb_bitmap, 0xFu);
        eq("navi44 rb: active_rb = SA-derived", cfg.active_rb_bitmap, 0xFu);
        eq("navi44 rb: num_rbs = 2 SE x 2", cfg.num_rbs, 4);
    }

    // ---- Navi 44, shader array 2 disabled: its RB goes with it ----
    {
        Scratch s;
        GFXConfig cfg = prepareRb(s, {12, 0, 0}, packGb(1, 1, 1, 3, 1, 1));
        s.gc0(GFXRegs::GRBM_CC_GC_SA_UNIT_DISABLE,
              0x4u << GRBM_CC_GC_SA_UNIT_DISABLE__SA_DISABLE__SHIFT);
        eq("navi44 SA2 off: setup_rb", gfx_setup_rb(s.dev, cfg), kIOReturnSuccess);
        eq("navi44 SA2 off: active_sa", cfg.active_sa_bitmap, 0xBu);
        eq("navi44 SA2 off: active_rb follows its SA", cfg.active_rb_bitmap, 0xBu);
        eq("navi44 SA2 off: num_rbs", cfg.num_rbs, 3);
    }

    // ---- Navi 44, one RB disabled by the use mask: SA intact, RB gone ----
    {
        Scratch s;
        GFXConfig cfg = prepareRb(s, {12, 0, 0}, packGb(1, 1, 1, 3, 1, 1));
        s.gc1(GFXRegs::GC_USER_RB_BACKEND_DISABLE,
              0x8u << GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT);
        eq("navi44 user RB3 off: setup_rb", gfx_setup_rb(s.dev, cfg), kIOReturnSuccess);
        eq("navi44 user RB3 off: active_sa untouched", cfg.active_sa_bitmap, 0xFu);
        eq("navi44 user RB3 off: global_active_rb", cfg.global_active_rb_bitmap, 0x7u);
        eq("navi44 user RB3 off: active_rb loses it too", cfg.active_rb_bitmap, 0x7u);
        eq("navi44 user RB3 off: num_rbs", cfg.num_rbs, 3);
    }

    // ---- Navi 48, nothing harvested ----
    {
        Scratch s;
        GFXConfig cfg = prepareRb(s, {12, 0, 1}, packGb(2, 2, 2, 3, 1, 1));
        eq("navi48 rb: SEs decoded", cfg.max_shader_engines, 4);
        eq("navi48 rb: RBs/SE decoded", cfg.max_backends_per_se, 4);
        eq("navi48 rb: setup_rb", gfx_setup_rb(s.dev, cfg), kIOReturnSuccess);
        eq("navi48 rb: active_sa = 8 SAs", cfg.active_sa_bitmap, 0xFFu);
        eq("navi48 rb: global_active_rb = 16 RBs", cfg.global_active_rb_bitmap, 0xFFFFu);
        eq("navi48 rb: active_rb = 2 bits per SA", cfg.active_rb_bitmap, 0xFFFFu);
        eq("navi48 rb: num_rbs = 4 SE x 4", cfg.num_rbs, 16);
    }

    // ---- Navi 48, SAs 0 and 1 disabled and RB 1 disabled ----
    // Each SA disables a 2-bit RB pair, so SAs 0,1 remove RBs 0..3; RB 1 was
    // already inside that, so the intersection loses nothing extra.
    {
        Scratch s;
        GFXConfig cfg = prepareRb(s, {12, 0, 1}, packGb(2, 2, 2, 3, 1, 1));
        s.gc0(GFXRegs::GRBM_CC_GC_SA_UNIT_DISABLE,
              0x3u << GRBM_CC_GC_SA_UNIT_DISABLE__SA_DISABLE__SHIFT);
        s.gc1(GFXRegs::GC_USER_RB_BACKEND_DISABLE,
              0x2u << GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT);
        eq("navi48 SA0-1 off: setup_rb", gfx_setup_rb(s.dev, cfg), kIOReturnSuccess);
        eq("navi48 SA0-1 off: active_sa", cfg.active_sa_bitmap, 0xFCu);
        eq("navi48 SA0-1 off: active_rb loses the pair", cfg.active_rb_bitmap, 0xFFF0u);
        eq("navi48 SA0-1 off: num_rbs = 12", cfg.num_rbs, 12);
    }

    // ---- refusals: unresolved GC, and caps that cannot be masked ----
    {
        Scratch s;
        s.dev.ip = IPBaseTable{};
        GFXConfig cfg{};
        eq("setup_rb without a GC base", gfx_setup_rb(s.dev, cfg), kIOReturnNotReady);
    }
    {
        Scratch s;
        GFXConfig cfg{};   // every cap zero: width/height unknown
        eq("setup_rb without caps", gfx_setup_rb(s.dev, cfg), kIOReturnNotReady);
    }
}

// ------------------------------------------------- topology record (QueryInfo tail)
//
// gfx_topo_report is what the kext appends to QueryInfo (ABI 1.13) so Mesa can
// stop assuming gfx1201. Its fields mirror Linux's amdgpu_info_ioctl one for
// one, so the expectations below are written from that ioctl (amdgpu_kms.c
// 952/953/969/970/1015/1020/1029) and not from GFXConfig — a copy that agrees
// with the struct but not with the ioctl is exactly the bug this pins.
//
// The load-bearing pair is num_rb_pipes vs enabled_rb_pipes_mask: the first is
// the PHYSICAL product SE x RB/SE, the second the HARVESTED bitmap. They are
// asserted to disagree in the harvested case, which no other check here does.
struct Brought {
    GFXConfig cfg;
    GFXTopo   topo;
};

// The real bring-up order, not a hand-built config: gfx_constants_init runs
// early_init -> gb_addr_config -> setup_rb -> get_cu_info and marks the config
// inited at the end, which is the gate gfx_topo_report reads.
static Brought bringUp(Scratch &s, IPVersion v, uint32_t gb)
{
    s.dev.ip.setVersion(IPBlock::GC, v);
    s.reg(gb);
    Brought b{};
    (void)gfx_constants_init(s.dev, b.cfg);
    gfx_topo_report(b.cfg, v, b.topo);
    return b;
}

void topoTests()
{
    std::printf("\n================ topology record (gfx_topo_report) ================\n");

    const uint32_t kGb44 = packGb(1, 1, 1, 3, 1, 1);   // 2 SE, 2 RB/SE
    const uint32_t kGb48 = packGb(2, 2, 2, 3, 1, 1);   // 4 SE, 4 RB/SE

    // ---- Navi 44, a full die: 2 SE x 2 SA x 8 CU, 2 RB/SE ----
    {
        Scratch s;
        const Brought b = bringUp(s, {12, 0, 0}, kGb44);
        ok("navi44 topo: constants_init inited the config", b.cfg.inited);
        ok("navi44 topo: valid", b.topo.valid);
        eq("navi44 topo: gc version",
           ((b.topo.gc_major << 16) | (b.topo.gc_minor << 8) | b.topo.gc_rev),
           0x000C0000u);   // IP_VERSION(12,0,0) — 12 is decimal, not 0x12
        eq("navi44 topo: max_shader_engines (:952)", b.topo.max_shader_engines, 2);
        eq("navi44 topo: max_sh_per_se (:953)", b.topo.max_sh_per_se, 2);
        eq("navi44 topo: max_cu_per_sh (:1029)", b.topo.max_cu_per_sh, 8);
        eq("navi44 topo: max_backends_per_se", b.topo.max_backends_per_se, 2);
        eq("navi44 topo: num_rb_pipes = 2 SE x 2 RB (:970)", b.topo.num_rb_pipes, 4);
        eq("navi44 topo: enabled_rb_pipes_mask (:969)", b.topo.enabled_rb_pipes_mask, 0xFu);
        eq("navi44 topo: mask_hi unused on RDNA4", b.topo.enabled_rb_pipes_mask_hi, 0u);
        eq("navi44 topo: cu_active_number = 32 (:1015)", b.topo.cu_active_number, 32);
        eq("navi44 topo: cu_active_wgp_number = 16", b.topo.cu_active_wgp_number, 16);
        eq("navi44 topo: num_se from live rows", b.topo.num_se, 2);
        for (uint32_t se = 0; se < 4; se++) {
            for (uint32_t sa = 0; sa < 2; sa++) {
                const uint32_t want = (se < 2) ? 0xFFu : 0u;   // 8 CUs, or no row
                eq(("navi44 topo: cu_bitmap[" + std::to_string(se) + "][" +
                    std::to_string(sa) + "] (:1020)").c_str(),
                   b.topo.cu_bitmap[se][sa], want);
            }
        }
        eq("navi44 topo: mask popcount == num_rbs",
           __builtin_popcount(b.topo.enabled_rb_pipes_mask), b.cfg.num_rbs);
    }

    // ---- Navi 48, a full die: 4 SE x 2 SA x 8 CU, 4 RB/SE ----
    // The die Mesa assumed for everyone: 64 CUs and a 16-bit RB mask.
    {
        Scratch s;
        const Brought b = bringUp(s, {12, 0, 1}, kGb48);
        ok("navi48 topo: valid", b.topo.valid);
        eq("navi48 topo: gc version",
           ((b.topo.gc_major << 16) | (b.topo.gc_minor << 8) | b.topo.gc_rev),
           0x000C0001u);   // IP_VERSION(12,0,1)
        eq("navi48 topo: max_shader_engines", b.topo.max_shader_engines, 4);
        eq("navi48 topo: max_backends_per_se", b.topo.max_backends_per_se, 4);
        eq("navi48 topo: num_rb_pipes = 4 SE x 4 RB", b.topo.num_rb_pipes, 16);
        eq("navi48 topo: enabled_rb_pipes_mask", b.topo.enabled_rb_pipes_mask, 0xFFFFu);
        eq("navi48 topo: cu_active_number = 64", b.topo.cu_active_number, 64);
        eq("navi48 topo: cu_active_wgp_number = 32", b.topo.cu_active_wgp_number, 32);
        eq("navi48 topo: num_se", b.topo.num_se, 4);
        for (uint32_t se = 0; se < 4; se++) {
            for (uint32_t sa = 0; sa < 2; sa++) {
                eq(("navi48 topo: cu_bitmap[" + std::to_string(se) + "][" +
                    std::to_string(sa) + "]").c_str(),
                   b.topo.cu_bitmap[se][sa], 0xFFu);
            }
        }
    }

    // ---- Navi 44 with SE1 harvested off entirely (SAs 2 and 3 disabled) ----
    // 1 shader array = 1 RB here, so the mask loses 2 bits while num_rb_pipes
    // stays 4: the physical/harvested split made visible.
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        s.reg(kGb44);
        s.gc0(GFXRegs::GRBM_CC_GC_SA_UNIT_DISABLE,
              0xCu << GRBM_CC_GC_SA_UNIT_DISABLE__SA_DISABLE__SHIFT);
        const Brought b = bringUp(s, {12, 0, 0}, kGb44);
        ok("navi44 SE1 gone: valid", b.topo.valid);
        eq("navi44 SE1 gone: num_rb_pipes keeps the physical 4",
           b.topo.num_rb_pipes, 4);
        eq("navi44 SE1 gone: enabled_rb_pipes_mask is the harvested 2",
           b.topo.enabled_rb_pipes_mask, 0x3u);
        ok("navi44 SE1 gone: the two RB fields must not be equal",
           b.topo.num_rb_pipes != b.topo.enabled_rb_pipes_mask);
        eq("navi44 SE1 gone: num_rbs agrees with the mask", b.cfg.num_rbs, 2);
        eq("navi44 SE1 gone: 16 CUs left", b.topo.cu_active_number, 16);
        eq("navi44 SE1 gone: 8 WGPs left", b.topo.cu_active_wgp_number, 8);
        eq("navi44 SE1 gone: num_se drops to 1", b.topo.num_se, 1);
        eq("navi44 SE1 gone: SE0 SA0 row", b.topo.cu_bitmap[0][0], 0xFFu);
        eq("navi44 SE1 gone: SE0 SA1 row", b.topo.cu_bitmap[0][1], 0xFFu);
        eq("navi44 SE1 gone: SE1 SA0 row empty", b.topo.cu_bitmap[1][0], 0u);
        eq("navi44 SE1 gone: SE1 SA1 row empty", b.topo.cu_bitmap[1][1], 0u);
    }

    // ---- Navi 44 with one WGP harvested in every live array ----
    // CC_GC_SHADER_ARRAY_CONFIG.INACTIVE_WGPS is bit-per-WGP at 31:16 and is
    // read broadcast-wide, so WGP0 disappearing costs 2 CUs in each of 4
    // arrays: 32 -> 24, and a 0xfc row instead of 0xff. The RB mask is
    // untouched — CU harvest and RB harvest are different fields, and Mesa
    // needs both to be reported separately.
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        s.reg(kGb44);
        s.gc0(GFXRegs::CC_GC_SHADER_ARRAY_CONFIG,
              0x1u << CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT);
        const Brought b = bringUp(s, {12, 0, 0}, kGb44);
        ok("navi44 WGP0 off: valid", b.topo.valid);
        eq("navi44 WGP0 off: 24 CUs", b.topo.cu_active_number, 24);
        eq("navi44 WGP0 off: 12 WGPs", b.topo.cu_active_wgp_number, 12);
        eq("navi44 WGP0 off: row is 8 CU bits minus the pair",
           b.topo.cu_bitmap[0][0], 0xFCu);
        eq("navi44 WGP0 off: every array loses the same WGP",
           b.topo.cu_bitmap[1][1], 0xFCu);
        eq("navi44 WGP0 off: RB mask unaffected", b.topo.enabled_rb_pipes_mask, 0xFu);
        eq("navi44 WGP0 off: num_rb_pipes unaffected", b.topo.num_rb_pipes, 4);
        eq("navi44 WGP0 off: num_se still 2", b.topo.num_se, 2);
    }

    // ---- the USER side of the same field ----
    // get_cu_info ORs CC and USER before inverting, so a user-forced WGP
    // disable harvests exactly like the factory fuse does.
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        s.reg(kGb44);
        s.gc1(GFXRegs::GC_USER_SHADER_ARRAY_CONFIG,
              0xFu << GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT);
        const Brought b = bringUp(s, {12, 0, 0}, kGb44);
        ok("navi44 all WGPs user-off: valid", b.topo.valid);
        eq("navi44 all WGPs user-off: no CUs left", b.topo.cu_active_number, 0);
        eq("navi44 all WGPs user-off: rows empty", b.topo.cu_bitmap[0][0], 0u);
        eq("navi44 all WGPs user-off: num_se falls to 0", b.topo.num_se, 0);
    }

    // ---- a config that never went through constants_init ----
    // Reporting zeros here would tell Mesa "this die has no CUs", which is a
    // lie that propagates into the RADEON_DEBUG topology print and the RSR
    // tables. valid=false is the signal for the kext to omit the tail and for
    // Mesa to keep its own table.
    {
        GFXConfig cfg{};
        GFXTopo t{};
        gfx_topo_report(cfg, IPVersion{12, 0, 0}, t);
        ok("uninited: not valid", !t.valid);
        eq("uninited: gc version still reported", t.gc_minor, 0u);
        eq("uninited: major still reported", t.gc_major, 12u);
        eq("uninited: rev still reported", t.gc_rev, 0u);
        eq("uninited: no SE count invented", t.max_shader_engines, 0u);
        eq("uninited: no RB count invented", t.num_rb_pipes, 0u);
        eq("uninited: no CU count invented", t.cu_active_number, 0u);
        eq("uninited: no live SEs", t.num_se, 0u);
        eq("uninited: mask left zero", t.enabled_rb_pipes_mask, 0u);
    }
    {
        Scratch s;
        s.dev.ip = IPBaseTable{};          // no GC base: constants_init refuses
        s.reg(kGb44);
        const Brought b = bringUp(s, {12, 0, 0}, kGb44);
        ok("no GC base: constants_init did not reach inited", !b.cfg.inited);
        ok("no GC base: tail stays invalid", !b.topo.valid);
    }
    {
        // GB_ADDR_CONFIG unreadable (all-ones BAR) is the other realistic
        // half-bring-up: constants_init still completes on the fallback caps,
        // so the tail must report the fallback geometry rather than nothing.
        Scratch s;
        s.reg(0xFFFFFFFFu);
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        GFXConfig cfg{};
        (void)gfx_constants_init(s.dev, cfg);
        GFXTopo t{};
        gfx_topo_report(cfg, IPVersion{12, 0, 0}, t);
        ok("gb unreadable: still inited", cfg.inited);
        ok("gb unreadable: tail valid", t.valid);
        eq("gb unreadable: fallback SEs", t.max_shader_engines, 2);
        eq("gb unreadable: fallback RBs/SE", t.max_backends_per_se, 2);
        eq("gb unreadable: num_rb_pipes", t.num_rb_pipes, 4);
        eq("gb unreadable: harvest regs read as 0 -> all CUs",
           t.cu_active_number, 32);
    }
}

// ------------------------------------------------ firmware set from discovery
//
// probe() has to guess the die from the PCI id, and its allowlist names three.
// These cases drive the correction path with version triples and pin two
// separate properties:
//
//   - a triple that names a die moves the table to that die's blobs even when
//     the guess was the other die (the case that matters: Navi 44 silicon under
//     an id nobody has seen, which would otherwise load psp_14_0_3_sos), and
//   - a triple that names nothing leaves the guess exactly where it was, from
//     either starting set. A half-matched triple must NOT pick a side: the PSP
//     chain rejecting TMR-resident LOAD_IP_FW three stages later is a far harder
//     diagnosis than the caller's "keeping the PCI-id pick" log line.
void fwSelectFromIpTests(const std::string &fwDir)
{
    std::printf("\n================ firmware set from IP discovery ================\n");

    const IPVersion kGc44{12, 0, 0}, kGc48{12, 0, 1};
    const IPVersion kMp44{14, 0, 2}, kMp48{14, 0, 3};

    // The headline case: probe() fell back to Navi 48 because it did not
    // recognise the id; discovery says 12.0.0 + 14.0.2 and the table moves.
    fw_select_for_device(0x75A0);            // an id probe()'s allowlist does not name
    ok("unlisted id starts on the Navi 48 guess", !fw_is_navi44());
    ok("discovery names Navi 44 -> accepted", fw_select_from_ip(kGc44, kMp44, kMp44));
    checkSet("discovery: GC 12.0.0 / MP0 14.0.2 / MP1 14.0.2", true, fwDir);

    // The other direction, and it has to be idempotent: probe() already got it
    // right for 0x7550, so re-selecting changes nothing observable.
    fw_select_for_device(0x7550);
    ok("discovery confirms Navi 48 -> accepted", fw_select_from_ip(kGc48, kMp48, kMp48));
    checkSet("discovery: GC 12.0.1 / MP0 14.0.3 (confirmed, not corrected)", false, fwDir);
    ok("discovery names Navi 44 over a Navi 48 guess -> accepted",
       fw_select_from_ip(kGc44, kMp44, kMp44));
    eqs("corrected set's SOS", fw_get(FwId::PSP_SOS)->name, "psp_14_0_2_sos");
    ok("re-selecting the set already in place -> accepted",
       fw_select_from_ip(kGc44, kMp44, kMp44));
    eqs("re-select rebuilt nothing", fw_get(FwId::PSP_SOS)->name, "psp_14_0_2_sos");

    eqs("fw_set_name follows the selection (navi44)", fw_set_name(), "navi44");
    fw_select_from_ip(kGc48, kMp48, kMp48);
    eqs("fw_set_name follows the selection (navi48)", fw_set_name(), "navi48");

    struct Refusal {
        IPVersion gc, mp0, mp1;
        const char *why;
    };
    const Refusal kRefusals[] = {
        {kGc44, kMp48, kMp48, "GC 12.0.0 but PSP/SMU 14.0.3"},
        {kGc48, kMp44, kMp44, "GC 12.0.1 but PSP/SMU 14.0.2"},
        {kGc44, kMp48, kMp44, "PSP disagrees with GC and SMU"},
        {kGc44, kMp44, kMp48, "SMU disagrees with GC and PSP"},
        {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, "nothing discovered (all-zero versions)"},
        {{12, 0, 0}, {0, 0, 0}, {14, 0, 2}, "MP0 never filled by discovery"},
        {{12, 0, 4}, {14, 0, 5}, {14, 0, 5}, "psp_14_0_5: no blobs embedded here"},
        {{12, 1, 0}, {14, 0, 2}, {14, 0, 2}, "GC minor 1: not the 12.0.x family"},
        {{11, 0, 3}, {14, 0, 2}, {14, 0, 2}, "GC 11.0.3: RDNA3, not this table"},
        {kGc44, {14, 1, 2}, kMp44, "MP0 minor 1"},
        {kGc44, kMp44, {15, 0, 2}, "MP1 major 15"},
    };
    for (const Refusal &r : kRefusals) {
        const std::string a44 = "refused (from navi44): ", b44 = "keeps navi44: ",
                          c44 = "navi44 blob untouched: ", a48 = "refused (from navi48): ",
                          b48 = "keeps navi48: ", c48 = "navi48 blob untouched: ";
        fw_select_for_device(0x7590);                    // from the Navi 44 set
        ok((a44 + r.why).c_str(), !fw_select_from_ip(r.gc, r.mp0, r.mp1));
        ok((b44 + r.why).c_str(), fw_is_navi44());
        eqs((c44 + r.why).c_str(), fw_get(FwId::PSP_SOS)->name, "psp_14_0_2_sos");

        fw_select_for_device(0x7550);                    // and from the Navi 48 set
        ok((a48 + r.why).c_str(), !fw_select_from_ip(r.gc, r.mp0, r.mp1));
        ok((b48 + r.why).c_str(), !fw_is_navi44());
        eqs((c48 + r.why).c_str(), fw_get(FwId::PSP_SOS)->name, "psp_14_0_3_sos");
    }

    // Leave the table on the die this port targets, so anything added after this
    // function starts from a known set.
    std::printf("-- refusals: %zu triples x 2 starting sets x 3 checks = %zu checks --\n",
                sizeof(kRefusals) / sizeof(kRefusals[0]),
                3 * 2 * (sizeof(kRefusals) / sizeof(kRefusals[0])));
    fw_select_from_ip(kGc44, kMp44, kMp44);
}

} // namespace

// ------------------------------------------------- GC info table caps (0014)
//
// gfx_gpu_early_init() no longer guesses the die geometry from the GC IP version: it
// reads the discovery GC info table, exactly where Linux reads it
// (amdgpu_discovery.c:2173-2181). gc_num_tccs is the reason the parser had to exist -
// RDNA4 states the TCC count nowhere else. gc_12_0_0_sh_mask.h has no TCC field at all
// and gfx_v12_0_get_tcc_info() is an empty stub, which is also why tcc_disabled_mask is
// provably 0 on this generation and why only the count is published.
//
// The order is Linux's too: table first, then GB_ADDR_CONFIG confirms SE and RB, because
// the register is what the VBIOS programmed for the die that is actually fitted.
static void setTab(Scratch &s, uint32_t se, uint32_t sh, uint32_t cu, uint32_t rb,
                   uint32_t tcc)
{
    IpGcInfo &g = s.dev.gcTab;
    g.valid = 1; g.verMajor = 2; g.verMinor = 0;
    g.numSe = se; g.numShPerSe = sh; g.numCuPerSh = cu; g.numRbPerSe = rb; g.numTccs = tcc;
}

void gcTableTests()
{
    std::printf("\n================ GC info table caps (0014) ================\n");

    struct TabCase {
        uint32_t se, sh, cu, rb, tcc;
        uint32_t wantSe, wantSh, wantCu, wantRb, wantTcc;
        const char *label;
    };
    // want* is what the caps must end up as: the table's own numbers when every field is
    // inside the bounds, or the Navi 44 per-IP-version guess (2 SE, 2 SA, 8 CU, 2 RB) with
    // TCC unreported when any one of them is not. All-or-nothing, deliberately: a table
    // that gets one word wrong is a table nobody checked, and half a harvest mask is the
    // kind of thing that corrupts the CU bitmap silently.
    const TabCase cases[] = {
        {2, 2, 8, 2,  8,   2, 2, 8, 2,  8, "Navi 44 shape, agrees with the guess, adds tcc 8"},
        {4, 2, 8, 4, 32,   4, 2, 8, 4, 32, "a 4 SE die reported to GC 12.0.0: the table wins"},
        {1, 1, 2, 1,  1,   1, 1, 2, 1,  1, "every field at its legal minimum"},
        {8, 2,32, 8, 64,   8, 2,32, 8, 64, "every field at its legal maximum"},
        {0, 2, 8, 2,  8,   2, 2, 8, 2,  0, "se=0"},
        {9, 2, 8, 2,  8,   2, 2, 8, 2,  0, "se=9"},
        {2, 3, 8, 2,  8,   2, 2, 8, 2,  0, "sh_per_se=3 (cu_bitmap has no room)"},
        {2, 2, 1, 2,  8,   2, 2, 8, 2,  0, "cu_per_sh=1"},
        {2, 2, 8, 9,  8,   2, 2, 8, 2,  0, "rb_per_se=9"},
        {2, 2, 8, 2,  0,   2, 2, 8, 2,  0, "tcc=0: reads as no number"},
        {2, 2, 8, 2, 65,   2, 2, 8, 2,  0, "tcc=65"},
        {2, 2, 8, 2,200,   2, 2, 8, 2,  0, "tcc=200 with the rest sane: geometry NOT overridden"},
    };
    for (const TabCase &c : cases) {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        setTab(s, c.se, c.sh, c.cu, c.rb, c.tcc);
        GFXConfig cfg{};
        const kern_return_t kr = gfx_gpu_early_init(s.dev, cfg);
        const std::string p = std::string("  [table ") + c.label + "] ";
        eq((p + "kIOReturn").c_str(), kr, kIOReturnSuccess);
        eq((p + "max_shader_engines").c_str(), cfg.max_shader_engines, c.wantSe);
        eq((p + "max_sh_per_se").c_str(), cfg.max_sh_per_se, c.wantSh);
        eq((p + "max_cu_per_sh").c_str(), cfg.max_cu_per_sh, c.wantCu);
        eq((p + "max_backends_per_se").c_str(), cfg.max_backends_per_se, c.wantRb);
        eq((p + "num_tcc_blocks").c_str(), cfg.num_tcc_blocks, c.wantTcc);
    }

    // ---- no table at all (the common case for a kext that never saw one) ----
    {
        Scratch s;
        s.dev.ip.setVersion(IPBlock::GC, {12, 0, 0});
        GFXConfig cfg{};
        (void)gfx_gpu_early_init(s.dev, cfg);
        eq("no table: caps stay the GC 12.0.0 guess", cfg.max_shader_engines, 2);
        eq("no table: num_tcc_blocks stays 0 (Mesa keeps its 32)", cfg.num_tcc_blocks, 0);
        ok("no table: gcTab really is the zeroed default", s.dev.gcTab.valid == 0);
    }

    // ---- the register still outranks the table, and the record carries both ----
    {
        Scratch s;
        setTab(s, 4, 2, 8, 4, 8);            // a Navi 48-shaped table
        s.reg(packGb(1, 1, 1, 3, 1, 1));     // Navi 44 silicon: 2 SE, 2 RB/SE
        const Brought b = bringUp(s, {12, 0, 0}, packGb(1, 1, 1, 3, 1, 1));
        ok("register vs table: constants_init inited the config", b.cfg.inited);
        eq("register vs table: GB_ADDR_CONFIG wins max_shader_engines",
           b.topo.max_shader_engines, 2);
        eq("register vs table: GB_ADDR_CONFIG wins max_backends_per_se",
           b.topo.max_backends_per_se, 2);
        eq("register vs table: the TCC count survives it (nothing contradicts it)",
           b.topo.num_tcc_blocks, 8);
        eq("register vs table: num_rb_pipes is the product of the REGISTER's numbers",
           b.topo.num_rb_pipes, 4);
    }

    // ---- a die with no table reports no TCC count in the QueryInfo tail either ----
    {
        Scratch s;
        const Brought b = bringUp(s, {12, 0, 0}, packGb(1, 1, 1, 3, 1, 1));
        eq("topo, no table: num_tcc_blocks", b.topo.num_tcc_blocks, 0);
        eq("topo, no table: the geometry is still the register's",
           b.topo.max_shader_engines, 2);
    }
}

int main(int argc, char **argv)
{
    const std::string fwDir = argc > 1 ? argv[1] : "firmware";
    std::printf("navi44_die_test -- Navi 44 firmware set (0002) + die geometry (0003)\n");
    firmwareTests(fwDir);
    fwSelectFromIpTests(fwDir);
    geometryTests();
    gcTableTests();
    setupRbTests();
    topoTests();

    std::printf("\n================ SUMMARY ================\n");
    std::printf("%d check(s) failed out of %d\n", gFail, gRun);
    return gFail == 0 ? 0 : 1;
}
