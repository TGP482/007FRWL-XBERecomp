#include <bit>
#include <map>

#include "common/options.h"
#include "xdk/hle.h"

// Music volume: the music is EA PATH, its tracks are sound streams whose level each track sets through its SetVolume
// (0x4C7A98, track level 0 to 127 to a stream level 0 to 1). That one call is scaled by the music volume setting.

using namespace kernel;

namespace {

// stream level, cdecl (stream, channel, level): -1 every channel
constexpr uint32_t kSetStreamLevel = 0x4D65D2;
constexpr uint32_t kTrackCallReturn = 0x4C7ACE;  // the call inside the track's SetVolume
constexpr uint32_t kAllChannels = 0xFFFFFFFF;

// the sound library's lock, cdecl, held around a stream change as the track does
constexpr uint32_t kSoundLock = 0x30DD17;
constexpr uint32_t kSoundUnlock = 0x30DD25;

constexpr float kFullPercent = 100.0f;

// each music stream's level as its track set it, to apply a changed setting at once
std::map<uint32_t, float> g_levels;
uint32_t g_applied = 100;

float Share() { return float(game::Opt().music_volume) / kFullPercent; }

}  // namespace

X86_EXTERN_FUNC(__imp__sub_004D65D2);
X86_FUNC(sub_004D65D2) {
    if (R32(ctx.esp) == kTrackCallReturn) {
        uint32_t stream = R32(ctx.esp + 4);
        float& level = G<float>(ctx.esp + 12);
        g_levels[stream] = level;
        level *= Share();
    }
    __imp__sub_004D65D2(ctx, base);
}

// end of the frame render: a changed setting reaches the playing music; a stream gone since is dropped
void ApplyMusicVolume(X86Context& ctx, uint8_t*) {
    uint32_t percent = game::Opt().music_volume;
    if (percent == g_applied) return;
    g_applied = percent;
    CallGuest(ctx, kSoundLock, {});
    for (auto it = g_levels.begin(); it != g_levels.end();) {
        uint32_t level = std::bit_cast<uint32_t>(it->second * Share());
        bool gone = int32_t(CallGuest(ctx, kSetStreamLevel, {it->first, kAllChannels, level})) < 0;
        it = gone ? g_levels.erase(it) : std::next(it);
    }
    CallGuest(ctx, kSoundUnlock, {});
}
