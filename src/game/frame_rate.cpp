#include <algorithm>
#include <chrono>
#include <cmath>

#include "xdk/hle.h"

// Frame rate unlock: the game's own frame limiter goes and its clock follows real time.

namespace {

// event types the clock listens for, each address holds the type id
constexpr uint32_t kRunningPre = 0x826E4C;
constexpr uint32_t kPausedPre = 0x826E44;
constexpr uint32_t kFrozenPre = 0x826E5C;

// the clock the game reads: whole milliseconds and seconds, scaled for play and unscaled for menus
constexpr uint32_t kUnscaledMs = 0x82435C;
constexpr uint32_t kUnscaledSeconds = 0x824358;
constexpr uint32_t kGameMs = 0x824360;
constexpr uint32_t kGameSeconds = 0x824364;
constexpr uint32_t kFrameCount = 0x824368;
constexpr uint32_t kTimeScale = 0x7379D8;  // float, slow motion
constexpr uint32_t kMaxFrameMs = 0x7379E0;  // a longer frame slows the game down, as on the console

constexpr uint32_t kEventOffset = 4;  // the event, from the stack pointer
constexpr double kMinFrameMs = 0.1;  // the game floors at 1, which would run it fast above 1000 fps
constexpr double kSecondsPerMs = 0.001;

// a counter of whole milliseconds that keeps the fraction between frames
struct MsCounter {
    double carry = 0.0;

    uint32_t Advance(double ms) {
        carry += ms;
        double whole = std::floor(carry);
        carry -= whole;
        return uint32_t(whole);
    }
};

struct Clock {
    bool started = false;
    double last_ms = 0.0;
    MsCounter unscaled;
    MsCounter game;
};
Clock g_clock;

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// real milliseconds since the last event, none on the first
double LatchElapsedMs() {
    double now = NowMs();
    double elapsed = g_clock.started ? now - g_clock.last_ms : 0.0;
    g_clock.started = true;
    g_clock.last_ms = now;
    return elapsed;
}

double ClampFrameMs(double ms) { return std::clamp(ms, kMinFrameMs, double(kernel::R32(kMaxFrameMs))); }

void StopClock() {
    kernel::G<float>(kGameSeconds) = 0.0f;
    kernel::G<float>(kUnscaledSeconds) = 0.0f;
}

void AdvanceUnscaled(double ms) {
    kernel::W32(kUnscaledMs, kernel::R32(kUnscaledMs) + g_clock.unscaled.Advance(ms));
    kernel::G<float>(kUnscaledSeconds) = float(ms * kSecondsPerMs);
}

void AdvanceGame(double ms) {
    double scaled = ClampFrameMs(ms * kernel::G<float>(kTimeScale));
    kernel::W32(kGameMs, kernel::R32(kGameMs) + g_clock.game.Advance(scaled));
    kernel::G<float>(kGameSeconds) = float(scaled * kSecondsPerMs);
    kernel::W32(kFrameCount, kernel::R32(kFrameCount) + 1);
}

}  // namespace

// the tick's jump to the limiter at 0x224770, which waits out the rest of 1/59.94 s; cdecl with the start time
X86_FUNC(sub_0001E76E) {
    kernel::Return(ctx, 0, 0);
}

// the clock listener, thiscall with the event: milliseconds from the real time, not whole ticks of the kernel
X86_FUNC(sub_00335960) {
    using namespace kernel;
    uint32_t type = R32(R32(ctx.esp + kEventOffset));
    if (type == R32(kFrozenPre)) {
        LatchElapsedMs();
        StopClock();
    } else if (type == R32(kRunningPre) || type == R32(kPausedPre)) {
        double ms = ClampFrameMs(LatchElapsedMs());
        AdvanceUnscaled(ms);
        if (type == R32(kRunningPre)) AdvanceGame(ms);
    }
    Return(ctx, 0, 1);
}
