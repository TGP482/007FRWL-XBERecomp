#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/output.h"
#include "common/menu.h"
#include "common/monitor.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/pad_binds.h"
#include "common/saves.h"
#include "core/input.h"

void TakeCutsceneSkip(int port, input::Pad& pad);  // cutscenes.cpp

namespace game {

namespace {

constexpr intptr_t kKeyWasDown = 1 << 30;  // bit 30 of a key message's lparam: a repeat
constexpr int kArrowCursorId = 32512;      // the system arrow
constexpr uintptr_t kSysCommandMask = 0xFFF0;  // the low bits of a WM_SYSCOMMAND code are the system's
constexpr uint32_t kHostApplyDisplay = WM_APP + 1;  // main.cpp's kApplyDisplay

bool Take(intptr_t* result) {
    *result = 0;
    return true;
}

// the panel's key opens and closes it, Escape closes it unless a key is being bound; the HUD key toggles the HUD
bool PanelKeyDown(uint32_t msg, uintptr_t key, intptr_t lp, intptr_t* result) {
    bool first_press = !(lp & kKeyWasDown);
    if (first_press && input::PanelKey(int(key))) {
        app::ToggleMenu();
        return Take(result);
    }
    if (first_press && !app::MenuOpen() && input::HudKey(int(key))) {
        g_hide_hud = !g_hide_hud;
        return Take(result);
    }
    bool binding = app::Capturing() || input::CaptureRecent();
    if (msg == WM_KEYDOWN && key == VK_ESCAPE && app::MenuOpen() && !binding) {
        app::ToggleMenu();
        return Take(result);
    }
    return false;
}

// a bound Alt must not open the window menu
bool SwallowAltMenu(uintptr_t command, intptr_t* result) {
    if ((command & kSysCommandMask) != SC_KEYMENU) return false;
    return Take(result);
}

// the arrow over the panel, the menu cursor over the game's menus, no cursor in play
bool ClientCursor(intptr_t lp, intptr_t* result) {
    if (LOWORD(lp) != HTCLIENT) return false;
    HCURSOR arrow = LoadCursorW(nullptr, MAKEINTRESOURCEW(kArrowCursorId));
    HCURSOR menu = input::CursorShown() ? static_cast<HCURSOR>(app::CursorImage()) : nullptr;
    SetCursor(app::MenuOpen() ? arrow : menu);
    *result = TRUE;
    return true;
}

// F3 and the display mode stay with the host
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    switch (msg) {
        case WM_CREATE:
            UpdateSplashMonitor();
            UpdateWindowMonitor(hwnd);
            return false;
        case kHostApplyDisplay:
            UpdateWindowMonitor(hwnd);
            return false;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) input::ReleaseCursor();
            return false;
        case WM_INPUT:
            input::OnRawInput(reinterpret_cast<void*>(lp));
            return false;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            return PanelKeyDown(msg, wp, lp, result);
        case WM_SYSCOMMAND:
            return SwallowAltMenu(wp, result);
        case WM_SETCURSOR:
            return ClientCursor(lp, result);
    }
    return false;
}

// The SDK reads and sets the controller slot numbered as the port. A player port takes the slot it is set to, so a
// slot read or set for another port goes past these hooks.
thread_local bool g_raw = false;
std::array<std::atomic<uint32_t>, kPlayers> g_motors{};  // by slot: left << 16 | right as its player last set

input::Pad RawPad(int slot) {
    g_raw = true;
    input::Pad pad = input::Poll(slot);
    g_raw = false;
    return pad;
}

void RawRumble(int slot, uint16_t left, uint16_t right) {
    g_raw = true;
    input::Rumble(slot, left, right);
    g_raw = false;
}

input::Pad SlotPad(int port, int slot, const input::Pad& own) {
    if (slot < 0) return input::Pad{};
    return slot == port ? own : RawPad(slot);
}

bool SlotOwned(int slot) {
    const auto& players = Opt().players;
    return std::any_of(players.begin(), players.end(), [slot](const PlayerPort& p) { return p.pad == slot; });
}

// the port's controller slot and, when set to, the keyboard; the game sees no input while the panel is open or a
// binding waits
void Pad(int port, input::Pad& pad) {
    if (g_raw || port < 0 || port >= kPlayers) return;
    PlayerPort player = Opt().players[size_t(port)];
    pad = SlotPad(port, player.pad, pad);
    if (player.keyboard) input::SeenPad(port, pad);
    input::RemapPad(port, pad);
    if (player.keyboard) pad.connected = true;
    if (app::MenuOpen() || app::Capturing()) {
        pad = input::Pad{.connected = pad.connected};
    } else if (player.keyboard) {
        input::Keyboard(pad);
    }
    TakeCutsceneSkip(port, pad);
}

// to the port's own slot; what the SDK then sets on the slot numbered as the port is that slot's player's.
// No vibration while the keyboard and mouse are in use.
void Rumble(int port, uint16_t& left, uint16_t& right) {
    if (g_raw || port < 0 || port >= kPlayers) return;
    PlayerPort player = Opt().players[size_t(port)];
    if (player.keyboard && input::KeyboardActive()) {
        left = 0;
        right = 0;
    }
    if (player.pad >= 0) g_motors[size_t(player.pad)] = uint32_t(left) << 16 | right;
    if (player.pad >= 0 && player.pad != port) RawRumble(player.pad, left, right);
    uint32_t motors = SlotOwned(port) ? g_motors[size_t(port)].load() : 0;
    left = uint16_t(motors >> 16);
    right = uint16_t(motors);
}

std::string PortName(const PlayerPort& p) {
    std::string name = p.keyboard ? "keyboard" : "";
    if (p.pad >= 0) name += (name.empty() ? "controller" : "+controller") + std::to_string(p.pad + 1);
    return name.empty() ? "none" : name;
}

