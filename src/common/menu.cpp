// the options panel on F10
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
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
#include "common/menu.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};

// The display mode and resolution wait for Apply, as Windows' display settings do; closing drops them.
bool g_display_held = false;
Settings g_display;

constexpr ImVec4 kPanelBg{0.04f, 0.045f, 0.05f, 0.94f};
constexpr ImVec4 kBorder{0.55f, 0.60f, 0.68f, 0.85f};
constexpr ImVec4 kText{0.89f, 0.90f, 0.92f, 1.0f};
constexpr ImVec4 kHighlight{0.21f, 0.26f, 0.33f, 1.0f};
constexpr ImVec4 kHeading{0.62f, 0.69f, 0.80f, 1.0f};
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
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, 0};
const char* const kDisplayModeNames[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
constexpr int kDisplayModeCount = int(std::size(kDisplayModeNames));

constexpr int kRenderMin = 20;
constexpr int kRenderMax = 200;
constexpr int kRenderStep = 5;
constexpr int kRenderSlots = (kRenderMax - kRenderMin) / kRenderStep + 1;

constexpr DWORD kMinModeWidth = 640;
constexpr DWORD kMinModeBits = 32;

constexpr float kValueEms = 12.0f;  // width of the value between the arrows
constexpr float kButtonEms = 8.0f;
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

// every mode the monitor reports, largest first
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
        auto larger = [](Mode a, Mode b) {
            if (a.width != b.width) return a.width > b.width;
            return a.height > b.height;
        };
        std::sort(list.begin(), list.end(), larger);
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

// Row text. Each function steps its setting first when step is not zero, then returns what the row shows.

std::string OnOff(bool on) { return on ? "ON" : "OFF"; }

std::string DisplayModeText(Settings& s, int step) {
    if (step) s.display_mode = DisplayMode(StepIndex(int(s.display_mode), step, kDisplayModeCount));
    return kDisplayModeNames[std::min(int(s.display_mode), kDisplayModeCount - 1)];
}

void StepResolution(Settings& s, int step) {
    const std::vector<Mode>& modes = Modes();
    if (modes.empty()) return;
    int next = StepIndex(ModeIndex(modes, ShownMode(s)), step, int(modes.size()));
    // the desktop's own mode is kept as 0, which follows the desktop
    Mode picked = modes[size_t(next)];
    bool native = SameMode(picked, Desktop());
    s.width = native ? 0 : picked.width;
    s.height = native ? 0 : picked.height;
}

std::string ResolutionText(Settings& s, int step) {
    if (step) StepResolution(s, step);
    Mode mode = ShownMode(s);
    return std::to_string(mode.width) + "x" + std::to_string(mode.height);
}

std::string RenderResolutionText(Settings& s, int step) {
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

std::string VsyncText(Settings& s, int step) {
    if (step) s.vsync = !s.vsync;
    return OnOff(s.vsync);
}

std::string MaxFpsText(Settings& s, int step) {
    uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
    return cap ? std::to_string(cap) : "UNLIMITED";
}

std::string AnisoText(Settings& s, int step) { return kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]; }

// The rows. A null text is a section heading, held rows wait for Apply.
using TextFn = std::string (*)(Settings& s, int step);

struct Row {
    const char* label;
    TextFn text;
    bool held;
};

constexpr Row kRows[] = {
    {"DISPLAY", nullptr, false},
    {"DISPLAY MODE", DisplayModeText, true},
    {"RESOLUTION", ResolutionText, true},
    {"RENDER RESOLUTION", RenderResolutionText, false},
    {"VERTICAL SYNC", VsyncText, false},
    {"MAX FRAME RATE", MaxFpsText, false},
    {"GRAPHICS", nullptr, false},
    {"ANISOTROPIC FILTERING", AnisoText, false},
};

bool SameDisplay(const Settings& a, const Settings& b) {
    return a.display_mode == b.display_mode && a.width == b.width && a.height == b.height;
}

void CopyDisplay(Settings& to, const Settings& from) {
    to.display_mode = from.display_mode;
    to.width = from.width;
    to.height = from.height;
}

void ApplyLiveStep(const Row& row, int step) {
    row.text(Get(), step);
    Save();
}

void ApplyHeldStep(const Row& row, int step) {
    const Settings& live = Get();
    if (!g_display_held) CopyDisplay(g_display, live);
    row.text(g_display, step);
    g_display_held = !SameDisplay(g_display, live);
}

void Apply() {
    CopyDisplay(Get(), g_display);
    g_display_held = false;
    ApplyDisplay();
    Save();
}

void RestoreDefaults() {
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
    float block_width = row.height * 2 + row.value_width + style.ItemSpacing.x * 2;
    row.block_x = row.origin.x + row.width - block_width;
    row.value_x = row.block_x + row.height + style.ItemSpacing.x;
    row.inc_x = row.value_x + row.value_width + style.ItemSpacing.x;
    return row;
}

// the row is the only nav stop; the focused row steps on the left and right keys
int FocusedStep(const RowLayout& row) {
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(row.width, row.height));
    if (!ImGui::IsItemFocused()) return 0;
    ImU32 fill = ImGui::GetColorU32(ImGuiCol_Header);
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), fill);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) return -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) return 1;
    return 0;
}

