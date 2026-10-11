#pragma once
#include <algorithm>
#include <atomic>

#include "app/app.h"

namespace game {

// the output's shape, 4:3 at the narrowest: the frame is shown that wide
inline float Aspect() {
    // asked per HUD point: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<float> aspect{16.0f / 9.0f};
    const app::Settings& s = app::Get();
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    if (key.exchange(k) != k) {
        app::Mode m = app::Output();
        aspect = std::max(float(m.width) / std::max(m.height, 1u), 4.0f / 3.0f);
    }
    return aspect;
}
// the share of the frame's width a 4:3 picture takes
inline float Narrow() { return 4.0f / 3.0f / Aspect(); }
inline bool Wide() { return Aspect() > 1.34f; }

}  // namespace game
