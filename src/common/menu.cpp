// the options panel on F10
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
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
#include "common/mouse_keyboard.h"
#include "common/options.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};

// The display mode and resolution wait for Apply, as Windows' display settings do; closing drops them.
bool g_display_held = false;
Settings g_display;

const char* const kPages[] = {"RECOMP SETTINGS", "MOUSE AND KEYBOARD"};
constexpr int kPageCount = int(std::size(kPages));
int g_page = 0;

// the key binding in progress: its keys must be let go before the next press binds
struct Capture {
    std::atomic<int> action{-1};  // the row being rebound, -1 when idle
    bool armed = false;
    int done_frame = 0;  // frame of the last bind, the key that bound must not start another
};
Capture g_capture;

constexpr ImVec4 kPanelBg{0.04f, 0.045f, 0.05f, 0.94f};
constexpr ImVec4 kBorder{0.55f, 0.60f, 0.68f, 0.85f};
constexpr ImVec4 kText{0.89f, 0.90f, 0.92f, 1.0f};
constexpr ImVec4 kHighlight{0.21f, 0.26f, 0.33f, 1.0f};
constexpr ImVec4 kHeading{0.62f, 0.69f, 0.80f, 1.0f};
constexpr ImVec4 kDim{0.57f, 0.60f, 0.64f, 1.0f};
constexpr ImVec4 kField{0.11f, 0.12f, 0.14f, 1.0f};
constexpr ImVec4 kFieldHovered{0.18f, 0.20f, 0.24f, 1.0f};
constexpr ImVec4 kButton{0.15f, 0.17f, 0.20f, 1.0f};
// lighter than the highlight, so it shows on a selected row
constexpr ImVec4 kButtonHovered{0.30f, 0.35f, 0.43f, 1.0f};
constexpr ImVec4 kLine{0.62f, 0.69f, 0.80f, 0.35f};

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
constexpr uint32_t kAoValues[] = {0, 1, 2};
const char* const kAoNames[] = {"OFF", "SSAO", "GTAO"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, 0};
const char* const kDisplayModeNames[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
constexpr int kDisplayModeCount = int(std::size(kDisplayModeNames));

constexpr int kSensitivitySlots = 50;  // 0.1 to 5.0 in tenths
constexpr float kTenthsPerUnit = 10.0f;

constexpr int kRenderMin = 20;
constexpr int kRenderMax = 200;
constexpr int kRenderStep = 5;
constexpr int kRenderSlots = (kRenderMax - kRenderMin) / kRenderStep + 1;

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

// every mode the monitor reports, smallest first so Left steps down
const std::vector<Mode>& Modes() {
    static const std::vector<Mode> modes = [] {
        std::vector<Mode> list;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
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
    }();
    return modes;
}

Mode ShownMode(const Settings& s) {
    bool custom = s.width != 0 && s.height != 0;
    return custom ? Mode{s.width, s.height} : Desktop();
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

std::string StepDisplayMode(Settings& s, int step) {
    if (step) s.display_mode = DisplayMode(StepIndex(int(s.display_mode), step, kDisplayModeCount));
    return kDisplayModeNames[std::min(int(s.display_mode), kDisplayModeCount - 1)];
}

void StepResolutionMode(Settings& s, int step) {
    const std::vector<Mode>& modes = Modes();
    if (modes.empty()) return;
    int next = StepIndex(ModeIndex(modes, ShownMode(s)), step, int(modes.size()));
    // the desktop's own mode is kept as 0, which follows the desktop
    Mode picked = modes[size_t(next)];
    bool native = SameMode(picked, Desktop());
    s.width = native ? 0 : picked.width;
    s.height = native ? 0 : picked.height;
}

std::string StepResolution(Settings& s, int step) {
    if (step) StepResolutionMode(s, step);
    Mode mode = ShownMode(s);
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

std::string StepMaxFps(Settings& s, int step) {
    uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
    return cap ? std::to_string(cap) : "UNLIMITED";
}

std::string StepFpsCounter(Settings&, int step) {
    bool& on = game::Opt().fps_counter;
    if (step) on = !on;
    return OnOff(on);
}

std::string StepAniso(Settings& s, int step) { return kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]; }

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
};

bool Held(int id) { return id == kDisplayModeRow || id == kResolutionRow; }

using rows::Kind;
using rows::Row;

constexpr Row kSettingsRows[] = {
    {Kind::Heading, -1, "DISPLAY"},
    {Kind::Value, kDisplayModeRow, "DISPLAY MODE"},
    {Kind::Value, kResolutionRow, "RESOLUTION"},
    {Kind::Value, kRenderResolutionRow, "RENDER RESOLUTION"},
    {Kind::Value, kVsyncRow, "VERTICAL SYNC"},
    {Kind::Value, kMaxFpsRow, "MAX FRAME RATE"},
    {Kind::Value, kFpsCounterRow, "FPS COUNTER"},
    {Kind::Heading, -1, "GRAPHICS"},
    {Kind::Value, kAnisoRow, "ANISOTROPIC FILTERING"},
    {Kind::Value, kAmbientOcclusionRow, "AMBIENT OCCLUSION"},
    {Kind::Heading, -1, "GENERAL"},
    {Kind::Value, kSkipIntrosRow, "SKIP INTROS"},
    {Kind::Value, kAudioOutputRow, "AUDIO OUTPUT"},
};

// the mouse rows, then a bind row for each input action
const std::vector<Row>& InputRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {
            {Kind::Heading, -1, "MOUSE"},
            {Kind::Value, kSensitivityRow, "MOUSE SENSITIVITY"},
            {Kind::Value, kInvertLookRow, "INVERT LOOK"},
            {Kind::Value, kCameraCentringRow, "CAMERA CENTRING"},
            {Kind::Heading, -1, "KEYBOARD"},
        };
        std::span<const input::Action> actions = input::Actions();
        for (size_t i = 0; i < actions.size(); i++) out.push_back({Kind::Bind, int(i), actions[i].label});
        return out;
    }();
    return list;
}

