#include <algorithm>
#include <bit>
#include <cstdio>
#include <string_view>

#include "common/options.h"
#include "core/log.h"
#include "game/hud.h"
#include "xdk/hle.h"

// Frame rate (top left), the front end watermark and other host text, drawn by the game's own text manager, the one
// the HUD and menus use, over the finished frame.

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
constexpr uint32_t kGrey = 11;
constexpr uint32_t kWhite = 12;
constexpr uint32_t kAlignLeft = 0;
constexpr uint32_t kAlignRight = 2;  // the position is the text's right end

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

// RenderWare's record of the texture bound on stage 0; the game's own SetTexture calls do not update it, so a record
// that still names the font would leave another texture bound under the text
constexpr uint32_t kAppliedTexture = 0x810D8C;
constexpr uint32_t kNoTexture = 0xFFFFFFFF;

struct RenderState {
    uint32_t state;
    uint32_t value;
};

// blend, vertex alpha on, no culling, depth test, depth write or fog, as the HUD sets them
constexpr RenderState kOverlayStates[] = {{0x0A, 5}, {0x0B, 6}, {0x0C, 1}, {0x1E, 0}, {0x06, 0}, {0x08, 0}, {0x0E, 0}};

constexpr float kShadowX = 1.0f / 640.0f;  // a one line shadow
constexpr float kShadowY = 1.0f / 480.0f;
constexpr uint32_t kMaxChars = 64;

// where a corner's text sits: x and its top as shares of the page
struct CornerPlace {
    float x;
    float top;
    float scale;
    uint32_t align;
    uint32_t colour;
    bool shadow;
};
// the frame rate as on the other recomps, 8 lines in on a page 480 high; the skip prompt inside the letterbox;
// the watermark grey without a shadow as on the other recomps, bottom right of the 4:3 page
constexpr CornerPlace kTopLeftPlace = {0.009f, 0.016f, 0.6f, kAlignLeft, kWhite, true};
constexpr CornerPlace kTopRightPlace = {0.94f, 0.07f, 0.8f, kAlignRight, kWhite, true};
constexpr CornerPlace kBottomRightPlace = {0.95f, 0.935f, 0.6f, kAlignRight, kGrey, false};

constexpr char kWatermark[] = "XBE Recomp V2";

// memory the text manager reads: the position, then the text
constexpr uint32_t kScratchText = 8;
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

Place PagePlace(uint32_t manager, const CornerPlace& corner, float drop) {
    using namespace kernel;
    float width = G<float>(manager + kManagerPageWidth);
    float height = G<float>(manager + kManagerPageHeight);
    uint32_t font = R32(manager + kManagerFont);
    float glyph_scale = G<float>(manager + kManagerGlyphScale);
    float text_height = float(int32_t(R32(font + kFontHeight))) * glyph_scale * corner.scale;
    return {corner.x * width, height * (1.0f - corner.top - drop) - text_height};
}

void Show(X86Context& ctx, uint32_t colour, Place place, uint32_t position, uint32_t text, const CornerPlace& corner) {
    kernel::G<float>(position) = place.x;
    kernel::G<float>(position + 4) = place.y;
    kernel::CallGuest(ctx, kSetColour, {colour});
    kernel::CallGuest(ctx, kDrawText, {position, Bits(corner.scale), text, corner.align});
}

// the counter's own font, whatever the game drew last, even none; false when it is not loaded
bool ApplyFont(X86Context& ctx, uint32_t manager) {
    static bool reported = true;
    uint32_t id = kernel::CallGuest(ctx, kFontId, {kDinSmall});
    ctx.ecx = manager;
    bool loaded = (kernel::CallGuest(ctx, kSetFont, {id}) & 0xFF) != 0;
    if (loaded != reported) LOGR("corner text: din_small %s", loaded ? "loaded" : "not loaded, nothing drawn");
    reported = loaded;
    return loaded;
}

