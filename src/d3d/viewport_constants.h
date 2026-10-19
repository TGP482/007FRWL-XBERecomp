#pragma once
#include <array>
#include <cstddef>

#include "gpu/state.h"

namespace gpu {
// c-38 and c-37 in the vertex constants, which start at c-96
constexpr size_t kViewportScaleSlot = 58;
constexpr size_t kViewportOffsetSlot = 59;

// c-38 and c-37 as D3D_GetViewportOffsetAndScale computes them. The renderer hands fixed function draws the same pair,
// so their positions round as a vertex program's do
inline void ViewportConstants(const State& s, std::array<float, 4>& scale, std::array<float, 4>& offset) {
    const Viewport& v = s.viewport;
    float hw = v.w * 0.5f;
    float hh = v.h * 0.5f;
    scale = {hw, -hh, (v.max_z - v.min_z) * s.zscale, 0};
    offset = {v.x + s.ss_offset_x + hw, v.y + s.ss_offset_y + hh, v.min_z * s.zscale, 0};
}
}  // namespace gpu
