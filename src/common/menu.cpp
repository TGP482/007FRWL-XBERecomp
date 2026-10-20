// the options panel on F10
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <imgui.h>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/monitor.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/pad_binds.h"
#include "common/translations.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};

// The monitor, display mode and resolution wait for Apply, as Windows' display settings do; closing drops them.
bool g_display_held = false;
Settings g_display;
std::string g_display_monitor;  // the held monitor, beside g_display

const char* const kPages[] = {"RECOMP SETTINGS", "MOUSE AND KEYBOARD", "CONTROLLER"};
constexpr int kPageCount = int(std::size(kPages));
constexpr int kInputPage = 1;
constexpr int kControllerPage = 2;
int g_page = 0;

// the key binding in progress: its keys must be let go before the next press binds
struct Capture {
    std::atomic<int> action{-1};  // the row being rebound, -1 when idle
    int slot = 0;  // its box: 0 the key, 1 the alternate
    bool pad = false;  // a controller button binds, not a key
    bool armed = false;
    int done_frame = 0;  // frame of the last bind, the key that bound must not start another
};
Capture g_capture;
int g_hovered_box = -1;  // the box under the mouse or focused with a key in it, this frame

// the game's own menus: grey text, the gold of a selected item and its popup's frame, black popup, steel plates
constexpr ImVec4 kPanelBg{0.03f, 0.03f, 0.03f, 0.94f};
constexpr ImVec4 kBorder{0.85f, 0.65f, 0.09f, 0.85f};
constexpr ImVec4 kText{0.80f, 0.80f, 0.80f, 1.0f};
constexpr ImVec4 kHighlight{0.25f, 0.19f, 0.03f, 1.0f};  // the selected gold at a quarter over the popup black
constexpr ImVec4 kHeading{0.91f, 0.67f, 0.02f, 1.0f};
constexpr ImVec4 kDim{0.58f, 0.59f, 0.58f, 1.0f};
constexpr ImVec4 kField{0.18f, 0.18f, 0.18f, 1.0f};
constexpr ImVec4 kFieldHovered{0.25f, 0.25f, 0.25f, 1.0f};
constexpr ImVec4 kButton{0.19f, 0.20f, 0.22f, 1.0f};
// lighter than the highlight, so it shows on a selected row
constexpr ImVec4 kButtonHovered{0.38f, 0.29f, 0.03f, 1.0f};
constexpr ImVec4 kLine{0.91f, 0.67f, 0.02f, 0.35f};

constexpr std::pair<ImGuiCol, ImVec4> kPanelColors[] = {
    {ImGuiCol_WindowBg, kPanelBg},
    {ImGuiCol_Border, kBorder},
    {ImGuiCol_Text, kText},
    {ImGuiCol_Header, kHighlight},
    {ImGuiCol_HeaderHovered, kHighlight},
    {ImGuiCol_HeaderActive, kHighlight},
    {ImGuiCol_Button, kButton},
    {ImGuiCol_ButtonHovered, kButtonHovered},
    {ImGuiCol_ButtonActive, kButtonHovered},
    {ImGuiCol_FrameBg, kField},
    {ImGuiCol_FrameBgHovered, kFieldHovered},
    {ImGuiCol_FrameBgActive, kHighlight},
    {ImGuiCol_ScrollbarGrab, kHighlight},
    {ImGuiCol_ScrollbarGrabHovered, kButtonHovered},
    {ImGuiCol_ScrollbarGrabActive, kHeading},
    {ImGuiCol_Separator, kLine},
    {ImGuiCol_NavHighlight, kHeading},
};

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"OFF", "2x", "4x", "8x"};
constexpr uint32_t kDrawDistances[] = {1, 2, 4, 8};
const char* const kDrawDistanceNames[] = {"GAME DEFAULT", "2x", "4x", "8x"};
constexpr uint32_t kAoValues[] = {0, 1, 2};
const char* const kAoNames[] = {"OFF", "SSAO", "GTAO"};
constexpr uint32_t kSkipPopups[] = {0, 1, 2};
const char* const kSkipPopupNames[] = {"OFF", "POP UPS", "ALL"};
constexpr uint32_t kFmvScales[] = {0, 1};
const char* const kFmvScaleNames[] = {"FILL", "4:3"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, kFpsCapRefresh, 0};
const char* const kDisplayModeNames[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
constexpr int kDisplayModeCount = int(std::size(kDisplayModeNames));

constexpr int kSensitivitySlots = 50;  // 0.1 to 5.0 in tenths
constexpr float kTenthsPerUnit = 10.0f;

constexpr int kRenderMin = 20;
constexpr int kRenderMax = 200;
constexpr int kRenderStep = 5;
constexpr int kRenderSlots = (kRenderMax - kRenderMin) / kRenderStep + 1;

constexpr int kWalkMin = 10;
constexpr int kWalkMax = 100;
constexpr int kWalkStep = 5;
constexpr int kWalkSlots = (kWalkMax - kWalkMin) / kWalkStep + 1;

constexpr DWORD kMinModeWidth = 640;
constexpr DWORD kMinModeBits = 32;

constexpr float kValueEms = 12.0f;  // width of the value between the arrows
constexpr float kButtonEms = 8.0f;
constexpr int kFooterButtons = 3;
constexpr int kArrowCount = 2;
constexpr float kHeadingGapEms = 0.5f;
constexpr float kHeadingAfterEms = 0.2f;
constexpr float kPaddingXEms = 1.6f;
constexpr float kPaddingYEms = 1.2f;
constexpr float kBorderSize = 2.0f;
constexpr float kWindowWidthEms = 50.0f;
constexpr float kWindowHeightEms = 32.0f;
constexpr float kRepeatSeconds = 0.1f;  // held arrows step ten times a second
constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
constexpr float kMinFontScale = 1.0f;
constexpr float kFontScaleLines = 720.0f;  // the font grows with the output height past this many lines

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

template <size_t N>
int NearestIndex(const uint32_t (&values)[N], uint32_t value) {
    int best = 0;
    uint32_t best_distance = UINT32_MAX;
    for (int i = 0; i < int(N); i++) {
        uint32_t distance = values[i] > value ? values[i] - value : value - values[i];
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

// the table index nearest to value, after stepping value to the next entry when step is not zero
template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int index = NearestIndex(values, value);
    if (step == 0) return index;
    index = StepIndex(index, step, int(N));
    value = values[index];
    return index;
}

bool SameMode(Mode a, Mode b) { return a.width == b.width && a.height == b.height; }

// every mode the monitor reports, smallest first so Left steps down; the primary's for an empty device
const std::vector<Mode>& Modes(const std::string& device) {
    static std::map<std::string, std::vector<Mode>> cache;
    // the panel and the game's options menu both ask
    std::lock_guard lock(UiLock());
    auto [it, fresh] = cache.try_emplace(device);
    std::vector<Mode>& list = it->second;
    if (!fresh) return list;
    std::wstring wide(device.begin(), device.end());
    const wchar_t* name = wide.empty() ? nullptr : wide.c_str();
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(name, i, &dm); i++) {
        if (dm.dmBitsPerPel < kMinModeBits || dm.dmPelsWidth < kMinModeWidth) continue;
        Mode mode{dm.dmPelsWidth, dm.dmPelsHeight};
        bool known = std::any_of(list.begin(), list.end(), [&](Mode other) { return SameMode(other, mode); });
        if (!known) list.push_back(mode);
    }
    auto smaller = [](Mode a, Mode b) {
        if (a.width != b.width) return a.width < b.width;
        return a.height < b.height;
    };
    std::sort(list.begin(), list.end(), smaller);
    return list;
}

// the monitor setting beside s: the held one beside the held display settings
std::string& MonitorSetting(const Settings& s) {
    if (&s == &Get()) return game::Opt().monitor;
    return g_display_monitor;
}

// the setting's monitor in the list, the primary's index when it is not connected
int MonitorIndex(const std::vector<game::Display>& list, const std::string& monitor) {
    int primary = 0;
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].device == monitor) return int(i);
        if (list[i].primary) primary = int(i);
    }
    return primary;
}

