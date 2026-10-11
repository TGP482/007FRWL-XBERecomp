#include <algorithm>
#include <cmath>

#include "common/mouse_keyboard.h"
#include "xdk/hle.h"

// Mouse look for the chase camera in normal play: the counts become the right stick that turns it as far this frame.

namespace {

constexpr uint32_t kGameMode = 0x826E20;
constexpr uint32_t kModeRunning = 2;

// offsets in the chase camera object, esi
constexpr uint32_t kStickEnabled = 0x5C;  // zero blocks the stick

// the camera update's frame: the stick values, read from the stack, and the seconds, read from the frame pointer
constexpr uint32_t kStickX = 0x6C;
constexpr uint32_t kStickY = 0x3C;
constexpr uint32_t kSeconds = 8;

constexpr float kStickDegrees = 3.0f;  // a full stick turns this far each frame at 59.94 Hz
constexpr float kFrameRate = 59.94f;
constexpr float kDegreesPerCount = 0.04f;  // at sensitivity 1
constexpr float kMinSeconds = 1.0f / 240.0f;
constexpr float kPitchDeadzone = 0.2f;  // the camera drops this much of the pitch stick, then scales the rest
constexpr float kPitchScale = 1.25f;

float StickFor(float counts, float seconds) {
    return counts * kDegreesPerCount / (kStickDegrees * kFrameRate * seconds);
}

float PastPitchDeadzone(float stick) {
    if (stick == 0.0f) return 0.0f;
    return std::copysign(kPitchDeadzone + std::fabs(stick) / kPitchScale, stick);
}

}  // namespace

// each camera update, after the game squared the stick x and before it reads the stick
void MouseLook(X86Context& ctx, uint8_t*) {
    using namespace kernel;
    if (R32(kGameMode) != kModeRunning) return;
    std::optional<input::MouseMove> move = input::TakeMouse();
    if (!move || R32(ctx.esi + kStickEnabled) == 0) return;
    float seconds = std::max(kMinSeconds, G<float>(ctx.ebp + kSeconds));
    G<float>(ctx.esp + kStickX) += StickFor(move->dx, seconds);
    // pitch is positive for a mouse moving up, which is negative counts
    G<float>(ctx.esp + kStickY) += PastPitchDeadzone(StickFor(-move->dy, seconds));
}
