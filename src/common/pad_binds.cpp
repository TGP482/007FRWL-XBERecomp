// the controller's buttons rebound, as the keyboard's keys are
#include "common/pad_binds.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <mutex>

#include "app/app.h"
#include "common/options.h"

namespace input {

namespace {

// the physical buttons in Pad order: the analog ones, then the bits of Pad::buttons
constexpr int kAnalogButtons = 8;
constexpr int kPadButtons = 16;
const char* const kPadNames[kPadButtons] = {
    "A", "B", "X", "Y", "RB", "LB", "LT", "RT",
    "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT", "START", "BACK", "LS", "RS",
};
constexpr uint8_t kAnalogPress = 30;  // a resting trigger may read a little above 0
constexpr uint8_t kFullPress = 0xFF;
constexpr int kPorts = 4;

struct PadAction {
    const char* id;
    const char* label;
    int button;  // its control, also the button it has by default
};
// D-pad left and right and the left thumb are on no action and keep their own controls
constexpr PadAction kPadActions[] = {
    {"jump", "JUMP / INTERACT / RELOAD", 0},
    {"dive_roll", "DIVE ROLL", 1},
    {"focus", "FOCUS", 2},
    {"wall_cover", "WALL COVER / EXIT VEHICLE", 3},
    {"crouch", "CROUCH", 4},
    {"item_select", "ITEM SELECT", 5},
    {"lock_on", "LOCK ON", 6},
    {"fire", "FIRE", 7},
    {"zoom_in", "SCOPE ZOOM IN", 8},
    {"zoom_out", "SCOPE ZOOM OUT", 9},
    {"pause", "PAUSE MENU", 12},
    {"objectives", "OBJECTIVES", 13},
    {"reset_camera", "RESET CAMERA", 15},
};
constexpr size_t kCount = std::size(kPadActions);

std::mutex g_lock;
std::array<int, kCount> g_binds{};  // each action's physical button, -1 none
// what a physical button presses in play: the control plus one, -1 nothing, 0 its own control
std::array<std::atomic<int>, kPadButtons> g_map{};
std::array<std::atomic<uint32_t>, kPorts> g_held{};  // by port, before the remap

uint32_t HeldButtons(const Pad& pad) {
    uint32_t held = 0;
    for (int b = 0; b < kAnalogButtons; b++) {
        if (pad.analog[b] > kAnalogPress) held |= 1u << b;
    }
    return held | uint32_t(pad.buttons) << kAnalogButtons;
}

int ButtonNamed(const std::string& name) {
    for (int b = 0; b < kPadButtons; b++) {
        if (name == kPadNames[b]) return b;
    }
    return -1;
}

void ApplyPadBinds() {
    for (int b = 0; b < kPadButtons; b++) {
        bool stock = std::ranges::any_of(kPadActions, [b](const PadAction& a) { return a.button == b; });
        g_map[size_t(b)] = stock ? -1 : 0;
    }
    for (size_t i = 0; i < kCount; i++) {
        if (g_binds[i] >= 0) g_map[size_t(g_binds[i])] = kPadActions[i].button + 1;
    }
}

// the saved binds, stock ones left out; under g_lock
void SaveBinds() {
    auto& saved = game::Opt().pad_buttons;
    for (size_t i = 0; i < kCount; i++) {
        int b = g_binds[i];
        if (b == kPadActions[i].button) saved.erase(kPadActions[i].id);
        else saved[kPadActions[i].id] = b >= 0 ? kPadNames[b] : "";
    }
}

void Press(Pad& out, int control, uint8_t level) {
    if (control < kAnalogButtons) out.analog[control] = std::max(out.analog[control], level);
    else out.buttons |= uint16_t(1u << (control - kAnalogButtons));
}

}  // namespace

std::span<const Action> PadActions() {
    static const auto list = [] {
        std::array<Action, kCount> actions{};
        for (size_t i = 0; i < kCount; i++) {
            actions[i] = {kPadActions[i].id, kPadActions[i].label, kPadNames[kPadActions[i].button]};
        }
        return actions;
    }();
    return list;
}

std::string PadBinding(size_t action) {
    std::lock_guard lock(g_lock);
    return action < kCount && g_binds[action] >= 0 ? kPadNames[g_binds[action]] : "";
}

void BindPad(size_t action, int button) {
    if (action >= kCount || button < 0 || button >= kPadButtons) return;
    {
        std::lock_guard lock(g_lock);
        int old = g_binds[action];
        for (int& b : g_binds) {
            if (b == button) b = old;
        }
        g_binds[action] = button;
        SaveBinds();
        ApplyPadBinds();
    }
    app::Save();
}

void ResetPadBindings() {
    game::Opt().pad_buttons.clear();
    LoadPadBindings();
    app::Save();
}

// a hand edited file may give two actions one button: the later one takes its stock button if that is free, else none
void LoadPadBindings() {
    std::lock_guard lock(g_lock);
    const auto& saved = game::Opt().pad_buttons;
    std::array<bool, kPadButtons> taken{};
    for (size_t i = 0; i < kCount; i++) {
        auto found = saved.find(kPadActions[i].id);
        int b = found == saved.end() ? kPadActions[i].button : ButtonNamed(found->second);
        if (b >= 0 && taken[size_t(b)]) b = taken[size_t(kPadActions[i].button)] ? -1 : kPadActions[i].button;
        g_binds[i] = b;
        if (b >= 0) taken[size_t(b)] = true;
    }
    ApplyPadBinds();
}

void RemapPad(int port, Pad& pad) {
    uint32_t held = HeldButtons(pad);
    if (port >= 0 && port < kPorts) g_held[size_t(port)] = held;
    if (InMenu()) return;
    Pad out{.connected = pad.connected, .lx = pad.lx, .ly = pad.ly, .rx = pad.rx, .ry = pad.ry};
    for (int b = 0; b < kPadButtons; b++) {
        int mapped = g_map[size_t(b)];
        if (!(held >> b & 1) || mapped < 0) continue;
        int control = mapped ? mapped - 1 : b;
        uint8_t level = b < kAnalogButtons ? pad.analog[b] : kFullPress;
        Press(out, control, level);
    }
    pad = out;
}

int PressedPadButton() {
    for (const std::atomic<uint32_t>& port : g_held) {
        if (uint32_t held = port) return std::countr_zero(held);
    }
    return -1;
}

}  // namespace input
