// Example native-library mod for the menu button system. A mod ships a DLL whose
// exports are referenced from roguesq_menu.json with a leading '@'. Each export
// has the recomp signature void(uint8_t* rdram, recomp_context* ctx); the menu
// system calls it with a zeroed ctx and marshals via registers: a toggle getter
// returns state in r2, a slider setter reads its value from r4.
//
// A real mod would include the recomp mod SDK's recomp.h for the full
// recomp_context type. This example only needs the general-purpose register file
// prefix (r0..r31), so it defines a minimal view to stay self-contained.

#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#define MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define MOD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Prefix of recomp_context: the 32 general-purpose registers as 64-bit values.
struct RecompRegs { uint64_t r[32]; };

// librecomp checks this on load; must be 1.
MOD_EXPORT uint32_t recomp_api_version = 1;

// A custom action button behavior (referenced as "@demo_action").
MOD_EXPORT void demo_action(uint8_t* rdram, void* ctx) {
    (void)rdram; (void)ctx;
    std::fprintf(stderr, "[native-demo] demo_action fired\n");
    std::fflush(stderr);
}

// A custom toggle backed by native state ("@demo_toggle" flips, "demo_toggle_get"
// reports state in r2).
static int s_demo_on = 0;
MOD_EXPORT void demo_toggle(uint8_t* rdram, void* ctx) {
    (void)rdram; (void)ctx;
    s_demo_on = !s_demo_on;
    std::fprintf(stderr, "[native-demo] demo_toggle -> %d\n", s_demo_on);
    std::fflush(stderr);
}
MOD_EXPORT void demo_toggle_get(uint8_t* rdram, void* ctx) {
    (void)rdram;
    ((RecompRegs*)ctx)->r[2] = (uint64_t)s_demo_on;
}

// A custom slider backed by native state ("@demo_slider" sets from r4,
// "demo_slider_get" returns the value in r2).
static int s_demo_value = 50;
MOD_EXPORT void demo_slider(uint8_t* rdram, void* ctx) {
    (void)rdram;
    s_demo_value = (int)(uint32_t)((RecompRegs*)ctx)->r[4];
}
MOD_EXPORT void demo_slider_get(uint8_t* rdram, void* ctx) {
    (void)rdram;
    ((RecompRegs*)ctx)->r[2] = (uint64_t)(unsigned)s_demo_value;
}
