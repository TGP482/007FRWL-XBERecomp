// 2D is laid out 640x480 and shown as wide as the output. Each widget the UI tree draws keeps its shape:
// x' = x * narrow + anchor * spare width, anchored left, centre or right by where it sits on the screen.
// The Flash menus and films stay 4:3 in the middle.
#include <cmath>
#include <mutex>
#include <vector>

#include "common/aspect.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// UI widget
constexpr uint32_t kWidgetVtable = 0x00;
constexpr uint32_t kImageVtable = 0x6CE048;
constexpr uint32_t kImageWidth = 0x28;   // share of the screen
constexpr uint32_t kImageScale = 0x4C;
constexpr uint32_t kImagePivotX = 0x50;  // share of the width left of the position

// Im2D vertex: x, y, z, rhw, colour, u, v
constexpr uint32_t kVertexStride = 0x1C;

constexpr uint32_t kScreenWidth = 640;
constexpr float kOff = -1;
constexpr float kLeft = 0;
constexpr float kCentre = 0.5f;
constexpr float kRight = 1;
constexpr float kMiddleBand = 0.125f;  // a widget this near the middle stays centred

float g_anchor = kOff;

struct Span {
    float left;
    float right;
};

// screen shares; only images have a width
Span WidgetSpan(uint32_t widget, float x) {
    if (R32(widget + kWidgetVtable) != kImageVtable) return {x, x};
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

}  // namespace

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
    float outer = g_anchor;
    if (game::Wide()) g_anchor = kCentre;
    __imp__sub_00438370(ctx, base);
    g_anchor = outer;
}

// Im2DRenderPrimitive(type, vertices, count), cdecl. Moved for the call and put back after
X86_EXTERN_FUNC(__imp__sub_00278AB0);
X86_FUNC(sub_00278AB0) {
    uint32_t vertices = R32(ctx.esp + 8);
    uint32_t count = R32(ctx.esp + 12);
    if (g_anchor == kOff || !DrawsToScreen()) return __imp__sub_00278AB0(ctx, base);
    static std::vector<float> saved;
    saved.resize(count);
    float narrow = game::Narrow();
    float offset = g_anchor * kScreenWidth * (1 - narrow);
    for (uint32_t i = 0; i < count; i++) {
        float& x = G<float>(vertices + i * kVertexStride);
        saved[i] = x;
        x = x * narrow + offset;
    }
    __imp__sub_00278AB0(ctx, base);
    for (uint32_t i = 0; i < count; i++) G<float>(vertices + i * kVertexStride) = saved[i];
}
