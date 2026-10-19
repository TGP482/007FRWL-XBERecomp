#include <atomic>
#include <bit>
#include <chrono>
#include <string>

#include "audio/output.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "game/hud.h"
#include "game/options_menu.h"
#include "xdk/hle.h"

// Skippable cutscenes: Start, or its key, fades the picture to black as the game's own fades do, then the game plays
// the rest of the cutscene behind it at full speed and muted, world and scripts together, so every spawn, message and
// music change still happens. The letterbox then slides away while the picture fades back.

using namespace kernel;

namespace {

// byte, set by the sequencer's cutscene begin 0x1D6F30 and cleared by its end 0x1D7750
constexpr uint32_t kCutscenePlaying = 0x82E0BE;

// the sequencer's game interface (vtable 0x6B3708), thiscall
constexpr uint32_t kSequencerGame = 0x82F2C8;
constexpr uint32_t kLetterboxOff = 0x80;     // (seconds)
constexpr uint32_t kFadeToColour = 0xB4;     // (r, g, b, seconds)
constexpr uint32_t kFadeFromColour = 0xB8;   // (r, g, b, seconds)

// sequence slots, run by the sequencer tick 0x4047F0
constexpr uint32_t kSlots = 0x82E3C0;
constexpr uint32_t kSlotSize = 0xF0;
constexpr uint32_t kSlotCount = 16;
constexpr uint32_t kSlotRunning = 0x01;
constexpr uint32_t kSlotCamera = 0x08;  // set by the camera track opcode 0x300: a cutscene
constexpr uint32_t kSlotHeld = 0x10;    // left alone by the engine's own skip check 0x40BB90
constexpr uint32_t kSlotInstance = 0xE8;  // the sequence instance + 0x20
constexpr uint32_t kInstanceLink = 0x20;
constexpr uint32_t kStopDialogue = 0x404BD0;  // fastcall(instance), its audio stream stops

// screen fader [0x77F720]: layer 0 at +0x10 is the letterbox, layer 1 at +0x30 the colour fade
constexpr uint32_t kFader = 0x77F720;
constexpr uint32_t kLetterboxShown = 0x14;  // byte
constexpr uint32_t kLetterboxLeaving = 0x18;
constexpr uint32_t kLetterboxSeconds = 0x1C;
constexpr uint32_t kFadeShown = 0x34;  // byte
constexpr uint32_t kFadeLeaving = 0x38;
constexpr uint32_t kFadeDone = 0x48;  // byte

constexpr float kFadeSeconds = 0.5f;  // as the cutscene end's own fade 0x404D70
constexpr float kBlackAtOnceSeconds = 0.01f;
constexpr float kLetterboxDefaultSeconds = 1.0f;
constexpr auto kFastForwardTimeout = std::chrono::seconds(120);  // a cutscene that never ends is shown
constexpr int kTicklessFrames = 30;  // a cutscene that ends the level stops the sequencer before it ends itself

constexpr uint16_t kPadStart = 0x10;
constexpr int kStartButton = 12;  // input::ButtonKey numbering
constexpr int kPorts = 4;

enum class Skip { Idle, FadingOut, FastForward };

Skip g_skip = Skip::Idle;
std::chrono::steady_clock::time_point g_since;
int g_frames_without_tick = 0;
std::atomic<bool> g_pressed{false};
std::atomic<bool> g_held[kPorts] = {};

uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }

bool Playing() { return G<uint8_t>(kCutscenePlaying) != 0; }

bool Offered() { return game::Opt().skippable_cutscenes && Playing() && !input::InMenu(); }

void CallGame(X86Context& ctx, uint32_t method, std::initializer_list<uint32_t> args) {
    uint32_t game = R32(kSequencerGame);
    if (!game) return;
    ctx.ecx = game;
    CallGuest(ctx, R32(R32(game) + method), args);
}

bool Black() {
    uint32_t fader = R32(kFader);
    if (!fader || !G<uint8_t>(fader + kFadeShown)) return false;
    return R32(fader + kFadeLeaving) == 0 && G<uint8_t>(fader + kFadeDone);
}

// black or still fading to it
bool FadeShown() {
    uint32_t fader = R32(kFader);
    return fader && G<uint8_t>(fader + kFadeShown) && R32(fader + kFadeLeaving) == 0;
}

bool LetterboxShown() {
    uint32_t fader = R32(kFader);
    return fader && G<uint8_t>(fader + kLetterboxShown) && R32(fader + kLetterboxLeaving) == 0;
}

