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
enum class View { Chase, Jetpack, Turret, Scope, LockOn, Throw, Copter, Count };

struct Share {
    input::MouseMove move{};
    input::MouseMove taken{};  // this frame's, for another camera of the view updated in the same frame
    uint32_t frame = 0;
};
std::array<Share, size_t(View::Count)> g_shares;

void AddToShares(input::MouseMove move) {
    for (Share& share : g_shares) {
        share.move.dx += move.dx;
        share.move.dy += move.dy;
    }
}

// the motion since the view last turned; a view that missed a frame starts again from none. Leaving a door the
// game updates two chase cameras a frame while it blends them, each turns by the same motion.
input::MouseMove Take(View view) {
    if (R32(kGameMode) != kModeRunning) return {};
    if (std::optional<input::MouseMove> move = input::TakeMouse()) AddToShares(*move);
    Share& share = g_shares[size_t(view)];
    uint32_t frame = R32(kFrameCount);
    if (frame == share.frame) return share.taken;
    bool missed = frame - share.frame > 1;
    share.taken = missed ? input::MouseMove{} : share.move;
    share.move = {};
    share.frame = frame;
    return share.taken;
}

// the player driven from the keyboard's port, the other players keep their controllers
bool OnKeyboardPort(uint32_t player) {
    return player != 0 && int(R32(player + kPlayerPort)) == input::KeyboardPort();
}

input::MouseMove TakeForPlayer(View view, uint32_t player) {
    input::MouseMove move = Take(view);
    return OnKeyboardPort(player) ? move : input::MouseMove{};
}

bool Moved(input::MouseMove move) { return move.dx != 0.0f || move.dy != 0.0f; }

// the stick deflection that turns a view by the angle this frame, at the rate a full stick turns it
float StickForTurn(float turn, float rate, float seconds) {
    float step = rate * std::max(kMinSeconds, seconds);
    return step > 0.0f ? turn / step : 0.0f;
}

// Chase camera, esi through its update.

constexpr uint32_t kChasePlayer = 0x8C;
constexpr uint32_t kChaseStickEnabled = 0x5C;  // zero blocks the stick
// the update's frame from esp: the turns it makes this frame, radians; the pitch is positive down
constexpr uint32_t kChaseYawStep = 0x10;
constexpr uint32_t kChasePitchStep = 0x14;
// the turn after Bond's move: the view's change from his move, the turn about him in xmm0, then the eye's
// pull toward its place in xmm1
constexpr uint32_t kChaseMoveTurn = 0x10;
constexpr int kChaseTurnReg = 0;
constexpr int kChasePullReg = 1;

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

// Sniper scope update, esi the player, after the stick curve and invert: x in xmm0, the rest from esp.

constexpr uint32_t kScopeStickY = 0x14;
constexpr uint32_t kScopeYawRate = 0x18;
constexpr uint32_t kScopePitchRate = 0x24;
constexpr uint32_t kScopeSeconds = 0x6C;
constexpr int kScopeStickXReg = 0;

// Lock on fine aim, edi the player: the stick moves the aim point about the target at a rate in xmm2 (x) and
// xmm5 (y), units a second, the rest from esp.

constexpr uint32_t kLockOnStickX = 0x10;
constexpr uint32_t kLockOnStickY = 0x14;
constexpr uint32_t kLockOnSeconds = 0x44;
constexpr int kLockOnRateXReg = 2;
constexpr int kLockOnRateYReg = 5;
constexpr float kLockOnUnitsPerCount = 10.0f * kRadiansPerCount;  // the mouse's turn seen at 10 units away

// Throw aim update, esi the player: the yaw step in xmm1 and the new pitch in xmm0, radians.

constexpr int kThrowYawReg = 1;
constexpr int kThrowPitchReg = 0;

input::MouseMove g_throw{};  // this update's motion

// Q copter, the right stick turns it about its up axis at a rate that eases toward the stick; the mouse adds its
// turn after the easing.

constexpr int kCopterTurnReg = 0;  // xmm, the turn this step in degrees

input::MouseMove g_copter{};  // this frame's motion

}  // namespace

// each chase camera update, before it turns
void ChaseMouseTake(X86Context& ctx, uint8_t*) {
    g_chase = TakeForPlayer(View::Chase, R32(ctx.esi + kChasePlayer));
    if (R32(ctx.esi + kChaseStickEnabled) == 0) g_chase = {};
}

