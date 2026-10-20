#include <bit>

#include "common/options.h"
#include "xdk/hle.h"

// The inventory (the item select HUD) pauses the game while it is open, as on the console; with INVENTORY PAUSE
// off the game runs on behind it. The game switches its tick lists to a new mode only at a frame's start, so a
// pause undone in the same frame never takes effect.

using namespace kernel;

namespace {

constexpr uint32_t kGameMode = 0x826E20;
constexpr uint32_t kModePaused = 1;
constexpr uint32_t kModeRunning = 2;

}  // namespace

// the inventory opens, thiscall on the HUD: item_select_hud goes on the port's controls and 0x3463D0 pauses
X86_EXTERN_FUNC(__imp__sub_00017BE4);
X86_FUNC(sub_00017BE4) {
    uint32_t mode = R32(kGameMode);
    __imp__sub_00017BE4(ctx, base);
    bool paused_here = mode == kModeRunning && R32(kGameMode) == kModePaused;
    if (paused_here && !game::Opt().inventory_pause) W32(kGameMode, kModeRunning);
}

// The preview's fade in after an outfit change stops a tick short of full alpha, which leaves Bond on the constant
// alpha blend and shows cut out textures (the dinner jacket's flower) with black edges. It ends at full alpha here
namespace {

constexpr uint32_t kOutfitState = 0x786FF4;
constexpr uint32_t kOutfitIdle = 1;
constexpr uint32_t kOutfitFadeIn = 4;
constexpr uint32_t kPreviewLink = 0x787044;     // preview actor + 0x20, 0 without one
constexpr uint32_t kSetPreviewAlpha = 0x17667;  // cdecl (alpha), needs the preview

}  // namespace

// inventory tick, the outfit change steps included
X86_EXTERN_FUNC(__imp__sub_00019494);
X86_FUNC(sub_00019494) {
    uint32_t state = R32(kOutfitState);
    __imp__sub_00019494(ctx, base);
    if (state != kOutfitFadeIn || R32(kOutfitState) != kOutfitIdle || !R32(kPreviewLink)) return;
    X86Context saved = ctx;
    CallGuest(ctx, kSetPreviewAlpha, {std::bit_cast<uint32_t>(1.0f)});
    ctx = saved;
}
