// dpc_bridge.h — C++-side companion to the DPC bridge core (dpc_bridge.cpp).
//
// The DPC bridge forwards the Factor 5 RSP gfx ucode's raw RDP command bytes
// (written via mtc0 to DPC_START/DPC_END) into RT64's RDP interpreter.
//
// The public C-linkage entry points (rsp_dpc_submit + the DPC MMIO macros)
// live in include/rsp_dpc_macros.h, which is force-included into the
// recompiled Factor 5 ucode. This header is the C++-side companion.
#ifndef RS64_DPC_BRIDGE_H
#define RS64_DPC_BRIDGE_H

#include <cstdint>

// ---- Public C-linkage surface (called from the recompiled ucode / host) ----
extern "C" {
    // Per-task synthetic-halt reporting + counter reset (factor5_ucode).
    void rsp_task_log_and_reset(uint32_t iters, uint32_t data_size, uint32_t r17,
                                uint32_t cmd_w0, uint32_t cmd_w1);
    // Re-submit the last real FULL_SYNC so RT64 sees task-end (factor5_ucode).
    void rsp_force_fullsync();

    // Session-wide RDP opcode histogram + FULL_SYNC count (read by the SEH
    // crash dump / VI diagnostics in rt64_render_context.cpp + main.cpp).
    void     rs64_dpc_drain_histogram(uint32_t out[64]);
    void     rs64_dpc_get_cumulative_histogram(uint32_t out[64]);
    uint32_t rs64_dpc_get_cumulative_fullsyncs();

    // Engine-aware fill filter queried by RT64's framebuffer_renderer: true if
    // the fb at `addr` was last targeted by a SET_COLOR_IMAGE from inside an
    // active cinematic effect slot (so a full-screen fill on it is erasure).
    bool rt64_cine_fb_is_slot_owned(uint32_t addr);

    // GFX-opcode / G_MOVEMEM-index histograms incremented directly by the
    // factor5_ucode dispatch loop (single RSP-task thread, plain arrays).
    extern uint32_t g_task_gfx_op_count[256];
    extern uint32_t g_task_movemem_idx_count[256];
    // Currently-executing cinematic effect slot (0-5, or -1). Written by the
    // recompiled slot dispatcher; read here to annotate per-fb ownership.
    extern int g_cine_current_slot;
}

// RDP-opcode histogram (low 6 bits), incremented in the core submit path and
// drained by rs64_dpc_drain_histogram.
extern uint32_t g_task_op_count[64];

#endif // RS64_DPC_BRIDGE_H
