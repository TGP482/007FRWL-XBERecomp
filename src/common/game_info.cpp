#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/input.h"

namespace game {

namespace {

constexpr intptr_t kKeyWasDown = 1 << 30;  // bit 30 of a key message's lparam: a repeat
constexpr int kArrowCursorId = 32512;      // the system arrow
constexpr uintptr_t kSysCommandMask = 0xFFF0;  // the low bits of a WM_SYSCOMMAND code are the system's

bool Take(intptr_t* result) {
    *result = 0;
    return true;
}

// the panel's key opens and closes it, Escape closes it unless a key is being bound
bool PanelKeyDown(uint32_t msg, uintptr_t key, intptr_t lp, intptr_t* result) {
    bool first_press = !(lp & kKeyWasDown);
    if (first_press && input::PanelKey(int(key))) {
        app::ToggleMenu();
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

// the arrow over the panel, no cursor over the game
bool PanelCursor(intptr_t lp, intptr_t* result) {
    if (LOWORD(lp) != HTCLIENT) return false;
    HCURSOR arrow = LoadCursorW(nullptr, MAKEINTRESOURCEW(kArrowCursorId));
    SetCursor(app::MenuOpen() ? arrow : nullptr);
    *result = TRUE;
    return true;
}

// F3 and the display mode stay with the host
bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    switch (msg) {
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
            return PanelCursor(lp, result);
    }
    return false;
}

// port 0 has the keyboard; the game sees no input while the panel is open
void Pad(int port, input::Pad& pad) {
    if (port == 0) pad.connected = true;
    if (app::MenuOpen()) {
        pad = input::Pad{.connected = pad.connected};
    } else if (port == 0) {
        input::Keyboard(pad);
    }
}

// what the options list cannot hold
void Load(const void* table) {
    const toml::table& settings = *static_cast<const toml::table*>(table);
    Options& options = Opt();
    float sensitivity = float(settings["mouse_sensitivity"].value_or(double(options.mouse_sensitivity)));
    if (std::isfinite(sensitivity)) options.mouse_sensitivity = sensitivity;
    const toml::table* keys = settings["keys"].as_table();
    if (!keys) return;
    for (auto&& [id, value] : *keys) {
        if (auto names = value.value<std::string>()) Opt().keys[std::string(id.str())] = *names;
    }
}

void Save(void* table) {
    toml::table& settings = *static_cast<toml::table*>(table);
    // thousandths, so the file reads 0.3 and not 0.30000001192092896
    settings.insert("mouse_sensitivity", std::round(double(Opt().mouse_sensitivity) * 1000.0) / 1000.0);
    toml::table keys;
    for (const auto& [id, names] : Opt().keys) keys.insert(id, names);
    settings.insert("keys", keys);
}

}  // namespace

Options& Opt() {
    static Options options;
    return options;
}

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
        {"skip_intros", &game::Opt().skip_intros, nullptr},
        {"fps_counter", &game::Opt().fps_counter, nullptr},
    },
    .load = game::Load,                     // reads more from the settings file's toml::table
    .save = game::Save,                     // writes more into it
    .init = input::LoadBindings,            // after the settings load, before the window and boot
    .loaded = nullptr,                      // image in memory, before its entry point runs
    .window = input::StartMouse,            // the game window is up
    .message = game::Message,               // window messages first; true takes one
    .ui = app::DrawUi,                      // ImGui drawing each frame
    .pad = game::Pad,                       // changes a polled pad
    .rumble = nullptr,                      // changes a rumble before it is set
};
