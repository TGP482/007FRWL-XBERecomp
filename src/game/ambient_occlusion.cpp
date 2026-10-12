// Ambient occlusion goes in before the main camera's sorted translucents: the sky, world, decals, shadows and
// characters are drawn and in the depth buffer, transparent objects, flares, particles and the HUD land on top.
#include "common/options.h"
#include "d3d/occlusion.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// the camera render 0x34CD40 draws its render manager's buckets 0 to 15 through the loop 0x3546B0
constexpr uint32_t kFirstTranslucentBucket = 8;
constexpr uint32_t kSceneDraw = 0x34CDA0;  // the camera render's return from the loop
// stack at the hook
constexpr uint32_t kLoopReturn = 0x18;  // the loop's return address
constexpr uint32_t kRenderPass = 0x5C;  // the camera render's pass: 0 the scene, 1 the late items

}  // namespace

// in the bucket loop, before bucket esi is drawn
void AmbientOcclusion(X86Context& ctx, uint8_t*) {
    uint32_t mode = game::Opt().ambient_occlusion;
    if (!mode || ctx.esi != kFirstTranslucentBucket) return;
    if (R32(ctx.esp + kLoopReturn) != kSceneDraw || R32(ctx.esp + kRenderPass) != 0) return;
    d3d::Occlusion(mode == 2);
}
