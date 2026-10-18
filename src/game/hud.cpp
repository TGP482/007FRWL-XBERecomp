// 2D is laid out 640x480 and shown as wide as the output. Each widget the UI tree draws keeps its shape:
// x' = x * narrow + anchor * spare width, anchored left, centre or right by where it sits on the screen.
// The Flash menus stay 4:3 in the middle, films fill the screen.
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "common/aspect.h"
#include "game/hud.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// UI widget, told apart by its Draw
constexpr uint32_t kWidgetVtable = 0x00;
constexpr uint32_t kVtableDraw = 0x04;
constexpr uint32_t kImageDraw = 0x3581E0;
constexpr uint32_t kImageWidth = 0x28;   // share of the screen
constexpr uint32_t kImageScale = 0x4C;
constexpr uint32_t kImagePivotX = 0x50;  // share of the width left of the position
constexpr uint32_t kMeterDraw = 0x399B20;
constexpr uint32_t kMeterWidth = 0x94;  // the whole bar, the fill is drawn inside it

// Im2D vertex: x, y, z, rhw, colour, u, v
constexpr uint32_t kVertexStride = 0x1C;
constexpr uint32_t kAxisX = 0x00;
constexpr uint32_t kAxisY = 0x04;

constexpr uint32_t kScreenWidth = 640;
constexpr float kOff = -1;
constexpr float kLeft = 0;
constexpr float kCentre = 0.5f;
constexpr float kRight = 1;
constexpr float kMiddleBand = 0.125f;  // a widget this near the middle stays centred

float g_anchor = kOff;
bool g_film = false;  // the film's quad draws on a wider screen

struct Span {
    float left;
    float right;
};

// screen shares; images and bars have a width
Span WidgetSpan(uint32_t widget, float x) {
    uint32_t draw = R32(R32(widget + kWidgetVtable) + kVtableDraw);
    if (draw == kMeterDraw) return {x, x + G<float>(widget + kMeterWidth)};
    if (draw != kImageDraw) return {x, x};
    float width = G<float>(widget + kImageWidth) * G<float>(widget + kImageScale);
    float left = x - G<float>(widget + kImagePivotX) * width;
    return {left, left + width};
}

float AnchorFor(Span span) {
    if (span.left <= 0 && span.right >= 1) return kOff;  // across the whole screen
    float middle = (span.left + span.right) / 2;
    if (std::fabs(middle - kCentre) < kMiddleBand) return kCentre;
    return middle < kCentre ? kLeft : kRight;
}

// the radar draws its map into a smaller target in the middle of its widget
bool DrawsToScreen() {
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    return d3d::State().viewport.w == kScreenWidth;
}

struct Point {
    float x;
    float y;
};

// x' = x * narrow + anchor * spare width
void AnchorPoints(uint32_t vertices, uint32_t count) {
    float narrow = game::Narrow();
    float offset = g_anchor * kScreenWidth * (1 - narrow);
    for (uint32_t i = 0; i < count; i++) {
        float& x = G<float>(vertices + i * kVertexStride);
        x = x * narrow + offset;
    }
}

// the film's corners scaled about their middle along one axis
void ScaleFilm(uint32_t vertices, uint32_t count, uint32_t axis, float scale) {
    float low = G<float>(vertices + axis);
    float high = low;
    for (uint32_t i = 1; i < count; i++) {
        float value = G<float>(vertices + i * kVertexStride + axis);
        low = std::min(low, value);
        high = std::max(high, value);
    }
    float middle = (low + high) / 2;
    for (uint32_t i = 0; i < count; i++) {
        float& value = G<float>(vertices + i * kVertexStride + axis);
        value = middle + (value - middle) * scale;
    }
}

// The film covers the frame: made taller about the middle so a 4:3 film fills the screen, top and bottom cut
void FitFilm(uint32_t vertices, uint32_t count) { ScaleFilm(vertices, count, kAxisY, 1 / game::Narrow()); }

}  // namespace

void HudAnchorLeft(bool on) { g_anchor = on && game::Wide() ? kLeft : kOff; }

// tree walk 0x3583B0, before a widget's Draw: esi the widget, [esp] its screen position
void HudWidgetBegin(X86Context& ctx, uint8_t*) {
    if (!game::Wide()) return;
    float x = G<float>(R32(ctx.esp));
    g_anchor = AnchorFor(WidgetSpan(ctx.esi, x));
}

// after the widget's Draw
void HudWidgetEnd(X86Context&, uint8_t*) { g_anchor = kOff; }

// around the Flash player's render, 0x412F1F
void MenuDrawBegin(X86Context&, uint8_t*) {
    if (game::Wide()) g_anchor = kCentre;
}

void MenuDrawEnd(X86Context&, uint8_t*) { g_anchor = kOff; }

// the film's quad over the frame, thiscall. Its own camera update, possibly inside the Flash render
X86_EXTERN_FUNC(__imp__sub_00438370);
X86_FUNC(sub_00438370) {
    g_film = game::Wide();
    __imp__sub_00438370(ctx, base);
    g_film = false;
}

// Im2DRenderPrimitive(type, vertices, count), cdecl. Moved for the call and put back after
X86_EXTERN_FUNC(__imp__sub_00278AB0);
X86_FUNC(sub_00278AB0) {
    uint32_t vertices = R32(ctx.esp + 8);
    uint32_t count = R32(ctx.esp + 12);
    bool moved = g_film || g_anchor != kOff;
    if (!moved || !DrawsToScreen()) return __imp__sub_00278AB0(ctx, base);
    static std::vector<Point> saved;
    saved.resize(count);
    for (uint32_t i = 0; i < count; i++) saved[i] = G<Point>(vertices + i * kVertexStride);
    if (g_film) FitFilm(vertices, count);
    else AnchorPoints(vertices, count);
    __imp__sub_00278AB0(ctx, base);
    for (uint32_t i = 0; i < count; i++) G<Point>(vertices + i * kVertexStride) = saved[i];
}
