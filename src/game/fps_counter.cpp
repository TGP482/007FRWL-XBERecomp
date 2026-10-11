#include <bit>
#include <cstdio>

#include "common/options.h"
#include "xdk/hle.h"

// Frame rate (top left), drawn by the game's own text manager, the one the HUD and menus use, over the finished frame.

namespace {

// the text manager, thiscall
constexpr uint32_t kTextManager = 0x77F70C;
constexpr uint32_t kSetColour = 0x355E10;  // palette index
constexpr uint32_t kDrawText = 0x357920;   // page position {x, y}, scale, text, alignment
constexpr uint32_t kManagerFont = 0x1C;
constexpr uint32_t kManagerCamera = 0x28;
constexpr uint32_t kManagerGlyphScale = 0x4C;
constexpr uint32_t kManagerPageWidth = 0x60;
constexpr uint32_t kManagerPageHeight = 0x64;
constexpr uint32_t kFontHeight = 0x18;  // int
constexpr uint32_t kBlack = 0;  // palette indices
constexpr uint32_t kWhite = 12;
constexpr uint32_t kAlignLeft = 0;

// RenderWare camera of the frame, cdecl
constexpr uint32_t kCamera = 0x826EBC;
constexpr uint32_t kBeginUpdate = 0x264600;
constexpr uint32_t kEndUpdate = 0x2645F0;

// as on the other recomps: top left, 8 lines in on a page 480 high, a one line shadow
constexpr float kLeft = 0.009f;  // of the page width
constexpr float kTop = 0.016f;   // of the page height
constexpr float kShadowX = 1.0f / 640.0f;
constexpr float kShadowY = 1.0f / 480.0f;
constexpr float kScale = 0.6f;  // as the game's own text
constexpr uint32_t kMaxChars = 32;

// memory the text manager reads: the position, then the text
uint32_t Scratch() {
    static const uint32_t base = mem::AllocSystem(0x40);
    return base;
}

uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }

// the text's page position: the page counts y up from its bottom
struct Place {
    float x;
    float y;
};

Place TopLeft(uint32_t manager) {
    using namespace kernel;
    float width = G<float>(manager + kManagerPageWidth);
    float height = G<float>(manager + kManagerPageHeight);
    uint32_t font = R32(manager + kManagerFont);
    float text_height = float(int32_t(R32(font + kFontHeight))) * G<float>(manager + kManagerGlyphScale) * kScale;
    return {kLeft * width, height * (1.0f - kTop) - text_height};
}

void Show(X86Context& ctx, uint32_t colour, Place place, uint32_t position, uint32_t text) {
    kernel::G<float>(position) = place.x;
    kernel::G<float>(position + 4) = place.y;
    kernel::CallGuest(ctx, kSetColour, {colour});
    kernel::CallGuest(ctx, kDrawText, {position, Bits(kScale), text, kAlignLeft});
}

}  // namespace

// the frame is finished and about to be shown
void DrawFps(X86Context& ctx, uint8_t*) {
    using namespace kernel;
    uint32_t manager = R32(kTextManager);
    uint32_t cam = R32(kCamera);
    if (!game::Opt().fps_counter || !manager || !cam) return;
    if (!R32(manager + kManagerFont) || !R32(manager + kManagerCamera)) return;
    uint32_t position = Scratch();
    uint32_t text = position + 8;
    char line[kMaxChars];
    std::snprintf(line, sizeof(line), "FPS: %.1f", double(game::g_fps.load()));
    for (uint32_t i = 0; i < kMaxChars; i++) {
        G<char>(text + i) = line[i];
        if (!line[i]) break;
    }
    Place place = TopLeft(manager);
    Place shadow = {place.x + kShadowX * G<float>(manager + kManagerPageWidth),
                    place.y - kShadowY * G<float>(manager + kManagerPageHeight)};
    uint32_t frame_ecx = ctx.ecx;
    CallGuest(ctx, kBeginUpdate, {cam});
    ctx.ecx = manager;  // the text manager's calls are thiscall
    Show(ctx, kBlack, shadow, position, text);
    Show(ctx, kWhite, place, position, text);
    ctx.ecx = frame_ecx;
    CallGuest(ctx, kEndUpdate, {cam});
}
