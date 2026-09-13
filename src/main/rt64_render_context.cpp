#include <memory>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#define HLSL_CPU
#include "hle/rt64_application.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"

// Material free-list integrity probe (src/main/upstream_compat.cpp).
extern "C" void rs64_matfreelist_check(uint8_t*, const char*);
// Neutralizes bogus matpool-targeting SET_COLOR_IMAGE commands in the HLE
// display list before RT64 processes them (src/main/upstream_compat.cpp).
extern "C" void rs64_neutralize_matpool_cimg(uint8_t*, uint32_t dl_phys);
// Dumps RT64's tracked-framebuffer registry to stderr — used to confirm
// whether RT64 is holding a framebuffer that overlaps the matpool when
// corruption fires. Definition below; called from upstream_compat.cpp's
// matpool-repair when it detects a corrupt free-list.
extern "C" void rs64_dump_rt64_framebuffers(const char* where);
// Sweeps RT64's tracked-FB registry and erases any entry with raw start
// below 0x400000 (real fb threshold). Definition below; called from
// the HLE send_dl path right after processDisplayLists as a safety net
// for CIMGs the pre-process DL walker missed.
extern "C" void rs64_sanitize_fb_registry(void);
// Time spent inside processDisplayLists for the last gfx task (reported by the ROGUESQ_LOG_GFX_TASK line).
extern "C" volatile long long g_rs64_pdl_us; volatile long long g_rs64_pdl_us = 0;
extern "C" uint8_t* g_rs64_parse_rdram;   // ultramodern events.cpp: RDRAM snapshot for the current parse
extern "C" int rs64_fb_guards_mask(void);
extern "C" int rs64_vi_driven(void);

// (g_attribution_active decl removed 2026-06-02 — was unused here; g_current_scene is the screen-state global.)
extern "C" volatile unsigned g_last_swap_fb;  // game's last-swapped front buffer (for present)

// Shared Application accessor for the LLE DPC bridge (src/rsp/dpc_bridge.cpp).
// The bridge needs to forward Factor 5 raw RDP byte ranges via
// processDisplayLists(isHLE=false). Set on construction, cleared on shutdown.
static std::atomic<RT64::Application*> g_rt64_app{nullptr};

extern "C" void rs64_dpc_get_cumulative_histogram(uint32_t out[64]);
extern "C" uint32_t rs64_dpc_get_cumulative_fullsyncs();
extern "C" void rs64_cine_dump_if_stuck(void);
extern "C" void rs64_cine_progress_log(void);
// LLE GFX entry — runs the RSP recompile (factor5_boot + factor5_ucode)
// against this task's data. Defined in main.cpp. OSTask comes from
// ultramodern/ultra64.h (already included via ultramodern.hpp).
extern "C" int rs64_run_lle_gfx(uint8_t* rdram, const OSTask* task);
extern "C" volatile int g_active_overlay;  // 0=gameplay 1=menu 2=cinematic (rs64_load_overlay)
extern "C" volatile int g_current_scene;   // menuOverlayInit action id (9 = attribution/LucasArts)
extern "C" unsigned long long rs64_cine_iter_get(void);  // cinematic loop iterations (CINE_YIELD ticks)

// Symbolicated stack dump (src/main/main.cpp). Used by the HLE-submission SEH
// filter to pinpoint which GBI op faults during the cinematic.
extern void print_stack_with_symbols(void** frames, unsigned short count);

