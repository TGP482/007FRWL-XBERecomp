// pause on focus loss: the game thread waits while the window is in the background or minimized
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>

#include "app/app.h"
#include "audio/output.h"
#include "common/options.h"
#include "core/log.h"

namespace game {

namespace {

constexpr DWORD kPollMs = 20;

std::atomic<uint32_t> g_paused_ms{0};
std::atomic<DWORD> g_game_thread{0};

// a hidden window, behind the splash or for a handover, never pauses: it opens from the game thread
bool Unfocused() {
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    return GetForegroundWindow() != hwnd || IsIconic(hwnd);
}

}  // namespace

void WaitWhileUnfocused() {
    g_game_thread = GetCurrentThreadId();
    if (!Opt().pause_on_focus_loss || !Unfocused()) return;
    LOGI("focus lost: paused");
    auto start = std::chrono::steady_clock::now();
    g_focus_paused = true;
    audio::SetPaused(true);
    while (Opt().pause_on_focus_loss && Unfocused()) Sleep(kPollMs);
    audio::SetPaused(false);
    auto waited = std::chrono::steady_clock::now() - start;
    g_paused_ms += uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count());
    g_focus_paused = false;
    LOGI("focus back: resumed");
}

uint32_t FocusPausedMs() { return g_paused_ms; }

bool OnGameThread() { return GetCurrentThreadId() == g_game_thread; }

}  // namespace game
