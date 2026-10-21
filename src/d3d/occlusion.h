#pragma once
#include <cstdint>

// Ambient occlusion reaches the renderer as a clear with a flag no D3DCLEAR value uses, so the render thread keeps it
// in order with the draws. Its four rects carry the sixteen floats Occlusion sends
namespace gpu {
constexpr uint32_t kClearOcclusion = 0x100;
constexpr uint32_t kOcclusionRects = 4;
// no rects, stencil 1 or 0: the draws that follow are characters or not
constexpr uint32_t kClearCharacters = 0x200;
}  // namespace gpu

namespace d3d {
// over the scene drawn so far, with the projection last set; gtao: GTAO, else SSAO
void Occlusion(bool gtao);
// ambient occlusion leaves the draws between on and off unoccluded
void MarkCharacters(bool on);
}  // namespace d3d