#ifdef _WIN32
// SEH filter for the HLE processDisplayLists __try. Logs exception code, the
// faulting instruction PC, the bad access address + R/W, and a symbolicated
// stack so we can see exactly which op crashes (e.g. the explosion bloom op_02
// geometry) instead of just "SEH streak". Logs the first few only, then quiet.
static int hle_seh_filter(EXCEPTION_POINTERS* ep) {
    static int s_logged = 0;
    if (ep && ep->ExceptionRecord && s_logged++ < 5) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        unsigned long long acc = (er->NumberParameters >= 2) ? (unsigned long long)er->ExceptionInformation[1] : 0;
        int rw = (er->NumberParameters >= 1) ? (int)er->ExceptionInformation[0] : -1;
        fprintf(stderr, "[hle-seh] code=0x%08X pc=%p access=0x%llX rw=%d (0=read,1=write,8=exec)\n",
                (unsigned)er->ExceptionCode, er->ExceptionAddress, acc, rw);
        void* frames[24];
        unsigned short n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        print_stack_with_symbols(frames, n);
        fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// RDP/RSP register state owned by this file
static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

static unsigned int MI_INTR_REG      = 0;
static unsigned int DPC_START_REG    = 0;
static unsigned int DPC_END_REG      = 0;
static unsigned int DPC_CURRENT_REG  = 0;
static unsigned int DPC_STATUS_REG   = 0;
static unsigned int DPC_CLOCK_REG    = 0;
static unsigned int DPC_BUFBUSY_REG  = 0;
static unsigned int DPC_PIPEBUSY_REG = 0;
static unsigned int DPC_TMEM_REG     = 0;

static void dummy_check_interrupts() {}

static ultramodern::renderer::SetupResult map_result(RT64::Application::SetupResult r) {
    switch (r) {
    case RT64::Application::SetupResult::Success:                  return ultramodern::renderer::SetupResult::Success;
    case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
    case RT64::Application::SetupResult::InvalidGraphicsAPI:       return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
    case RT64::Application::SetupResult::GraphicsAPINotFound:      return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
    case RT64::Application::SetupResult::GraphicsDeviceNotFound:   return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
}

// ---------------------------------------------------------------------------
// RT64Context — wraps RT64::Application as a RendererContext
// ---------------------------------------------------------------------------
namespace recomp {

class RT64Context : public ultramodern::renderer::RendererContext {
public:
    std::unique_ptr<RT64::Application> app;
    static inline std::atomic<bool> s_hle_disabled{false};

    RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool debug) {
        static unsigned char dummy_rom_header[0x40] = {};

        RT64::Application::Core appCore{};
#if defined(_WIN32)
        appCore.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
        appCore.window = window_handle;
#elif defined(__APPLE__)
        appCore.window.window = window_handle.window;
        appCore.window.view   = window_handle.view;
#endif

        appCore.checkInterrupts = dummy_check_interrupts;
        appCore.HEADER  = dummy_rom_header;
        appCore.RDRAM   = rdram;
        appCore.DMEM    = DMEM;
        appCore.IMEM    = IMEM;

        appCore.MI_INTR_REG     = &MI_INTR_REG;
        appCore.DPC_START_REG   = &DPC_START_REG;
        appCore.DPC_END_REG     = &DPC_END_REG;
        appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
        appCore.DPC_STATUS_REG  = &DPC_STATUS_REG;
        appCore.DPC_CLOCK_REG   = &DPC_CLOCK_REG;
        appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
        appCore.DPC_PIPEBUSY_REG= &DPC_PIPEBUSY_REG;
        appCore.DPC_TMEM_REG    = &DPC_TMEM_REG;

        ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        appCore.VI_STATUS_REG         = &vi->VI_STATUS_REG;
        appCore.VI_ORIGIN_REG         = &vi->VI_ORIGIN_REG;
        appCore.VI_WIDTH_REG          = &vi->VI_WIDTH_REG;
        appCore.VI_INTR_REG           = &vi->VI_INTR_REG;
        appCore.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        appCore.VI_TIMING_REG         = &vi->VI_TIMING_REG;
        appCore.VI_V_SYNC_REG         = &vi->VI_V_SYNC_REG;
        appCore.VI_H_SYNC_REG         = &vi->VI_H_SYNC_REG;
        appCore.VI_LEAP_REG           = &vi->VI_LEAP_REG;
        appCore.VI_H_START_REG        = &vi->VI_H_START_REG;
        appCore.VI_V_START_REG        = &vi->VI_V_START_REG;
        appCore.VI_V_BURST_REG        = &vi->VI_V_BURST_REG;
        appCore.VI_X_SCALE_REG        = &vi->VI_X_SCALE_REG;
        appCore.VI_Y_SCALE_REG        = &vi->VI_Y_SCALE_REG;

        RT64::ApplicationConfiguration appConfig;
        appConfig.useConfigurationFile = false;
        appConfig.detectDataPath = false;

        app = std::make_unique<RT64::Application>(appCore, appConfig);
        // Dev mode default: ON for Debug builds (so F1 inspector is available),
        // OFF for Release. Override either way with ROGUESQ_HLE_DEV_MODE=0/1.
        // The inspector can be unstable with our HLE flow under load — fall
        // back to ROGUESQ_HLE_DEV_MODE=0 if it locks up an investigation.
#ifdef _DEBUG
        bool dev_mode_on = true;
#else
        bool dev_mode_on = false;
#endif
        if (const char* v = std::getenv("ROGUESQ_HLE_DEV_MODE")) {
            dev_mode_on = (v[0] && v[0] != '0');
        }
        app->userConfig.developerMode = debug || dev_mode_on;
        app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;

        // PresentEarly ON by default. Our cinematic stays on a single VI fb
        // address; without PresentEarly, RT64's updateScreen only pushes a
        // present when VI changes or RDRAM at VI_ORIGIN changes — neither
        // happens for HLE single-buffer flow, so rendered fbPairs never
        // reach the swapchain. PresentEarly pushes presents directly from
        // fullSync. Opt out via ROGUESQ_HLE_PRESENT_EARLY=0.
        bool present_early = true;
        if (const char* v = std::getenv("ROGUESQ_HLE_PRESENT_EARLY")) {
            present_early = (v[0] && v[0] != '0');
        }
        if (present_early) {
            app->enhancementConfig.presentation.mode =
                RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        }

        // 2026-05-10: Apply Zelda64Recomp's RT64 enhancement-config settings.
        // Without these, RT64's textureLOD selection and gbi branching take
        // different paths that cause our texture lookups to sample stale or
        // empty TMEM regions → glyph texrects render invisibly.
        //
        // forceBranch=true: forces gbi depth branches, preventing LODs from
        // kicking in for textures that don't have proper mipmap chains.
        //
        // textureLOD.scale=true: scales LODs based on output resolution so
        // higher-res rendering doesn't bias toward LOD 0 unnecessarily.
        app->enhancementConfig.f3dex.forceBranch = true;
        app->enhancementConfig.textureLOD.scale = true;
        // Diagnostic: ROGUESQ_GFX_API=vulkan forces Vulkan instead of the
        // default D3D12 (Automatic on Windows). Lets us isolate whether crashes
        // are DX12-specific or apply to both backends.
        if (const char* api = std::getenv("ROGUESQ_GFX_API")) {
            if (std::string_view(api) == "vulkan") {
                app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
                fprintf(stderr, "[RT64] graphics API forced to Vulkan via ROGUESQ_GFX_API\n");
            } else if (std::string_view(api) == "d3d12") {
                app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
                fprintf(stderr, "[RT64] graphics API forced to D3D12 via ROGUESQ_GFX_API\n");
            }
        }

        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_result(app->setup(thread_id));
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            fprintf(stderr, "[RT64] setup failed: %d\n", (int)setup_result);
            app = nullptr;
        }
        else {
            // Propagate construction-time enhancementConfig changes (e.g.
            // PresentEarly) into RT64's internal state. setup() initializes
            // before these are read, so without this call the changes stay
            // dormant.
            app->updateEnhancementConfig();
            fprintf(stderr, "[RT64] enhancementConfig: presentMode=%d forceBranch=%d textureLOD.scale=%d\n",
                    (int)app->enhancementConfig.presentation.mode,
                    (int)app->enhancementConfig.f3dex.forceBranch,
                    (int)app->enhancementConfig.textureLOD.scale);
            fflush(stderr);
        }
        if (app && app->appWindow) {
            // Diagnostic: dump the post-setup state of RT64's window/filter
            // so we know whether F1-F4 keypresses can reach the inspector.
            fprintf(stderr,
                "[RT64] post-setup: sdlWindow=%p sdlEventFilterInstalled=%d windowHook=%p developerMode=%d usesWindowMessageFilter=%d\n",
                (void*)app->appWindow->sdlWindow,
                (int)app->appWindow->sdlEventFilterInstalled,
#ifdef _WIN32
                (void*)app->appWindow->windowHook,
#else
                (void*)nullptr,
#endif
                (int)app->userConfig.developerMode,
                (int)app->usesWindowMessageFilter());
            fflush(stderr);
        }
        g_rt64_app.store(app.get());
    }

    bool valid() override {
        return app != nullptr;
    }

    bool update_config(const ultramodern::renderer::GraphicsConfig&,
                       const ultramodern::renderer::GraphicsConfig&) override {
        return true;
    }

    void enable_instant_present() override {
        // PresentEarly is set at construction (see ctor); this host hook
        // would re-apply it, but constructor-time setup is sufficient.
        // Respects ROGUESQ_HLE_PRESENT_EARLY=0 opt-out.
        if (app) {
            bool present_early = true;
            if (const char* v = std::getenv("ROGUESQ_HLE_PRESENT_EARLY")) {
                present_early = (v[0] && v[0] != '0');
            }
            if (present_early) {
                app->enhancementConfig.presentation.mode =
                    RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
                app->updateEnhancementConfig();
            }
        }
    }

    void send_dl(const OSTask* task) override {
        // ROGUESQ_LLE_FORCE=1: route this GFX task through the LLE RSP
        // recompile path INSTEAD of HLE. factor5_gfx_runner reproduces what
        // real-hardware RSP would do — runs boot+main ucode against DMEM,
        // emits RDP byte stream via mtc0 DPC_END writes, src/rsp/dpc_bridge.cpp
        // forwards those bytes to RT64 via processDisplayLists(isHLE=false).
        // This is the path needed for Factor 5's op 0x02 (vertex transform)
        // to actually emit triangles — HLE drops it.
        static int s_lle_force = -1;
        if (s_lle_force < 0) {
            const char* v = std::getenv("ROGUESQ_LLE_FORCE");
            s_lle_force = (v && *v && v[0] != '0') ? 1 : 0;
            if (s_lle_force) {
                fprintf(stderr, "[lle-force] LLE path enabled for M_GFXTASK; "
                                "HLE send_dl will skip processDisplayLists\n");
                fflush(stderr);
            }
        }
        if (s_lle_force && app) {
            // Gate LLE to attribution-shape DLs only. Cinematic-phase tasks
            // hit Unhandled jump target in factor5_ucode_recompiled and
            // corrupt RT64's deferred state (which then asserts 4-bit
            // readback downstream). Attribution DL signature: data_ptr =
            // 0x80720108 for task #1, 0x80720318 for task #2 of the same
            // phase (observed). Both live in the 0x80720000 page. Restrict
            // LLE to data_ptr in that page so cinematic tasks bypass it.
            //
            // ROGUESQ_LLE_UNGATED=1 disables this gate (debug/regression).
            const uint32_t dlp = (uint32_t)task->t.data_ptr;
            static int s_lle_ungated = -1;
            if (s_lle_ungated < 0) {
                const char* v = std::getenv("ROGUESQ_LLE_UNGATED");
                s_lle_ungated = (v && *v && v[0] != '0') ? 1 : 0;
            }
            const bool in_attribution_page = ((dlp & 0xFFFF0000u) == 0x80720000u);
            if (s_lle_ungated || in_attribution_page) {
                static int s_n = 0;
                int n = ++s_n;
                int r = rs64_run_lle_gfx(app->core.RDRAM, task);
                if (n <= 8 || (n & 31) == 0) {
                    fprintf(stderr, "[lle send_dl #%d] gate=%s exit=%d task->ucode=0x%08X dl=0x%08X\n",
                            n, in_attribution_page ? "attr" : "ungated",
                            r, (unsigned)task->t.ucode, dlp);
                    fflush(stderr);
                }
                // ROGUESQ_LLE_SOLO=1 skips the HLE fallthrough below. By default
                // we run HLE in parallel so HLE's green fillRect-override marker
                // stays visible alongside whatever LLE produces.
                static int s_lle_solo = -1;
                if (s_lle_solo < 0) {
                    const char* v = std::getenv("ROGUESQ_LLE_SOLO");
                    s_lle_solo = (v && *v && v[0] != '0') ? 1 : 0;
                }
                if (s_lle_solo) {
                    return;
                }
                // Fall through to HLE below.
            } else {
                // Cinematic / N64-logo / other tasks: HLE only. Skip LLE.
                static int s_skip_count = 0;
                int sn = ++s_skip_count;
                if (sn <= 4 || (sn & 127) == 0) {
                    fprintf(stderr, "[lle send_dl skip #%d] non-attribution DL dl=0x%08X (LLE bypassed)\n",
                            sn, dlp);
                    fflush(stderr);
                }
            }
        }

        // Standard HLE pipeline (matches Zelda64Recompiled / Starfox64Recomp).
        // RT64's GBI database now includes the Factor 5 ucode signatures (see
        // lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp), so loadUCodeGBI matches
        // and dispatches commands via the F3DFACTOR5 handlers.
        //
        // Mask 0x3FFFFFF strips the KSEG0 bits to get the physical RDRAM
        // offset that processDisplayLists expects.
        // ROGUESQ_LOG_OPCODE_HIST=1: per-call opcode histogram of the first
        // 256 DL commands at task->t.data_ptr. Lets us compare which
        // F3DFACTOR5 opcodes fire during attribution vs N64-logo phase to
        // test the "some opcode carries attribution-glyph semantics but is
        // currently no-op'd in our HLE" hypothesis.
        if (app) {
            // ROGUESQ_DUMP_UCODE=path — dump task ucode (IMEM) + ucode_data
            // (DMEM-bound segment) to disk on the first call so the F3DFACTOR5
            // ucode can be disassembled offline to decode op 0x02 semantics.
            static int s_dumped_ucode = 0;
            if (!s_dumped_ucode) {
                const char* dump_path = std::getenv("ROGUESQ_DUMP_UCODE");
                if (dump_path && *dump_path) {
                    s_dumped_ucode = 1;
                    const uint32_t ucode_phys = (uint32_t)task->t.ucode & 0x3FFFFFF;
                    const uint32_t udata_phys = (uint32_t)task->t.ucode_data & 0x3FFFFFF;
                    const uint32_t udata_size = task->t.ucode_data_size ? task->t.ucode_data_size : 0x800;
                    uint8_t* rdram = app->core.RDRAM;
                    // Standard RSP ucode IMEM is 4KB. Dump 8KB to be safe.
                    char ipath[512], dpath[512];
                    snprintf(ipath, sizeof ipath, "%s.imem.bin", dump_path);
                    snprintf(dpath, sizeof dpath, "%s.dmem.bin", dump_path);
                    FILE* f = fopen(ipath, "wb");
                    if (f) {
                        uint8_t buf[0x2000];
                        for (uint32_t i = 0; i < sizeof(buf); ++i) {
                            uint32_t off = ucode_phys + i;
                            buf[i] = (off < 0x800000u) ? rdram[off ^ 3] : 0;
                        }
                        fwrite(buf, 1, sizeof(buf), f);
                        fclose(f);
                        fprintf(stderr, "[dump-ucode] wrote IMEM %u bytes to %s (src=0x%08X)\n",
                                (unsigned)sizeof(buf), ipath, ucode_phys);
                    }
                    f = fopen(dpath, "wb");
                    if (f) {
                        std::vector<uint8_t> buf(udata_size);
                        for (uint32_t i = 0; i < udata_size; ++i) {
                            uint32_t off = udata_phys + i;
                            buf[i] = (off < 0x800000u) ? rdram[off ^ 3] : 0;
                        }
                        fwrite(buf.data(), 1, udata_size, f);
                        fclose(f);
                        fprintf(stderr, "[dump-ucode] wrote DMEM %u bytes to %s (src=0x%08X)\n",
                                udata_size, dpath, udata_phys);
                    }
                    fflush(stderr);
                }
            }
            // ROGUESQ_DUMP_VTXDATA=path — dump 64 KB starting at RAM
            // 0x80700000 and 0x80710000 on the first send_dl call. These
            // are the addresses op 0x01 references in attribution DLs
            // (alternating between consecutive submissions). Offline
            // decode reveals what vertex / asset data the F3DFACTOR5
            // ucode reads to drive op 0x02's matrix-vertex pipeline.
            static int s_dumped_vtx = 0;
            if (!s_dumped_vtx) {
                const char* dump_path = std::getenv("ROGUESQ_DUMP_VTXDATA");
                if (dump_path && *dump_path) {
                    s_dumped_vtx = 1;
                    uint8_t* rdram = app->core.RDRAM;
                    // Extended dump set: add the .data segment region around
                    // the state-setup sub-DL (0x80037658) to search for
                    // static vertex arrays.
                    for (uint32_t base : { 0x00700000u, 0x00710000u, 0x00037000u }) {
                        char path[512];
                        snprintf(path, sizeof path, "%s.0x%08X.bin", dump_path, base);
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            uint8_t buf[0x10000];
                            for (uint32_t i = 0; i < sizeof(buf); ++i) {
                                uint32_t off = base + i;
                                buf[i] = (off < 0x800000u) ? rdram[off ^ 3] : 0;
                            }
                            fwrite(buf, 1, sizeof(buf), f);
                            fclose(f);
                            fprintf(stderr, "[dump-vtxdata] wrote 64 KB to %s\n", path);
                        }
                    }
                    fflush(stderr);
                }
            }
            static int s_op_hist = -1;
            if (s_op_hist < 0) {
                const char* v = std::getenv("ROGUESQ_LOG_OPCODE_HIST");
                s_op_hist = (v && *v && v[0] != '0') ? 1 : 0;
            }
            if (s_op_hist) {
                static int s_oh_count = 0;
                int ohn = ++s_oh_count;
                const uint32_t dl_phys = (uint32_t)task->t.data_ptr & 0x3FFFFFF;
                uint8_t* rdram = app->core.RDRAM;
                uint32_t hist[256] = {0};
                uint32_t total = 0;
                // Walk up to 256 8-byte commands (or stop on G_ENDDL=0xDF or
                // a zeroed slot). Follow at most 2 G_DL jumps so we get a
                // representative sample even when Factor 5 chains via G_DL.
                uint32_t addr = dl_phys;
                uint32_t stack[16];
                int sp = 0;
                int jumps_left = 16;
                // Track first 8 instances of each "interesting" no-op'd
                // opcode so we can see if their payloads encode something
                // meaningful (like a DMA src/dst pair).
                uint64_t op02_payloads[8] = {0};
                int op02_count = 0;
                uint64_t op80_payloads[8] = {0};
                int op80_count = 0;
                for (int i = 0; i < 1024; ++i) {
                    if (addr + 8u > 0x800000u) break;
                    uint8_t op = rdram[addr ^ 3];
                    hist[op]++;
                    total++;
                    if (op == 0x02 && op02_count < 8) {
                        uint64_t w = 0;
                        for (int k = 0; k < 8; ++k) {
                            w = (w << 8) | rdram[(addr + k) ^ 3];
                        }
                        op02_payloads[op02_count++] = w;
                    }
                    if (op == 0x80 && op80_count < 8) {
                        uint64_t w = 0;
                        for (int k = 0; k < 8; ++k) {
                            w = (w << 8) | rdram[(addr + k) ^ 3];
                        }
                        op80_payloads[op80_count++] = w;
                    }
                    // F3D-style G_ENDDL = 0xB8 (Factor 5 uses F3D's 0xB8,
                    // not F3DEX2's 0xDF). Pop on B8.
                    if (op == 0xB8) {
                        if (sp > 0) { addr = stack[--sp]; continue; }
                        break;
                    }
                    if (op == 0xDF) break;
                    // F3D G_DL = 0x06, F3DEX2 G_DL = 0xDE.
                    // 0x06 in Factor 5: branch=byte1 (0=push, 1=branch).
                    if ((op == 0x06 || op == 0xDE) && jumps_left > 0) {
                        uint8_t branch = rdram[(addr + 1) ^ 3];
                        uint32_t w1 = 0;
                        for (int k = 0; k < 4; ++k) {
                            w1 = (w1 << 8) | rdram[(addr + 4 + k) ^ 3];
                        }
                        uint32_t target = w1 & 0x3FFFFFF;
                        if (branch == 0) {  // G_DL push: save return addr
                            if (sp < 16) stack[sp++] = addr + 8;
                        }
                        addr = target;
                        jumps_left--;
                        continue;
                    }
                    addr += 8;
                }
                // Dump full DL bytes for the first call so we can hand-decode
                // the attribution DL structure.
                if (ohn == 1) {
                    uint32_t a = dl_phys;
                    int dump_stack_sp = 0;
                    uint32_t dump_stack[16];
                    int dump_jumps = 16;
                    fprintf(stderr, "[opcode-hist #1 FULL DL DUMP] start=0x%08X\n", dl_phys);
                    for (int i = 0; i < 256; ++i) {
                        if (a + 8u > 0x800000u) break;
                        uint64_t w = 0;
                        for (int k = 0; k < 8; ++k) {
                            w = (w << 8) | rdram[(a + k) ^ 3];
                        }
                        uint8_t op = rdram[a ^ 3];
                        fprintf(stderr, "  %08X: %016llX  op=%02X\n",
                                a, (unsigned long long)w, op);
                        if (op == 0xB8) {
                            if (dump_stack_sp > 0) { a = dump_stack[--dump_stack_sp]; continue; }
                            break;
                        }
                        if (op == 0xDF) break;
                        if ((op == 0x06 || op == 0xDE) && dump_jumps > 0) {
                            uint8_t branch = rdram[(a + 1) ^ 3];
                            uint32_t w1 = 0;
                            for (int k = 0; k < 4; ++k) {
                                w1 = (w1 << 8) | rdram[(a + 4 + k) ^ 3];
                            }
                            uint32_t target = w1 & 0x3FFFFFF;
                            if (branch == 0) {
                                if (dump_stack_sp < 16) dump_stack[dump_stack_sp++] = a + 8;
                            }
                            a = target;
                            dump_jumps--;
                            continue;
                        }
                        a += 8;
                    }
                    fflush(stderr);
                }
                fprintf(stderr, "[opcode-hist #%d] dl=0x%08X total=%u", ohn, dl_phys, total);
                for (int op = 0; op < 256; ++op) {
                    if (hist[op]) {
                        fprintf(stderr, " %02X=%u", op, hist[op]);
                    }
                }
                fprintf(stderr, "\n");
                if (op02_count > 0) {
                    fprintf(stderr, "[opcode-hist #%d op02 payloads]", ohn);
                    for (int k = 0; k < op02_count; ++k) {
                        fprintf(stderr, " %016llX", (unsigned long long)op02_payloads[k]);
                    }
                    fprintf(stderr, "\n");
                }
                if (op80_count > 0) {
                    fprintf(stderr, "[opcode-hist #%d op80 payloads]", ohn);
                    for (int k = 0; k < op80_count; ++k) {
                        fprintf(stderr, " %016llX", (unsigned long long)op80_payloads[k]);
                    }
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
        }
        if (app) {
            static int s_count = 0;
            static uint32_t s_last_ucode = 0;
            static uint32_t s_last_data = 0;
            int n = ++s_count;
            const bool ucode_changed = (task->t.ucode != s_last_ucode) || (task->t.ucode_data != s_last_data);
            if (n <= 8 || (n & 63) == 0 || ucode_changed) {
                // Capture workload state pre- and post-DL so we can see
                // fbPair growth and presentEarly matcher activity.
                int wq_wc_pre = app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1;
                int pq_wc_pre = app->presentQueue ? (int)app->presentQueue->writeCursor : -1;
                fprintf(stderr,
                    "[hle send_dl #%d]%s type=%u ucode=0x%08X data=0x%08X dl=0x%08X size=%u wq=%d pq=%d\n",
                    n, ucode_changed ? " [UCODE-CHANGE]" : "",
                    (unsigned)task->t.type, (unsigned)task->t.ucode,
                    (unsigned)task->t.ucode_data, (unsigned)task->t.data_ptr,
                    (unsigned)task->t.data_size,
                    wq_wc_pre, pq_wc_pre);
                fflush(stderr);
            }
            s_last_ucode = task->t.ucode;
            s_last_data = task->t.ucode_data;
            if (s_hle_disabled.load(std::memory_order_relaxed)) {
                // Auto-recovery. The 3-in-a-row latch below trips on normal
                // scene transitions (cinematic→menu) where a handful of DLs
                // reference not-yet-loaded vertices and AV in drawIndexedTri.
                // Permanently disabling killed ALL later rendering → black
                // menu. Retry every 120 tasks (~2s): if the stream recovered
                // we re-enable and render; if still broken, the streak re-trips
                // and we go quiet again (cost: ~3 SEH-caught AVs per 120 frames).
                static int s_retry = 0;
                if ((++s_retry % 120) != 0) {
                    return;
                }
                s_hle_disabled.store(false, std::memory_order_relaxed);
                fprintf(stderr, "[hle send_dl] auto-recovery: re-enabling HLE submission\n");
                fflush(stderr);
            }
            app->state->rsp->reset();
            app->interpreter->loadUCodeGBI(
                task->t.ucode & 0x3FFFFFF,
                task->t.ucode_data & 0x3FFFFFF,
                true);
#ifdef _WIN32
            static std::atomic<int> s_seh_streak{0};
            __try {
                int wq_wc_pre = app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1;
                int pq_wc_pre = app->presentQueue ? (int)app->presentQueue->writeCursor : -1;
                // Material free-list integrity around RT64's DL processing —
                // tests whether the renderer (CIMG / framebuffer writeback)
                // corrupts the node pool. Gated by ROGUESQ_LOG_MATFREELIST.
                rs64_matfreelist_check(app->core.RDRAM, "RT64 send_dl PRE-processDisplayLists");
                // Strip bogus matpool-targeting SET_COLOR_IMAGE commands so
                // RT64 never registers a framebuffer over the node pool.
                // Parse input: the RDRAM snapshot taken at task start (events.cpp) when available.
                uint8_t* const parseMem = g_rs64_parse_rdram ? g_rs64_parse_rdram : app->core.RDRAM;
                rs64_neutralize_matpool_cimg(parseMem,
                                             (uint32_t)task->t.data_ptr & 0x3FFFFFF);
                const auto pdl0 = std::chrono::high_resolution_clock::now();
                app->state->RDRAM = parseMem;
                app->state->writeBackRDRAM = (parseMem != app->core.RDRAM) ? app->core.RDRAM : nullptr;
                app->processDisplayLists(parseMem,
                                         task->t.data_ptr & 0x3FFFFFF,
                                         0,
                                         /*isHLE*/ true);
                app->state->RDRAM = app->core.RDRAM;
                app->state->writeBackRDRAM = nullptr;
                g_rs64_pdl_us = (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - pdl0).count();
                // Catch any garbage CIMG-derived FBs the DL walker missed.
                // Erase them before the workload's writeback can scribble
                // over the matpool (or other heap regions).
                rs64_sanitize_fb_registry();
                rs64_matfreelist_check(app->core.RDRAM, "RT64 send_dl POST-processDisplayLists");
                s_seh_streak.store(0, std::memory_order_relaxed);
                // Post-DL: see if workload + present cursors moved. Track
                // distinct transition patterns so we can spot when pq
                // *should* be advancing but isn't.
                if (n <= 8 || (n & 63) == 0) {
                    int wq_wc_post = app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1;
                    int pq_wc_post = app->presentQueue ? (int)app->presentQueue->writeCursor : -1;
                    int wc = app->workloadQueue ? (int)app->workloadQueue->writeCursor : 0;
                    auto& wl = app->workloadQueue->workloads[wc];
                    auto& wlPrev = app->workloadQueue->workloads[(wc + app->workloadQueue->workloads.size() - 1) % app->workloadQueue->workloads.size()];
                    fprintf(stderr,
                        "[hle send_dl #%d post] wq=%d->%d pq=%d->%d nextFbPairs=%u prevFbPairs=%u\n",
                        n, wq_wc_pre, wq_wc_post, pq_wc_pre, pq_wc_post,
                        (unsigned)wl.fbPairCount, (unsigned)wlPrev.fbPairCount);
                    fflush(stderr);
                }

                // Default ON: needed for stability in cinematic phase (workload's
                // fbPairCount grows unbounded without flushing). Disable via
                // ROGUESQ_HLE_AUTO_FULLSYNC=0 to investigate cases where it might
                // double-fire and overwrite a present.
                static bool s_auto_fullsync = []() {
                    const char* v = std::getenv("ROGUESQ_HLE_AUTO_FULLSYNC");
                    if (v && v[0]) return v[0] != '0';
                    return true;
                }();
                if (app->state) {
                    // Conditional fullSync. Factor 5's cinematic DLs emit fullSync
                    // mid-DL but the post-fullSync workload accumulates fbPairs
                    // unbounded (60→229+ in one DL) without committing, eventually
                    // tripping RT64's internal limits. We flush when the current
                    // workload has uncommitted work; the gate skips the case where
                    // Factor 5's natural fullSync already advanced the cursor
                    // (next workload starts empty so needs_flush=false).
                    // Override OFF via ROGUESQ_HLE_AUTO_FULLSYNC=0 (default ON).
                    if (s_auto_fullsync) {
                        int wc = app->state->ext.workloadQueue->writeCursor;
                        auto& wl = app->state->ext.workloadQueue->workloads[wc];
                        const bool needs_flush = (wl.fbPairCount > wl.fbPairSubmitted) ||
                                                 (app->state->drawCall.triangleCount > 0);
                        if (needs_flush) {
                            app->state->dlCpuProfiler.start();
                            app->state->fullSync();
                            app->state->dlCpuProfiler.end();
                        }
                    }

                    // NOTE: a present-side sync was tried here (waitForPresentId,
                    // then workload+present waitForIdle) to remove the attribution
                    // white-screen race without the per-frame hold — NEITHER worked
                    // (still white). The race isn't captured by these RT64 queue
                    // sync points; the working fix remains the loop-top hold in
                    // rogue_squadron.toml (ROGUESQ_ATTRIB_HOLD_MS, default 50).
                }
            }
            __except (hle_seh_filter(GetExceptionInformation())) {
                // Log and try to keep going. History: latching on the FIRST
                // SEH killed the screen on transient boot AVs; then "3 in a
                // row" still tripped on the normal cinematic→menu transition
                // (a short burst of DLs reference not-yet-loaded vertices and
                // AV in drawIndexedTri) and PERMANENTLY blacked out the menu.
                // The SEH handler below independently cleans up RT64 state
                // after every AV, so consecutive AVs are no more dangerous
                // than isolated ones (the cinematic survives ~5 of them). So
                // the threshold only needs to catch a genuinely-stuck stream:
                // raise it well above any normal transition burst. Auto-
                // recovery (above) re-enables periodically as a backstop.
                constexpr int kDisableStreak = 90;
                int streak = s_seh_streak.fetch_add(1, std::memory_order_relaxed) + 1;
                fprintf(stderr, "[hle send_dl] SEH streak=%d in processDisplayLists "
                                "ucode=0x%08X data=0x%08X dl=0x%08X%s\n",
                                streak,
                                (unsigned)task->t.ucode, (unsigned)task->t.ucode_data,
                                (unsigned)task->t.data_ptr,
                                streak >= kDisableStreak ? " — HLE submission DISABLED" : "");
                fflush(stderr);
                if (streak >= kDisableStreak) {
                    s_hle_disabled.store(true, std::memory_order_relaxed);
                }
                if (app && app->state) {
                    app->state->dlCpuProfiler.startedTimestamp = RT64::Timestamp{};
                    // Clear RT64 state vectors that the AV may have left
                    // mid-operation. checkRDRAM (called from the NEXT
                    // processDisplayLists at the start of each task) asserts
                    // drawFbOperations.empty() and drawFbDiscards.empty();
                    // without clearing, that assert kills the GFX thread.
                    // Also clear differentFbs (used by checkRDRAM) and reset
                    // rdramCheckPending so checkRDRAM does NOT actually run
                    // on a partial workload — safer to skip one check than
                    // assert on inconsistent state.
                    app->state->drawFbOperations.clear();
                    app->state->drawFbDiscards.clear();
                    app->state->differentFbs.clear();
                    app->state->rdramCheckPending = false;
                }
            }
#else
            app->processDisplayLists(app->core.RDRAM,
                                     task->t.data_ptr & 0x3FFFFFF,
                                     0,
                                     /*isHLE*/ true);
#endif
        }
    }

    void update_screen() override {
        if (app) {
            // Shared RDRAM readers (big-endian word-swap: byte index ^ 3). Hoisted
            // so the buffer-arbiter driver and the gamestate poller below share one
            // copy. Guarded for null RDRAM and out-of-range addr.
            auto rb = [&](uint32_t addr) -> uint8_t {
                if (!app->core.RDRAM || addr >= 0x800000) return 0;
                return app->core.RDRAM[addr ^ 3];
            };
            auto rw = [&](uint32_t addr) -> uint32_t {
                return (uint32_t(rb(addr)) << 24) | (uint32_t(rb(addr+1)) << 16) |
                       (uint32_t(rb(addr+2)) <<  8) |  uint32_t(rb(addr+3));
            };
            auto ww = [&](uint32_t addr, uint32_t val) {
                if (!app->core.RDRAM || addr + 3 >= 0x800000) return;
                app->core.RDRAM[(addr  ) ^ 3] = uint8_t(val >> 24);
                app->core.RDRAM[(addr+1) ^ 3] = uint8_t(val >> 16);
                app->core.RDRAM[(addr+2) ^ 3] = uint8_t(val >>  8);
                app->core.RDRAM[(addr+3) ^ 3] = uint8_t(val);
            };
            // Voice-unstick watchdog. The MORT streamed-voice codec is unimplemented, so a keyed
            // voiceline's "active" byte (0x80154620) never clears when decode finishes; cutscenes that
            // wait on isSpeechSlotActive->isVoiceHandleActive wedge with the cutscene gate (0x800B0B28)
            // frozen while the VI thread keeps drawing. When the gate is stuck for N presents and a voice
            // is still "active", clear the active byte so the wait releases (each line gets a short
            // timeout). Runtime-verified: at the demo freeze gateCtr=86, speechFileLoaded=1, 0x154620=1,
            // voiceId=0x223. Gated ROGUESQ_VOICE_UNSTICK (default on), ROGUESQ_VOICE_UNSTICK_FRAMES.
            {
                // OPT-IN ONLY (default off): clearing the voice-active flags does NOT fix the demo/FrontEnd
                // cutscene freeze — it only trades a hard freeze at gateCtr=86 for the stage re-looping
                // (gate cycles 0->~86) without advancing. Root cause is the unimplemented MORT codec for
                // flag=0x01 streamed voicelines (demo line 309); a real fix needs MORT or recompiled
                // cutscene/voice-completion logic. Kept as a diagnostic lever behind ROGUESQ_VOICE_UNSTICK=1.
                static int s_vu = -1;
                if (s_vu < 0) { const char* e = std::getenv("ROGUESQ_VOICE_UNSTICK"); s_vu = (e && e[0] == '1') ? 1 : 0; }
                if (s_vu && app->core.RDRAM) {
                    static uint32_t s_lastGate = 0xFFFFFFFFu; static int s_stuck = 0;
                    const uint32_t gate = rw(0x0B0B28);
                    if (gate == s_lastGate) ++s_stuck; else { s_stuck = 0; s_lastGate = gate; }
                    if (gate < 0x100000u && s_stuck >= 8) {
                        for (int i = 0; i < 8; ++i) ww(0x139B80 + i * 4, 0xFFFFFFFFu);  // voice-handle array -> empty (-1)
                        app->core.RDRAM[0x154620 ^ 3] = 0;                              // streamed-voice active byte -> 0
                    }
                }
                // Read-only voice-state trace (ROGUESQ_VOICE_STATE_LOG=1): while the cutscene gate is stuck,
                // dump the speech-slot state table (0x80154620 region: per-slot state + 0x4638 status bytes)
                // to see which completion transition stalls for the flag=0x01 line. Pure observation.
                static int s_vs = -1;
                if (s_vs < 0) { const char* e = std::getenv("ROGUESQ_VOICE_STATE_LOG"); s_vs = (e && e[0] == '1') ? 1 : 0; }
                if (s_vs && app->core.RDRAM) {
                    static uint32_t s_lg = 0xFFFFFFFFu; static int s_sk = 0, s_ct = 0;
                    const uint32_t g = rw(0x0B0B28);
                    if (g == s_lg) ++s_sk; else { s_sk = 0; s_lg = g; }
                    if (g < 0x100000u && s_sk >= 8 && s_ct < 40) {
                        ++s_ct;
                        char b1[80] = {0}, b2[80] = {0}; int p1 = 0, p2 = 0;
                        for (int i = 0; i < 8; ++i) p1 += snprintf(b1 + p1, sizeof(b1) - p1, "%02X ", rb(0x154620 + i)); // state/active
                        for (int i = 0; i < 8; ++i) p2 += snprintf(b2 + p2, sizeof(b2) - p2, "%02X ", rb(0x154638 + i)); // status
                        const uint32_t vp = rw(0x139010);
                        fprintf(stderr, "[vstate] gate=%u  slot@154620=[%s] status@154638=[%s] handles=[%08X %08X] speechLoaded=%u voiceId=%04X\n",
                            g, b1, b2, rw(0x139B80), rw(0x139B84), rb(0xA5121),
                            ((vp >> 24) == 0x80) ? (unsigned)((rb((vp & 0xFFFFFF) + 0) << 8) | rb((vp & 0xFFFFFF) + 1)) : 0xFFFFu);
                        fflush(stderr);
                    }
                }
            }
            // ROGUESQ_FORCE_SWAP_FB=1: present the buffer the game last SWAPPED to
            // (g_last_swap_fb) rather than the raw VI_ORIGIN_REG. The post-logo
            // cinematic single-buffers (osViSwapBuffer stays on one fb) but writes
            // cycling intermediate render-pass buffers to VI_ORIGIN → the VI scans
            // out the wrong buffers → flicker. Forcing the swapped fb fixes it.
            static const bool s_force_swap = [](){ const char* v = std::getenv("ROGUESQ_FORCE_SWAP_FB"); return v && v[0] && v[0] != '0'; }();
            if (s_force_swap && g_last_swap_fb) {
                ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
                // VI_ORIGIN_REG is a PHYSICAL address — strip g_last_swap_fb's
                // KSEG0 bit (0x80000000). Writing 0x8076A500 (kseg0) made RT64
                // read it as physical → out of RDRAM → garbage FB → crash.
                if (vi) { uint32_t off = vi->VI_ORIGIN_REG & 0xFFFu; vi->VI_ORIGIN_REG = ((g_last_swap_fb & 0x03FFFFFFu) & ~0xFFFu) | off; }
            }
            // ROGUESQ_FORCE_VI_ADDR=0x795C00: pin VI scanout to a fixed RDRAM addr.
            // Diagnostic for the missing cinematic composite: the explosion is texrect-
            // rendered into offscreen 0x795C00/0x290000 (heavy draws) but VI scans the
            // lightly-drawn 0x62B800/0x695C00. Forcing VI to the heavy buffer proves
            // whether the explosion content actually lives there.
            static const uint32_t s_force_vi = [](){ const char* v = std::getenv("ROGUESQ_FORCE_VI_ADDR"); return v && v[0] ? (uint32_t)strtoul(v, nullptr, 0) : 0u; }();
            if (s_force_vi) {
                ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
                if (vi) { uint32_t off = vi->VI_ORIGIN_REG & 0xFFFu; vi->VI_ORIGIN_REG = (s_force_vi & ~0xFFFu) | off; }
            }
            // Always log first 4 + every 64th update_screen so we can tell
            // whether VI events are firing at all. The visual-output question
            // depends entirely on this being called regularly.
            static int s_n = 0;
            static bool s_filter_logged = false;
            ++s_n;

            // Watchdog moved to its own thread (rs64_cine_start_watchdog_thread)
            // because the gfx_thread can itself be deadlocked on the same mutex
            // chain that's hanging the game thread.

            // Host-side buffer-arbiter consumer, driven by the REAL present event.
            // Under HLE the game's viRetraceHandlerThread doesn't reliably advance
            // display buffers out of PRESENTED, so bufferArbiterProducerScanWait
            // (func_8000BF60) spins waiting for a slot to free. Here we advance the
            // consumer side ONLY for the slot that actually went on screen this
            // present (matched via g_last_swap_fb), cooperatively with the game
            // thread. Default ON; ROGUESQ_FORCE_BUFFER_PROGRESS=0 restores the old
            // no-op path for A/B.
            static const bool s_force_buffer = [](){
                const char *v = std::getenv("ROGUESQ_FORCE_BUFFER_PROGRESS");
                return !(v && v[0] == '0') && !rs64_vi_driven();   // the game's retrace thread does this itself when VI-driven
            }();
            if (s_force_buffer && app->core.RDRAM) {
                static const bool s_arb_dump = [](){
                    const char *v = std::getenv("ROGUESQ_DUMP_ARBITER");
                    return v && v[0] && v[0] != '0';
                }();
                const uint32_t STATE_BASE = 0x128EAA;   // per-slot state bytes
                const uint32_t COUNT_ADDR = 0x128EAD;   // slot count
                const uint32_t FBPTR_BASE = 0x128E98;   // per-slot fb pointers
                uint8_t total = rb(COUNT_ADDR);
                if (total > 0 && total <= 8 && g_last_swap_fb) {
                    // Find the slot whose fb-ptr matches the just-swapped buffer
                    // (same KSEG0/page mask as the VI_ORIGIN present above).
                    uint32_t want = (g_last_swap_fb & 0x03FFFFFFu) & ~0xFFFu;
                    int matched = -1;
                    for (uint8_t i = 0; i < total; i++) {
                        uint32_t p = rw(FBPTR_BASE + 4u * i);
                        if (((p & 0x03FFFFFFu) & ~0xFFFu) == want) { matched = (int)i; break; }
                    }
                    static int s_prev_displayed = -1;
                    static unsigned s_arb_miss = 0;
                    if (matched >= 0) {
                        // 4 PRESENTED → 5 DISPLAYED, as a CAS so it's idempotent if
                        // the game's own consumer already advanced it. Never touch
                        // 2/3 (producer-owned — bypassing them AV'd at iter 1255).
                        uint32_t maddr = STATE_BASE + (uint32_t)matched;
                        uint8_t mst = (maddr < 0x800000) ? app->core.RDRAM[maddr ^ 3] : 0xFF;
                        uint8_t pst = 0xFF;
                        bool did45 = false, did50 = false;
                        if (mst == 4) { app->core.RDRAM[maddr ^ 3] = 5; did45 = true; }
                        // The previously-displayed slot has now been scanned out of:
                        // 5 DISPLAYED → 0 FREE, and post the free-token the producer's
                        // waitOnVideoQueue (0x80128CF0) blocks on — only on a real free.
                        if (s_prev_displayed >= 0 && s_prev_displayed != matched) {
                            uint32_t paddr = STATE_BASE + (uint32_t)s_prev_displayed;
                            pst = (paddr < 0x800000) ? app->core.RDRAM[paddr ^ 3] : 0xFF;
                            if (pst == 5) {
                                app->core.RDRAM[paddr ^ 3] = 0;
                                did50 = true;
                                ultramodern::enqueue_external_message(
                                    (PTR(OSMesgQueue))0x80128CF0u, (OSMesg)0, false, false);
                            }
                        }
                        if (s_arb_dump && (++s_arb_miss & 63) == 1) {
                            fprintf(stderr, "[arbiter-host] match slot=%d st=%u prev=%d pst=%u total=%u 45=%d 50=%d\n",
                                    matched, mst, s_prev_displayed, pst, total, did45, did50);
                            fflush(stderr);
                        }
                        s_prev_displayed = matched;
                    } else if (s_arb_dump && (++s_arb_miss & 63) == 1) {
                        // No slot matches the swap fb — make the failure visible
                        // rather than silently stalling the arbiter.
                        fprintf(stderr, "[arbiter-host] no slot matches swap fb 0x%08X (miss #%u)\n",
                                (unsigned)g_last_swap_fb, s_arb_miss);
                        fflush(stderr);
                    }
                }
            }

            // ROGUESQ_CINE_FASTFWD=N (default 0/off): fast-forward the intro cutscene
            // so the game's OWN completion logic fires and it transitions to the menu.
            // The cinematic timeline clock (gateCtr 0x800B0B28) is driven by dt, which
            // cinematicComputeDt derives from the VI-retrace count 0x8011A890. Headless
            // that count crawls (~3/sec, render/arbiter-gated) so the ~1200-frame intro
            // would take ~6 min. We add N extra ticks to 0x8011A890 each present while the
            // intro cutscene is active and gateCtr < its end-frame threshold (cuts44-0xA):
            // this inflates dt so the WHOLE timeline + its action list run faster and
            // complete NATURALLY (not a skip — the game still sets its own done bits).
            // Stops boosting at the threshold so the natural end runs unperturbed.
            static const int s_cine_ffwd = [](){
                const char *v = std::getenv("ROGUESQ_CINE_FASTFWD");
                return (v && v[0]) ? std::atoi(v) : 0;
            }();
            if (s_cine_ffwd > 0 && app->core.RDRAM) {
                uint32_t cut = rw(0x0B1904);
                if (cut >= 0x80000000u && cut < 0x80800000u) {
                    uint32_t gate = rw(0x0B0B28);
                    uint32_t c44 = rw((cut - 0x80000000u) + 0x44);
                    if (c44 > 0x10 && c44 < 0x100000u && gate < (c44 - 0xA)) {
                        ww(0x11A890, rw(0x11A890) + (uint32_t)s_cine_ffwd);
                        static int s_ff_log = 0;
                        if ((s_ff_log++ & 127) == 0) {
                            fprintf(stderr, "[cine-ffwd] +%d/present gateCtr=%u/%u cutscene=0x%08X\n",
                                    s_cine_ffwd, gate, c44, cut);
                            fflush(stderr);
                        }
                    }
                }
            }
            // Game-state poller: read engine globals via RDRAM and log changes.
            // Tells us whether the game is actually progressing past boot, even
            // when the screen is black. Enable via ROGUESQ_LOG_GAMESTATE=1.
            static const bool s_log_gs = [](){
                const char *v = std::getenv("ROGUESQ_LOG_GAMESTATE");
                return v && v[0] && v[0] != '0';
            }();
            if (s_log_gs && app->core.RDRAM && (s_n & 63) == 0) {
                // Globals from docs/game-architecture.md. RDRAM is little-endian
                // word-swap relative to MIPS BE — use byte index ^ 3.
                uint8_t curLevel = rb(0x130B70);
                uint8_t curCraft = rb(0x130B41);
                uint32_t cineStatePtr = rw(0x0B0934);
                uint8_t cineStage = rb(0x0B0938);
                uint32_t curCutsceneFile = rw(0x0B1904);
                uint8_t unk20 = rb(0x130B60);  // gGameSettings+0x20 (menu sub-state)
                uint8_t unk21 = rb(0x130B61);  // gGameSettings+0x21 (cinematic sub-state)
                uint8_t unk22 = rb(0x130B62);  // gGameSettings+0x22 (mission sub-state)
                // Cinematic-completion chain (RE 2026-06-14): the intro loops in
                // mainGameLoop while screenState (0x130B14) >= 6; it only drops below 6
                // (-> menu) after gateCtr (0x0B0B28) reaches the cutscene end-frame
                // cuts44-0xA (cutscene[0x44]). Watching gateCtr vs cuts44 tells us
                // whether the timeline is advancing or stuck.
                uint8_t  screenState = rb(0x130B14);
                uint32_t gateCtr = rw(0x0B0B28);
                uint32_t cuts44 = 0;
                if (curCutsceneFile >= 0x80000000u && curCutsceneFile < 0x80800000u) {
                    cuts44 = rw((curCutsceneFile - 0x80000000u) + 0x44);
                }

                static uint8_t s_last_level = 0xFF, s_last_craft = 0xFF;
                static uint32_t s_last_cineState = 0xFFFFFFFF;
                static uint8_t s_last_cineStage = 0xFF;
                static uint32_t s_last_cutscene = 0xFFFFFFFF;
                static uint8_t s_last_u20 = 0xFF, s_last_u21 = 0xFF, s_last_u22 = 0xFF;
                static uint8_t s_last_screen = 0xFF;
                bool changed = (curLevel != s_last_level) || (curCraft != s_last_craft) ||
                               (cineStatePtr != s_last_cineState) || (cineStage != s_last_cineStage) ||
                               (curCutsceneFile != s_last_cutscene) ||
                               (unk20 != s_last_u20) || (unk21 != s_last_u21) || (unk22 != s_last_u22) ||
                               (screenState != s_last_screen);
                if (changed || (s_n & 511) == 0) {
                    fprintf(stderr,
                        "[gamestate vi=#%d] screen=%u level=%u craft=%u u20=%u u21=%u u22=%u cineState=0x%08X cineStage=%u cutscene=0x%08X gateCtr=%u cuts44=%u%s\n",
                        s_n, screenState, curLevel, curCraft, unk20, unk21, unk22,
                        cineStatePtr, cineStage, curCutsceneFile, gateCtr, cuts44,
                        changed ? " [CHANGED]" : "");
                    // Dump first 32 bytes of cutscene struct (filename field). Helps confirm WHICH cutscene.
                    if (curCutsceneFile >= 0x80000000 && curCutsceneFile < 0x80800000) {
                        uint32_t off = curCutsceneFile - 0x80000000;
                        fprintf(stderr, "  cutscene-filename: \"");
                        for (int i = 0; i < 32 && (off + i) < 0x800000; i++) {
                            uint8_t b = app->core.RDRAM[(off + i) ^ 3];
                            if (b >= 0x20 && b < 0x7F) fputc(b, stderr);
                            else if (b == 0) break;
                            else fputc('?', stderr);
                        }
                        fprintf(stderr, "\"\n");
                    }
                    fflush(stderr);
                    s_last_level = curLevel;
                    s_last_craft = curCraft;
                    s_last_cineState = cineStatePtr;
                    s_last_cineStage = cineStage;
                    s_last_cutscene = curCutsceneFile;
                    s_last_u20 = unk20;
                    s_last_u21 = unk21;
                    s_last_u22 = unk22;
                    s_last_screen = screenState;
                }
            }
            // ROGUESQ_DUMP_RDRAM_ON_SCREEN=<n|menu> / ROGUESQ_DUMP_RDRAM_AT_VI=<n>: write RDRAM
            // (un-swizzled to MIPS big-endian, same layout as a PJ64 memory dump) once, for
            // tools/validate/rdram_golden_diff.py. "menu" = first VI where screenState (0x130B14)
            // drops below 6 after the cinematic (>= 6) was seen. Path: ROGUESQ_DUMP_RDRAM_PATH
            // (default dumps/rdram_<screen|vi>_<n>.bin). "menu" = menu overlay (g_active_overlay 1)
            // resident again after the cinematic overlay (2) was seen.
            {
                static const char* s_on_screen = std::getenv("ROGUESQ_DUMP_RDRAM_ON_SCREEN");
                static const char* s_at_vi = std::getenv("ROGUESQ_DUMP_RDRAM_AT_VI");
                // ROGUESQ_DUMP_RDRAM_ON_SCENE=<id>[,settleVIs]: host g_current_scene (9 = LucasArts
                // attribution) held for settleVIs presents (default 60) so the scene is fully set up.
                static const char* s_on_scene = std::getenv("ROGUESQ_DUMP_RDRAM_ON_SCENE");
                // ROGUESQ_DUMP_RDRAM_ON_CINE_ITER=<n>: first present after cinematic loop iteration n
                // (matches the PJ64 per-frame hook in tools/validate/pj64_rs64_dump.js).
                static const char* s_on_cine = std::getenv("ROGUESQ_DUMP_RDRAM_ON_CINE_ITER");
                static const char* s_on_stall = std::getenv("ROGUESQ_DUMP_RDRAM_ON_CINE_STALL");
                static bool s_done = false, s_seen_cine = false;
                static int s_scene_held = 0;
                if (!s_done && app->core.RDRAM && ((s_on_screen && *s_on_screen) || (s_at_vi && *s_at_vi) || (s_on_scene && *s_on_scene) || (s_on_cine && *s_on_cine) || (s_on_stall && *s_on_stall))) {
                    uint8_t screenState = rb(0x130B14);
                    if (screenState >= 6 || g_active_overlay == 2) s_seen_cine = true;
                    bool fire = false; char tag[48] = {0};
                    if (s_at_vi && *s_at_vi && s_n >= std::atoi(s_at_vi)) {
                        fire = true; std::snprintf(tag, sizeof(tag), "vi_%d", s_n);
                    } else if (s_on_cine && *s_on_cine) {
                        unsigned long long it = rs64_cine_iter_get();
                        if (it >= (unsigned long long)std::atoll(s_on_cine)) {
                            fire = true; std::snprintf(tag, sizeof(tag), "cine_iter%llu", it);
                        }
                    } else if (s_on_stall && *s_on_stall) {
                        // ROGUESQ_DUMP_RDRAM_ON_CINE_STALL=<ms>: cinematic FRAME counter (game's 0x8013889C, bumped
                        // once per cinematicComputeDt) unchanged for that long; the loop may keep yielding while stuck.
                        unsigned long long it = rw(0x13889C);
                        static unsigned long long s_last_it = 0; static uint64_t s_last_ms = 0;
                        uint64_t now = GetTickCount64();
                        if (it != s_last_it) { s_last_it = it; s_last_ms = now; }
                        else if (it > 0 && s_last_ms && now - s_last_ms >= (uint64_t)std::atoll(s_on_stall)) {
                            fire = true; std::snprintf(tag, sizeof(tag), "cine_stall_iter%llu", it);
                        }
                    } else if (s_on_scene && *s_on_scene) {
                        int want = std::atoi(s_on_scene);
                        const char* comma = std::strchr(s_on_scene, ',');
                        int settle = comma ? std::atoi(comma + 1) : 60;
                        s_scene_held = (g_current_scene == want) ? s_scene_held + 1 : 0;
                        if (s_scene_held >= settle) {
                            fire = true; std::snprintf(tag, sizeof(tag), "scene_%d", want);
                        }
                    } else if (s_on_screen && *s_on_screen) {
                        // "menu[,settleVIs]": menu overlay resident after the cinematic, held for settleVIs presents
                        // (the menu's own init, e.g. the voice-pool registration, runs a few frames in).
                        bool want_menu = (s_on_screen[0] == 'm');
                        static int s_menu_held = 0;
                        const char* comma = std::strchr(s_on_screen, ',');
                        int settle = comma ? std::atoi(comma + 1) : 0;
                        bool cond = want_menu ? (s_seen_cine && g_active_overlay == 1) : (screenState == (uint8_t)std::atoi(s_on_screen));
                        s_menu_held = cond ? s_menu_held + 1 : 0;
                        if (cond && s_menu_held > settle) {
                            fire = true; std::snprintf(tag, sizeof(tag), "screen_%u", screenState);
                        }
                    }
                    if (fire) {
                        s_done = true;
                        const char* pathEnv = std::getenv("ROGUESQ_DUMP_RDRAM_PATH");
                        char path[512];
                        if (pathEnv && *pathEnv) std::snprintf(path, sizeof(path), "%s", pathEnv);
                        else std::snprintf(path, sizeof(path), "dumps/rdram_%s.bin", tag);
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            static uint8_t buf[0x10000];
                            const uint8_t* rd = app->core.RDRAM;
                            for (uint32_t base = 0; base < 0x800000u; base += sizeof(buf)) {
                                for (uint32_t i = 0; i < sizeof(buf); ++i) buf[i] = rd[(base + i) ^ 3];
                                fwrite(buf, 1, sizeof(buf), f);
                            }
                            fclose(f);
                        }
                        fprintf(stderr, "[rdram-dump] vi=#%d screen=%u -> %s (%s)\n", s_n, screenState, path, f ? "ok" : "OPEN FAILED");
                        fflush(stderr);
                    }
                }
            }
            // Log once when the SDL filter actually installs.
            if (!s_filter_logged && app->appWindow && app->appWindow->sdlEventFilterInstalled) {
                fprintf(stderr,
                    "[RT64] sdlEventFilter NOW installed at update_screen #%d (sdlWindow=%p)\n",
                    s_n, (void*)app->appWindow->sdlWindow);
                fflush(stderr);
                s_filter_logged = true;
            }
            if (s_n <= 4 || (s_n & 63) == 0) {
                ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
                // Sample 16 bytes at VI_ORIGIN to see if there's actual pixel
                // data there. If all zeros, VI is sampling an empty buffer.
                uint32_t origin = vi->VI_ORIGIN_REG & 0x00FFFFFF;
                uint32_t any_nonzero = 0;
                if (app && app->core.RDRAM && origin > 0 && origin + 16 < 0x800000) {
                    for (int i = 0; i < 16; i++) {
                        any_nonzero |= app->core.RDRAM[origin + i];
                    }
                }
                // Also log presentQueue + workloadQueue cursors so we can
                // see whether PresentEarly is pushing presents and the
                // workload worker is consuming them. If presents stay flat
                // while workloads advance, the matcher is failing.
                int pq_wc = app->presentQueue ? (int)app->presentQueue->writeCursor : -1;
                int wq_wc = app->workloadQueue ? (int)app->workloadQueue->writeCursor : -1;
                fprintf(stderr,
                    "[vi] update_screen #%d origin=0x%08X width=%u status=0x%X v_current=%u nonzero=%d fs=%u pq.wc=%d wq.wc=%d\n",
                    s_n, vi->VI_ORIGIN_REG, vi->VI_WIDTH_REG,
                    vi->VI_STATUS_REG, vi->VI_V_CURRENT_LINE_REG, any_nonzero != 0,
                    rs64_dpc_get_cumulative_fullsyncs(),
                    pq_wc, wq_wc);
                // Every 256 updates, also dump the cumulative opcode histogram.
                if ((s_n & 255) == 0) {
                    uint32_t hist[64];
                    rs64_dpc_get_cumulative_histogram(hist);
                    uint32_t total = 0;
                    for (int i = 0; i < 64; ++i) total += hist[i];
                    fprintf(stderr, "  [opcode-hist total=%u top:", total);
                    uint32_t copy[64];
                    for (int i = 0; i < 64; ++i) copy[i] = hist[i];
                    for (int slot = 0; slot < 6; ++slot) {
                        int max_idx = 0;
                        for (int i = 0; i < 64; ++i) {
                            if (copy[i] > copy[max_idx]) max_idx = i;
                        }
                        if (copy[max_idx] == 0) break;
                        fprintf(stderr, " op%02X=%u", max_idx, copy[max_idx]);
                        copy[max_idx] = 0;
                    }
                    fprintf(stderr, "]\n");
                }
                fflush(stderr);
            }
            app->updateScreen();
        }
    }

    void shutdown() override {
        if (app) {
            g_rt64_app.store(nullptr);
            app->end();
            app.reset();
        }
    }

    uint32_t get_display_framerate() const override {
        if (app && app->presentQueue) {
            return app->presentQueue->ext.sharedResources->swapChainRate;
        }
        return 60;
    }

    float get_resolution_scale() const override {
        return app ? float(app->userConfig.resolutionMultiplier) : 1.0f;
    }
};

std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window, developer_mode);
}

} // namespace recomp

