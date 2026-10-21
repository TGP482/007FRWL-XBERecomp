#include <bit>
#include <map>

#include "common/options.h"
#include "xdk/hle.h"

// Music volume: the music is EA PATH, its tracks are sound streams whose level each track sets through its SetVolume
// (0x4C7A98, track level 0 to 127 to a stream level 0 to 1). That one call is scaled by the music volume setting.
// The front end's music is a sound the menus play by name (PlayBackgroundMusic, 0x1493A) on one of the sound
// manager's two stream slots, which speech also uses; its stream is scaled while its slot still plays it.

using namespace kernel;

namespace {

// stream level, cdecl (stream, channel, level): -1 every channel
constexpr uint32_t kSetStreamLevel = 0x4D65D2;
constexpr uint32_t kTrackCallReturn = 0x4C7ACE;  // the call inside the track's SetVolume
constexpr uint32_t kAllChannels = 0xFFFFFFFF;
constexpr uint32_t kStreamCallReturn = 0x427036;  // the call inside the stream slots' SetVolume

// sound manager: two stream slots, each the id of the sound it plays and its streamer
constexpr uint32_t kSoundManager = 0x837EE0;
constexpr uint32_t kSlotCount = 2;
constexpr uint32_t kSlotStride = 0x3A0;
constexpr uint32_t kSlotStreamer = 0x04;
constexpr uint32_t kSlotSound = 0x390;
constexpr uint32_t kStreamerStream = 0x178;  // its device stream, made at 0x424BF0

// the sound library's lock, cdecl, held around a stream change as the track does
constexpr uint32_t kSoundLock = 0x30DD17;
constexpr uint32_t kSoundUnlock = 0x30DD25;

constexpr float kFullPercent = 100.0f;

// each music stream's level as the game set it, to apply a changed setting at once
std::map<uint32_t, float> g_levels;
uint32_t g_applied = 100;
uint32_t g_menu_music = 0;  // the sound id of the front end's music, 0 none

// the stream belongs to the slot playing the front end's music
bool IsMenuMusic(uint32_t stream) {
    uint32_t manager = R32(kSoundManager);
    if (!g_menu_music || !manager) return false;
    for (uint32_t i = 0; i < kSlotCount; i++) {
        uint32_t slot = manager + i * kSlotStride;
        uint32_t streamer = slot + kSlotStreamer;
        bool playing = R32(slot + kSlotSound) == g_menu_music;
        if (playing && R32(streamer + kStreamerStream) == stream) return true;
    }
    return false;
}

float Share() { return float(game::Opt().music_volume) / kFullPercent; }

}  // namespace

// PlayBackgroundMusic(name, loop, volume), thiscall; returns the sound id
X86_EXTERN_FUNC(__imp__sub_0001493A);
X86_FUNC(sub_0001493A) {
    __imp__sub_0001493A(ctx, base);
    g_menu_music = ctx.eax;
}

X86_EXTERN_FUNC(__imp__sub_004D65D2);
X86_FUNC(sub_004D65D2) {
    uint32_t from = R32(ctx.esp);
    uint32_t stream = R32(ctx.esp + 4);
    bool slot_stream = from == kStreamCallReturn;
    bool menu_music = slot_stream && IsMenuMusic(stream);
    if (slot_stream && !menu_music) g_levels.erase(stream);  // the slot now plays speech or another sound
    if (from == kTrackCallReturn || menu_music) {
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