void FadeOut(X86Context& ctx) {
    if (!Black()) CallGame(ctx, kFadeToColour, {0, 0, 0, Bits(kFadeSeconds)});
}

// the cutscene's next shots fade themselves in, so the picture goes straight back to black
void EnsureBlack(X86Context& ctx) {
    if (g_skip == Skip::FastForward && !Black()) CallGame(ctx, kFadeToColour, {0, 0, 0, Bits(kBlackAtOnceSeconds)});
}

bool CutsceneSlot(uint32_t slot) {
    uint32_t flags = R32(slot);
    return (flags & kSlotRunning) && (flags & kSlotCamera) && !(flags & kSlotHeld);
}

void StopDialogue(X86Context& ctx, uint32_t slot) {
    uint32_t link = R32(slot + kSlotInstance);
    if (link <= kInstanceLink) return;
    ctx.ecx = link - kInstanceLink;
    CallGuest(ctx, kStopDialogue, {});
}

void StopCutsceneDialogue(X86Context& ctx) {
    for (uint32_t i = 0; i < kSlotCount; i++) {
        uint32_t slot = kSlots + i * kSlotSize;
        if (CutsceneSlot(slot)) StopDialogue(ctx, slot);
    }
}

// the letterbox leaves as slowly as it came in
void FadeBack(X86Context& ctx) {
    if (LetterboxShown()) {
        float seconds = G<float>(R32(kFader) + kLetterboxSeconds);
        if (!(seconds > 0)) seconds = kLetterboxDefaultSeconds;
        CallGame(ctx, kLetterboxOff, {Bits(seconds)});
    }
    if (FadeShown()) CallGame(ctx, kFadeFromColour, {0, 0, 0, Bits(kFadeSeconds)});
}

void ApplyFastForward(bool on) {
    game::g_fast_forward = on;
    audio::SetMuted(on);
}

void Finish(X86Context& ctx) {
    ApplyFastForward(false);
    FadeBack(ctx);
    g_skip = Skip::Idle;
}

void UpdateSkip(X86Context& ctx) {
    bool pressed = g_pressed.exchange(false);
    auto waited = std::chrono::steady_clock::now() - g_since;
    switch (g_skip) {
        case Skip::Idle:
            if (pressed && Offered()) {
                FadeOut(ctx);
                g_skip = Skip::FadingOut;
            }
            break;
        case Skip::FadingOut:
            if (!Playing()) {
                FadeBack(ctx);
                g_skip = Skip::Idle;
            } else if (Black()) {
                g_since = std::chrono::steady_clock::now();
                ApplyFastForward(true);
                g_skip = Skip::FastForward;
            }
            break;
        case Skip::FastForward:
            if (!Playing() || waited >= kFastForwardTimeout) {
                Finish(ctx);
            } else {
                StopCutsceneDialogue(ctx);
            }
            break;
    }
}

std::string PromptText() {
    std::string skip = MenuText("Skip", false);
    if (!input::KeyboardActive()) return skip + " [Start]";
    std::string key = input::ButtonKey(kStartButton);
    if (key == "Escape") key = "ESC";
    return skip + " (" + key + ")";
}

}  // namespace

// any pad, the keyboard's included: Start skips while a cutscene plays and does not reach the game
void TakeCutsceneSkip(int port, input::Pad& pad) {
    if (port < 0 || port >= kPorts) return;
    bool down = pad.buttons & kPadStart;
    bool was_down = g_held[port].exchange(down);
    if (!Offered()) return;
    pad.buttons &= ~kPadStart;
    if (down && !was_down) g_pressed = true;
}

// sequencer tick, cdecl(seconds), each running tick
X86_EXTERN_FUNC(__imp__sub_004047F0);
X86_FUNC(sub_004047F0) {
    uint32_t tick_ecx = ctx.ecx;
    g_frames_without_tick = 0;
    UpdateSkip(ctx);
    ctx.ecx = tick_ecx;
    __imp__sub_004047F0(ctx, base);
    EnsureBlack(ctx);
}

// end of the frame render, over the finished frame
void DrawSkipPrompt(X86Context& ctx, uint8_t*) {
    bool skipping = g_skip != Skip::Idle;
    if (skipping && ++g_frames_without_tick > kTicklessFrames) Finish(ctx);
    if (g_skip == Skip::Idle && Offered()) DrawCornerText(ctx, PromptText(), Corner::TopRight);
}
