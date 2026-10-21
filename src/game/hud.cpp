// 2D is laid out 640x480 and shown as wide as the output. Each widget the UI tree draws keeps its shape:
// x' = x * narrow + anchor * spare width, anchored left, centre or right by where it sits on the screen.
// The Flash menus stay 4:3 in the middle, black beside a backdrop that reaches the 4:3 sides; films fill the screen up to
// their letterbox or stay 4:3 as set; a menu's film stays 4:3.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/aspect.h"
#include "common/options.h"
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
constexpr uint32_t kBarDraw = 0x359080;  // a bar filled to a share, drawn from its position across its width

constexpr uint32_t kDrawArgBytes = 4;  // a widget's Draw is thiscall (screen position), ret 4

// Im2D vertex: x, y, z, rhw, colour, u, v
constexpr uint32_t kVertexStride = 0x1C;
constexpr uint32_t kAxisX = 0x00;
constexpr uint32_t kAxisY = 0x04;
constexpr uint32_t kVertexColour = 0x10;  // ARGB
constexpr uint32_t kAlphaMask = 0xFF000000;

// Im2DRenderPrimitive(type, vertices, count)
constexpr uint32_t kIm2DRenderPrimitive = 0x278AB0;
constexpr uint32_t kMaxBarVertices = 256;

constexpr uint32_t kScreenWidth = 640;
constexpr float kOff = -1;
constexpr float kLeft = 0;
constexpr float kCentre = 0.5f;
constexpr float kRight = 1;
constexpr float kMiddleBand = 0.125f;  // a widget this near the middle stays centred

constexpr uint32_t kFmvFill = 0;  // fmv_scale

// film quad: +0x20 the film's width in pixels. The front end's loops are 512x448, every other film 640x480
constexpr uint32_t kQuadFilmWidth = 0x20;
constexpr float kMenuFilmWidth = 512;

// the film player; +4 its open film, cleared when the film stops (0x362900)
constexpr uint32_t kFilmPlayer = 0x77F954;
constexpr uint32_t kPlayerFilm = 0x04;

// letterbox bar of each letterboxed film, in film rows top and bottom alike, measured from the disc's films
struct Letterbox {
    const char* name;
    float bar;
};
constexpr Letterbox kLetterboxes[] = {{"intro_", 62}, {"title_", 58}, {"attract_", 56}};
constexpr float kFilmHeight = 480;
constexpr float kFilmEdge = 24;  // rows the quad leaves off top and bottom, 0x438100

enum class Film { None, Shown, Menu };

float g_anchor = kOff;
Film g_film = Film::None;  // the film's quad draws on a wider screen
float g_letterbox = 0;     // share of the shown film height that is letterbox
bool g_menu = false;       // inside the Flash render
bool g_bar = false;        // a side bar is being drawn
std::chrono::steady_clock::time_point g_menu_film_shown;

struct Span {
    float left;
    float right;
};

// screen shares; images and bars have a width
Span WidgetSpan(uint32_t widget, float x) {
    uint32_t draw = R32(R32(widget + kWidgetVtable) + kVtableDraw);
    if (draw == kMeterDraw) return {x, x + G<float>(widget + kMeterWidth)};
    if (draw == kBarDraw) return {x, x + G<float>(widget + kImageWidth) * G<float>(widget + kImageScale)};
    if (draw != kImageDraw) return {x, x};
    float width = G<float>(widget + kImageWidth) * G<float>(widget + kImageScale);
    float left = x - G<float>(widget + kImagePivotX) * width;
    return {left, left + width};
}

bool CoversScreen(Span span) { return span.left <= 0 && span.right >= 1; }

float AnchorFor(Span span) {
    if (CoversScreen(span)) return kOff;
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

bool OnEdge(float x, float edge) {
    constexpr float kSlack = 0.5f;
    return std::fabs(x - edge) < kSlack;
}

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

void LatchLetterbox(const char* file) {
    g_letterbox = 0;
    for (const Letterbox& film : kLetterboxes) {
        if (std::strstr(file, film.name)) g_letterbox = 2 * (film.bar - kFilmEdge) / (kFilmHeight - 2 * kFilmEdge);
    }
}

// The film covers the frame, shown as wide as the output, and is narrowed to 4:3. Filled, it is zoomed about the middle
// until only its letterbox is cut or it fills the width. A menu's film stays 4:3
void FitFilm(uint32_t vertices, uint32_t count) {
    bool fill = g_film == Film::Shown && game::Opt().fmv_scale == kFmvFill;
    float zoom = fill ? std::min(1 / game::Narrow(), 1 / (1 - g_letterbox)) : 1;
    ScaleFilm(vertices, count, kAxisY, zoom);
    ScaleFilm(vertices, count, kAxisX, zoom * game::Narrow());
}

// A menu shape touching a 4:3 side, the backdrop, is drawn again black over the spare width that side: points on
// the edge go to the screen's side, the rest to the 4:3 side. Its alpha is kept, so a fade darkens the bar too
void DrawSideBar(X86Context& ctx, uint32_t type, uint32_t vertices, uint32_t count, float edge) {
    static const uint32_t bar = mem::AllocSystem(kMaxBarVertices * kVertexStride);
    bool touches = false;
    for (uint32_t i = 0; i < count; i++) touches |= OnEdge(G<float>(vertices + i * kVertexStride), edge);
    if (!touches || count > kMaxBarVertices) return;
    float spare = kScreenWidth * (1 - game::Narrow()) / 2;
    float inner = edge == 0 ? spare : kScreenWidth - spare;
    float outer = edge == 0 ? 0 : kScreenWidth;
    std::memcpy(HostPtr(bar), HostPtr(vertices), count * kVertexStride);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t vertex = bar + i * kVertexStride;
        float& x = G<float>(vertex + kAxisX);
        x = OnEdge(x, edge) ? outer : inner;
        W32(vertex + kVertexColour, R32(vertex + kVertexColour) & kAlphaMask);
    }
    g_bar = true;
    CallGuest(ctx, kIm2DRenderPrimitive, {type, bar, count});
    g_bar = false;
}