// the monitor the resolution row lists: the setting's, the primary when it is not connected
struct ModeMonitor {
    std::string device;
    Mode desktop;
};

ModeMonitor ShownMonitor(const Settings& s) {
    std::vector<game::Display> list = game::Displays();
    if (list.empty()) return {std::string(), Desktop()};
    const game::Display& d = list[size_t(MonitorIndex(list, MonitorSetting(s)))];
    return {d.device, d.desktop};
}

Mode ShownMode(const Settings& s, Mode desktop) {
    bool custom = s.width != 0 && s.height != 0;
    return custom ? Mode{s.width, s.height} : desktop;
}

int ModeIndex(const std::vector<Mode>& modes, Mode mode) {
    int index = 0;
    for (size_t i = 0; i < modes.size(); i++) {
        if (SameMode(modes[i], mode)) index = int(i);
    }
    return index;
}

// Row steps. Each function steps its setting when step is not zero, then returns the text the row shows.

std::string OnOff(bool on) { return on ? "ON" : "OFF"; }

std::string StepMonitor(Settings& s, int step) {
    std::vector<game::Display> list = game::Displays();
    if (list.empty()) return "PRIMARY";
    int current = MonitorIndex(list, MonitorSetting(s));
    int i = StepIndex(current, step, int(list.size()));
    const game::Display& d = list[size_t(i)];
    // another monitor starts on its own desktop mode
    if (step && i != current) {
        MonitorSetting(s) = d.device;
        s.width = 0;
        s.height = 0;
    }
    const char* format = d.primary ? "%d: %ux%u (PRIMARY)" : "%d: %ux%u";
    char text[64];
    std::snprintf(text, sizeof(text), format, d.number, d.desktop.width, d.desktop.height);
    return text;
}

std::string StepDisplayMode(Settings& s, int step) {
    if (step) s.display_mode = DisplayMode(StepIndex(int(s.display_mode), step, kDisplayModeCount));
    return kDisplayModeNames[std::min(int(s.display_mode), kDisplayModeCount - 1)];
}

void StepResolutionMode(Settings& s, int step) {
    ModeMonitor monitor = ShownMonitor(s);
    const std::vector<Mode>& modes = Modes(monitor.device);
    if (modes.empty()) return;
    int next = StepIndex(ModeIndex(modes, ShownMode(s, monitor.desktop)), step, int(modes.size()));
    // the desktop's own mode is kept as 0, which follows the desktop
    Mode picked = modes[size_t(next)];
    bool native = SameMode(picked, monitor.desktop);
    s.width = native ? 0 : picked.width;
    s.height = native ? 0 : picked.height;
}

std::string StepResolution(Settings& s, int step) {
    if (step) StepResolutionMode(s, step);
    Mode mode = ShownMode(s, ShownMonitor(s).desktop);
    return std::to_string(mode.width) + "x" + std::to_string(mode.height);
}

