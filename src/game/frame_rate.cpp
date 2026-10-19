#include <algorithm>
#include <chrono>
#include <cmath>

#include "common/options.h"
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
MsCounter g_flash_ms;

// speedboat sway, the boat at ebx: each frame the waves kick its roll and pitch by a fixed amount while the pull back
// scales with the frame time, so the sway grew with the frame rate
constexpr uint32_t kBoatRoll = 0xDC;
constexpr uint32_t kBoatPitch = 0xE4;
constexpr double kConsoleFrameSeconds = 1.0 / 59.94;

struct BoatSway {
    float roll = 0.0f;
    float pitch = 0.0f;
};
BoatSway g_boat;

// Havok solver info in the world: contacts push overlapping bodies apart at tau times the overlap over the step, a
// speed the bodies keep, so short steps threw a shield dropped inside its holder into the air. Tau scaled by the step
// against the console's keeps that speed as at 30 fps
constexpr uint32_t kWorldTau = 0x164;
constexpr uint32_t kWorldContactTau = 0x16C;
constexpr uint32_t kStepInfo = 4;                    // from esp: seconds, then 1 / seconds
constexpr float kConsoleStepSeconds = 1.0f / 60.0f;  // the world steps twice a frame (0x2101B0), at 30 fps 1/60 s

float FrameShare() { return float(kernel::G<float>(kGameSeconds) / kConsoleFrameSeconds); }

// focus pauses are no time
double NowMs() {
    using namespace std::chrono;
    double now = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    return now - double(game::FocusPausedMs());
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

// the Flash menus' tick at 0x411183, about to pass ftol(frame seconds * 1000): whole milliseconds with the fraction
// kept, as the dropped fraction slowed menu animations and stopped them above 1000 fps
void FlashTickMs(X86Context& ctx, uint8_t*) {
    double ms = double(kernel::G<float>(kUnscaledSeconds)) / kSecondsPerMs;
    ctx.eax = g_flash_ms.Advance(ms);
}

// before the waves' roll kick, 0x20C372
void BoatRollKick(X86Context& ctx, uint8_t*) { g_boat.roll = kernel::G<float>(ctx.ebx + kBoatRoll); }

// after the roll kick and before the pitch kick, 0x20C396: the roll kick as a share of a console frame
void BoatPitchKick(X86Context& ctx, uint8_t*) {
    float& roll = kernel::G<float>(ctx.ebx + kBoatRoll);
    roll = g_boat.roll + (roll - g_boat.roll) * FrameShare();
    g_boat.pitch = kernel::G<float>(ctx.ebx + kBoatPitch);
}

// after the pitch kick, 0x20C3B7
void BoatKicked(X86Context& ctx, uint8_t*) {
    float& pitch = kernel::G<float>(ctx.ebx + kBoatPitch);
    pitch = g_boat.pitch + (pitch - g_boat.pitch) * FrameShare();
}

// hkWorld simulate, thiscall(world, step)
X86_EXTERN_FUNC(__imp__sub_0028BBD0);
X86_FUNC(sub_0028BBD0) {
    using namespace kernel;
    uint32_t world = ctx.ecx;
    float seconds = G<float>(R32(ctx.esp + kStepInfo));
    float share = std::min(1.0f, seconds / kConsoleStepSeconds);
    float tau = G<float>(world + kWorldTau);
    float contact_tau = G<float>(world + kWorldContactTau);
    G<float>(world + kWorldTau) = tau * share;
    G<float>(world + kWorldContactTau) = contact_tau * share;
    __imp__sub_0028BBD0(ctx, base);
    G<float>(world + kWorldTau) = tau;
    G<float>(world + kWorldContactTau) = contact_tau;
}
