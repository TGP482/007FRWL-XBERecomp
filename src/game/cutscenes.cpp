#include <algorithm>
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

// sequence instance speech: the instance ends only once its speech is done (0x405530)
constexpr uint32_t kSpeechFlags = 0xE4;
constexpr uint32_t kSpeechHandle = 0x10C;
constexpr int kSpeechStateShift = 7;
constexpr uint32_t kSpeechStateMask = 0xF;
constexpr uint32_t kSpeechPlaying = 2;
constexpr uint32_t kStopSpeech = 0x38F690;   // cdecl(handle)
constexpr uint32_t kSpeechEnded = 0x405210;  // cdecl(cookie, event, instance), the stream's end callback
constexpr uint32_t kSpeechFinished = 1;

// music: a cutscene's piece runs on the sound clock, which keeps real time, so a skip leaves it playing
constexpr uint32_t kMusicPlayer = 0x837EE4;
constexpr uint32_t kStopMusic = 0x41FBE0;  // thiscall(player, channel)
constexpr uint32_t kPlayMusic = 0x420110;  // cdecl(event name)
constexpr uint32_t kMusicChannel = 0;
constexpr auto kMusicLead = std::chrono::seconds(2);  // a cutscene's music event comes just before it begins

// screen fader [0x77F720]: layer 0 at +0x10 is the letterbox, layer 1 at +0x30 the colour fade
constexpr uint32_t kFader = 0x77F720;
constexpr uint32_t kLetterboxShown = 0x14;  // byte
constexpr uint32_t kLetterboxLeaving = 0x18;
constexpr uint32_t kLetterboxSeconds = 0x1C;
constexpr uint32_t kLetterboxElapsed = 0x2C;
constexpr float kLetterboxBar = 0.125f;  // each bar's share of the screen height once in, 0x6CE0B4
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
using Time = std::chrono::steady_clock::time_point;
Time g_cutscene_began;
Time g_music_sent;
uint32_t g_level_music = 0;  // event name the level sent once the skipped cutscene ended
bool g_was_playing = false;
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

uint32_t SpeechState(uint32_t instance) {
    return (R32(instance + kSpeechFlags) >> kSpeechStateShift) & kSpeechStateMask;
}

// ends the speech as its stream running out would; the game's own stop 0x404BD0 queues it again,
// and a queued speech holds the instance open so its follow on sequence never starts
void FinishSpeech(X86Context& ctx, uint32_t slot) {
    uint32_t link = R32(slot + kSlotInstance);
    if (link <= kInstanceLink) return;
    uint32_t instance = link - kInstanceLink;
    uint32_t handle = R32(instance + kSpeechHandle);
    if (!handle) return;
    CallGuest(ctx, kStopSpeech, {handle});
    W32(instance + kSpeechHandle, 0);
    // a speech handed over from a chained instance is not tracked by this one's state
    bool waiting = SpeechState(instance) == kSpeechPlaying;
    if (waiting) CallGuest(ctx, kSpeechEnded, {0, kSpeechFinished, instance});
}

void FinishCutsceneSpeech(X86Context& ctx) {
    for (uint32_t i = 0; i < kSlotCount; i++) {
        uint32_t slot = kSlots + i * kSlotSize;
        if (CutsceneSlot(slot)) FinishSpeech(ctx, slot);
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

// the piece would have ended with the cutscene; the level's next music only follows once a piece is over,
// so it is sent again after the stop
void StopCutsceneMusic(X86Context& ctx) {
    uint32_t player = R32(kMusicPlayer);
    uint32_t level_music = g_level_music;
    g_level_music = 0;
    bool cutscene_music = g_music_sent >= g_cutscene_began - kMusicLead;
    if (!cutscene_music || !player) return;
    g_music_sent = {};
    ctx.ecx = player;
    CallGuest(ctx, kStopMusic, {kMusicChannel});
    if (level_music) CallGuest(ctx, kPlayMusic, {level_music});
    g_level_music = 0;
}

void Finish(X86Context& ctx) {
    StopCutsceneMusic(ctx);
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
                FinishCutsceneSpeech(ctx);
            }
            break;
    }
}

// how far the top bar is from fully in, as a share of the screen height: it slides in over its seconds and out
// again (0x35D460). No letterbox leaves the prompt where it is
float LetterboxRise() {
    uint32_t fader = R32(kFader);
    if (!fader || !G<uint8_t>(fader + kLetterboxShown)) return 0;
    float seconds = G<float>(fader + kLetterboxSeconds);
    float done = seconds > 0 ? std::min(G<float>(fader + kLetterboxElapsed) / seconds, 1.0f) : 1.0f;
    float shown = R32(fader + kLetterboxLeaving) ? 1 - done : done;
    return (1 - shown) * kLetterboxBar;
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
    bool playing = Playing();
    if (playing && !g_was_playing) g_cutscene_began = std::chrono::steady_clock::now();
    g_was_playing = playing;
    bool skipping = g_skip != Skip::Idle;
    if (skipping && ++g_frames_without_tick > kTicklessFrames) Finish(ctx);
    if (g_skip == Skip::Idle && Offered()) DrawCornerText(ctx, PromptText(), Corner::TopRight, -LetterboxRise());
}

// play a music event by name, cdecl(name): MusicEvent messages and the level's music state 0x1B64D0.
// One sent after the skipped cutscene ended is the level's music again.
X86_EXTERN_FUNC(__imp__sub_00420110);
X86_FUNC(sub_00420110) {
    uint32_t name = R32(ctx.esp + 4);
    bool after_cutscene = g_skip == Skip::FastForward && !Playing();
    if (name && after_cutscene) g_level_music = name;
    if (name && !after_cutscene) g_music_sent = std::chrono::steady_clock::now();
    __imp__sub_00420110(ctx, base);
}