std::string StepRenderResolution(Settings& s, int step) {
    if (step) {
        int percent = std::clamp(int(s.render_percent), kRenderMin, kRenderMax);
        int slot = StepIndex((percent - kRenderMin) / kRenderStep, step, kRenderSlots);
        s.render_percent = uint32_t(kRenderMin + kRenderStep * slot);
    }
    Mode out = Output();
    std::string size = std::to_string(out.width * s.render_percent / 100) + "x";
    size += std::to_string(out.height * s.render_percent / 100);
    return std::to_string(s.render_percent) + "% (" + size + ")";
}

std::string StepVsync(Settings& s, int step) {
    if (step) s.vsync = !s.vsync;
    return OnOff(s.vsync);
}

std::string StepHdrOutput(Settings&, int step) {
    bool& on = game::Opt().hdr_output;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepMaxFps(Settings& s, int step) {
    uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
    if (cap == kFpsCapRefresh) return "REFRESH RATE";
    if (cap == 0) return "UNLIMITED";
    return std::to_string(cap);
}

std::string StepFpsCounter(Settings&, int step) {
    bool& on = game::Opt().fps_counter;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepFmvScale(Settings&, int step) {
    return kFmvScaleNames[StepTable(kFmvScales, game::Opt().fmv_scale, step)];
}

std::string StepAniso(Settings& s, int step) { return kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]; }

std::string StepMsaa(Settings&, int step) { return kMsaaNames[StepTable(kMsaaValues, game::Opt().msaa, step)]; }

std::string StepDrawDistance(Settings&, int step) {
    return kDrawDistanceNames[StepTable(kDrawDistances, game::Opt().draw_distance, step)];
}

std::string StepSmaa(Settings&, int step) {
    bool& on = game::Opt().smaa;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepAmbientOcclusion(Settings&, int step) {
    return kAoNames[StepTable(kAoValues, game::Opt().ambient_occlusion, step)];
}

std::string StepAudioOutput(Settings&, int step) {
    uint32_t& mode = game::Opt().audio_output;
    int i = StepTable(kAudioOutputs, mode, step);
    audio::SetOutputMode(mode);
    return kAudioOutputNames[i];
}

std::string StepSkipIntros(Settings&, int step) {
    bool& on = game::Opt().skip_intros;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepSkippableCutscenes(Settings&, int step) {
    bool& on = game::Opt().skippable_cutscenes;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepSkipPopups(Settings&, int step) {
    return kSkipPopupNames[StepTable(kSkipPopups, game::Opt().skip_popups, step)];
}

std::string StepPersistentBodies(Settings&, int step) {
    bool& on = game::Opt().persistent_bodies;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepInventoryPause(Settings&, int step) {
    bool& on = game::Opt().inventory_pause;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepPauseOnFocusLoss(Settings&, int step) {
    bool& on = game::Opt().pause_on_focus_loss;
    if (step) on = !on;
    return OnOff(on);
}

std::string SensitivityText(int tenths) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f", float(tenths) / kTenthsPerUnit);
    return text;
}

std::string StepSensitivity(Settings&, int step) {
    float& sensitivity = game::Opt().mouse_sensitivity;
    int tenths = int(std::lround(sensitivity * kTenthsPerUnit));
    if (step) {
        int slot = std::clamp(tenths, 1, kSensitivitySlots) - 1;
        tenths = StepIndex(slot, step, kSensitivitySlots) + 1;
        sensitivity = float(tenths) / kTenthsPerUnit;
    }
    return SensitivityText(tenths);
}

std::string StepInvertLook(Settings&, int step) {
    bool& on = game::Opt().invert_mouse;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepCameraCentring(Settings&, int step) {
    bool& on = game::Opt().camera_centring;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepFreeAim(Settings&, int step) {
    bool& on = game::Opt().free_aim;
    if (step) on = !on;
    return OnOff(on);
}

// 45 to 140 in steps of 1, 65 the game's own
constexpr int kFovMin = 45;
constexpr int kFovMax = 140;
constexpr uint32_t kGameFov = 65;

std::string StepFieldOfView(Settings&, int step) {
    uint32_t& fov = game::Opt().fov;
    if (step) {
        int slot = std::clamp(int(fov), kFovMin, kFovMax) - kFovMin;
        fov = uint32_t(kFovMin + StepIndex(slot, step, kFovMax - kFovMin + 1));
    }
    std::string text = std::to_string(fov);
    return fov == kGameFov ? text + " (DEFAULT)" : text;
}

std::string StepWalkSpeed(Settings&, int step) {
    uint32_t& speed = game::Opt().walk_speed;
    if (step) {
        int percent = std::clamp(int(speed), kWalkMin, kWalkMax);
        int slot = StepIndex((percent - kWalkMin) / kWalkStep, step, kWalkSlots);
        speed = uint32_t(kWalkMin + kWalkStep * slot);
    }
    return std::to_string(speed) + "%";
}

// INPUT DEVICES: what each player port has plugged in. A step goes to the next choice whose devices no other
// player has; player 1 never has none, since single player is driven from port 1.
constexpr game::PlayerPort kPortChoices[] = {{false, -1}, {true, 0},  {true, 1},  {true, 2},  {true, 3},
                                             {true, -1},  {false, 0}, {false, 1}, {false, 2}, {false, 3}};

std::string PortText(const game::PlayerPort& p) {
    if (p.keyboard && p.pad >= 0) return "KEYBOARD + CONTROLLER " + std::to_string(p.pad + 1);
    if (p.keyboard) return "KEYBOARD";
    if (p.pad >= 0) return "CONTROLLER " + std::to_string(p.pad + 1);
    return "NOT CONNECTED";
}

bool SamePort(const game::PlayerPort& a, const game::PlayerPort& b) {
    return a.keyboard == b.keyboard && a.pad == b.pad;
}

bool SharesDevice(const game::PlayerPort& a, const game::PlayerPort& b) {
    return (a.keyboard && b.keyboard) || (a.pad >= 0 && a.pad == b.pad);
}

bool Available(int player, const game::PlayerPort& choice) {
    bool empty = !choice.keyboard && choice.pad < 0;
    if (player == 0 && empty) return false;
    if (choice.pad >= 0 && !game::ControllerPlugged(choice.pad)) return false;
    const auto& players = game::Opt().players;
    for (int other = 0; other < game::kPlayers; other++) {
        if (other != player && SharesDevice(choice, players[size_t(other)])) return false;
    }
    return true;
}

std::string StepPlayer(int player, int step) {
    game::PlayerPort& port = game::Opt().players[size_t(player)];
    constexpr int count = int(std::size(kPortChoices));
    int index = 0;
    for (int i = 0; i < count; i++) {
        if (SamePort(kPortChoices[i], port)) index = i;
    }
    for (int tries = 1; step && tries < count; tries++) {
        index = StepIndex(index, step, count);
        if (Available(player, kPortChoices[index])) {
            port = kPortChoices[index];
            break;
        }
    }
    std::string text = PortText(port);
    bool unplugged = port.pad >= 0 && !game::ControllerPlugged(port.pad);
    return unplugged ? text + " (UNPLUGGED)" : text;
}

template <int Player>
std::string StepPlayerPort(Settings&, int step) {
    return StepPlayer(Player, step);
}

// The rows, shared with the game's own options menu. A value row's id picks its step, held rows wait for Apply.
using StepFn = std::string (*)(Settings& s, int step);

enum RowId {
    kDisplayModeRow,
    kResolutionRow,
    kRenderResolutionRow,
    kVsyncRow,
    kMaxFpsRow,
    kFpsCounterRow,
    kAnisoRow,
    kSkipIntrosRow,
    kSensitivityRow,
    kInvertLookRow,
    kCameraCentringRow,
    kAmbientOcclusionRow,
    kAudioOutputRow,
    kHdrOutputRow,
    kMsaaRow,
    kSmaaRow,
    kFieldOfViewRow,
    kFmvScaleRow,
    kWalkSpeedRow,
    kMonitorRow,
    kPauseOnFocusLossRow,
    kSkippableCutscenesRow,
    kFreeAimRow,
    kPersistentBodiesRow,
    kDrawDistanceRow,
    kInventoryPauseRow,
    kPlayer1Row,
    kPlayer2Row,
    kPlayer3Row,
    kPlayer4Row,
    kSkipPopupsRow,
};

// in RowId order
constexpr StepFn kSteps[] = {
    StepDisplayMode,
    StepResolution,
    StepRenderResolution,
    StepVsync,
    StepMaxFps,
    StepFpsCounter,
    StepAniso,
    StepSkipIntros,
    StepSensitivity,
    StepInvertLook,
    StepCameraCentring,
    StepAmbientOcclusion,
    StepAudioOutput,
    StepHdrOutput,
    StepMsaa,
    StepSmaa,
    StepFieldOfView,
    StepFmvScale,
    StepWalkSpeed,
    StepMonitor,
    StepPauseOnFocusLoss,
    StepSkippableCutscenes,
    StepFreeAim,
    StepPersistentBodies,
    StepDrawDistance,
    StepInventoryPause,
    StepPlayerPort<0>,
    StepPlayerPort<1>,
    StepPlayerPort<2>,
    StepPlayerPort<3>,
    StepSkipPopups,
};

bool Held(int id) { return id == kMonitorRow || id == kDisplayModeRow || id == kResolutionRow; }

// shown dimmed and left as it is: HDR output without an HDR display
bool Greyed(int id) { return id == kHdrOutputRow && !game::g_hdr_display; }

using rows::Kind;
using rows::Row;

constexpr Row kSettingsRows[] = {
    {Kind::Heading, -1, "DISPLAY"},
    {Kind::Value, kMonitorRow, "MONITOR"},
    {Kind::Value, kDisplayModeRow, "DISPLAY MODE"},
    {Kind::Value, kResolutionRow, "RESOLUTION"},
    {Kind::Value, kRenderResolutionRow, "RENDER RESOLUTION"},
    {Kind::Value, kFieldOfViewRow, "FIELD OF VIEW"},
    {Kind::Value, kVsyncRow, "VERTICAL SYNC"},
    {Kind::Value, kHdrOutputRow, "HDR OUTPUT"},
    {Kind::Value, kMaxFpsRow, "MAX FRAME RATE"},
    {Kind::Value, kFpsCounterRow, "FPS COUNTER"},
    {Kind::Value, kFmvScaleRow, "FMV SCALE"},
    {Kind::Heading, -1, "GRAPHICS"},
    {Kind::Value, kMsaaRow, "MSAA"},
    {Kind::Value, kSmaaRow, "SMAA"},
    {Kind::Value, kAnisoRow, "ANISOTROPIC FILTERING"},
    {Kind::Value, kDrawDistanceRow, "DRAW DISTANCE"},
    {Kind::Value, kAmbientOcclusionRow, "AMBIENT OCCLUSION"},
    {Kind::Heading, -1, "GENERAL"},
    {Kind::Value, kSkipIntrosRow, "SKIP INTROS"},
    {Kind::Value, kSkippableCutscenesRow, "SKIPPABLE CUTSCENES"},
    {Kind::Value, kSkipPopupsRow, "SKIP POP UPS"},
    {Kind::Value, kPersistentBodiesRow, "PERSISTENT BODIES"},
    {Kind::Value, kInventoryPauseRow, "INVENTORY PAUSE"},
    {Kind::Value, kPauseOnFocusLossRow, "PAUSE ON FOCUS LOSS"},
    {Kind::Value, kAudioOutputRow, "AUDIO OUTPUT"},
    {Kind::Heading, -1, "INPUT DEVICES"},
    {Kind::Value, kPlayer1Row, "PLAYER 1"},
    {Kind::Value, kPlayer2Row, "PLAYER 2"},
    {Kind::Value, kPlayer3Row, "PLAYER 3"},
    {Kind::Value, kPlayer4Row, "PLAYER 4"},
};

// the mouse rows, then a bind row for each input action
const std::vector<Row>& InputRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {
            {Kind::Heading, -1, "MOUSE"},
            {Kind::Value, kSensitivityRow, "MOUSE SENSITIVITY"},
            {Kind::Value, kInvertLookRow, "INVERT LOOK"},
            {Kind::Value, kCameraCentringRow, "CAMERA CENTRING"},
            {Kind::Value, kFreeAimRow, "FREE AIM"},
            {Kind::Heading, -1, "KEYBOARD"},
        };
        std::span<const input::Action> actions = input::Actions();
        const char* section = nullptr;
        for (size_t i = 0; i < actions.size(); i++) {
            // the on foot keys end with the walk speed, then each section under its heading
            if (actions[i].section != section) {
                if (!section) out.push_back({Kind::Value, kWalkSpeedRow, "WALK SPEED"});
                section = actions[i].section;
                out.push_back({Kind::Heading, -1, section});
            }
            out.push_back({Kind::Bind, int(i), actions[i].label});
        }
        if (!section) out.push_back({Kind::Value, kWalkSpeedRow, "WALK SPEED"});
        return out;
    }();
    return list;
}

// a bind row for each controller action
const std::vector<Row>& ControllerRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "CONTROLLER"}};
        std::span<const input::Action> actions = input::PadActions();
        for (size_t i = 0; i < actions.size(); i++) out.push_back({Kind::PadBind, int(i), actions[i].label});
        return out;
    }();
    return list;
}

bool SameDisplay(const Settings& a, const Settings& b) {
    bool same_monitor = MonitorSetting(a) == MonitorSetting(b);
    return same_monitor && a.display_mode == b.display_mode && a.width == b.width && a.height == b.height;
}

void CopyDisplay(Settings& to, const Settings& from) {
    MonitorSetting(to) = MonitorSetting(from);
    to.display_mode = from.display_mode;
    to.width = from.width;
    to.height = from.height;
}

void ApplyLiveStep(const Row& row, int step) {
    kSteps[row.id](Get(), step);
    Save();
}

void ApplyHeldStep(const Row& row, int step) {
    const Settings& live = Get();
    if (!g_display_held) CopyDisplay(g_display, live);
    kSteps[row.id](g_display, step);
    g_display_held = !SameDisplay(g_display, live);
}

void Apply() {
    CopyDisplay(Get(), g_display);
    g_display_held = false;
    ApplyDisplay();
    Save();
}

void RestoreSettingsDefaults() {
    Settings& s = Get();
    std::string join_ip = s.join_ip;
    s = Settings{};
    s.join_ip = join_ip;
    g_display_held = false;
    ApplyDisplay();
    Save();
}

// Drawing. A row has the label left, then an arrow either side of the value on the right.

struct RowLayout {
    ImVec2 origin;
    float width;
    float height;
    float value_width;
    float block_x;  // left edge of the first arrow
    float value_x;
    float inc_x;    // the second arrow
};

RowLayout MeasureRow() {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowLayout row;
    row.origin = ImGui::GetCursorPos();
    row.width = ImGui::GetContentRegionAvail().x;
    row.height = ImGui::GetFrameHeight();
    row.value_width = ImGui::GetFontSize() * kValueEms;
    float block_width = row.height * kArrowCount + row.value_width + style.ItemSpacing.x * kArrowCount;
    row.block_x = row.origin.x + row.width - block_width;
    row.value_x = row.block_x + row.height + style.ItemSpacing.x;
    row.inc_x = row.value_x + row.value_width + style.ItemSpacing.x;
    return row;
}

// the row is the only nav stop, filled while focused; true when it was pressed
bool DrawNavStop(const RowLayout& row, bool highlight) {
    bool pressed = ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(row.width, row.height));
    if (highlight || ImGui::IsItemFocused()) {
        ImU32 fill = ImGui::GetColorU32(ImGuiCol_Header);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), fill);
    }
    return pressed;
}

