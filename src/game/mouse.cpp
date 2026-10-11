#include <array>
#include <optional>

#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "xdk/hle.h"

// Mouse look: the counts turn each view by an angle this frame, past the smoothing the game gives the stick.
// Each view's signs follow its own stick, so the mouse turns it the way the stick does.

using namespace kernel;

namespace {

constexpr uint32_t kGameMode = 0x826E20;
constexpr uint32_t kModeRunning = 2;
constexpr uint32_t kFrameCount = 0x824368;
constexpr uint32_t kPlayerPort = 0xB24;  // in the player, the controller that drives it

constexpr float kDegreesPerCount = 0.04f;  // at sensitivity 1
constexpr float kRadiansPerDegree = 0.017453292f;
constexpr float kRadiansPerCount = kDegreesPerCount * kRadiansPerDegree;

// views that turn with the mouse, each takes its own share of the motion
enum class View { Chase, Count };

struct Share {
    input::MouseMove move{};
    uint32_t frame = 0;
};
std::array<Share, size_t(View::Count)> g_shares;

void AddToShares(input::MouseMove move) {
    for (Share& share : g_shares) {
        share.move.dx += move.dx;
        share.move.dy += move.dy;
    }
}

// the motion since the view last turned; a view that missed a frame starts again from none
input::MouseMove Take(View view) {
    if (R32(kGameMode) != kModeRunning) return {};
    if (std::optional<input::MouseMove> move = input::TakeMouse()) AddToShares(*move);
    Share& share = g_shares[size_t(view)];
    uint32_t frame = R32(kFrameCount);
    bool missed = frame - share.frame > 1;
    input::MouseMove taken = missed ? input::MouseMove{} : share.move;
    share.move = {};
    share.frame = frame;
    return taken;
}

// the keyboard and mouse are port 0, the other players keep their controllers
bool OnPortZero(uint32_t player) { return player != 0 && R32(player + kPlayerPort) == 0; }

input::MouseMove TakeForPlayer(View view, uint32_t player) {
    input::MouseMove move = Take(view);
    return OnPortZero(player) ? move : input::MouseMove{};
}

bool Moved(input::MouseMove move) { return move.dx != 0.0f || move.dy != 0.0f; }

// Chase camera, esi through its update.

constexpr uint32_t kChasePlayer = 0x8C;
constexpr uint32_t kChaseStickEnabled = 0x5C;  // zero blocks the stick
// the update's frame from esp: the turns it makes this frame, radians; the pitch is positive down
constexpr uint32_t kChaseYawStep = 0x10;
constexpr uint32_t kChasePitchStep = 0x14;

input::MouseMove g_chase{};  // this update's motion

}  // namespace

// each chase camera update, before it turns
void ChaseMouseTake(X86Context& ctx, uint8_t*) {
    g_chase = TakeForPlayer(View::Chase, R32(ctx.esi + kChasePlayer));
    if (R32(ctx.esi + kChaseStickEnabled) == 0) g_chase = {};
}

// no stick input this update, so the camera may swing back behind Bond; true keeps it where the mouse left it
bool ChaseHoldsCentring(X86Context& ctx, uint8_t*) {
    if (!input::KeyboardActive() || !OnPortZero(R32(ctx.esi + kChasePlayer))) return false;
    return !game::Opt().camera_centring || Moved(g_chase);
}

void ChasePitch(X86Context& ctx, uint8_t*) {
    G<float>(ctx.esp + kChasePitchStep) += g_chase.dy * kRadiansPerCount;
}

void ChaseYaw(X86Context& ctx, uint8_t*) {
    G<float>(ctx.esp + kChaseYawStep) -= g_chase.dx * kRadiansPerCount;
}
