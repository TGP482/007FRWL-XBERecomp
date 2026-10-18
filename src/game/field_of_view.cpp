// The views of play take the field of view setting: the chase camera, the car's and the jetpack's, each scaled from its
// own angle. Cutscene, scope and other cameras keep theirs.
#include <algorithm>
#include <cmath>

#include "common/options.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kGameFov = 65;  // the chase camera's own, [camera + 0x68] from its constructor 0xB3214
constexpr uint32_t kFovMin = 45;
constexpr uint32_t kFovMax = 140;
constexpr float kHalfRadiansPerDegree = 3.14159265f / 360.0f;
constexpr int kAngleReg = 1;  // xmm, the horizontal angle in radians before the camera stores it at +0x24

// camera director blend 0x38E2F0: ecx the camera, its angle +0x24 just read into xmm2
constexpr int kBlendAngleReg = 2;
constexpr uint32_t kDrivingCamera = 0x69F388;  // vtable
constexpr uint32_t kJetpackCamera = 0x6A60E0;

float HalfTangent(uint32_t degrees) { return std::tan(float(degrees) * kHalfRadiansPerDegree); }

// the angle's tangent scaled by the setting's over the game's
void Widen(float& angle) {
    uint32_t fov = std::clamp(game::Opt().fov, kFovMin, kFovMax);
    if (fov == kGameFov) return;
    float wider = HalfTangent(fov) / HalfTangent(kGameFov);
    angle = 2 * std::atan(std::tan(angle / 2) * wider);
}

}  // namespace

// chase camera vtable 0x69F220 slots 1 and 2, before the angle is stored
void ChaseFieldOfView(X86Context& ctx, uint8_t*) { Widen(ctx.xmm[kAngleReg].f[0]); }

// The car and jetpack cameras step their stored angle toward a target, so it is widened where the director reads it
void PlayFieldOfView(X86Context& ctx, uint8_t*) {
    uint32_t vtable = kernel::R32(ctx.ecx);
    if (vtable == kDrivingCamera || vtable == kJetpackCamera) Widen(ctx.xmm[kBlendAngleReg].f[0]);
}