// the focused row steps on the left and right keys
int FocusedStep(const RowLayout& row) {
    DrawNavStop(row, false);
    if (!ImGui::IsItemFocused()) return 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) return -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) return 1;
    return 0;
}

void DrawLabel(const RowLayout& row, const char* label) {
    ImGui::SetCursorPos(ImVec2(row.origin.x + ImGui::GetStyle().ItemInnerSpacing.x, row.origin.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Ui(label).c_str());
}

void DrawValueText(const RowLayout& row, const char* value) {
    float centred = (row.value_width - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(row.value_x + std::max(centred, 0.0f), row.origin.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);
}

// the arrows keep stepping while held
int DrawValueBlock(const RowLayout& row, const char* value) {
    int step = 0;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);
    ImGui::SetCursorPos(ImVec2(row.block_x, row.origin.y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    DrawValueText(row, value);
    ImGui::SetCursorPos(ImVec2(row.inc_x, row.origin.y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    return step;
}

// closes the PushID a row opened and moves past the row
void EndRow(const RowLayout& row) {
    ImGui::PopID();
    ImGui::SetCursorPos(row.origin);
    ImGui::Dummy(ImVec2(row.width, row.height));
}

// the step asked for this frame, 0 for none
int OptionRow(const char* label, const char* value) {
    RowLayout row = MeasureRow();
    ImGui::PushID(label);
    int key_step = FocusedStep(row);
    DrawLabel(row, label);
    int arrow_step = DrawValueBlock(row, value);
    EndRow(row);
    return arrow_step ? arrow_step : key_step;
}

void DrawHeading(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * kHeadingGapEms));
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(Ui(title).c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * kHeadingAfterEms));
}

void DrawValueRow(const Row& row) {
    bool held = Held(row.id);
    bool greyed = Greyed(row.id);
    Settings& shown = held && g_display_held ? g_display : Get();
    std::string value = greyed ? "NOT SUPPORTED" : kSteps[row.id](shown, 0);
    ImGui::BeginDisabled(greyed);
    int step = OptionRow(row.label, Ui(value).c_str());
    ImGui::EndDisabled();
    if (step == 0 || greyed) return;
    if (held) {
        ApplyHeldStep(row, step);
    } else {
        ApplyLiveStep(row, step);
    }
}

// Key bindings.

constexpr const char* kUnbound = "UNBOUND";

std::string KeyText(std::string keys) {
    if (keys.empty()) return kUnbound;
    for (char& c : keys) c = c == ',' ? '/' : char(std::toupper(uint8_t(c)));
    return keys;
}

void CancelCapture() { g_capture.action = -1; }

void StartCapture(int action, int slot, bool pad = false) {
    g_capture.action = action;
    g_capture.slot = slot;
    g_capture.pad = pad;
    g_capture.armed = false;
}

void EndCapture() {
    CancelCapture();
    g_capture.done_frame = ImGui::GetFrameCount();
}

// a row or a tab was just bound or chosen: nothing may start on the frame after a bind
bool BindingSettled() { return !Capturing() && ImGui::GetFrameCount() > g_capture.done_frame + 1; }

// Escape cancels, a controller button binds once every button is let go
void PollPadCapture(int vk) {
    int button = input::PressedPadButton();
    if (vk == VK_ESCAPE) {
        EndCapture();
        return;
    }
    if (!g_capture.armed) {
        g_capture.armed = button < 0;
        return;
    }
    if (button < 0) return;
    input::BindPad(size_t(g_capture.action.load()), button);
    EndCapture();
}

// Escape cancels, any other bindable key or mouse button binds
void PollCapture() {
    if (!Capturing()) return;
    int vk = input::PollCapturedKey();
    if (g_capture.pad) return PollPadCapture(vk);
    if (!g_capture.armed) {
        g_capture.armed = vk == 0;
        return;
    }
    if (vk == 0) return;
    if (vk != VK_ESCAPE) input::Bind(size_t(g_capture.action.load()), vk, g_capture.slot);
    EndCapture();
}

// the action's key and its alternate as their boxes show them, the alternate blank when there is none
std::vector<std::string> KeyBoxes(size_t action) {
    std::string keys = input::Binding(action);
    size_t comma = keys.find(',');
    std::string second;
    if (comma != std::string::npos) second = keys.substr(comma + 1);
    second = second.substr(0, second.find(','));
    if (!second.empty()) second = KeyText(second);
    return {KeyText(keys.substr(0, comma)), second};
}

constexpr float kKeyBoxEms = 10.0f;  // a key box's width when the row has two

struct KeyBox {
    float x;
    float width;
    bool capturing;
};

// what one box saw this frame
struct BoxInput {
    bool pressed = false;
    bool cleared = false;  // right clicked, or Delete while focused
    bool pointed = false;  // under the mouse or focused
};

// lit while focused, hovered or capturing
BoxInput DrawKeyBox(const RowLayout& row, const KeyBox& box, const std::string& key) {
    ImGui::SetCursorPos(ImVec2(box.x, row.origin.y));
    BoxInput seen;
    seen.pressed = ImGui::InvisibleButton("##key", ImVec2(box.width, row.height));
    bool deleted = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    seen.cleared = ImGui::IsItemClicked(ImGuiMouseButton_Right) || deleted;
    seen.pointed = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    bool lit = box.capturing || ImGui::IsItemFocused() || ImGui::IsItemHovered();
    ImU32 fill = ImGui::GetColorU32(lit ? ImGuiCol_Header : ImGuiCol_FrameBg);
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), fill);
    std::string text = key == kUnbound ? Ui(key) : key;  // key names as they are
    if (box.capturing) text = Ui(g_capture.pad ? "PRESS A BUTTON" : "PRESS A KEY");
    float centred = (box.width - ImGui::CalcTextSize(text.c_str()).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(box.x + std::max(centred, 0.0f), row.origin.y));
    ImGui::AlignTextToFramePadding();
    if (box.capturing) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(text.c_str());
    if (box.capturing) ImGui::PopStyleColor();
    return seen;
}