bool SameDisplay(const Settings& a, const Settings& b) {
    return a.display_mode == b.display_mode && a.width == b.width && a.height == b.height;
}

void CopyDisplay(Settings& to, const Settings& from) {
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
    ImGui::TextUnformatted(label);
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
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * kHeadingAfterEms));
}

void DrawValueRow(const Row& row) {
    bool held = Held(row.id);
    Settings& shown = held && g_display_held ? g_display : Get();
    int step = OptionRow(row.label, kSteps[row.id](shown, 0).c_str());
    if (step == 0) return;
    if (held) {
        ApplyHeldStep(row, step);
    } else {
        ApplyLiveStep(row, step);
    }
}

// Key bindings.

std::string KeyText(std::string keys) {
    if (keys.empty()) return "UNBOUND";
    for (char& c : keys) c = c == ',' ? '/' : char(std::toupper(uint8_t(c)));
    return keys;
}

void CancelCapture() { g_capture.action = -1; }

void StartCapture(int action) {
    g_capture.action = action;
    g_capture.armed = false;
}

void EndCapture() {
    CancelCapture();
    g_capture.done_frame = ImGui::GetFrameCount();
}

// a row or a tab was just bound or chosen: nothing may start on the frame after a bind
bool BindingSettled() { return !Capturing() && ImGui::GetFrameCount() > g_capture.done_frame + 1; }

// Escape cancels, any other bindable key or mouse button binds
void PollCapture() {
    if (!Capturing()) return;
    int vk = input::PollCapturedKey();
    if (!g_capture.armed) {
        g_capture.armed = vk == 0;
        return;
    }
    if (vk == 0) return;
    if (vk != VK_ESCAPE) input::Bind(size_t(g_capture.action.load()), vk);
    EndCapture();
}

// label left, the keys where the value sits; Enter or a click starts the capture
bool BindRow(const char* label, const std::string& keys, bool capturing) {
    RowLayout row = MeasureRow();
    ImGui::PushID(label);
    bool pressed = DrawNavStop(row, capturing);
    DrawLabel(row, label);
    if (capturing) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    DrawValueText(row, capturing ? "PRESS A KEY" : keys.c_str());
    if (capturing) ImGui::PopStyleColor();
    EndRow(row);
    return pressed;
}

void DrawBindRow(const Row& row) {
    bool capturing = g_capture.action == row.id;
    std::string keys = KeyText(input::Binding(size_t(row.id)));
    bool pressed = BindRow(row.label, keys, capturing);
    if (pressed && BindingSettled()) StartCapture(row.id);
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
    }
}

void RestoreDefaults(int page) {
    CancelCapture();
    if (page == 0) {
        const game::Options defaults;
        game::Opt().skip_intros = defaults.skip_intros;
        game::Opt().fps_counter = defaults.fps_counter;
        game::Opt().audio_output = defaults.audio_output;
        audio::SetOutputMode(defaults.audio_output);
        RestoreSettingsDefaults();
    } else {
        const game::Options defaults;
        game::Options& options = game::Opt();
        options.mouse_sensitivity = defaults.mouse_sensitivity;
        options.invert_mouse = defaults.invert_mouse;
        options.camera_centring = defaults.camera_centring;
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
    if (!ImGui::BeginTabItem(kPages[page], nullptr, flags)) return;
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
    const char* title = "SETTINGS";
    float spare = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(spare * 0.5f, 0.0f));
    ImGui::TextUnformatted(title);
}

void DrawHint() {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(Capturing() ? "Press a key or mouse button to bind, Escape cancels" : "");
    ImGui::PopStyleColor();
}

void DrawFooter() {
    float em = ImGui::GetFontSize();
    float button_width = em * kButtonEms;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float spare = ImGui::GetContentRegionAvail().x - button_width * kFooterButtons - spacing * (kFooterButtons - 1);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(spare, 0.0f));
    if (ImGui::Button("Back", ImVec2(button_width, 0))) ToggleMenu();
    ImGui::SameLine();
    if (ImGui::Button("Defaults", ImVec2(button_width, 0))) RestoreDefaults(g_page);
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_display_held);
    if (ImGui::Button("Apply", ImVec2(button_width, 0))) Apply();
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

}  // namespace

const rows::Row* rows::Page(int page, size_t* count) {
    if (page == 0) {
        *count = std::size(kSettingsRows);
        return kSettingsRows;
    }
    *count = InputRows().size();
    return InputRows().data();
}

// the game's menu applies a change at once, display rows too
std::string rows::Value(const Row& row, int step) {
    if (row.kind == Kind::Bind && g_capture.action == row.id) return "PRESS A KEY";
    if (row.kind == Kind::Bind) return KeyText(input::Binding(size_t(row.id)));
    if (row.kind != Kind::Value) return {};
    std::string text = kSteps[row.id](Get(), step);
    if (step == 0) return text;
    if (Held(row.id)) ApplyDisplay();
    Save();
    return text;
}

void rows::Defaults(int page) { RestoreDefaults(page); }

void rows::Bind(const Row& row) {
    if (row.kind == Kind::Bind && !Capturing()) StartCapture(row.id);
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