PlayerPort ParsePort(const std::string& name, PlayerPort fallback) {
    PlayerPort p;
    p.keyboard = name.find("keyboard") != std::string::npos;
    if (size_t at = name.find("controller"); at != std::string::npos && at + 10 < name.size()) {
        int n = name[at + 10] - '0';  // the digit after "controller"
        if (n >= 1 && n <= kPlayers) p.pad = n - 1;
    }
    return p.keyboard || p.pad >= 0 || name == "none" ? p : fallback;
}

std::string PlayerKey(int player) { return "player" + std::to_string(player + 1); }

// a device sits on one port only, and player 1 never has none
void LoadPlayers(const toml::table& settings, std::array<PlayerPort, kPlayers>& players) {
    for (int i = 0; i < kPlayers; i++) {
        PlayerPort& p = players[size_t(i)];
        p = ParsePort(settings[PlayerKey(i)].value_or(PortName(p)), p);
        for (int j = 0; j < i; j++) {
            if (players[size_t(j)].keyboard) p.keyboard = false;
            if (p.pad >= 0 && players[size_t(j)].pad == p.pad) p.pad = -1;
        }
    }
    auto used = [](const PlayerPort& p) { return p.keyboard || p.pad >= 0; };
    auto first = std::find_if(players.begin(), players.end(), used);
    if (first == players.end()) players[0] = PlayerPort{true, 0};
    else std::swap(players[0], *first);
}

void LoadNames(const toml::table& settings, const char* name, std::map<std::string, std::string>& names) {
    const toml::table* table = settings[name].as_table();
    if (!table) return;
    for (auto&& [id, value] : *table) {
        if (auto text = value.value<std::string>()) names[std::string(id.str())] = *text;
    }
}

void SaveNames(toml::table& settings, const char* name, const std::map<std::string, std::string>& names) {
    toml::table table;
    for (const auto& [id, text] : names) table.insert(id, text);
    settings.insert(name, table);
}

// what the options list cannot hold
void Load(const void* table) {
    const toml::table& settings = *static_cast<const toml::table*>(table);
    Options& options = Opt();
    float sensitivity = float(settings["mouse_sensitivity"].value_or(double(options.mouse_sensitivity)));
    if (std::isfinite(sensitivity)) options.mouse_sensitivity = sensitivity;
    options.monitor = settings["monitor"].value_or(options.monitor);
    LoadNames(settings, "keys", options.keys);
    LoadNames(settings, "pad_buttons", options.pad_buttons);
    LoadPlayers(settings, options.players);
}

void Save(void* table) {
    toml::table& settings = *static_cast<toml::table*>(table);
    settings.insert("monitor", Opt().monitor);
    // thousandths, so the file reads 0.3 and not 0.30000001192092896
    settings.insert("mouse_sensitivity", std::round(double(Opt().mouse_sensitivity) * 1000.0) / 1000.0);
    SaveNames(settings, "keys", Opt().keys);
    SaveNames(settings, "pad_buttons", Opt().pad_buttons);
    for (int i = 0; i < kPlayers; i++) settings.insert(PlayerKey(i), PortName(Opt().players[size_t(i)]));
}

void Init() {
    input::LoadBindings();
    input::LoadPadBindings();
    audio::SetOutputMode(Opt().audio_output);
    RemoveUnfinishedSaves();
}

}  // namespace

Options& Opt() {
    static Options options;
    return options;
}

bool ControllerPlugged(int slot) { return slot >= 0 && slot < kPlayers && RawPad(slot).connected; }

}  // namespace game

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"007: From Russia with Love", // window and splash
    .short_title = L"007 FRWL",             // message boxes
    .name = L"007FRWL",                     // window classes, crash dumps, handover event
    .log = "007frwl",                       // log file prefix
    .settings = "settings.toml",            // settings file beside the exe
    .splash = L"splash",                    // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "007FRWL"}}, // each recompiled xbe and its exe, for title launches
    .options = {                            // game settings kept in the settings file
        {"invert_mouse", &game::Opt().invert_mouse, nullptr},
        {"camera_centring", &game::Opt().camera_centring, nullptr},
        {"free_aim", &game::Opt().free_aim, nullptr},
        {"skip_intros", &game::Opt().skip_intros, nullptr},
        {"skippable_cutscenes", &game::Opt().skippable_cutscenes, nullptr},
        {"fps_counter", &game::Opt().fps_counter, nullptr},
        {"hdr_output", &game::Opt().hdr_output, nullptr},
        {"ambient_occlusion", nullptr, &game::Opt().ambient_occlusion},
        {"audio_output", nullptr, &game::Opt().audio_output},
        {"msaa", nullptr, &game::Opt().msaa},
        {"smaa", &game::Opt().smaa, nullptr},
        {"fov", nullptr, &game::Opt().fov},
        {"fmv_scale", nullptr, &game::Opt().fmv_scale},
        {"walk_speed", nullptr, &game::Opt().walk_speed},
        {"pause_on_focus_loss", &game::Opt().pause_on_focus_loss, nullptr},
        {"async_shaders", &game::Opt().async_shaders, nullptr},
    },
    .load = game::Load,                     // reads more from the settings file's toml::table
    .save = game::Save,                     // writes more into it
    .init = game::Init,                     // after the settings load, before the window and boot
    .loaded = nullptr,                      // image in memory, before its entry point runs
    .window = input::StartMouse,            // the game window is up
    .message = game::Message,               // window messages first; true takes one
    .ui = app::DrawUi,                      // ImGui drawing each frame
    .pad = game::Pad,                       // changes a polled pad
    .rumble = game::Rumble,                 // changes a rumble before it is set
};
