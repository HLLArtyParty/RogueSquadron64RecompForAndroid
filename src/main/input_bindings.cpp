#include "input_bindings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "json/json.hpp"

namespace rs64::input {

// N64 button bitmasks (mirrors main.cpp).
namespace {
constexpr uint16_t A      = 0x8000;
constexpr uint16_t B      = 0x4000;
constexpr uint16_t Z      = 0x2000;
constexpr uint16_t START  = 0x1000;
constexpr uint16_t JU     = 0x0800;
constexpr uint16_t JD     = 0x0400;
constexpr uint16_t JL     = 0x0200;
constexpr uint16_t JR     = 0x0100;
constexpr uint16_t LT     = 0x0020;
constexpr uint16_t RT     = 0x0010;
constexpr uint16_t CU     = 0x0008;
constexpr uint16_t CD     = 0x0004;
constexpr uint16_t CL     = 0x0002;
constexpr uint16_t CR     = 0x0001;

// Button targets -> N64 mask. Stick targets carry no mask (handled as axes).
uint16_t target_mask(Target t) {
    switch (t) {
        case Target::A:        return A;
        case Target::B:        return B;
        case Target::Z:        return Z;
        case Target::Start:    return START;
        case Target::DpadUp:   return JU;
        case Target::DpadDown: return JD;
        case Target::DpadLeft: return JL;
        case Target::DpadRight:return JR;
        case Target::LTrig:    return LT;
        case Target::RTrig:    return RT;
        case Target::CUp:      return CU;
        case Target::CDown:    return CD;
        case Target::CLeft:    return CL;
        case Target::CRight:   return CR;
        default:               return 0;
    }
}

constexpr float PAD_DEADZONE = 0.15f;

// Deflection [0,1] contributed by one source toward its target's direction.
float source_value(const Source& s, const RawState& st) {
    switch (s.kind) {
        case SourceKind::Key:
            if (st.keys && s.code >= 0 && s.code < st.keys_len)
                return st.keys[s.code] ? 1.0f : 0.0f;
            return 0.0f;
        case SourceKind::PadButton:
            if (st.pad && SDL_GameControllerGetButton(st.pad, (SDL_GameControllerButton)s.code))
                return 1.0f;
            return 0.0f;
        case SourceKind::PadAxis: {
            if (!st.pad) return 0.0f;
            float v = SDL_GameControllerGetAxis(st.pad, (SDL_GameControllerAxis)s.code) / 32767.0f;
            v *= (float)s.dir;                 // select requested half
            if (v <= PAD_DEADZONE) return 0.0f;
            return std::min(1.0f, (v - PAD_DEADZONE) / (1.0f - PAD_DEADZONE));
        }
        case SourceKind::MouseButton:
            return (st.mouse_buttons & SDL_BUTTON((uint32_t)s.code)) ? 1.0f : 0.0f;
        case SourceKind::MouseAxis: {
            if (!st.mouse_active) return 0.0f;
            float d = (s.code == 0) ? st.mouse_dx : st.mouse_dy;
            d *= (float)s.dir;                 // select requested half
            if (d <= 0.0f) return 0.0f;
            float m = std::min(1.0f, d);
            return st.mouse_curve != 1.0f ? std::pow(m, st.mouse_curve) : m;
        }
        default:
            return 0.0f;
    }
}
} // namespace

Bindings default_bindings() {
    Bindings b;
    auto add = [&](Target t, Source s) { b.targets[(int)t].push_back(s); };
    auto key = [](int sc) { return Source{ SourceKind::Key, sc, 0 }; };
    auto pb  = [](int c)  { return Source{ SourceKind::PadButton, c, 0 }; };
    auto pax = [](int c, int d) { return Source{ SourceKind::PadAxis, c, (int8_t)d }; };
    auto mb  = [](int c)  { return Source{ SourceKind::MouseButton, c, 0 }; };
    auto max = [](int c, int d) { return Source{ SourceKind::MouseAxis, c, (int8_t)d }; };

    // --- Keyboard ---
    // PC-port (Rogue Squadron 3D) layout, mapped through the game's default
    // "Luke" controller preset (ROM table 0x9EA18).
    add(Target::StickUp,    key(SDL_SCANCODE_UP));
    add(Target::StickDown,  key(SDL_SCANCODE_DOWN));
    add(Target::StickLeft,  key(SDL_SCANCODE_LEFT));  add(Target::StickLeft,  key(SDL_SCANCODE_A));
    add(Target::StickRight, key(SDL_SCANCODE_RIGHT)); add(Target::StickRight, key(SDL_SCANCODE_D));
    add(Target::A,     key(SDL_SCANCODE_W));        // thrust
    add(Target::A,     key(SDL_SCANCODE_RETURN));   // menu confirm
    add(Target::B,     key(SDL_SCANCODE_SPACE));    // fire blasters
    add(Target::B,     key(SDL_SCANCODE_BACKSPACE));// menu back
    add(Target::Z,     key(SDL_SCANCODE_S));        // brake
    add(Target::RTrig, key(SDL_SCANCODE_E));        // roll
    add(Target::CLeft, key(SDL_SCANCODE_LALT));     // fire secondary
    add(Target::CLeft, key(SDL_SCANCODE_RALT));
    add(Target::CDown, key(SDL_SCANCODE_X));        // fire mode
    add(Target::CRight,key(SDL_SCANCODE_F));        // special
    add(Target::Start, key(SDL_SCANCODE_ESCAPE));   // pause
    add(Target::DpadUp,   key(SDL_SCANCODE_F1));    // cockpit view
    add(Target::DpadDown, key(SDL_SCANCODE_F2));    // standard view
    add(Target::DpadRight,key(SDL_SCANCODE_F3));    // close view
    add(Target::LTrig,    key(SDL_SCANCODE_F4));    // switch view
    add(Target::CUp,      key(SDL_SCANCODE_Q));     // look around (F5 = profiler HUD host hotkey)
    add(Target::DpadLeft, key(SDL_SCANCODE_Z));     // drop camera

    // --- Mouse (flight steering) ---
    add(Target::StickRight, max(0, +1)); add(Target::StickLeft, max(0, -1));
    add(Target::StickDown,  max(1, +1)); add(Target::StickUp,   max(1, -1));  // SDL y-down -> N64 up
    add(Target::B,     mb(SDL_BUTTON_LEFT));        // fire blasters
    add(Target::CLeft, mb(SDL_BUTTON_RIGHT));       // fire secondary
    add(Target::A,     mb(SDL_BUTTON_MIDDLE));      // menu confirm

    // --- Gamepad (mirrors the prior hardcoded map) ---
    add(Target::A,     pb(SDL_CONTROLLER_BUTTON_A));
    add(Target::B,     pb(SDL_CONTROLLER_BUTTON_X));
    add(Target::Start, pb(SDL_CONTROLLER_BUTTON_START));
    add(Target::DpadUp,   pb(SDL_CONTROLLER_BUTTON_DPAD_UP));
    add(Target::DpadDown, pb(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    add(Target::DpadLeft, pb(SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    add(Target::DpadRight,pb(SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
    add(Target::LTrig, pb(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    add(Target::RTrig, pb(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    add(Target::CUp,   pb(SDL_CONTROLLER_BUTTON_Y));
    add(Target::CDown, pb(SDL_CONTROLLER_BUTTON_B));
    add(Target::CLeft, pb(SDL_CONTROLLER_BUTTON_BACK));
    add(Target::CRight,pb(SDL_CONTROLLER_BUTTON_GUIDE));
    add(Target::Z,     pax(SDL_CONTROLLER_AXIS_TRIGGERLEFT, +1));
    add(Target::StickRight, pax(SDL_CONTROLLER_AXIS_LEFTX, +1));
    add(Target::StickLeft,  pax(SDL_CONTROLLER_AXIS_LEFTX, -1));
    add(Target::StickUp,    pax(SDL_CONTROLLER_AXIS_LEFTY, -1));
    add(Target::StickDown,  pax(SDL_CONTROLLER_AXIS_LEFTY, +1));

    return b;
}

bool resolve(const Bindings& b, const RawState& s, uint16_t* buttons, float* x, float* y) {
    if (!b.keyboard_enabled && s.pad == nullptr) return false;

    RawState st = s;
    if (b.mouse_invert_x) st.mouse_dx = -st.mouse_dx;
    if (b.mouse_invert_y) st.mouse_dy = -st.mouse_dy;
    st.mouse_dx *= b.mouse_sensitivity;
    st.mouse_dy *= b.mouse_sensitivity;
    st.mouse_curve = b.mouse_curve;

    uint16_t btn = 0;
    float defl[(int)Target::Count] = {0};
    for (int t = 0; t < (int)Target::Count; ++t) {
        float v = 0.0f;
        for (const Source& src : b.targets[t]) v = std::max(v, source_value(src, st));
        defl[t] = v;
        uint16_t mask = target_mask((Target)t);
        if (mask && v > 0.5f) btn |= mask;
    }

    float sx = defl[(int)Target::StickRight] - defl[(int)Target::StickLeft];
    float sy = defl[(int)Target::StickUp]    - defl[(int)Target::StickDown];
    *x = std::clamp(sx, -1.0f, 1.0f);
    *y = std::clamp(sy, -1.0f, 1.0f);
    *buttons = btn;
    return true;
}

// --- Persistence (roguesq_input.json) --------------------------------------
namespace {
using nlohmann::json;

const char* target_name(Target t) {
    switch (t) {
        case Target::A: return "A";                 case Target::B: return "B";
        case Target::Z: return "Z";                 case Target::Start: return "Start";
        case Target::DpadUp: return "DpadUp";       case Target::DpadDown: return "DpadDown";
        case Target::DpadLeft: return "DpadLeft";   case Target::DpadRight: return "DpadRight";
        case Target::LTrig: return "LTrig";         case Target::RTrig: return "RTrig";
        case Target::CUp: return "CUp";             case Target::CDown: return "CDown";
        case Target::CLeft: return "CLeft";         case Target::CRight: return "CRight";
        case Target::StickUp: return "StickUp";     case Target::StickDown: return "StickDown";
        case Target::StickLeft: return "StickLeft"; case Target::StickRight: return "StickRight";
        default: return "";
    }
}

const char* mouse_button_name(int c) {
    switch (c) {
        case SDL_BUTTON_LEFT: return "left";   case SDL_BUTTON_MIDDLE: return "middle";
        case SDL_BUTTON_RIGHT: return "right"; case SDL_BUTTON_X1: return "x1";
        case SDL_BUTTON_X2: return "x2";       default: return "";
    }
}
int mouse_button_code(const std::string& s) {
    if (s == "left") return SDL_BUTTON_LEFT;   if (s == "middle") return SDL_BUTTON_MIDDLE;
    if (s == "right") return SDL_BUTTON_RIGHT; if (s == "x1") return SDL_BUTTON_X1;
    if (s == "x2") return SDL_BUTTON_X2;       return 0;
}

// Serialize one source to {kind, name, dir?}. Returns false if unrepresentable.
bool source_to_json(const Source& s, json& o) {
    switch (s.kind) {
        case SourceKind::Key: {
            const char* n = SDL_GetScancodeName((SDL_Scancode)s.code);
            if (!n || !n[0]) return false;
            o = { {"kind","key"}, {"name", n} };
            return true;
        }
        case SourceKind::PadButton: {
            const char* n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)s.code);
            if (!n || !n[0]) return false;
            o = { {"kind","padbutton"}, {"name", n} };
            return true;
        }
        case SourceKind::PadAxis: {
            const char* n = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)s.code);
            if (!n || !n[0]) return false;
            o = { {"kind","padaxis"}, {"name", n}, {"dir", (int)s.dir} };
            return true;
        }
        case SourceKind::MouseButton:
            o = { {"kind","mousebutton"}, {"name", mouse_button_name(s.code)} };
            return true;
        case SourceKind::MouseAxis:
            o = { {"kind","mouseaxis"}, {"name", s.code == 0 ? "x" : "y"}, {"dir", (int)s.dir} };
            return true;
        default:
            return false;
    }
}

bool source_from_json(const json& o, Source& s) {
    if (!o.is_object() || !o.contains("kind") || !o.contains("name")) return false;
    std::string kind = o["kind"].get<std::string>();
    std::string name = o["name"].get<std::string>();
    int dir = o.value("dir", 0);
    if (kind == "key") {
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        s = { SourceKind::Key, (int)sc, 0 };
    } else if (kind == "padbutton") {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name.c_str());
        if (b == SDL_CONTROLLER_BUTTON_INVALID) return false;
        s = { SourceKind::PadButton, (int)b, 0 };
    } else if (kind == "padaxis") {
        SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name.c_str());
        if (a == SDL_CONTROLLER_AXIS_INVALID) return false;
        s = { SourceKind::PadAxis, (int)a, (int8_t)dir };
    } else if (kind == "mousebutton") {
        int c = mouse_button_code(name);
        if (!c) return false;
        s = { SourceKind::MouseButton, c, 0 };
    } else if (kind == "mouseaxis") {
        s = { SourceKind::MouseAxis, (name == "y") ? 1 : 0, (int8_t)dir };
    } else {
        return false;
    }
    return true;
}
} // namespace

int target_count() { return (int)Target::Count; }

const char* target_label(Target t) { return target_name(t); }

std::string source_label(const Source& s) {
    switch (s.kind) {
        case SourceKind::Key: {
            const char* n = SDL_GetScancodeName((SDL_Scancode)s.code);
            return (n && n[0]) ? std::string(n) : std::string("key?");
        }
        case SourceKind::PadButton: {
            const char* n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)s.code);
            return std::string("pad:") + ((n && n[0]) ? n : "?");
        }
        case SourceKind::PadAxis: {
            const char* n = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)s.code);
            return std::string("pad:") + ((n && n[0]) ? n : "?") + (s.dir >= 0 ? "+" : "-");
        }
        case SourceKind::MouseButton:
            return std::string("mouse:") + mouse_button_name(s.code);
        case SourceKind::MouseAxis:
            return std::string("mouse") + (s.code == 0 ? "X" : "Y") + (s.dir >= 0 ? "+" : "-");
        default:
            return "?";
    }
}

std::string default_config_path() {
#if defined(__ANDROID__)
    if (const char* data = std::getenv("ROGUESQ_ANDROID_DATA_DIR"); data && data[0]) {
        return (std::filesystem::path(data) / "roguesq_input.json").string();
    }
#endif
    std::string dir;
    char* base = SDL_GetBasePath();
    if (base) { dir = base; SDL_free(base); }
    return dir + "roguesq_input.json";
}

bool load_bindings(Bindings& b, const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    json j;
    try { f >> j; } catch (...) { return false; }

    if (j.contains("mouse") && j["mouse"].is_object()) {
        const json& m = j["mouse"];
        b.mouse_sensitivity = m.value("sensitivity", b.mouse_sensitivity);
        b.mouse_smoothing   = m.value("smoothing", b.mouse_smoothing);
        b.mouse_curve       = m.value("curve", b.mouse_curve);
        b.mouse_invert_x    = m.value("invert_x", b.mouse_invert_x);
        b.mouse_invert_y    = m.value("invert_y", b.mouse_invert_y);
    }
    b.keyboard_enabled = j.value("keyboard_enabled", b.keyboard_enabled);

    if (j.contains("binds") && j["binds"].is_object()) {
        for (int t = 0; t < (int)Target::Count; ++t) {
            const char* name = target_name((Target)t);
            if (!j["binds"].contains(name)) continue;   // absent -> keep default
            b.targets[t].clear();
            for (const json& e : j["binds"][name]) {
                Source s;
                if (source_from_json(e, s)) b.targets[t].push_back(s);
            }
        }
    }
    return true;
}

bool save_bindings(const Bindings& b, const std::string& path) {
    json j;
    j["mouse"] = { {"sensitivity", b.mouse_sensitivity},
                   {"smoothing", b.mouse_smoothing}, {"curve", b.mouse_curve},
                   {"invert_x", b.mouse_invert_x}, {"invert_y", b.mouse_invert_y} };
    j["keyboard_enabled"] = b.keyboard_enabled;
    json binds = json::object();
    for (int t = 0; t < (int)Target::Count; ++t) {
        json arr = json::array();
        for (const Source& s : b.targets[t]) {
            json o;
            if (source_to_json(s, o)) arr.push_back(o);
        }
        binds[target_name((Target)t)] = arr;
    }
    j["binds"] = binds;

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << j.dump(2) << "\n";
    return f.good();
}

} // namespace rs64::input
