#pragma once
#include <cstdint>

// Ambient occlusion reaches the renderer as a clear with a flag no D3DCLEAR value uses, so the render thread keeps it
// in order with the draws. Its three rects carry the twelve floats Occlusion sends
namespace gpu {
constexpr uint32_t kClearOcclusion = 0x100;
constexpr uint32_t kOcclusionRects = 3;
}  // namespace gpu

namespace d3d {
// over the scene drawn so far, with the projection last set; gtao: GTAO, else SSAO
void Occlusion(bool gtao);
}  // namespace d3d