// Free-function bridge for the LLE DPC pipeline (see src/rsp/dpc_bridge.cpp).
// Forwards raw RDP byte ranges from the recompiled Factor 5 ucode into the
// live RT64::Application for rasterization (isHLE=false).
namespace ultramodern {
    // Catch AVs from RT64's processDisplayLists. Currently
    // RT64::RDP::updateCallTexcoords (rt64_rdp.cpp:1343) reads
    // workload.drawData.callTiles[drawCall.tileIndex + t] before that
    // vector has been populated, when Factor 5's ucode emits a TEXRECT
    // without prior tile setup. The OOB returns garbage in release
    // builds (and our _ITERATOR_DEBUG_LEVEL=0 path) which then AVs on
    // the load. Catch the SEH exception here and skip the submission;
    // we lose that frame's content but the game continues running.
    // SEH-protected submission. RT64's processDisplayLists can AV in
    // RDP::updateCallTexcoords (rt64_rdp.cpp:1343) when Factor 5's ucode
    // emits a TEXRECT-like sequence without prior tile setup —
    // drawCall.tileCount > 0 but workload.drawData.callTiles is empty,
    // so the indexed read goes past end.
    //
    // After the AV, even if we reset dlCpuProfiler, downstream RT64 state
    // is corrupted in multiple places (next assert is at
    // rt64_framebuffer_manager.cpp:248). Rather than chase each, we
    // disable submission entirely after the first SEH. Game continues to
    // run, just without further graphics output. Better than crashing.
    // TODO: pre-validate at dpc_bridge to drop bad commands BEFORE submit.
    static std::atomic<bool> s_rdp_disabled{false};

