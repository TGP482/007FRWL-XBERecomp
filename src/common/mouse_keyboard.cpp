// the keyboard and mouse as the controller of the player port they are set to
#include "common/mouse_keyboard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "common/aspect.h"
#include "common/menu.h"
#include "common/options.h"

namespace input {

namespace {

// what an action presses on the pad
enum AnalogButton {
    kA,
    kB,
    kX,
    kY,
    kBlack,
    kWhite,
    kLeftTrigger,
    kRightTrigger,
};
enum PadButton {
    kDpadUp,
    kDpadDown,
    kDpadLeft,
    kDpadRight,
    kStart,
    kBack,
    kLeftThumb,
    kRightThumb,
};
enum Move {
    kForward,
    kBackward,
    kLeft,
    kRight,
    kMoveCount,
};
enum Look {
    kLookUp,
    kLookDown,
    kLookLeft,
    kLookRight,
    kLookCount,
};
enum class Kind { Analog, Button, Move, Look, Host, Walk };

struct Entry {
    Action action;
    Kind kind;
    int index;
};

// the game's controller layout
constexpr Entry kEntries[] = {
    {{"move_forward", "MOVE FORWARD", "W"}, Kind::Move, kForward},
    {{"move_back", "MOVE BACK", "S"}, Kind::Move, kBackward},
    {{"strafe_left", "STRAFE LEFT", "A"}, Kind::Move, kLeft},
    {{"strafe_right", "STRAFE RIGHT", "D"}, Kind::Move, kRight},
    {{"walk", "WALK", "Alt"}, Kind::Walk, 0},
    {{"jump", "JUMP", "Space"}, Kind::Analog, kA},
    {{"interact", "INTERACT / CLIMB", "F"}, Kind::Analog, kA},
    {{"reload", "RELOAD", "R"}, Kind::Analog, kA},
    {{"dive_roll", "DIVE ROLL", "Shift"}, Kind::Analog, kB},
    {{"focus", "FOCUS", "E"}, Kind::Analog, kX},
    {{"wall_cover", "WALL COVER / EXIT VEHICLE", "Ctrl"}, Kind::Analog, kY},
    {{"crouch", "CROUCH", "C"}, Kind::Analog, kBlack},
    {{"item_select", "ITEM SELECT", "Q"}, Kind::Analog, kWhite},
    {{"lock_on", "LOCK ON", "RMB"}, Kind::Analog, kLeftTrigger},
    {{"fire", "FIRE", "LMB"}, Kind::Analog, kRightTrigger},
    {{"zoom_in", "SCOPE ZOOM IN", "Z,Wheel Up"}, Kind::Button, kDpadUp},
    {{"zoom_out", "SCOPE ZOOM OUT", "X,Wheel Down"}, Kind::Button, kDpadDown},
    {{"reset_camera", "RESET CAMERA", "V"}, Kind::Button, kRightThumb},
    {{"objectives", "OBJECTIVES", "Tab"}, Kind::Button, kBack},
    {{"pause", "PAUSE MENU", "Escape"}, Kind::Button, kStart},
    {{"hide_hud", "TOGGLE HUD", ""}, Kind::Host, 0},
    {{"settings_panel", "SETTINGS PANEL", "F10"}, Kind::Host, 0},
    {{"look_up", "LOOK UP", "Up"}, Kind::Look, kLookUp},
    {{"look_down", "LOOK DOWN", "Down"}, Kind::Look, kLookDown},
    {{"look_left", "LOOK LEFT", "Left"}, Kind::Look, kLookLeft},
    {{"look_right", "LOOK RIGHT", "Right"}, Kind::Look, kLookRight},
};
constexpr size_t kCount = std::size(kEntries);

constexpr size_t ActionIndex(std::string_view id) {
    size_t i = 0;
    while (kEntries[i].action.id != id) i++;
    return i;
}
constexpr size_t kHudAction = ActionIndex("hide_hud");
constexpr size_t kPanelAction = ActionIndex("settings_panel");

constexpr size_t PauseActionIndex() {
    size_t i = 0;
    while (kEntries[i].kind != Kind::Button || kEntries[i].index != kStart) i++;
    return i;
}
constexpr size_t kPauseAction = PauseActionIndex();
constexpr int kFirstPadButton = 8;  // ButtonKey's numbering: the analog buttons, then the pad buttons

constexpr uint8_t kFullPress = 0xFF;
constexpr int16_t kFullStick = 32767;
constexpr float kPercent = 100.0f;
constexpr uint64_t kCaptureGuardMs = 250;  // the panel key waits this long after a key capture poll
constexpr size_t kMaxKeysPerAction = 2;
constexpr uint64_t kPlayMs = 250;  // play is the look hook asking for the mouse this recently
constexpr uint64_t kTapMs = 80;    // a wheel notch holds what it presses this long
constexpr int kPadStickPress = 8000;  // a controller stick past this counts as a press
constexpr uint8_t kPadAnalogPress = 30;  // a resting trigger may read a little above 0
constexpr int kPadDigitalButtons = 8;   // Pad::buttons bits in use
constexpr int kPorts = 4;
constexpr USHORT kUsagePageGeneric = 1;
constexpr USHORT kUsageMouse = 2;
constexpr uint16_t kWheelUp = 1 << kDpadUp;
constexpr uint16_t kWheelDown = 1 << kDpadDown;
// the wheel binds as two keys, codes Windows leaves unassigned
constexpr int kWheelUpKey = 0x0E;
constexpr int kWheelDownKey = 0x0F;
constexpr int kFirstVk = 1;
constexpr int kEndVk = 255;
constexpr uint32_t kAnalogShift = 16;    // a tap's analog buttons sit above its pad buttons

struct Name {
    int vk;
    const char* name;
};
// letters, digits and F keys are named by their own character; F3 stays with the host
constexpr Name kNames[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {kWheelUpKey, "Wheel Up"}, {kWheelDownKey, "Wheel Down"},
    {VK_SPACE, "Space"}, {VK_ESCAPE, "Escape"}, {VK_TAB, "Tab"}, {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"},
    {VK_MENU, "Alt"}, {VK_CAPITAL, "CapsLock"}, {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"},
    {VK_END, "End"}, {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"}, {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"},
    {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"}, {VK_DECIMAL, "NumpadPeriod"},
    {VK_OEM_3, "Backtick"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_6, "RBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Quote"},
    {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
};

constexpr int kFirstFKey = 1;
constexpr int kLastFKey = 12;
constexpr int kHostFKey = 3;

// US layout scan codes of the keys that move with the layout: a bind names the US key at that position
struct Position {
    int vk;
    UINT scan;
};
constexpr Position kPositions[] = {
    {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06}, {'6', 0x07}, {'7', 0x08}, {'8', 0x09},
    {'9', 0x0A}, {'0', 0x0B}, {VK_OEM_MINUS, 0x0C}, {VK_OEM_PLUS, 0x0D},
    {'Q', 0x10}, {'W', 0x11}, {'E', 0x12}, {'R', 0x13}, {'T', 0x14}, {'Y', 0x15}, {'U', 0x16}, {'I', 0x17},
    {'O', 0x18}, {'P', 0x19}, {VK_OEM_4, 0x1A}, {VK_OEM_6, 0x1B},
    {'A', 0x1E}, {'S', 0x1F}, {'D', 0x20}, {'F', 0x21}, {'G', 0x22}, {'H', 0x23}, {'J', 0x24}, {'K', 0x25},
    {'L', 0x26}, {VK_OEM_1, 0x27}, {VK_OEM_7, 0x28}, {VK_OEM_3, 0x29}, {VK_OEM_5, 0x2B},
    {'Z', 0x2C}, {'X', 0x2D}, {'C', 0x2E}, {'V', 0x2F}, {'B', 0x30}, {'N', 0x31}, {'M', 0x32},
    {VK_OEM_COMMA, 0x33}, {VK_OEM_PERIOD, 0x34}, {VK_OEM_2, 0x35},
};
constexpr UINT kCharBits = 0x7FFF;  // MapVirtualKeyExW's character, without the top bit that flags a dead key

using Binds = std::array<std::vector<int>, kCount>;

struct StickKeys {
    std::array<bool, kMoveCount> move{};
    std::array<bool, kLookCount> look{};
    bool walk = false;  // the move keys push the stick by the walk speed
};

std::mutex g_bind_lock;
Binds g_binds;
std::atomic<bool> g_settle{false};
std::atomic<int> g_dx{0};
std::atomic<int> g_dy{0};
std::atomic<uint64_t> g_take_ms{0};  // TakeMouse, asked every tick in play
std::array<std::atomic<uint32_t>, kPorts> g_pad_held{};  // by port, each polled on its own
std::atomic<uint64_t> g_wheel_ms[2] = {};  // the last notch up, down
std::atomic<uint64_t> g_capture_ms{0};
std::atomic<bool> g_menu{false};
std::atomic<bool> g_keyboard{true};
std::atomic<int> g_wheel{0};  // notches in the menus not yet pressed
std::atomic<uint32_t> g_press{0};
std::atomic<uint64_t> g_press_until{0};

// a wheel notch in the menus, pressed on the dpad with a release before the next; pad thread only
struct WheelStep {
    uint16_t button = 0;
    uint64_t until = 0;
};
WheelStep g_step;

// the menu page's accept and cancel as tap bits, zero outside a page
std::atomic<uint32_t> g_page_accept{0};
std::atomic<uint32_t> g_page_cancel{0};

// pad thread: the page keys in use, and Enter, Space or a pause key held since they changed
struct PageState {
    uint32_t accept = 0;
    uint32_t cancel = 0;
    bool held = false;
};
PageState g_page;

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

bool IsLetterOrDigit(int vk) { return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'); }

bool IsBindableFKey(int vk) {
    int number = vk - VK_F1 + 1;
    return number >= kFirstFKey && number <= kLastFKey && number != kHostFKey;
}

// empty for a key that cannot be bound
std::string NameOf(int vk) {
    if (IsLetterOrDigit(vk)) return std::string(1, char(vk));
    if (IsBindableFKey(vk)) return "F" + std::to_string(vk - VK_F1 + 1);
    for (const Name& n : kNames) {
        if (n.vk == vk) return n.name;
    }
    return {};
}

int Vk(const std::string& name) {
    if (name.size() == 1 && std::isalnum(uint8_t(name[0]))) return std::toupper(uint8_t(name[0]));
    for (int vk = VK_F1; vk <= VK_F12; vk++) {
        if (IsBindableFKey(vk) && _stricmp(NameOf(vk).c_str(), name.c_str()) == 0) return vk;
    }
    for (const Name& n : kNames) {
        if (_stricmp(n.name, name.c_str()) == 0) return n.vk;
    }
    return 0;
}

UINT ScanOf(int vk) {
    for (const Position& p : kPositions) {
        if (p.vk == vk) return p.scan;
    }
    return 0;
}

// the keyboard layout the player types with, switched per window thread
HKL Layout() { return GetKeyboardLayout(GetWindowThreadProcessId(HWND(app::Window()), nullptr)); }

// the key the current layout has at the bind's position
int LiveVk(int vk) {
    UINT scan = ScanOf(vk);
    if (!scan) return vk;
    UINT live = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, Layout());
    return live ? int(live) : vk;
}

// a key the window was sent, as the position bound to it
int PositionVk(int live) {
    if (!NameOf(live).empty() && !ScanOf(live)) return live;
    UINT scan = MapVirtualKeyExW(UINT(live), MAPVK_VK_TO_VSC, Layout());
    for (const Position& p : kPositions) {
        if (p.scan == scan) return p.vk;
    }
    return live;
}

// the key as the player's keyboard labels it
std::string Label(int vk) {
    int live = LiveVk(vk);
    if (live == vk) return NameOf(vk);
    if (IsLetterOrDigit(live)) return std::string(1, char(live));
    UINT c = MapVirtualKeyExW(UINT(live), MAPVK_VK_TO_CHAR, Layout()) & kCharBits;
    // a comma would split the key list
    if (c == ',') return NameOf(VK_OEM_COMMA);
    if (c > ' ' && c < 0x7F) return std::string(1, char(c));
    return NameOf(vk);
}

std::vector<int> Parse(const std::string& list) {
    std::vector<int> keys;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        if (int vk = Vk(list.substr(start, comma - start))) keys.push_back(vk);
        start = comma + 1;
    }
    return keys;
}

// the saved names, or the player's labels for the menus
std::string Join(const std::vector<int>& keys, bool labels = false) {
    std::string text;
    for (int vk : keys) {
        if (!text.empty()) text += ',';
        text += labels ? Label(vk) : NameOf(vk);
    }
    return text;
}

bool IsWheel(int vk) { return vk == kWheelUpKey || vk == kWheelDownKey; }

// a wheel key is held for one tap after its notch
bool Down(int vk) {
    if (IsWheel(vk)) return NowMs() - g_wheel_ms[vk == kWheelDownKey] < kTapMs;
    return GetAsyncKeyState(LiveVk(vk)) < 0;
}

bool IsMouseButton(int vk) {
    bool button = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    return button || IsWheel(vk);
}

bool Play() { return NowMs() - g_take_ms < kPlayMs; }

bool Foreground() { return GetForegroundWindow() == app::Window(); }

// the game window has the keyboard and the panel is closed
bool Active() { return Foreground() && !app::MenuOpen(); }

Binds CopyBinds() {
    std::lock_guard lock(g_bind_lock);
    return g_binds;
}

bool AnyDown(const std::vector<int>& keys) { return std::any_of(keys.begin(), keys.end(), Down); }

constexpr int kArrows[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};

// Enter and Space accept in the menus only; in play they do what they are bound to
bool AcceptDown() { return Down(VK_RETURN) || Down(VK_SPACE); }

// keys held when the window got the input wait for release
bool AnyPressingKeyDown(const Binds& binds) {
    bool menu_key = AcceptDown() || Down(VK_BACK) || std::any_of(std::begin(kArrows), std::end(kArrows), Down);
    if (menu_key) return true;
    return std::any_of(binds.begin(), binds.end(), [](const std::vector<int>& keys) { return AnyDown(keys); });
}

void Press(const Entry& entry, Pad& pad, StickKeys& sticks) {
    switch (entry.kind) {
        case Kind::Analog:
            pad.analog[entry.index] = kFullPress;
            break;
        case Kind::Button:
            pad.buttons |= uint16_t(1u << entry.index);
            break;
        case Kind::Move:
            sticks.move[size_t(entry.index)] = true;
            break;
        case Kind::Look:
            sticks.look[size_t(entry.index)] = true;
            break;
        case Kind::Walk:
            sticks.walk = true;
            break;
        case Kind::Host:
            break;
    }
}

uint32_t TapBits(Tap tap) {
    switch (tap) {
        case Tap::Start:
            return 1u << kStart;
        case Tap::Back:
            return 1u << kBack;
        case Tap::Up:
            return 1u << kDpadUp;
        case Tap::Down:
            return 1u << kDpadDown;
        case Tap::Left:
            return 1u << kDpadLeft;
        case Tap::Right:
            return 1u << kDpadRight;
        case Tap::A:
            return 1u << (kAnalogShift + kA);
        case Tap::B:
            return 1u << (kAnalogShift + kB);
    }
    return 0;
}

void ApplyBits(Pad& pad, uint32_t bits) {
    pad.buttons |= uint16_t(bits);
    for (int i = kA; i <= kRightTrigger; i++) {
        if (bits >> (kAnalogShift + i) & 1) pad.analog[i] = kFullPress;
    }
}

// Delete clears a binding on the options pages: bound to a move key it would move the selection off the row first.
// Out of play the wheel scrolls, and with the cursor up the mouse buttons are the cursor's.
bool GameDown(int vk, bool play) {
    if (g_menu && vk == VK_DELETE) return false;
    if (!play && IsWheel(vk)) return false;
    if (!play && g_menu && IsMouseButton(vk)) return false;
    return Down(vk);
}

bool AnyGameDown(const std::vector<int>& keys, bool play) {
    return std::any_of(keys.begin(), keys.end(), [play](int vk) { return GameDown(vk, play); });
}

// the look keys act in play only, where the arrows are not the dpad; in a menu the mouse buttons are the cursor's
// and the pause keys the page's cancel
void PressBoundKeys(const Binds& binds, bool play, Pad& pad, StickKeys& sticks) {
    bool page = g_page.cancel || g_page.held;
    for (size_t i = 0; i < kCount; i++) {
        bool blocked = (kEntries[i].kind == Kind::Look && !play) || (i == kPauseAction && page);
        bool down = AnyGameDown(binds[i], play);
        if (!blocked && down) Press(kEntries[i], pad, sticks);
    }
}

// a change of page keys waits for Enter, Space and the pause keys to be let go, so the press that opened or closed
// a page does not act on the next
void LatchPageKeys(const Binds& binds) {
    uint32_t accept = g_page_accept;
    uint32_t cancel = g_page_cancel;
    if (accept != g_page.accept || cancel != g_page.cancel) g_page = {accept, cancel, true};
    if (!AcceptDown() && !AnyDown(binds[kPauseAction])) g_page.held = false;
}

// only out of play: the arrows are the dpad, Enter and Space accept, Backspace backs out; on a menu page Enter,
// Space and the pause keys are the page's own accept and cancel
void PressMenuKeys(const Binds& binds, Pad& pad) {
    if (AcceptDown() && !g_page.held) ApplyBits(pad, g_page.accept ? g_page.accept : TapBits(Tap::A));
    if (g_page.cancel && !g_page.held && AnyDown(binds[kPauseAction])) ApplyBits(pad, g_page.cancel);
    if (Down(VK_BACK)) pad.analog[kB] = kFullPress;
    for (size_t i = 0; i < std::size(kArrows); i++) {
        int bit = kDpadUp + int(i);
        if (Down(kArrows[i])) pad.buttons |= uint16_t(1u << bit);
    }
}

// the key goes in its box and out of the other box; vk 0 empties the box
void SetSlot(std::vector<int>& keys, int vk, int slot) {
    std::array<int, kMaxKeysPerAction> slots{};
    for (size_t i = 0; i < keys.size() && i < slots.size(); i++) slots[i] = keys[i];
    size_t other = 1 - size_t(slot);
    if (vk && slots[other] == vk) slots[other] = 0;
    slots[size_t(slot)] = vk;
    keys.clear();
    for (int key : slots) {
        if (key) keys.push_back(key);
    }
}

int Axis(bool positive, bool negative) { return int(positive) - int(negative); }

void SetSticks(Pad& pad, const StickKeys& sticks) {
    float move = sticks.walk ? float(game::Opt().walk_speed) / kPercent : 1.0f;
    if (int x = Axis(sticks.move[kRight], sticks.move[kLeft])) pad.lx = int16_t(float(x) * move * kFullStick);
    if (int y = Axis(sticks.move[kForward], sticks.move[kBackward])) pad.ly = int16_t(float(y) * move * kFullStick);
    if (int x = Axis(sticks.look[kLookRight], sticks.look[kLookLeft])) pad.rx = int16_t(x * kFullStick);
    if (int y = Axis(sticks.look[kLookUp], sticks.look[kLookDown])) pad.ry = int16_t(y * kFullStick);
}

// the cursor stays inside the window while the mouse looks
void ClipToWindow() {
    HWND hwnd = HWND(app::Window());
    RECT client, current;
    GetClientRect(hwnd, &client);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
    if (!GetClipCursor(&current) || !EqualRect(&client, &current)) ClipCursor(&client);
}

void AddMotion(const RAWMOUSE& mouse) {
    if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return;
    g_dx += mouse.lLastX;
    g_dy += mouse.lLastY;
}

// a wheel notch holds its wheel key for one tap
void LatchWheelKey(const RAWMOUSE& mouse) {
    if (!(mouse.usButtonFlags & RI_MOUSE_WHEEL)) return;
    bool down = SHORT(mouse.usButtonData) < 0;
    g_wheel_ms[down] = NowMs();
}

// Windows asks for the cursor only when the mouse moves
void RefreshCursor() {
    HWND hwnd = HWND(app::Window());
    if (hwnd) PostMessageW(hwnd, WM_SETCURSOR, WPARAM(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void SetKeyboard(bool on) {
    if (g_keyboard.exchange(on) != on) RefreshCursor();
}

void AddMenuWheel(const RAWMOUSE& mouse) {
    if (!(mouse.usButtonFlags & RI_MOUSE_WHEEL)) return;
    g_wheel += SHORT(mouse.usButtonData) > 0 ? 1 : -1;
}

void PressTap(Pad& pad) {
    if (NowMs() < g_press_until) ApplyBits(pad, g_press);
}

void PressWheel(Pad& pad) {
    uint64_t now = NowMs();
    if (now < g_step.until) {
        pad.buttons |= g_step.button;
        return;
    }
    bool released = now >= g_step.until + kTapMs;
    int notches = g_wheel;
    if (!released || notches == 0) return;
    int step = notches > 0 ? 1 : -1;
    g_wheel -= step;
    g_step = {step > 0 ? kWheelUp : kWheelDown, now + kTapMs};
    pad.buttons |= g_step.button;
}

}  // namespace

void StartMouse(void* hwnd) {
    RAWINPUTDEVICE mouse{kUsagePageGeneric, kUsageMouse, 0, static_cast<HWND>(hwnd)};
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
}

void OnRawInput(void* handle) {
    RAWINPUT raw;
    UINT size = sizeof(raw);
    UINT read = GetRawInputData(static_cast<HRAWINPUT>(handle), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
    if (read == UINT(-1) || raw.header.dwType != RIM_TYPEMOUSE || !Foreground()) return;
    // noted while the settings panel has the input too, so its key capture sees the wheel
    LatchWheelKey(raw.data.mouse);
    if (!Active()) return;
    SetKeyboard(true);
    // the cursor is free outside play, where the wheel scrolls unless a key capture takes it
    if (!Play()) {
        ClipCursor(nullptr);
        if (!CaptureRecent()) AddMenuWheel(raw.data.mouse);
        return;
    }
    ClipToWindow();
    AddMotion(raw.data.mouse);
}

void ReleaseCursor() {
    ClipCursor(nullptr);
    g_settle = true;
}

std::optional<MouseMove> TakeMouse() {
    uint64_t now = NowMs();
    bool was_idle = now - g_take_ms.exchange(now) >= kPlayMs;
    int x = g_dx.exchange(0);
    int y = g_dy.exchange(0);
    if ((x == 0 && y == 0) || was_idle || !Active()) return std::nullopt;
    const game::Options& options = game::Opt();
    float sign = options.invert_mouse ? -1.0f : 1.0f;
    return MouseMove{float(x) * options.mouse_sensitivity, float(y) * sign * options.mouse_sensitivity};
}

void Keyboard(Pad& pad) {
    if (!Active()) {
        g_settle = true;
        return;
    }
    // a bind started in the game's options menu takes the next key, and the key that bound waits for release
    if (app::Capturing()) {
        g_settle = true;
        return;
    }
    Binds binds = CopyBinds();
    bool pressing = AnyPressingKeyDown(binds);
    if (g_settle && pressing) return;
    g_settle = false;
    if (pressing) SetKeyboard(true);
    bool play = Play();
    if (!play) ClipCursor(nullptr);
    if (!g_menu) g_wheel = 0;
    LatchPageKeys(binds);
    StickKeys sticks;
    PressBoundKeys(binds, play, pad, sticks);
    if (!play) PressMenuKeys(binds, pad);
    if (!play) PressTap(pad);
    if (!play && g_menu) PressWheel(pad);
    SetSticks(pad, sticks);
}

int KeyboardPort() {
    const auto& players = game::Opt().players;
    for (int port = 0; port < game::kPlayers; port++) {
        if (players[size_t(port)].keyboard) return port;
    }
    return -1;
}

// only new presses count, so a drifting stick never takes over from the keyboard and mouse
void SeenPad(int port, const Pad& pad) {
    if (port < 0 || port >= kPorts) return;
    uint32_t held = pad.buttons;
    int bit = kPadDigitalButtons;
    for (uint8_t analog : pad.analog) {
        bool pressed = analog > kPadAnalogPress;
        held |= uint32_t(pressed) << bit;
        bit++;
    }
    for (int16_t axis : {pad.lx, pad.ly, pad.rx, pad.ry}) {
        bool pressed = std::abs(int(axis)) > kPadStickPress;
        held |= uint32_t(pressed) << bit;
        bit++;
    }
    if (held & ~g_pad_held[size_t(port)].exchange(held)) SetKeyboard(false);
}

void SetMenu(bool menu) {
    if (g_menu.exchange(menu) != menu) RefreshCursor();
}

bool InMenu() { return g_menu; }

bool KeyboardActive() { return g_keyboard; }

bool LeftButton() { return Active() && !g_settle && Down(VK_LBUTTON); }

bool CursorShown() { return g_menu && g_keyboard && !Play(); }

std::optional<Pointer> Cursor() {
    HWND hwnd = HWND(app::Window());
    POINT point;
    RECT client;
    if (!Active() || !CursorShown() || !GetCursorPos(&point) || !ScreenToClient(hwnd, &point)) return std::nullopt;
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) return std::nullopt;
    // the frame is shown in the middle of the window
    float aspect = game::Aspect();
    float width = float(client.right);
    float height = float(client.bottom);
    float shown_width = std::min(width, height * aspect);
    float shown_height = shown_width / aspect;
    float x = (float(point.x) - (width - shown_width) / 2) / shown_width;
    float y = (float(point.y) - (height - shown_height) / 2) / shown_height;
    return Pointer{x, y, Down(VK_LBUTTON), Down(VK_RBUTTON)};
}

void Press(Tap tap) {
    g_press = TapBits(tap);
    g_press_until = NowMs() + kTapMs;
}

void SetPageKeys(std::optional<PageKeys> keys) {
    g_page_accept = keys ? TapBits(keys->accept) : 0;
    g_page_cancel = keys ? TapBits(keys->cancel) : 0;
}

std::string ButtonKey(int button) {
    std::lock_guard lock(g_bind_lock);
    for (size_t i = 0; i < kCount; i++) {
        const Entry& entry = kEntries[i];
        bool analog = entry.kind == Kind::Analog && entry.index == button;
        bool pad = entry.kind == Kind::Button && entry.index + kFirstPadButton == button;
        if ((analog || pad) && !g_binds[i].empty()) return Label(g_binds[i].front());
    }
    return {};
}

std::span<const Action> Actions() {
    static const std::array<Action, kCount> list = [] {
        std::array<Action, kCount> actions{};
        for (size_t i = 0; i < kCount; i++) actions[i] = kEntries[i].action;
        return actions;
    }();
    return list;
}

std::string Binding(size_t action) {
    std::lock_guard lock(g_bind_lock);
    return action < kCount ? Join(g_binds[action], true) : std::string();
}

void Bind(size_t action, int vk, int slot) {
    bool known = vk == 0 || !NameOf(vk).empty();
    if (action >= kCount || slot < 0 || slot >= int(kMaxKeysPerAction) || !known) return;
    // the host keys act on the window's key messages, which mouse buttons do not send
    if (kEntries[action].kind == Kind::Host && IsMouseButton(vk)) return;
    {
        std::lock_guard lock(g_bind_lock);
        std::vector<int> keys = g_binds[action];
        SetSlot(keys, vk, slot);
        // the settings panel with no key would leave a keyboard player no way back into it
        if (keys.empty() && action == kPanelAction) return;
        g_binds[action] = keys;
        game::Opt().keys[kEntries[action].action.id] = Join(keys);
    }
    app::Save();
}

void Unbind(size_t action) {
    if (action >= kCount || action == kPanelAction) return;
    {
        std::lock_guard lock(g_bind_lock);
        g_binds[action].clear();
        game::Opt().keys[kEntries[action].action.id].clear();
    }
    app::Save();
}

bool TakeDeletePress() {
    static bool held = false;
    bool down = Down(VK_DELETE);
    bool fresh = down && !held;
    held = down;
    return fresh && Active();
}

void ResetBindings() {
    game::Opt().keys.clear();
    LoadBindings();
    app::Save();
}

void LoadBindings() {
    std::lock_guard lock(g_bind_lock);
    const auto& saved = game::Opt().keys;
    for (size_t i = 0; i < kCount; i++) {
        auto found = saved.find(kEntries[i].action.id);
        std::string names = found != saved.end() ? found->second : kEntries[i].action.defaults;
        g_binds[i] = Parse(names);
        // an action may be left with no key, but the settings panel must keep one
        if (g_binds[i].empty() && i == kPanelAction) g_binds[i] = Parse(kEntries[i].action.defaults);
    }
}

int PollCapturedKey() {
    g_capture_ms = NowMs();
    for (int vk = kFirstVk; vk < kEndVk; vk++) {
        if (Down(vk) && !NameOf(vk).empty()) return vk;
    }
    return 0;
}

bool CaptureRecent() { return NowMs() - g_capture_ms <= kCaptureGuardMs; }

// the action is bound to this key and no key capture wants it
bool HostKeyBound(size_t action, int vk) {
    if (CaptureRecent()) return false;
    std::lock_guard lock(g_bind_lock);
    const std::vector<int>& keys = g_binds[action];
    return std::find(keys.begin(), keys.end(), PositionVk(vk)) != keys.end();
}

bool PanelKey(int vk) { return HostKeyBound(kPanelAction, vk); }

bool HudKey(int vk) { return HostKeyBound(kHudAction, vk); }

}  // namespace input
