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
