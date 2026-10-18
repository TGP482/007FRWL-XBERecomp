// The chase camera, the view of play, takes the field of view setting; cutscene, scope and other cameras keep theirs.
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

float HalfTangent(uint32_t degrees) { return std::tan(float(degrees) * kHalfRadiansPerDegree); }

}  // namespace

// chase camera vtable 0x69F220 slots 1 and 2: the angle's tangent scaled by the setting's over the game's
void ChaseFieldOfView(X86Context& ctx, uint8_t*) {
    uint32_t fov = std::clamp(game::Opt().fov, kFovMin, kFovMax);
    if (fov == kGameFov) return;
    float& angle = ctx.xmm[kAngleReg].f[0];
    float wider = HalfTangent(fov) / HalfTangent(kGameFov);
    angle = 2 * std::atan(std::tan(angle / 2) * wider);
}