// what a bind row's boxes saw this frame, each a box or -1
struct BindInput {
    int pressed = -1;
    int cleared = -1;
    int pointed = -1;  // only a box with a key in it
};

// label left, a box per key (the key and its alternate side by side) ending where the other rows' values do;
// Enter or a click on a box starts its capture, a right click or Delete clears it
BindInput BindRow(const char* label, const std::vector<std::string>& keys, int capturing) {
    RowLayout row = MeasureRow();
    ImGui::PushID(label);
    DrawLabel(row, label);
    int count = int(keys.size());
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float width = count > 1 ? ImGui::GetFontSize() * kKeyBoxEms : row.value_width;
    float end = row.value_x + row.value_width;
    BindInput seen;
    for (int i = 0; i < count; i++) {
        const std::string& key = keys[size_t(i)];
        float x = end - float(count - i) * width - float(count - 1 - i) * spacing;
        ImGui::PushID(i);
        BoxInput box = DrawKeyBox(row, {x, width, capturing == i}, key);
        ImGui::PopID();
        if (box.pressed) seen.pressed = i;
        if (box.cleared) seen.cleared = i;
        if (box.pointed && !key.empty() && key != kUnbound) seen.pointed = i;
    }
    EndRow(row);
    return seen;
}

void DrawBindRow(const Row& row) {
    int capturing = !g_capture.pad && g_capture.action == row.id ? g_capture.slot : -1;
    BindInput seen = BindRow(row.label, KeyBoxes(size_t(row.id)), capturing);
    if (seen.pointed >= 0) g_hovered_box = seen.pointed;
    // a Delete that answered a capture is no clear
    if (seen.cleared >= 0 && BindingSettled()) input::Bind(size_t(row.id), 0, seen.cleared);
    if (seen.pressed >= 0 && BindingSettled()) StartCapture(row.id, seen.pressed);
}

