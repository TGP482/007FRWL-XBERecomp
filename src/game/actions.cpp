#include <array>
#include <cstdint>
#include <optional>

#include "common/mouse_keyboard.h"
#include "xdk/hle.h"

// The game reads its controls as named actions: each is a pattern of the pad's buttons (a press, a hold of 100 ms)
// in the control map on top of the player port's stack. Many share a button, A alone is jump, climb, use and,
// held, reload. For the keyboard player the actions below are answered from their own keys instead, so a key does
// only what it is bound to.

using namespace kernel;

namespace {

// an action by the hash of its name (0x3A7E60) and the keyboard actions that answer it
struct Answer {
    uint32_t hash;
    std::array<const char*, 2> keys;
};

// basic_player
constexpr Answer kAnswers[] = {
    {0x7DDCA04E, {"jump"}},              // jump
    {0x2A1213D2, {"jump"}},              // vault
    {0x97DEE18E, {"jump"}},              // step_up
    {0xF7E02080, {"jump"}},              // vault_and_climb
    {0x196A4515, {"jump", "interact"}},  // climb
    {0xC4CCF1F7, {"interact"}},          // use_object
    {0xA2D67105, {"interact"}},          // enter_vehicle
    {0x83EEAB39, {"reload"}},            // reload
};

// how the game asks: held now, or pressed since the last update
constexpr uint32_t kHeld = 0;
constexpr uint32_t kPressed = 1;
constexpr uint32_t kPortMask = 0xFF;

// the keys as the input manager's last update saw them, a bit per keyboard action
struct KeyFrame {
    uint64_t now = 0;
    uint64_t before = 0;
};
KeyFrame g_keys;

uint64_t KeyMask(const Answer& answer) {
    uint64_t mask = 0;
    for (const char* id : answer.keys) {
        int index = id ? input::ActionIndex(id) : -1;
        if (index >= 0) mask |= uint64_t(1) << index;
    }
    return mask;
}

const Answer* FindAnswer(uint32_t hash) {
    for (const Answer& answer : kAnswers) {
        if (answer.hash == hash) return &answer;
    }
    return nullptr;
}

// none leaves the action to the pad
std::optional<bool> KeyAnswer(uint32_t port, uint32_t hash, uint32_t mode) {
    bool keyboard = int(port) == input::KeyboardPort() && input::KeyboardActive();
    const Answer* answer = keyboard && mode <= kPressed ? FindAnswer(hash) : nullptr;
    if (!answer) return std::nullopt;
    uint64_t keys = mode == kHeld ? g_keys.now : g_keys.now & ~g_keys.before;
    return (keys & KeyMask(*answer)) != 0;
}

}  // namespace

// the input manager's update, once a frame whether paused or running, before it matches the ports' actions
X86_EXTERN_FUNC(__imp__sub_0034FEB0);
X86_FUNC(sub_0034FEB0) {
    g_keys.before = g_keys.now;
    g_keys.now = input::HeldActions();
    __imp__sub_0034FEB0(ctx, base);
}

// the action query, thiscall on the input manager (port, hash, mode)
X86_EXTERN_FUNC(__imp__sub_0034DDE0);
X86_FUNC(sub_0034DDE0) {
    uint32_t port = R32(ctx.esp + 4) & kPortMask;
    std::optional<bool> keys = KeyAnswer(port, R32(ctx.esp + 8), R32(ctx.esp + 12));
    if (!keys) return __imp__sub_0034DDE0(ctx, base);
    Return(ctx, *keys ? 1 : 0, 3);
}
