#pragma once
#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 6;

// channel order of the frames the mixer renders
enum Speaker : uint32_t {
    FrontLeft,
    FrontRight,
    Centre,
    Sub,
    BackLeft,
    BackRight,
};

// starts the platform output, which pulls interleaved float frames of kChannels from render.
// Returns once the device is open and IsSurround is known.
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

// the default device has 5.1 speakers; any other device hears the channels folded to stereo
bool IsSurround();

}  // namespace audio