// one box: a controller action always has a button, so it is not cleared
void DrawPadBindRow(const Row& row) {
    int capturing = g_capture.pad && g_capture.action == row.id ? 0 : -1;
    std::string button = input::PadBinding(size_t(row.id));
    BindInput seen = BindRow(row.label, {button.empty() ? kUnbound : button}, capturing);
    if (seen.pressed >= 0 && BindingSettled()) StartCapture(row.id, 0, true);
}

void DrawRow(const Row& row) {
    switch (row.kind) {
        case Kind::Heading:
            DrawHeading(row.label);
            break;
        case Kind::Value:
            DrawValueRow(row);
            break;
        case Kind::Bind:
            DrawBindRow(row);
            break;
        case Kind::PadBind:
            DrawPadBindRow(row);
            break;
    }
}

void RestoreDefaults(int page) {
    CancelCapture();
    if (page == 0) {
        const game::Options defaults;
        game::Opt().skip_intros = defaults.skip_intros;
        game::Opt().skippable_cutscenes = defaults.skippable_cutscenes;
        game::Opt().skip_popups = defaults.skip_popups;
        game::Opt().persistent_bodies = defaults.persistent_bodies;
        game::Opt().inventory_pause = defaults.inventory_pause;
        game::Opt().fps_counter = defaults.fps_counter;
        game::Opt().hdr_output = defaults.hdr_output;
        game::Opt().fmv_scale = defaults.fmv_scale;
        game::Opt().ambient_occlusion = defaults.ambient_occlusion;
        game::Opt().audio_output = defaults.audio_output;
        game::Opt().msaa = defaults.msaa;
        game::Opt().draw_distance = defaults.draw_distance;
        game::Opt().smaa = defaults.smaa;
        game::Opt().fov = defaults.fov;
        game::Opt().monitor = defaults.monitor;
        game::Opt().pause_on_focus_loss = defaults.pause_on_focus_loss;
        game::Opt().players = defaults.players;
        audio::SetOutputMode(defaults.audio_output);
        RestoreSettingsDefaults();
    } else if (page == kControllerPage) {
        input::ResetPadBindings();
    } else {
        const game::Options defaults;
        game::Options& options = game::Opt();
        options.mouse_sensitivity = defaults.mouse_sensitivity;
        options.invert_mouse = defaults.invert_mouse;
        options.camera_centring = defaults.camera_centring;
        options.free_aim = defaults.free_aim;
        options.walk_speed = defaults.walk_speed;
        input::ResetBindings();
    }
}

