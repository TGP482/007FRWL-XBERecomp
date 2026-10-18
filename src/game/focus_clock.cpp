// Pause on focus loss: the game thread's tick count leaves out the pauses, so its timers do not jump on return
#include "common/options.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kKeTickCountImport = 0x852CCC;  // the image's kernel thunk slot of KeTickCount

}  // namespace

// GetTickCount 0x31BA9A, the only reader of KeTickCount; the music feeder thread 0x30E65A keeps the real count
X86_FUNC(sub_0031BA9A) {
    using namespace kernel;
    uint32_t ticks = R32(R32(kKeTickCountImport));
    if (game::OnGameThread()) ticks -= game::FocusPausedMs();
    Return(ctx, ticks, 0);
}
