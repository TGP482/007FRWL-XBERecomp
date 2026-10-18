#include <bit>
#include <cstdio>

#include "common/options.h"
#include "game/hud.h"
#include "xdk/hle.h"

// Frame rate (top left), drawn by the game's own text manager, the one the HUD and menus use, over the finished frame.

namespace {

// the text manager, thiscall
constexpr uint32_t kTextManager = 0x77F70C;
constexpr uint32_t kSetColour = 0x355E10;  // palette index
constexpr uint32_t kDrawText = 0x357920;   // page position {x, y}, scale, text, alignment
constexpr uint32_t kSetFont = 0x355D70;    // font id, false and no font when it is not loaded
constexpr uint32_t kFontId = 0x3A7E60;     // cdecl (name), the hash fonts are registered under
constexpr uint32_t kDinSmall = 0x6D2FAC;   // "din_small", the Flash menus' default font
constexpr uint32_t kManagerFont = 0x1C;
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
constexpr uint32_t kRenderStateSet = 0x84C6E0;  // import slot, cdecl (state, value)

// the HUD's own 2D pass (0x359900): its camera, 2D device and render states
constexpr uint32_t kOverlayCameras = 0x778E64;
constexpr uint32_t kOverlayCamera = 0x337B50;  // thiscall (camera), the overlay camera drawn over it
constexpr uint32_t kSetCamera2d = 0x24ED50;
constexpr uint32_t kSetViewWindow2d = 0x24F660;  // x, y, width, height
constexpr uint32_t kPushMatrix2d = 0x24F0B0;
constexpr uint32_t kIdentity2d = 0x24EB00;
constexpr uint32_t kPopMatrix2d = 0x24F210;
constexpr uint32_t kSetTextCamera = 0x355E60;  // thiscall (camera)
constexpr uint32_t kManagerDepth = 0x2C;
constexpr uint32_t kOverlayDepth = 0x84C6D8;  // float the HUD copies into the text manager

struct RenderState {
    uint32_t state;
    uint32_t value;
};

// blend, vertex alpha on, no culling, depth test, depth write or fog, as the HUD sets them
constexpr RenderState kOverlayStates[] = {{0x0A, 5}, {0x0B, 6}, {0x0C, 1}, {0x1E, 0}, {0x06, 0}, {0x08, 0}, {0x0E, 0}};

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

// the counter's own font, whatever the game drew last; returns the font to put back
uint32_t ApplyFont(X86Context& ctx, uint32_t manager) {
    using namespace kernel;
    uint32_t previous = R32(manager + kManagerFont);
    uint32_t id = CallGuest(ctx, kFontId, {kDinSmall});
    ctx.ecx = manager;
    if (!(CallGuest(ctx, kSetFont, {id}) & 0xFF)) W32(manager + kManagerFont, previous);
    return previous;
}

// the state the HUD pass draws its text in, so the counter does not depend on what else was drawn this frame
void BeginOverlay(X86Context& ctx, uint32_t manager, uint32_t camera) {
    using namespace kernel;
    CallGuest(ctx, kBeginUpdate, {camera});
    CallGuest(ctx, kSetCamera2d, {camera});
    CallGuest(ctx, kSetViewWindow2d, {Bits(0.0f), Bits(0.0f), Bits(1.0f), Bits(1.0f)});
    uint32_t render_state_set = R32(kRenderStateSet);
    for (RenderState rs : kOverlayStates) CallGuest(ctx, render_state_set, {rs.state, rs.value});
    CallGuest(ctx, kPushMatrix2d, {});
    CallGuest(ctx, kIdentity2d, {});
    ctx.ecx = manager;
    CallGuest(ctx, kSetTextCamera, {camera});
    G<float>(manager + kManagerDepth) = G<float>(kOverlayDepth);
}

}  // namespace

// the frame is finished and about to be shown
void DrawFps(X86Context& ctx, uint8_t*) {
    using namespace kernel;
    uint32_t manager = R32(kTextManager);
    uint32_t cam = R32(kCamera);
    if (!game::Opt().fps_counter || !manager || !cam) return;
    if (!R32(manager + kManagerFont)) return;
    uint32_t position = Scratch();
    uint32_t text = position + 8;
    char line[kMaxChars];
    std::snprintf(line, sizeof(line), "FPS: %.1f", double(game::g_fps.load()));
    for (uint32_t i = 0; i < kMaxChars; i++) {
        G<char>(text + i) = line[i];
        if (!line[i]) break;
    }
    uint32_t frame_ecx = ctx.ecx;
    ctx.ecx = R32(kOverlayCameras);
    uint32_t overlay = ctx.ecx ? CallGuest(ctx, kOverlayCamera, {cam}) : cam;
    BeginOverlay(ctx, manager, overlay);
    uint32_t previous_font = ApplyFont(ctx, manager);
    Place place = TopLeft(manager);
    Place shadow = {place.x + kShadowX * G<float>(manager + kManagerPageWidth),
                    place.y - kShadowY * G<float>(manager + kManagerPageHeight)};
    HudAnchorLeft(true);
    ctx.ecx = manager;  // the text manager's calls are thiscall
    Show(ctx, kBlack, shadow, position, text);
    Show(ctx, kWhite, place, position, text);
    HudAnchorLeft(false);
    W32(manager + kManagerFont, previous_font);
    CallGuest(ctx, kPopMatrix2d, {});
    CallGuest(ctx, kEndUpdate, {overlay});
    ctx.ecx = frame_ecx;
}