// Pages. LB and RB step the tabs, one tab a page as wide as they share the line.

int TabStep() {
    return int(ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) - int(ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false));
}

// the page's tab, selected by a step when stepped is set; a click selects it
void DrawTab(int page, float width, bool stepped) {
    ImGui::SetNextItemWidth(width);
    ImGuiTabItemFlags flags = stepped && page == g_page ? ImGuiTabItemFlags_SetSelected : 0;
    std::string tab = Ui(kPages[page]) + "###" + kPages[page];
    if (!ImGui::BeginTabItem(tab.c_str(), nullptr, flags)) return;
    bool clicked = !stepped && page != g_page;
    if (clicked) {
        g_page = page;
        CancelCapture();
    }
    ImGui::EndTabItem();
}

void DrawTabs() {
    // a selection made here shows from the next frame, the tab drawn this one still reads as selected
    int step = TabStep();
    bool stepped = step != 0 && BindingSettled();
    if (stepped) g_page = StepIndex(g_page, step, kPageCount);
    ImGui::PushStyleColor(ImGuiCol_Tab, kField);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, kFieldHovered);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, kHighlight);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kHeading);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
    if (ImGui::BeginTabBar("##pages")) {
        float spacing = ImGui::GetStyle().ItemInnerSpacing.x * float(kPageCount - 1);
        float width = (ImGui::GetContentRegionAvail().x - spacing) / float(kPageCount);
        for (int page = 0; page < kPageCount; page++) DrawTab(page, width, stepped);
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void DrawTitle() {
    std::string title = Ui("SETTINGS");
    float spare = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(spare * 0.5f, 0.0f));
    ImGui::TextUnformatted(title.c_str());
}