// the state the HUD pass draws its text in, so the counter does not depend on what else was drawn this frame
void BeginOverlay(X86Context& ctx, uint32_t manager, uint32_t camera) {
    using namespace kernel;
    CallGuest(ctx, kBeginUpdate, {camera});
    CallGuest(ctx, kSetCamera2d, {camera});
    CallGuest(ctx, kSetViewWindow2d, {Bits(0.0f), Bits(0.0f), Bits(1.0f), Bits(1.0f)});
    uint32_t render_state_set = R32(kRenderStateSet);
    for (RenderState rs : kOverlayStates) CallGuest(ctx, render_state_set, {rs.state, rs.value});
    W32(kAppliedTexture, kNoTexture);
    CallGuest(ctx, kPushMatrix2d, {});
    CallGuest(ctx, kIdentity2d, {});
    ctx.ecx = manager;
    CallGuest(ctx, kSetTextCamera, {camera});
    G<float>(manager + kManagerDepth) = G<float>(kOverlayDepth);
}

const CornerPlace& PlaceOf(Corner corner) {
    switch (corner) {
        case Corner::TopLeft:
            return kTopLeftPlace;
        case Corner::TopRight:
            return kTopRightPlace;
        default:
            return kBottomRightPlace;
    }
}

void ApplyAnchor(Corner corner) {
    switch (corner) {
        case Corner::TopLeft:
            HudAnchorLeft(true);
            break;
        case Corner::TopRight:
            HudAnchorRight(true);
            break;
        default:
            HudAnchorCentre(true);
    }
}

// the line and its shadow, in the corner's place
void ShowLine(X86Context& ctx, uint32_t manager, uint32_t text, Corner corner, float drop) {
    using namespace kernel;
    const CornerPlace& place_at = PlaceOf(corner);
    uint32_t position = Scratch();
    Place place = PagePlace(manager, place_at, drop);
    Place shadow = {place.x + kShadowX * G<float>(manager + kManagerPageWidth),
                    place.y - kShadowY * G<float>(manager + kManagerPageHeight)};
    ApplyAnchor(corner);
    ctx.ecx = manager;  // the text manager's calls are thiscall
    if (place_at.shadow) Show(ctx, kBlack, shadow, position, text, place_at);
    Show(ctx, place_at.colour, place, position, text, place_at);
    HudAnchorLeft(false);
}

}  // namespace

// text over the finished frame in the HUD's own 2D state; Latin 1, as the game's text is
void DrawCornerText(X86Context& ctx, std::string_view line, Corner corner, float drop) {
    using namespace kernel;
    uint32_t manager = R32(kTextManager);
    uint32_t cam = R32(kCamera);
    if (!manager || !cam) return;
    uint32_t text = Scratch() + kScratchText;
    size_t length = std::min<size_t>(line.size(), kMaxChars - 1);
    for (size_t i = 0; i < length; i++) G<char>(text + uint32_t(i)) = line[i];
    G<char>(text + uint32_t(length)) = 0;
    uint32_t frame_ecx = ctx.ecx;
    ctx.ecx = R32(kOverlayCameras);
    uint32_t overlay = ctx.ecx ? CallGuest(ctx, kOverlayCamera, {cam}) : cam;
    BeginOverlay(ctx, manager, overlay);
    uint32_t previous_font = R32(manager + kManagerFont);
    if (ApplyFont(ctx, manager)) ShowLine(ctx, manager, text, corner, drop);
    W32(manager + kManagerFont, previous_font);
    CallGuest(ctx, kPopMatrix2d, {});
    CallGuest(ctx, kEndUpdate, {overlay});
    ctx.ecx = frame_ecx;
}

// the frame is finished and about to be shown
void DrawFps(X86Context& ctx, uint8_t*) {
    if (InFrontEnd()) DrawCornerText(ctx, kWatermark, Corner::BottomRight);
    if (!game::Opt().fps_counter) return;
    char line[kMaxChars];
    std::snprintf(line, sizeof(line), "FPS: %.1f", double(game::g_fps.load()));
    DrawCornerText(ctx, line, Corner::TopLeft);
}