// no stick input this update, so the camera may swing back behind Bond; true keeps it where the mouse left it
bool ChaseHoldsCentring(X86Context& ctx, uint8_t*) {
    if (!input::KeyboardActive() || !OnKeyboardPort(R32(ctx.esi + kChasePlayer))) return false;
    return !game::Opt().camera_centring || Moved(g_chase);
}

// on keyboard and mouse with centring off the camera keeps its direction while Bond moves
static bool HoldsDirection(uint32_t camera) {
    if (game::Opt().camera_centring) return false;
    return input::KeyboardActive() && OnKeyboardPort(R32(camera + kChasePlayer));
}

// Bond moved and the view turned after him; the turn back undoes it, as the game does when focusing
void ChaseTracking(X86Context& ctx, uint8_t*) {
    if (HoldsDirection(ctx.esi)) ctx.xmm[kChaseTurnReg].f[0] = -G<float>(ctx.esp + kChaseMoveTurn);
}

// the eye trails its place and so turns the view after Bond; it takes its place at once
void ChaseFollow(X86Context& ctx, uint8_t*) {
    if (HoldsDirection(ctx.esi)) ctx.xmm[kChasePullReg].f[0] = 1.0f;
}

void ChasePitch(X86Context& ctx, uint8_t*) {
    G<float>(ctx.esp + kChasePitchStep) += g_chase.dy * kRadiansPerCount;
}

void ChaseYaw(X86Context& ctx, uint8_t*) {
    G<float>(ctx.esp + kChaseYawStep) -= g_chase.dx * kRadiansPerCount;
}

// the jetpack picks its turn stick; on keyboard and mouse A and D strafe and the mouse turns
void JetpackTurnStick(X86Context& ctx, uint8_t*) {
    bool mouse = input::KeyboardActive() && OnKeyboardPort(R32(ctx.ebx + kJetpackPlayer));
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
    // a positive pitch input lowers the turret
    pitch = TurretInput(pitch, move.dy * kRadiansPerCount, G<float>(ctx.esi + kTurretPitchSpeed), seconds);
}

// the turret turned, its smoothing goes on from the stick alone
void TurretAimDone(X86Context& ctx, uint8_t*) {
    G<float>(ctx.esi + kTurretYawInput) = g_turret_stick.yaw;
    G<float>(ctx.esi + kTurretPitchInput) = g_turret_stick.pitch;
}

void ScopeAim(X86Context& ctx, uint8_t*) {
    input::MouseMove move = TakeForPlayer(View::Scope, ctx.esi);
    float seconds = G<float>(ctx.esp + kScopeSeconds);
    float yaw_rate = G<float>(ctx.esp + kScopeYawRate);
    float pitch_rate = G<float>(ctx.esp + kScopePitchRate);
    ctx.xmm[kScopeStickXReg].f[0] += StickForTurn(move.dx * kRadiansPerCount, yaw_rate, seconds);
    // a positive stick y lowers the scope, as it lowers the chase camera
    G<float>(ctx.esp + kScopeStickY) += StickForTurn(move.dy * kRadiansPerCount, pitch_rate, seconds);
}

void LockOnAim(X86Context& ctx, uint8_t*) {
    input::MouseMove move = TakeForPlayer(View::LockOn, ctx.edi);
    float seconds = G<float>(ctx.esp + kLockOnSeconds);
    float rate_x = ctx.xmm[kLockOnRateXReg].f[0];
    float rate_y = ctx.xmm[kLockOnRateYReg].f[0];
    G<float>(ctx.esp + kLockOnStickX) += StickForTurn(move.dx * kLockOnUnitsPerCount, rate_x, seconds);
    // a positive stick y lowers the aim here
    G<float>(ctx.esp + kLockOnStickY) += StickForTurn(move.dy * kLockOnUnitsPerCount, rate_y, seconds);
}

void ThrowYaw(X86Context& ctx, uint8_t*) {
    g_throw = TakeForPlayer(View::Throw, ctx.esi);
    ctx.xmm[kThrowYawReg].f[0] -= g_throw.dx * kRadiansPerCount;
}

// the pitch grows with the stick y, so a positive pitch lowers the throw
void ThrowPitch(X86Context& ctx, uint8_t*) {
    ctx.xmm[kThrowPitchReg].f[0] += g_throw.dy * kRadiansPerCount;
}

// the copter reads its sticks for the player in eax
void CopterMouseTake(X86Context& ctx, uint8_t*) {
    g_copter = TakeForPlayer(View::Copter, ctx.eax);
}

void CopterTurn(X86Context& ctx, uint8_t*) {
    ctx.xmm[kCopterTurnReg].f[0] -= g_copter.dx * kDegreesPerCount;
    g_copter = {};
}