void DrawHint() {
    const char* hint = "";
    if (Capturing() && g_capture.pad) {
        hint = "Press a controller button to bind, Escape cancels";
    } else if (Capturing()) {
        hint = "Press a key or mouse button to bind, Escape cancels";
    } else if (g_page == kInputPage && g_hovered_box >= 0) {
        hint = "Right click or Delete clears the key";
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(Ui(hint, false).c_str());
    ImGui::PopStyleColor();
}

void DrawFooter() {
    float em = ImGui::GetFontSize();
    float button_width = em * kButtonEms;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float spare = ImGui::GetContentRegionAvail().x - button_width * kFooterButtons - spacing * (kFooterButtons - 1);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(spare, 0.0f));
    if (ImGui::Button(Ui("Back", false).c_str(), ImVec2(button_width, 0))) ToggleMenu();
    ImGui::SameLine();
    if (ImGui::Button(Ui("Defaults", false).c_str(), ImVec2(button_width, 0))) RestoreDefaults(g_page);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_display_held);
    if (ImGui::Button(Ui("Apply", false).c_str(), ImVec2(button_width, 0))) Apply();
    ImGui::EndDisabled();
}

// colours and square corners for the panel's window, undone when it goes out of scope
struct PanelStyle {
    PanelStyle() {
        float em = ImGui::GetFontSize();
        for (const auto& [slot, color] : kPanelColors) ImGui::PushStyleColor(slot, color);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, kBorderSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * kPaddingXEms, em * kPaddingYEms));
    }
    ~PanelStyle() {
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(int(std::size(kPanelColors)));
    }
};

void DrawPanel() {
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * kWindowWidthEms, em * kWindowHeightEms), ImGuiCond_Always);
    PanelStyle style;
    if (ImGui::Begin("##settings", nullptr, kWindowFlags)) {
        DrawTitle();
        DrawTabs();
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        g_hovered_box = -1;
        size_t count = 0;
        const Row* list = rows::Page(g_page, &count);
        for (size_t i = 0; i < count; i++) DrawRow(list[i]);
        ImGui::EndChild();
        ImGui::Separator();
        DrawHint();
        DrawFooter();
    }
    ImGui::End();
}

// the game window's monitor's refresh rate, read again each second (a mode change, a move to another monitor); 0
// when unreadable
uint32_t RefreshRate() {
    static uint32_t rate = 0;
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return rate;
    next = now + 1000;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    bool read = GetMonitorInfoW(monitor, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode);
    rate = read && mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
    return rate;
}

// as app::Pace, at this cap
void PaceAt(uint32_t cap) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) Sleep(1);
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace

void PaceFrame() {
    if (game::g_fast_forward) return;
    if (Get().fps_cap != kFpsCapRefresh) {
        Pace();
        return;
    }
    // a few under, so a variable refresh display stays in its range and vsync never waits a whole refresh
    constexpr uint32_t kRefreshMargin = 3;
    uint32_t rate = RefreshRate();
    PaceAt(rate > kRefreshMargin ? rate - kRefreshMargin : 0);
}

const rows::Row* rows::Page(int page, size_t* count) {
    if (page == 0) {
        *count = std::size(kSettingsRows);
        return kSettingsRows;
    }
    const std::vector<Row>& list = page == kControllerPage ? ControllerRows() : InputRows();
    *count = list.size();
    return list.data();
}

// the game's menu applies a change at once, display rows too
std::string rows::Value(const Row& row, int step) {
    bool capturing = g_capture.action == row.id;
    if (row.kind == Kind::Bind && capturing && !g_capture.pad) return "PRESS A KEY";
    if (row.kind == Kind::Bind) return KeyText(input::Binding(size_t(row.id)));
    if (row.kind == Kind::PadBind && capturing && g_capture.pad) return "PRESS A BUTTON";
    if (row.kind == Kind::PadBind) return KeyText(input::PadBinding(size_t(row.id)));
    if (row.kind != Kind::Value) return {};
    if (Greyed(row.id)) return "NOT SUPPORTED";
    std::string text = kSteps[row.id](Get(), step);
    if (step == 0) return text;
    if (Held(row.id)) ApplyDisplay();
    Save();
    return text;
}

void rows::Defaults(int page) { RestoreDefaults(page); }

void rows::Bind(const Row& row) {
    if (row.kind == Kind::Bind && !Capturing()) StartCapture(row.id, 0);
    if (row.kind == Kind::PadBind && !Capturing()) StartCapture(row.id, 0, true);
}

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

bool Capturing() { return g_capture.action >= 0; }

// Windows asks for the cursor only when the mouse moves
void RefreshCursor() {
    void* hwnd = Window();
    if (hwnd) PostMessageW(HWND(hwnd), WM_SETCURSOR, WPARAM(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void ToggleMenu() {
    g_open.store(!MenuOpen(), std::memory_order_relaxed);
    g_display_held = false;
    CancelCapture();
    input::ReleaseCursor();
    RefreshCursor();
}

// ImGui sets the cursor only while the panel is open
void UpdateCursorFlag(ImGuiIO& io) {
    if (MenuOpen()) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
}

void DrawUi(double fps) {
    game::g_fps = float(fps);
    ImGuiIO& io = ImGui::GetIO();
    UpdateCursorFlag(io);
    io.KeyRepeatRate = kRepeatSeconds;
    io.FontGlobalScale = std::max(kMinFontScale, io.DisplaySize.y / kFontScaleLines);
    PollCapture();  // a bind started in the game's options menu waits here too
    if (MenuOpen()) DrawPanel();
}

}  // namespace app
