#include <algorithm>
#include <array>
#include <cmath>
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
constexpr float kMinSeconds = 1.0f / 240.0f;

// views that turn with the mouse, each takes its own share of the motion
enum class View { Chase, Jetpack, Turret, Count };

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

// Jetpack update, ebx the jetpack state.

constexpr uint32_t kJetpackPlayer = 0x4C;
constexpr uint32_t kJetpackTurnStep = 0;  // from esp, pushed for the player's turn, radians
constexpr uint32_t kLowByte = 0xFF;
constexpr uint32_t kRightStickTurns = 1;  // cl: the right stick turns, the left only strafes

// Gun turret update, esi the player's use of the turret; an input drives the turret at its top speed times the input.

constexpr uint32_t kTurretPlayer = 0x4C;
constexpr uint32_t kTurretYawSpeed = 0x68;  // radians a second
constexpr uint32_t kTurretPitchSpeed = 0x6C;
constexpr uint32_t kTurretYawInput = 0x74;  // the smoothed stick
constexpr uint32_t kTurretPitchInput = 0x70;
constexpr uint32_t kTurretSeconds = 0x24;  // from esp
constexpr int kTurretYawReg = 0;  // xmm, the inputs before they are stored
constexpr int kTurretPitchReg = 2;

struct TurretStick {
    float yaw = 0.0f;
    float pitch = 0.0f;
};
TurretStick g_turret_stick;  // the smoothed stick alone, put back once the turret turned

// past a full input the game raises the speed by the input too, so a turn of stick times that plus the mouse
// goes back in as its root
float TurretInput(float stick, float turn, float speed, float seconds) {
    if (speed <= 0.0f) return stick;
    float stick_turn = stick * std::max(1.0f, std::fabs(stick));
    float target = stick_turn + turn / (speed * std::max(kMinSeconds, seconds));
    if (std::fabs(target) <= 1.0f) return target;
    return std::copysign(std::sqrt(std::fabs(target)), target);
}

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

// the jetpack picks its turn stick; on keyboard and mouse A and D strafe and the mouse turns
void JetpackTurnStick(X86Context& ctx, uint8_t*) {
    bool mouse = input::KeyboardActive() && OnPortZero(R32(ctx.ebx + kJetpackPlayer));
    if (mouse) ctx.ecx = (ctx.ecx & ~kLowByte) | kRightStickTurns;
}

void JetpackTurn(X86Context& ctx, uint8_t*) {
    input::MouseMove move = TakeForPlayer(View::Jetpack, R32(ctx.ebx + kJetpackPlayer));
    G<float>(ctx.esp + kJetpackTurnStep) -= move.dx * kRadiansPerCount;
}

// the smoothed stick, before the turret takes it
void TurretAim(X86Context& ctx, uint8_t*) {
    float& yaw = ctx.xmm[kTurretYawReg].f[0];
    float& pitch = ctx.xmm[kTurretPitchReg].f[0];
    g_turret_stick = {yaw, pitch};
    input::MouseMove move = TakeForPlayer(View::Turret, R32(ctx.esi + kTurretPlayer));
    float seconds = G<float>(ctx.esp + kTurretSeconds);
    yaw = TurretInput(yaw, -move.dx * kRadiansPerCount, G<float>(ctx.esi + kTurretYawSpeed), seconds);
    pitch = TurretInput(pitch, -move.dy * kRadiansPerCount, G<float>(ctx.esi + kTurretPitchSpeed), seconds);
}

// the turret turned, its smoothing goes on from the stick alone
void TurretAimDone(X86Context& ctx, uint8_t*) {
    G<float>(ctx.esi + kTurretYawInput) = g_turret_stick.yaw;
    G<float>(ctx.esi + kTurretPitchInput) = g_turret_stick.pitch;
}
