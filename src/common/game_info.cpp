#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "common/menu.h"
#include "core/input.h"

namespace game {

namespace {

constexpr intptr_t kKeyWasDown = 1 << 30;  // bit 30 of a key message's lparam: a repeat
constexpr int kArrowCursorId = 32512;      // the system arrow

bool Take(intptr_t* result) {
    *result = 0;
    return true;
}

// F10 opens and closes the panel, Escape closes it; F10 is a system key, so it must never reach the window menu
bool PanelKeyDown(uint32_t msg, uintptr_t key, intptr_t lp, intptr_t* result) {
    if (key == VK_F10) {
        if (!(lp & kKeyWasDown)) app::ToggleMenu();
        return Take(result);
    }
    if (msg == WM_KEYDOWN && key == VK_ESCAPE && app::MenuOpen()) {
        app::ToggleMenu();
        return Take(result);
    }
    return false;
}

// a bound Alt must not open the window menu
bool SwallowAltMenu(uintptr_t command, intptr_t* result) {
    if ((command & 0xFFF0) != SC_KEYMENU) return false;
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

// the game sees no input while the panel is open
void Pad(int, input::Pad& pad) {
    if (app::MenuOpen()) pad = input::Pad{.connected = pad.connected};
}

}  // namespace

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
    .options = {},                          // game settings kept in the settings file
    .load = nullptr,                        // reads more from the settings file's toml::table
    .save = nullptr,                        // writes more into it
    .init = nullptr,                        // after the settings load, before the window and boot
    .loaded = nullptr,                      // image in memory, before its entry point runs
    .window = nullptr,                      // the game window is up
    .message = game::Message,               // window messages first; true takes one
    .ui = app::DrawUi,                      // ImGui drawing each frame
    .pad = game::Pad,                       // changes a polled pad
    .rumble = nullptr,                      // changes a rumble before it is set
};
