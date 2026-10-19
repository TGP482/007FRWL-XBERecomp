#include "common/options.h"
#include "xdk/hle.h"

// Draw distance: each object's distance windows (data "distance" values, up to four LOD or visibility ranges) are
// tested against the camera distance times a zoom scale; dividing that scale reaches every window from further away.

namespace {

constexpr int kAllObjectsScale = 3;  // xmm, the zoom scale of the per camera pass
constexpr int kOneObjectScale = 1;   // xmm, the zoom scale of a single object's test

void Shrink(float& scale) {
    uint32_t multiplier = game::Opt().draw_distance;
    if (multiplier > 1) scale /= float(multiplier);
}

}  // namespace

// every object against the camera (0x365A20), its scale just made
void AllObjectsDrawDistance(X86Context& ctx, uint8_t*) { Shrink(ctx.xmm[kAllObjectsScale].f[0]); }

// one object against a point (0x364460), its scale just made
void OneObjectDrawDistance(X86Context& ctx, uint8_t*) { Shrink(ctx.xmm[kOneObjectScale].f[0]); }