void DrawLabel(const RowLayout& row, const char* label) {
    ImGui::SetCursorPos(ImVec2(row.origin.x + ImGui::GetStyle().ItemInnerSpacing.x, row.origin.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
}

// the arrows keep stepping while held
int DrawValueBlock(const RowLayout& row, const char* value) {
    int step = 0;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);
    ImGui::SetCursorPos(ImVec2(row.block_x, row.origin.y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    float centred = (row.value_width - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(row.value_x + std::max(centred, 0.0f), row.origin.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);
    ImGui::SetCursorPos(ImVec2(row.inc_x, row.origin.y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    return step;
}

// the step asked for this frame, 0 for none
int OptionRow(const char* label, const char* value) {
    RowLayout row = MeasureRow();
    ImGui::PushID(label);
    int key_step = FocusedStep(row);
    DrawLabel(row, label);
    int arrow_step = DrawValueBlock(row, value);
    ImGui::PopID();
    ImGui::SetCursorPos(row.origin);
    ImGui::Dummy(ImVec2(row.width, row.height));
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

void DrawRow(const Row& row) {
    if (!row.text) {
        DrawHeading(row.label);
        return;
    }
    Settings& shown = row.held && g_display_held ? g_display : Get();
    int step = OptionRow(row.label, row.text(shown, 0).c_str());
    if (step == 0) return;
    if (row.held) {
        ApplyHeldStep(row, step);
    } else {
        ApplyLiveStep(row, step);
    }
}

void DrawTitle() {
    const char* title = "SETTINGS";
    float spare = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(spare * 0.5f, 0.0f));
    ImGui::TextUnformatted(title);
}

void DrawFooter() {
    float em = ImGui::GetFontSize();
    float button_width = em * kButtonEms;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float spare = ImGui::GetContentRegionAvail().x - button_width * 3 - spacing * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(spare, 0.0f));
    if (ImGui::Button("Back", ImVec2(button_width, 0))) ToggleMenu();
    ImGui::SameLine();
    if (ImGui::Button("Defaults", ImVec2(button_width, 0))) RestoreDefaults();
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
        float footer = ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        for (const Row& row : kRows) DrawRow(row);
        ImGui::EndChild();
        ImGui::Separator();
        DrawFooter();
    }
    ImGui::End();
}

}  // namespace

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

void ToggleMenu() {
    g_open.store(!MenuOpen(), std::memory_order_relaxed);
    g_display_held = false;
}

void DrawUi(double /*fps*/) {
    ImGuiIO& io = ImGui::GetIO();
    if (MenuOpen()) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
    io.KeyRepeatRate = kRepeatSeconds;
    io.FontGlobalScale = std::max(kMinFontScale, io.DisplaySize.y / kFontScaleLines);
    if (MenuOpen()) DrawPanel();
}

}  // namespace app
