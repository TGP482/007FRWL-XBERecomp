#define NOMINMAX
#include <windows.h>

#include "common/mouse_keyboard.h"
#include "core/input.h"
#include "xdk/hle.h"

// Every film ends on A, Start, Enter, Space, Escape or the left mouse button, through the same path as its natural end

namespace {

// film player, the object at [0x77F954]
constexpr uint32_t kFilmStopping = 0x22;   // byte, set by the film's stop 0x362B50; the update then ends the film
constexpr uint32_t kFilmStartTime = 0x98;  // clock at the film's first frame, written at 0x362EFF

constexpr uint16_t kPadStart = 0x10;
constexpr uint8_t kPadPressed = 0x20;  // analog button threshold

bool PadSkipDown() {
    for (int port = 0; port < 4; port++) {
        input::Pad pad = input::Poll(port);
        bool a_down = pad.analog[0] > kPadPressed;
        if (a_down || (pad.buttons & kPadStart)) return true;
    }
    return false;
}

bool KeySkipDown() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return false;
    for (int key : {VK_RETURN, VK_SPACE, VK_ESCAPE})
        if (GetAsyncKeyState(key) & 0x8000) return true;
    return false;
}

// a press counts only after the keys were let go during this film, so the press that started it does not end it
bool UpdateSkipPress(uint32_t film_start) {
    static uint32_t s_film = 0;
    static bool s_armed = false;
    if (film_start != s_film) {
        s_film = film_start;
        s_armed = false;
    }
    bool down = PadSkipDown() || KeySkipDown() || input::LeftButton();
    bool skip = down && s_armed;
    s_armed = !down;
    return skip;
}

}  // namespace

// film update, thiscall on the film player, once a frame while a film plays
X86_EXTERN_FUNC(__imp__sub_00362C80);
X86_FUNC(sub_00362C80) {
    uint32_t film_start = kernel::R32(ctx.ecx + kFilmStartTime);
    if (UpdateSkipPress(film_start)) kernel::G<uint8_t>(ctx.ecx + kFilmStopping) = 1;
    __imp__sub_00362C80(ctx, base);
}