void DrawSideBars(X86Context& ctx, uint32_t type, uint32_t vertices, uint32_t count) {
    DrawSideBar(ctx, type, vertices, count, 0);
    DrawSideBar(ctx, type, vertices, count, float(kScreenWidth));
}

}  // namespace

void HudAnchorLeft(bool on) { g_anchor = on && game::Wide() ? kLeft : kOff; }
void HudAnchorRight(bool on) { g_anchor = on && game::Wide() ? kRight : kOff; }
void HudAnchorCentre(bool on) { g_anchor = on && game::Wide() ? kCentre : kOff; }

// the front end's film is still open and drew lately; a film loop restarting can skip a frame
bool InFrontEnd() {
    constexpr auto kRecent = std::chrono::milliseconds(500);
    uint32_t player = R32(kFilmPlayer);
    bool open = player && R32(player + kPlayerFilm);
    return open && std::chrono::steady_clock::now() - g_menu_film_shown < kRecent;
}

// tree walk 0x3583B0, before a widget's Draw: esi the widget, [esp] its screen position
void HudWidgetBegin(X86Context& ctx, uint8_t*) {
    if (!game::Wide()) return;
    float x = G<float>(R32(ctx.esp));
    g_anchor = AnchorFor(WidgetSpan(ctx.esi, x));
}

// after the widget's Draw
void HudWidgetEnd(X86Context&, uint8_t*) { g_anchor = kOff; }

// hidden HUD: only widgets covering the screen draw; false skips the Draw and pops its argument as its ret would
bool HudWidgetShown(X86Context& ctx, uint8_t*) {
    if (!game::g_hide_hud) return true;
    float x = G<float>(R32(ctx.esp));
    if (CoversScreen(WidgetSpan(ctx.esi, x))) return true;
    ctx.esp += kDrawArgBytes;
    return false;
}

// around the Flash player's render, 0x412F1F
void MenuDrawBegin(X86Context&, uint8_t*) {
    if (!game::Wide()) return;
    g_anchor = kCentre;
    g_menu = true;
}

void MenuDrawEnd(X86Context&, uint8_t*) {
    g_anchor = kOff;
    g_menu = false;
}

// film open, thiscall on the film player with the play request by value, its file name first
X86_EXTERN_FUNC(__imp__sub_003626D0);
X86_FUNC(sub_003626D0) {
    LatchLetterbox(static_cast<const char*>(HostPtr(ctx.esp + 4)));
    __imp__sub_003626D0(ctx, base);
}

// the film's quad over the frame, thiscall. Its own camera update, possibly inside the Flash render
X86_EXTERN_FUNC(__imp__sub_00438370);
X86_FUNC(sub_00438370) {
    bool menu = G<float>(ctx.ecx + kQuadFilmWidth) == kMenuFilmWidth;
    if (menu) g_menu_film_shown = std::chrono::steady_clock::now();
    if (game::Wide()) g_film = menu ? Film::Menu : Film::Shown;
    __imp__sub_00438370(ctx, base);
    g_film = Film::None;
}

// Im2DRenderPrimitive(type, vertices, count), cdecl. Moved for the call and put back after
X86_EXTERN_FUNC(__imp__sub_00278AB0);
X86_FUNC(sub_00278AB0) {
    uint32_t type = R32(ctx.esp + 4);
    uint32_t vertices = R32(ctx.esp + 8);
    uint32_t count = R32(ctx.esp + 12);
    bool moved = g_film != Film::None || g_anchor != kOff;
    if (g_bar || !moved || !DrawsToScreen()) return __imp__sub_00278AB0(ctx, base);
    if (g_menu && g_film == Film::None) DrawSideBars(ctx, type, vertices, count);
    static std::vector<Point> saved;
    saved.resize(count);
    for (uint32_t i = 0; i < count; i++) saved[i] = G<Point>(vertices + i * kVertexStride);
    if (g_film != Film::None) FitFilm(vertices, count);
    else AnchorPoints(vertices, count);
    __imp__sub_00278AB0(ctx, base);
    for (uint32_t i = 0; i < count; i++) G<Point>(vertices + i * kVertexStride) = saved[i];
}
