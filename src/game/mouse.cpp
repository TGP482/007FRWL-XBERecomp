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

bool Aims(View view) { return view == View::Scope || view == View::LockOn || view == View::Throw; }

input::MouseMove TakeForPlayer(View view, uint32_t player) {
    input::MouseMove move = Take(view);
    if (!OnKeyboardPort(player)) return {};
    if (Aims(view)) {
        move.dx *= game::Opt().aim_horizontal_sensitivity;
        move.dy *= game::Opt().aim_vertical_sensitivity;
    }
    return move;
}

// the right stick's share of a turn, scaled by the controller sensitivities; aim views take the aim ones too
struct StickScale {
    float x;
    float y;
};

StickScale PadScale(View view) {
    const game::Options& o = game::Opt();
    bool aim = Aims(view);
    float x = o.pad_horizontal_sensitivity * (aim ? o.pad_aim_horizontal_sensitivity : 1.0f);
    float y = o.pad_vertical_sensitivity * (aim ? o.pad_aim_vertical_sensitivity : 1.0f);
    return {x, y};
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

// past a full input the game raises the speed by the input too, so a turn of stick times that, scaled, plus the
// mouse goes back in as its root
float TurretInput(float stick, float scale, float turn, float speed, float seconds) {
    if (speed <= 0.0f) return stick;
    float stick_turn = stick * std::max(1.0f, std::fabs(stick)) * scale;
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

// the view directors, one a player port; each holds the field of view its frame took, radians across
constexpr uint32_t kViews = 0x778E64;
constexpr uint32_t kViewsShared = 0x30;  // one view for every port, as in cutscenes
constexpr uint32_t kViewDirectors = 0x0C;
constexpr uint32_t kDirectorCount = 4;
constexpr uint32_t kDirectorBytes = 0x18C;
constexpr uint32_t kDirectorFov = 0x1C;
constexpr uint32_t kFovMin = 45;  // the range field_of_view.cpp takes
constexpr uint32_t kFovMax = 140;
constexpr float kPi = 3.14159265f;

// the scope's view over the view from the hip, as tangents: a count turns the same share of the view zoomed or not
float ScopeZoom(uint32_t player) {
    uint32_t views = R32(kViews);
    uint32_t port = R32(player + kPlayerPort);
    if (!views || R32(views + kViewsShared) || port >= kDirectorCount) return 1.0f;
    uint32_t director = R32(views + kViewDirectors) + port * kDirectorBytes;
    float fov = G<float>(director + kDirectorFov);
    if (!(fov > 0.0f && fov < kPi)) return 1.0f;
    float hip = float(std::clamp(game::Opt().fov, kFovMin, kFovMax)) * kRadiansPerDegree;
    return std::tan(fov / 2) / std::tan(hip / 2);
}

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
constexpr uint32_t kThrowStatePitch = 0x68;  // edi, the pitch before this update

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

// the keyboard's port with it used last takes the mouse setting, a controller its own
static bool OnMouse(uint32_t player) { return input::KeyboardActive() && OnKeyboardPort(player); }

static bool CentringOn(uint32_t player) {
    return OnMouse(player) ? game::Opt().camera_centring : game::Opt().pad_camera_centring;
}

// no stick input this update, so the camera may swing back behind Bond; true keeps it where the view was left
bool ChaseHoldsCentring(X86Context& ctx, uint8_t*) {
    uint32_t player = R32(ctx.esi + kChasePlayer);
    if (!CentringOn(player)) return true;
    return OnMouse(player) && Moved(g_chase);
}

// with centring off the camera keeps its direction while Bond moves
static bool HoldsDirection(uint32_t camera) { return !CentringOn(R32(camera + kChasePlayer)); }

// Bond moved and the view turned after him; the turn back undoes it, as the game does when focusing
void ChaseTracking(X86Context& ctx, uint8_t*) {
    if (HoldsDirection(ctx.esi)) ctx.xmm[kChaseTurnReg].f[0] = -G<float>(ctx.esp + kChaseMoveTurn);
}

// the eye trails its place and so turns the view after Bond; it takes its place at once
void ChaseFollow(X86Context& ctx, uint8_t*) {
    if (HoldsDirection(ctx.esi)) ctx.xmm[kChasePullReg].f[0] = 1.0f;
}

void ChasePitch(X86Context& ctx, uint8_t*) {
    G<float>(ctx.esp + kChasePitchStep) *= PadScale(View::Chase).y;
    G<float>(ctx.esp + kChasePitchStep) += g_chase.dy * kRadiansPerCount;
}

void ChaseYaw(X86Context& ctx, uint8_t*) {
    G<float>(ctx.esp + kChaseYawStep) *= PadScale(View::Chase).x;
    G<float>(ctx.esp + kChaseYawStep) -= g_chase.dx * kRadiansPerCount;
}

// the jetpack picks its turn stick; on keyboard and mouse A and D strafe and the mouse turns
void JetpackTurnStick(X86Context& ctx, uint8_t*) {
    bool mouse = input::KeyboardActive() && OnKeyboardPort(R32(ctx.ebx + kJetpackPlayer));
    if (mouse) ctx.ecx = (ctx.ecx & ~kLowByte) | kRightStickTurns;
}

void JetpackTurn(X86Context& ctx, uint8_t*) {
    input::MouseMove move = TakeForPlayer(View::Jetpack, R32(ctx.ebx + kJetpackPlayer));
    G<float>(ctx.esp + kJetpackTurnStep) *= PadScale(View::Jetpack).x;
    G<float>(ctx.esp + kJetpackTurnStep) -= move.dx * kRadiansPerCount;
}

// the smoothed stick, before the turret takes it
void TurretAim(X86Context& ctx, uint8_t*) {
    float& yaw = ctx.xmm[kTurretYawReg].f[0];
    float& pitch = ctx.xmm[kTurretPitchReg].f[0];
    g_turret_stick = {yaw, pitch};
    input::MouseMove move = TakeForPlayer(View::Turret, R32(ctx.esi + kTurretPlayer));
    float seconds = G<float>(ctx.esp + kTurretSeconds);
    StickScale scale = PadScale(View::Turret);
    float yaw_speed = G<float>(ctx.esi + kTurretYawSpeed);
    float pitch_speed = G<float>(ctx.esi + kTurretPitchSpeed);
    yaw = TurretInput(yaw, scale.x, -move.dx * kRadiansPerCount, yaw_speed, seconds);
    // a positive pitch input lowers the turret
    pitch = TurretInput(pitch, scale.y, move.dy * kRadiansPerCount, pitch_speed, seconds);
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
    float per_count = kRadiansPerCount * ScopeZoom(ctx.esi);
    StickScale scale = PadScale(View::Scope);
    ctx.xmm[kScopeStickXReg].f[0] *= scale.x;
    G<float>(ctx.esp + kScopeStickY) *= scale.y;
    ctx.xmm[kScopeStickXReg].f[0] += StickForTurn(move.dx * per_count, yaw_rate, seconds);
    // a positive stick y lowers the scope, as it lowers the chase camera
    G<float>(ctx.esp + kScopeStickY) += StickForTurn(move.dy * per_count, pitch_rate, seconds);
}

void LockOnAim(X86Context& ctx, uint8_t*) {
    input::MouseMove move = TakeForPlayer(View::LockOn, ctx.edi);
    float seconds = G<float>(ctx.esp + kLockOnSeconds);
    float rate_x = ctx.xmm[kLockOnRateXReg].f[0];
    float rate_y = ctx.xmm[kLockOnRateYReg].f[0];
    StickScale scale = PadScale(View::LockOn);
    G<float>(ctx.esp + kLockOnStickX) *= scale.x;
    G<float>(ctx.esp + kLockOnStickY) *= scale.y;
    G<float>(ctx.esp + kLockOnStickX) += StickForTurn(move.dx * kLockOnUnitsPerCount, rate_x, seconds);
    // a positive stick y lowers the aim here
    G<float>(ctx.esp + kLockOnStickY) += StickForTurn(move.dy * kLockOnUnitsPerCount, rate_y, seconds);
}

void ThrowYaw(X86Context& ctx, uint8_t*) {
    g_throw = TakeForPlayer(View::Throw, ctx.esi);
    ctx.xmm[kThrowYawReg].f[0] *= PadScale(View::Throw).x;
    ctx.xmm[kThrowYawReg].f[0] -= g_throw.dx * kRadiansPerCount;
}

// the pitch grows with the stick y, so a positive pitch lowers the throw
void ThrowPitch(X86Context& ctx, uint8_t*) {
    float before = G<float>(ctx.edi + kThrowStatePitch);
    float& pitch = ctx.xmm[kThrowPitchReg].f[0];
    pitch = before + (pitch - before) * PadScale(View::Throw).y;
    pitch += g_throw.dy * kRadiansPerCount;
}

// the copter reads its sticks for the player in eax
void CopterMouseTake(X86Context& ctx, uint8_t*) {
    g_copter = TakeForPlayer(View::Copter, ctx.eax);
}

void CopterTurn(X86Context& ctx, uint8_t*) {
    ctx.xmm[kCopterTurnReg].f[0] *= PadScale(View::Copter).x;
    ctx.xmm[kCopterTurnReg].f[0] -= g_copter.dx * kDegreesPerCount;
    g_copter = {};
}
