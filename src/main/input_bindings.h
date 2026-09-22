#ifndef RS64_INPUT_BINDINGS_H
#define RS64_INPUT_BINDINGS_H

#include <cstdint>
#include <string>
#include <vector>

struct _SDL_GameController;

namespace rs64::input {

// N64 controller outputs, as rebindable targets. The analog stick is modeled as
// four directional targets so keys, gamepad axes, and mouse motion all bind the
// same way; x/y are reconstructed from opposing deflections in resolve().
enum class Target : int {
    A, B, Z, Start,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    LTrig, RTrig,
    CUp, CDown, CLeft, CRight,
    StickUp, StickDown, StickLeft, StickRight,
    Count
};

enum class SourceKind : uint8_t { None, Key, PadButton, PadAxis, MouseButton, MouseAxis };

// One physical input bound to a target.
//   Key:         code = SDL_Scancode
//   PadButton:   code = SDL_GameControllerButton
//   PadAxis:     code = SDL_GameControllerAxis, dir = +1/-1 selects the half-axis
//   MouseButton: code = SDL mouse button index (SDL_BUTTON_LEFT ...)
//   MouseAxis:   code = 0 (x) or 1 (y),         dir = +1/-1 selects the half-axis
struct Source {
    SourceKind kind = SourceKind::None;
    int32_t    code = 0;
    int8_t     dir  = 0;
};

struct Bindings {
    std::vector<Source> targets[(int)Target::Count];
    float mouse_sensitivity = 0.05f;  // relative-pixel -> stick deflection
    float mouse_smoothing   = 0.3f;   // 0 = raw per-frame delta, higher = softer onset/recenter (time constant, ~ms/100)
    float mouse_curve       = 1.0f;   // response exponent on [0,1] deflection; >1 eases small movements
    bool  mouse_invert_x    = false;
    bool  mouse_invert_y    = false;
    bool  keyboard_enabled  = true;   // report a controller and read the keyboard
};

// Snapshot of live device state handed to resolve() each poll.
struct RawState {
    const uint8_t*          keys         = nullptr;  // SDL_GetKeyboardState array (by scancode)
    int                     keys_len     = 0;
    _SDL_GameController*     pad          = nullptr;  // null if no gamepad
    float                   mouse_dx     = 0.0f;      // relative motion this frame (already smoothed by caller)
    float                   mouse_dy     = 0.0f;
    float                   mouse_curve  = 1.0f;       // response exponent applied to mouse-axis deflection
    uint32_t                mouse_buttons= 0;          // SDL_GetMouseState button mask
    bool                    mouse_active = false;      // relative capture engaged
};

Bindings default_bindings();

// UI helpers.
int         target_count();                    // == (int)Target::Count
const char* target_label(Target t);            // "A", "StickLeft", ...
std::string source_label(const Source& s);     // "Space", "pad:a", "mouse:left", "mouseX+"

// Resolve device state into an N64 button mask + analog x/y in [-1,1].
// Returns true if the profile is an active input source (keyboard_enabled or a
// gamepad is present), false to signal "no controller".
bool resolve(const Bindings& b, const RawState& s, uint16_t* buttons, float* x, float* y);

// roguesq_input.json next to the executable.
std::string default_config_path();

// Parse a bindings JSON file into `b`. Returns false if the file is missing or
// unparseable (b is left untouched); malformed individual sources are skipped.
bool load_bindings(Bindings& b, const std::string& path);

// Write `b` to `path` (human-readable source names). Returns false on I/O error.
bool save_bindings(const Bindings& b, const std::string& path);

} // namespace rs64::input

#endif