    static void run_rdp_submission(RT64::Application* app, uint32_t lo_phys, uint32_t hi_phys) {
        if (s_rdp_disabled.load(std::memory_order_relaxed)) {
            return;
        }
#ifdef _WIN32
        __try {
            app->processDisplayLists(app->core.RDRAM, lo_phys, hi_phys, /*isHLE*/ false);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Reset the profiler timer too in case anything still tries to
            // touch it (the disable flag should prevent that, but be safe).
            if (app->state) {
                app->state->dlCpuProfiler.startedTimestamp = RT64::Timestamp{};
            }
            s_rdp_disabled.store(true, std::memory_order_relaxed);
            fprintf(stderr, "[rdp-submit] SEH in processDisplayLists "
                            "lo=0x%08X hi=0x%08X — RDP submission DISABLED for the rest "
                            "of this session (RT64 state corrupted by AV).\n",
                            lo_phys, hi_phys);
            // Dump cumulative opcode histogram so we can see what kinds of
            // commands the game submitted before the crash. Tells us
            // whether actual rendering work (TEXRECT, FILLRECT, triangles)
            // was happening.
            uint32_t hist[64];
            rs64_dpc_get_cumulative_histogram(hist);
            uint32_t total = 0;
            for (int i = 0; i < 64; ++i) total += hist[i];
            fprintf(stderr, "[rdp-submit] cumulative opcode histogram (total=%u):\n", total);
            // Print top 12 by count
            uint32_t copy[64];
            for (int i = 0; i < 64; ++i) copy[i] = hist[i];
            for (int slot = 0; slot < 12; ++slot) {
                int max_idx = 0;
                for (int i = 0; i < 64; ++i) {
                    if (copy[i] > copy[max_idx]) max_idx = i;
                }
                if (copy[max_idx] == 0) break;
                fprintf(stderr, "  op 0x%02X = %u\n", max_idx, copy[max_idx]);
                copy[max_idx] = 0;
            }
            fflush(stderr);
        }
#else
        app->processDisplayLists(app->core.RDRAM, lo_phys, hi_phys, /*isHLE*/ false);
#endif
    }

