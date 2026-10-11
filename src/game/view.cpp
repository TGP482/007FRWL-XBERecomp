// The 3D view follows the output aspect: a camera that draws to the screen sees wider, its height of view kept.
#include <cmath>

#include "common/aspect.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// RwCamera
constexpr uint32_t kCameraProjection = 0x14;  // 1 perspective, 2 parallel
constexpr uint32_t kCameraRaster = 0x60;
constexpr uint32_t kCameraViewWindow = 0x68;  // x, y half extents at distance 1
constexpr uint32_t kCameraRecipViewWindow = 0x70;
constexpr uint32_t kPerspective = 1;

// RwRaster
constexpr uint32_t kRasterWidth = 0x0C;
constexpr uint32_t kRasterHeight = 0x10;

constexpr int32_t kScreenWidth = 640;
constexpr float kFourThreeRatio = 0.75f;  // view window y over x the game gives a screen camera
constexpr float kRatioSlack = 0.01f;

bool DrawsToScreen(uint32_t camera) {
    if (R32(camera + kCameraProjection) != kPerspective) return false;
    uint32_t raster = R32(camera + kCameraRaster);
    if (!raster) return false;
    int32_t width = int32_t(R32(raster + kRasterWidth));
    int32_t height = int32_t(R32(raster + kRasterHeight));
    return width >= kScreenWidth && width * 3 == height * 4;
}

// a 4:3 window from the game, or one this already widened
bool ScreenShaped(float x, float y) {
    float ratio = y / x;
    bool four_three = std::fabs(ratio - kFourThreeRatio) < kRatioSlack;
    bool widened = std::fabs(ratio * game::Aspect() - 1) < kRatioSlack;
    return four_three || widened;
}

}  // namespace

// RwCameraSetViewWindow(camera, window), cdecl. The width comes from the height, so setting it twice changes nothing
X86_EXTERN_FUNC(__imp__sub_00264900);
X86_FUNC(sub_00264900) {
    uint32_t camera = R32(ctx.esp + 4);
    __imp__sub_00264900(ctx, base);
    float& x = G<float>(camera + kCameraViewWindow);
    float y = G<float>(camera + kCameraViewWindow + 4);
    if (x <= 0 || y <= 0 || !DrawsToScreen(camera) || !ScreenShaped(x, y)) return;
    x = y * game::Aspect();
    G<float>(camera + kCameraRecipViewWindow) = 1 / x;
}
