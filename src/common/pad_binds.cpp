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
    bool car = false;
};
// D-pad left and right and the left thumb are on no action and keep their own controls
constexpr PadAction kPadActions[] = {
    {"jump", "INTERACT / RELOAD", 0},
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
    // AstonMartinPlayerDriver: Y held is the tire slasher, pressed it exits
    {"car_accelerate", "ACCELERATE", 0, true},
    {"car_brake", "BRAKE / REVERSE", 2, true},
    {"car_handbrake", "HANDBRAKE", 1, true},
    {"car_fire", "FIRE WEAPON", 7, true},
    {"car_target_lock", "TARGET LOCK", 6, true},
    {"car_cycle_weapon", "CYCLE WEAPON", 5, true},
    {"car_exit", "TIRE SLASHER / EXIT VEHICLE", 3, true},
};
constexpr size_t kCount = std::size(kPadActions);

std::mutex g_lock;
std::array<int, kCount> g_binds{};  // each action's physical button, -1 none
// what a physical button presses in play: the control plus one, -1 nothing, 0 its own control; on foot, then driving
using ButtonMap = std::array<std::atomic<int>, kPadButtons>;
std::array<ButtonMap, 2> g_maps{};
std::array<std::atomic<uint32_t>, kPorts> g_held{};  // by port, before the remap
std::array<std::atomic<bool>, kPorts> g_driving{};

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

bool CarControl(int control) {
    return std::ranges::any_of(kPadActions, [control](const PadAction& a) { return a.car && a.button == control; });
}

// the car keeps the on foot binds of the controls it does not use
bool InMap(const PadAction& action, bool car) { return car ? action.car || !CarControl(action.button) : !action.car; }

void ApplyMap(bool car) {
    ButtonMap& map = g_maps[car];
    for (int b = 0; b < kPadButtons; b++) {
        bool stock = std::ranges::any_of(kPadActions, [b, car](const PadAction& a) {
            return InMap(a, car) && a.button == b;
        });
        map[size_t(b)] = stock ? -1 : 0;
    }
    // the car's binds come last, so they win a button an on foot bind also has
    for (size_t i = 0; i < kCount; i++) {
        if (InMap(kPadActions[i], car) && g_binds[i] >= 0) map[size_t(g_binds[i])] = kPadActions[i].button + 1;
    }
}

void ApplyPadBinds() {
    ApplyMap(false);
    ApplyMap(true);
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
            const PadAction& a = kPadActions[i];
            actions[i] = {a.id, a.label, kPadNames[a.button], a.car ? "CAR" : nullptr};
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
        for (size_t i = 0; i < kCount; i++) {
            if (g_binds[i] == button && kPadActions[i].car == kPadActions[action].car) g_binds[i] = old;
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
    std::array<std::array<bool, kPadButtons>, 2> taken_by{};  // on foot, driving
    for (size_t i = 0; i < kCount; i++) {
        const PadAction& a = kPadActions[i];
        auto& taken = taken_by[a.car];
        auto found = saved.find(a.id);
        int b = found == saved.end() ? a.button : ButtonNamed(found->second);
        if (b >= 0 && taken[size_t(b)]) b = taken[size_t(a.button)] ? -1 : a.button;
        g_binds[i] = b;
        if (b >= 0) taken[size_t(b)] = true;
    }
    ApplyPadBinds();
}

void RemapPad(int port, Pad& pad) {
    uint32_t held = HeldButtons(pad);
    if (port >= 0 && port < kPorts) g_held[size_t(port)] = held;
    if (InMenu()) return;
    bool driving = port >= 0 && port < kPorts && g_driving[size_t(port)];
    const ButtonMap& map = g_maps[driving];
    Pad out{.connected = pad.connected, .lx = pad.lx, .ly = pad.ly, .rx = pad.rx, .ry = pad.ry};
    for (int b = 0; b < kPadButtons; b++) {
        int mapped = map[size_t(b)];
        if (!(held >> b & 1) || mapped < 0) continue;
        int control = mapped ? mapped - 1 : b;
        uint8_t level = b < kAnalogButtons ? pad.analog[b] : kFullPress;
        Press(out, control, level);
    }
    pad = out;
}

void SetPadDriving(int port, bool driving) {
    if (port >= 0 && port < kPorts) g_driving[size_t(port)] = driving;
}

int PressedPadButton() {
    for (const std::atomic<uint32_t>& port : g_held) {
        if (uint32_t held = port) return std::countr_zero(held);
    }
    return -1;
}

}  // namespace input