    void submit_rdp_range(uint32_t lo_phys, uint32_t hi_phys) {
        RT64::Application *app = g_rt64_app.load();
        if (app && hi_phys > lo_phys) {
            // Diagnostic: log the LAST submission's last bytes so we can see
            // what RDP commands trigger RT64's vector-OOB assert. Gate behind
            // ROGUESQ_LOG_RDP_SUBMIT=1.
            static int s_log = -1;
            if (s_log < 0) {
                const char* e = std::getenv("ROGUESQ_LOG_RDP_SUBMIT");
                s_log = (e && *e && *e != '0') ? 1 : 0;
            }
            if (s_log) {
                static int s_count = 0;
                ++s_count;
                uint32_t len = hi_phys - lo_phys;
                fprintf(stderr, "[rdp-submit #%d] lo=0x%08X hi=0x%08X len=%u\n",
                        s_count, lo_phys, hi_phys, len);
                // Dump first 32 bytes of submission (4 commands worth)
                if (len >= 8 && app->core.RDRAM) {
                    uint8_t* bytes = app->core.RDRAM + lo_phys;
                    int dump_len = (int)std::min<uint32_t>(len, 64);
                    fprintf(stderr, "  bytes: ");
                    for (int i = 0; i < dump_len; ++i) {
                        fprintf(stderr, "%02X ", bytes[i]);
                        if ((i & 7) == 7 && i + 1 < dump_len) fprintf(stderr, "\n         ");
                    }
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
            run_rdp_submission(app, lo_phys, hi_phys);
        }
    }
}

// --- RT64 tracked-framebuffer registry dump ---
// Diagnostic for the matpool corruption: lists every framebuffer RT64 has
// registered in its FramebufferManager, flagging any whose [addressStart,
// addressEnd) overlaps the matpool at [0x8015FDE0, 0x80162DE0). If the
// CIMG neutralizer reports zero hits but a framebuffer is still tracked at
// that range, the registration came from an earlier DL (stale entry that
// outlived the heap alloc that put the matpool there). Capped at 16 calls
// to keep logs short; called from rs64_matpool_repair when corruption is
// detected. The state mutex isn't held — diagnostic only, snapshot may be
// torn, but that's fine for the question we're asking.
extern "C" void rs64_dump_rt64_framebuffers(const char* where) {
    RT64::Application* app = g_rt64_app.load();
    if (!app || !app->state) return;

    static std::atomic<int> s_calls{0};
    int call = s_calls.fetch_add(1);
    if (call >= 16) return;

    constexpr uint32_t POOL_LO = 0x8015FDE0u;
    constexpr uint32_t POOL_HI = 0x80162DE0u;

    // SEH-wrap: caller is the recompiled game thread but the map is mutated
    // by RT64's own threads. A torn read shouldn't bring down the process.
#ifdef _WIN32
    __try {
#endif
        auto& fbs = app->state->framebufferManager.framebuffers;
        fprintf(stderr, "[fb-registry @%s] %zu tracked framebuffers (matpool=[0x%08X,0x%08X)):\n",
                where ? where : "?", fbs.size(), POOL_LO, POOL_HI);
        int i = 0;
        for (const auto& kv : fbs) {
            if (i++ >= 32) { fprintf(stderr, "[fb-registry]   (...more)\n"); break; }
            const RT64::Framebuffer& fb = kv.second;
            uint32_t start_n64 = fb.addressStart | 0x80000000u;
            uint32_t end_n64   = fb.addressEnd   | 0x80000000u;
            bool overlap = (end_n64 > POOL_LO) && (start_n64 < POOL_HI);
            fprintf(stderr, "[fb-registry]   key=0x%08X start=0x%08X end=0x%08X w=%u h=%u siz=%u ts=%llu%s\n",
                    (unsigned)kv.first, start_n64, end_n64,
                    (unsigned)fb.width, (unsigned)fb.height, (unsigned)fb.siz,
                    (unsigned long long)fb.lastWriteTimestamp,
                    overlap ? "   *** OVERLAPS MATPOOL ***" : "");
        }
        fflush(stderr);
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[fb-registry @%s] EXCEPTION during dump — torn read\n",
                where ? where : "?");
        fflush(stderr);
    }
#endif
}

// --- Post-processDisplayLists FB-map sanitizer ---
// Belt-and-braces complement to the pre-process DL CIMG neutralizer in
// upstream_compat.cpp: the walker can miss CIMGs that live in unwalked
// branches of the DL. If anything garbage slipped through, this catches
// it right after RT64 registers the FB but before any workload consumes
// it — same thread, same call, so the workload-processor can't have
// started yet.
//
// Real game framebuffers in this title all live in [0x4B7800, 0x800000)
// — above the heap (PJ64 dumps cross-checked) and below the 8 MB RDRAM
// ceiling. Any FB registered with a raw start < 0x400000 is Factor 5
// ucode garbage targeting low RAM (heap region); a raw start >= 0x800000
// is garbage targeting past-RDRAM host memory (which holds the recompile
// heap). Both corrupt the game's heap free-lists and freeze the cinematic
// thread later. Erasing the entry is safe: the FB hasn't been drawn into
// yet (rendering is synchronous in send_dl), so no workload data holds a
// pointer to it.
extern "C" void rs64_sanitize_fb_registry(void) {
    if (!(rs64_fb_guards_mask() & 4)) return;
    RT64::Application* app = g_rt64_app.load();
    if (!app || !app->state) return;

    static int s_logs = 0;
#ifdef _WIN32
    __try {
#endif
        auto& fbs = app->state->framebufferManager.framebuffers;
        for (auto it = fbs.begin(); it != fbs.end(); ) {
            uint32_t raw_start = it->second.addressStart;
            bool is_low  = raw_start < 0x400000u;
            bool is_high = raw_start >= 0x800000u;
            if (is_low || is_high) {
                uint32_t key       = it->first;
                uint32_t start_n64 = raw_start | 0x80000000u;
                uint32_t end_n64   = it->second.addressEnd | 0x80000000u;
                uint32_t w         = it->second.width;
                uint32_t h         = it->second.height;
                if (s_logs++ < 16) {
                    fprintf(stderr, "[fb-sanitize] erasing garbage fb key=0x%08X "
                            "start=0x%08X end=0x%08X w=%u h=%u (%s)\n",
                            key, start_n64, end_n64, w, h,
                            is_low ? "below 0x400000" : "above 0x800000");
                    fflush(stderr);
                }
                it = fbs.erase(it);
            } else {
                ++it;
            }
        }
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr, "[fb-sanitize] EXCEPTION during sweep\n");
        fflush(stderr);
    }
#endif
}
